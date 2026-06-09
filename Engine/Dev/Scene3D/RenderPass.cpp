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

#include "RenderPass.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../Format.h"
#include "../../ViewIdx.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <memory>

#include "Scene3D.h"
#include "Group3D.h"
#include "Light3D.h"

// Live Cycles preview path. RenderPass is always compiled (Scene3D), but the
// Cycles renderer only exists when NATRON_CYCLES is on — so the preview render
// is gated and falls back to a no-op stub otherwise.
#ifdef NATRON_CYCLES
#include "CameraProvider.h"
#include "../Cycles/CyclesPassRender.h"
#include "../Cycles/CyclesRenderer.h"
#include "../Cycles/CyclesRenderSettings.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

// Cycles ray visibility flags (from kernel/types.h)
static const uint32_t PATH_RAY_CAMERA       = (1 << 0);
static const uint32_t PATH_RAY_REFLECT      = (1 << 1);
static const uint32_t PATH_RAY_TRANSMIT     = (1 << 2);
static const uint32_t PATH_RAY_DIFFUSE      = (1 << 3);
static const uint32_t PATH_RAY_GLOSSY       = (1 << 4);
static const uint32_t PATH_RAY_SINGULAR     = (1 << 5);
static const uint32_t PATH_RAY_TRANSPARENT  = (1 << 6);
static const uint32_t PATH_RAY_VOLUME_SCATTER = (1 << 7);
static const uint32_t PATH_RAY_SHADOW       = (1 << 9) | (1 << 10);
static const uint32_t PATH_RAY_ALL_VISIBILITY = 0x7FF;

struct RenderPassPrivate
{
    KnobStringWPtr passName;
    KnobButtonWPtr refreshBtn;
    KnobStringWPtr infoKnob;

    // Object category checkboxes (pre-allocated, hidden when unused)
    KnobGroupWPtr cameraGroup;
    KnobBoolWPtr  cameraObjects[RENDERPASS_MAX_OBJECTS];

    KnobGroupWPtr traceGroup;
    KnobBoolWPtr  traceObjects[RENDERPASS_MAX_OBJECTS];

    KnobGroupWPtr matteGroup;
    KnobBoolWPtr  matteObjects[RENDERPASS_MAX_OBJECTS];

    KnobGroupWPtr shadowCatcherGroup;
    KnobBoolWPtr  shadowCatchers[RENDERPASS_MAX_OBJECTS];

    KnobGroupWPtr excludedGroup;
    KnobBoolWPtr  excludedObjects[RENDERPASS_MAX_OBJECTS];

    KnobGroupWPtr reflectionMatteGroup;
    KnobBoolWPtr  reflectionMatteObjects[RENDERPASS_MAX_OBJECTS];

    // Lights
    KnobGroupWPtr lightsGroup;
    KnobBoolWPtr  activeLightsKnobs[RENDERPASS_MAX_OBJECTS];
    // Per-light ray-visibility toggles (same row as the active checkbox).
    KnobBoolWPtr  lightVisCamera[RENDERPASS_MAX_OBJECTS];
    KnobBoolWPtr  lightVisGlossy[RENDERPASS_MAX_OBJECTS];
    KnobBoolWPtr  lightVisDiffuse[RENDERPASS_MAX_OBJECTS];
    KnobBoolWPtr  lightVisTransmit[RENDERPASS_MAX_OBJECTS];

    // Live preview render knobs
    KnobIntWPtr  outputWidth, outputHeight;
    KnobIntWPtr  samples;
    KnobBoolWPtr previewMode;   // render at half res, upscale

    // AOV enable knobs (mirror CyclesRender's AOV Passes tab)
    KnobButtonWPtr refreshPassesBtn;
    KnobBoolWPtr aovDiffDir, aovDiffInd, aovDiffCol, aovDiffuse, aovTrans;
    KnobBoolWPtr aovGlossDir, aovGlossInd, aovGlossCol, aovGlossy;
    KnobBoolWPtr aovEmission, aovEnv, aovAO;
    KnobBoolWPtr aovNormal, aovDepth, aovUV, aovMist;
    KnobBoolWPtr aovReflMatte;

    // Cached discovered names (index matches knob index)
    std::vector<std::string> geoNames;
    std::vector<std::string> lightNames;

#ifdef NATRON_CYCLES
    // Persistent renderer so a new preview can cancel an in-flight one when the
    // user drags a slider / moves the camera.
    std::unique_ptr<CyclesRenderer> activeRenderer;
#endif
};


