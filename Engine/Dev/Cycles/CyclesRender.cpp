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
#include <map>
#include <set>
#include <vector>

#include "CyclesRenderer.h"
#include "CyclesRenderSettings.h"
#include "CyclesPassRender.h"

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
#include "../../Format.h"
#include "../../Project.h"
#include "../Scene3D/Light3D.h"
#include "../Scene3D/Material3D.h"
#include "../Scene3D/RenderPass.h"
#include "../Scene3D/Volume3D.h"
#include "../Scene3D/ReadVDB.h"
#include "../../KnobFile.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct CyclesRenderPrivate
{
    KnobIntWPtr samples;
    KnobIntWPtr maxBounces;
    KnobIntWPtr diffuseBounces;
    KnobIntWPtr glossyBounces;
    KnobIntWPtr transmissionBounces;
    KnobBoolWPtr denoise;
    KnobBoolWPtr previewMode; // half-res render, upscaled to full

    // Explicit output size — does NOT come from upstream input format.
    // (Mirror of ScanlineRender's outputWidth/outputHeight. Required: without
    // these, getOutputFormat() inherits transitively from input 1 (geometry
    // chain), so attaching a texture Read to a Sphere3D silently dictated the
    // Cycles render canvas — the 2K Earth daymap bug.)
    KnobIntWPtr outputWidth, outputHeight;
    KnobButtonWPtr syncToProject; // copies project default format → width/height

    // Depth of Field (render-side params; F-Stop lives on Camera3D Lens tab)
    KnobBoolWPtr dofEnabled;
    KnobDoubleWPtr focusDistance;
    KnobIntWPtr bokehBlades;
    KnobDoubleWPtr bladeRotation;

    // Motion blur
    KnobBoolWPtr motionBlur;
    KnobDoubleWPtr shutterTime;
    KnobChoiceWPtr shutterPosition;

    // AOV enable knobs
    KnobBoolWPtr aovDiffDir, aovDiffInd, aovDiffCol, aovDiffuse, aovTrans;
    KnobBoolWPtr aovGlossDir, aovGlossInd, aovGlossCol, aovGlossy;
    KnobBoolWPtr aovEmission, aovEnv, aovAO;
    KnobBoolWPtr aovNormal, aovDepth, aovUV, aovMist;

    // Focus helper
    KnobChoiceWPtr focusObject;
    KnobButtonWPtr refreshFocusBtn;
    KnobButtonWPtr setFocusBtn;

    // EXR output
    KnobFileWPtr exrOutputPath;
    KnobButtonWPtr saveExrBtn;

    // Forces a metadata + cache refresh so a changed AOV pass set re-publishes
    // and re-renders without having to scrub the timeline.
    KnobButtonWPtr refreshPassesBtn;

    // Render cache (multi-pass)
    std::map<std::string, std::vector<float>> cachedPassBuffers;
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
        case 3: return "settings";
        case 4: return "holdout";
        default: return "";
    }
}

