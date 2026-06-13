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

#include "UVProject.h"

#include <cassert>
#include <cmath>
#include <cstring>

#include "CameraMath.h"
#include "CameraProvider.h"
#include "../DotUtils.h"
#include "RotationConventions.h"

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

struct UVProjectPrivate
{
    // Settings page
    KnobChoiceWPtr mode;
    KnobBoolWPtr   generatePerspective;
    KnobBoolWPtr   lockReferenceFrame;
    KnobIntWPtr    referenceFrame;

    // UV Transform page
    KnobDoubleWPtr uScale, vScale;
    KnobBoolWPtr   uInvert, vInvert;

    // Projection Transform page (used by static modes only)
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX,    rotateY,    rotateZ;
    KnobDoubleWPtr scaleX,     scaleY,     scaleZ;
};


UVProject::UVProject(NodePtr node)
    : EffectInstance(node)
    , _imp(new UVProjectPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

UVProject::~UVProject() {}

std::string
UVProject::getPluginDescription() const
{
    return tr("Rewrite UVs on a 3D mesh via projection.\n\n"
              "Modes:\n"
              "  Perspective    Project from the camera connected to 'cam'. With Generate "
              "Perspective ON, emits 3-component (s,t,w) coords so the perspective divide "
              "happens at the fragment — perspective-correct even on coarse meshes.\n"
              "  Planar XY/YZ/ZX  Project along one local axis after the Projection Transform.\n"
              "  Spherical      Equirectangular wrap (longitude/latitude).\n"
              "  Cylindrical    Cylindrical wrap. Mesh Y assumed in [-1, +1]; use V Scale to fit.\n\n"
              "Inputs:\n"
              "  geo  Required upstream geometry.\n"
              "  cam  Projection camera (Perspective mode only).\n"
              "  img  Optional projection plate that overrides the upstream mesh's texture.\n\n"
              "UVProject only rewrites texture coords — the actual rasterisation happens in "
              "ScanlineRender downstream.").toStdString();
}

std::string
UVProject::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "geo";
    if (inputNb == 1) return "cam";
    if (inputNb == 2) return "img";
    return "";
}

bool
UVProject::isInputOptional(int inputNb) const
{
    return (inputNb != 0); // geo is the only required input
}

void
UVProject::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
UVProject::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
UVProject::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
UVProject::initializeKnobs()
{
    // ----- Settings -----
    KnobPagePtr settingsPage = AppManager::createKnob<KnobPage>(this, tr("Settings"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
        k->setName("uvProjectMode"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("off",         "Off",         "Pass through upstream UVs unchanged."));
        entries.push_back(ChoiceOption("perspective", "Perspective", "Project from 'cam' input. Requires a camera."));
        entries.push_back(ChoiceOption("planar_xy",   "Planar XY",   "Planar projection along local Z."));
        entries.push_back(ChoiceOption("planar_yz",   "Planar YZ",   "Planar projection along local X."));
        entries.push_back(ChoiceOption("planar_zx",   "Planar ZX",   "Planar projection along local Y."));
        entries.push_back(ChoiceOption("spherical",   "Spherical",   "Equirectangular sphere wrap."));
        entries.push_back(ChoiceOption("cylindrical", "Cylindrical", "Cylindrical wrap; assumes mesh Y in [-1, 1]."));
        k->populateChoices(entries);
        k->setDefaultValue(eModePerspective);
        settingsPage->addKnob(k); _imp->mode = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Generate Perspective"));
        k->setName("generatePerspective");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Perspective mode only. When ON, emits (s,t,w) triples so OpenGL "
                              "does the perspective divide at the fragment — correct even on "
                              "coarse meshes. When OFF, divides at the vertex (legacy Project3D "
                              "behavior, visibly skewed on flat oblique geometry)."));
        settingsPage->addKnob(k); _imp->generatePerspective = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Lock Reference Frame"));
        k->setName("lockReferenceFrame"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Perspective mode only. Sample the projection camera at the "
                              "Reference Frame instead of the current time. Useful to bake a "
                              "projection from a held camera pose."));
        settingsPage->addKnob(k); _imp->lockReferenceFrame = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Reference Frame"));
        k->setName("referenceFrame"); k->setDefaultValue(1);
        settingsPage->addKnob(k); _imp->referenceFrame = k;
    }

    // ----- UV Transform -----
    KnobPagePtr uvPage = AppManager::createKnob<KnobPage>(this, tr("UV Transform"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("U Scale"));
        k->setName("uScale"); k->setDefaultValue(1.0);
        k->setDisplayMinimum(0.01); k->setDisplayMaximum(10.0);
        uvPage->addKnob(k); _imp->uScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("V Scale"));
        k->setName("vScale"); k->setDefaultValue(1.0);
        k->setDisplayMinimum(0.01); k->setDisplayMaximum(10.0);
        uvPage->addKnob(k); _imp->vScale = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("U Invert"));
        k->setName("uInvert"); k->setDefaultValue(false);
        uvPage->addKnob(k); _imp->uInvert = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("V Invert"));
        k->setName("vInvert"); k->setDefaultValue(false);
        uvPage->addKnob(k); _imp->vInvert = k;
    }

    // ----- Projection Transform (static modes) -----
    KnobPagePtr xfPage = AppManager::createKnob<KnobPage>(this, tr("Projection Transform"));
    const char* tNames[3] = { "projTranslateX", "projTranslateY", "projTranslateZ" };
    const char* tLabels[3] = { "Translate X", "Translate Y", "Translate Z" };
    KnobDoubleWPtr* tSlots[3] = { &_imp->translateX, &_imp->translateY, &_imp->translateZ };
    for (int i = 0; i < 3; ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(tLabels[i]));
        k->setName(tNames[i]); k->setDefaultValue(0.0);
        xfPage->addKnob(k); *tSlots[i] = k;
    }
    const char* rNames[3] = { "projRotateX", "projRotateY", "projRotateZ" };
    const char* rLabels[3] = { "Rotate X", "Rotate Y", "Rotate Z" };
    KnobDoubleWPtr* rSlots[3] = { &_imp->rotateX, &_imp->rotateY, &_imp->rotateZ };
    for (int i = 0; i < 3; ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(rLabels[i]));
        k->setName(rNames[i]); k->setDefaultValue(0.0);
        xfPage->addKnob(k); *rSlots[i] = k;
    }
    const char* sNames[3] = { "projScaleX", "projScaleY", "projScaleZ" };
    const char* sLabels[3] = { "Scale X", "Scale Y", "Scale Z" };
    KnobDoubleWPtr* sSlots[3] = { &_imp->scaleX, &_imp->scaleY, &_imp->scaleZ };
    for (int i = 0; i < 3; ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(sLabels[i]));
        k->setName(sNames[i]); k->setDefaultValue(1.0);
        xfPage->addKnob(k); *sSlots[i] = k;
    }
}

