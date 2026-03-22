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

#include "Card3D.h"

#include <cmath>
#include <vector>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../TimeLine.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct Card3DPrivate
{
    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY;
};


Card3D::Card3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Card3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Card3D::~Card3D()
{
}

std::string
Card3D::getPluginDescription() const
{
    return tr("Flat textured card (quad) geometry.\n\n"
              "Input 0 (img): 2D image to texture onto the card.\n\n"
              "Connect to ScanlineRender's obj/scn input.\n"
              "Card aspect ratio is derived from the img input dimensions.\n\n"
              "Equivalent to Nuke's Card node.").toStdString();
}

std::string
Card3D::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "img";
    return "";
}

void
Card3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Card3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Card3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
Card3D::initializeKnobs()
{
    // Transform page
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateZ = k;
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
        k->setName("scaleX"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Y"));
        k->setName("scaleY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleY = k;
    }
}

// ==================== Accessors ====================

void
Card3D::getCardTransform(double time,
                         double& tx, double& ty, double& tz,
                         double& rx, double& ry, double& rz,
                         double& sx, double& sy) const
{
    tx = _imp->translateX.lock()->getValueAtTime(time);
    ty = _imp->translateY.lock()->getValueAtTime(time);
    tz = _imp->translateZ.lock()->getValueAtTime(time);
    rx = _imp->rotateX.lock()->getValueAtTime(time);
    ry = _imp->rotateY.lock()->getValueAtTime(time);
    rz = _imp->rotateZ.lock()->getValueAtTime(time);
    sx = _imp->scaleX.lock()->getValueAtTime(time);
    sy = _imp->scaleY.lock()->getValueAtTime(time);
}

void
Card3D::generateCardMesh(double time,
                         std::vector<CardVertex>& outVertices,
                         std::vector<int>& outTriIndices) const
{
    outVertices.clear();
    outTriIndices.clear();

    // Determine aspect ratio from img input
    float aspect = 16.0f / 9.0f; // default
    EffectInstancePtr imgInput = getInput(0);
    if (imgInput) {
        RectD rod;
        RenderScale scale;
        bool isProjectFormat = false;
        StatusEnum st = imgInput->getRegionOfDefinition_public(0, time, scale, ViewIdx(0), &rod, &isProjectFormat);
        if (st == eStatusOK) {
            double w = rod.x2 - rod.x1;
            double h = rod.y2 - rod.y1;
            if (h > 0 && w > 0) {
                aspect = (float)(w / h);
            }
        }
    }

    float halfW = aspect * 0.5f;
    float halfH = 0.5f;

    // 4 vertices: bottom-left, bottom-right, top-right, top-left
    // Card lies in XY plane, facing +Z
    CardVertex v0, v1, v2, v3;

    v0.x = -halfW; v0.y = -halfH; v0.z = 0.0f;
    v0.u = 0.0f;   v0.v = 0.0f;
    v0.nx = 0.0f;  v0.ny = 0.0f;  v0.nz = 1.0f;

    v1.x =  halfW; v1.y = -halfH; v1.z = 0.0f;
    v1.u = 1.0f;   v1.v = 0.0f;
    v1.nx = 0.0f;  v1.ny = 0.0f;  v1.nz = 1.0f;

    v2.x =  halfW; v2.y =  halfH; v2.z = 0.0f;
    v2.u = 1.0f;   v2.v = 1.0f;
    v2.nx = 0.0f;  v2.ny = 0.0f;  v2.nz = 1.0f;

    v3.x = -halfW; v3.y =  halfH; v3.z = 0.0f;
    v3.u = 0.0f;   v3.v = 1.0f;
    v3.nx = 0.0f;  v3.ny = 0.0f;  v3.nz = 1.0f;

    outVertices.push_back(v0);
    outVertices.push_back(v1);
    outVertices.push_back(v2);
    outVertices.push_back(v3);

    // 2 triangles
    outTriIndices.push_back(0);
    outTriIndices.push_back(1);
    outTriIndices.push_back(2);

    outTriIndices.push_back(0);
    outTriIndices.push_back(2);
    outTriIndices.push_back(3);
}

// ==================== Texture cache for viewport ====================

void
Card3D::updateCachedTexture(double time)
{
    _cachedTexture.pixels.clear();
    _cachedTexture.width = 0;
    _cachedTexture.height = 0;

    EffectInstancePtr imgInput = getInput(0);
    if (!imgInput) return;

    // Render the input at a preview size for viewport display
    int maxSize = 512;
    RectI roiPixel;
    ImagePtr img = getImage(0, time, RenderScale(), ViewIdx(0),
                            NULL, NULL, false, true,
                            eStorageModeRAM, 0, &roiPixel);
    if (!img) return;

    RectI bounds = img->getBounds();
    int w = bounds.width();
    int h = bounds.height();
    if (w <= 0 || h <= 0) return;

    // Downscale if needed
    int dstW = w, dstH = h;
    if (w > maxSize || h > maxSize) {
        float scale = (float)maxSize / std::max(w, h);
        dstW = std::max(1, (int)(w * scale));
        dstH = std::max(1, (int)(h * scale));
    }

    _cachedTexture.width = dstW;
    _cachedTexture.height = dstH;
    _cachedTexture.pixels.resize(dstW * dstH * 4, 0.0f);

    Image::ReadAccess ra(img.get());
    for (int dy = 0; dy < dstH; ++dy) {
        int sy = bounds.y1 + (dy * h / dstH);
        for (int dx = 0; dx < dstW; ++dx) {
            int sx = bounds.x1 + (dx * w / dstW);
            const float* pix = (const float*)ra.pixelAt(sx, sy);
            if (pix) {
                int idx = (dy * dstW + dx) * 4;
                _cachedTexture.pixels[idx + 0] = pix[0];
                _cachedTexture.pixels[idx + 1] = pix[1];
                _cachedTexture.pixels[idx + 2] = pix[2];
                _cachedTexture.pixels[idx + 3] = (img->getComponents().getNumComponents() >= 4) ? pix[3] : 1.0f;
            }
        }
    }
}

// ==================== RoD / Render ====================

StatusEnum
Card3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                              ViewIdx /*view*/, RectD* rod)
{
    // Geometry node — 1x1 dummy output
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Card3D::render(const RenderActionArgs& args)
{
    // Geometry node — no image output
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

#include "moc_Card3D.cpp"
