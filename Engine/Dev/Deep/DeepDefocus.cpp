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

#include "DeepDefocus.h"

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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct DeepDefocusPrivate
{
    KnobDoubleWPtr focalDistance;
    KnobDoubleWPtr fStop;
    KnobDoubleWPtr blurScale;
    KnobIntWPtr numLayers;
    KnobDoubleWPtr maxBlur;
    KnobBoolWPtr defocusForeground;
    KnobBoolWPtr defocusBackground;

    // Focus picker
    KnobIntWPtr focusPointX;
    KnobIntWPtr focusPointY;
    KnobButtonWPtr pickFocusBtn;

    // Bokeh
    KnobIntWPtr apertureBlades; // 0 = circular, 6 = hex, 8 = octagonal
    KnobDoubleWPtr apertureRotation;

    KnobStringWPtr info;
};


DeepDefocus::DeepDefocus(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepDefocusPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepDefocus::~DeepDefocus()
{
}

std::string
DeepDefocus::getPluginDescription() const
{
    return tr("Depth-of-field effect directly on deep compositing data.\n\n"
              "Stratifies deep samples into depth layers, computes circle of confusion "
              "per layer based on distance from focal plane, applies Gaussian blur, "
              "and composites layers back-to-front.\n\n"
              "Output is a flat RGBA image (defocus destroys deep structure).\n\n"
              "Parameters:\n"
              "- Focal Distance: Z-depth of the focal plane\n"
              "- F-Stop: Lower values = shallower depth of field (more blur)\n"
              "- Blur Scale: Artistic multiplier on blur radius\n"
              "- Num Layers: More layers = better quality, slower (15-20 is good)\n"
              "- Max Blur: Maximum blur radius in pixels (clamp large CoC values)").toStdString();
}

void
DeepDefocus::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepDefocus::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepDefocus::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepDefocus::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr fd = AppManager::createKnob<KnobDouble>(this, tr("Focal Distance"));
    fd->setName("focalDistance");
    fd->setHintToolTip(tr("Z-depth of the focal plane. Samples at this depth will be sharp."));
    fd->setAnimationEnabled(true);
    fd->setDefaultValue(5.0);
    fd->setMinimum(0.001);
    fd->setDisplayMinimum(0.1);
    fd->setDisplayMaximum(100.0);
    page->addKnob(fd);
    _imp->focalDistance = fd;

    KnobDoublePtr fs = AppManager::createKnob<KnobDouble>(this, tr("F-Stop"));
    fs->setName("fStop");
    fs->setHintToolTip(tr("Aperture f-stop. Lower values = shallower depth of field (more blur). "
                           "f/1.4 = very shallow, f/16 = deep focus."));
    fs->setAnimationEnabled(true);
    fs->setDefaultValue(2.8);
    fs->setMinimum(0.5);
    fs->setMaximum(128.0);
    fs->setDisplayMinimum(0.5);
    fs->setDisplayMaximum(22.0);
    page->addKnob(fs);
    _imp->fStop = fs;

    KnobDoublePtr bs = AppManager::createKnob<KnobDouble>(this, tr("Blur Scale"));
    bs->setName("blurScale");
    bs->setHintToolTip(tr("Artistic multiplier on the blur radius. 1.0 = physically based, "
                           "higher values exaggerate the effect."));
    bs->setAnimationEnabled(true);
    bs->setDefaultValue(1.0);
    bs->setMinimum(0.0);
    bs->setDisplayMinimum(0.0);
    bs->setDisplayMaximum(10.0);
    page->addKnob(bs);
    _imp->blurScale = bs;

    KnobDoublePtr mb = AppManager::createKnob<KnobDouble>(this, tr("Max Blur"));
    mb->setName("maxBlur");
    mb->setHintToolTip(tr("Maximum blur radius in pixels. Clamps large circle of confusion values."));
    mb->setAnimationEnabled(false);
    mb->setDefaultValue(50.0);
    mb->setMinimum(1.0);
    mb->setDisplayMinimum(1.0);
    mb->setDisplayMaximum(200.0);
    page->addKnob(mb);
    _imp->maxBlur = mb;

    KnobIntPtr nl = AppManager::createKnob<KnobInt>(this, tr("Num Layers"));
    nl->setName("numLayers");
    nl->setHintToolTip(tr("Number of depth layers for stratification. More = better quality, slower. "
                           "15-20 is good for most cases."));
    nl->setAnimationEnabled(false);
    nl->setDefaultValue(15);
    nl->setMinimum(3);
    nl->setMaximum(50);
    nl->setDisplayMinimum(3);
    nl->setDisplayMaximum(30);
    page->addKnob(nl);
    _imp->numLayers = nl;

    KnobBoolPtr dfg = AppManager::createKnob<KnobBool>(this, tr("Defocus Foreground"));
    dfg->setName("defocusForeground");
    dfg->setDefaultValue(true);
    dfg->setHintToolTip(tr("Apply defocus to samples in front of the focal plane."));
    page->addKnob(dfg);
    _imp->defocusForeground = dfg;

    KnobBoolPtr dbg = AppManager::createKnob<KnobBool>(this, tr("Defocus Background"));
    dbg->setName("defocusBackground");
    dbg->setDefaultValue(true);
    dbg->setHintToolTip(tr("Apply defocus to samples behind the focal plane."));
    page->addKnob(dbg);
    _imp->defocusBackground = dbg;

    // Focus picker page
    KnobPagePtr pickPage = AppManager::createKnob<KnobPage>(this, tr("Focus Picker"));

    KnobIntPtr fpx = AppManager::createKnob<KnobInt>(this, tr("Focus Point X"));
    fpx->setName("focusPointX"); fpx->setDefaultValue(640);
    fpx->setDisplayMinimum(0); fpx->setDisplayMaximum(4096);
    fpx->setHintToolTip(tr("X pixel coordinate to sample Z depth from. Set to the pixel you want in focus."));
    pickPage->addKnob(fpx); _imp->focusPointX = fpx;

    KnobIntPtr fpy = AppManager::createKnob<KnobInt>(this, tr("Focus Point Y"));
    fpy->setName("focusPointY"); fpy->setDefaultValue(360);
    fpy->setDisplayMinimum(0); fpy->setDisplayMaximum(4096);
    fpy->setHintToolTip(tr("Y pixel coordinate to sample Z depth from."));
    pickPage->addKnob(fpy); _imp->focusPointY = fpy;

    KnobButtonPtr pickBtn = AppManager::createKnob<KnobButton>(this, tr("Set Focus From Point"));
    pickBtn->setName("pickFocus");
    pickBtn->setHintToolTip(tr("Read the Z depth at the Focus Point X/Y coordinates and set Focal Distance to that value."));
    pickPage->addKnob(pickBtn); _imp->pickFocusBtn = pickBtn;

    // Bokeh page
    KnobPagePtr bokehPage = AppManager::createKnob<KnobPage>(this, tr("Bokeh"));

    KnobIntPtr blades = AppManager::createKnob<KnobInt>(this, tr("Aperture Blades"));
    blades->setName("apertureBlades"); blades->setDefaultValue(0);
    blades->setMinimum(0); blades->setMaximum(12);
    blades->setDisplayMinimum(0); blades->setDisplayMaximum(12);
    blades->setHintToolTip(tr("Number of aperture blades. 0 = circular (disk), 6 = hexagonal, 8 = octagonal."));
    bokehPage->addKnob(blades); _imp->apertureBlades = blades;

    KnobDoublePtr rot = AppManager::createKnob<KnobDouble>(this, tr("Aperture Rotation"));
    rot->setName("apertureRotation"); rot->setDefaultValue(0.0);
    rot->setDisplayMinimum(0.0); rot->setDisplayMaximum(360.0);
    rot->setHintToolTip(tr("Rotation of the aperture shape in degrees."));
    bokehPage->addKnob(rot); _imp->apertureRotation = rot;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect a deep node to the input.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepDefocus::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/, double time, bool /*originatedFromMainThread*/)
{
    if (_imp->pickFocusBtn.lock().get() == k) {
        // Read Z depth at the focus point coordinates from the deep image
        EffectInstancePtr deepInput = getInput(0);
        DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
        if (!srcDeep) {
            setPersistentMessage(eMessageTypeError, "No deep image connected.");
            return true;
        }

        int fpx = _imp->focusPointX.lock()->getValue();
        int fpy = _imp->focusPointY.lock()->getValue();

        const RectI& dw = srcDeep->getDataWindow();
        int nChannels = srcDeep->getNumChannels();
        int zIdx = srcDeep->findChannelIndex("Z");
        if (zIdx < 0) {
            setPersistentMessage(eMessageTypeError, "No Z channel in deep image.");
            return true;
        }

        // Clamp to data window
        fpx = std::max(dw.x1, std::min(dw.x2 - 1, fpx));
        fpy = std::max(dw.y1, std::min(dw.y2 - 1, fpy));

        int nSamples = srcDeep->getSampleCount(fpx, fpy);
        if (nSamples == 0) {
            setPersistentMessage(eMessageTypeWarning, "No deep samples at focus point.");
            return true;
        }

        // Get the frontmost Z value
        const float* data = srcDeep->getSampleData(fpx, fpy);
        float frontZ = data[zIdx];
        for (int s = 1; s < nSamples; ++s) {
            float z = data[s * nChannels + zIdx];
            if (z < frontZ) frontZ = z;
        }

        _imp->focalDistance.lock()->setValue((double)frontZ);
        clearPersistentMessage(false);
        return true;
    }
    return false;
}

