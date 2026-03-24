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

#include "DeepNormalMatte.h"

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


struct DeepNormalMattePrivate
{
    KnobDoubleWPtr dirX;
    KnobDoubleWPtr dirY;
    KnobDoubleWPtr dirZ;
    KnobDoubleWPtr tolerance;
    KnobDoubleWPtr falloff;
    KnobDoubleWPtr normalScale;
    KnobBoolWPtr invert;
    KnobStringWPtr info;
};


DeepNormalMatte::DeepNormalMatte(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepNormalMattePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepNormalMatte::~DeepNormalMatte()
{
}

std::string
DeepNormalMatte::getPluginDescription() const
{
    return tr("Generate a matte based on estimated surface normal direction from deep sample depth gradients. "
              "Useful for isolating upward-facing surfaces (for snow, rain, dust) or side-facing surfaces.").toStdString();
}

void
DeepNormalMatte::addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepNormalMatte::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepNormalMatte::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                                bool* /*defaultG*/,
                                                bool* /*defaultB*/,
                                                bool* /*defaultA*/) const
{
    return false;
}

void
DeepNormalMatte::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr dirX = AppManager::createKnob<KnobDouble>(this, tr("Direction X"));
    dirX->setName("dirX");
    dirX->setHintToolTip(tr("X component of the target normal direction."));
    dirX->setAnimationEnabled(true);
    dirX->setDefaultValue(0.0);
    dirX->setDisplayMinimum(-1.0);
    dirX->setDisplayMaximum(1.0);
    page->addKnob(dirX);
    _imp->dirX = dirX;

    KnobDoublePtr dirY = AppManager::createKnob<KnobDouble>(this, tr("Direction Y"));
    dirY->setName("dirY");
    dirY->setHintToolTip(tr("Y component of the target normal direction. Default 1.0 = upward-facing."));
    dirY->setAnimationEnabled(true);
    dirY->setDefaultValue(1.0);
    dirY->setDisplayMinimum(-1.0);
    dirY->setDisplayMaximum(1.0);
    page->addKnob(dirY);
    _imp->dirY = dirY;

    KnobDoublePtr dirZ = AppManager::createKnob<KnobDouble>(this, tr("Direction Z"));
    dirZ->setName("dirZ");
    dirZ->setHintToolTip(tr("Z component of the target normal direction."));
    dirZ->setAnimationEnabled(true);
    dirZ->setDefaultValue(0.0);
    dirZ->setDisplayMinimum(-1.0);
    dirZ->setDisplayMaximum(1.0);
    page->addKnob(dirZ);
    _imp->dirZ = dirZ;

    KnobDoublePtr tolerance = AppManager::createKnob<KnobDouble>(this, tr("Tolerance"));
    tolerance->setName("tolerance");
    tolerance->setHintToolTip(tr("How closely the surface normal must match the target direction. "
                                 "1.0 = exact match only, 0.0 = all normals pass."));
    tolerance->setAnimationEnabled(true);
    tolerance->setDefaultValue(0.5);
    tolerance->setMinimum(0.0);
    tolerance->setMaximum(1.0);
    tolerance->setDisplayMinimum(0.0);
    tolerance->setDisplayMaximum(1.0);
    page->addKnob(tolerance);
    _imp->tolerance = tolerance;

    KnobDoublePtr falloff = AppManager::createKnob<KnobDouble>(this, tr("Falloff"));
    falloff->setName("falloff");
    falloff->setHintToolTip(tr("Softness of the matte transition at the tolerance boundary."));
    falloff->setAnimationEnabled(true);
    falloff->setDefaultValue(0.2);
    falloff->setMinimum(0.0);
    falloff->setMaximum(1.0);
    falloff->setDisplayMinimum(0.0);
    falloff->setDisplayMaximum(1.0);
    page->addKnob(falloff);
    _imp->falloff = falloff;

    KnobDoublePtr normalScale = AppManager::createKnob<KnobDouble>(this, tr("Normal Scale"));
    normalScale->setName("normalScale");
    normalScale->setHintToolTip(tr("Scale factor for depth-to-normal conversion. Larger values produce "
                                   "more pronounced normals from subtle depth variations."));
    normalScale->setAnimationEnabled(true);
    normalScale->setDefaultValue(1.0);
    normalScale->setMinimum(0.01);
    normalScale->setDisplayMinimum(0.01);
    normalScale->setDisplayMaximum(100.0);
    page->addKnob(normalScale);
    _imp->normalScale = normalScale;

    KnobBoolPtr invert = AppManager::createKnob<KnobBool>(this, tr("Invert"));
    invert->setName("invert");
    invert->setHintToolTip(tr("Invert the matte so that non-matching normals are kept instead."));
    invert->setAnimationEnabled(false);
    invert->setDefaultValue(false);
    page->addKnob(invert);
    _imp->invert = invert;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set target normal direction to generate a matte from depth gradients.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepNormalMatte::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if (_imp->dirX.lock().get() == k ||
        _imp->dirY.lock().get() == k ||
        _imp->dirZ.lock().get() == k ||
        _imp->tolerance.lock().get() == k ||
        _imp->falloff.lock().get() == k ||
        _imp->normalScale.lock().get() == k ||
        _imp->invert.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepNormalMatte::getRegionOfDefinition(U64 /*hash*/,
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
DeepNormalMatte::getDeepImage() const
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
DeepNormalMatte::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double dirXVal = _imp->dirX.lock()->getValue();
    double dirYVal = _imp->dirY.lock()->getValue();
    double dirZVal = _imp->dirZ.lock()->getValue();
    double toleranceVal = _imp->tolerance.lock()->getValue();
    double falloffVal = _imp->falloff.lock()->getValue();
    double normalScaleVal = _imp->normalScale.lock()->getValue();
    bool invertVal = _imp->invert.lock()->getValue();

    // Normalize target direction
    double dirLen = std::sqrt(dirXVal * dirXVal + dirYVal * dirYVal + dirZVal * dirZVal);
    if (dirLen > 0.0) {
        dirXVal /= dirLen;
        dirYVal /= dirLen;
        dirZVal /= dirLen;
    }

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");

    if (zIdx < 0 || aIdx < 0) {
        // No Z or A channel — pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // Output: same sample counts, but modulated colors/alpha
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

                // Estimate surface normal from depth gradients
                float zLeft   = getClosestZ(srcDeep.get(), xl, y, zIdx, nChannels, zS);
                float zRight  = getClosestZ(srcDeep.get(), xr, y, zIdx, nChannels, zS);
                float zBottom = getClosestZ(srcDeep.get(), x, yb, zIdx, nChannels, zS);
                float zTop    = getClosestZ(srcDeep.get(), x, yt, zIdx, nChannels, zS);

                double dzdx = (double)(zRight - zLeft) / 2.0 * normalScaleVal;
                double dzdy = (double)(zTop - zBottom) / 2.0 * normalScaleVal;

                // Normal = normalize(-dzdx, -dzdy, 1.0)
                double nx = -dzdx;
                double ny = -dzdy;
                double nz = 1.0;
                double nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (nLen > 0.0) {
                    nx /= nLen;
                    ny /= nLen;
                    nz /= nLen;
                }

                // Dot product with target direction
                double dot = nx * dirXVal + ny * dirYVal + nz * dirZVal;
                // dot ranges from -1 to 1, remap: 1 = perfect match, -1 = opposite

                // Compute matte using tolerance and falloff
                // tolerance defines the threshold: dot must be >= (1 - tolerance) to get matte=1
                float threshold = (float)(1.0 - toleranceVal);
                float edgeLow = threshold - (float)falloffVal;
                float matte = smoothstep(edgeLow, threshold, (float)dot);

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
    ss << "Dir: (" << dirXVal << ", " << dirYVal << ", " << dirZVal << ")"
       << " | Tolerance: " << toleranceVal
       << " | Falloff: " << falloffVal
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

#include "moc_DeepNormalMatte.cpp"