#ifdef NATRON_CYCLES
// Collect enabled AOV pass names (mirrors CyclesRender::getEnabledPasses — the
// strings must match what executeCyclesPasses produces). "Combined" is always on.
static std::vector<std::string>
getEnabledPassesRP(const RenderPassPrivate* imp)
{
    std::vector<std::string> passes;
    passes.push_back("Combined");
    auto check = [&](const KnobBoolWPtr& knob, const char* name) {
        KnobBoolPtr k = knob.lock();
        if (k && k->getValue()) passes.push_back(name);
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
    check(imp->aovReflMatte, "ReflectionMatte");
    return passes;
}

// Map a pass name → ImagePlaneDesc (mirrors CyclesRender::passNameToPlane so the
// plane IDs match — a RenderPass preview and a CyclesRender produce identical AOV
// plane names).
static ImagePlaneDesc
passNameToPlaneRP(const std::string& name)
{
    if (name == "Combined") return ImagePlaneDesc::getRGBAComponents();
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
    if (name == "AO")       return ImagePlaneDesc("AO",    "Ambient Occlusion", "", rgb3, 3);
    if (name == "Depth")    return ImagePlaneDesc("Depth", "Depth",             "", rgb3, 3);
    if (name == "Mist")     return ImagePlaneDesc("Mist",  "Mist",              "", rgb3, 3);
    if (name == "ReflectionMatte") {
        static const char* rgba4[] = {"R", "G", "B", "A"};
        return ImagePlaneDesc("ReflectionMatte", "Reflection Matte", "", rgba4, 4);
    }
    return ImagePlaneDesc::getRGBAComponents();
}
#endif // NATRON_CYCLES

RenderPass::RenderPass(NodePtr node)
    : EffectInstance(node)
    , _imp(new RenderPassPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RenderPass::~RenderPass()
{
}

std::string
RenderPass::getPluginDescription() const
{
    return tr("Render pass filter for multi-pass rendering.\n\n"
              "Sits between Scene3D and CyclesRender. Defines which objects are visible "
              "to camera, which are trace-only (phantom), holdouts, and shadow catchers.\n\n"
              "Each CyclesRender with a RenderPass produces a separate element for compositing.").toStdString();
}

std::string
RenderPass::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "scene";
    if (inputNb == 1) return "camera";
    if (inputNb == 2) return "settings";
    return std::string();
}

void
RenderPass::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
RenderPass::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
RenderPass::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

// Helper to create a group of RENDERPASS_MAX_OBJECTS hidden bool knobs
static void createObjectBoolGroup(RenderPass* self,
                                  const KnobPagePtr& page,
                                  const QString& groupLabel,
                                  const std::string& groupName,
                                  const QString& tooltip,
                                  KnobGroupWPtr& outGroup,
                                  KnobBoolWPtr outKnobs[],
                                  int maxObjects)
{
    KnobGroupPtr grp = AppManager::createKnob<KnobGroup>(self, groupLabel);
    grp->setName(groupName);
    grp->setHintToolTip(tooltip);
    grp->setDefaultValue(false); // collapsed by default
    page->addKnob(grp);
    outGroup = grp;

    for (int i = 0; i < maxObjects; ++i) {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(self, QString::fromUtf8("---"));
        k->setName(groupName + std::to_string(i));
        k->setDefaultValue(false);
        k->setSecret(true);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        grp->addKnob(k);
        outKnobs[i] = k;
    }
}

void
RenderPass::initializeKnobs()
{
    // --- Objects page ---
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Objects"));

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Pass Name"));
        k->setName("passName");
        k->setDefaultValue("beauty");
        k->setHintToolTip(tr("Name for this render pass (e.g., char, env, ground, fx)."));
        page->addKnob(k);
        _imp->passName = k;
    }

    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Refresh Objects"));
        k->setName("refreshObjects");
        k->setHintToolTip(tr("Re-scan the connected Scene for objects and lights."));
        page->addKnob(k);
        _imp->refreshBtn = k;
    }

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Discovered"));
        k->setName("info");
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setIsPersistent(false);
        k->setDefaultValue("Connect a Scene and click Refresh.");
        page->addKnob(k);
        _imp->infoKnob = k;
    }

    // Object category groups
    createObjectBoolGroup(this, page, tr("Camera Objects"), "camObj",
                          tr("Objects visible to camera rays (primary render subjects)."),
                          _imp->cameraGroup, _imp->cameraObjects, RENDERPASS_MAX_OBJECTS);

    createObjectBoolGroup(this, page, tr("Trace Objects"), "traceObj",
                          tr("Objects visible to reflection/GI/shadow but NOT camera (phantom)."),
                          _imp->traceGroup, _imp->traceObjects, RENDERPASS_MAX_OBJECTS);

    createObjectBoolGroup(this, page, tr("Matte Objects"), "matteObj",
                          tr("Holdout objects \xe2\x80\x94 cut alpha holes where they appear."),
                          _imp->matteGroup, _imp->matteObjects, RENDERPASS_MAX_OBJECTS);

    createObjectBoolGroup(this, page, tr("Excluded Objects"), "exclObj",
                          tr("Objects completely removed from the render."),
                          _imp->excludedGroup, _imp->excludedObjects, RENDERPASS_MAX_OBJECTS);

    // --- Separate passes — Shadow Catcher and Reflection Matte each emit their
    // own output plane (the catcher composites its own pass; the matte runs a
    // second emissive render). Behind a separator to set them apart from the
    // beauty-visibility categories above. ---
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Separate Passes"));
        sep->setName("passesSeparator");
        page->addKnob(sep);
    }

    createObjectBoolGroup(this, page, tr("Shadow Catchers"), "shadowCatch",
                          tr("Objects that receive shadows but render transparent. "
                             "Composites into its own Shadow Catcher pass."),
                          _imp->shadowCatcherGroup, _imp->shadowCatchers, RENDERPASS_MAX_OBJECTS);

    createObjectBoolGroup(this, page, tr("Reflection Matte"), "reflMatte",
                          tr("Objects to include in the Reflection Matte plane (enable it in the AOV "
                             "Passes tab). Flagged objects are re-rendered as pure white emitters, so "
                             "they read as a matte both directly and wherever they're reflected (e.g. a "
                             "sphere mirrored in a glossy floor). Adds a second render pass."),
                          _imp->reflectionMatteGroup, _imp->reflectionMatteObjects, RENDERPASS_MAX_OBJECTS);

    // --- Lights — on the Objects page, after Excluded Objects, behind a separator ---
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Lights"));
        sep->setName("lightsSeparator");
        page->addKnob(sep);
    }
    // Active Lights — one row per light: [Active] [Cam] [Refl] [Diff] [Trans].
    // Active = light contributes to this pass (NONE ticked = no lights). The Cam/Refl/
    // Diff/Trans toggles are per-light ray VISIBILITY (default on = inherit natural):
    // untick to remove that light / the dome environment from that ray type — e.g.
    // untick Refl so a dome/area light doesn't appear in reflections (still lights the
    // scene). Cam is meaningful for the dome (matches Renderable); for area/point
    // lights it's a no-op (they're camera-invisible by default).
    {
        KnobGroupPtr grp = AppManager::createKnob<KnobGroup>(this, tr("Active Lights"));
        grp->setName("activeLight");
        grp->setHintToolTip(tr("Per light: Active = contributes to this pass. "
                               "Cam/Refl/Diff/Trans = visible to that ray type (untick Refl to keep "
                               "the light/dome out of reflections; it still lights the scene)."));
        grp->setDefaultValue(false);
        page->addKnob(grp);
        _imp->lightsGroup = grp;

        auto makeRay = [&](KnobBoolWPtr& out, int i, const char* suffix, const QString& label) {
            KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, label);
            k->setName(std::string("light") + suffix + std::to_string(i));
            k->setDefaultValue(true);            // visible by default
            k->setSecret(true);
            k->setAnimationEnabled(false);
            k->setEvaluateOnChange(false);
            k->setAddNewLine(false);             // sit on the same row
            grp->addKnob(k);
            out = k;
        };

        for (int i = 0; i < RENDERPASS_MAX_OBJECTS; ++i) {
            KnobBoolPtr a = AppManager::createKnob<KnobBool>(this, QString::fromUtf8("---"));
            a->setName("activeLight" + std::to_string(i));
            a->setDefaultValue(false);
            a->setSecret(true);
            a->setAnimationEnabled(false);
            a->setEvaluateOnChange(false);
            a->setAddNewLine(false);             // ray toggles follow on this line
            a->setSpacingBetweenItems(60);       // padding before the ray toggles
            grp->addKnob(a);
            _imp->activeLightsKnobs[i] = a;

            makeRay(_imp->lightVisCamera[i],   i, "Cam",  tr("Cam"));
            makeRay(_imp->lightVisGlossy[i],   i, "Refl", tr("Refl"));
            makeRay(_imp->lightVisDiffuse[i],  i, "Diff", tr("Diff"));
            // Last toggle ends the row (default addNewLine = true).
            {
                KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Trans"));
                k->setName(std::string("lightTrans") + std::to_string(i));
                k->setDefaultValue(true);
                k->setSecret(true);
                k->setAnimationEnabled(false);
                k->setEvaluateOnChange(false);
                grp->addKnob(k);
                _imp->lightVisTransmit[i] = k;
            }
        }
    }

    // --- Preview page (live Cycles render of this pass) ---
    // Connect a camera (input 1) and optionally a CyclesRenderSettings (input 2),
    // then wire this node to a Viewer to see the pass render live. The preview
    // reflects this pass's own object visibility / holdout / shadow-catcher /
    // light selection.
    KnobPagePtr previewPage = AppManager::createKnob<KnobPage>(this, tr("Preview"));
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Output Width"));
        k->setName("outputWidth"); k->setDefaultValue(1920);
        k->setMinimum(1); k->setDisplayMinimum(1); k->setDisplayMaximum(8192);
        k->setHintToolTip(tr("Preview render width in pixels."));
        previewPage->addKnob(k); _imp->outputWidth = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Output Height"));
        k->setName("outputHeight"); k->setDefaultValue(1080);
        k->setMinimum(1); k->setDisplayMinimum(1); k->setDisplayMaximum(8192);
        k->setHintToolTip(tr("Preview render height in pixels."));
        previewPage->addKnob(k); _imp->outputHeight = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Samples"));
        k->setName("previewSamples"); k->setDefaultValue(16);
        k->setMinimum(1); k->setMaximum(8192);
        k->setHintToolTip(tr("Path-trace samples for the live preview. Ignored when a "
                             "CyclesRenderSettings node is connected (input 2)."));
        previewPage->addKnob(k); _imp->samples = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Preview (Half Res)"));
        k->setName("previewMode"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Render at half resolution and upscale. Faster for interactive work."));
        previewPage->addKnob(k); _imp->previewMode = k;
    }

    // --- AOV Passes page (mirror CyclesRender) ---
    KnobPagePtr aovPage = AppManager::createKnob<KnobPage>(this, tr("AOV Passes"));
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Refresh Passes"));
        k->setName("refreshPasses");
        k->setHintToolTip(tr("Re-publish the output planes after toggling AOVs, so the "
                             "viewer / downstream picks up the new pass set without scrubbing."));
        aovPage->addKnob(k); _imp->refreshPassesBtn = k;
    }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse Direct"));   k->setName("aovDiffDir");  k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovDiffDir  = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse Indirect")); k->setName("aovDiffInd");  k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovDiffInd  = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse Color"));    k->setName("aovDiffCol");  k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovDiffCol  = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Diffuse"));          k->setName("aovDiffuse");  k->setDefaultValue(false);
      k->setHintToolTip(tr("Combined diffuse contribution as it appears in the beauty: (Diffuse Direct + "
                           "Diffuse Indirect) \xc3\x97 Diffuse Color. Synthesized from the sub-passes."));
      aovPage->addKnob(k); _imp->aovDiffuse = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy Direct"));    k->setName("aovGlossDir"); k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovGlossDir = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy Indirect"));  k->setName("aovGlossInd"); k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovGlossInd = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy Color"));     k->setName("aovGlossCol"); k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovGlossCol = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glossy"));           k->setName("aovGlossy");   k->setDefaultValue(false);
      k->setHintToolTip(tr("Combined glossy contribution as it appears in the beauty: (Glossy Direct + "
                           "Glossy Indirect) \xc3\x97 Glossy Color. Synthesized from the sub-passes, so it's "
                           "directly viewable without rebuilding it in comp."));
      aovPage->addKnob(k); _imp->aovGlossy = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Transmission"));     k->setName("aovTrans");    k->setDefaultValue(false);
      k->setHintToolTip(tr("Combined transmission contribution as it appears in the beauty: (Transmission "
                           "Direct + Transmission Indirect) \xc3\x97 Transmission Color. Synthesized from the "
                           "sub-passes (glass / refraction)."));
      aovPage->addKnob(k); _imp->aovTrans = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Emission"));         k->setName("aovEmission"); k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovEmission = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Environment"));      k->setName("aovEnv");      k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovEnv      = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Ambient Occlusion"));k->setName("aovAO");       k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovAO       = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Normal"));           k->setName("aovNormal");   k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovNormal   = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Depth"));            k->setName("aovDepth");    k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovDepth    = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("UV"));               k->setName("aovUV");       k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovUV       = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Mist"));             k->setName("aovMist");     k->setDefaultValue(false); aovPage->addKnob(k); _imp->aovMist     = k; }
    { KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Reflection Matte")); k->setName("aovReflMatte"); k->setDefaultValue(false);
      k->setHintToolTip(tr("Output a Reflection Matte plane: objects flagged 'Reflection Matte' (Objects tab) "
                           "are re-rendered as pure white emitters, so they read as a matte both directly and "
                           "in reflections. This adds a second render pass (roughly doubles render time)."));
      aovPage->addKnob(k); _imp->aovReflMatte = k; }
}

