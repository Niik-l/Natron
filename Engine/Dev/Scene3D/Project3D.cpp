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

#include "Project3D.h"
#include "MaterialTextureBake.h"

#include <algorithm>

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Image.h"
#include "../../Node.h"
#include "CameraProvider.h"
#include "CameraMath.h"
#include "RotationConventions.h"
#include "../DotUtils.h"

NATRON_NAMESPACE_ENTER

struct Project3DPrivate
{
    KnobChoiceWPtr projectOn;    // 0 Front, 1 Back, 2 Both
    KnobBoolWPtr   cropToFrame;  // transparent outside the plate frame
    KnobDoubleWPtr nearClip;
    KnobDoubleWPtr farClip;
    KnobChoiceWPtr occlusion;    // 0 None, 1 Self, 2 World
};

// View matrix = inverse of camera-to-world (Natron extrinsic XYZ convention). Same as
// ScanlineRender::buildViewMatrix / the old Project3D.
static void
p3dBuildViewMatrix(double tx, double ty, double tz, double rx, double ry, double rz, float out[16])
{
    double mInv[3][3];
    RotationConventions::composeInverse(rx, ry, rz, mInv);
    const float ntx = -(float)tx, nty = -(float)ty, ntz = -(float)tz;
    out[0]  = (float)mInv[0][0]; out[1]  = (float)mInv[1][0]; out[2]  = (float)mInv[2][0]; out[3]  = 0.f;
    out[4]  = (float)mInv[0][1]; out[5]  = (float)mInv[1][1]; out[6]  = (float)mInv[2][1]; out[7]  = 0.f;
    out[8]  = (float)mInv[0][2]; out[9]  = (float)mInv[1][2]; out[10] = (float)mInv[2][2]; out[11] = 0.f;
    out[12] = (float)(mInv[0][0]*ntx + mInv[0][1]*nty + mInv[0][2]*ntz);
    out[13] = (float)(mInv[1][0]*ntx + mInv[1][1]*nty + mInv[1][2]*ntz);
    out[14] = (float)(mInv[2][0]*ntx + mInv[2][1]*nty + mInv[2][2]*ntz);
    out[15] = 1.f;
}

// out = a * b (column-major).
static void
p3dMat4Mul(float out[16], const float a[16], const float b[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

Project3D::Project3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Project3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Project3D::~Project3D()
{
}

std::string
Project3D::getPluginDescription() const
{
    return tr("Project a 2D plate through a camera onto geometry, as a live material "
              "(Nuke Project3D parity).\n\n"
              "Connect a plate to 'img' and a Camera3D to 'cam', then connect this node to "
              "a geometry node's material ('mat') input. ScanlineRender projects the plate "
              "onto the geo from the camera's point of view, per fragment.\n\n"
              "Controls follow Nuke's Project3D: project on (front / back / both faces), "
              "crop (clip to the plate frame), near / far clip, and occlusion (none / self / "
              "world — hide surfaces blocked from the projector by nearer geometry).\n\n"
              "The classic matte-painting 'lock' is a FrameHold on the projection camera. "
              "(Projected in ScanlineRender; the Cycles path renders it as a plain white "
              "material for now.)").toStdString();
}

std::string
Project3D::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "img";   // the plate to project
        case 1: return "cam";   // the projection camera
        default: return std::string();
    }
}

bool
Project3D::isInputOptional(int /*inputNb*/) const
{
    // Both optional so the node never errors in the graph; projection is simply
    // inactive until both a plate and a camera are connected.
    return true;
}

void
Project3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
}