StatusEnum
DeepDefocus::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                   ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

// Check if a point is inside a regular polygon (for bokeh kernel)
static bool
isInsidePolygon(float x, float y, int numSides, float radius, float rotationDeg)
{
    if (numSides <= 0) {
        // Circle: just check distance
        return (x * x + y * y) <= (radius * radius);
    }

    float rotRad = rotationDeg * (float)M_PI / 180.0f;
    // Rotate the point by -rotation to align polygon
    float cosR = cosf(-rotRad);
    float sinR = sinf(-rotRad);
    float rx = x * cosR - y * sinR;
    float ry = x * sinR + y * cosR;

    // Check if point is inside regular polygon
    float angle = atan2f(ry, rx);
    if (angle < 0) angle += 2.0f * (float)M_PI;

    float segmentAngle = 2.0f * (float)M_PI / numSides;
    float halfAngle = segmentAngle * 0.5f;

    // Distance from center to edge at this angle
    float localAngle = fmodf(angle, segmentAngle);
    if (localAngle > halfAngle) localAngle = segmentAngle - localAngle;

    float edgeDist = radius * cosf(halfAngle) / cosf(localAngle);
    float pointDist = sqrtf(rx * rx + ry * ry);

    return pointDist <= edgeDist;
}

// Bokeh blur: convolve with a shaped kernel (disk or polygon)
static void
bokehBlurRGBA(const std::vector<float>& src, std::vector<float>& dst,
              int width, int height, float radius, int blades, float rotationDeg)
{
    if (radius < 0.5f) {
        dst = src;
        return;
    }

    int kernelRadius = std::min((int)std::ceil(radius), 100);
    if (kernelRadius < 1) kernelRadius = 1;

    // Build 2D kernel
    int kernelSize = kernelRadius * 2 + 1;
    std::vector<float> kernel(kernelSize * kernelSize, 0.0f);
    float sum = 0;

    for (int ky = -kernelRadius; ky <= kernelRadius; ++ky) {
        for (int kx = -kernelRadius; kx <= kernelRadius; ++kx) {
            if (isInsidePolygon((float)kx, (float)ky, blades, radius, rotationDeg)) {
                kernel[(ky + kernelRadius) * kernelSize + (kx + kernelRadius)] = 1.0f;
                sum += 1.0f;
            }
        }
    }

    // Normalize
    if (sum > 0) {
        for (size_t i = 0; i < kernel.size(); ++i) kernel[i] /= sum;
    }

    // Convolve
    dst.resize(width * height * 4, 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int ky = -kernelRadius; ky <= kernelRadius; ++ky) {
                int sy = std::max(0, std::min(height - 1, y + ky));
                for (int kx = -kernelRadius; kx <= kernelRadius; ++kx) {
                    int sx = std::max(0, std::min(width - 1, x + kx));
                    float w = kernel[(ky + kernelRadius) * kernelSize + (kx + kernelRadius)];
                    if (w <= 0) continue;
                    int srcIdx = (sy * width + sx) * 4;
                    r += src[srcIdx + 0] * w;
                    g += src[srcIdx + 1] * w;
                    b += src[srcIdx + 2] * w;
                    a += src[srcIdx + 3] * w;
                }
            }
            int dstIdx = (y * width + x) * 4;
            dst[dstIdx + 0] = r;
            dst[dstIdx + 1] = g;
            dst[dstIdx + 2] = b;
            dst[dstIdx + 3] = a;
        }
    }
}

