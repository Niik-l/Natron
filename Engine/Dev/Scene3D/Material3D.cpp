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

#include "Material3D.h"
#include "MaterialTextureBake.h"

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../KnobFile.h"
#include "../../Node.h"
#include "../../OCIOColorSpaceUtils.h"
#include "../../ViewIdx.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <memory>
#ifdef NATRON_HAVE_OPENIMAGEIO
#include <OpenImageIO/imageio.h>
#endif

NATRON_NAMESPACE_ENTER

// Build the entries for a texture-colorspace dropdown from the active OCIO config,
// so the names actually resolve under whatever config the user runs (e.g. ACES v4,
// where plain "sRGB" does not exist — it's "sRGB - Texture" etc.). The list is
// "Raw" + "Linear" (both = no transform / already in render space) followed by every
// colorspace name in the config. If OCIO is unavailable, falls back to the legacy
// fixed set. *defaultIndex receives a sensible default (an sRGB-ish entry if present).
// The chosen entry's id string is what gets handed to Cycles' set_colorspace().
static std::vector<ChoiceOption>
buildTextureColorspaceChoices(int* defaultIndex)
{
    std::vector<ChoiceOption> entries;
    entries.push_back( ChoiceOption("Raw", "", "Raw data, no color conversion") );
    entries.push_back( ChoiceOption("Linear", "", "Already in scene-linear / render space, no conversion") );

    const std::vector<std::string> configSpaces = getOcioColorSpaceNames();
    int srgbIdx = -1;
    if ( !configSpaces.empty() ) {
        for (std::size_t i = 0; i < configSpaces.size(); ++i) {
            const std::string& name = configSpaces[i];
            // Skip names that would duplicate the synthetic Raw/Linear entries.
            if (name == "Raw" || name == "Linear") {
                continue;
            }
            // Remember the first sRGB-ish space to use as the default (typical albedo).
            if (srgbIdx < 0) {
                std::string lower = name;
                for (std::size_t c = 0; c < lower.size(); ++c) {
                    lower[c] = (char)std::tolower( (unsigned char)lower[c] );
                }
                if (lower.find("srgb") != std::string::npos) {
                    srgbIdx = (int)entries.size();
                }
            }
            entries.push_back( ChoiceOption(name, "", "") );
        }
    } else {
        // No OCIO config available — keep the original fixed choices.
        entries.push_back( ChoiceOption("sRGB", "", "sRGB gamma-encoded (PNG, JPEG)") );
        entries.push_back( ChoiceOption("ACEScg", "", "ACEScg (AP1 linear, ACES pipeline)") );
        srgbIdx = 2;
    }

    if (defaultIndex) {
        *defaultIndex = (srgbIdx >= 0) ? srgbIdx : 1; // sRGB-ish, else "Linear"
    }
    return entries;
}

struct Material3DPrivate
{
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    KnobFileWPtr textureFile;

    // PBR texture maps
    KnobFileWPtr normalMapFile;
    KnobFileWPtr specularMapFile;
    KnobFileWPtr opacityMapFile;
    KnobBoolWPtr opacityInvert;
    KnobFileWPtr translucencyMapFile;
    KnobDoubleWPtr translucencyStrength;
    KnobFileWPtr displacementMapFile;
    KnobDoubleWPtr displacementScale, displacementMidlevel;
    KnobDoubleWPtr normalStrength;
    KnobFileWPtr roughnessMapFile;
    KnobFileWPtr metallicMapFile;
    KnobFileWPtr emissionMapFile;
    KnobFileWPtr transmissionMapFile;

    // Texture colorspace
    KnobChoiceWPtr diffuseColorspace;
    KnobChoiceWPtr emissionColorspace;

    // Baked input texture temp file paths (from connected 2D nodes)
    // Guards the six baked paths: bakeInputTextures writes them from the
    // Cycles render path while the getMaterial*File getters are read from the
    // scene-hash / shader-build threads — a torn std::string read is UB.
    std::mutex bakePathMutex;
    // What the last bake was made from: the connected inputs' hash + the time
    // it was baked at. Re-baking is a full input render plus a multi-MB HDR
    // write per map, so it is skipped while both are unchanged.
    bool hasBakedOnce = false;
    unsigned long long lastBakeInputsHash = 0;
    double lastBakeTime = 0.;