bool
RenderPass::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    KnobButtonPtr refreshBtn = _imp->refreshBtn.lock();
    if (refreshBtn && k == refreshBtn.get()) {
        refreshObjectLists();
        return true;
    }

    // Refresh Passes — re-publish output planes after an AOV toggle + re-render.
    KnobButtonPtr refreshPasses = _imp->refreshPassesBtn.lock();
    if (refreshPasses && k == refreshPasses.get()) {
        refreshMetadata_public(true);
        clearPersistentMessage(false);
        AppInstancePtr app = getApp();
        if (app) {
            app->renderAllViewers(true);
        }
        return true;
    }

    // Object / light selection checkboxes are created setEvaluateOnChange(false)
    // (so the discovery refresh doesn't storm renders), which means Natron does NOT
    // fold them into the node hash. Without that, toggling a light/object left
    // previously-rendered frames served from the frame cache with the OLD selection
    // (only the scrubbed-to frame ever refreshed). Detect a selection toggle, bump
    // the knobs age (incrementKnobsAge() also recomputes the hash → busts the frame
    // cache for ALL frames), and re-render so the change shows everywhere.
    for (int i = 0; i < RENDERPASS_MAX_OBJECTS; ++i) {
        if ( _imp->cameraObjects[i].lock().get()      == k ||
             _imp->traceObjects[i].lock().get()       == k ||
             _imp->matteObjects[i].lock().get()       == k ||
             _imp->shadowCatchers[i].lock().get()     == k ||
             _imp->excludedObjects[i].lock().get()    == k ||
             _imp->reflectionMatteObjects[i].lock().get() == k ||
             _imp->activeLightsKnobs[i].lock().get()  == k ||
             _imp->lightVisCamera[i].lock().get()     == k ||
             _imp->lightVisGlossy[i].lock().get()     == k ||
             _imp->lightVisDiffuse[i].lock().get()    == k ||
             _imp->lightVisTransmit[i].lock().get()   == k ) {
            NodePtr node = getNode();
            if (node) {
                node->incrementKnobsAge();
            }
            AppInstancePtr app = getApp();
            if (app) {
                app->renderAllViewers(true);
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Refresh object/light checkbox lists
// ---------------------------------------------------------------------------

static void updateBoolKnobs(KnobBoolWPtr knobs[], int maxSlots,
                             const std::vector<std::string>& names,
                             bool defaultChecked)
{
    int count = std::min((int)names.size(), maxSlots);
    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = knobs[i].lock();
        if (!k) continue;
        k->setLabel(names[i]);
        k->setSecret(false);
        // Only set default on first population (don't overwrite user edits)
        // We detect "first population" by checking if the label was the placeholder
    }
    // Hide unused slots
    for (int i = count; i < maxSlots; ++i) {
        KnobBoolPtr k = knobs[i].lock();
        if (!k) continue;
        k->setSecret(true);
        k->setValue(false);
        k->setLabel(std::string("---"));
    }
}

void
RenderPass::refreshObjectLists()
{
    std::vector<std::string> geo, lights;
    discoverSceneObjects(geo, lights);

    _imp->geoNames = geo;
    _imp->lightNames = lights;

    // Update info display
    std::string info;
    if (geo.empty() && lights.empty()) {
        info = "No objects found. Connect a Scene3D or Group3D.";
    } else {
        info = "Geo: ";
        for (size_t i = 0; i < geo.size(); ++i) {
            if (i > 0) info += ", ";
            info += geo[i];
        }
        info += " | Lights: ";
        if (lights.empty()) {
            info += "(none)";
        } else {
            for (size_t i = 0; i < lights.size(); ++i) {
                if (i > 0) info += ", ";
                info += lights[i];
            }
        }
    }
    KnobStringPtr infoK = _imp->infoKnob.lock();
    if (infoK) infoK->setValue(info);

    // Update all category checkbox groups with discovered geo names
    updateBoolKnobs(_imp->cameraObjects,  RENDERPASS_MAX_OBJECTS, geo, false);
    updateBoolKnobs(_imp->traceObjects,   RENDERPASS_MAX_OBJECTS, geo, false);
    updateBoolKnobs(_imp->matteObjects,   RENDERPASS_MAX_OBJECTS, geo, false);
    updateBoolKnobs(_imp->shadowCatchers, RENDERPASS_MAX_OBJECTS, geo, false);
    updateBoolKnobs(_imp->excludedObjects,RENDERPASS_MAX_OBJECTS, geo, false);
    updateBoolKnobs(_imp->reflectionMatteObjects, RENDERPASS_MAX_OBJECTS, geo, false);

    // Update lights — active checkbox label/visibility...
    updateBoolKnobs(_imp->activeLightsKnobs, RENDERPASS_MAX_OBJECTS, lights, false);
    // ...and show/hide the per-light ray-visibility toggles in sync with the rows.
    {
        const int lcount = std::min((int)lights.size(), RENDERPASS_MAX_OBJECTS);
        auto setSecret = [](const KnobBoolWPtr& w, bool secret) {
            KnobBoolPtr k = w.lock(); if (k) k->setSecret(secret);
        };
        for (int i = 0; i < RENDERPASS_MAX_OBJECTS; ++i) {
            const bool hidden = (i >= lcount);
            setSecret(_imp->lightVisCamera[i],   hidden);
            setSecret(_imp->lightVisGlossy[i],   hidden);
            setSecret(_imp->lightVisDiffuse[i],  hidden);
            setSecret(_imp->lightVisTransmit[i], hidden);
        }
    }
}

// ---------------------------------------------------------------------------
// Pass data accessors
// ---------------------------------------------------------------------------

std::string
RenderPass::getPassName() const
{
    KnobStringPtr k = _imp->passName.lock();
    return k ? k->getValue() : "beauty";
}

void
RenderPass::discoverSceneObjects(std::vector<std::string>& outGeo,
                                  std::vector<std::string>& outLights) const
{
    outGeo.clear();
    outLights.clear();

#ifdef NATRON_CYCLES
    // Use the SAME recursive traversal the renderer uses (collectSceneNodes via
    // the enumerate* helpers): descends Scene3D / Group3D containers, sees through
    // Dot routing nodes, and classifies Light3D vs geo. This is why a Dot no
    // longer shows up as "geo" and lights nested inside a Group3D are found.
    // scene is on THIS node's input 0, so pass sceneInputSlot = 0.
    EffectInstance* self = const_cast<EffectInstance*>( static_cast<const EffectInstance*>(this) );

    std::vector<SceneGeoInfo> geoInfo;
    enumerateSceneGeo(self, 0.0, geoInfo, /*sceneInputSlot=*/0);
    for (size_t i = 0; i < geoInfo.size(); ++i) {
        outGeo.push_back(geoInfo[i].scriptName);
    }

    std::vector<SceneLightInfo> lightInfo;
    enumerateSceneLights(self, 0.0, lightInfo, /*sceneInputSlot=*/0);
    for (size_t i = 0; i < lightInfo.size(); ++i) {
        outLights.push_back(lightInfo[i].scriptName);
    }
#else
    // No Cycles: fall back to a shallow one-level walk of the scene input.
    EffectInstancePtr sceneEffect = getInput(0);
    if (!sceneEffect) return;
    Scene3D* scene3d = dynamic_cast<Scene3D*>(sceneEffect.get());
    Group3D* group3d = dynamic_cast<Group3D*>(sceneEffect.get());
    std::vector<EffectInstancePtr> sceneInputs;
    if (scene3d) {
        for (int i = 0; i < scene3d->getNInputs(); ++i) {
            EffectInstancePtr inp = scene3d->getInput(i);
            if (inp) sceneInputs.push_back(inp);
        }
    } else if (group3d) {
        for (int i = 0; i < group3d->getNInputs(); ++i) {
            EffectInstancePtr inp = group3d->getInput(i);
            if (inp) sceneInputs.push_back(inp);
        }
    }
    for (const auto& eff : sceneInputs) {
        NodePtr node = eff->getNode();
        if (!node) continue;
        std::string name = node->getScriptName_mt_safe();
        if (dynamic_cast<Light3D*>(eff.get())) outLights.push_back(name);
        else outGeo.push_back(name);
    }
#endif
}

std::map<std::string, ObjectVisibility>
RenderPass::getObjectVisibilityMap() const
{
    std::map<std::string, ObjectVisibility> visMap;

    const std::vector<std::string>& geo = _imp->geoNames;
    int count = std::min((int)geo.size(), RENDERPASS_MAX_OBJECTS);

    // First pass: mark everything as excluded by default
    for (int i = 0; i < count; ++i) {
        ObjectVisibility vis;
        vis.rayVisibility = 0;
        vis.isHoldout = false;
        vis.isShadowCatcher = false;
        vis.isExcluded = true;
        visMap[geo[i]] = vis;
    }

    // Camera Objects: full visibility
    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = _imp->cameraObjects[i].lock();
        if (k && k->getValue()) {
            ObjectVisibility& vis = visMap[geo[i]];
            vis.rayVisibility = PATH_RAY_ALL_VISIBILITY;
            vis.isExcluded = false;
        }
    }

    // Trace Objects: all rays except camera
    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = _imp->traceObjects[i].lock();
        if (k && k->getValue()) {
            ObjectVisibility& vis = visMap[geo[i]];
            vis.rayVisibility = PATH_RAY_ALL_VISIBILITY & ~PATH_RAY_CAMERA;
            vis.isExcluded = false;
        }
    }

    // Matte Objects: full visibility + holdout
    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = _imp->matteObjects[i].lock();
        if (k && k->getValue()) {
            ObjectVisibility& vis = visMap[geo[i]];
            vis.rayVisibility = PATH_RAY_ALL_VISIBILITY;
            vis.isHoldout = true;
            vis.isExcluded = false;
        }
    }

    // Shadow Catchers: full visibility + shadow catcher flag
    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = _imp->shadowCatchers[i].lock();
        if (k && k->getValue()) {
            ObjectVisibility& vis = visMap[geo[i]];
            vis.rayVisibility = PATH_RAY_ALL_VISIBILITY;
            vis.isShadowCatcher = true;
            vis.isExcluded = false;
        }
    }

    // Excluded Objects: force excluded (overrides any other category)
    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = _imp->excludedObjects[i].lock();
        if (k && k->getValue()) {
            ObjectVisibility& vis = visMap[geo[i]];
            vis.rayVisibility = 0;
            vis.isHoldout = false;
            vis.isShadowCatcher = false;
            vis.isExcluded = true;
        }
    }

    // Reflection Matte: orthogonal flag (the object must also be visible to reflection
    // rays — e.g. Camera or Trace Objects — for the matte to appear in a reflection).
    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = _imp->reflectionMatteObjects[i].lock();
        if (k && k->getValue()) {
            visMap[geo[i]].reflectionMatte = true;
        }
    }

    return visMap;
}