void
Project3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Project3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Project3D::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Project3D"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Project On"));
        k->setName("projectOn");
        k->setHintToolTip(tr("Which faces receive the projection, relative to the projection camera.\n"
                             "Front: faces pointing toward the camera (default).\n"
                             "Back: faces pointing away.\n"
                             "Both: every face."));
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Front", "", "Faces pointing toward the camera"));
        entries.push_back(ChoiceOption("Back", "", "Faces pointing away from the camera"));
        entries.push_back(ChoiceOption("Both", "", "All faces"));
        k->populateChoices(entries);
        k->setDefaultValue(eProjectBoth);   // matches Nuke's Project3D default
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->projectOn = k;
    }

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Crop"));
        k->setName("crop");
        k->setHintToolTip(tr("On: the projection is transparent outside the plate frame (the "
                             "camera's view rectangle). Off: the plate's edge pixels are clamped "
                             "and smear outward."));
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->cropToFrame = k;
    }

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Occlusion"));
        k->setName("occlusion");
        k->setHintToolTip(tr("Hide surfaces the projector can't 'see' because nearer geometry "
                             "blocks them (raycast / depth test from the projection camera).\n"
                             "None: project through everything (default).\n"
                             "Self: occlude against the geometry this material is on.\n"
                             "World: occlude against all geometry in the scene."));
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("None", "", "Project through (no occlusion)"));
        entries.push_back(ChoiceOption("Self", "", "Occlude against this object"));
        entries.push_back(ChoiceOption("World", "", "Occlude against the whole scene"));
        k->populateChoices(entries);
        k->setDefaultValue(eOcclusionNone);
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->occlusion = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Near Clip"));
        k->setName("nearClip");
        k->setHintToolTip(tr("Geometry closer to the projection camera than this distance is not "
                             "projected onto."));
        k->setDefaultValue(0.1);
        k->setMinimum(0.0);
        k->setDisplayMinimum(0.0);
        k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->nearClip = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Far Clip"));
        k->setName("farClip");
        k->setHintToolTip(tr("Geometry farther from the projection camera than this distance is "
                             "not projected onto."));
        k->setDefaultValue(10000.0);
        k->setMinimum(0.0);
        k->setDisplayMinimum(1.0);
        k->setDisplayMaximum(100000.0);
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->farClip = k;
    }
}

// ---- MaterialProvider: neutral defaults (the real work is the projection below) ----

void
Project3D::getMaterialBaseColor(double /*time*/, double& r, double& g, double& b) const
{
    r = g = b = 1.0;
}

double Project3D::getMaterialRoughness(double /*time*/) const { return 0.5; }
double Project3D::getMaterialMetallic(double /*time*/) const { return 0.0; }
double Project3D::getMaterialSpecular(double /*time*/) const { return 0.5; }

void
Project3D::getMaterialEmission(double /*time*/, double& r, double& g, double& b, double& strength) const
{
    r = g = b = 0.0;
    strength = 0.0;
}

double Project3D::getMaterialTransmission(double /*time*/) const { return 0.0; }
double Project3D::getMaterialIOR(double /*time*/) const { return 1.45; }

// ---- Projection query (read by ScanlineRender) ----

bool
Project3D::getProjectorCamera(double time,
                              double& tx, double& ty, double& tz,
                              double& rx, double& ry, double& rz,
                              double& focal, double& hAperture, double& vAperture) const
{
    EffectInstancePtr camEff = skipDots(getInput(1));
    CameraProvider* cam = camEff ? dynamic_cast<CameraProvider*>(camEff.get()) : NULL;
    if (!cam) {
        return false;
    }
    cam->getCameraPosition(time, tx, ty, tz, rx, ry, rz);
    focal = cam->getCameraFocalLength(time);
    hAperture = cam->getCameraHAperture(time);
    vAperture = cam->getCameraVAperture(time);
    return true;
}

int
Project3D::getProjectOn(double time) const
{
    KnobChoicePtr k = _imp->projectOn.lock();
    return k ? k->getValueAtTime(time) : (int)eProjectFront;
}

bool
Project3D::getCropToFrame(double time) const
{
    KnobBoolPtr k = _imp->cropToFrame.lock();
    return k ? k->getValueAtTime(time) : true;
}

double
Project3D::getNearClip(double time) const
{
    KnobDoublePtr k = _imp->nearClip.lock();
    return k ? k->getValueAtTime(time) : 0.1;
}

