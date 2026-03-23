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

#include "DeepGodRays.h"

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


struct DeepGodRaysPrivate
{
    KnobDoubleWPtr lightPosX;
    KnobDoubleWPtr lightPosY;
    KnobDoubleWPtr intensity;
    KnobIntWPtr numSteps;
    KnobDoubleWPtr density;
    KnobDoubleWPtr colorR;
    KnobDoubleWPtr colorG;
    KnobDoubleWPtr colorB;
    KnobDoubleWPtr decay;
    KnobDoubleWPtr exposure;
    KnobStringWPtr info;
};


DeepGodRays::DeepGodRays(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepGodRaysPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepGodRays::~DeepGodRays()
{
}

std::string
DeepGodRays::getPluginDescription() const
{
    return tr("Compute volumetric light shafts from deep image transmittance. "
              "Ray-marches from each pixel toward a light source, accumulating light "
              "contribution where the deep data is transparent. "
              "Output is a flat RGBA image to be added over the flattened result.").toStdString();
}

void
DeepGodRays::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepGodRays::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepGodRays::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepGodRays::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Light position (NDC)
    KnobDoublePtr lpx = AppManager::createKnob<KnobDouble>(this, tr("Light Pos X"));
    lpx->setName("lightPosX");
    lpx->setHintToolTip(tr("X position of the light source in normalized device coordinates (0=left, 1=right)."));
    lpx->setAnimationEnabled(true);
    lpx->setDefaultValue(0.5);
    lpx->setMinimum(0.0);
    lpx->setMaximum(1.0);
    lpx->setDisplayMinimum(0.0);
    lpx->setDisplayMaximum(1.0);
    page->addKnob(lpx);
    _imp->lightPosX = lpx;

    KnobDoublePtr lpy = AppManager::createKnob<KnobDouble>(this, tr("Light Pos Y"));
    lpy->setName("lightPosY");
    lpy->setHintToolTip(tr("Y position of the light source in normalized device coordinates (0=bottom, 1=top)."));
    lpy->setAnimationEnabled(true);
    lpy->setDefaultValue(0.5);
    lpy->setMinimum(0.0);
    lpy->setMaximum(1.0);
    lpy->setDisplayMinimum(0.0);
    lpy->setDisplayMaximum(1.0);
    page->addKnob(lpy);
    _imp->lightPosY = lpy;

    // Intensity
    KnobDoublePtr intens = AppManager::createKnob<KnobDouble>(this, tr("Intensity"));
    intens->setName("intensity");
    intens->setHintToolTip(tr("Overall brightness of the god rays."));
    intens->setAnimationEnabled(true);
    intens->setDefaultValue(1.0);
    intens->setMinimum(0.0);
    intens->setDisplayMinimum(0.0);
    intens->setDisplayMaximum(10.0);
    page->addKnob(intens);
    _imp->intensity = intens;

    // Num steps
    KnobIntPtr steps = AppManager::createKnob<KnobInt>(this, tr("Num Steps"));
    steps->setName("numSteps");
    steps->setHintToolTip(tr("Number of ray march steps. More steps = smoother rays, slower render."));
    steps->setAnimationEnabled(false);
    steps->setDefaultValue(64);
    steps->setMinimum(8);
    steps->setMaximum(256);
    steps->setDisplayMinimum(8);
    steps->setDisplayMaximum(256);
    page->addKnob(steps);
    _imp->numSteps = steps;

    // Density
    KnobDoublePtr dens = AppManager::createKnob<KnobDouble>(this, tr("Density"));
    dens->setName("density");
    dens->setHintToolTip(tr("Density of the volumetric medium. Higher values produce brighter rays."));
    dens->setAnimationEnabled(true);
    dens->setDefaultValue(1.0);
    dens->setMinimum(0.0);
    dens->setDisplayMinimum(0.0);
    dens->setDisplayMaximum(10.0);
    page->addKnob(dens);
    _imp->density = dens;

    // Light color
    KnobDoublePtr cr = AppManager::createKnob<KnobDouble>(this, tr("Color Red"));
    cr->setName("colorR");
    cr->setHintToolTip(tr("Red component of the light color."));
    cr->setAnimationEnabled(true);
    cr->setDefaultValue(1.0);
    cr->setMinimum(0.0);
    cr->setDisplayMinimum(0.0);
    cr->setDisplayMaximum(1.0);
    page->addKnob(cr);
    _imp->colorR = cr;

    KnobDoublePtr cg = AppManager::createKnob<KnobDouble>(this, tr("Color Green"));
    cg->setName("colorG");
    cg->setHintToolTip(tr("Green component of the light color."));
    cg->setAnimationEnabled(true);
    cg->setDefaultValue(0.9);
    cg->setMinimum(0.0);
    cg->setDisplayMinimum(0.0);
    cg->setDisplayMaximum(1.0);
    page->addKnob(cg);
    _imp->colorG = cg;

    KnobDoublePtr cb = AppManager::createKnob<KnobDouble>(this, tr("Color Blue"));
    cb->setName("colorB");
    cb->setHintToolTip(tr("Blue component of the light color."));
    cb->setAnimationEnabled(true);
    cb->setDefaultValue(0.7);
    cb->setMinimum(0.0);
    cb->setDisplayMinimum(0.0);
    cb->setDisplayMaximum(1.0);
    page->addKnob(cb);
    _imp->colorB = cb;

    // Decay
    KnobDoublePtr dec = AppManager::createKnob<KnobDouble>(this, tr("Decay"));
    dec->setName("decay");
    dec->setHintToolTip(tr("Illumination decay per step. Lower values cause rays to fade faster."));
    dec->setAnimationEnabled(true);
    dec->setDefaultValue(0.97);
    dec->setMinimum(0.5);
    dec->setMaximum(1.0);
    dec->setDisplayMinimum(0.5);
    dec->setDisplayMaximum(1.0);
    page->addKnob(dec);
    _imp->decay = dec;

    // Exposure
    KnobDoublePtr exp = AppManager::createKnob<KnobDouble>(this, tr("Exposure"));
    exp->setName("exposure");
    exp->setHintToolTip(tr("Final exposure multiplier applied to the output."));
    exp->setAnimationEnabled(true);
    exp->setDefaultValue(1.0);
    exp->setMinimum(0.0);
    exp->setDisplayMinimum(0.0);
    exp->setDisplayMaximum(10.0);
    page->addKnob(exp);
    _imp->exposure = exp;

    // Info
    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect a deep node to compute volumetric light shafts.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepGodRays::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    if (_imp->lightPosX.lock().get() == k ||
        _imp->lightPosY.lock().get() == k ||
        _imp->intensity.lock().get() == k ||
        _imp->numSteps.lock().get() == k ||
        _imp->density.lock().get() == k ||
        _imp->decay.lock().get() == k ||
        _imp->exposure.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepGodRays::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                   ViewIdx /*view*/, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), 0, RenderScale(), ViewIdx(0), rod, &isProjectFormat);
}

