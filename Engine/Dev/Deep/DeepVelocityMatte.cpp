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

#include "DeepVelocityMatte.h"

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


struct DeepVelocityMattePrivate
{
    KnobDoubleWPtr threshold;
    KnobDoubleWPtr softness;
    KnobBoolWPtr invert;
    KnobStringWPtr info;
};


DeepVelocityMatte::DeepVelocityMatte(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepVelocityMattePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepVelocityMatte::~DeepVelocityMatte()
{
}

std::string
DeepVelocityMatte::getPluginDescription() const
{
    return tr("Generate a matte based on depth velocity — how much a sample's Z position changes "
              "relative to its neighbors, indicating motion or dynamic elements.").toStdString();
}

void
DeepVelocityMatte::addAcceptedComponents(int /*inputNb*/,
                                          std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepVelocityMatte::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepVelocityMatte::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                                   bool* /*defaultG*/,
                                                   bool* /*defaultB*/,
                                                   bool* /*defaultA*/) const
{
    return false;
}

void
DeepVelocityMatte::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr threshold = AppManager::createKnob<KnobDouble>(this, tr("Threshold"));
    threshold->setName("threshold");
    threshold->setHintToolTip(tr("Depth variance threshold. Samples with variance above this value "
                                 "are considered moving. Lower values detect subtler motion."));
    threshold->setAnimationEnabled(true);
    threshold->setDefaultValue(0.1);
    threshold->setMinimum(0.0);
    threshold->setDisplayMinimum(0.0);
    threshold->setDisplayMaximum(1.0);
    page->addKnob(threshold);
    _imp->threshold = threshold;

    KnobDoublePtr softness = AppManager::createKnob<KnobDouble>(this, tr("Softness"));
    softness->setName("softness");
    softness->setHintToolTip(tr("Softness of the matte transition at the threshold boundary."));
    softness->setAnimationEnabled(true);
    softness->setDefaultValue(0.05);
    softness->setMinimum(0.0);
    softness->setDisplayMinimum(0.0);
    softness->setDisplayMaximum(0.5);
    page->addKnob(softness);
    _imp->softness = softness;

    KnobBoolPtr invert = AppManager::createKnob<KnobBool>(this, tr("Invert"));
    invert->setName("invert");
    invert->setHintToolTip(tr("Invert the matte so that static samples are kept instead."));
    invert->setAnimationEnabled(false);
    invert->setDefaultValue(false);
    page->addKnob(invert);
    _imp->invert = invert;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set threshold to detect depth velocity (motion) in the deep image.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepVelocityMatte::knobChanged(KnobI* k,
                                ValueChangedReasonEnum /*reason*/,
                                ViewSpec /*view*/,
                                double /*time*/,
                                bool /*originatedFromMainThread*/)
{
    if (_imp->threshold.lock().get() == k ||
        _imp->softness.lock().get() == k ||
        _imp->invert.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepVelocityMatte::getRegionOfDefinition(U64 /*hash*/,
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
DeepVelocityMatte::getDeepImage() const
{
    return _lastDeepImage;
}

// Helper: get closest Z value at a neighbor pixel
static float
getClosestZ(const DeepImage* img, int x, int y, int zIdx, int nChannels, float refZ)
{
    int nSamples = img->getSampleCount(x, y);
    if (nSamples == 0) {
        return refZ;
    }
    const float* data = img->getSampleData(x, y);
    float bestZ = data[zIdx];
    float bestDist = std::fabs(bestZ - refZ);
    for (int s = 1; s < nSamples; ++s) {
        float z = data[s * nChannels + zIdx];
        float dist = std::fabs(z - refZ);
        if (dist < bestDist) {
            bestDist = dist;
            bestZ = z;
        }
    }
    return bestZ;
}

// Smoothstep helper
static float
smoothstep(float edge0, float edge1, float x)
{
    if (edge0 >= edge1) return (x >= edge0) ? 1.0f : 0.0f;
    float t = std::max(0.0f, std::min(1.0f, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0f - 2.0f * t);
}

StatusEnum
DeepVelocityMatte::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double thresholdVal = _imp->threshold.lock()->getValue();
    double softnessVal = _imp->softness.lock()->getValue();
    bool invertVal = _imp->invert.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");

    if (zIdx < 0 || aIdx < 0) {
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // Output: same sample counts, modulated by velocity matte
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            result->setSampleCount(x, y, nSamples);
        }
    }

    result->allocateFromSampleCounts();

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);
            if (!dstData) continue;

            // Neighbor pixel coords (clamped)
            int xl = (x - 1 >= dw.x1) ? (x - 1) : x;
            int xr = (x + 1 < dw.x2) ? (x + 1) : x;
            int yb = (y - 1 >= dw.y1) ? (y - 1) : y;
            int yt = (y + 1 < dw.y2) ? (y + 1) : y;

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                float zS = src[zIdx];

                // Compute depth variance: compare Z to neighbor pixel Z values at similar depth
                float zLeft   = getClosestZ(srcDeep.get(), xl, y, zIdx, nChannels, zS);
                float zRight  = getClosestZ(srcDeep.get(), xr, y, zIdx, nChannels, zS);
                float zBottom = getClosestZ(srcDeep.get(), x, yb, zIdx, nChannels, zS);
                float zTop    = getClosestZ(srcDeep.get(), x, yt, zIdx, nChannels, zS);

                // Compute variance as mean squared difference from center Z
                float dLeft   = zLeft - zS;
                float dRight  = zRight - zS;
                float dBottom = zBottom - zS;
                float dTop    = zTop - zS;

                float variance = (dLeft * dLeft + dRight * dRight +
                                  dBottom * dBottom + dTop * dTop) / 4.0f;
                float stddev = std::sqrt(variance);

                // Matte: high variance = moving
                float edgeLow = (float)(thresholdVal - softnessVal);
                float edgeHigh = (float)(thresholdVal + softnessVal);
                float matte = smoothstep(edgeLow, edgeHigh, stddev);

                if (invertVal) {
                    matte = 1.0f - matte;
                }

                // Copy all channels, then modulate RGB and A
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }
                if (rIdx >= 0) dst[rIdx] = src[rIdx] * matte;
                if (gIdx >= 0) dst[gIdx] = src[gIdx] * matte;
                if (bIdx >= 0) dst[bIdx] = src[bIdx] * matte;
                dst[aIdx] = src[aIdx] * matte;
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Threshold: " << thresholdVal
       << " | Softness: " << softnessVal
       << " | Total samples: " << result->totalSamples();
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

#include "moc_DeepVelocityMatte.cpp"