    std::string bakedDiffusePath;
    std::string bakedMetallicPath;
    std::string bakedRoughnessPath;
    std::string bakedEmissionPath;
    std::string bakedNormalPath;
    std::string bakedTransmissionPath;
};

Material3D::Material3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Material3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Material3D::~Material3D()
{
}

std::string
Material3D::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "Diffuse";
        case 1: return "Metallic";
        case 2: return "Roughness";
        case 3: return "Emission";
        case 4: return "Normal";
        case 5: return "Transmission";
        default: return std::string();
    }
}

std::string
Material3D::getPluginDescription() const
{
    return tr("Principled BSDF material for Cycles rendering.\n\n"
              "Connect to a geometry node's 'mat' input to assign this material.\n"
              "Multiple geometry nodes can share the same Material3D.\n\n"
              "Covers most real-world materials: plastic, metal, glass, ceramic, etc.").toStdString();
}

void
Material3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Material3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Material3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Material3D::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Material"));

    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Base Color"), 3);
        k->setName("baseColor");
        k->setDefaultValue(0.8, 0); k->setDefaultValue(0.8, 1); k->setDefaultValue(0.8, 2);
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->baseColor = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Roughness"));
        k->setName("roughness"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->roughness = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Metallic"));
        k->setName("metallic"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->metallic = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Specular"));
        k->setName("specular"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->specular = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Emission Color"), 3);
        k->setName("emissionColor");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(1.0, 2);
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->emissionColor = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emission Strength"));
        k->setName("emissionStrength"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->emissionStrength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Transmission"));
        k->setName("transmission"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("0 = opaque, 1 = fully transparent (glass). Use with IOR."));
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->transmission = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("IOR"));
        k->setName("ior"); k->setDefaultValue(1.45);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(2.5);
        k->setHintToolTip(tr("Index of refraction. Glass=1.5, Water=1.33, Diamond=2.42"));
        k->setAnimationEnabled(true);
        page->addKnob(k); _imp->ior = k;
    }
    // Texture Maps page — order: Diffuse, Metallic, Roughness, Emission, Normal
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
        int defIdx = 0;
        std::vector<ChoiceOption> entries = buildTextureColorspaceChoices(&defIdx);
        k->populateChoices(entries);
        k->setDefaultValue(defIdx);
        k->setHintToolTip(tr("Color space of the diffuse texture file (from the active OCIO config). "
                             "Cycles converts to scene linear at load time."));
        texPage->addKnob(k); _imp->diffuseColorspace = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Metallic Map"));
        k->setName("metallicMapFile");
        k->setHintToolTip(tr("Grayscale metallic texture. Overrides the Metallic slider."));
        texPage->addKnob(k); _imp->metallicMapFile = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Roughness Map"));
        k->setName("roughnessMapFile");
        k->setHintToolTip(tr("Grayscale roughness texture. Overrides the Roughness slider."));
        texPage->addKnob(k); _imp->roughnessMapFile = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Transmission Map"));
        k->setName("transmissionMapFile");
        k->setHintToolTip(tr("Grayscale transmission mask. Overrides the Transmission slider "
                             "(white = glass/transparent, black = opaque). Or connect a 2D node "
                             "to the Transmission input."));
        texPage->addKnob(k); _imp->transmissionMapFile = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Emission Map"));
        k->setName("emissionMapFile");
        k->setHintToolTip(tr("RGB emission texture. Multiplied by Emission Strength."));
        texPage->addKnob(k); _imp->emissionMapFile = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Emission Colorspace"));
        k->setName("emissionColorspace");
        int defIdx = 0;
        std::vector<ChoiceOption> entries = buildTextureColorspaceChoices(&defIdx);
        k->populateChoices(entries);
        k->setDefaultValue(defIdx);
        k->setHintToolTip(tr("Color space of the emission texture file (from the active OCIO config)."));
        texPage->addKnob(k); _imp->emissionColorspace = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Normal Map"));
        k->setName("normalMapFile");
        k->setHintToolTip(tr("Tangent-space normal map. Color space set to Non-Color automatically."));
        texPage->addKnob(k); _imp->normalMapFile = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Normal Strength"));
        k->setName("normalStrength"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Strength of the normal map effect. 0 = flat, 1 = full strength."));
        k->setAnimationEnabled(true);
        texPage->addKnob(k); _imp->normalStrength = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Specular Map"));
        k->setName("specularMapFile");
        k->setHintToolTip(tr("Greyscale specular level map, driving Principled's "
                             "\"Specular IOR Level\" (0.5 = default dielectric). "
                             "Read as Non-Color."));
        texPage->addKnob(k); _imp->specularMapFile = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Opacity Map"));
        k->setName("opacityMapFile");
        k->setHintToolTip(tr("Greyscale cutout mask driving Principled's Alpha: white = opaque, "
                             "black = fully transparent. Required for atlas-based foliage — "
                             "without it every leaf card renders as a solid quad. Read as "
                             "Non-Color."));
        texPage->addKnob(k); _imp->opacityMapFile = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert Opacity"));
        k->setName("opacityInvert"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Flip the cutout. Some libraries ship the mask the other way up "
                             "and call it \"Transparency\" — white meaning see-through rather "
                             "than solid. The giveaway is a plant whose leaves vanish and whose "
                             "background turns solid.\n\n"
                             "Only opacity gets this tick: roughness and normal have image "
                             "inputs, so those can be inverted upstream in the comp."));
        k->setAnimationEnabled(false);
        texPage->addKnob(k); _imp->opacityInvert = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Translucency Map"));
        k->setName("translucencyMapFile");
        k->setHintToolTip(tr("Backlight colour for thin surfaces (leaves, petals, paper). Mixed "
                             "in as a Translucent BSDF using the map's luminance as the blend "
                             "amount — cheaper and better suited to thin geometry than "
                             "subsurface scattering, which is noisy on single-sided cards."));
        texPage->addKnob(k); _imp->translucencyMapFile = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translucency Amount"));
        k->setName("translucencyStrength"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Scales how much of the Translucency Map is blended in. "
                             "0 = ignore the map entirely, 1 = full strength."));
        k->setAnimationEnabled(true);
        texPage->addKnob(k); _imp->translucencyStrength = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Displacement Map"));
        k->setName("displacementMapFile");
        k->setHintToolTip(tr("Greyscale height map. Currently applied as BUMP — it perturbs "
                             "shading normals, so surface detail lights correctly but the "
                             "silhouette is unchanged. True displacement needs mesh "
                             "subdivision and is not wired yet. Read as Non-Color."));
        texPage->addKnob(k); _imp->displacementMapFile = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Displacement Scale"));
        k->setName("displacementScale"); k->setDefaultValue(0.1);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Height multiplier, in scene units. Megascans height maps are "
                             "normalised 0-1, so this is how deep the detail reads — start "
                             "small and raise until it looks right."));
        k->setAnimationEnabled(true);
        texPage->addKnob(k); _imp->displacementScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Displacement Midlevel"));
        k->setName("displacementMidlevel"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Height value treated as \"no displacement\". 0.5 matches the "
                             "usual convention where mid-grey is the neutral surface, so "
                             "darker pushes in and brighter pushes out."));
        k->setAnimationEnabled(true);
        texPage->addKnob(k); _imp->displacementMidlevel = k;
    }
}

