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

#include "DeepFog.h"

#include <algorithm>
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


struct DeepFogPrivate
{
    KnobDoubleWPtr fogR;
    KnobDoubleWPtr fogG;
    KnobDoubleWPtr fogB;
    KnobDoubleWPtr density;
    KnobDoubleWPtr nearDepth;
    KnobDoubleWPtr farDepth;
    KnobChoiceWPtr falloffType;
    KnobStringWPtr info;
};


DeepFog::DeepFog(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepFogPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepFog::~DeepFog()
{
}

std::string
DeepFog::getPluginDescription() const
{
    return tr("Insert volumetric fog between deep samples at a specified depth range. "
              "Uses Beer-Lambert attenuation for physically-based fog density. "
              "The fog correctly wraps around semi-transparent elements like hair, smoke, and glass.").toStdString();
}

void
DeepFog::addAcceptedComponents(int /*inputNb*/,
                               std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepFog::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepFog::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                        bool* /*defaultG*/,
                                        bool* /*defaultB*/,
                                        bool* /*defaultA*/) const
{
    return false;
}

void
DeepFog::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Fog color
    KnobDoublePtr fr = AppManager::createKnob<KnobDouble>(this, tr("Fog Red"));
    fr->setName("fogR");
    fr->setHintToolTip(tr("Red component of the fog color."));
    fr->setAnimationEnabled(true);
    fr->setDefaultValue(0.5);
    fr->setMinimum(0.0);
    fr->setMaximum(1.0);
    fr->setDisplayMinimum(0.0);
    fr->setDisplayMaximum(1.0);
    page->addKnob(fr);
    _imp->fogR = fr;

    KnobDoublePtr fg = AppManager::createKnob<KnobDouble>(this, tr("Fog Green"));
    fg->setName("fogG");
    fg->setHintToolTip(tr("Green component of the fog color."));
    fg->setAnimationEnabled(true);
    fg->setDefaultValue(0.5);
    fg->setMinimum(0.0);
    fg->setMaximum(1.0);
    fg->setDisplayMinimum(0.0);
    fg->setDisplayMaximum(1.0);
    page->addKnob(fg);
    _imp->fogG = fg;

    KnobDoublePtr fb = AppManager::createKnob<KnobDouble>(this, tr("Fog Blue"));
    fb->setName("fogB");
    fb->setHintToolTip(tr("Blue component of the fog color."));
    fb->setAnimationEnabled(true);
    fb->setDefaultValue(0.5);
    fb->setMinimum(0.0);
    fb->setMaximum(1.0);
    fb->setDisplayMinimum(0.0);
    fb->setDisplayMaximum(1.0);
    page->addKnob(fb);
    _imp->fogB = fb;

    // Density
    KnobDoublePtr dens = AppManager::createKnob<KnobDouble>(this, tr("Density"));
    dens->setName("density");
    dens->setHintToolTip(tr("Fog density. Higher values produce thicker fog."));
    dens->setAnimationEnabled(true);
    dens->setDefaultValue(0.1);
    dens->setMinimum(0.0);
    dens->setDisplayMinimum(0.0);
    dens->setDisplayMaximum(2.0);
    page->addKnob(dens);
    _imp->density = dens;

    // Depth range
    KnobDoublePtr nd = AppManager::createKnob<KnobDouble>(this, tr("Near Depth"));
    nd->setName("nearDepth");
    nd->setHintToolTip(tr("Fog starts at this Z depth."));
    nd->setAnimationEnabled(true);
    nd->setDefaultValue(0.0);
    nd->setMinimum(0.0);
    page->addKnob(nd);
    _imp->nearDepth = nd;

    KnobDoublePtr fd = AppManager::createKnob<KnobDouble>(this, tr("Far Depth"));
    fd->setName("farDepth");
    fd->setHintToolTip(tr("Fog ends at this Z depth. Set to 0 for unlimited (fog extends to infinity)."));
    fd->setAnimationEnabled(true);
    fd->setDefaultValue(0.0);
    fd->setMinimum(0.0);
    page->addKnob(fd);
    _imp->farDepth = fd;

    // Falloff type
    KnobChoicePtr falloff = AppManager::createKnob<KnobChoice>(this, tr("Falloff"));
    falloff->setName("falloffType");
    falloff->setHintToolTip(tr("Fog attenuation model.\n"
                                "Exponential: Beer-Lambert law, physically based.\n"
                                "Linear: Simple linear opacity ramp."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Exponential", "Exponential", ""));
        entries.push_back(ChoiceOption("Linear", "Linear", ""));
        falloff->populateChoices(entries);
    }
    falloff->setDefaultValue(0);
    page->addKnob(falloff);
    _imp->falloffType = falloff;

    // Info
    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set depth range and density to insert volumetric fog.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepFog::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    if (_imp->density.lock().get() == k ||
        _imp->nearDepth.lock().get() == k ||
        _imp->farDepth.lock().get() == k ||
        _imp->falloffType.lock().get() == k ||
        _imp->fogR.lock().get() == k ||
        _imp->fogG.lock().get() == k ||
        _imp->fogB.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepFog::getRegionOfDefinition(U64 /*hash*/,
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
DeepFog::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepFog::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double fogRVal = _imp->fogR.lock()->getValue();
    double fogGVal = _imp->fogG.lock()->getValue();
    double fogBVal = _imp->fogB.lock()->getValue();
    double densityVal = _imp->density.lock()->getValue();
    double nearDepthVal = _imp->nearDepth.lock()->getValue();
    double farDepthVal = _imp->farDepth.lock()->getValue();
    int falloffTypeVal = _imp->falloffType.lock()->getValue();

    // If farDepth is 0, treat as unlimited
    if (farDepthVal <= 0.0) {
        farDepthVal = std::numeric_limits<double>::max();
    }

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");
    int zbIdx = srcDeep->findChannelIndex("ZBack");

    if (zIdx < 0) {
        // No Z channel — can't insert fog, pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // Pass 1: count output samples per pixel
    // For each pixel, we walk sorted samples front-to-back.
    // Between consecutive samples (and before first / after last),
    // if the gap overlaps [nearDepth, farDepth], we insert 1 fog sample.
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);

            if (nSamples == 0) {
                // No source samples — check if fog range is valid, insert one fog sample
                // We need at least some depth span to create fog
                if (nearDepthVal < farDepthVal && densityVal > 0.0) {
                    result->setSampleCount(x, y, 1);
                } else {
                    result->setSampleCount(x, y, 0);
                }
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);

            // Collect Z front values and sort
            std::vector<std::pair<float, int>> sortedSamples(nSamples);
            for (int s = 0; s < nSamples; ++s) {
                float z = srcData[s * nChannels + zIdx];
                sortedSamples[s] = std::make_pair(z, s);
            }
            std::sort(sortedSamples.begin(), sortedSamples.end());

            int fogCount = 0;

            // Check gap before first sample
            float firstZ = sortedSamples[0].first;
            if (firstZ > nearDepthVal && nearDepthVal < farDepthVal) {
                float gapStart = (float)nearDepthVal;
                float gapEnd = std::min(firstZ, (float)farDepthVal);
                if (gapEnd > gapStart) {
                    ++fogCount;
                }
            }

            // Check gaps between consecutive samples
            for (int s = 0; s < nSamples - 1; ++s) {
                int si = sortedSamples[s].second;
                float zBack = (zbIdx >= 0) ? srcData[si * nChannels + zbIdx] : sortedSamples[s].first;
                float nextZ = sortedSamples[s + 1].first;

                float gapStart = std::max(zBack, (float)nearDepthVal);
                float gapEnd = std::min(nextZ, (float)farDepthVal);
                if (gapEnd > gapStart) {
                    ++fogCount;
                }
            }

            // Check gap after last sample
            {
                int si = sortedSamples[nSamples - 1].second;
                float lastZBack = (zbIdx >= 0) ? srcData[si * nChannels + zbIdx] : sortedSamples[nSamples - 1].first;
                if (lastZBack < farDepthVal && farDepthVal > nearDepthVal) {
                    float gapStart = std::max(lastZBack, (float)nearDepthVal);
                    float gapEnd = (float)farDepthVal;
                    if (gapEnd > gapStart && gapEnd < std::numeric_limits<float>::max()) {
                        ++fogCount;
                    } else if (gapEnd > gapStart && farDepthVal >= std::numeric_limits<double>::max() / 2.0) {
                        // Unlimited far: create fog sample with a large but finite extent
                        ++fogCount;
                    }
                }
            }

            result->setSampleCount(x, y, nSamples + fogCount);
        }
    }

    result->allocateFromSampleCounts();

    // Pass 2: fill samples — walk front-to-back, inserting fog samples in gaps
    int totalFogInserted = 0;

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            float* dstData = result->getSampleData(x, y);
            if (!dstData) continue;

            int outIdx = 0;

            if (nSamples == 0) {
                // Insert a single fog sample if range is valid
                if (nearDepthVal < farDepthVal && densityVal > 0.0) {
                    float gapStart = (float)nearDepthVal;
                    float gapEnd = (farDepthVal >= std::numeric_limits<double>::max() / 2.0)
                                   ? (float)(nearDepthVal + 1000.0) : (float)farDepthVal;
                    float gapLength = gapEnd - gapStart;

                    float fogAlpha;
                    if (falloffTypeVal == 0) {
                        // Exponential (Beer-Lambert)
                        fogAlpha = 1.0f - std::exp(-(float)densityVal * gapLength);
                    } else {
                        // Linear
                        fogAlpha = std::min(1.0f, std::max(0.0f, (float)densityVal * gapLength));
                    }

                    float* dst = dstData + outIdx * nChannels;
                    for (int c = 0; c < nChannels; ++c) dst[c] = 0.0f;
                    if (rIdx >= 0) dst[rIdx] = (float)fogRVal * fogAlpha;
                    if (gIdx >= 0) dst[gIdx] = (float)fogGVal * fogAlpha;
                    if (bIdx >= 0) dst[bIdx] = (float)fogBVal * fogAlpha;
                    if (aIdx >= 0) dst[aIdx] = fogAlpha;
                    dst[zIdx] = gapStart;
                    if (zbIdx >= 0) dst[zbIdx] = gapEnd;
                    ++outIdx;
                    ++totalFogInserted;
                }
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);

            // Sort samples front-to-back by Z
            std::vector<std::pair<float, int>> sortedSamples(nSamples);
            for (int s = 0; s < nSamples; ++s) {
                float z = srcData[s * nChannels + zIdx];
                sortedSamples[s] = std::make_pair(z, s);
            }
            std::sort(sortedSamples.begin(), sortedSamples.end());

            // Helper lambda to insert a fog sample
            auto insertFog = [&](float gapStart, float gapEnd) {
                float gapLength = gapEnd - gapStart;
                if (gapLength <= 0.0f) return;

                float fogAlpha;
                if (falloffTypeVal == 0) {
                    // Exponential (Beer-Lambert)
                    fogAlpha = 1.0f - std::exp(-(float)densityVal * gapLength);
                } else {
                    // Linear
                    fogAlpha = std::min(1.0f, std::max(0.0f, (float)densityVal * gapLength));
                }

                float* dst = dstData + outIdx * nChannels;
                for (int c = 0; c < nChannels; ++c) dst[c] = 0.0f;
                if (rIdx >= 0) dst[rIdx] = (float)fogRVal * fogAlpha;  // premultiplied
                if (gIdx >= 0) dst[gIdx] = (float)fogGVal * fogAlpha;
                if (bIdx >= 0) dst[bIdx] = (float)fogBVal * fogAlpha;
                if (aIdx >= 0) dst[aIdx] = fogAlpha;
                dst[zIdx] = gapStart;
                if (zbIdx >= 0) dst[zbIdx] = gapEnd;
                ++outIdx;
                ++totalFogInserted;
            };

            // Check gap before first sample
            {
                float firstZ = sortedSamples[0].first;
                if (firstZ > nearDepthVal && nearDepthVal < farDepthVal) {
                    float gapStart = (float)nearDepthVal;
                    float gapEnd = std::min(firstZ, (float)farDepthVal);
                    if (gapEnd > gapStart) {
                        insertFog(gapStart, gapEnd);
                    }
                }
            }

            // Walk sorted samples, inserting fog between consecutive pairs
            for (int s = 0; s < nSamples; ++s) {
                int si = sortedSamples[s].second;

                // Copy original sample verbatim
                const float* src = srcData + si * nChannels;
                float* dst = dstData + outIdx * nChannels;
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }
                ++outIdx;

                // Check gap after this sample and before next
                float zBack = (zbIdx >= 0) ? src[zbIdx] : sortedSamples[s].first;

                if (s < nSamples - 1) {
                    float nextZ = sortedSamples[s + 1].first;
                    float gapStart = std::max(zBack, (float)nearDepthVal);
                    float gapEnd = std::min(nextZ, (float)farDepthVal);
                    if (gapEnd > gapStart) {
                        insertFog(gapStart, gapEnd);
                    }
                } else {
                    // Gap after last sample
                    if (zBack < farDepthVal && farDepthVal > nearDepthVal) {
                        float gapStart = std::max(zBack, (float)nearDepthVal);
                        float gapEnd;
                        if (farDepthVal >= std::numeric_limits<double>::max() / 2.0) {
                            // Unlimited far: use a large but finite extent
                            gapEnd = gapStart + 1000.0f;
                        } else {
                            gapEnd = (float)farDepthVal;
                        }
                        if (gapEnd > gapStart) {
                            insertFog(gapStart, gapEnd);
                        }
                    }
                }
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Fog samples inserted: " << totalFogInserted
       << " | Density: " << densityVal
       << " | Range: " << nearDepthVal << " - ";
    if (farDepthVal >= std::numeric_limits<double>::max() / 2.0) {
        ss << "unlimited";
    } else {
        ss << farDepthVal;
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

#include "moc_DeepFog.cpp"
