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

NATRON_NAMESPACE_ENTER

struct Cube3DPrivate
{
    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobDoubleWPtr uniformScale;   // multiplies all three axes together

    // Geometry
    KnobDoubleWPtr size;

    // Material
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    KnobFileWPtr textureFile;
    KnobChoiceWPtr diffuseColorspace;
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
    if (inputNb == 1) return "mat";
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
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Uniform Scale"));
        k->setName("uniformScale"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Scales all axes together, multiplied on top of the per-axis Scale values."));
        xformPage->addKnob(k); _imp->uniformScale = k;
    }

    // Geometry page
    KnobPagePtr geoPage = AppManager::createKnob<KnobPage>(this, tr("Geometry"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"));
        k->setName("size"); k->setDefaultValue(1.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(100.0);
        geoPage->addKnob(k); _imp->size = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Viewport Display"));
        k->setName("viewportDisplay");
        k->setHintToolTip(tr("How the cube draws in the 3D viewport. Auto shows wireframe "
                             "when the cube feeds a bounds input (ParticleKillBox, Blast) "
                             "so the particles/geometry inside stay visible, and solid "
                             "otherwise. Renders (ScanlineRender / Cycles) are unaffected."));
        {
            std::vector<ChoiceOption> opts;
            opts.push_back(ChoiceOption("Auto", "", "Wireframe when used as a bounds volume, solid otherwise"));
            opts.push_back(ChoiceOption("Solid", "", "Shaded faces + wire overlay"));
            opts.push_back(ChoiceOption("Wireframe", "", "Edges only"));
            k->populateChoices(opts);
        }
        k->setDefaultValue(0);
        geoPage->addKnob(k);
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
    const double us = _imp->uniformScale.lock()->getValueAtTime(time);
    sx *= us; sy *= us; sz *= us;
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
    // Build into a local and publish an immutable snapshot on every exit path
    // — the previously published texture is shared with concurrent readers
    // (GUI paint / render workers) and must never be mutated in place.
    std::shared_ptr<CachedTexture> tex = std::make_shared<CachedTexture>();
    auto publish = [&]() {
        std::lock_guard<std::mutex> lk(_texMutex);
        _cachedTexture = tex;
    };
    tex->pixels.clear();
    tex->width = 0;
    tex->height = 0;

    // Prefer a connected Material3D's diffuse texture so a textured material shows
    // on this shape in the 3D viewport (not only in the Cycles render).
    if (Material3D* m3d = dynamic_cast<Material3D*>(getConnectedMaterial())) {
        m3d->updateCachedTexture(time);
        Material3D::CachedTexturePtr mtPtr = m3d->getCachedTexture();
        const Material3D::CachedTexture& mt = *mtPtr;
        if (mt.width > 0 && mt.height > 0 && !mt.pixels.empty()) {
            tex->width = mt.width;
            tex->height = mt.height;
            tex->pixels = mt.pixels;
            { publish(); return; }
        }
    }

    EffectInstancePtr imgInput = getInput(0);
    if (!imgInput) { publish(); return; }

    // Render the input at a preview size for viewport display
    int maxSize = 512;
    RectI roiPixel;
    ImagePtr img = getImage(0, time, RenderScale(), ViewIdx(0),
                            NULL, NULL, false, true,
                            eStorageModeRAM, 0, &roiPixel);
    if (!img) { publish(); return; }

    RectI bounds = img->getBounds();
    int w = bounds.width();
    int h = bounds.height();
    if (w <= 0 || h <= 0) { publish(); return; }

    // Downscale if needed
    int dstW = w, dstH = h;
    if (w > maxSize || h > maxSize) {
        float scale = (float)maxSize / std::max(w, h);
        dstW = std::max(1, (int)(w * scale));
        dstH = std::max(1, (int)(h * scale));
    }

    tex->width = dstW;
    tex->height = dstH;
    tex->pixels.resize(dstW * dstH * 4, 0.0f);

    Image::ReadAccess ra(img.get());
    for (int dy = 0; dy < dstH; ++dy) {
        int sy = bounds.y1 + (dy * h / dstH);
        for (int dx = 0; dx < dstW; ++dx) {
            int sx = bounds.x1 + (dx * w / dstW);
            const float* pix = (const float*)ra.pixelAt(sx, sy);
            if (pix) {
                int idx = (dy * dstW + dx) * 4;
                tex->pixels[idx + 0] = pix[0];
                tex->pixels[idx + 1] = pix[1];
                tex->pixels[idx + 2] = pix[2];
                tex->pixels[idx + 3] = (img->getComponents().getNumComponents() >= 4) ? pix[3] : 1.0f;
            }
        }
    }
    publish();
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

// ==================== MaterialProvider ====================

void
Cube3D::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}

double Cube3D::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }

double Cube3D::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Cube3D::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5; }

void
Cube3D::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}

double Cube3D::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Cube3D::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }

bool Cube3D::usingBakedInput() const
{
    KnobFilePtr k = _imp->textureFile.lock();
    if (k && !k->getValue().empty()) return false;   // an explicit file wins
    std::lock_guard<std::mutex> lk(_bakedImg.mutex);
    return !_bakedImg.path.empty();
}

std::string Cube3D::getMaterialTextureFile() const
{
    KnobFilePtr k = _imp->textureFile.lock();
    std::string file = k ? k->getValue() : std::string();
    if (!file.empty()) return file;
    // No explicit texture: the img input, baked to a temp EXR by
    // bakeImageInput(). Empty until the first Cycles render request bakes it.
    std::lock_guard<std::mutex> lk(_bakedImg.mutex);
    return _bakedImg.path;
}

std::string Cube3D::getMaterialDiffuseColorspace() const
{
    // The bake writes the input's scene-linear float pixels verbatim, so the
    // Diffuse Colorspace knob (meant for files on disk) must not be applied.
    if (usingBakedInput()) return "Linear";
    KnobChoicePtr k = _imp->diffuseColorspace.lock();
    int idx = k ? k->getValue() : 0;
    const char* names[] = {"sRGB", "Linear", "ACEScg", "Raw"};
    return (idx >= 0 && idx < 4) ? names[idx] : "sRGB";
}

bool Cube3D::getMaterialTextureUsesAlpha() const
{
    // The img input carries the comp's alpha (a card IS its cutout); a
    // Texture File path keeps the old opaque behaviour.
    return usingBakedInput();
}

unsigned long long Cube3D::getMaterialInputsHash(double /*time*/) const
{
    return materialInputChainHash(const_cast<Cube3D*>(this)->getInput(0), 0);
}

void Cube3D::bakeImageInput(double time)
{
    bakeShapeImageInput(this, 0, time, "cube3d_img", _bakedImg);
}

bool Cube3D::hasMaterialInput() const
{
    EffectInstancePtr inp = skipDots(getInput(1));
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}

MaterialProvider* Cube3D::getConnectedMaterial() const
{
    EffectInstancePtr inp = skipDots(getInput(1));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Cube3D.cpp"