// ==================== MaterialProvider ====================

void
Material3D::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}

void
Material3D::updateCachedTexture(double time)
{
    // Build into a local and publish an immutable snapshot on every exit path
    // — the previously published texture is shared with concurrent readers
    // (GUI paint / render workers) and must never be mutated in place.
    std::shared_ptr<CachedTexture> tex = std::make_shared<CachedTexture>();
    auto publish = [&]() {
        std::lock_guard<std::mutex> lk(_texMutex);
        _cachedTexture = tex;
    };
    // Render the Diffuse input (input 0) at a preview size, so a geo shape this
    // material is connected to can display the texture in the 3D viewport. Mirrors
    // Card3D::updateCachedTexture. Empty cache => no diffuse texture connected.
    tex->pixels.clear();
    tex->width = 0;
    tex->height = 0;

    EffectInstancePtr diffuseInput = getInput(0);
    if (!diffuseInput) { publish(); return; }

    const int maxSize = 512;
    RectI roiPixel;
    ImagePtr img = getImage(0, time, RenderScale(), ViewIdx(0),
                            NULL, NULL, false, true,
                            eStorageModeRAM, 0, &roiPixel);
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
    tex->pixels.resize(dstW * dstH * 4, 0.0f);

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

double Material3D::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }

double Material3D::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Material3D::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5; }

