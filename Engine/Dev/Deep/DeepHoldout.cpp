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
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DeepHoldout.h"

#include <cassert>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


DeepHoldout::DeepHoldout(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepHoldout::~DeepHoldout()
{
}

std::string
DeepHoldout::getPluginDescription() const
{
    return tr("Remove deep samples that are behind a holdout matte. "
              "For each pixel, the holdout input's accumulated alpha at each depth is used to "
              "attenuate or remove samples from the main deep input that are at or behind that depth.\n\n"
              "Input 'Deep': The deep image to apply holdout to.\n"
              "Input 'Holdout': A deep image used as the holdout matte.\n\n"
              "This is useful for inserting flat CG elements between deep layers — "
              "use DeepHoldout to cut a hole in the deep image at the insertion depth.").toStdString();
}

void
DeepHoldout::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepHoldout::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepHoldout::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepHoldout::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect deep image to 'Deep' and holdout matte to 'Holdout'.");
    page->addKnob(info);
}

StatusEnum
DeepHoldout::getRegionOfDefinition(U64 /*hash*/,
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
DeepHoldout::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepHoldout::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    EffectInstancePtr holdoutInput = getInput(1);

    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    DeepImagePtr holdoutDeep = getDeepImageFromEffect(holdoutInput.get());

    // If no holdout connected, pass through
    if (!holdoutDeep) {
        _lastDeepImage = srcDeep;
    } else {
        const RectI& dw = srcDeep->getDataWindow();
        int nChannels = srcDeep->getNumChannels();
        const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

        int zIdx = srcDeep->findChannelIndex("Z");
        int aIdx = srcDeep->findChannelIndex("A");
        int holdZIdx = holdoutDeep->findChannelIndex("Z");
        int holdAIdx = holdoutDeep->findChannelIndex("A");

        if (zIdx < 0 || aIdx < 0 || holdZIdx < 0 || holdAIdx < 0) {
            // Missing required channels, pass through
            _lastDeepImage = srcDeep;
        } else {
            // For each pixel, compute the holdout's accumulated alpha at each depth,
            // then attenuate the main deep samples accordingly.
            DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);
            int holdNChannels = holdoutDeep->getNumChannels();

            // First pass: count surviving samples
            for (int y = dw.y1; y < dw.y2; ++y) {
                for (int x = dw.x1; x < dw.x2; ++x) {
                    int nSamples = srcDeep->getSampleCount(x, y);
                    int holdSamples = holdoutDeep->getSampleCount(x, y);

                    if (nSamples == 0 || holdSamples == 0) {
                        result->setSampleCount(x, y, nSamples);
                        continue;
                    }

                    // Find the maximum accumulated alpha from the holdout
                    // Composite holdout front-to-back to get holdout alpha at each depth
                    const float* holdData = holdoutDeep->getSampleData(x, y);
                    float holdoutAccumA = 0.0f;
                    for (int hs = 0; hs < holdSamples; ++hs) {
                        float ha = holdData[hs * holdNChannels + holdAIdx];
                        holdoutAccumA = holdoutAccumA + ha * (1.0f - holdoutAccumA);
                    }

                    // Get the holdout's front-most Z
                    float holdoutFrontZ = holdData[0 * holdNChannels + holdZIdx];

                    // Count samples that are in front of the holdout
                    const float* srcData = srcDeep->getSampleData(x, y);
                    int kept = 0;
                    for (int s = 0; s < nSamples; ++s) {
                        float z = srcData[s * nChannels + zIdx];
                        // Keep samples that are in front of the holdout
                        if (z < holdoutFrontZ) {
                            ++kept;
                        } else {
                            // Attenuate by (1 - holdout_alpha)
                            float attenuation = 1.0f - holdoutAccumA;
                            if (attenuation > 0.001f) {
                                ++kept;
                            }
                        }
                    }
                    result->setSampleCount(x, y, kept);
                }
            }

            result->allocateFromSampleCounts();

            // Second pass: copy and attenuate
            for (int y = dw.y1; y < dw.y2; ++y) {
                for (int x = dw.x1; x < dw.x2; ++x) {
                    int nSamples = srcDeep->getSampleCount(x, y);
                    int holdSamples = holdoutDeep->getSampleCount(x, y);

                    if (nSamples == 0) continue;

                    const float* srcData = srcDeep->getSampleData(x, y);
                    float* dstData = result->getSampleData(x, y);
                    if (!dstData) continue;

                    if (holdSamples == 0) {
                        // No holdout — straight copy
                        for (int s = 0; s < nSamples; ++s) {
                            for (int c = 0; c < nChannels; ++c) {
                                dstData[s * nChannels + c] = srcData[s * nChannels + c];
                            }
                        }
                        continue;
                    }

                    const float* holdData = holdoutDeep->getSampleData(x, y);
                    float holdoutAccumA = 0.0f;
                    for (int hs = 0; hs < holdSamples; ++hs) {
                        float ha = holdData[hs * holdNChannels + holdAIdx];
                        holdoutAccumA = holdoutAccumA + ha * (1.0f - holdoutAccumA);
                    }
                    float holdoutFrontZ = holdData[0 * holdNChannels + holdZIdx];

                    int outIdx = 0;
                    for (int s = 0; s < nSamples; ++s) {
                        float z = srcData[s * nChannels + zIdx];

                        if (z < holdoutFrontZ) {
                            // In front of holdout — copy unchanged
                            for (int c = 0; c < nChannels; ++c) {
                                dstData[outIdx * nChannels + c] = srcData[s * nChannels + c];
                            }
                            ++outIdx;
                        } else {
                            float attenuation = 1.0f - holdoutAccumA;
                            if (attenuation > 0.001f) {
                                // Behind holdout — attenuate RGBA
                                for (int c = 0; c < nChannels; ++c) {
                                    float val = srcData[s * nChannels + c];
                                    // Attenuate color and alpha channels, keep Z unchanged
                                    if (chanNames[c] == "Z" || chanNames[c] == "ZBack") {
                                        dstData[outIdx * nChannels + c] = val;
                                    } else {
                                        dstData[outIdx * nChannels + c] = val * attenuation;
                                    }
                                }
                                ++outIdx;
                            }
                            // else: fully occluded, skip
                        }
                    }
                }
            }

            _lastDeepImage = result;
        }
    }

    // Produce flattened preview
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (outImg && _lastDeepImage) {
        _lastDeepImage->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepHoldout.cpp"