std::set<std::string>
RenderPass::getActiveLights() const
{
    std::set<std::string> lights;
    const std::vector<std::string>& lightNames = _imp->lightNames;
    int count = std::min((int)lightNames.size(), RENDERPASS_MAX_OBJECTS);

    for (int i = 0; i < count; ++i) {
        KnobBoolPtr k = _imp->activeLightsKnobs[i].lock();
        if (k && k->getValue()) {
            lights.insert(lightNames[i]);
        }
    }

    // Convention: nothing ticked = render with NO lights (not all). The shared
    // Cycles renderer treats a NULL activeLights as "all lights" and a non-empty
    // set as "only these"; so when no light is selected we return a sentinel that
    // matches no real light, which makes the renderer exclude every light. The
    // sentinel uses a control char so it can never collide with a node script name.
    // Centralised here so it applies both to RenderPass's own preview and to a
    // downstream CyclesRender that reads this selection (CyclesRender.cpp:906/966).
    if (lights.empty()) {
        lights.insert(std::string("\x01__renderpass_no_lights__"));
    }
    return lights;
}

std::map<std::string, LightRayVis>
RenderPass::getLightRayVisibility() const
{
    std::map<std::string, LightRayVis> out;
    const std::vector<std::string>& names = _imp->lightNames;
    int count = std::min((int)names.size(), RENDERPASS_MAX_OBJECTS);
    for (int i = 0; i < count; ++i) {
        LightRayVis v;
        KnobBoolPtr kc = _imp->lightVisCamera[i].lock();
        KnobBoolPtr kg = _imp->lightVisGlossy[i].lock();
        KnobBoolPtr kd = _imp->lightVisDiffuse[i].lock();
        KnobBoolPtr kt = _imp->lightVisTransmit[i].lock();
        v.camera   = kc ? kc->getValue() : true;
        v.glossy   = kg ? kg->getValue() : true;
        v.diffuse  = kd ? kd->getValue() : true;
        v.transmit = kt ? kt->getValue() : true;
        out[names[i]] = v;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Render (pass-through)
// ---------------------------------------------------------------------------

void
RenderPass::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                           EffectInstance::ComponentsNeededMap* comps,
                                           double* passThroughTime, int* passThroughView,
                                           int* passThroughInput)
{
    std::list<ImagePlaneDesc> produced;
#ifdef NATRON_CYCLES
    std::vector<std::string> passes = getEnabledPassesRP(_imp.get());
    for (const std::string& p : passes) {
        produced.push_back( passNameToPlaneRP(p) );
    }
#else
    produced.push_back( ImagePlaneDesc::getRGBAComponents() );
#endif
    (*comps)[-1] = produced;
    *passThroughTime  = 0;
    *passThroughView  = 0;
    *passThroughInput = 0;
}

StatusEnum
RenderPass::getPreferredMetadata(NodeMetadata& metadata)
{
    // The preview depends on the current frame (animated geo / lights / camera).
    metadata.setIsFrameVarying(true);

    int w = _imp->outputWidth.lock()  ? _imp->outputWidth.lock()->getValue()  : 1920;
    int h = _imp->outputHeight.lock() ? _imp->outputHeight.lock()->getValue() : 1080;
    RectI fmt;
    fmt.x1 = 0; fmt.y1 = 0;
    fmt.x2 = std::max(1, w);
    fmt.y2 = std::max(1, h);
    metadata.setOutputFormat(fmt);
    return eStatusOK;
}

StatusEnum
RenderPass::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                   ViewIdx /*view*/, RectD* rod)
{
    int w = _imp->outputWidth.lock()  ? _imp->outputWidth.lock()->getValue()  : 1920;
    int h = _imp->outputHeight.lock() ? _imp->outputHeight.lock()->getValue() : 1080;
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = std::max(1, w);
    rod->y2 = std::max(1, h);
    return eStatusOK;
}

