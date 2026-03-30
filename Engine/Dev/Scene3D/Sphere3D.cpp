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

#include "Sphere3D.h"

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

struct Sphere3DPrivate
{
    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    // Geometry
    KnobIntWPtr rows, columns;
    KnobDoubleWPtr radius;
    KnobDoubleWPtr uExtent, vExtent;

    // Material
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    KnobFileWPtr textureFile;
};


Sphere3D::Sphere3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Sphere3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Sphere3D::~Sphere3D()
{
}

std::string
Sphere3D::getPluginDescription() const
{
    return tr("Sphere geometry with equirectangular UV mapping.\n\n"
              "Input 0 (img): 2D image to texture onto the sphere.\n\n"
              "Connect to ScanlineRender's obj/scn input.\n"
              "UV mapping is equirectangular — suitable for HDRI environment maps.\n\n"
              "Equivalent to Nuke's Sphere node.").toStdString();
}

std::string
Sphere3D::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "img";
    if (inputNb == 1) return "mat";
    return "";
}

void
Sphere3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Sphere3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Sphere3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
Sphere3D::initializeKnobs()
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
        k->setName("rows"); k->setDefaultValue(30);
        k->setMinimum(3); k->setDisplayMinimum(3); k->setDisplayMaximum(128);
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
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("U Extent (degrees)"));
        k->setName("uExtent"); k->setDefaultValue(360.0);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(360.0);
        geoPage->addKnob(k); _imp->uExtent = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("V Extent (degrees)"));
        k->setName("vExtent"); k->setDefaultValue(180.0);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(180.0);
        geoPage->addKnob(k); _imp->vExtent = k;
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
}

// ==================== Accessors ====================

void
Sphere3D::getSphereTransform(double time,
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

int Sphere3D::getRows(double time) const { return _imp->rows.lock()->getValueAtTime(time); }
int Sphere3D::getColumns(double time) const { return _imp->columns.lock()->getValueAtTime(time); }
double Sphere3D::getRadius(double time) const { return _imp->radius.lock()->getValueAtTime(time); }

void
Sphere3D::generateSphereMesh(double time,
                             std::vector<SphereVertex>& outVertices,
                             std::vector<int>& outTriIndices) const
{
    int nRows = getRows(time);
    int nCols = getColumns(time);
    float r = (float)getRadius(time);
    float uExt = (float)(_imp->uExtent.lock()->getValueAtTime(time) * M_PI / 180.0);
    float vExt = (float)(_imp->vExtent.lock()->getValueAtTime(time) * M_PI / 180.0);

    outVertices.clear();
    outTriIndices.clear();

    // Generate vertices with equirectangular UVs
    for (int row = 0; row <= nRows; ++row) {
        float v = (float)row / (float)nRows;
        float phi = v * vExt - vExt * 0.5f; // -vExt/2 to +vExt/2 (poles at top/bottom)

        for (int col = 0; col <= nCols; ++col) {
            float u = (float)col / (float)nCols;
            float theta = u * uExt;

            SphereVertex sv;
            sv.nx = cosf(phi) * sinf(theta);
            sv.ny = sinf(phi);
            sv.nz = cosf(phi) * cosf(theta);
            sv.x = r * sv.nx;
            sv.y = r * sv.ny;
            sv.z = r * sv.nz;
            sv.u = u;
            sv.v = v;

            outVertices.push_back(sv);
        }
    }

    // Generate triangle indices
    int vertsPerRow = nCols + 1;
    for (int row = 0; row < nRows; ++row) {
        for (int col = 0; col < nCols; ++col) {
            int topLeft = row * vertsPerRow + col;
            int topRight = topLeft + 1;
            int botLeft = (row + 1) * vertsPerRow + col;
            int botRight = botLeft + 1;

            // Two triangles per quad
            outTriIndices.push_back(topLeft);
            outTriIndices.push_back(botLeft);
            outTriIndices.push_back(topRight);

            outTriIndices.push_back(topRight);
            outTriIndices.push_back(botLeft);
            outTriIndices.push_back(botRight);
        }
    }
}

// ==================== Texture cache for viewport ====================

void
Sphere3D::updateCachedTexture(double time)
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
Sphere3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                ViewIdx /*view*/, RectD* rod)
{
    // Geometry node — 1x1 dummy output
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Sphere3D::render(const RenderActionArgs& args)
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
Sphere3D::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}

double Sphere3D::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }

double Sphere3D::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Sphere3D::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5; }

void
Sphere3D::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}

double Sphere3D::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Sphere3D::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }

std::string Sphere3D::getMaterialTextureFile() const
{ KnobFilePtr k = _imp->textureFile.lock(); return k ? k->getValue() : std::string(); }

bool Sphere3D::hasMaterialInput() const
{
    EffectInstancePtr inp = getInput(1);
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}

MaterialProvider* Sphere3D::getConnectedMaterial() const
{
    EffectInstancePtr inp = getInput(1);
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Sphere3D.cpp"
