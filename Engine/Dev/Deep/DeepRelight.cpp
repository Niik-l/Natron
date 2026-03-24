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

#include "DeepRelight.h"

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


struct DeepRelightPrivate
{
    KnobChoiceWPtr lightType;
    KnobDoubleWPtr lightPosX;
    KnobDoubleWPtr lightPosY;
    KnobDoubleWPtr lightPosZ;
    KnobDoubleWPtr lightDirX;
    KnobDoubleWPtr lightDirY;
    KnobDoubleWPtr lightDirZ;
    KnobDoubleWPtr lightColorR;
    KnobDoubleWPtr lightColorG;
    KnobDoubleWPtr lightColorB;
    KnobDoubleWPtr intensity;
    KnobDoubleWPtr ambient;
    KnobDoubleWPtr diffuseStrength;
    KnobDoubleWPtr normalScale;
    KnobChoiceWPtr mixMode;
    KnobStringWPtr info;
};


DeepRelight::DeepRelight(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepRelightPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepRelight::~DeepRelight()
{
}

std::string
DeepRelight::getPluginDescription() const
{
    return tr("Add point or directional lighting to a deep image using per-sample depth information. "
              "Normals are estimated from depth gradients in neighboring pixels. "
              "This enables relighting a rendered scene in compositing without re-rendering.").toStdString();
}

void
DeepRelight::addAcceptedComponents(int /*inputNb*/,
                                   std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepRelight::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepRelight::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                            bool* /*defaultG*/,
                                            bool* /*defaultB*/,
                                            bool* /*defaultA*/) const
{
    return false;
}

void
DeepRelight::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Light Type
    KnobChoicePtr lightType = AppManager::createKnob<KnobChoice>(this, tr("Light Type"));
    lightType->setName("lightType");
    lightType->setHintToolTip(tr("Type of light source. Point light radiates from a position in space. "
                                 "Directional light has parallel rays from a given direction."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Point", "Point", ""));
        entries.push_back(ChoiceOption("Directional", "Directional", ""));
        lightType->populateChoices(entries);
    }
    lightType->setDefaultValue(0);
    page->addKnob(lightType);
    _imp->lightType = lightType;

    // Light Position (for point light)
    KnobDoublePtr lightPosX = AppManager::createKnob<KnobDouble>(this, tr("Light Pos X"));
    lightPosX->setName("lightPosX");
    lightPosX->setHintToolTip(tr("X position of the point light in pixel coordinates."));
    lightPosX->setAnimationEnabled(true);
    lightPosX->setDefaultValue(960.0);
    page->addKnob(lightPosX);
    _imp->lightPosX = lightPosX;

    KnobDoublePtr lightPosY = AppManager::createKnob<KnobDouble>(this, tr("Light Pos Y"));
    lightPosY->setName("lightPosY");
    lightPosY->setHintToolTip(tr("Y position of the point light in pixel coordinates."));
    lightPosY->setAnimationEnabled(true);
    lightPosY->setDefaultValue(540.0);
    page->addKnob(lightPosY);
    _imp->lightPosY = lightPosY;

    KnobDoublePtr lightPosZ = AppManager::createKnob<KnobDouble>(this, tr("Light Pos Z"));
    lightPosZ->setName("lightPosZ");
    lightPosZ->setHintToolTip(tr("Z depth position of the point light."));
    lightPosZ->setAnimationEnabled(true);
    lightPosZ->setDefaultValue(10.0);
    page->addKnob(lightPosZ);
    _imp->lightPosZ = lightPosZ;

    // Light Direction (for directional light)
    KnobDoublePtr lightDirX = AppManager::createKnob<KnobDouble>(this, tr("Light Dir X"));
    lightDirX->setName("lightDirX");
    lightDirX->setHintToolTip(tr("X component of the directional light direction."));
    lightDirX->setAnimationEnabled(true);
    lightDirX->setDefaultValue(0.0);
    page->addKnob(lightDirX);
    _imp->lightDirX = lightDirX;

    KnobDoublePtr lightDirY = AppManager::createKnob<KnobDouble>(this, tr("Light Dir Y"));
    lightDirY->setName("lightDirY");
    lightDirY->setHintToolTip(tr("Y component of the directional light direction."));
    lightDirY->setAnimationEnabled(true);
    lightDirY->setDefaultValue(-1.0);
    page->addKnob(lightDirY);
    _imp->lightDirY = lightDirY;

    KnobDoublePtr lightDirZ = AppManager::createKnob<KnobDouble>(this, tr("Light Dir Z"));
    lightDirZ->setName("lightDirZ");
    lightDirZ->setHintToolTip(tr("Z component of the directional light direction."));
    lightDirZ->setAnimationEnabled(true);
    lightDirZ->setDefaultValue(0.0);
    page->addKnob(lightDirZ);
    _imp->lightDirZ = lightDirZ;

    // Light Color
    KnobDoublePtr lightColorR = AppManager::createKnob<KnobDouble>(this, tr("Light Color R"));
    lightColorR->setName("lightColorR");
    lightColorR->setHintToolTip(tr("Red component of the light color."));
    lightColorR->setAnimationEnabled(true);
    lightColorR->setDefaultValue(1.0);
    lightColorR->setMinimum(0.0);
    lightColorR->setDisplayMinimum(0.0);
    lightColorR->setDisplayMaximum(2.0);
    page->addKnob(lightColorR);
    _imp->lightColorR = lightColorR;

    KnobDoublePtr lightColorG = AppManager::createKnob<KnobDouble>(this, tr("Light Color G"));
    lightColorG->setName("lightColorG");
    lightColorG->setHintToolTip(tr("Green component of the light color."));
    lightColorG->setAnimationEnabled(true);
    lightColorG->setDefaultValue(1.0);
    lightColorG->setMinimum(0.0);
    lightColorG->setDisplayMinimum(0.0);
    lightColorG->setDisplayMaximum(2.0);
    page->addKnob(lightColorG);
    _imp->lightColorG = lightColorG;

    KnobDoublePtr lightColorB = AppManager::createKnob<KnobDouble>(this, tr("Light Color B"));
    lightColorB->setName("lightColorB");
    lightColorB->setHintToolTip(tr("Blue component of the light color."));
    lightColorB->setAnimationEnabled(true);
    lightColorB->setDefaultValue(1.0);
    lightColorB->setMinimum(0.0);
    lightColorB->setDisplayMinimum(0.0);
    lightColorB->setDisplayMaximum(2.0);
    page->addKnob(lightColorB);
    _imp->lightColorB = lightColorB;

    // Intensity
    KnobDoublePtr intensity = AppManager::createKnob<KnobDouble>(this, tr("Intensity"));
    intensity->setName("intensity");
    intensity->setHintToolTip(tr("Overall light intensity multiplier."));
    intensity->setAnimationEnabled(true);
    intensity->setDefaultValue(1.0);
    intensity->setMinimum(0.0);
    intensity->setDisplayMinimum(0.0);
    intensity->setDisplayMaximum(10.0);
    page->addKnob(intensity);
    _imp->intensity = intensity;

    // Ambient
    KnobDoublePtr ambient = AppManager::createKnob<KnobDouble>(this, tr("Ambient"));
    ambient->setName("ambient");
    ambient->setHintToolTip(tr("Minimum ambient light level. Areas facing away from the light "
                               "will not go darker than this value."));
    ambient->setAnimationEnabled(true);
    ambient->setDefaultValue(0.1);
    ambient->setMinimum(0.0);
    ambient->setDisplayMinimum(0.0);
    ambient->setDisplayMaximum(1.0);
    page->addKnob(ambient);
    _imp->ambient = ambient;

    // Diffuse Strength
    KnobDoublePtr diffuseStrength = AppManager::createKnob<KnobDouble>(this, tr("Diffuse Strength"));
    diffuseStrength->setName("diffuseStrength");
    diffuseStrength->setHintToolTip(tr("Multiplier for the diffuse (Lambertian) lighting component."));
    diffuseStrength->setAnimationEnabled(true);
    diffuseStrength->setDefaultValue(1.0);
    diffuseStrength->setMinimum(0.0);
    diffuseStrength->setDisplayMinimum(0.0);
    diffuseStrength->setDisplayMaximum(2.0);
    page->addKnob(diffuseStrength);
    _imp->diffuseStrength = diffuseStrength;

    // Normal Scale
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

    // Mix Mode
    KnobChoicePtr mixMode = AppManager::createKnob<KnobChoice>(this, tr("Mix Mode"));
    mixMode->setName("mixMode");
    mixMode->setHintToolTip(tr("How the computed lighting is combined with the original colors.\n"
                               "Add: Original + light * alpha.\n"
                               "Multiply: Original * light.\n"
                               "Replace: Light * alpha (replaces original color)."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Add", "Add", ""));
        entries.push_back(ChoiceOption("Multiply", "Multiply", ""));
        entries.push_back(ChoiceOption("Replace", "Replace", ""));
        mixMode->populateChoices(entries);
    }
    mixMode->setDefaultValue(0);
    page->addKnob(mixMode);
    _imp->mixMode = mixMode;

    // Info
    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Adjust light position/direction and intensity to relight the deep image.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepRelight::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    if (_imp->lightType.lock().get() == k ||
        _imp->lightPosX.lock().get() == k ||
        _imp->lightPosY.lock().get() == k ||
        _imp->lightPosZ.lock().get() == k ||
        _imp->lightDirX.lock().get() == k ||
        _imp->lightDirY.lock().get() == k ||
        _imp->lightDirZ.lock().get() == k ||
        _imp->lightColorR.lock().get() == k ||
        _imp->lightColorG.lock().get() == k ||
        _imp->lightColorB.lock().get() == k ||
        _imp->intensity.lock().get() == k ||
        _imp->ambient.lock().get() == k ||
        _imp->diffuseStrength.lock().get() == k ||
        _imp->normalScale.lock().get() == k ||
        _imp->mixMode.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepRelight::getRegionOfDefinition(U64 /*hash*/,
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
DeepRelight::getDeepImage() const
{
    return _lastDeepImage;
}

// Helper: get the closest Z value at a given pixel from the source deep image
static float getClosestZ(const DeepImage* srcDeep, int x, int y, int zIdx, int nChannels, float refZ)
{
    int nSamples = srcDeep->getSampleCount(x, y);
    if (nSamples == 0) {
        return refZ; // no data, return reference Z to produce zero gradient
    }
    const float* data = srcDeep->getSampleData(x, y);
    float bestZ = data[0 * nChannels + zIdx];
    float bestDist = std::fabs(bestZ - refZ);
    for (int s = 1; s < nSamples; ++s) {
        float z = data[s * nChannels + zIdx];
        float dist = std::fabs(z - refZ);
        if (dist < bestDist) {
            bestZ = z;
            bestDist = dist;
        }
    }
    return bestZ;
}

StatusEnum
DeepRelight::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    int lightTypeVal = _imp->lightType.lock()->getValue();
    double lpx = _imp->lightPosX.lock()->getValue();
    double lpy = _imp->lightPosY.lock()->getValue();
    double lpz = _imp->lightPosZ.lock()->getValue();
    double ldx = _imp->lightDirX.lock()->getValue();
    double ldy = _imp->lightDirY.lock()->getValue();
    double ldz = _imp->lightDirZ.lock()->getValue();
    double lcr = _imp->lightColorR.lock()->getValue();
    double lcg = _imp->lightColorG.lock()->getValue();
    double lcb = _imp->lightColorB.lock()->getValue();
    double intensityVal = _imp->intensity.lock()->getValue();
    double ambientVal = _imp->ambient.lock()->getValue();
    double diffuseVal = _imp->diffuseStrength.lock()->getValue();
    double normalScaleVal = _imp->normalScale.lock()->getValue();
    int mixModeVal = _imp->mixMode.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    if (zIdx < 0 || rIdx < 0 || gIdx < 0 || bIdx < 0) {
        // Missing required channels, pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // Normalize directional light direction
    double dirLen = std::sqrt(ldx * ldx + ldy * ldy + ldz * ldz);
    if (dirLen > 0.0) {
        ldx /= dirLen;
        ldy /= dirLen;
        ldz /= dirLen;
    }

    // First pass: copy sample counts
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: compute lighting and write samples
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

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                float zS = src[zIdx];

                // Estimate surface normal from depth gradients
                // Sample Z at neighboring pixels, using the closest sample to this depth
                int xl = (x - 1 >= dw.x1) ? (x - 1) : x;
                int xr = (x + 1 < dw.x2) ? (x + 1) : x;
                int yb = (y - 1 >= dw.y1) ? (y - 1) : y;
                int yt = (y + 1 < dw.y2) ? (y + 1) : y;

                float zLeft  = getClosestZ(srcDeep.get(), xl, y, zIdx, nChannels, zS);
                float zRight = getClosestZ(srcDeep.get(), xr, y, zIdx, nChannels, zS);
                float zBottom = getClosestZ(srcDeep.get(), x, yb, zIdx, nChannels, zS);
                float zTop   = getClosestZ(srcDeep.get(), x, yt, zIdx, nChannels, zS);

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

                // Compute light direction at this sample
                double lx, ly, lz;
                if (lightTypeVal == 0) {
                    // Point light: direction from sample position to light position
                    lx = lpx - (double)x;
                    ly = lpy - (double)y;
                    lz = lpz - (double)zS;
                    double ll = std::sqrt(lx * lx + ly * ly + lz * lz);
                    if (ll > 0.0) {
                        lx /= ll;
                        ly /= ll;
                        lz /= ll;
                    }
                } else {
                    // Directional light: use pre-normalized direction
                    lx = ldx;
                    ly = ldy;
                    lz = ldz;
                }

                // NdotL
                double NdotL = nx * lx + ny * ly + nz * lz;
                if (NdotL < 0.0) {
                    NdotL = 0.0;
                }

                // Compute per-channel light contribution
                double lightR = ambientVal + diffuseVal * NdotL * intensityVal * lcr;
                double lightG = ambientVal + diffuseVal * NdotL * intensityVal * lcg;
                double lightB = ambientVal + diffuseVal * NdotL * intensityVal * lcb;

                float srcR = src[rIdx];
                float srcG = src[gIdx];
                float srcB = src[bIdx];
                float srcA = (aIdx >= 0) ? src[aIdx] : 1.0f;

                float dstR, dstG, dstB;

                if (mixModeVal == 0) {
                    // Add: dst.RGB = src.RGB + light * src.A
                    dstR = srcR + (float)(lightR * (double)srcA);
                    dstG = srcG + (float)(lightG * (double)srcA);
                    dstB = srcB + (float)(lightB * (double)srcA);
                } else if (mixModeVal == 1) {
                    // Multiply: dst.RGB = src.RGB * light
                    dstR = srcR * (float)lightR;
                    dstG = srcG * (float)lightG;
                    dstB = srcB * (float)lightB;
                } else {
                    // Replace: dst.RGB = light * src.A (premultiplied)
                    dstR = (float)(lightR * (double)srcA);
                    dstG = (float)(lightG * (double)srcA);
                    dstB = (float)(lightB * (double)srcA);
                }

                // Copy all channels first
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }
                // Overwrite R, G, B
                dst[rIdx] = dstR;
                dst[gIdx] = dstG;
                dst[bIdx] = dstB;
                // A, Z, ZBack remain unchanged (already copied)
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Light: " << (lightTypeVal == 0 ? "Point" : "Directional")
       << " | Intensity: " << intensityVal
       << " | Ambient: " << ambientVal
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

#include "moc_DeepRelight.cpp"