// Separable Gaussian blur on a float RGBA buffer (fast fallback)
static void
gaussianBlurRGBA(const std::vector<float>& src, std::vector<float>& dst,
                 int width, int height, float radius)
{
    if (radius < 0.5f) {
        dst = src;
        return;
    }

    int kernelRadius = std::min((int)std::ceil(radius * 2.5f), 100);
    if (kernelRadius < 1) kernelRadius = 1;

    // Build 1D Gaussian kernel
    std::vector<float> kernel(kernelRadius * 2 + 1);
    float sigma = radius;
    float sum = 0;
    for (int i = -kernelRadius; i <= kernelRadius; ++i) {
        float val = std::exp(-(float)(i * i) / (2.0f * sigma * sigma));
        kernel[i + kernelRadius] = val;
        sum += val;
    }
    for (int i = 0; i < (int)kernel.size(); ++i) {
        kernel[i] /= sum;
    }

    // Horizontal pass
    std::vector<float> temp(width * height * 4, 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int k = -kernelRadius; k <= kernelRadius; ++k) {
                int sx = std::max(0, std::min(width - 1, x + k));
                int srcIdx = (y * width + sx) * 4;
                float w = kernel[k + kernelRadius];
                r += src[srcIdx + 0] * w;
                g += src[srcIdx + 1] * w;
                b += src[srcIdx + 2] * w;
                a += src[srcIdx + 3] * w;
            }
            int dstIdx = (y * width + x) * 4;
            temp[dstIdx + 0] = r;
            temp[dstIdx + 1] = g;
            temp[dstIdx + 2] = b;
            temp[dstIdx + 3] = a;
        }
    }

    // Vertical pass
    dst.resize(width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int k = -kernelRadius; k <= kernelRadius; ++k) {
                int sy = std::max(0, std::min(height - 1, y + k));
                int srcIdx = (sy * width + x) * 4;
                float w = kernel[k + kernelRadius];
                r += temp[srcIdx + 0] * w;
                g += temp[srcIdx + 1] * w;
                b += temp[srcIdx + 2] * w;
                a += temp[srcIdx + 3] * w;
            }
            int dstIdx = (y * width + x) * 4;
            dst[dstIdx + 0] = r;
            dst[dstIdx + 1] = g;
            dst[dstIdx + 2] = b;
            dst[dstIdx + 3] = a;
        }
    }
}

