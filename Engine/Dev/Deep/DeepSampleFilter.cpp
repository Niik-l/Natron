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

#include "DeepSampleFilter.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "../../ChoiceOption.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepSampleFilterPrivate
{
    KnobChoiceWPtr mode;
    KnobDoubleWPtr alphaMin;
    KnobIntWPtr maxSamples;
    KnobDoubleWPtr depthMin;
    KnobDoubleWPtr depthMax;
    KnobStringWPtr info;
};


DeepSampleFilter::DeepSampleFilter(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepSampleFilterPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepSampleFilter::~DeepSampleFilter()
{
}

std::string
DeepSampleFilter::getPluginDescription() const
{
    return tr("Filter deep samples by various criteria: remove by alpha threshold, "
              "limit sample count per pixel, or remove by depth range.").toStdString();
}

void
DeepSampleFilter::addAcceptedComponents(int /*inputNb*/,
                                        std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepSampleFilter::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepSampleFilter::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                                 bool* /*defaultG*/,
                                                 bool* /*defaultB*/,
                                                 bool* /*defaultA*/) const
{
    return false;
}

void
DeepSampleFilter::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobChoicePtr mode = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
    mode->setName("mode");
    mode->setHintToolTip(tr("Filter mode:\n"
                            "- Alpha Threshold: remove samples with alpha below the minimum.\n"
                            "- Max Count: keep only the first N samples per pixel (front-to-back).\n"
                            "- Depth Range: remove samples outside [Depth Min, Depth Max]."));
    mode->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Alpha Threshold", "Alpha Threshold", ""));
        entries.push_back(ChoiceOption("Max Count", "Max Count", ""));
        entries.push_back(ChoiceOption("Depth Range", "Depth Range", ""));
        mode->populateChoices(entries);
    }
    mode->setDefaultValue(0);
    page->addKnob(mode);
    _imp->mode = mode;

    KnobDoublePtr alphaMin = AppManager::createKnob<KnobDouble>(this, tr("Alpha Min"));
    alphaMin->setName("alphaMin");
    alphaMin->setHintToolTip(tr("Minimum alpha value. Samples with alpha below this are removed. "
                                "Only used in Alpha Threshold mode."));
    alphaMin->setAnimationEnabled(true);
    alphaMin->setDefaultValue(0.001);
    alphaMin->setMinimum(0.0);
    alphaMin->setMaximum(1.0);
    alphaMin->setDisplayMinimum(0.0);
    alphaMin->setDisplayMaximum(1.0);
    page->addKnob(alphaMin);
    _imp->alphaMin = alphaMin;

    KnobIntPtr maxSamples = AppManager::createKnob<KnobInt>(this, tr("Max Samples"));
    maxSamples->setName("maxSamples");
    maxSamples->setHintToolTip(tr("Maximum number of samples to keep per pixel. "
                                  "Only used in Max Count mode."));
    maxSamples->setAnimationEnabled(false);
    maxSamples->setDefaultValue(32);
    maxSamples->setMinimum(1);
    maxSamples->setDisplayMinimum(1);
    maxSamples->setDisplayMaximum(256);
    page->addKnob(maxSamples);
    _imp->maxSamples = maxSamples;

    KnobDoublePtr depthMin = AppManager::createKnob<KnobDouble>(this, tr("Depth Min"));
    depthMin->setName("depthMin");
    depthMin->setHintToolTip(tr("Minimum depth. Samples with Z less than this are removed. "
                                "Only used in Depth Range mode."));
    depthMin->setAnimationEnabled(true);
    depthMin->setDefaultValue(0.0);
    depthMin->setMinimum(0.0);
    page->addKnob(depthMin);
    _imp->depthMin = depthMin;

    KnobDoublePtr depthMax = AppManager::createKnob<KnobDouble>(this, tr("Depth Max"));
    depthMax->setName("depthMax");
    depthMax->setHintToolTip(tr("Maximum depth. Samples with Z greater than this are removed. "
                                "Set to 0 for unlimited. Only used in Depth Range mode."));
    depthMax->setAnimationEnabled(true);
    depthMax->setDefaultValue(0.0);
    depthMax->setMinimum(0.0);
    page->addKnob(depthMax);
    _imp->depthMax = depthMax;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Filter deep samples by threshold, count, or depth range.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepSampleFilter::knobChanged(KnobI* k,
                               ValueChangedReasonEnum /*reason*/,
                               ViewSpec /*view*/,
                               double /*time*/,
                               bool /*originatedFromMainThread*/)
{
    if (_imp->mode.lock().get() == k ||
        _imp->alphaMin.lock().get() == k ||
        _imp->maxSamples.lock().get() == k ||
        _imp->depthMin.lock().get() == k ||
        _imp->depthMax.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepSampleFilter::getRegionOfDefinition(U64 /*hash*/,
                                        double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

DeepImagePtr
DeepSampleFilter::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepSampleFilter::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    int modeVal = _imp->mode.lock()->getValue();
    double alphaMinVal = _imp->alphaMin.lock()->getValue();
    int maxSamplesVal = _imp->maxSamples.lock()->getValue();
    double depthMinVal = _imp->depthMin.lock()->getValue();
    double depthMaxVal = _imp->depthMax.lock()->getValue();

    if (depthMaxVal <= 0.0) {
        depthMaxVal = std::numeric_limits<double>::max();
    }

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    // First pass: count how many samples survive per pixel
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
                bool keep = false;

                if (modeVal == 0) {
                    // Alpha Threshold
                    if (aIdx >= 0) {
                        float alpha = srcData[s * nChannels + aIdx];
                        keep = (alpha >= alphaMinVal);
                    } else {
                        keep = true;
                    }
                } else if (modeVal == 1) {
                    // Max Count
                    keep = (kept < maxSamplesVal);
                } else if (modeVal == 2) {
                    // Depth Range
                    if (zIdx >= 0) {
                        float z = srcData[s * nChannels + zIdx];
                        keep = (z >= depthMinVal && z <= depthMaxVal);
                    } else {
                        keep = true;
                    }
                }

                if (keep) {
                    ++kept;
                }
            }
            result->setSampleCount(x, y, kept);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy surviving samples
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
                bool keep = false;

                if (modeVal == 0) {
                    if (aIdx >= 0) {
                        float alpha = srcData[s * nChannels + aIdx];
                        keep = (alpha >= alphaMinVal);
                    } else {
                        keep = true;
                    }
                } else if (modeVal == 1) {
                    keep = (outIdx < maxSamplesVal);
                } else if (modeVal == 2) {
                    if (zIdx >= 0) {
                        float z = srcData[s * nChannels + zIdx];
                        keep = (z >= depthMinVal && z <= depthMaxVal);
                    } else {
                        keep = true;
                    }
                }

                if (keep) {
                    const float* src = srcData + s * nChannels;
                    float* dst = dstData + outIdx * nChannels;
                    for (int c = 0; c < nChannels; ++c) {
                        dst[c] = src[c];
                    }
                    ++outIdx;
                }
            }
        }
    }

    // Update info
    std::ostringstream ss;
    if (modeVal == 0) {
        ss << "Mode: Alpha Threshold (min=" << alphaMinVal << ")";
    } else if (modeVal == 1) {
        ss << "Mode: Max Count (max=" << maxSamplesVal << ")";
    } else {
        ss << "Mode: Depth Range [" << depthMinVal << ", ";
        if (depthMaxVal >= std::numeric_limits<double>::max() / 2.0) {
            ss << "unlimited";
        } else {
            ss << depthMaxVal;
        }
        ss << "]";
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

#include "moc_DeepSampleFilter.cpp"
