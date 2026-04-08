/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "Volume3D.h"

#include <cmath>
#include <algorithm>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct Volume3DPrivate
{
    // Transform
    KnobDoubleWPtr centerX, centerY, centerZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    // Volume
    KnobChoiceWPtr volumeType; // 0=sphere, 1=box
    KnobIntWPtr resolution;
    KnobDoubleWPtr density;
    KnobDoubleWPtr noiseScale;
    KnobDoubleWPtr noiseDetail;
    KnobDoubleWPtr stepSize;
    KnobIntWPtr volumeBounces;

    // Color
    KnobDoubleWPtr colorR, colorG, colorB;
};


Volume3D::Volume3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Volume3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
    _cachedVolRes = 0;
    _cachedVolTime = -1;
}

Volume3D::~Volume3D()
{
}

std::string
Volume3D::getPluginDescription() const
{
    return tr("Procedural 3D volume for volumetric rendering.\n\n"
              "Generates a 3D density field (sphere, noise cloud, box) "
              "that renders through ScanlineRender using GPU ray marching.\n\n"
              "Connect to ScanlineRender's obj/scn input with a Camera3D.").toStdString();
}

void
Volume3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Volume3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Volume3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Volume3D::initializeKnobs()
{
    // Transform
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->centerX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->centerY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->centerZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        k->setName("rotateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        k->setName("rotateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        k->setName("rotateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale X"));
        k->setName("scaleX"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Y"));
        k->setName("scaleY"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
        k->setName("scaleZ"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleZ = k;
    }

    // Volume
    KnobPagePtr volPage = AppManager::createKnob<KnobPage>(this, tr("Volume"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Type"));
        k->setName("volumeType");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("sphere", "Sphere", "Spherical density falloff"));
        entries.push_back(ChoiceOption("box", "Box", "Uniform density box"));
        k->populateChoices(entries);
        k->setDefaultValue(0); // sphere by default
        volPage->addKnob(k); _imp->volumeType = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Resolution"));
        k->setName("resolution"); k->setDefaultValue(64);
        k->setMinimum(8); k->setDisplayMinimum(16); k->setDisplayMaximum(128);
        volPage->addKnob(k); _imp->resolution = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Density"));
        k->setName("density"); k->setDefaultValue(8.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(50.0);
        volPage->addKnob(k); _imp->density = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Noise Scale"));
        k->setName("noiseScale"); k->setDefaultValue(3.0); k->setAnimationEnabled(true);
        k->setMinimum(0.1); k->setDisplayMinimum(0.1); k->setDisplayMaximum(20.0);
        volPage->addKnob(k); _imp->noiseScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Noise Detail"));
        k->setName("noiseDetail"); k->setDefaultValue(4.0); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(8.0);
        volPage->addKnob(k); _imp->noiseDetail = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Step Size"));
        k->setName("stepSize"); k->setDefaultValue(0.1);
        k->setMinimum(0.001); k->setDisplayMinimum(0.01); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Ray marching step size. Smaller = smoother/slower. 0 = auto."));
        volPage->addKnob(k); _imp->stepSize = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Volume Bounces"));
        k->setName("volumeBounces"); k->setDefaultValue(2);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(8);
        k->setHintToolTip(tr("Max light bounces inside volume. 0 = single scatter only."));
        volPage->addKnob(k); _imp->volumeBounces = k;
    }

    // Color
    KnobPagePtr colorPage = AppManager::createKnob<KnobPage>(this, tr("Color"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color R"));
        k->setName("colorR"); k->setDefaultValue(0.8); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color G"));
        k->setName("colorG"); k->setDefaultValue(0.8); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color B"));
        k->setName("colorB"); k->setDefaultValue(0.9); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorB = k;
    }
}

Volume3D::VolumeParams
Volume3D::getVolumeParams(double time) const
{
    VolumeParams vp;
    vp.centerX = (float)_imp->centerX.lock()->getValueAtTime(time);
    vp.centerY = (float)_imp->centerY.lock()->getValueAtTime(time);
    vp.centerZ = (float)_imp->centerZ.lock()->getValueAtTime(time);
    vp.scaleX = (float)_imp->scaleX.lock()->getValueAtTime(time);
    vp.scaleY = (float)_imp->scaleY.lock()->getValueAtTime(time);
    vp.scaleZ = (float)_imp->scaleZ.lock()->getValueAtTime(time);
    vp.density = (float)_imp->density.lock()->getValueAtTime(time);
    vp.colorR = (float)_imp->colorR.lock()->getValueAtTime(time);
    vp.colorG = (float)_imp->colorG.lock()->getValueAtTime(time);
    vp.colorB = (float)_imp->colorB.lock()->getValueAtTime(time);
    vp.resolution = _imp->resolution.lock()->getValueAtTime(time);
    vp.volumeType = _imp->volumeType.lock()->getValueAtTime(time);
    vp.noiseScale = (float)_imp->noiseScale.lock()->getValueAtTime(time);
    vp.noiseDetail = (float)_imp->noiseDetail.lock()->getValueAtTime(time);
    vp.stepSize = (float)_imp->stepSize.lock()->getValueAtTime(time);
    vp.volumeBounces = _imp->volumeBounces.lock()->getValueAtTime(time);
    return vp;
}

// Simple 3D noise function
static float hash3D(float x, float y, float z)
{
    float n = sinf(x * 127.1f + y * 311.7f + z * 74.7f) * 43758.5453f;
    return n - floorf(n);
}

static float smoothNoise3D(float x, float y, float z)
{
    float ix = floorf(x), iy = floorf(y), iz = floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;

    // Smoothstep
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    fz = fz * fz * (3.0f - 2.0f * fz);

    // Trilinear interpolation of hash values
    float v000 = hash3D(ix, iy, iz);
    float v100 = hash3D(ix + 1, iy, iz);
    float v010 = hash3D(ix, iy + 1, iz);
    float v110 = hash3D(ix + 1, iy + 1, iz);
    float v001 = hash3D(ix, iy, iz + 1);
    float v101 = hash3D(ix + 1, iy, iz + 1);
    float v011 = hash3D(ix, iy + 1, iz + 1);
    float v111 = hash3D(ix + 1, iy + 1, iz + 1);

    float v00 = v000 + fx * (v100 - v000);
    float v10 = v010 + fx * (v110 - v010);
    float v01 = v001 + fx * (v101 - v001);
    float v11 = v011 + fx * (v111 - v011);

    float v0 = v00 + fy * (v10 - v00);
    float v1 = v01 + fy * (v11 - v01);

    return v0 + fz * (v1 - v0);
}

static float fbm3D(float x, float y, float z, int octaves)
{
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        value += amplitude * smoothNoise3D(x * frequency, y * frequency, z * frequency);
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return value;
}

void
Volume3D::generateVolumeData(double time, std::vector<float>& outData, int& res) const
{
    VolumeParams vp = getVolumeParams(time);

    // Cache key includes all params, not just time
    U64 paramHash = 0;
    {
        auto hc = [](U64 s, U64 v) -> U64 { return s ^ (v * 0x9e3779b97f4a7c15ULL + (s << 6) + (s >> 2)); };
        union { double d; U64 u; } conv;
        conv.d = time; paramHash = hc(paramHash, conv.u);
        paramHash = hc(paramHash, (U64)vp.volumeType);
        paramHash = hc(paramHash, (U64)vp.resolution);
        conv.d = vp.density; paramHash = hc(paramHash, conv.u);
        conv.d = vp.noiseScale; paramHash = hc(paramHash, conv.u);
        conv.d = vp.noiseDetail; paramHash = hc(paramHash, conv.u);
    }
    if (paramHash == _cachedVolHash && _cachedVolRes > 0 && !_cachedVolData.empty()) {
        outData = _cachedVolData;
        res = _cachedVolRes;
        return;
    }
    res = std::max(8, std::min(128, vp.resolution));

    outData.resize(res * res * res, 0.0f);

    float invRes = 1.0f / (float)res;

    for (int z = 0; z < res; ++z) {
        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                // Normalized coords [0, 1]
                float u = ((float)x + 0.5f) * invRes;
                float v = ((float)y + 0.5f) * invRes;
                float w = ((float)z + 0.5f) * invRes;

                // Map to [-1, 1]
                float lx = u * 2.0f - 1.0f;
                float ly = v * 2.0f - 1.0f;
                float lz = w * 2.0f - 1.0f;

                float density = 0.0f;

                if (vp.volumeType == 0) {
                    // Sphere: density = 1 - distance from center
                    float dist = sqrtf(lx * lx + ly * ly + lz * lz);
                    density = std::max(0.0f, 1.0f - dist);
                } else if (vp.volumeType == 1) {
                    // Noise cloud: FBM noise modulated by spherical falloff
                    float dist = sqrtf(lx * lx + ly * ly + lz * lz);
                    float sphereFalloff = std::max(0.0f, 1.0f - dist);

                    float noise = fbm3D(lx * vp.noiseScale, ly * vp.noiseScale, lz * vp.noiseScale,
                                        (int)vp.noiseDetail);
                    density = sphereFalloff * noise * 2.0f;
                } else if (vp.volumeType == 2) {
                    // Box: uniform density inside
                    if (fabsf(lx) < 0.8f && fabsf(ly) < 0.8f && fabsf(lz) < 0.8f) {
                        density = 1.0f;
                    }
                }

                outData[z * res * res + y * res + x] = std::max(0.0f, density);
            }
        }
    }

    // Cache
    _cachedVolData = outData;
    _cachedVolRes = res;
    _cachedVolTime = time;
    _cachedVolHash = paramHash;
}

StatusEnum
Volume3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Volume3D::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.0f; }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Volume3D.cpp"