bool
CyclesRender::isInputOptional(int inputNb) const
{
    // Everything except the obj/scn input (1) is optional.
    return (inputNb != 1);
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
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Samples"));
        k->setName("samples"); k->setDefaultValue(6);
        k->setMinimum(1); k->setMaximum(8192);
        k->setDisplayMinimum(1); k->setDisplayMaximum(4096);
        k->setHintToolTip(tr("Number of path tracing samples. Higher = less noise, slower."));
        page->addKnob(k); _imp->samples = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Bounces"));
        k->setName("maxBounces"); k->setDefaultValue(8);
        k->setMinimum(0); k->setMaximum(128);
        k->setDisplayMinimum(0); k->setDisplayMaximum(25);
        k->setHintToolTip(tr("Maximum total light bounces (all types combined)."));
        page->addKnob(k); _imp->maxBounces = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Diffuse Bounces"));
        k->setName("diffuseBounces"); k->setDefaultValue(4);
        k->setMinimum(0); k->setMaximum(128);
        k->setDisplayMinimum(0); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Maximum number of diffuse reflection bounces (0 = diffuse off)."));
        page->addKnob(k); _imp->diffuseBounces = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Glossy Bounces"));
        k->setName("glossyBounces"); k->setDefaultValue(4);
        k->setMinimum(0); k->setMaximum(128);
        k->setDisplayMinimum(0); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Maximum number of glossy/specular reflection bounces (0 = glossy off)."));
        page->addKnob(k); _imp->glossyBounces = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Transmission Bounces"));
        k->setName("transmissionBounces"); k->setDefaultValue(8);
        k->setMinimum(0); k->setMaximum(128);
        k->setDisplayMinimum(0); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Maximum number of transmission/refraction bounces (glass, water)."));
        page->addKnob(k); _imp->transmissionBounces = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Denoise"));
        k->setName("denoise"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Apply OpenImageDenoise after rendering."));
        page->addKnob(k); _imp->denoise = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Preview (half res)"));
        k->setName("previewMode"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Render at half resolution and upscale. Faster for interactive work."));
        page->addKnob(k); _imp->previewMode = k;
    }

    // Output size — explicit knobs. Cycles no longer inherits resolution from
    // upstream input metadata (which used to leak texture-Read sizes through
    // the geometry chain). Set these to match your plate, or drop a Reformat
    // upstream and match here.
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Width"));
        k->setName("outputWidth"); k->setDefaultValue(1920);
        k->setMinimum(1); k->setDisplayMinimum(320); k->setDisplayMaximum(4096);
        k->setHintToolTip(tr("Render output width in pixels. Independent of upstream input format."));
        page->addKnob(k); _imp->outputWidth = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Height"));
        k->setName("outputHeight"); k->setDefaultValue(1080);
        k->setMinimum(1); k->setDisplayMinimum(240); k->setDisplayMaximum(4096);
        k->setHintToolTip(tr("Render output height in pixels. Independent of upstream input format."));
        page->addKnob(k); _imp->outputHeight = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Sync to Project"));
        k->setName("syncToProject");
        k->setHintToolTip(tr("Copy the current project default format's width and "
                             "height into the Width/Height knobs above."));
        page->addKnob(k); _imp->syncToProject = k;
    }

    // AOV Passes page
    KnobPagePtr aovPage = AppManager::createKnob<KnobPage>(this, tr("AOV Passes"));
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Refresh Passes"));
        k->setName("refreshPasses");
        k->setHintToolTip(tr("Re-publish the output planes and force a re-render. Use this if "
                             "the viewer / downstream doesn't pick up newly toggled AOV passes "
                             "without scrubbing the timeline."));
        aovPage->addKnob(k); _imp->refreshPassesBtn = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse Direct")); k->setName("aovDiffDir"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovDiffDir = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse Indirect")); k->setName("aovDiffInd"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovDiffInd = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse Color")); k->setName("aovDiffCol"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovDiffCol = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse")); k->setName("aovDiffuse"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Combined diffuse contribution as it appears in the beauty: (Diffuse Direct + "
                             "Diffuse Indirect) \xc3\x97 Diffuse Color. Synthesized from the sub-passes."));
        aovPage->addKnob(k); _imp->aovDiffuse = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy Direct")); k->setName("aovGlossDir"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovGlossDir = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy Indirect")); k->setName("aovGlossInd"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovGlossInd = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy Color")); k->setName("aovGlossCol"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovGlossCol = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy")); k->setName("aovGlossy"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Combined glossy contribution as it appears in the beauty: (Glossy Direct + "
                             "Glossy Indirect) \xc3\x97 Glossy Color. Synthesized from the sub-passes, so it's "
                             "directly viewable without rebuilding it in comp."));
        aovPage->addKnob(k); _imp->aovGlossy = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Transmission")); k->setName("aovTrans"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Combined transmission contribution as it appears in the beauty: (Transmission "
                             "Direct + Transmission Indirect) \xc3\x97 Transmission Color. Synthesized from the "
                             "sub-passes (glass / refraction)."));
        aovPage->addKnob(k); _imp->aovTrans = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Emission")); k->setName("aovEmission"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovEmission = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Environment")); k->setName("aovEnv"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovEnv = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Ambient Occlusion")); k->setName("aovAO"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovAO = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Normal")); k->setName("aovNormal"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovNormal = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Depth")); k->setName("aovDepth"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovDepth = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("UV")); k->setName("aovUV"); k->setDefaultValue(false);
        aovPage->addKnob(k); _imp->aovUV = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Mist"));
        k->setName("aovMist"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Distance-attenuated mist factor [0,1]. "
            "Uses Cycles' default Film mist range; expose Mist Start / Depth / "
            "Falloff knobs later if tuning is needed."));
        aovPage->addKnob(k); _imp->aovMist = k;
    }

    // --- Depth of Field tab ---
    KnobPagePtr dofPage = AppManager::createKnob<KnobPage>(this, tr("Depth of Field"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Enable DOF"));
        k->setName("dofEnabled"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Enable depth of field. F-Stop is set on Camera3D's Lens tab."));
        dofPage->addKnob(k); _imp->dofEnabled = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focus Distance"));
        k->setName("focusDistance"); k->setDefaultValue(10.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(1000.0);
        k->setHintToolTip(tr("Distance from the camera where objects are in perfect focus (world units)."));
        k->setAnimationEnabled(true);
        dofPage->addKnob(k); _imp->focusDistance = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Bokeh Blades"));
        k->setName("bokehBlades"); k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Number of aperture blades. 0 = circular bokeh, 3+ = polygonal bokeh."));
        dofPage->addKnob(k); _imp->bokehBlades = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Blade Rotation"));
        k->setName("bladeRotation"); k->setDefaultValue(0.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(360.0);
        k->setHintToolTip(tr("Rotation of the aperture blades in degrees."));
        k->setAnimationEnabled(true);
        dofPage->addKnob(k); _imp->bladeRotation = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Focus Object"));
        k->setName("focusObject");
        k->setHintToolTip(tr("Select a scene object. Click 'Set Focus' to set Focus Distance to this object."));
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("(none)", "", "No object selected"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        dofPage->addKnob(k); _imp->focusObject = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Refresh Objects"));
        k->setName("refreshFocusObjects");
        k->setHintToolTip(tr("Re-scan the connected Scene for objects."));
        dofPage->addKnob(k); _imp->refreshFocusBtn = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Set Focus"));
        k->setName("setFocus");
        k->setHintToolTip(tr("Compute the distance from the camera to the selected object and set Focus Distance."));
        dofPage->addKnob(k); _imp->setFocusBtn = k;
    }

    // --- Motion Blur tab ---
    KnobPagePtr mbPage = AppManager::createKnob<KnobPage>(this, tr("Motion Blur"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Enable Motion Blur"));
        k->setName("motionBlur"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Enable motion blur. Animated objects and particles will blur based on shutter time."));
        mbPage->addKnob(k); _imp->motionBlur = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shutter Time"));
        k->setName("shutterTime"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Shutter duration in frames. 0.5 = half frame of blur (common). 1.0 = full frame."));
        k->setAnimationEnabled(true);
        mbPage->addKnob(k); _imp->shutterTime = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Shutter Position"));
        k->setName("shutterPosition");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Start", "", "Shutter opens at current frame"));
        entries.push_back(ChoiceOption("Center", "", "Shutter centered on current frame (most common)"));
        entries.push_back(ChoiceOption("End", "", "Shutter closes at current frame"));
        k->populateChoices(entries);
        k->setDefaultValue(1);
        mbPage->addKnob(k); _imp->shutterPosition = k;
    }

    KnobPagePtr outPage = AppManager::createKnob<KnobPage>(this, tr("Output"));
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("EXR Output Path"));
        k->setName("exrOutputPath");
        k->setHintToolTip(tr("Path to save multi-layer EXR with all enabled AOV passes."));
        k->setDefaultValue("");
        outPage->addKnob(k); _imp->exrOutputPath = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Save Multi-Layer EXR"));
        k->setName("saveExr");
        k->setHintToolTip(tr("Render and save all enabled passes to a multi-layer EXR file."));
        outPage->addKnob(k); _imp->saveExrBtn = k;
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

    // Publish the explicit knob-driven output format. Without this override,
    // EffectInstance falls back to the first-optional-input format, which
    // transitively walks through the geometry chain and picks up texture-Read
    // dimensions — making any image plugged into a Sphere3D/Cube3D etc.
    // silently determine the Cycles render canvas.
    int w = _imp->outputWidth.lock()->getValue();
    int h = _imp->outputHeight.lock()->getValue();
    RectI fmt;
    fmt.x1 = 0; fmt.y1 = 0;
    fmt.x2 = std::max(1, w);
    fmt.y2 = std::max(1, h);
    metadata.setOutputFormat(fmt);

    return eStatusOK;
}

// Helper: collect enabled AOV pass names from knobs
static std::vector<std::string>
getEnabledPasses(const CyclesRenderPrivate* imp)
{
    std::vector<std::string> passes;
    passes.push_back("Combined"); // always

    auto check = [&](const KnobBoolWPtr& knob, const char* name) {
        KnobBoolPtr k = knob.lock();
        bool valid = (k != nullptr);
        bool val = valid ? k->getValue() : false;
        if (valid && val) passes.push_back(name);
    };
    check(imp->aovDiffDir,  "DiffDir");
    check(imp->aovDiffInd,  "DiffInd");
    check(imp->aovDiffCol,  "DiffCol");
    check(imp->aovDiffuse,  "Diffuse");
    check(imp->aovGlossDir, "GlossDir");
    check(imp->aovGlossInd, "GlossInd");
    check(imp->aovGlossCol, "GlossCol");
    check(imp->aovGlossy,   "Glossy");
    check(imp->aovTrans,    "Transmission");
    check(imp->aovEmission, "Emit");
    check(imp->aovEnv,      "Env");
    check(imp->aovAO,       "AO");
    check(imp->aovNormal,   "Normal");
    check(imp->aovDepth,    "Depth");
    check(imp->aovUV,       "UV");
    check(imp->aovMist,     "Mist");
    return passes;
}

// Helper: map pass name to an ImagePlaneDesc
static ImagePlaneDesc
passNameToPlane(const std::string& name)
{
    if (name == "Combined") return ImagePlaneDesc::getRGBAComponents();

    // 3-channel RGB passes
    static const char* rgb3[] = {"R", "G", "B"};
    if (name == "DiffDir")  return ImagePlaneDesc("DiffuseDirect",  "Diffuse Direct",  "", rgb3, 3);
    if (name == "DiffInd")  return ImagePlaneDesc("DiffuseIndirect","Diffuse Indirect", "", rgb3, 3);
    if (name == "DiffCol")  return ImagePlaneDesc("DiffuseColor",   "Diffuse Color",   "", rgb3, 3);
    if (name == "Diffuse")  return ImagePlaneDesc("Diffuse",        "Diffuse",         "", rgb3, 3);
    if (name == "GlossDir") return ImagePlaneDesc("GlossyDirect",   "Glossy Direct",   "", rgb3, 3);
    if (name == "GlossInd") return ImagePlaneDesc("GlossyIndirect", "Glossy Indirect",  "", rgb3, 3);
    if (name == "GlossCol") return ImagePlaneDesc("GlossyColor",    "Glossy Color",    "", rgb3, 3);
    if (name == "Glossy")   return ImagePlaneDesc("Glossy",         "Glossy",          "", rgb3, 3);
    if (name == "Transmission") return ImagePlaneDesc("Transmission", "Transmission",  "", rgb3, 3);
    if (name == "Emit")     return ImagePlaneDesc("Emission",       "Emission",        "", rgb3, 3);
    if (name == "Env")      return ImagePlaneDesc("Environment",    "Environment",     "", rgb3, 3);
    if (name == "Normal")   return ImagePlaneDesc("Normal",         "Normal",          "", rgb3, 3);
    if (name == "UV")       return ImagePlaneDesc("UV",             "UV",              "", rgb3, 3);

    // Single-value passes broadcast to 3-channel R/G/B so Natron's viewer
    // displays them as grayscale directly. (1-channel "A" planes render
    // black in the viewer's default RGB mode — display trap not data loss.)
    if (name == "AO")       return ImagePlaneDesc("AO",    "Ambient Occlusion", "", rgb3, 3);
    if (name == "Depth")    return ImagePlaneDesc("Depth", "Depth",             "", rgb3, 3);
    if (name == "Mist")     return ImagePlaneDesc("Mist",  "Mist",              "", rgb3, 3);

    // Light group passes (Combined_<name>)
    if (name.substr(0, 9) == "Combined_") {
        std::string grpName = name.substr(9);
        return ImagePlaneDesc("LightGroup_" + grpName, "LightGroup " + grpName, "", rgb3, 3);
    }

    return ImagePlaneDesc::getRGBAComponents();
}

void
CyclesRender::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                              EffectInstance::ComponentsNeededMap* comps,
                                              double* passThroughTime, int* passThroughView,
                                              int* passThroughInput)
{
    // Declare output planes for all enabled AOVs
    std::list<ImagePlaneDesc> produced;
    std::vector<std::string> passes = getEnabledPasses(_imp.get());
    for (const std::string& p : passes) {
        produced.push_back(passNameToPlane(p));
    }
    printf("[CyclesRender] getComponentsNeededAndProduced: declaring %d planes\n", (int)produced.size());
    for (const auto& pl : produced) {
        printf("[CyclesRender]   plane: id='%s' nComps=%d\n", pl.getPlaneID().c_str(), pl.getNumComponents());
    }

    // Scan scene for light groups so they appear as output planes
    {
        AppInstancePtr app = getApp();
        if (app) {
            ProjectPtr project = app->getProject();
            if (project) {
                NodesList allNodes;
                project->getNodes_recursive(allNodes, true);
                std::set<std::string> groups;
                for (const NodePtr& n : allNodes) {
                    if (!n || !n->isActivated()) continue;
                    Light3D* light = dynamic_cast<Light3D*>(n->getEffectInstance().get());
                    if (!light) continue;
                    std::string grp = light->getLightGroup();
                    if (!grp.empty()) groups.insert(grp);
                }
                for (const std::string& g : groups) {
                    produced.push_back(passNameToPlane("Combined_" + g));
                }
            }
        }
    }

    (*comps)[-1] = produced;

    // Pass through background input for non-rendered planes
    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0;
}

StatusEnum
CyclesRender::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                    ViewIdx /*view*/, RectD* rod)
{
    // RoD from explicit knobs — NOT from upstream input format (see knob comment).
    int w = _imp->outputWidth.lock()->getValue();
    int h = _imp->outputHeight.lock()->getValue();
    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = std::max(1, w);
    rod->y2 = std::max(1, h);
    return eStatusOK;
}

StatusEnum
CyclesRender::render(const RenderActionArgs& args)
{
    assert(!args.outputPlanes.empty());
    printf("[CyclesRender] render() called with %d output planes:\n", (int)args.outputPlanes.size());
    for (const auto& pp : args.outputPlanes) {
        printf("[CyclesRender]   plane: id='%s' label='%s' nComps=%d\n",
               pp.first.getPlaneID().c_str(), pp.first.getPlaneLabel().c_str(),
               pp.first.getNumComponents());
    }
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    // Resolution from explicit knobs. Earlier versions used getOutputFormat()
    // which transitively inherited from input 1's geometry chain — meaning a
    // 2K texture on a Sphere3D dictated the render canvas. Fixed by the knobs.
    int outW = _imp->outputWidth.lock()->getValue();
    int outH = _imp->outputHeight.lock()->getValue();
    if (outW <= 0) outW = 1920;
    if (outH <= 0) outH = 1080;

    // Preview: render at half res, upscale to full RoD for display
    bool isPreview = false;
    {
        KnobBoolPtr pk = _imp->previewMode.lock();
        if (pk) isPreview = pk->getValue();
    }
    // --- Settings provider (input 3, optional) ---
    // When connected, all Render / Integrator / DOF / Motion Blur values are
    // pulled from the provider; local knobs are ignored (and hidden via
    // onInputChanged). The local-knob branches below remain so disconnecting
    // the Settings input falls cleanly back to standalone behavior.
    const CyclesRenderSettings* settings = nullptr;
    {
        EffectInstancePtr settingsEffect = getInput(3);
        if (settingsEffect) {
            settings = dynamic_cast<const CyclesRenderSettings*>(settingsEffect.get());
        }
    }

    int renderW = isPreview ? std::max(64, outW / 2) : outW;
    int renderH = isPreview ? std::max(64, outH / 2) : outH;
    int renderSamples = settings
        ? settings->getSamples(args.time)
        : _imp->samples.lock()->getValue();

    // --- Get camera from input 2 ---
    EffectInstancePtr camEffect = getInput(2);
    CameraProvider* cam = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;

    double camTX = 0, camTY = 2, camTZ = -8;
    double camRX = 0, camRY = 0, camRZ = 0;
    double camFL = 50.0, camHA = 24.576, camVA = 18.672;

    if (cam) {
        cam->getCameraPosition(args.time, camTX, camTY, camTZ, camRX, camRY, camRZ);
        camFL = cam->getCameraFocalLength(args.time);
        camHA = cam->getCameraHAperture(args.time);
        camVA = cam->getCameraVAperture(args.time);
    }

    // --- DOF params (Enable/Focus/Bokeh from Settings or local knobs;
    //     F-Stop always from Camera3D since it's a lens property) ---
    CyclesRenderer::DOFParams dofParams;
    {
        bool dofOn = settings
            ? settings->getDOFEnabled(args.time)
            : (_imp->dofEnabled.lock() && _imp->dofEnabled.lock()->getValue());
        if (dofOn) {
            dofParams.enabled = true;
            double fstop = cam ? cam->getCameraFStop(args.time) : 2.8;
            if (fstop < 0.1) fstop = 0.1;
            dofParams.apertureSize = (float)(camFL / (2.0 * fstop) / 1000.0); // mm to meters
            double focusDist = settings
                ? settings->getFocusDistance(args.time)
                : _imp->focusDistance.lock()->getValueAtTime(args.time);
            int blades = settings
                ? settings->getBokehBlades(args.time)
                : (_imp->bokehBlades.lock() ? _imp->bokehBlades.lock()->getValue() : 0);
            double rotDeg = settings
                ? settings->getBladeRotation(args.time)
                : _imp->bladeRotation.lock()->getValueAtTime(args.time);
            dofParams.focusDistance = (float)focusDist;
            dofParams.blades        = blades;
            dofParams.bladeRotation = (float)(rotDeg * M_PI / 180.0);
        }
    }

    // --- Motion Blur params ---
    CyclesRenderer::MotionBlurParams mbParams;
    {
        bool mbOn = settings
            ? settings->getMotionBlurEnabled(args.time)
            : (_imp->motionBlur.lock() && _imp->motionBlur.lock()->getValue());
        if (mbOn) {
            mbParams.enabled = true;
            mbParams.shutterTime = (float)(settings
                ? settings->getShutterTime(args.time)
                : _imp->shutterTime.lock()->getValueAtTime(args.time));
            mbParams.shutterPosition = settings
                ? settings->getShutterPosition(args.time)
                : (_imp->shutterPosition.lock() ? _imp->shutterPosition.lock()->getValue() : 1);
        }
    }

    // --- Integrator params (Settings node wins; else local knobs).
    //     Resolved up here so the hash block below can fold them into the
    //     cache key — previously they were resolved inside the cache-miss
    //     branch and so changing maxBounces / diffuseBounces / glossyBounces
    //     / transmissionBounces silently failed to invalidate the cache. ---
    CyclesRenderer::IntegratorParams integParams;
    if (settings) {
        integParams.maxBounces          = settings->getMaxBounces(args.time);
        integParams.diffuseBounces      = settings->getDiffuseBounces(args.time);
        integParams.glossyBounces       = settings->getGlossyBounces(args.time);
        integParams.transmissionBounces = settings->getTransmissionBounces(args.time);
    } else {
        KnobIntPtr k;
        k = _imp->maxBounces.lock();           if (k) integParams.maxBounces          = k->getValue();
        k = _imp->diffuseBounces.lock();       if (k) integParams.diffuseBounces      = k->getValue();
        k = _imp->glossyBounces.lock();        if (k) integParams.glossyBounces       = k->getValue();
        k = _imp->transmissionBounces.lock();  if (k) integParams.transmissionBounces = k->getValue();
    }

    // Collect enabled passes early (needed for hash).
    std::vector<std::string> requestedPasses = getEnabledPasses(_imp.get());

    // --- Build the CyclesPassRequest and hand it to the shared helper
    //     for scene graph + Material3D bake + camera resolve. The helper
    //     also re-walks input 1 / input 2 internally; the local camera
    //     resolution above runs in parallel so the apertureSize calc has
    //     access to a CameraProvider* before this prepare call. ---
    CyclesPassRequest req;
    req.time            = args.time;
    req.view            = ViewIdx(0);
    req.width           = renderW;
    req.height          = renderH;
    req.samples         = renderSamples;
    req.requestedPasses = requestedPasses;
    req.transparentBg   = false;
    if (dofParams.enabled) req.dof = &dofParams;
    if (mbParams.enabled)  req.mb  = &mbParams;
    req.integrator = &integParams;

    CyclesPassPrepared prepared;
    {
        std::string prepErr;
        if (!prepareCyclesPasses(this, req, prepared, prepErr)) {
            // Match the historical failure mode: silent eStatusFailed.
            // The helper's prepErr describes the reason (missing input 1,
            // empty scene graph, etc.) but CyclesRender doesn't surface
            // it through a persistent message — preserve that for now.
            return eStatusFailed;
        }
    }
    // Aliases so the hash block below reads naturally (the local sceneGraph
    // / renderPass names predate the helper split).
    const SceneGraph& sceneGraph = prepared.sceneGraph;
    RenderPass* renderPass = prepared.renderPass;

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
        hashFloat(camFL); hashFloat(camHA); hashFloat(camVA);
        hashFloat(args.time);

        // Settings
        sceneHash = hashCombine(sceneHash, (U64)renderW);
        sceneHash = hashCombine(sceneHash, (U64)renderH);
        sceneHash = hashCombine(sceneHash, (U64)renderSamples);
        sceneHash = hashCombine(sceneHash, isPreview ? 1ULL : 0ULL);

        // Hash enabled AOV passes so changing checkboxes triggers re-render
        sceneHash = hashCombine(sceneHash, (U64)requestedPasses.size());
        for (const auto& pn : requestedPasses) {
            for (size_t ci = 0; ci < pn.size(); ++ci) {
                sceneHash = hashCombine(sceneHash, (U64)pn[ci]);
            }
        }

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
            // Hash material params. Mirror the renderer's per-node precedence:
            // a per-part override (sn.materialNode, from a GeoMaterialOverride)
            // wins over the source node's own material — so editing the override's
            // surface list OR its material busts the render cache and re-renders.
            {
                NodePtr matOverrideNode = sn.materialNode.lock(); // per-part override
                NodePtr srcNode = sn.sourceNode.lock();
                // Whether an override applies to this node — toggling a surface in
                // or out of the override flips this even if the two materials would
                // otherwise hash identically.
                sceneHash = hashCombine(sceneHash, matOverrideNode ? 1ULL : 0ULL);
                NodePtr node = matOverrideNode ? matOverrideNode : srcNode;
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
            // Hash volume params if it's a volume
            if (sn.type == eSceneNodeVolume) {
                NodePtr node = sn.sourceNode.lock();
                if (node) {
                    Volume3D* vol3d = dynamic_cast<Volume3D*>(node->getEffectInstance().get());
                    if (vol3d) {
                        Volume3D::VolumeParams vp = vol3d->getVolumeParams(args.time);
                        hashFloat(vp.density); hashFloat(vp.colorR); hashFloat(vp.colorG); hashFloat(vp.colorB);
                        sceneHash = hashCombine(sceneHash, (U64)vp.resolution);
                        sceneHash = hashCombine(sceneHash, (U64)vp.volumeType);
                        hashFloat(vp.noiseScale); hashFloat(vp.noiseDetail);
                    }
                    ReadVDB* readVdb = dynamic_cast<ReadVDB*>(node->getEffectInstance().get());
                    if (readVdb) {
                        // Hash the file path and transform
                        KnobIPtr fk = node->getEffectInstance()->getKnobByName("filePath");
                        if (fk) {
                            std::string fp = dynamic_cast<KnobFile*>(fk.get())->getValue();
                            for (size_t ci = 0; ci < fp.size(); ++ci)
                                sceneHash = hashCombine(sceneHash, (U64)fp[ci]);
                        }
                    }
                }
            }
        }

        // Hash RenderPass visibility state
        if (renderPass) {
            renderPass->refreshObjectLists();
            std::map<std::string, ObjectVisibility> visMap = renderPass->getObjectVisibilityMap();
            for (const auto& entry : visMap) {
                for (size_t ci = 0; ci < entry.first.size(); ++ci) {
                    sceneHash = hashCombine(sceneHash, (U64)entry.first[ci]);
                }
                sceneHash = hashCombine(sceneHash, (U64)entry.second.rayVisibility);
                sceneHash = hashCombine(sceneHash, entry.second.isHoldout ? 1ULL : 0ULL);
                sceneHash = hashCombine(sceneHash, entry.second.isShadowCatcher ? 1ULL : 0ULL);
                sceneHash = hashCombine(sceneHash, entry.second.isExcluded ? 1ULL : 0ULL);
            }
            std::set<std::string> activeLightSet = renderPass->getActiveLights();
            for (const auto& ln : activeLightSet) {
                for (size_t ci = 0; ci < ln.size(); ++ci) {
                    sceneHash = hashCombine(sceneHash, (U64)ln[ci]);
                }
            }
            // Hash pass name too
            std::string passName = renderPass->getPassName();
            for (size_t ci = 0; ci < passName.size(); ++ci) {
                sceneHash = hashCombine(sceneHash, (U64)passName[ci]);
            }
        }

        // Hash DOF params
        if (dofParams.enabled) {
            sceneHash = hashCombine(sceneHash, 0xD0FULL);
            hashFloat(dofParams.apertureSize);
            hashFloat(dofParams.focusDistance);
            sceneHash = hashCombine(sceneHash, (U64)dofParams.blades);
            hashFloat(dofParams.bladeRotation);
        }

        // Hash motion blur params
        if (mbParams.enabled) {
            sceneHash = hashCombine(sceneHash, 0xBB11ULL);
            hashFloat(mbParams.shutterTime);
            sceneHash = hashCombine(sceneHash, (U64)mbParams.shutterPosition);
        }

        // Hash integrator params — changing bounce counts now invalidates
        // the cache (previously a latent bug; harmless when the values
        // never change, surprising when they do).
        sceneHash = hashCombine(sceneHash, 0x12FAULL);
        sceneHash = hashCombine(sceneHash, (U64)integParams.maxBounces);
        sceneHash = hashCombine(sceneHash, (U64)integParams.diffuseBounces);
        sceneHash = hashCombine(sceneHash, (U64)integParams.glossyBounces);
        sceneHash = hashCombine(sceneHash, (U64)integParams.transmissionBounces);
    }

    // --- Cache check: skip render if nothing changed ---
    if (sceneHash == _imp->cachedHash &&
        !_imp->cachedPassBuffers.empty() &&
        _imp->cachedWidth == renderW &&
        _imp->cachedHeight == renderH) {
        // Use cached pixels — no re-render needed
    } else {
        // --- Cancel any active render ---
        if (_imp->activeRenderer) {
            _imp->activeRenderer->cancelRender();
            _imp->activeRenderer.reset();
        }

        // --- Build RenderPass visibility map (if connected) ---
        // Stored locally so the pointers in `req` remain valid for the
        // duration of executeCyclesPasses below.
        std::map<std::string, ObjectVisibility> visMap;
        std::set<std::string> activeLightSet;
        std::map<std::string, LightRayVis> lightRayVis;
        if (renderPass) {
            renderPass->refreshObjectLists();
            visMap = renderPass->getObjectVisibilityMap();
            activeLightSet = renderPass->getActiveLights();
            lightRayVis = renderPass->getLightRayVisibility();
            if (!visMap.empty())        req.visMap       = &visMap;
            if (!activeLightSet.empty()) req.activeLights = &activeLightSet;
            if (!lightRayVis.empty())   req.lightRayVis  = &lightRayVis;
        }

        // --- Render with Cycles via the shared helper ---
        _imp->activeRenderer = std::make_unique<CyclesRenderer>();
        std::string execErr;
        bool ok = executeCyclesPasses(*_imp->activeRenderer, prepared, req,
                                       _imp->cachedPassBuffers, execErr);
        if (!ok) {
            _imp->activeRenderer.reset();
            return eStatusFailed;
        }

        _imp->cachedHash = sceneHash;
        _imp->cachedWidth = renderW;
        _imp->cachedHeight = renderH;
    }

    int srcW = _imp->cachedWidth;
    int srcH = _imp->cachedHeight;

    // --- TEST: write gradient for non-Color planes to verify pipeline ---
    for (auto& planePair : args.outputPlanes) {
        const ImagePlaneDesc& planeDesc = planePair.first;
        ImagePtr planeImg = planePair.second;
        if (!planeImg) continue;

        // If this is NOT the main Color plane, write a test gradient
        if (planeDesc.getPlaneID() != "uk.co.thefoundry.OfxImagePlaneColour") {
            RectI testBounds = planeImg->getBounds();
            int tw = testBounds.width();
            int th = testBounds.height();
            int nc = planeDesc.getNumComponents();
            printf("[TEST] Writing gradient for plane '%s' (%dx%d, %d comps)\n",
                   planeDesc.getPlaneID().c_str(), tw, th, nc);
            Image::WriteAccess wa(planeImg.get());
            for (int y = testBounds.y1; y < testBounds.y2; ++y) {
                for (int x = testBounds.x1; x < testBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    float u = (float)(x - testBounds.x1) / (float)tw;
                    float v = (float)(y - testBounds.y1) / (float)th;
                    dst[0] = u; // red = horizontal gradient
                    if (nc > 1) dst[1] = v; // green = vertical gradient
                    if (nc > 2) dst[2] = 0.5f;
                    if (nc > 3) dst[3] = 1.0f;
                }
            }
            continue; // skip normal pass processing for this plane
        }
    }

    // --- Fill each output plane from its corresponding pass buffer ---
    for (auto& planePair : args.outputPlanes) {
        const ImagePlaneDesc& planeDesc = planePair.first;
        ImagePtr planeImg = planePair.second;
        if (!planeImg) continue;

        // Find the matching pass buffer
        std::string passName;
        for (const std::string& p : requestedPasses) {
            if (passNameToPlane(p).getPlaneID() == planeDesc.getPlaneID()) {
                passName = p;
                break;
            }
        }
        // Also check light group passes in cached buffers
        if (passName.empty()) {
            for (auto& cached : _imp->cachedPassBuffers) {
                if (passNameToPlane(cached.first).getPlaneID() == planeDesc.getPlaneID()) {
                    passName = cached.first;
                    break;
                }
            }
        }

        // Default to Combined for the main RGBA output
        if (passName.empty() && planeDesc.getNumComponents() == 4) {
            passName = "Combined";
        }
        printf("[CyclesRender]   plane '%s' → pass '%s' (found=%s)\n",
               planeDesc.getPlaneID().c_str(), passName.c_str(),
               passName.empty() ? "NO" : "YES");
        if (passName.empty()) continue;

        auto it = _imp->cachedPassBuffers.find(passName);
        if (it == _imp->cachedPassBuffers.end()) continue;
        const std::vector<float>& srcPixels = it->second;
        if (srcPixels.empty()) continue;

        int numOutComps = planeDesc.getNumComponents();
        RectI outBounds = planeImg->getBounds();

        Image::WriteAccess wa(planeImg.get());

        // Optional: composite Combined over background
        ImagePtr bgImg;
        Image::ReadAccess* bgRa = NULL;
        RectI bgBounds;
        bool isCombined = (passName == "Combined");
        if (isCombined) {
            EffectInstancePtr bgEffect = getInput(0);
            if (bgEffect) {
                RectI bgRoi;
                bgImg = getImage(0, args.time, RenderScale(), args.view,
                                 NULL, NULL, false, true,
                                 eStorageModeRAM, 0, &bgRoi);
            }
            if (bgImg) {
                bgRa = new Image::ReadAccess(bgImg.get());
                bgBounds = bgImg->getBounds();
            }
        }

        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;

                int fbX = x - outBounds.x1;
                int fbY = y - outBounds.y1;
                int dstW = outBounds.width();
                int dstH = outBounds.height();
                int srcX = (srcW == dstW) ? fbX : (fbX * srcW / dstW);
                int srcY = (srcH == dstH) ? fbY : (fbY * srcH / dstH);
                srcX = std::min(srcX, srcW - 1);
                srcY = std::min(srcY, srcH - 1);

                // All pass buffers are stored as 4 channels from Cycles
                int idx = (srcY * srcW + srcX) * 4;
                float r = srcPixels[idx + 0];
                float g = srcPixels[idx + 1];
                float b = srcPixels[idx + 2];
                float a = srcPixels[idx + 3];

                // Depth pass: output raw camera-space Z as grayscale.
                // Use Grade node in comp to remap range.
                if (passName == "Depth") {
                    if (r >= 1e9f) r = 0.0f; // infinity → black
                    g = b = r;
                    a = 1.0f;
                }
                // Mist / AO: single-value passes — broadcast R to G/B for
                // grayscale display. Cycles writes the scalar to R; G/B may
                // contain stale values from the 4-channel buffer.
                else if (passName == "Mist" || passName == "AO") {
                    g = b = r;
                    a = 1.0f;
                }

                if (isCombined && bgRa && bgBounds.contains(x, y)) {
                    const float* bgPix = (const float*)bgRa->pixelAt(x, y);
                    if (bgPix) {
                        dst[0] = r + bgPix[0] * (1.0f - a);
                        if (numOutComps > 1) dst[1] = g + bgPix[1] * (1.0f - a);
                        if (numOutComps > 2) dst[2] = b + bgPix[2] * (1.0f - a);
                        if (numOutComps > 3) dst[3] = a + bgPix[3] * (1.0f - a);
                        continue;
                    }
                }

                dst[0] = r;
                if (numOutComps > 1) dst[1] = g;
                if (numOutComps > 2) dst[2] = b;
                if (numOutComps > 3) dst[3] = a;
            }
        }

        delete bgRa;
    }

    return eStatusOK;
}

bool
CyclesRender::knobChanged(KnobI* k, ValueChangedReasonEnum reason,
                           ViewSpec /*view*/, double time, bool /*originatedFromMainThread*/)
{
    // --- Sync to Project — copy project default format → width/height ---
    KnobButtonPtr syncBtn = _imp->syncToProject.lock();
    if (syncBtn && k == syncBtn.get()) {
        AppInstancePtr app = getApp();
        if (app && app->getProject()) {
            Format fmt;
            app->getProject()->getProjectDefaultFormat(&fmt);
            const int pw = fmt.width();
            const int ph = fmt.height();
            KnobIntPtr wk = _imp->outputWidth.lock();
            KnobIntPtr hk = _imp->outputHeight.lock();
            if (pw > 0 && ph > 0 && wk && hk) {
                wk->setValue(pw);
                hk->setValue(ph);
            }
        }
        return true;
    }

    // --- Focus Helper: Set Focus button ---
    if (_imp->setFocusBtn.lock().get() == k) {
        // Get camera
        EffectInstancePtr camEffect = getInput(2);
        CameraProvider* cam = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;
        if (!cam) {
            setPersistentMessage(eMessageTypeError, "No camera connected to input 2.");
            return true;
        }

        // Get selected object name
        KnobChoicePtr focusKnob = _imp->focusObject.lock();
        if (!focusKnob || focusKnob->getValue() == 0) {
            setPersistentMessage(eMessageTypeError, "Select an object first.");
            return true;
        }
        ChoiceOption selected = focusKnob->getActiveEntry();
        std::string objName = selected.id;

        // Get scene and find the object's position
        EffectInstancePtr geoEffect = getInput(1);
        if (!geoEffect) {
            setPersistentMessage(eMessageTypeError, "No scene connected.");
            return true;
        }

        // Follow through RenderPass if present
        RenderPass* renderPass = dynamic_cast<RenderPass*>(geoEffect.get());
        if (renderPass) {
            geoEffect = renderPass->getInput(0);
            if (!geoEffect) { setPersistentMessage(eMessageTypeError, "RenderPass has no scene."); return true; }
        }

        // Find the object in scene inputs
        double objTX = 0, objTY = 0, objTZ = 0;
        bool found = false;
        Scene3D* scene3d = dynamic_cast<Scene3D*>(geoEffect.get());
        Group3D* group3d = dynamic_cast<Group3D*>(geoEffect.get());
        int numInputs = scene3d ? scene3d->getNInputs() : (group3d ? group3d->getNInputs() : 0);
        EffectInstance* sceneNode = scene3d ? (EffectInstance*)scene3d : (EffectInstance*)group3d;

        for (int i = 0; i < numInputs && !found; ++i) {
            EffectInstancePtr inp = sceneNode->getInput(i);
            if (!inp) continue;
            NodePtr node = inp->getNode();
            if (!node || node->getScriptName_mt_safe() != objName) continue;

            // Read translate knobs from the object
            KnobIPtr kTX = inp->getKnobByName("translateX");
            KnobIPtr kTY = inp->getKnobByName("translateY");
            KnobIPtr kTZ = inp->getKnobByName("translateZ");
            if (kTX) objTX = dynamic_cast<KnobDouble*>(kTX.get())->getValueAtTime(time);
            if (kTY) objTY = dynamic_cast<KnobDouble*>(kTY.get())->getValueAtTime(time);
            if (kTZ) objTZ = dynamic_cast<KnobDouble*>(kTZ.get())->getValueAtTime(time);
            found = true;
        }

        if (!found) {
            setPersistentMessage(eMessageTypeError, "Object '" + objName + "' not found in scene.");
            return true;
        }

        // Get camera position
        double camTX = 0, camTY = 0, camTZ = 0, camRX = 0, camRY = 0, camRZ = 0;
        cam->getCameraPosition(time, camTX, camTY, camTZ, camRX, camRY, camRZ);

        // Compute distance
        double dx = objTX - camTX;
        double dy = objTY - camTY;
        double dz = objTZ - camTZ;
        double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist < 0.001) dist = 0.001;

        // Set this node's focusDistance knob
        KnobDoublePtr fd = _imp->focusDistance.lock();
        if (fd) {
            fd->setValue(dist);
            clearPersistentMessage(false);
        }
        return true;
    }

    // --- Focus Helper: Refresh Objects button ---
    if (_imp->refreshFocusBtn.lock().get() == k) {
        KnobChoicePtr focusKnob = _imp->focusObject.lock();
        if (!focusKnob) return true;

        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("(none)", "", "No object selected"));

        EffectInstancePtr geoEffect = getInput(1);
        if (geoEffect) {
            RenderPass* rp = dynamic_cast<RenderPass*>(geoEffect.get());
            if (rp) geoEffect = rp->getInput(0);
        }
        if (geoEffect) {
            Scene3D* scene3d = dynamic_cast<Scene3D*>(geoEffect.get());
            Group3D* group3d = dynamic_cast<Group3D*>(geoEffect.get());
            int numInputs = scene3d ? scene3d->getNInputs() : (group3d ? group3d->getNInputs() : 0);
            EffectInstance* scn = scene3d ? (EffectInstance*)scene3d : (EffectInstance*)group3d;
            for (int i = 0; i < numInputs; ++i) {
                EffectInstancePtr inp = scn ? scn->getInput(i) : EffectInstancePtr();
                if (!inp) continue;
                NodePtr node = inp->getNode();
                if (!node) continue;
                std::string name = node->getScriptName_mt_safe();
                entries.push_back(ChoiceOption(name, "", name));
            }
        }
        focusKnob->populateChoices(entries);
        return true;
    }

    if (_imp->saveExrBtn.lock().get() == k) {
        std::string outPath = _imp->exrOutputPath.lock()->getValue();
        if (outPath.empty()) {
            setPersistentMessage(eMessageTypeError, "Set EXR output path first.");
            return true;
        }
        if (_imp->cachedPassBuffers.empty() || _imp->cachedWidth <= 0) {
            setPersistentMessage(eMessageTypeError, "No render cached. Render first, then save.");
            return true;
        }

        bool ok = CyclesRenderer::saveMultiLayerEXR(outPath, _imp->cachedPassBuffers,
                                                      _imp->cachedWidth, _imp->cachedHeight);
        if (ok) {
            clearPersistentMessage(false);
        } else {
            setPersistentMessage(eMessageTypeError, "Failed to write EXR: " + outPath);
        }
        return true;
    }

    // --- Refresh Passes — re-publish output planes + force a re-render ---
    if (_imp->refreshPassesBtn.lock().get() == k) {
        // Drop the internal render cache so the next render isn't served stale...
        _imp->cachedHash = 0;
        _imp->cachedPassBuffers.clear();
        // ...and re-run metadata so the (possibly changed) AOV plane set is
        // re-published to downstream / the viewer without scrubbing the timeline.
        refreshMetadata_public(true);
        clearPersistentMessage(false);
        // Force the viewers to re-pull at the current frame (what scrubbing did).
        AppInstancePtr app = getApp();
        if (app) {
            app->renderAllViewers(true);
        }
        return true;
    }

    return false;
}

