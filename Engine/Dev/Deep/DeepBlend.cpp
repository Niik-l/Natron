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

#include "DeepBlend.h"

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
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepBlendPrivate
{
    KnobDoubleWPtr mix;
    KnobStringWPtr info;
};


DeepBlend::DeepBlend(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepBlendPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepBlend::~DeepBlend()
{
}

std::string
DeepBlend::getPluginDescription() const
{
    return tr("Blend between two deep images. For each sample, interpolates color values "
              "between the two inputs based on the mix factor. Samples are matched by depth proximity.").toStdString();
}

void
DeepBlend::addAcceptedComponents(int /*inputNb*/,
                                 std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepBlend::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepBlend::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                          bool* /*defaultG*/,
                                          bool* /*defaultB*/,
                                          bool* /*defaultA*/) const
{
    return false;
}

void
DeepBlend::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr mix = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
    mix->setName("mix");
    mix->setHintToolTip(tr("Blend factor between the two inputs. "
                           "0.0 = only Deep A, 1.0 = only Deep B, 0.5 = equal blend."));
    mix->setAnimationEnabled(true);
    mix->setDefaultValue(0.5);
    mix->setMinimum(0.0);
    mix->setMaximum(1.0);
    mix->setDisplayMinimum(0.0);
    mix->setDisplayMaximum(1.0);
    page->addKnob(mix);
    _imp->mix = mix;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect two deep images to blend between them.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepBlend::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    if (_imp->mix.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepBlend::getRegionOfDefinition(U64 /*hash*/,
                                 double /*time*/,
                                 const RenderScale& /*scale*/,
                                 ViewIdx /*view*/,
                                 RectD* rod)
{
    // Union of both inputs
    EffectInstancePtr inputA = getInput(0);
    EffectInstancePtr inputB = getInput(1);
    if (!inputA && !inputB) return eStatusFailed;

    bool isProjectFormat = false;
    RectD rodA, rodB;
    bool hasA = false, hasB = false;

    if (inputA) {
        if (inputA->getRegionOfDefinition_public(inputA->getHash(), 0, RenderScale(), ViewIdx(0), &rodA, &isProjectFormat) == eStatusOK) {
            hasA = true;
        }
    }
    if (inputB) {
        if (inputB->getRegionOfDefinition_public(inputB->getHash(), 0, RenderScale(), ViewIdx(0), &rodB, &isProjectFormat) == eStatusOK) {
            hasB = true;
        }
    }

    if (hasA && hasB) {
        rod->x1 = std::min(rodA.x1, rodB.x1);
        rod->y1 = std::min(rodA.y1, rodB.y1);
        rod->x2 = std::max(rodA.x2, rodB.x2);
        rod->y2 = std::max(rodA.y2, rodB.y2);
    } else if (hasA) {
        *rod = rodA;
    } else if (hasB) {
        *rod = rodB;
    } else {
        return eStatusFailed;
    }

    return eStatusOK;
}

DeepImagePtr
DeepBlend::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepBlend::render(const RenderActionArgs& args)
{
    EffectInstancePtr inputA = getInput(0);
    EffectInstancePtr inputB = getInput(1);

    DeepImagePtr deepA = getDeepImageFromEffect(inputA.get());
    DeepImagePtr deepB = getDeepImageFromEffect(inputB.get());

    if (!deepA && !deepB) {
        return eStatusFailed;
    }

    double mixVal = _imp->mix.lock()->getValue();
    float wA = (float)(1.0 - mixVal);
    float wB = (float)mixVal;

    // If only one input, use it with appropriate weight
    if (!deepA) {
        deepA = deepB;
        wA = 0.0f;
        wB = 1.0f;
    }
    if (!deepB) {
        deepB = deepA;
        wA = 1.0f;
        wB = 0.0f;
    }

    const RectI& dwA = deepA->getDataWindow();
    const RectI& dwB = deepB->getDataWindow();

    // Compute union data window
    RectI dw;
    dw.x1 = std::min(dwA.x1, dwB.x1);
    dw.y1 = std::min(dwA.y1, dwB.y1);
    dw.x2 = std::max(dwA.x2, dwB.x2);
    dw.y2 = std::max(dwA.y2, dwB.y2);

    int nChannelsA = deepA->getNumChannels();
    int nChannelsB = deepB->getNumChannels();

    // Use the channel set from input A for the output
    int nChannels = nChannelsA;
    const std::vector<std::string>& chanNames = deepA->getChannelNames();

    // Find R,G,B,A indices in each input
    int rIdxA = deepA->findChannelIndex("R");
    int gIdxA = deepA->findChannelIndex("G");
    int bIdxA = deepA->findChannelIndex("B");
    int aIdxA = deepA->findChannelIndex("A");

    int rIdxB = deepB->findChannelIndex("R");
    int gIdxB = deepB->findChannelIndex("G");
    int bIdxB = deepB->findChannelIndex("B");
    int aIdxB = deepB->findChannelIndex("A");

    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    // First pass: count output samples (sum of A and B samples per pixel)
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int samplesA = 0, samplesB = 0;
            if (x >= dwA.x1 && x < dwA.x2 && y >= dwA.y1 && y < dwA.y2) {
                samplesA = deepA->getSampleCount(x, y);
            }
            if (x >= dwB.x1 && x < dwB.x2 && y >= dwB.y1 && y < dwB.y2) {
                samplesB = deepB->getSampleCount(x, y);
            }
            result->setSampleCount(x, y, samplesA + samplesB);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy samples with mix weights applied
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int samplesA = 0, samplesB = 0;
            const float* srcDataA = nullptr;
            const float* srcDataB = nullptr;

            if (x >= dwA.x1 && x < dwA.x2 && y >= dwA.y1 && y < dwA.y2) {
                samplesA = deepA->getSampleCount(x, y);
                if (samplesA > 0) srcDataA = deepA->getSampleData(x, y);
            }
            if (x >= dwB.x1 && x < dwB.x2 && y >= dwB.y1 && y < dwB.y2) {
                samplesB = deepB->getSampleCount(x, y);
                if (samplesB > 0) srcDataB = deepB->getSampleData(x, y);
            }

            int totalSamples = samplesA + samplesB;
            if (totalSamples == 0) continue;

            float* dstData = result->getSampleData(x, y);
            if (!dstData) continue;

            int outIdx = 0;

            // Copy A samples, scaled by wA
            for (int s = 0; s < samplesA; ++s) {
                const float* src = srcDataA + s * nChannelsA;
                float* dst = dstData + outIdx * nChannels;
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }
                // Scale RGBA by wA
                if (rIdxA >= 0) dst[rIdxA] = src[rIdxA] * wA;
                if (gIdxA >= 0) dst[gIdxA] = src[gIdxA] * wA;
                if (bIdxA >= 0) dst[bIdxA] = src[bIdxA] * wA;
                if (aIdxA >= 0) dst[aIdxA] = src[aIdxA] * wA;
                ++outIdx;
            }

            // Copy B samples, scaled by wB
            for (int s = 0; s < samplesB; ++s) {
                const float* src = srcDataB + s * nChannelsB;
                float* dst = dstData + outIdx * nChannels;
                // Initialize all channels to 0
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = 0.0f;
                }
                // Map B channels to output channels by name
                if (rIdxB >= 0 && rIdxA >= 0) dst[rIdxA] = src[rIdxB] * wB;
                if (gIdxB >= 0 && gIdxA >= 0) dst[gIdxA] = src[gIdxB] * wB;
                if (bIdxB >= 0 && bIdxA >= 0) dst[bIdxA] = src[bIdxB] * wB;
                if (aIdxB >= 0 && aIdxA >= 0) dst[aIdxA] = src[aIdxB] * wB;
                // Copy Z/ZBack if present
                int zIdxA = deepA->findChannelIndex("Z");
                int zIdxB = deepB->findChannelIndex("Z");
                int zbIdxA = deepA->findChannelIndex("ZBack");
                int zbIdxB = deepB->findChannelIndex("ZBack");
                if (zIdxB >= 0 && zIdxA >= 0) dst[zIdxA] = src[zIdxB];
                if (zbIdxB >= 0 && zbIdxA >= 0) dst[zbIdxA] = src[zbIdxB];
                ++outIdx;
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Mix: " << mixVal
       << " | A samples: " << (deepA ? deepA->totalSamples() : 0)
       << " | B samples: " << (deepB ? deepB->totalSamples() : 0)
       << " | Output samples: " << result->totalSamples();
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

#include "moc_DeepBlend.cpp"
