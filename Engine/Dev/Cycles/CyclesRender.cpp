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

#include "CyclesRender.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "CyclesRenderer.h"

#include "../Scene3D/CameraProvider.h"
#include "../Scene3D/MaterialProvider.h"
#include "../Scene3D/SceneGraph.h"
#include "../Scene3D/Scene3D.h"
#include "../Scene3D/Group3D.h"
#include "../../AppManager.h"
#include "../../AppInstance.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../Project.h"
#include "../Scene3D/Light3D.h"
#include "../Scene3D/Material3D.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct CyclesRenderPrivate
{
    KnobIntWPtr outputWidth, outputHeight;
    KnobIntWPtr samples;
    KnobIntWPtr maxBounces;
    KnobBoolWPtr denoise;
    KnobChoiceWPtr renderMode; // Preview / Final

    // Render cache
    std::vector<float> cachedPixels;
    U64 cachedHash = 0;
    int cachedWidth = 0;
    int cachedHeight = 0;

    // Active render session (for cancel/reuse)
    std::unique_ptr<CyclesRenderer> activeRenderer;
};

CyclesRender::CyclesRender(NodePtr node)
    : EffectInstance(node)
    , _imp(new CyclesRenderPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

CyclesRender::~CyclesRender()
{
}

std::string
CyclesRender::getPluginDescription() const
{
    return tr("Render a 3D scene with Blender's Cycles path tracer.\n\n"
              "Input 0 (bg): Optional background image (composited behind)\n"
              "Input 1 (obj/scn): 3D geometry (Sphere3D, Card3D, Cube3D, etc.)\n"
              "Input 2 (cam): Camera (Camera3D or ReadAlembicCamera)\n\n"
              "Produces a path-traced 2D image with global illumination,\n"
              "soft shadows, and PBR materials.").toStdString();
}

std::string
CyclesRender::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "bg";
        case 1: return "obj/scn";
        case 2: return "cam";
        default: return "";
    }
}

bool
CyclesRender::isInputOptional(int inputNb) const
{
    return (inputNb == 0 || inputNb == 2);
}

void
CyclesRender::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
CyclesRender::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
CyclesRender::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
CyclesRender::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Settings"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Render Mode"));
        k->setName("renderMode");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Preview", "", "Fast preview: 4 samples, quarter resolution"));
        entries.push_back(ChoiceOption("Final", "", "Full quality: uses Samples and Resolution settings"));
        k->populateChoices(entries);
        k->setDefaultValue(0); // Preview by default
        k->setHintToolTip(tr("Preview: quarter resolution, max 4 samples — fast but noisy.\n"
                              "Final: full resolution and sample count — clean but slower."));
        page->addKnob(k);
        _imp->renderMode = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Width"));
        k->setName("outputWidth"); k->setDefaultValue(1920);
        k->setMinimum(1); k->setDisplayMinimum(320); k->setDisplayMaximum(4096);
        k->setHintToolTip(tr("Output image width in pixels. Only used in Final mode."));
        page->addKnob(k); _imp->outputWidth = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Height"));
        k->setName("outputHeight"); k->setDefaultValue(1080);
        k->setMinimum(1); k->setDisplayMinimum(240); k->setDisplayMaximum(4096);
        k->setHintToolTip(tr("Output image height in pixels. Only used in Final mode."));
        page->addKnob(k); _imp->outputHeight = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Samples"));
        k->setName("samples"); k->setDefaultValue(64);
        k->setMinimum(1); k->setDisplayMinimum(1); k->setDisplayMaximum(4096);
        k->setHintToolTip(tr("Number of path tracing samples. Higher = less noise, slower."));
        page->addKnob(k); _imp->samples = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Bounces"));
        k->setName("maxBounces"); k->setDefaultValue(4);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(32);
        k->setHintToolTip(tr("Maximum number of light bounces (diffuse + glossy + transmission)."));
        page->addKnob(k); _imp->maxBounces = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Denoise"));
        k->setName("denoise"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Apply OpenImageDenoise after rendering."));
        page->addKnob(k); _imp->denoise = k;
    }
    {
        // Hidden knob that is always animated — forces Natron's cache to
        // include time in the ImageKey so render() is called every frame.
        // Without this, the cache ignores time for "non-animated" nodes
        // and serves stale renders when upstream lights/geo are keyframed.
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("frameTag"));
        k->setName("frameTag");
        k->setSecret(true);
        k->setAnimationEnabled(true);
        k->setValueAtTime(0.0, 0, ViewSpec::all(), 0);  // time=0, value=0
        k->setValueAtTime(1.0, 1, ViewSpec::all(), 0);  // time=1, value=1
        page->addKnob(k);
    }
}