// Toggle visibility of the Render / Integrator / DOF / Motion Blur knobs
// based on whether a CyclesRenderSettings is wired to input slot 3. When
// connected, those knobs become read-from-the-settings-node and showing the
// local ones would be misleading. Output / AOV / Focus / EXR knobs stay
// visible since they're CyclesRender-only concerns.
static void
setSettingsKnobsSecret(CyclesRenderPrivate* p, bool secret)
{
    auto hide = [secret](const auto& wp) {
        auto k = wp.lock();
        if (k) k->setSecret(secret);
    };
    // Render tab
    hide(p->samples);
    hide(p->denoise);
    // Integrator
    hide(p->maxBounces);
    hide(p->diffuseBounces);
    hide(p->glossyBounces);
    hide(p->transmissionBounces);
    // DOF (focus helper buttons stay — they target focusDistance which is now
    // also hidden, but the buttons themselves remain visible; that's a minor
    // UX wart we'll fix when the focus helper learns to write to the
    // upstream Settings node directly.)
    hide(p->dofEnabled);
    hide(p->focusDistance);
    hide(p->bokehBlades);
    hide(p->bladeRotation);
    // Motion blur
    hide(p->motionBlur);
    hide(p->shutterTime);
    hide(p->shutterPosition);
}

void
CyclesRender::onInputChanged(int inputNo)
{
    if (inputNo != 3) return;
    EffectInstancePtr settingsEffect = getInput(3);
    const bool connected =
        settingsEffect &&
        dynamic_cast<const CyclesRenderSettings*>(settingsEffect.get()) != nullptr;
    setSettingsKnobsSecret(_imp.get(), connected);
}

NATRON_NAMESPACE_EXIT

#include "moc_CyclesRender.cpp"