StatusEnum
DeepDefocus::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    double focalDist = _imp->focalDistance.lock()->getValue();
    double fStopVal = _imp->fStop.lock()->getValue();
    double blurScaleVal = _imp->blurScale.lock()->getValue();
    double maxBlurVal = _imp->maxBlur.lock()->getValue();
    int numLayersVal = _imp->numLayers.lock()->getValue();
    bool defocusFG = _imp->defocusForeground.lock()->getValue();
    bool defocusBG = _imp->defocusBackground.lock()->getValue();
    int bladesVal = _imp->apertureBlades.lock()->getValue();
    float rotationVal = (float)_imp->apertureRotation.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int w = dw.width();
    int h = dw.height();
    int nChannels = srcDeep->getNumChannels();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    if (zIdx < 0 || aIdx < 0) return eStatusFailed;

    // Find Z range
    float minZ = std::numeric_limits<float>::max();
    float maxZ = -std::numeric_limits<float>::max();

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;
            const float* data = srcDeep->getSampleData(x, y);
            for (int s = 0; s < nSamples; ++s) {
                float z = data[s * nChannels + zIdx];
                if (z < minZ) minZ = z;
                if (z > maxZ) maxZ = z;
            }
        }
    }

    if (minZ >= maxZ) {
        // All at same depth — just flatten
        assert(!args.outputPlanes.empty());
        ImagePtr outImg = args.outputPlanes.front().second;
        if (outImg) srcDeep->flattenToImage(outImg.get());
        return eStatusOK;
    }

    // Create depth layer boundaries
    std::vector<float> layerDepths(numLayersVal);
    for (int i = 0; i < numLayersVal; ++i) {
        layerDepths[i] = minZ + (maxZ - minZ) * ((float)i / (numLayersVal - 1));
    }

    // Flatten each layer: for each pixel, accumulate samples that fall in each layer's depth range
    std::vector<std::vector<float>> layerImages(numLayersVal);
    for (int i = 0; i < numLayersVal; ++i) {
        layerImages[i].resize(w * h * 4, 0.0f);
    }

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* data = srcDeep->getSampleData(x, y);
            int px = x - dw.x1;
            int py = y - dw.y1;
            int pixIdx = (py * w + px) * 4;

            for (int s = 0; s < nSamples; ++s) {
                const float* sample = data + s * nChannels;
                float z = sample[zIdx];
                float alpha = sample[aIdx];
                if (alpha < 0.0001f) continue;

                float r = (rIdx >= 0) ? sample[rIdx] : 0;
                float g = (gIdx >= 0) ? sample[gIdx] : 0;
                float b = (bIdx >= 0) ? sample[bIdx] : 0;

                // Soft layer blending: distribute sample across two nearest layers
                // Find which two layers this sample falls between
                int lowerLayer = 0;
                for (int i = 0; i < numLayersVal - 1; ++i) {
                    if (z >= layerDepths[i] && z <= layerDepths[i + 1]) {
                        lowerLayer = i;
                        break;
                    }
                    if (z > layerDepths[numLayersVal - 1]) {
                        lowerLayer = numLayersVal - 2;
                    }
                }
                int upperLayer = lowerLayer + 1;
                if (upperLayer >= numLayersVal) upperLayer = numLayersVal - 1;

                // Compute interpolation weight
                float layerSpan = layerDepths[upperLayer] - layerDepths[lowerLayer];
                float weightUpper = 0.5f;
                if (layerSpan > 0.0001f) {
                    weightUpper = (z - layerDepths[lowerLayer]) / layerSpan;
                    weightUpper = std::max(0.0f, std::min(1.0f, weightUpper));
                }
                float weightLower = 1.0f - weightUpper;

                // Add weighted sample to both layers
                for (int li = 0; li < 2; ++li) {
                    int layerIdx = (li == 0) ? lowerLayer : upperLayer;
                    float weight = (li == 0) ? weightLower : weightUpper;

                    if (weight < 0.001f) continue;

                    float wR = r * weight;
                    float wG = g * weight;
                    float wB = b * weight;
                    float wA = alpha * weight;

                    // Additive blending into layer (weighted)
                    float dstA = layerImages[layerIdx][pixIdx + 3];

                    layerImages[layerIdx][pixIdx + 0] += wR * (1.0f - dstA);
                    layerImages[layerIdx][pixIdx + 1] += wG * (1.0f - dstA);
                    layerImages[layerIdx][pixIdx + 2] += wB * (1.0f - dstA);
                    layerImages[layerIdx][pixIdx + 3] += wA * (1.0f - dstA);
                }
            }
        }
    }

    // Blur each layer based on its CoC
    std::vector<std::vector<float>> blurredLayers(numLayersVal);

    for (int i = 0; i < numLayersVal; ++i) {
        float layerZ = layerDepths[i];
        float distFromFocal = std::abs(layerZ - (float)focalDist);

        // Check foreground/background toggle
        bool isForeground = (layerZ < focalDist);
        if (isForeground && !defocusFG) {
            blurredLayers[i] = layerImages[i];
            continue;
        }
        if (!isForeground && !defocusBG) {
            blurredLayers[i] = layerImages[i];
            continue;
        }

        // CoC = distance * scale / f-stop
        float coc = distFromFocal * (float)blurScaleVal / std::max(0.5f, (float)fStopVal);
        coc = std::min(coc, (float)maxBlurVal);

        if (bladesVal > 0) {
            // Shaped bokeh blur (disk or polygon)
            bokehBlurRGBA(layerImages[i], blurredLayers[i], w, h, coc, bladesVal, rotationVal);
        } else {
            // Fast separable Gaussian (circular approximation)
            gaussianBlurRGBA(layerImages[i], blurredLayers[i], w, h, coc);
        }
    }

    // Composite layers back-to-front
    std::vector<float> result(w * h * 4, 0.0f);

    for (int i = numLayersVal - 1; i >= 0; --i) {
        const std::vector<float>& layer = blurredLayers[i];
        for (int p = 0; p < w * h; ++p) {
            int idx = p * 4;
            float srcR = layer[idx + 0];
            float srcG = layer[idx + 1];
            float srcB = layer[idx + 2];
            float srcA = layer[idx + 3];

            float dstA = result[idx + 3];

            // "Over" compositing: dst = src + dst * (1 - srcA)
            result[idx + 0] = srcR + result[idx + 0] * (1.0f - srcA);
            result[idx + 1] = srcG + result[idx + 1] * (1.0f - srcA);
            result[idx + 2] = srcB + result[idx + 2] * (1.0f - srcA);
            result[idx + 3] = srcA + dstA * (1.0f - srcA);
        }
    }

    // Write to output image with Y-flip (deep is top-down, Natron is bottom-up)
    assert(!args.outputPlanes.empty());
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    RectI outBounds = outImg->getBounds();
    int outW = outBounds.width();
    int outH = outBounds.height();

    Image::WriteAccess wa(outImg.get());
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        // Y-flip: Natron bottom-up, deep top-down
        float normY = (float)(y - outBounds.y1) / std::max(1, outH - 1);
        int srcRow = (int)((1.0f - normY) * (h - 1) + 0.5f);
        srcRow = std::max(0, std::min(h - 1, srcRow));

        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            float normX = (float)(x - outBounds.x1) / std::max(1, outW - 1);
            int srcCol = (int)(normX * (w - 1) + 0.5f);
            srcCol = std::max(0, std::min(w - 1, srcCol));

            int srcIdx = (srcRow * w + srcCol) * 4;
            dst[0] = result[srcIdx + 0];
            dst[1] = result[srcIdx + 1];
            dst[2] = result[srcIdx + 2];
            dst[3] = result[srcIdx + 3];
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Layers: " << numLayersVal
       << " | Z range: " << minZ << " - " << maxZ
       << " | Focal: " << focalDist
       << " | Max CoC: " << (maxZ - (float)focalDist) * (float)blurScaleVal / std::max(0.5f, (float)fStopVal) << "px";
    _imp->info.lock()->setValue(ss.str());

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepDefocus.cpp"
