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

#include "DeepAO.h"

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


struct DeepAOPrivate
{
    KnobDoubleWPtr radius;
    KnobIntWPtr kernelRadius;
    KnobIntWPtr samples;
    KnobDoubleWPtr intensity;
    KnobDoubleWPtr bias;
    KnobStringWPtr info;
};


DeepAO::DeepAO(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepAOPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepAO::~DeepAO()
{
}

std::string
DeepAO::getPluginDescription() const
{
    return tr("Compute ambient occlusion from deep sample depth distributions. "
              "For each sample, neighboring pixels' deep samples are checked — "
              "closely packed samples at similar depths produce darkening. "
              "This 2.5D approach uses actual depth data for dramatically better "
              "contact shadows than screen-space methods.").toStdString();
}

void
DeepAO::addAcceptedComponents(int /*inputNb*/,
                              std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepAO::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepAO::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                       bool* /*defaultG*/,
                                       bool* /*defaultB*/,
                                       bool* /*defaultA*/) const
{
    return false;
}

void
DeepAO::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr radius = AppManager::createKnob<KnobDouble>(this, tr("Radius"));
    radius->setName("radius");
    radius->setHintToolTip(tr("Depth search radius. Samples within this depth distance from the current sample "
                              "contribute to occlusion."));
    radius->setAnimationEnabled(true);
    radius->setDefaultValue(1.0);
    radius->setMinimum(0.001);
    radius->setDisplayMinimum(0.0);
    radius->setDisplayMaximum(10.0);
    page->addKnob(radius);
    _imp->radius = radius;

    KnobIntPtr kernelRadius = AppManager::createKnob<KnobInt>(this, tr("Kernel Radius"));
    kernelRadius->setName("kernelRadius");
    kernelRadius->setHintToolTip(tr("Screen-space search radius in pixels. Larger values look further for occluders "
                                    "but are slower."));
    kernelRadius->setAnimationEnabled(true);
    kernelRadius->setDefaultValue(5);
    kernelRadius->setMinimum(1);
    kernelRadius->setMaximum(20);
    page->addKnob(kernelRadius);
    _imp->kernelRadius = kernelRadius;

    KnobIntPtr samples = AppManager::createKnob<KnobInt>(this, tr("Samples"));
    samples->setName("samples");
    samples->setHintToolTip(tr("Number of sample offsets within the kernel to check. Higher values produce smoother "
                               "results but are slower."));
    samples->setAnimationEnabled(true);
    samples->setDefaultValue(16);
    samples->setMinimum(4);
    samples->setMaximum(64);
    page->addKnob(samples);
    _imp->samples = samples;

    KnobDoublePtr intensity = AppManager::createKnob<KnobDouble>(this, tr("Intensity"));
    intensity->setName("intensity");
    intensity->setHintToolTip(tr("Strength of the ambient occlusion darkening effect."));
    intensity->setAnimationEnabled(true);
    intensity->setDefaultValue(1.0);
    intensity->setMinimum(0.0);
    intensity->setDisplayMinimum(0.0);
    intensity->setDisplayMaximum(5.0);
    page->addKnob(intensity);
    _imp->intensity = intensity;

    KnobDoublePtr bias = AppManager::createKnob<KnobDouble>(this, tr("Bias"));
    bias->setName("bias");
    bias->setHintToolTip(tr("Depth bias to avoid self-occlusion. Neighbor samples within this depth distance "
                            "of the current sample are ignored."));
    bias->setAnimationEnabled(true);
    bias->setDefaultValue(0.01);
    bias->setMinimum(0.0);
    bias->setDisplayMinimum(0.0);
    bias->setDisplayMaximum(1.0);
    page->addKnob(bias);
    _imp->bias = bias;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Adjust radius and kernel to control AO quality.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepAO::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    if (_imp->radius.lock().get() == k ||
        _imp->kernelRadius.lock().get() == k ||
        _imp->samples.lock().get() == k ||
        _imp->intensity.lock().get() == k ||
        _imp->bias.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepAO::getRegionOfDefinition(U64 /*hash*/,
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
DeepAO::getDeepImage() const
{
    return _lastDeepImage;
}

// Simple deterministic hash for generating sample offsets
static inline unsigned int aoHash(unsigned int seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed = seed + (seed << 3u);
    seed = seed ^ (seed >> 4u);
    seed = seed * 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

StatusEnum
DeepAO::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double radiusVal = _imp->radius.lock()->getValue();
    int kernelRadiusVal = _imp->kernelRadius.lock()->getValue();
    int samplesVal = _imp->samples.lock()->getValue();
    double intensityVal = _imp->intensity.lock()->getValue();
    double biasVal = _imp->bias.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdxCached = srcDeep->findChannelIndex("A");

    if (zIdx < 0) {
        // No Z channel — pass through unchanged
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    int w = dw.x2 - dw.x1;
    int h = dw.y2 - dw.y1;

    // PRE-BUILD DEPTH STRUCTURE
    // For each pixel, extract a compact list of Z values for fast neighbor lookup
    std::vector<std::vector<float> > depthLists(w * h);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                continue;
            }
            int pixIdx = (y - dw.y1) * w + (x - dw.x1);
            depthLists[pixIdx].resize(nSamples);
            const float* srcData = srcDeep->getSampleData(x, y);
            for (int s = 0; s < nSamples; ++s) {
                depthLists[pixIdx][s] = srcData[s * nChannels + zIdx];
            }
        }
    }

    // Pre-generate deterministic sample offsets within the kernel
    // These are (dx, dy) pairs within kernelRadius, chosen via hashing
    std::vector<int> sampleDx(samplesVal);
    std::vector<int> sampleDy(samplesVal);
    std::vector<float> sampleScreenDist(samplesVal);
    int validOffsets = 0;

    for (int i = 0; i < samplesVal * 4 && validOffsets < samplesVal; ++i) {
        unsigned int h1 = aoHash(i * 2u + 1u);
        unsigned int h2 = aoHash(i * 2u + 2u);
        int dx = (int)(h1 % (unsigned int)(kernelRadiusVal * 2 + 1)) - kernelRadiusVal;
        int dy = (int)(h2 % (unsigned int)(kernelRadiusVal * 2 + 1)) - kernelRadiusVal;
        float screenDist = std::sqrt((float)(dx * dx + dy * dy));
        if (screenDist > (float)kernelRadiusVal || (dx == 0 && dy == 0)) {
            continue;
        }
        sampleDx[validOffsets] = dx;
        sampleDy[validOffsets] = dy;
        sampleScreenDist[validOffsets] = screenDist;
        ++validOffsets;
    }

    // Compute AO per pixel per sample — store in a flat structure matching the source
    // aoFactors[pixIdx][sampleIdx] = ao multiplier for that sample
    std::vector<std::vector<float> > aoFactors(w * h);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                continue;
            }
            int pixIdx = (y - dw.y1) * w + (x - dw.x1);
            aoFactors[pixIdx].resize(nSamples);

            const float* srcData = srcDeep->getSampleData(x, y);

            for (int s = 0; s < nSamples; ++s) {
                float zS = srcData[s * nChannels + zIdx];
                double occlusion = 0.0;
                double totalWeight = 0.0;

                for (int oi = 0; oi < validOffsets; ++oi) {
                    int nx = x + sampleDx[oi];
                    int ny = y + sampleDy[oi];

                    // Skip if outside bounds
                    if (nx < dw.x1 || nx >= dw.x2 || ny < dw.y1 || ny >= dw.y2) {
                        continue;
                    }

                    float screenDist = sampleScreenDist[oi];
                    double spatialWeight = 1.0 - (double)(screenDist / (float)kernelRadiusVal);

                    int nPixIdx = (ny - dw.y1) * w + (nx - dw.x1);
                    const std::vector<float>& neighborDepths = depthLists[nPixIdx];

                    for (size_t nd = 0; nd < neighborDepths.size(); ++nd) {
                        float zN = neighborDepths[nd];
                        double depthDiff = std::fabs((double)zN - (double)zS);

                        if (depthDiff < biasVal) {
                            continue; // self-occlusion avoidance
                        }
                        if (depthDiff < radiusVal) {
                            double depthWeight = 1.0 - (depthDiff / radiusVal);
                            occlusion += spatialWeight * depthWeight;
                            totalWeight += spatialWeight;
                        }
                    }
                }

                double ao;
                if (totalWeight > 0.0) {
                    double raw = intensityVal * occlusion / totalWeight;
                    if (raw < 0.0) raw = 0.0;
                    if (raw > 1.0) raw = 1.0;
                    ao = 1.0 - raw;
                } else {
                    ao = 1.0;
                }
                aoFactors[pixIdx][s] = (float)ao;
            }
        }
    }

    // OUTPUT: Create result deep image, same structure as source
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            result->setSampleCount(x, y, nSamples);
        }
    }

    result->allocateFromSampleCounts();

    // Copy all samples, multiply R,G,B by ao factor (A, Z, ZBack unchanged)
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                continue;
            }

            int pixIdx = (y - dw.y1) * w + (x - dw.x1);
            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);
            if (!dstData) {
                continue;
            }

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;
                float ao = aoFactors[pixIdx][s];

                // Copy all channels, then apply AO:
                // - RGB multiplied by ao (darkened in occluded areas)
                // - Alpha SET to ao (so flattened output carries AO as a gradable mask)
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }
                if (rIdx >= 0) dst[rIdx] = ao;
                if (gIdx >= 0) dst[gIdx] = ao;
                if (bIdx >= 0) dst[bIdx] = ao;
                if (aIdxCached >= 0) dst[aIdxCached] = ao;
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "AO: radius=" << radiusVal
       << " kernel=" << kernelRadiusVal
       << " samples=" << samplesVal
       << " intensity=" << intensityVal
       << " bias=" << biasVal
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

#include "moc_DeepAO.cpp"