StatusEnum
DeepGodRays::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    double lightPosXVal = _imp->lightPosX.lock()->getValue();
    double lightPosYVal = _imp->lightPosY.lock()->getValue();
    double intensityVal = _imp->intensity.lock()->getValue();
    int numStepsVal = _imp->numSteps.lock()->getValue();
    double densityVal = _imp->density.lock()->getValue();
    double colorRVal = _imp->colorR.lock()->getValue();
    double colorGVal = _imp->colorG.lock()->getValue();
    double colorBVal = _imp->colorB.lock()->getValue();
    double decayVal = _imp->decay.lock()->getValue();
    double exposureVal = _imp->exposure.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int w = dw.width();
    int h = dw.height();
    int nChannels = srcDeep->getNumChannels();

    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    if (aIdx < 0) return eStatusFailed;

    // Pre-compute transmittance map: for each pixel, flatten alpha front-to-back
    // transmittance = product of (1 - sampleAlpha)
    std::vector<float> transmittanceMap(w * h, 1.0f);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* data = srcDeep->getSampleData(x, y);
            float transmittance = 1.0f;
            for (int s = 0; s < nSamples; ++s) {
                float alpha = data[s * nChannels + aIdx];
                transmittance *= (1.0f - std::min(1.0f, std::max(0.0f, alpha)));
            }

            int px = x - dw.x1;
            int py = y - dw.y1;
            transmittanceMap[py * w + px] = transmittance;
        }
    }

    // Light position in pixel coordinates
    float lightPixX = (float)(lightPosXVal * (w - 1));
    float lightPixY = (float)(lightPosYVal * (h - 1));

    // Ray march: for each pixel, march toward light source
    std::vector<float> resultBuf(w * h * 4, 0.0f);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Direction toward light
            float dx = lightPixX - (float)x;
            float dy = lightPixY - (float)y;

            // Step size
            float stepX = dx / (float)numStepsVal;
            float stepY = dy / (float)numStepsVal;

            float illumination = 1.0f;
            float accumR = 0.0f, accumG = 0.0f, accumB = 0.0f;

            float sampleX = (float)x;
            float sampleY = (float)y;

            for (int step = 0; step < numStepsVal; ++step) {
                sampleX += stepX;
                sampleY += stepY;

                // Bilinear sample the transmittance map
                float fx = std::max(0.0f, std::min((float)(w - 1), sampleX));
                float fy = std::max(0.0f, std::min((float)(h - 1), sampleY));

                int ix0 = (int)fx;
                int iy0 = (int)fy;
                int ix1 = std::min(ix0 + 1, w - 1);
                int iy1 = std::min(iy0 + 1, h - 1);

                float fracX = fx - (float)ix0;
                float fracY = fy - (float)iy0;

                float t00 = transmittanceMap[iy0 * w + ix0];
                float t10 = transmittanceMap[iy0 * w + ix1];
                float t01 = transmittanceMap[iy1 * w + ix0];
                float t11 = transmittanceMap[iy1 * w + ix1];

                float transmittance = t00 * (1.0f - fracX) * (1.0f - fracY)
                                    + t10 * fracX * (1.0f - fracY)
                                    + t01 * (1.0f - fracX) * fracY
                                    + t11 * fracX * fracY;

                float contribution = transmittance * illumination * (float)densityVal / (float)numStepsVal;

                accumR += contribution * (float)colorRVal;
                accumG += contribution * (float)colorGVal;
                accumB += contribution * (float)colorBVal;

                illumination *= (float)decayVal;
            }

            // Apply intensity and exposure
            accumR *= (float)(intensityVal * exposureVal);
            accumG *= (float)(intensityVal * exposureVal);
            accumB *= (float)(intensityVal * exposureVal);

            int pixIdx = (y * w + x) * 4;
            resultBuf[pixIdx + 0] = accumR;
            resultBuf[pixIdx + 1] = accumG;
            resultBuf[pixIdx + 2] = accumB;
            resultBuf[pixIdx + 3] = std::max(accumR, std::max(accumG, accumB));
        }
    }

    // Write to output Image (Y-flip pattern from DeepDefocus)
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
            dst[0] = resultBuf[srcIdx + 0];
            dst[1] = resultBuf[srcIdx + 1];
            dst[2] = resultBuf[srcIdx + 2];
            dst[3] = resultBuf[srcIdx + 3];
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Steps: " << numStepsVal
       << " | Light: (" << lightPosXVal << ", " << lightPosYVal << ")"
       << " | Image: " << w << "x" << h
       << " | Density: " << densityVal
       << " | Decay: " << decayVal;
    _imp->info.lock()->setValue(ss.str());

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepGodRays.cpp"
