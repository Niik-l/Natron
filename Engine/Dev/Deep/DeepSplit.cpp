/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DeepSplit.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../ChoiceOption.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepSplitPrivate
{
    KnobDoubleWPtr threshold;
    KnobChoiceWPtr outputSelect;
    KnobBoolWPtr softEdge;
    KnobDoubleWPtr softWidth;
    KnobStringWPtr info;
};


DeepSplit::DeepSplit(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepSplitPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepSplit::~DeepSplit()
{
}

std::string
DeepSplit::getPluginDescription() const
{
    return tr("Split a deep image into two parts by depth threshold. "
              "Samples in front of the threshold go to the 'Front' output, "
              "samples behind go to 'Back'. This is the inverse of DeepMerge.\n\n"
              "When soft edge is enabled, samples near the threshold boundary "
              "are blended smoothly over the specified width.").toStdString();
}

void
DeepSplit::addAcceptedComponents(int /*inputNb*/,
                                 std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepSplit::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepSplit::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                          bool* /*defaultG*/,
                                          bool* /*defaultB*/,
                                          bool* /*defaultA*/) const
{
    return false;
}

void
DeepSplit::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr threshold = AppManager::createKnob<KnobDouble>(this, tr("Threshold"));
    threshold->setName("threshold");
    threshold->setHintToolTip(tr("Z depth threshold for splitting. Samples with Z less than this value "
                                 "go to the Front output, samples with Z greater or equal go to Back."));
    threshold->setAnimationEnabled(true);
    threshold->setDefaultValue(5.0);
    threshold->setMinimum(0.0);
    page->addKnob(threshold);
    _imp->threshold = threshold;

    KnobChoicePtr outputSelect = AppManager::createKnob<KnobChoice>(this, tr("Output"));
    outputSelect->setName("outputSelect");
    outputSelect->setHintToolTip(tr("Which side of the split to output. "
                                    "Front: samples with Z < threshold. "
                                    "Back: samples with Z >= threshold."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Front", "Front", ""));
        entries.push_back(ChoiceOption("Back", "Back", ""));
        outputSelect->populateChoices(entries);
    }
    outputSelect->setDefaultValue(0);
    page->addKnob(outputSelect);
    _imp->outputSelect = outputSelect;

    KnobBoolPtr softEdge = AppManager::createKnob<KnobBool>(this, tr("Soft Edge"));
    softEdge->setName("softEdge");
    softEdge->setHintToolTip(tr("Enable soft blending at the threshold boundary. "
                                "Samples within the soft width of the threshold will have "
                                "their alpha and premultiplied RGB blended smoothly."));
    softEdge->setDefaultValue(false);
    page->addKnob(softEdge);
    _imp->softEdge = softEdge;

    KnobDoublePtr softWidth = AppManager::createKnob<KnobDouble>(this, tr("Soft Width"));
    softWidth->setName("softWidth");
    softWidth->setHintToolTip(tr("Depth width of the soft transition zone around the threshold. "
                                 "Only active when Soft Edge is enabled."));
    softWidth->setAnimationEnabled(true);
    softWidth->setDefaultValue(0.5);
    softWidth->setMinimum(0.001);
    page->addKnob(softWidth);
    _imp->softWidth = softWidth;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set threshold to split the deep image by depth.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepSplit::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    if (_imp->threshold.lock().get() == k ||
        _imp->outputSelect.lock().get() == k ||
        _imp->softEdge.lock().get() == k ||
        _imp->softWidth.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepSplit::getRegionOfDefinition(U64 /*hash*/,
                                 double /*time*/,
                                 const RenderScale& /*scale*/,
                                 ViewIdx /*view*/,
                                 RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), 0, RenderScale(), ViewIdx(0), rod, &isProjectFormat);
}

DeepImagePtr
DeepSplit::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepSplit::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double thresholdVal = _imp->threshold.lock()->getValue();
    int outputIdx = _imp->outputSelect.lock()->getValue();
    bool useSoftEdge = _imp->softEdge.lock()->getValue();
    double softWidthVal = _imp->softWidth.lock()->getValue();

    bool wantFront = (outputIdx == 0);

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    if (zIdx < 0) {
        // No Z channel — can't split, pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // Find alpha channel index for soft blending
    int aIdx = srcDeep->findChannelIndex("A");

    // Find RGB channel indices for premultiplied soft blending
    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");

    // First pass: count how many samples match the criterion per pixel
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                result->setSampleCount(x, y, 0);
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);
            int kept = 0;
            for (int s = 0; s < nSamples; ++s) {
                float z = srcData[s * nChannels + zIdx];
                bool isFront = (z < thresholdVal);

                if (useSoftEdge) {
                    // In soft edge mode, samples within softWidth of threshold are kept on both sides
                    double dist = std::fabs(z - thresholdVal);
                    if (dist < softWidthVal) {
                        ++kept;
                    } else if (wantFront == isFront) {
                        ++kept;
                    }
                } else {
                    if (wantFront == isFront) {
                        ++kept;
                    }
                }
            }
            result->setSampleCount(x, y, kept);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy matching samples, apply soft blend if applicable
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);
            if (!dstData) {
                continue;
            }

            int outIdx = 0;
            for (int s = 0; s < nSamples; ++s) {
                float z = srcData[s * nChannels + zIdx];
                bool isFront = (z < thresholdVal);
                double blendFactor = 1.0;
                bool include = false;

                if (useSoftEdge) {
                    double dist = std::fabs(z - thresholdVal);
                    if (dist < softWidthVal) {
                        // Within soft zone — compute blend
                        double t = dist / softWidthVal; // 0 at threshold, 1 at edge
                        if (wantFront) {
                            // Front output: full strength for samples well in front,
                            // fading to 0 at threshold + softWidth
                            blendFactor = isFront ? (1.0 - t * 0.5) : (0.5 * (1.0 - t));
                        } else {
                            // Back output: full strength for samples well behind,
                            // fading to 0 at threshold - softWidth
                            blendFactor = isFront ? (0.5 * (1.0 - t)) : (1.0 - t * 0.5);
                        }
                        include = true;
                    } else {
                        include = (wantFront == isFront);
                        blendFactor = 1.0;
                    }
                } else {
                    include = (wantFront == isFront);
                }

                if (include) {
                    const float* src = srcData + s * nChannels;
                    float* dst = dstData + outIdx * nChannels;
                    for (int c = 0; c < nChannels; ++c) {
                        dst[c] = src[c];
                    }

                    // Apply soft blend factor to alpha and premultiplied RGB
                    if (useSoftEdge && blendFactor < 1.0) {
                        if (aIdx >= 0) {
                            dst[aIdx] *= static_cast<float>(blendFactor);
                        }
                        if (rIdx >= 0) {
                            dst[rIdx] *= static_cast<float>(blendFactor);
                        }
                        if (gIdx >= 0) {
                            dst[gIdx] *= static_cast<float>(blendFactor);
                        }
                        if (bIdx >= 0) {
                            dst[bIdx] *= static_cast<float>(blendFactor);
                        }
                    }
                    ++outIdx;
                }
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Output: " << (wantFront ? "Front" : "Back")
       << " | Threshold: " << thresholdVal;
    if (useSoftEdge) {
        ss << " | Soft Width: " << softWidthVal;
    }
    ss << " | Total samples: " << result->totalSamples();
    KnobStringPtr infoKnob = _imp->info.lock();
    if (infoKnob) {
        infoKnob->setValue(ss.str());
    }

    _lastDeepImage = result;

    // Produce flattened preview
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (outImg) {
        result->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepSplit.cpp"