void
Material3D::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}

double Material3D::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double Material3D::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }

std::string Material3D::getMaterialTextureFile() const
{
    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        if (!_imp->bakedDiffusePath.empty()) return _imp->bakedDiffusePath;
    }
    KnobFilePtr k = _imp->textureFile.lock(); return k ? k->getValue() : std::string();
}

std::string Material3D::getMaterialNormalMapFile() const
{
    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        if (!_imp->bakedNormalPath.empty()) return _imp->bakedNormalPath;
    }
    KnobFilePtr k = _imp->normalMapFile.lock(); return k ? k->getValue() : std::string();
}

double Material3D::getMaterialNormalStrength(double time) const
{ KnobDoublePtr k = _imp->normalStrength.lock(); return k ? k->getValueAtTime(time) : 1.0; }

// Specular + displacement are file-only: Material3D's six image inputs are
// Diffuse/Metallic/Roughness/Emission/Normal/Transmission, so there is no input
// to bake from — unlike the getters above, which prefer a baked input image.
std::string Material3D::getMaterialSpecularMapFile() const
{ KnobFilePtr k = _imp->specularMapFile.lock(); return k ? k->getValue() : std::string(); }

std::string Material3D::getMaterialDisplacementMapFile() const
{ KnobFilePtr k = _imp->displacementMapFile.lock(); return k ? k->getValue() : std::string(); }

std::string Material3D::getMaterialOpacityMapFile() const
{ KnobFilePtr k = _imp->opacityMapFile.lock(); return k ? k->getValue() : std::string(); }

bool Material3D::getMaterialOpacityInvert() const
{ KnobBoolPtr k = _imp->opacityInvert.lock(); return k ? k->getValue() : false; }

std::string Material3D::getMaterialTranslucencyMapFile() const
{ KnobFilePtr k = _imp->translucencyMapFile.lock(); return k ? k->getValue() : std::string(); }

double Material3D::getMaterialTranslucencyStrength(double time) const
{ KnobDoublePtr k = _imp->translucencyStrength.lock(); return k ? k->getValueAtTime(time) : 1.0; }

double Material3D::getMaterialDisplacementScale(double time) const
{ KnobDoublePtr k = _imp->displacementScale.lock(); return k ? k->getValueAtTime(time) : 0.1; }

double Material3D::getMaterialDisplacementMidlevel(double time) const
{ KnobDoublePtr k = _imp->displacementMidlevel.lock(); return k ? k->getValueAtTime(time) : 0.5; }

std::string Material3D::getMaterialRoughnessMapFile() const
{
    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        if (!_imp->bakedRoughnessPath.empty()) return _imp->bakedRoughnessPath;
    }
    KnobFilePtr k = _imp->roughnessMapFile.lock(); return k ? k->getValue() : std::string();
}

std::string Material3D::getMaterialMetallicMapFile() const
{
    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        if (!_imp->bakedMetallicPath.empty()) return _imp->bakedMetallicPath;
    }
    KnobFilePtr k = _imp->metallicMapFile.lock(); return k ? k->getValue() : std::string();
}