double
Project3D::getFarClip(double time) const
{
    KnobDoublePtr k = _imp->farClip.lock();
    return k ? k->getValueAtTime(time) : 10000.0;
}

int
Project3D::getOcclusionMode(double time) const
{
    KnobChoicePtr k = _imp->occlusion.lock();
    return k ? k->getValueAtTime(time) : (int)eOcclusionNone;
}

bool
Project3D::getProjectorViewProj(double time, float outVP[16]) const
{
    double tx, ty, tz, rx, ry, rz, focal, hAp, vAp;
    if (!getProjectorCamera(time, tx, ty, tz, rx, ry, rz, focal, hAp, vAp)) {
        return false;
    }
    float view[16], proj[16];
    p3dBuildViewMatrix(tx, ty, tz, rx, ry, rz, view);
    const float nearC = (float)getNearClip(time);
    CameraMath::composeProjectionMatrix(focal, hAp, vAp, (nearC > 1e-4f ? nearC : 0.1f),
                                        (float)getFarClip(time), proj);
    p3dMat4Mul(outVP, proj, view);  // outVP = proj * view (world -> projector clip)
    return true;
}

void
Project3D::updateCachedTexture(double time)
{
    // Nothing feeding the texture changed since the last build: keep it. The
    // 3D viewport calls this on every paint for every geo using this node.
    const unsigned long long key = materialTextureCacheKey(getInput(0), time, nullptr);
    {
        std::lock_guard<std::mutex> lk(_texMutex);
        if (_texKeyValid && _texKey == key) return;
    }
    // Build into a local and publish an immutable snapshot on every exit path
    // — the previously published texture is shared with concurrent readers
    // (GUI paint / render workers) and must never be mutated in place.
    std::shared_ptr<CachedTexture> tex = std::make_shared<CachedTexture>();
    auto publish = [&]() {
        std::lock_guard<std::mutex> lk(_texMutex);
        _cachedTexture = tex;
        _texKey = key;
        _texKeyValid = true;
    };
    // Render the plate (input 0) at a preview size so a geo this material is on can show
    // the projection live in the 3D viewport. Mirrors Material3D::updateCachedTexture.
    tex->pixels.clear();
    tex->width = 0;
    tex->height = 0;

    if (!getInput(0)) { publish(); return; }

    const int maxSize = 512;
    RectI roiPixel;
    ImagePtr img = getImage(0, time, RenderScale(), ViewIdx(0),
                            NULL, NULL, false, true, eStorageModeRAM, 0, &roiPixel);
    if (!img) { publish(); return; }

    RectI bounds = img->getBounds();
    int w = bounds.width();
    int h = bounds.height();
    if (w <= 0 || h <= 0) { publish(); return; }

    int dstW = w, dstH = h;
    if (w > maxSize || h > maxSize) {
        float scale = (float)maxSize / std::max(w, h);
        dstW = std::max(1, (int)(w * scale));
        dstH = std::max(1, (int)(h * scale));
    }

    tex->width = dstW;
    tex->height = dstH;
    tex->pixels.resize((size_t)dstW * dstH * 4, 0.0f);

    Image::ReadAccess ra(img.get());
    const int nComp = img->getComponents().getNumComponents();
    for (int dy = 0; dy < dstH; ++dy) {
        int sy = bounds.y1 + (dy * h / dstH);
        for (int dx = 0; dx < dstW; ++dx) {
            int sx = bounds.x1 + (dx * w / dstW);
            const float* pix = (const float*)ra.pixelAt(sx, sy);
            if (pix) {
                int idx = (dy * dstW + dx) * 4;
                tex->pixels[idx + 0] = pix[0];
                tex->pixels[idx + 1] = (nComp >= 2) ? pix[1] : pix[0];
                tex->pixels[idx + 2] = (nComp >= 3) ? pix[2] : pix[0];
                tex->pixels[idx + 3] = (nComp >= 4) ? pix[3] : 1.0f;
            }
        }
    }
    publish();
}

// ---- Effect plumbing: this is a material/data node, not an image producer ----

StatusEnum
Project3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                 ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Project3D::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.f; }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