StatusEnum
CyclesRender::getPreferredMetadata(NodeMetadata& metadata)
{
    // A 3D renderer always depends on the current frame — upstream geometry,
    // lights, and cameras may be animated.  Force the cache to include time
    // in the ImageKey so every frame triggers a fresh render().
    metadata.setIsFrameVarying(true);
    return eStatusOK;
}

StatusEnum
CyclesRender::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                    ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = _imp->outputWidth.lock()->getValue();
    rod->y2 = _imp->outputHeight.lock()->getValue();
    return eStatusOK;
}

StatusEnum
CyclesRender::render(const RenderActionArgs& args)
{
    assert(!args.outputPlanes.empty());
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    // --- Render mode: Preview vs Final ---
    bool isPreview = true;
    {
        KnobChoicePtr modeKnob = _imp->renderMode.lock();
        if (modeKnob) isPreview = (modeKnob->getValue() == 0);
    }

    int outW = _imp->outputWidth.lock()->getValue();
    int outH = _imp->outputHeight.lock()->getValue();
    int numSamples = _imp->samples.lock()->getValue();

    // Preview mode: quarter res, 4 samples, 2 bounces
    int renderW = outW;
    int renderH = outH;
    int renderSamples = numSamples;
    if (isPreview) {
        renderW = std::max(64, outW / 4);
        renderH = std::max(64, outH / 4);
        renderSamples = std::min(numSamples, 4);
    }

    // --- Get camera from input 2 ---
    EffectInstancePtr camEffect = getInput(2);
    CameraProvider* cam = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;

    double camTX = 0, camTY = 2, camTZ = -8;
    double camRX = 0, camRY = 0, camRZ = 0;
    double camFL = 50.0, camHA = 24.576;

    if (cam) {
        cam->getCameraPosition(args.time, camTX, camTY, camTZ, camRX, camRY, camRZ);
        camFL = cam->getCameraFocalLength(args.time);
        camHA = cam->getCameraHAperture(args.time);
    }

    // --- Build scene graph FIRST (needed for hash) ---
    EffectInstancePtr geoEffect = getInput(1);
    if (!geoEffect) return eStatusFailed;

    NodesList allNodes;
    allNodes.push_back(geoEffect->getNode());

    Scene3D* scene3d = dynamic_cast<Scene3D*>(geoEffect.get());
    if (scene3d) {
        for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
            EffectInstancePtr inp = scene3d->getInput(i);
            if (inp) allNodes.push_back(inp->getNode());
        }
    }
    Group3D* group3d = dynamic_cast<Group3D*>(geoEffect.get());
    if (group3d) {
        for (int i = 0; i < GROUP3D_MAX_INPUTS; ++i) {
            EffectInstancePtr inp = group3d->getInput(i);
            if (inp) allNodes.push_back(inp->getNode());
        }
    }

    SceneGraph sceneGraph;
    sceneGraph.rebuild(allNodes, args.time);

    if (sceneGraph.size() == 0) return eStatusFailed;

    // Bake Material3D input textures (Read → Grade → Material3D input pipeline)
    {
        const std::vector<SceneNode>& sceneNodes = sceneGraph.nodes();
        for (size_t i = 0; i < sceneNodes.size(); ++i) {
            NodePtr srcNode = sceneNodes[i].sourceNode.lock();
            if (!srcNode) continue;
            MaterialProvider* matProv = dynamic_cast<MaterialProvider*>(srcNode->getEffectInstance().get());
            if (!matProv) continue;
            // Check for connected Material3D
            MaterialProvider* resolved = matProv;
            if (matProv->hasMaterialInput()) {
                MaterialProvider* connected = matProv->getConnectedMaterial();
                if (connected) resolved = connected;
            }
            Material3D* mat3d = dynamic_cast<Material3D*>(resolved);
            if (mat3d) {
                mat3d->bakeInputTextures(args.time);
            }
        }
    }

    // --- Build scene hash for cache check ---
    // Hash EVERYTHING that affects the render: camera, settings, AND all scene node transforms/params
    U64 sceneHash = 0;
    {
        auto hashCombine = [](U64 seed, U64 val) -> U64 {
            return seed ^ (val * 0x9e3779b97f4a7c15ULL + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
        };
        auto hashFloat = [&](double v) {
            union { double d; U64 u; } conv;
            conv.d = v;
            sceneHash = hashCombine(sceneHash, conv.u);
        };

        // Camera
        hashFloat(camTX); hashFloat(camTY); hashFloat(camTZ);
        hashFloat(camRX); hashFloat(camRY); hashFloat(camRZ);
        hashFloat(camFL); hashFloat(args.time);

        // Settings
        sceneHash = hashCombine(sceneHash, (U64)renderW);
        sceneHash = hashCombine(sceneHash, (U64)renderH);
        sceneHash = hashCombine(sceneHash, (U64)renderSamples);
        sceneHash = hashCombine(sceneHash, isPreview ? 1ULL : 0ULL);

        // Scene graph: hash all node transforms + types + names + count
        const std::vector<SceneNode>& sgNodes = sceneGraph.nodes();
        sceneHash = hashCombine(sceneHash, (U64)sgNodes.size());
        for (size_t i = 0; i < sgNodes.size(); ++i) {
            const SceneNode& sn = sgNodes[i];
            sceneHash = hashCombine(sceneHash, (U64)sn.type);
            sceneHash = hashCombine(sceneHash, (U64)sn.visible);
            // Hash the world matrix (16 floats)
            for (int m = 0; m < 16; ++m) {
                hashFloat((double)sn.worldMatrix[m]);
            }
            // Hash light params if it's a light
            if (sn.type == eSceneNodeLight) {
                NodePtr node = sn.sourceNode.lock();
                if (node) {
                    Light3D* light3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
                    if (light3d) {
                        double ltx, lty, ltz, lr, lg, lb, lint, lexp;
                        light3d->getLightParams(args.time, ltx, lty, ltz, lr, lg, lb, lint, lexp);
                        hashFloat(ltx); hashFloat(lty); hashFloat(ltz);
                        hashFloat(lr); hashFloat(lg); hashFloat(lb);
                        hashFloat(lint); hashFloat(lexp);
                        sceneHash = hashCombine(sceneHash, (U64)light3d->getLightType());
                        // Hash light rotation knobs
                        EffectInstancePtr leff = node->getEffectInstance();
                        if (leff) {
                            KnobIPtr krx = leff->getKnobByName("rotateX");
                            KnobIPtr kry = leff->getKnobByName("rotateY");
                            KnobIPtr krz = leff->getKnobByName("rotateZ");
                            if (krx) hashFloat(dynamic_cast<KnobDouble*>(krx.get())->getValueAtTime(args.time));
                            if (kry) hashFloat(dynamic_cast<KnobDouble*>(kry.get())->getValueAtTime(args.time));
                            if (krz) hashFloat(dynamic_cast<KnobDouble*>(krz.get())->getValueAtTime(args.time));
                        }
                        // Hash spot/area light settings
                        hashFloat(light3d->getSpotAngle(args.time));
                        hashFloat(light3d->getSpotSmooth(args.time));
                        hashFloat(light3d->getAreaSizeU(args.time));
                        hashFloat(light3d->getAreaSizeV(args.time));
                        hashFloat(light3d->getSpread(args.time));
                    }
                }
            }
            // Hash material params if geometry has MaterialProvider
            {
                NodePtr node = sn.sourceNode.lock();
                if (node) {
                    MaterialProvider* matProv = dynamic_cast<MaterialProvider*>(node->getEffectInstance().get());
                    if (matProv) {
                        // Resolve connected material
                        MaterialProvider* mat = matProv;
                        if (mat->hasMaterialInput()) {
                            MaterialProvider* connected = mat->getConnectedMaterial();
                            if (connected) mat = connected;
                        }
                        double mr, mg, mb;
                        mat->getMaterialBaseColor(args.time, mr, mg, mb);
                        hashFloat(mr); hashFloat(mg); hashFloat(mb);
                        hashFloat(mat->getMaterialRoughness(args.time));
                        hashFloat(mat->getMaterialMetallic(args.time));
                        hashFloat(mat->getMaterialSpecular(args.time));
                        hashFloat(mat->getMaterialTransmission(args.time));
                        hashFloat(mat->getMaterialIOR(args.time));
                        double er, eg, eb, es;
                        mat->getMaterialEmission(args.time, er, eg, eb, es);
                        hashFloat(er); hashFloat(eg); hashFloat(eb); hashFloat(es);
                        std::string texFile = mat->getMaterialTextureFile();
                        for (size_t ci = 0; ci < texFile.size(); ++ci) {
                            sceneHash = hashCombine(sceneHash, (U64)texFile[ci]);
                        }
                    }
                }
            }
        }
    }

    // --- Cache check: skip render if nothing changed ---
    {
        FILE* cdbg = fopen("D:/cycles_cache_debug.log", "a");
        if (cdbg) {
            fprintf(cdbg, "render() time=%.1f hash=%llu cachedHash=%llu match=%d\n",
                    args.time, (unsigned long long)sceneHash, (unsigned long long)_imp->cachedHash,
                    (int)(sceneHash == _imp->cachedHash));
            fclose(cdbg);
        }
    }
    if (sceneHash == _imp->cachedHash &&
        !_imp->cachedPixels.empty() &&
        _imp->cachedWidth == renderW &&
        _imp->cachedHeight == renderH) {
        // Use cached pixels — no re-render needed
    } else {
        // --- Cancel any active render ---
        if (_imp->activeRenderer) {
            _imp->activeRenderer->cancelRender();
            _imp->activeRenderer.reset();
        }

        // --- Render with Cycles (scene graph already built above) ---
        _imp->activeRenderer = std::make_unique<CyclesRenderer>();
        bool ok = _imp->activeRenderer->renderToBufferWithCamera(
            sceneGraph,
            camTX, camTY, camTZ,
            camRX, camRY, camRZ,
            camFL, camHA,
            _imp->cachedPixels, renderW, renderH, renderSamples,
            args.time);

        if (!ok || _imp->cachedPixels.empty()) {
            _imp->activeRenderer.reset();
            return eStatusFailed;
        }

        _imp->cachedHash = sceneHash;
        _imp->cachedWidth = renderW;
        _imp->cachedHeight = renderH;
    }

    // --- Scale pixels to output resolution if preview ---
    const std::vector<float>& srcPixels = _imp->cachedPixels;
    int srcW = _imp->cachedWidth;
    int srcH = _imp->cachedHeight;

    // --- Copy pixels to output image (with nearest-neighbor upscale for preview) ---
    RectI outBounds = outImg->getBounds();
    {
        Image::WriteAccess wa(outImg.get());

        // Optional: composite over background input
        ImagePtr bgImg;
        EffectInstancePtr bgEffect = getInput(0);
        if (bgEffect) {
            RectI bgRoi;
            bgImg = getImage(0, args.time, RenderScale(), args.view,
                             NULL, NULL, false, true,
                             eStorageModeRAM, 0, &bgRoi);
        }
        Image::ReadAccess* bgRa = bgImg ? new Image::ReadAccess(bgImg.get()) : NULL;
        RectI bgBounds;
        if (bgImg) bgBounds = bgImg->getBounds();

        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;

                int fbX = x - outBounds.x1;
                int fbY = y - outBounds.y1;

                // Scale from output coords to render coords (nearest neighbor)
                int srcX = (srcW == outW) ? fbX : (fbX * srcW / outW);
                int srcY = (srcH == outH) ? fbY : (fbY * srcH / outH);
                srcX = std::min(srcX, srcW - 1);
                srcY = std::min(srcY, srcH - 1);

                float fgR = 0, fgG = 0, fgB = 0, fgA = 0;
                if (srcX >= 0 && srcY >= 0) {
                    int idx = (srcY * srcW + srcX) * 4;
                    fgR = srcPixels[idx + 0];
                    fgG = srcPixels[idx + 1];
                    fgB = srcPixels[idx + 2];
                    fgA = srcPixels[idx + 3];
                }

                // Over composite: fg over bg
                if (bgRa && bgBounds.contains(x, y)) {
                    const float* bgPix = (const float*)bgRa->pixelAt(x, y);
                    if (bgPix) {
                        dst[0] = fgR + bgPix[0] * (1.0f - fgA);
                        dst[1] = fgG + bgPix[1] * (1.0f - fgA);
                        dst[2] = fgB + bgPix[2] * (1.0f - fgA);
                        dst[3] = fgA + bgPix[3] * (1.0f - fgA);
                        continue;
                    }
                }

                dst[0] = fgR;
                dst[1] = fgG;
                dst[2] = fgB;
                dst[3] = fgA;
            }
        }

        delete bgRa;
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