StatusEnum
RenderPass::render(const RenderActionArgs& args)
{
    if ( args.outputPlanes.empty() ) {
        return eStatusFailed;
    }
    if ( !args.outputPlanes.front().second ) {
        return eStatusFailed;
    }

#ifdef NATRON_CYCLES
    // --- Resolution (half-res preview like CyclesRender) ---
    int outW = _imp->outputWidth.lock()  ? _imp->outputWidth.lock()->getValue()  : 1920;
    int outH = _imp->outputHeight.lock() ? _imp->outputHeight.lock()->getValue() : 1080;
    if (outW <= 0) outW = 1920;
    if (outH <= 0) outH = 1080;
    const bool isPreview = _imp->previewMode.lock() && _imp->previewMode.lock()->getValue();
    const int renderW = isPreview ? std::max(64, outW / 2) : outW;
    const int renderH = isPreview ? std::max(64, outH / 2) : outH;

    // --- Optional CyclesRenderSettings (input 2) ---
    const CyclesRenderSettings* settings = nullptr;
    if ( EffectInstancePtr se = getInput(2) ) {
        settings = dynamic_cast<const CyclesRenderSettings*>( se.get() );
    }
    const int renderSamples = settings
        ? settings->getSamples(args.time)
        : (_imp->samples.lock() ? _imp->samples.lock()->getValue() : 16);

    // --- Camera (input 1) → passed to the helper as cameraOverride ---
    const CameraProvider* cam = nullptr;
    if ( EffectInstancePtr ce = getInput(1) ) {
        cam = dynamic_cast<const CameraProvider*>( ce.get() );
    }

    // --- Integrator / DOF / Motion Blur: only from a connected Settings node
    //     (the preview keeps a lean knob set; full control lives on the Settings
    //     node, shared with the final render). ---
    CyclesRenderer::IntegratorParams integParams;
    CyclesRenderer::DOFParams        dofParams;
    CyclesRenderer::MotionBlurParams mbParams;
    if (settings) {
        integParams.maxBounces          = settings->getMaxBounces(args.time);
        integParams.diffuseBounces      = settings->getDiffuseBounces(args.time);
        integParams.glossyBounces       = settings->getGlossyBounces(args.time);
        integParams.transmissionBounces = settings->getTransmissionBounces(args.time);
        if ( settings->getDOFEnabled(args.time) && cam ) {
            dofParams.enabled = true;
            double camFL = cam->getCameraFocalLength(args.time);
            double fstop = cam->getCameraFStop(args.time);
            if (fstop < 0.1) fstop = 0.1;
            dofParams.apertureSize  = (float)(camFL / (2.0 * fstop) / 1000.0);
            dofParams.focusDistance = (float)settings->getFocusDistance(args.time);
            dofParams.blades        = settings->getBokehBlades(args.time);
            dofParams.bladeRotation = (float)(settings->getBladeRotation(args.time) * M_PI / 180.0);
        }
        if ( settings->getMotionBlurEnabled(args.time) ) {
            mbParams.enabled         = true;
            mbParams.shutterTime     = (float)settings->getShutterTime(args.time);
            mbParams.shutterPosition = settings->getShutterPosition(args.time);
        }
    }

    // --- Build the request and prepare the scene from THIS node's input 0 ---
    CyclesPassRequest req;
    req.time            = args.time;
    req.view            = ViewIdx(0);
    req.width           = renderW;
    req.height          = renderH;
    req.samples         = renderSamples;
    req.requestedPasses = getEnabledPassesRP(_imp.get());
    req.transparentBg   = false;
    req.integrator      = &integParams;
    if (dofParams.enabled) req.dof = &dofParams;
    if (mbParams.enabled)  req.mb  = &mbParams;
    req.cameraOverride  = cam;

    CyclesPassPrepared prepared;
    {
        std::string prepErr;
        if ( !prepareCyclesPasses(this, req, prepared, prepErr, /*sceneInputSlot=*/0) ) {
            return eStatusFailed;
        }
    }

    // --- Apply THIS pass's own visibility / light setup (the whole point) ---
    refreshObjectLists();
    std::map<std::string, ObjectVisibility> visMap = getObjectVisibilityMap();
    std::set<std::string>                   activeLightSet = getActiveLights();
    std::map<std::string, LightRayVis>      lightRayVis = getLightRayVisibility();
    if ( !visMap.empty() )         req.visMap       = &visMap;
    if ( !activeLightSet.empty() ) req.activeLights = &activeLightSet;
    if ( !lightRayVis.empty() )    req.lightRayVis  = &lightRayVis;

    // --- Render (cancel any in-flight preview first) ---
    if (_imp->activeRenderer) {
        _imp->activeRenderer->cancelRender();
        _imp->activeRenderer.reset();
    }
    _imp->activeRenderer = std::unique_ptr<CyclesRenderer>(new CyclesRenderer());
    std::map<std::string, std::vector<float>> passBuffers;
    {
        std::string execErr;
        if ( !executeCyclesPasses(*_imp->activeRenderer, prepared, req, passBuffers, execErr) ) {
            _imp->activeRenderer.reset();
            return eStatusFailed;
        }
    }

    const int srcW = renderW;
    const int srcH = renderH;

    // --- Fill each requested output plane from its matching pass buffer
    //     (mirror CyclesRender: bottom-up, nearest upscale from the preview
    //     buffer; per-AOV display tweaks for Depth / Mist / AO). ---
    for (auto& pp : args.outputPlanes) {
        const ImagePlaneDesc& pd = pp.first;
        ImagePtr planeImg = pp.second;
        if (!planeImg) continue;

        // Match this plane to one of the rendered passes.
        std::string passName;
        for (size_t pi = 0; pi < req.requestedPasses.size(); ++pi) {
            if ( passNameToPlaneRP(req.requestedPasses[pi]).getPlaneID() == pd.getPlaneID() ) {
                passName = req.requestedPasses[pi];
                break;
            }
        }
        if ( passName.empty() && pd.getNumComponents() == 4 ) passName = "Combined";
        if ( passName.empty() ) continue;

        std::map<std::string, std::vector<float>>::const_iterator bit = passBuffers.find(passName);
        if ( bit == passBuffers.end() || bit->second.empty() ) continue;
        const std::vector<float>& src = bit->second;

        const int numOutComps = pd.getNumComponents();
        const RectI outBounds = planeImg->getBounds();
        Image::WriteAccess wa( planeImg.get() );
        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const int fbX = x - outBounds.x1;
                const int fbY = y - outBounds.y1;
                const int dstW = outBounds.width();
                const int dstH = outBounds.height();
                int sx = (srcW == dstW) ? fbX : (fbX * srcW / dstW);
                int sy = (srcH == dstH) ? fbY : (fbY * srcH / dstH);
                sx = std::min(sx, srcW - 1);
                sy = std::min(sy, srcH - 1);
                const int idx = (sy * srcW + sx) * 4;
                float r = src[idx + 0], g = src[idx + 1], b = src[idx + 2], a = src[idx + 3];
                if (passName == "Depth") {
                    if (r >= 1e9f) r = 0.0f;   // infinity → black
                    g = b = r; a = 1.0f;
                } else if (passName == "Mist" || passName == "AO") {
                    g = b = r; a = 1.0f;       // single-value → grayscale
                }
                dst[0] = r;
                if (numOutComps > 1) dst[1] = g;
                if (numOutComps > 2) dst[2] = b;
                if (numOutComps > 3) dst[3] = a;
            }
        }
    }
    return eStatusOK;
#else
    // No Cycles in this build — clear to black so the viewer shows nothing
    // rather than uninitialized memory.
    const ImagePlaneDesc& planeDesc = args.outputPlanes.front().first;
    ImagePtr outImg = args.outputPlanes.front().second;
    const int numOutComps = planeDesc.getNumComponents();
    const RectI outBounds = outImg->getBounds();
    Image::WriteAccess wa( outImg.get() );
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;
            for (int c = 0; c < numOutComps; ++c) dst[c] = 0.f;
        }
    }
    return eStatusOK;
#endif // NATRON_CYCLES
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_RenderPass.cpp"