std::string Material3D::getMaterialTransmissionMapFile() const
{
    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        if (!_imp->bakedTransmissionPath.empty()) return _imp->bakedTransmissionPath;
    }
    KnobFilePtr k = _imp->transmissionMapFile.lock(); return k ? k->getValue() : std::string();
}

std::string Material3D::getMaterialEmissionMapFile() const
{
    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        if (!_imp->bakedEmissionPath.empty()) return _imp->bakedEmissionPath;
    }
    KnobFilePtr k = _imp->emissionMapFile.lock(); return k ? k->getValue() : std::string();
}

// Return the selected entry's id string directly. Entries are populated from the
// active OCIO config (see buildTextureColorspaceChoices), so the returned name is a
// valid OCIO colorspace (or the synthetic "Raw"/"Linear") that resolves under the
// running config. This is fed to Cycles via materialColorspaceToCycles().
std::string Material3D::getMaterialDiffuseColorspace() const
{
    KnobChoicePtr k = _imp->diffuseColorspace.lock();
    if (!k) {
        return "sRGB";
    }
    const std::string id = k->getActiveEntry().id;
    return id.empty() ? std::string("Linear") : id;
}

std::string Material3D::getMaterialEmissionColorspace() const
{
    KnobChoicePtr k = _imp->emissionColorspace.lock();
    if (!k) {
        return "sRGB";
    }
    const std::string id = k->getActiveEntry().id;
    return id.empty() ? std::string("Linear") : id;
}

// ==================== Input Texture Baking ====================