// ==================== Matrix helpers (local) ====================

namespace {

// View matrix from camera TRS (extrinsic XYZ column-vector). Matches
// Project3D::buildViewMatrix and ScanlineRender::buildViewMatrix verbatim.
void
buildCameraView(double tx, double ty, double tz,
                double rx, double ry, double rz,
                float out[16])
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

// Column-major 4x4 multiply: out = a * b.
void
mat4Mul(const float a[16], const float b[16], float out[16])
{
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c*4 + r] =
                a[0*4 + r] * b[c*4 + 0] +
                a[1*4 + r] * b[c*4 + 1] +
                a[2*4 + r] * b[c*4 + 2] +
                a[3*4 + r] * b[c*4 + 3];
        }
    }
}

} // anonymous namespace

// ==================== UV rewrite (consumed by ScanlineRender) ====================

void
UVProject::rewriteUVs(const std::vector<float>& verts,
                      const float worldMatrix[16],
                      double time,
                      std::vector<float>& outUVs,
                      std::vector<float>& outSTW,
                      int& outComponents) const
{
    outUVs.clear();
    outSTW.clear();
    outComponents = 0;

    const int mode = _imp->mode.lock()->getValue();
    if (mode == eModeOff) return;

    const int nv = (int)(verts.size() / 3);
    if (nv == 0) return;

    // UV Transform params (applied uniformly post-mode).
    const double us = _imp->uScale.lock()->getValue();
    const double vs = _imp->vScale.lock()->getValue();
    const bool   ui = _imp->uInvert.lock()->getValue();
    const bool   vi = _imp->vInvert.lock()->getValue();

    auto applyUVScale = [&](float& u, float& v) {
        u *= (float)us; v *= (float)vs;
        if (ui) u = 1.0f - u;
        if (vi) v = 1.0f - v;
    };

    // ---------- Perspective mode ----------
    if (mode == eModePerspective) {
        EffectInstancePtr camEffect = skipDots(const_cast<UVProject*>(this)->getInput(1));
        CameraProvider* cam = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;
        if (!cam) return; // no camera → leave outComponents == 0 (caller keeps upstream UVs)

        double camTime = time;
        if (_imp->lockReferenceFrame.lock()->getValue()) {
            camTime = (double)_imp->referenceFrame.lock()->getValue();
        }

        double tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0;
        cam->getCameraPosition(camTime, tx, ty, tz, rx, ry, rz);
        const double focal  = cam->getCameraFocalLength(camTime);
        const double hAp    = cam->getCameraHAperture(camTime);
        const double vAp    = cam->getCameraVAperture(camTime);
        const double nearC  = cam->getCameraNear(camTime);
        const double farC   = cam->getCameraFar(camTime);

        float view[16], proj[16];
        buildCameraView(tx, ty, tz, rx, ry, rz, view);
        CameraMath::composeProjectionMatrix(focal, hAp, vAp, (float)nearC, (float)farC, proj);

        // MVP = proj * view * world.
        float viewWorld[16], mvp[16];
        mat4Mul(view, worldMatrix, viewWorld);
        mat4Mul(proj, viewWorld, mvp);

        const bool gen = _imp->generatePerspective.lock()->getValue();
        if (gen) {
            // 3-component (s, t, w) — fragment-level perspective divide.
            outSTW.assign(nv * 3, 0.0f);
            for (int i = 0; i < nv; ++i) {
                const float x = verts[i*3 + 0];
                const float y = verts[i*3 + 1];
                const float z = verts[i*3 + 2];
                const float cx = mvp[0]*x + mvp[4]*y + mvp[8] *z + mvp[12];
                const float cy = mvp[1]*x + mvp[5]*y + mvp[9] *z + mvp[13];
                const float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
                // Pre-divide [-1,+1] → [0,1] mapping: u = (cx/cw + 1)/2 = (cx + cw) / (2w).
                // To keep linearity for fragment interp, pack the numerator as s = (cx+cw)/2.
                float s = (cx + cw) * 0.5f;
                float t = (cy + cw) * 0.5f;
                // Scale folds into s,t pre-divide so it's truly scale-invariant after /w.
                s *= (float)us;
                t *= (float)vs;
                // Invert: s'/w = 1 - s/w  ⇔  s' = w - s.
                if (ui) s = cw - s;
                if (vi) t = cw - t;
                outSTW[i*3 + 0] = s;
                outSTW[i*3 + 1] = t;
                outSTW[i*3 + 2] = cw;
            }
            outComponents = 3;
        } else {
            // Per-vertex divide — legacy Project3D path; visibly skews on coarse meshes.
            outUVs.assign(nv * 2, 0.0f);
            for (int i = 0; i < nv; ++i) {
                const float x = verts[i*3 + 0];
                const float y = verts[i*3 + 1];
                const float z = verts[i*3 + 2];
                const float cx = mvp[0]*x + mvp[4]*y + mvp[8] *z + mvp[12];
                const float cy = mvp[1]*x + mvp[5]*y + mvp[9] *z + mvp[13];
                const float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
                float u = (std::fabs(cw) > 1e-6f) ? (cx / cw) * 0.5f + 0.5f : 0.0f;
                float v = (std::fabs(cw) > 1e-6f) ? (cy / cw) * 0.5f + 0.5f : 0.0f;
                applyUVScale(u, v);
                outUVs[i*2 + 0] = u;
                outUVs[i*2 + 1] = v;
            }
            outComponents = 2;
        }
        return;
    }

    // ---------- Static modes ----------
    const double pjTx = _imp->translateX.lock()->getValue();
    const double pjTy = _imp->translateY.lock()->getValue();
    const double pjTz = _imp->translateZ.lock()->getValue();
    const double pjRx = _imp->rotateX.lock()->getValue();
    const double pjRy = _imp->rotateY.lock()->getValue();
    const double pjRz = _imp->rotateZ.lock()->getValue();
    const double pjSx = _imp->scaleX.lock()->getValue();
    const double pjSy = _imp->scaleY.lock()->getValue();
    const double pjSz = _imp->scaleZ.lock()->getValue();

    double rInv[3][3];
    RotationConventions::composeInverse(pjRx, pjRy, pjRz, rInv);

    auto worldToLocal = [&](float wx, float wy, float wz, float& lx, float& ly, float& lz) {
        const double dx = (double)wx - pjTx;
        const double dy = (double)wy - pjTy;
        const double dz = (double)wz - pjTz;
        const double rx_ = rInv[0][0]*dx + rInv[0][1]*dy + rInv[0][2]*dz;
        const double ry_ = rInv[1][0]*dx + rInv[1][1]*dy + rInv[1][2]*dz;
        const double rz_ = rInv[2][0]*dx + rInv[2][1]*dy + rInv[2][2]*dz;
        const double sx_safe = (std::fabs(pjSx) > 1e-6) ? pjSx : 1.0;
        const double sy_safe = (std::fabs(pjSy) > 1e-6) ? pjSy : 1.0;
        const double sz_safe = (std::fabs(pjSz) > 1e-6) ? pjSz : 1.0;
        lx = (float)(rx_ / sx_safe);
        ly = (float)(ry_ / sy_safe);
        lz = (float)(rz_ / sz_safe);
    };

    outUVs.assign(nv * 2, 0.0f);
    for (int i = 0; i < nv; ++i) {
        const float vx = verts[i*3 + 0];
        const float vy = verts[i*3 + 1];
        const float vz = verts[i*3 + 2];

        // Local → world via the mesh's worldMatrix (column-major 4x4).
        const float wx = worldMatrix[0]*vx + worldMatrix[4]*vy + worldMatrix[8] *vz + worldMatrix[12];
        const float wy = worldMatrix[1]*vx + worldMatrix[5]*vy + worldMatrix[9] *vz + worldMatrix[13];
        const float wz = worldMatrix[2]*vx + worldMatrix[6]*vy + worldMatrix[10]*vz + worldMatrix[14];

        float lx = 0, ly = 0, lz = 0;
        worldToLocal(wx, wy, wz, lx, ly, lz);

        float u = 0.5f, v = 0.5f;
        switch (mode) {
            case eModePlanarXY: u = lx * 0.5f + 0.5f; v = ly * 0.5f + 0.5f; break;
            case eModePlanarYZ: u = ly * 0.5f + 0.5f; v = lz * 0.5f + 0.5f; break;
            case eModePlanarZX: u = lz * 0.5f + 0.5f; v = lx * 0.5f + 0.5f; break;
            case eModeSpherical: {
                const float r = std::sqrt(lx*lx + ly*ly + lz*lz);
                u = (float)(std::atan2(lz, lx) / (2.0 * M_PI) + 0.5);
                v = (r > 1e-6f) ? (float)(std::asin(ly / r) / M_PI + 0.5) : 0.5f;
                break;
            }
            case eModeCylindrical: {
                u = (float)(std::atan2(lz, lx) / (2.0 * M_PI) + 0.5);
                v = (ly + 1.0f) * 0.5f;
                break;
            }
            default: break;
        }
        applyUVScale(u, v);
        outUVs[i*2 + 0] = u;
        outUVs[i*2 + 1] = v;
    }
    outComponents = 2;
}

