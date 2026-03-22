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

#include "Cube3D.h"

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

struct Cube3DPrivate
{
    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    // Geometry
    KnobDoubleWPtr size;
};


Cube3D::Cube3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Cube3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Cube3D::~Cube3D()
{
}

std::string
Cube3D::getPluginDescription() const
{
    return tr("Cube geometry with per-face UV mapping.\n\n"
              "Input 0 (img): 2D image to texture onto each face.\n\n"
              "Connect to ScanlineRender's obj/scn input.\n"
              "Each face gets the full 0-1 UV range.\n\n"
              "Equivalent to Nuke's Cube node.").toStdString();
}

std::string
Cube3D::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "img";
    return "";
}

void
Cube3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Cube3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Cube3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
Cube3D::initializeKnobs()
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
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
        k->setName("scaleZ"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleZ = k;
    }

    // Geometry page
    KnobPagePtr geoPage = AppManager::createKnob<KnobPage>(this, tr("Geometry"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"));
        k->setName("size"); k->setDefaultValue(1.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(100.0);
        geoPage->addKnob(k); _imp->size = k;
    }
}

// ==================== Accessors ====================

void
Cube3D::getCubeTransform(double time,
                         double& tx, double& ty, double& tz,
                         double& rx, double& ry, double& rz,
                         double& sx, double& sy, double& sz) const
{
    tx = _imp->translateX.lock()->getValueAtTime(time);
    ty = _imp->translateY.lock()->getValueAtTime(time);
    tz = _imp->translateZ.lock()->getValueAtTime(time);
    rx = _imp->rotateX.lock()->getValueAtTime(time);
    ry = _imp->rotateY.lock()->getValueAtTime(time);
    rz = _imp->rotateZ.lock()->getValueAtTime(time);
    sx = _imp->scaleX.lock()->getValueAtTime(time);
    sy = _imp->scaleY.lock()->getValueAtTime(time);
    sz = _imp->scaleZ.lock()->getValueAtTime(time);
}

double Cube3D::getSize(double time) const { return _imp->size.lock()->getValueAtTime(time); }

void
Cube3D::generateCubeMesh(double time,
                         std::vector<CubeVertex>& outVertices,
                         std::vector<int>& outTriIndices) const
{
    outVertices.clear();
    outTriIndices.clear();

    float s = (float)getSize(time) * 0.5f;

    // 24 unique vertices (4 per face) for per-face UV mapping
    // Face order: +Z (front), -Z (back), +X (right), -X (left), +Y (top), -Y (bottom)

    // Helper lambda to add a face (4 vertices + 2 triangles)
    auto addFace = [&](float x0, float y0, float z0,
                       float x1, float y1, float z1,
                       float x2, float y2, float z2,
                       float x3, float y3, float z3,
                       float nx, float ny, float nz) {
        int base = (int)outVertices.size();

        CubeVertex v;
        v.nx = nx; v.ny = ny; v.nz = nz;

        v.x = x0; v.y = y0; v.z = z0; v.u = 0.0f; v.v = 0.0f;
        outVertices.push_back(v);
        v.x = x1; v.y = y1; v.z = z1; v.u = 1.0f; v.v = 0.0f;
        outVertices.push_back(v);
        v.x = x2; v.y = y2; v.z = z2; v.u = 1.0f; v.v = 1.0f;
        outVertices.push_back(v);
        v.x = x3; v.y = y3; v.z = z3; v.u = 0.0f; v.v = 1.0f;
        outVertices.push_back(v);

        outTriIndices.push_back(base + 0);
        outTriIndices.push_back(base + 1);
        outTriIndices.push_back(base + 2);

        outTriIndices.push_back(base + 0);
        outTriIndices.push_back(base + 2);
        outTriIndices.push_back(base + 3);
    };

    // Front face (+Z)
    addFace(-s, -s,  s,
             s, -s,  s,
             s,  s,  s,
            -s,  s,  s,
            0.0f, 0.0f, 1.0f);

    // Back face (-Z)
    addFace( s, -s, -s,
            -s, -s, -s,
            -s,  s, -s,
             s,  s, -s,
            0.0f, 0.0f, -1.0f);

    // Right face (+X)
    addFace( s, -s,  s,
             s, -s, -s,
             s,  s, -s,
             s,  s,  s,
            1.0f, 0.0f, 0.0f);

    // Left face (-X)
    addFace(-s, -s, -s,
            -s, -s,  s,
            -s,  s,  s,
            -s,  s, -s,
            -1.0f, 0.0f, 0.0f);

    // Top face (+Y)
    addFace(-s,  s,  s,
             s,  s,  s,
             s,  s, -s,
            -s,  s, -s,
            0.0f, 1.0f, 0.0f);

    // Bottom face (-Y)
    addFace(-s, -s, -s,
             s, -s, -s,
             s, -s,  s,
            -s, -s,  s,
            0.0f, -1.0f, 0.0f);
}

// ==================== Texture cache for viewport ====================

void
Cube3D::updateCachedTexture(double time)
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
Cube3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                              ViewIdx /*view*/, RectD* rod)
{
    // Geometry node — 1x1 dummy output
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Cube3D::render(const RenderActionArgs& args)
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

#include "moc_Cube3D.cpp"
