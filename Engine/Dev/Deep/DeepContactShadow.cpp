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

#include "DeepContactShadow.h"

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


struct DeepContactShadowPrivate
{
    KnobDoubleWPtr proximity;
    KnobDoubleWPtr intensity;
    KnobIntWPtr kernelRadius;
    KnobDoubleWPtr selfShadowBias;
    KnobStringWPtr info;
};


DeepContactShadow::DeepContactShadow(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepContactShadowPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepContactShadow::~DeepContactShadow()
{
}

std::string
DeepContactShadow::getPluginDescription() const
{
    return tr("Generate contact shadows by darkening areas where deep samples from different objects "
              "are close together in Z depth. This creates automatic darkening at contact points "
              "between overlapping geometry without requiring a separate render pass.").toStdString();
}

void
DeepContactShadow::addAcceptedComponents(int /*inputNb*/,
                                         std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepContactShadow::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepContactShadow::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                                  bool* /*defaultG*/,
                                                  bool* /*defaultB*/,
                                                  bool* /*defaultA*/) const
{
    return false;
}

void
DeepContactShadow::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Proximity
    KnobDoublePtr proximity = AppManager::createKnob<KnobDouble>(this, tr("Proximity"));
    proximity->setName("proximity");
    proximity->setHintToolTip(tr("Z distance threshold for contact detection. Samples from different objects "
                                 "within this depth distance are considered in contact."));
    proximity->setAnimationEnabled(true);
    proximity->setDefaultValue(0.5);
    proximity->setMinimum(0.001);
    proximity->setDisplayMinimum(0.0);
    proximity->setDisplayMaximum(5.0);
    page->addKnob(proximity);
    _imp->proximity = proximity;

    // Intensity
    KnobDoublePtr intensity = AppManager::createKnob<KnobDouble>(this, tr("Intensity"));
    intensity->setName("intensity");
    intensity->setHintToolTip(tr("Shadow darkening strength. Higher values produce darker contact shadows."));
    intensity->setAnimationEnabled(true);
    intensity->setDefaultValue(0.5);
    intensity->setMinimum(0.0);
    intensity->setDisplayMinimum(0.0);
    intensity->setDisplayMaximum(2.0);
    page->addKnob(intensity);
    _imp->intensity = intensity;

    // Kernel Radius
    KnobIntPtr kernelRadius = AppManager::createKnob<KnobInt>(this, tr("Kernel Radius"));
    kernelRadius->setName("kernelRadius");
    kernelRadius->setHintToolTip(tr("Screen-space search radius in pixels. Larger values detect contacts "
                                    "over a wider area but are slower."));
    kernelRadius->setAnimationEnabled(true);
    kernelRadius->setDefaultValue(3);
    kernelRadius->setMinimum(1);
    kernelRadius->setMaximum(10);
    page->addKnob(kernelRadius);
    _imp->kernelRadius = kernelRadius;

    // Self-Shadow Bias
    KnobDoublePtr selfShadowBias = AppManager::createKnob<KnobDouble>(this, tr("Self-Shadow Bias"));
    selfShadowBias->setName("selfShadowBias");
    selfShadowBias->setHintToolTip(tr("Minimum depth difference to consider two samples as belonging to different "
                                      "objects. Prevents the same surface from shadowing itself."));
    selfShadowBias->setAnimationEnabled(true);
    selfShadowBias->setDefaultValue(0.01);
    selfShadowBias->setMinimum(0.0);
    selfShadowBias->setDisplayMinimum(0.0);
    selfShadowBias->setDisplayMaximum(0.5);
    page->addKnob(selfShadowBias);
    _imp->selfShadowBias = selfShadowBias;

    // Info
    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Adjust proximity and intensity to control contact shadow strength.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepContactShadow::knobChanged(KnobI* k,
                                ValueChangedReasonEnum /*reason*/,
                                ViewSpec /*view*/,
                                double /*time*/,
                                bool /*originatedFromMainThread*/)
{
    if (_imp->proximity.lock().get() == k ||
        _imp->intensity.lock().get() == k ||
        _imp->kernelRadius.lock().get() == k ||
        _imp->selfShadowBias.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepContactShadow::getRegionOfDefinition(U64 /*hash*/,
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
DeepContactShadow::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepContactShadow::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double proximityVal = _imp->proximity.lock()->getValue();
    double intensityVal = _imp->intensity.lock()->getValue();
    int kernelRadiusVal = _imp->kernelRadius.lock()->getValue();
    double selfShadowBiasVal = _imp->selfShadowBias.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

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

    // Maximum expected contacts for normalization
    // Use the kernel area as an approximate upper bound
    double maxExpectedContacts = (double)((2 * kernelRadiusVal + 1) * (2 * kernelRadiusVal + 1));

    // First pass: copy sample counts
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: compute contact shadows and write samples
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

            int pixIdx = (y - dw.y1) * w + (x - dw.x1);

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                float zS = src[zIdx];
                int contactCount = 0;

                // Check this pixel and neighbors within kernelRadius
                for (int dy = -kernelRadiusVal; dy <= kernelRadiusVal; ++dy) {
                    for (int dx = -kernelRadiusVal; dx <= kernelRadiusVal; ++dx) {
                        int nx = x + dx;
                        int ny = y + dy;

                        // Skip if outside bounds
                        if (nx < dw.x1 || nx >= dw.x2 || ny < dw.y1 || ny >= dw.y2) {
                            continue;
                        }

                        int nPixIdx = (ny - dw.y1) * w + (nx - dw.x1);
                        const std::vector<float>& neighborDepths = depthLists[nPixIdx];

                        for (size_t nd = 0; nd < neighborDepths.size(); ++nd) {
                            float zN = neighborDepths[nd];
                            double depthDiff = std::fabs((double)zN - (double)zS);

                            // Skip if within self-shadow bias (same object)
                            if (depthDiff < selfShadowBiasVal) {
                                continue;
                            }

                            // Count if within proximity distance
                            if (depthDiff < proximityVal) {
                                ++contactCount;
                            }
                        }
                    }
                }

                // Compute shadow factor
                double shadowRaw = intensityVal * (double)contactCount / maxExpectedContacts;
                if (shadowRaw < 0.0) {
                    shadowRaw = 0.0;
                }
                if (shadowRaw > 1.0) {
                    shadowRaw = 1.0;
                }
                float shadow = (float)(1.0 - shadowRaw);

                // Copy all channels first
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Output shadow as mask: R=G=B=A=shadow (like DeepAO)
                // This way it can be flattened and graded over the plate
                if (rIdx >= 0) dst[rIdx] = shadow;
                if (gIdx >= 0) dst[gIdx] = shadow;
                if (bIdx >= 0) dst[bIdx] = shadow;
                if (aIdx >= 0) dst[aIdx] = shadow;
                // Z, ZBack remain unchanged (already copied)
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Proximity: " << proximityVal
       << " | Intensity: " << intensityVal
       << " | Kernel: " << kernelRadiusVal
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

#include "moc_DeepContactShadow.cpp"