static bool
renderInputToFile(EffectInstance* input, double time, const std::string& outPath)
{
    // Get region of definition
    RectD rod;
    bool isProjectFormat;
    StatusEnum stat = input->getRegionOfDefinition_public(
        input->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
    if (stat != eStatusOK || rod.isNull()) return false;

    double par = input->getAspectRatio(-1);
    RectI pixelRoI = rod.toPixelEnclosing(0, par);
    int w = pixelRoI.width();
    int h = pixelRoI.height();
    if (w <= 0 || h <= 0) return false;

    // Request RGBA float render
    std::list<ImagePlaneDesc> comps;
    comps.push_back(ImagePlaneDesc::getRGBAComponents());

    std::map<ImagePlaneDesc, ImagePtr> outputPlanes;
    EffectInstance::RenderRoIRetCode ret = input->renderRoI(
        EffectInstance::RenderRoIArgs(
            time, RenderScale::identity, 0, ViewIdx(0),
            false, pixelRoI, rod, comps,
            eImageBitDepthFloat, false, NULL,
            eStorageModeRAM, time),
        &outputPlanes);

    if (ret != EffectInstance::eRenderRoIRetCodeOk || outputPlanes.empty()) return false;

    ImagePtr image = outputPlanes.begin()->second;
    if (!image) return false;

    RectI bounds = image->getBounds();
    int imgW = bounds.width();
    int imgH = bounds.height();

    // Read pixels into a contiguous buffer (flip Y for OIIO — bottom-up to top-down)
    std::vector<float> pixels(imgW * imgH * 4);
    {
        Image::ReadAccess ra(image.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            int srcRow = y - bounds.y1;
            int dstRow = (imgH - 1) - srcRow; // flip Y
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                const float* src = (const float*)ra.pixelAt(x, y);
                int col = x - bounds.x1;
                float* dst = &pixels[(dstRow * imgW + col) * 4];
                if (src) {
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                } else {
                    dst[0] = dst[1] = dst[2] = 0; dst[3] = 1;
                }
            }
        }
    }

    // Write Radiance HDR (.hdr) — simple format, no external library needed
    FILE* fp = fopen(outPath.c_str(), "wb");
    if (!fp) return false;

    fprintf(fp, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", imgH, imgW);

    // Write uncompressed RGBE scanlines
    for (int y = 0; y < imgH; ++y) {
        for (int x = 0; x < imgW; ++x) {
            const float* px = &pixels[(y * imgW + x) * 4];
            float r = px[0], g = px[1], b = px[2];
            float maxVal = r;
            if (g > maxVal) maxVal = g;
            if (b > maxVal) maxVal = b;

            unsigned char rgbe[4];
            if (maxVal < 1e-32f) {
                rgbe[0] = rgbe[1] = rgbe[2] = rgbe[3] = 0;
            } else {
                int e;
                float scale = frexpf(maxVal, &e) * 256.0f / maxVal;
                rgbe[0] = (unsigned char)(r * scale);
                rgbe[1] = (unsigned char)(g * scale);
                rgbe[2] = (unsigned char)(b * scale);
                rgbe[3] = (unsigned char)(e + 128);
            }
            fwrite(rgbe, 4, 1, fp);
        }
    }

    fclose(fp);
    return true;
}

unsigned long long
Material3D::getMaterialInputsHash(double /*time*/) const
{
    // Same mixer CyclesRender uses for its scene hash. The per-input hash is
    // EffectInstance::getHash(), which tracks the whole upstream chain — so a
    // Grade edit three nodes up changes this value.
    auto mix = [](unsigned long long seed, unsigned long long val) -> unsigned long long {
        return seed ^ (val * 0x9e3779b97f4a7c15ULL + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };

    unsigned long long h = 0;
    for (int i = 0; i < 6; ++i) {
        EffectInstancePtr input = getInput(i);
        // Mix the slot index either way, so connecting/disconnecting a map
        // changes the hash instead of two different wirings colliding.
        h = mix(h, (unsigned long long)(i + 1));
        h = mix(h, input ? (unsigned long long)input->getHash() : 0ULL);
    }
    return h;
}

// ==================== Shape img-input bake (MaterialTextureBake.h) ====================

unsigned long long
materialInputChainHash(const EffectInstancePtr& input, int slot)
{
    auto mix = [](unsigned long long seed, unsigned long long val) -> unsigned long long {
        return seed ^ (val * 0x9e3779b97f4a7c15ULL + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    unsigned long long h = mix(0ULL, (unsigned long long)(slot + 1));
    return mix(h, input ? (unsigned long long)input->getHash() : 0ULL);
}

std::string
bakeImageInputToExr(EffectInstance* input, double time, const char* tag, const void* owner)
{
    std::string outPath;
#ifdef NATRON_HAVE_OPENIMAGEIO
    if (!input) return outPath;
    RectD rod;
    bool isProjectFormat = false;
    StatusEnum stat = input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity,
                                                          ViewIdx(0), &rod, &isProjectFormat);
    if (stat != eStatusOK || rod.isNull()) return outPath;
    const double par = input->getAspectRatio(-1);
    RectI pixelRoI = rod.toPixelEnclosing(0, par);
    const int w = pixelRoI.width();
    const int h = pixelRoI.height();
    if (w <= 0 || h <= 0) return outPath;

    std::list<ImagePlaneDesc> comps;
    comps.push_back(ImagePlaneDesc::getRGBAComponents());
    std::map<ImagePlaneDesc, ImagePtr> outputPlanes;
    EffectInstance::RenderRoIRetCode ret = input->renderRoI(
        EffectInstance::RenderRoIArgs(time, RenderScale::identity, 0, ViewIdx(0),
                                      false, pixelRoI, rod, comps,
                                      eImageBitDepthFloat, false, NULL,
                                      eStorageModeRAM, time),
        &outputPlanes);
    if (ret != EffectInstance::eRenderRoIRetCodeOk || outputPlanes.empty()) return outPath;
    ImagePtr image = outputPlanes.begin()->second;
    if (!image) return outPath;

    const RectI bounds = image->getBounds();
    const int imgW = bounds.width();
    const int imgH = bounds.height();
    const int nComps = image->getComponents().getNumComponents();
    // Top-down rows for OIIO; pixels stay premultiplied (Natron's convention)
    // and CyclesRenderer marks the image node's alpha associated.
    std::vector<float> pixels((size_t)imgW * imgH * 4, 0.0f);
    {
        Image::ReadAccess ra(image.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            const int dstRow = (imgH - 1) - (y - bounds.y1);
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                const float* src = (const float*)ra.pixelAt(x, y);
                float* dst = &pixels[((size_t)dstRow * imgW + (x - bounds.x1)) * 4];
                if (src) {
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                    dst[3] = (nComps >= 4) ? src[3] : 1.0f;
                }
            }
        }
    }
    char tmpPath[512];
    const char* tmpDir = std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp";
    std::snprintf(tmpPath, sizeof(tmpPath), "%s/natron_%s_%p.exr", tmpDir, tag, owner);
    std::unique_ptr<OIIO::ImageOutput> out = OIIO::ImageOutput::create(tmpPath);
    if (!out) return outPath;
    OIIO::ImageSpec spec(imgW, imgH, 4, OIIO::TypeDesc::HALF);
    spec.attribute("compression", "zip");
    spec.attribute("oiio:ColorSpace", "linear");
    if (out->open(tmpPath, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) {
        outPath = tmpPath;
    }
    out->close();
#else
    (void)input; (void)time; (void)tag; (void)owner;
#endif
    return outPath;
}

void
bakeShapeImageInput(EffectInstance* owner, int inputIdx, double time, const char* tag, BakedInputTexture& state)
{
    EffectInstancePtr input = owner->getInput(inputIdx);
    if (!input) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.path.clear();
        state.baked = false;
        return;
    }
    const unsigned long long h = materialInputChainHash(input, inputIdx);
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        if (state.baked && state.hash == h && state.time == time) return;
    }
    // Bake unlocked - it is a full input render - and publish under the lock.
    const std::string path = bakeImageInputToExr(input.get(), time, tag, owner);
    std::lock_guard<std::mutex> lk(state.mutex);
    state.path = path;
    state.hash = h;
    state.time = time;
    state.baked = true;
}

void
Material3D::bakeInputTextures(double time)
{
    // Skip the work when nothing upstream moved. Without this every Cycles
    // render re-pulls each connected input and rewrites a multi-MB HDR, even
    // when only the camera nudged.
    const unsigned long long inputsHash = getMaterialInputsHash(time);
    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        if (_imp->hasBakedOnce &&
            _imp->lastBakeInputsHash == inputsHash &&
            _imp->lastBakeTime == time) {
            return;
        }
    }

    // Input mapping: 0=Diffuse, 1=Metallic, 2=Roughness, 3=Emission, 4=Normal,
    // 5=Transmission
    // Bake into locals first, publish under the lock at the end — holding the
    // lock across renderInputToFile (a full render pull) would stall readers,
    // and writing the strings unlocked tears them under concurrent getters.
    std::string local[6];
    const char* names[6] = {"diffuse", "metallic", "roughness", "emission", "normal", "transmission"};

    for (int i = 0; i < 6; ++i) {
        EffectInstancePtr input = getInput(i);
        if (!input) continue;

        // Build temp file path
        char tmpPath[512];
        snprintf(tmpPath, sizeof(tmpPath), "%s/natron_mat3d_%s_%p.hdr",
                 std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp",
                 names[i], (void*)this);

        if (renderInputToFile(input.get(), time, tmpPath)) {
            local[i] = tmpPath;
        }
    }

    {
        std::lock_guard<std::mutex> lk(_imp->bakePathMutex);
        _imp->bakedDiffusePath      = local[0];
        _imp->bakedMetallicPath     = local[1];
        _imp->bakedRoughnessPath    = local[2];
        _imp->bakedEmissionPath     = local[3];
        _imp->bakedNormalPath       = local[4];
        _imp->bakedTransmissionPath = local[5];
        _imp->lastBakeInputsHash    = inputsHash;
        _imp->lastBakeTime          = time;
        _imp->hasBakedOnce          = true;
    }
}

// ==================== Render (dummy) ====================

StatusEnum
Material3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                   ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Material3D::render(const RenderActionArgs& args)
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

#include "moc_Material3D.cpp"
