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

#include "Cylinder3D.h"
#include "Material3D.h"
#include "../DotUtils.h"

#include <cmath>
#include <vector>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../KnobFile.h"
#include "../../Node.h"
#include "../../TimeLine.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct Cylinder3DPrivate
{
    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    // Geometry
    KnobIntWPtr rows, columns;
    KnobDoubleWPtr radius, height;
    KnobBoolWPtr closeTop, closeBottom;

    // Material
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    KnobFileWPtr textureFile;
    KnobChoiceWPtr diffuseColorspace;
};


Cylinder3D::Cylinder3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Cylinder3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Cylinder3D::~Cylinder3D()
{
}

std::string
Cylinder3D::getPluginDescription() const
{
    return tr("Cylinder geometry with configurable rows, columns, radius, height,\n"
              "and optional top/bottom caps.\n\n"
              "Input 0 (img): 2D image to texture onto the cylinder.\n\n"
              "Connect to ScanlineRender's obj/scn input.\n"
              "UV mapping: U wraps around the circumference, V goes along the height.\n\n"
              "Equivalent to Nuke's Cylinder node.").toStdString();
}

std::string
Cylinder3D::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "img";
    if (inputNb == 1) return "mat";
    return "";
}

void
Cylinder3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Cylinder3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Cylinder3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
Cylinder3D::initializeKnobs()
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
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Rows"));
        k->setName("rows"); k->setDefaultValue(1);
        k->setMinimum(1); k->setDisplayMinimum(1); k->setDisplayMaximum(64);
        geoPage->addKnob(k); _imp->rows = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Columns"));
        k->setName("columns"); k->setDefaultValue(30);
        k->setMinimum(3); k->setDisplayMinimum(3); k->setDisplayMaximum(128);
        geoPage->addKnob(k); _imp->columns = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Radius"));
        k->setName("radius"); k->setDefaultValue(1.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(100.0);
        geoPage->addKnob(k); _imp->radius = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Height"));
        k->setName("height"); k->setDefaultValue(2.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(100.0);
        geoPage->addKnob(k); _imp->height = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Close Top"));
        k->setName("closeTop"); k->setDefaultValue(true);
        geoPage->addKnob(k); _imp->closeTop = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Close Bottom"));
        k->setName("closeBottom"); k->setDefaultValue(true);
        geoPage->addKnob(k); _imp->closeBottom = k;
    }

    // Material page
    KnobPagePtr matPage = AppManager::createKnob<KnobPage>(this, tr("Material"));
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Base Color"), 3);
        k->setName("baseColor");
        k->setDefaultValue(0.8, 0); k->setDefaultValue(0.8, 1); k->setDefaultValue(0.8, 2);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->baseColor = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Roughness"));
        k->setName("roughness"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->roughness = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Metallic"));
        k->setName("metallic"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->metallic = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Specular"));
        k->setName("specular"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->specular = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Emission Color"), 3);
        k->setName("emissionColor");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(1.0, 2);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->emissionColor = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emission Strength"));
        k->setName("emissionStrength"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->emissionStrength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Transmission"));
        k->setName("transmission"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("0 = opaque, 1 = fully transparent (glass). Use with IOR."));
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->transmission = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("IOR"));
        k->setName("ior"); k->setDefaultValue(1.45);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(2.5);
        k->setHintToolTip(tr("Index of refraction. Glass=1.5, Water=1.33, Diamond=2.42"));
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->ior = k;
    }
    // Texture Maps page
    KnobPagePtr texPage = AppManager::createKnob<KnobPage>(this, tr("Texture Maps"));
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Diffuse Map"));
        k->setName("textureFile");
        k->setHintToolTip(tr("Base color / albedo texture. Supports .exr, .hdr, .png, .jpg"));
        texPage->addKnob(k); _imp->textureFile = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Diffuse Colorspace"));
        k->setName("diffuseColorspace");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("sRGB", "", "sRGB gamma-encoded (PNG, JPEG)"));
        entries.push_back(ChoiceOption("Linear", "", "Linear / scene-referred (EXR, HDR)"));
        entries.push_back(ChoiceOption("ACEScg", "", "ACEScg (AP1 linear, ACES pipeline)"));
        entries.push_back(ChoiceOption("Raw", "", "Raw data, no conversion"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Color space of the diffuse texture file."));
        texPage->addKnob(k); _imp->diffuseColorspace = k;
    }
}

// ==================== Accessors ====================

void
Cylinder3D::getCylinderTransform(double time,
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

int Cylinder3D::getRows(double time) const { return _imp->rows.lock()->getValueAtTime(time); }
int Cylinder3D::getColumns(double time) const { return _imp->columns.lock()->getValueAtTime(time); }
double Cylinder3D::getRadius(double time) const { return _imp->radius.lock()->getValueAtTime(time); }
double Cylinder3D::getHeight(double time) const { return _imp->height.lock()->getValueAtTime(time); }

void
Cylinder3D::generateCylinderMesh(double time,
                                 std::vector<CylinderVertex>& outVertices,
                                 std::vector<int>& outTriIndices) const
{
    int nRows = getRows(time);
    int nCols = getColumns(time);
    float r = (float)getRadius(time);
    float h = (float)getHeight(time);
    bool capTop = _imp->closeTop.lock()->getValueAtTime(time);
    bool capBottom = _imp->closeBottom.lock()->getValueAtTime(time);

    outVertices.clear();
    outTriIndices.clear();

    float halfH = h * 0.5f;

    // Generate cylinder body vertices
    // (nRows+1) rings along height, (nCols+1) vertices per ring (duplicated seam)
    for (int row = 0; row <= nRows; ++row) {
        float v = (float)row / (float)nRows;
        float y = -halfH + v * h;

        for (int col = 0; col <= nCols; ++col) {
            float u = (float)col / (float)nCols;
            float theta = u * 2.0f * (float)M_PI;

            CylinderVertex cv;
            cv.nx = cosf(theta);
            cv.ny = 0.0f;
            cv.nz = sinf(theta);
            cv.x = r * cv.nx;
            cv.y = y;
            cv.z = r * cv.nz;
            cv.u = u;
            cv.v = v;

            outVertices.push_back(cv);
        }
    }

    // Generate body triangle indices
    int vertsPerRing = nCols + 1;
    for (int row = 0; row < nRows; ++row) {
        for (int col = 0; col < nCols; ++col) {
            int topLeft = row * vertsPerRing + col;
            int topRight = topLeft + 1;
            int botLeft = (row + 1) * vertsPerRing + col;
            int botRight = botLeft + 1;

            outTriIndices.push_back(topLeft);
            outTriIndices.push_back(botLeft);
            outTriIndices.push_back(topRight);

            outTriIndices.push_back(topRight);
            outTriIndices.push_back(botLeft);
            outTriIndices.push_back(botRight);
        }
    }

    // Top cap
    if (capTop) {
        int centerIdx = (int)outVertices.size();
        CylinderVertex center;
        center.x = 0.0f; center.y = halfH; center.z = 0.0f;
        center.u = 0.5f; center.v = 0.5f;
        center.nx = 0.0f; center.ny = 1.0f; center.nz = 0.0f;
        outVertices.push_back(center);

        int ringStart = (int)outVertices.size();
        for (int col = 0; col <= nCols; ++col) {
            float u = (float)col / (float)nCols;
            float theta = u * 2.0f * (float)M_PI;

            CylinderVertex cv;
            cv.x = r * cosf(theta);
            cv.y = halfH;
            cv.z = r * sinf(theta);
            cv.u = 0.5f + 0.5f * cosf(theta);
            cv.v = 0.5f + 0.5f * sinf(theta);
            cv.nx = 0.0f; cv.ny = 1.0f; cv.nz = 0.0f;
            outVertices.push_back(cv);
        }

        for (int col = 0; col < nCols; ++col) {
            outTriIndices.push_back(centerIdx);
            outTriIndices.push_back(ringStart + col);
            outTriIndices.push_back(ringStart + col + 1);
        }
    }

    // Bottom cap
    if (capBottom) {
        int centerIdx = (int)outVertices.size();
        CylinderVertex center;
        center.x = 0.0f; center.y = -halfH; center.z = 0.0f;
        center.u = 0.5f; center.v = 0.5f;
        center.nx = 0.0f; center.ny = -1.0f; center.nz = 0.0f;
        outVertices.push_back(center);

        int ringStart = (int)outVertices.size();
        for (int col = 0; col <= nCols; ++col) {
            float u = (float)col / (float)nCols;
            float theta = u * 2.0f * (float)M_PI;

            CylinderVertex cv;
            cv.x = r * cosf(theta);
            cv.y = -halfH;
            cv.z = r * sinf(theta);
            cv.u = 0.5f + 0.5f * cosf(theta);
            cv.v = 0.5f + 0.5f * sinf(theta);
            cv.nx = 0.0f; cv.ny = -1.0f; cv.nz = 0.0f;
            outVertices.push_back(cv);
        }

        for (int col = 0; col < nCols; ++col) {
            outTriIndices.push_back(centerIdx);
            outTriIndices.push_back(ringStart + col + 1);
            outTriIndices.push_back(ringStart + col);
        }
    }
}

// ==================== Texture cache for viewport ====================

void
Cylinder3D::updateCachedTexture(double time)
{
    _cachedTexture.pixels.clear();
    _cachedTexture.width = 0;
    _cachedTexture.height = 0;

    // Prefer a connected Material3D's diffuse texture so a textured material shows
    // on this shape in the 3D viewport (not only in the Cycles render).
    if (Material3D* m3d = dynamic_cast<Material3D*>(getConnectedMaterial())) {
        m3d->updateCachedTexture(time);
        const Material3D::CachedTexture& mt = m3d->getCachedTexture();
        if (mt.width > 0 && mt.height > 0 && !mt.pixels.empty()) {
            _cachedTexture.width = mt.width;
            _cachedTexture.height = mt.height;
            _cachedTexture.pixels = mt.pixels;
            return;
        }
    }

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
Cylinder3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                  ViewIdx /*view*/, RectD* rod)
{
    // Geometry node — 1x1 dummy output
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Cylinder3D::render(const RenderActionArgs& args)
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

// ==================== MaterialProvider ====================

void
Cylinder3D::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}

double Cylinder3D::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }

double Cylinder3D::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Cylinder3D::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5; }

void
Cylinder3D::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}

double Cylinder3D::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Cylinder3D::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }

std::string Cylinder3D::getMaterialTextureFile() const
{ KnobFilePtr k = _imp->textureFile.lock(); return k ? k->getValue() : std::string(); }

std::string Cylinder3D::getMaterialDiffuseColorspace() const
{
    KnobChoicePtr k = _imp->diffuseColorspace.lock();
    int idx = k ? k->getValue() : 0;
    const char* names[] = {"sRGB", "Linear", "ACEScg", "Raw"};
    return (idx >= 0 && idx < 4) ? names[idx] : "sRGB";
}

bool Cylinder3D::hasMaterialInput() const
{
    EffectInstancePtr inp = skipDots(getInput(1));
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}

MaterialProvider* Cylinder3D::getConnectedMaterial() const
{
    EffectInstancePtr inp = skipDots(getInput(1));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Cylinder3D.cpp"