// ==================== Generator-node plumbing (no real output) ====================

StatusEnum
UVProject::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                  ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0; rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
UVProject::render(const RenderActionArgs& /*args*/)
{
    // UVProject is a UV rewriter consumed by ScanlineRender. No image output.
    return eStatusOK;
}

void
UVProject::updateCachedTexture(double time)
{
    // Render the projection image (input 2, "img") at preview size so the 3D
    // viewport can display the projected texture on the geo. Empty => no img.
    _cachedTexture.pixels.clear();
    _cachedTexture.width = 0;
    _cachedTexture.height = 0;

    if (!getInput(2)) return;

    const int maxSize = 512;
    RectI roiPixel;
    ImagePtr img = getImage(2, time, RenderScale(), ViewIdx(0),
                            NULL, NULL, false, true,
                            eStorageModeRAM, 0, &roiPixel);
    if (!img) return;

    RectI bounds = img->getBounds();
    int w = bounds.width();
    int h = bounds.height();
    if (w <= 0 || h <= 0) return;

    int dstW = w, dstH = h;
    if (w > maxSize || h > maxSize) {
        float scale = (float)maxSize / (w > h ? w : h);
        dstW = (int)(w * scale); if (dstW < 1) dstW = 1;
        dstH = (int)(h * scale); if (dstH < 1) dstH = 1;
    }

    _cachedTexture.width = dstW;
    _cachedTexture.height = dstH;
    _cachedTexture.pixels.resize(dstW * dstH * 4, 0.0f);

    Image::ReadAccess ra(img.get());
    const int nComp = img->getComponents().getNumComponents();
    for (int dy = 0; dy < dstH; ++dy) {
        int sy = bounds.y1 + (dy * h / dstH);
        for (int dx = 0; dx < dstW; ++dx) {
            int sx = bounds.x1 + (dx * w / dstW);
            const float* pix = (const float*)ra.pixelAt(sx, sy);
            if (pix) {
                int idx = (dy * dstW + dx) * 4;
                _cachedTexture.pixels[idx + 0] = pix[0];
                _cachedTexture.pixels[idx + 1] = (nComp >= 2) ? pix[1] : pix[0];
                _cachedTexture.pixels[idx + 2] = (nComp >= 3) ? pix[2] : pix[0];
                _cachedTexture.pixels[idx + 3] = (nComp >= 4) ? pix[3] : 1.0f;
            }
        }
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_UVProject.cpp"
