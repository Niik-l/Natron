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

#include "CyclesRenderPass.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Project.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../KnobFile.h"
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

// Live Cycles preview path. CyclesRenderPass is always compiled (Scene3D), but the
// Cycles renderer only exists when NATRON_CYCLES is on — so the preview render
// is gated and falls back to a no-op stub otherwise.
#ifdef NATRON_CYCLES
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QProcess>
#include <cstdlib>   // std::getenv (NATRON_RV_PATH)
#include "CameraProvider.h"
#include "../DotUtils.h"
#include "../Cycles/CyclesPassRender.h"
#include "../Deep/DeepImage.h"
#include "../Deep/DeepUtils.h"
#include "../Cycles/CyclesRenderer.h"
#include "../Cycles/CyclesRenderSettings.h"
#include "../../CreateNodeArgs.h"
#include "../../NodeGroup.h"
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
    // Deep output (AOV tab) + deep write (Output tab)
    KnobBoolWPtr   deepOutput;
    KnobIntWPtr    deepMaxSamples;
    KnobChoiceWPtr deepChannels;
    KnobDoubleWPtr deepMergeThreshold;
    KnobDoubleWPtr deepAlphaMergeThreshold;
    KnobChoiceWPtr deepCompression;
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
    KnobBoolWPtr denoise;       // OpenImageDenoise toggle

    // AOV enable knobs (mirror CyclesRender's AOV Passes tab)
    KnobButtonWPtr refreshPassesBtn;
    KnobBoolWPtr aovDiffDir, aovDiffInd, aovDiffCol, aovDiffuse, aovTrans;
    KnobBoolWPtr aovGlossDir, aovGlossInd, aovGlossCol, aovGlossy;
    KnobBoolWPtr aovEmission, aovEnv, aovAO;
    KnobBoolWPtr aovNormal, aovDepth, aovUV, aovMist;
    KnobBoolWPtr aovReflMatte;

    // Output / render-to-disk (Output page)
    KnobIntWPtr    frameStart, frameEnd, frameInc;
    KnobButtonWPtr renderToDiskBtn;
    KnobFileWPtr   rvPathKnob;
    KnobButtonWPtr openInRvBtn;
    KnobButtonWPtr importRenderBtn;
    KnobButtonWPtr updateRenderBtn;
    KnobStringWPtr linkStatus;        // read-only: linked-Read status / outdated flag
    KnobStringWPtr linkedReadName;    // hidden + persistent: linked Read's script name
    NodeWPtr       linkedReadNode;    // in-session cache of the linked Read

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
// plane IDs match — a CyclesRenderPass preview and a CyclesRender produce identical AOV
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

CyclesRenderPass::CyclesRenderPass(NodePtr node)
    : EffectInstance(node)
    , _imp(new RenderPassPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

CyclesRenderPass::~CyclesRenderPass()
{
}

std::string
CyclesRenderPass::getPluginDescription() const
{
    return tr("Render pass filter for multi-pass rendering.\n\n"
              "Sits between Scene3D and CyclesRender. Defines which objects are visible "
              "to camera, which are trace-only (phantom), holdouts, and shadow catchers.\n\n"
              "Each CyclesRender with a CyclesRenderPass produces a separate element for compositing.").toStdString();
}

std::string
CyclesRenderPass::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "scene";
    if (inputNb == 1) return "camera";
    if (inputNb == 2) return "settings";
    return std::string();
}

void
CyclesRenderPass::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
CyclesRenderPass::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
CyclesRenderPass::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

// Helper to create a group of RENDERPASS_MAX_OBJECTS hidden bool knobs
static void createObjectBoolGroup(CyclesRenderPass* self,
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
CyclesRenderPass::initializeKnobs()
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
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Denoise"));
        k->setName("denoise"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Run OpenImageDenoise (CPU) on the result. Cleans up noise at low "
                             "sample counts; adds a denoise pass after rendering."));
        previewPage->addKnob(k); _imp->denoise = k;
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
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Deep"));
        k->setName("deepOutput"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Accumulate per-pixel deep samples during this pass's preview "
                             "render and publish them as a deep image — connect this node to "
                             "any Deep node. Save from the Output tab (Write Deep EXR)."));
        aovPage->addKnob(k); _imp->deepOutput = k;
    }

    // --- Output page: write this pass to disk over a frame range ---
    // Path = <Output Path>/<Pass Name>/v###/<Pass Name>.####.exr, where Output Path
    // comes from a connected CyclesRenderSettings (else the project folder) and the
    // version auto-increments per render. One multi-layer EXR per frame (Combined +
    // every enabled AOV as layers).
    {
        KnobPagePtr outPage = AppManager::createKnob<KnobPage>(this, tr("Output"));

        // Auto-fill the frame range from the project on creation.
        double pf = 1.0, pl = 1.0;
        if (getApp() && getApp()->getProject()) {
            getApp()->getProject()->getFrameRange(&pf, &pl);
        }
        {
            KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Range"));
            k->setName("frameStart"); k->setDefaultValue((int)pf);
            k->setHintToolTip(tr("First frame to render to disk (auto-filled from the project frame range)."));
            k->setAddNewLine(false);
            outPage->addKnob(k); _imp->frameStart = k;
        }
        {
            KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("End"));
            k->setName("frameEnd"); k->setDefaultValue((int)pl);
            k->setHintToolTip(tr("Last frame to render to disk."));
            k->setAddNewLine(false);
            outPage->addKnob(k); _imp->frameEnd = k;
        }
        {
            KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Increment"));
            k->setName("frameInc"); k->setDefaultValue(1); k->setMinimum(1);
            k->setHintToolTip(tr("Frame step."));
            outPage->addKnob(k); _imp->frameInc = k;
        }
        {
            KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Render to Disk"));
            k->setName("renderToDisk");
            k->setHintToolTip(tr(
                "Render this pass over the frame range to multi-layer EXRs:\n"
                "    <Output Path>/<Pass Name>/v###/<Pass Name>.####.exr\n\n"
                "Output Path is read from a connected CyclesRenderSettings node (input 2); "
                "if none is connected it falls back to the project folder. Each render "
                "auto-increments the version (v001, v002, ...). The beauty (Combined) plus "
                "every AOV you enabled on the AOV Passes tab are written as named layers in "
                "one EXR per frame.\n\n"
                "Note: this blocks the UI while the range renders — progress prints to the "
                "terminal."));
            outPage->addKnob(k); _imp->renderToDiskBtn = k;
        }

        // --- Review / import the written sequence ---
        {
            KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("RV Executable"));
            k->setName("rvPath");
            k->setAnimationEnabled(false);
            k->setEvaluateOnChange(false);
            k->setHintToolTip(tr("Path to the RV / OpenRV executable (rv.exe on Windows). Used by "
                                 "'Open in RV'. Defaults to the NATRON_RV_PATH environment variable if set."));
            if (const char* envRv = std::getenv("NATRON_RV_PATH")) {
                if (envRv[0] != '\0') k->setDefaultValue(envRv);
            }
            outPage->addKnob(k); _imp->rvPathKnob = k;
        }
        {
            KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Open in RV"));
            k->setName("openInRv"); k->setAddNewLine(false);
            k->setHintToolTip(tr("Launch RV / OpenRV on the latest rendered version's sequence. "
                                 "Requires the RV Executable path (or NATRON_RV_PATH)."));
            outPage->addKnob(k); _imp->openInRvBtn = k;
        }
        {
            KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Import Render"));
            k->setName("importRender"); k->setAddNewLine(false);
            k->setHintToolTip(tr("Create a Read node reading the latest rendered version of this pass, "
                                 "linked to this node. If a linked Read already exists it is re-pointed "
                                 "to the latest version. The link survives save/reload."));
            outPage->addKnob(k); _imp->importRenderBtn = k;
        }
        {
            KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Update Render"));
            k->setName("updateRender");
            k->setHintToolTip(tr("Re-point the linked Read to the latest version on disk and clear its "
                                 "outdated flag. Use this after a re-render when you're happy with the new "
                                 "version (nothing changes automatically, so a broken re-render can't sneak in)."));
            outPage->addKnob(k); _imp->updateRenderBtn = k;
        }
        {
            KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Linked Read"));
            k->setName("linkStatus");
            k->setAsLabel();   // read-only status line
            k->setHintToolTip(tr("Status of the imported Read: up to date, outdated, or none."));
            k->setDefaultValue("No linked Read (use Import Render).");
            outPage->addKnob(k); _imp->linkStatus = k;
        }
        {
            // Hidden + persistent: remembers the linked Read across save/reload.
            KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Linked Read Name"));
            k->setName("linkedReadName");
            k->setSecretByDefault(true);
            k->setEvaluateOnChange(false);
            outPage->addKnob(k); _imp->linkedReadName = k;
        }
        {
            KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Deep Max Samples"));
            k->setName("deepMaxSamples"); k->setDefaultValue(32);
            k->setMinimum(1); k->setDisplayMinimum(4); k->setDisplayMaximum(128);
            k->setHintToolTip(tr("Kernel-side cap on deep samples per pixel."));
            outPage->addKnob(k); _imp->deepMaxSamples = k;
        }
        {
            KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Deep Channels"));
            k->setName("deepChannels");
            k->setHintToolTip(tr("RGBA: full recolored deep. Alpha + Depth: DCM-style A/Z/ZBack "
                                 "only — smaller, recolor in comp with DeepRecolor."));
            {
                std::vector<ChoiceOption> opts;
                opts.push_back(ChoiceOption("RGBA", "", "Full recolored deep samples"));
                opts.push_back(ChoiceOption("Alpha + Depth (DCM)", "", "A/Z/ZBack only"));
                k->populateChoices(opts);
            }
            k->setDefaultValue(0);
            outPage->addKnob(k); _imp->deepChannels = k;
        }
        {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Deep Merge Threshold"));
            k->setName("deepMergeThreshold"); k->setDefaultValue(0.001);
            k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
            k->setHintToolTip(tr("Depth tolerance for merging nearby samples (sample compression)."));
            outPage->addKnob(k); _imp->deepMergeThreshold = k;
        }
        {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Deep Alpha Merge Threshold"));
            k->setName("deepAlphaMergeThreshold"); k->setDefaultValue(0.01);
            k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
            k->setHintToolTip(tr("Alpha tolerance for merging nearby samples."));
            outPage->addKnob(k); _imp->deepAlphaMergeThreshold = k;
        }
        {
            KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Deep Compression"));
            k->setName("deepCompression");
            k->setHintToolTip(tr("EXR compression for the deep sequence files written by Render to Disk (<pass>_deep.####.exr). Deep EXR supports Zips/RLE/None."));
            {
                std::vector<ChoiceOption> opts;
                opts.push_back(ChoiceOption("Zips", "", "zip per scanline (recommended)"));
                opts.push_back(ChoiceOption("RLE", "", "run-length encoding"));
                opts.push_back(ChoiceOption("None", "", "uncompressed"));
                k->populateChoices(opts);
            }
            k->setDefaultValue(0);
            outPage->addKnob(k); _imp->deepCompression = k;
        }
    }
}

bool
CyclesRenderPass::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
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

    // Render to Disk — write this pass over the frame range to multi-layer EXRs.
    KnobButtonPtr rtd = _imp->renderToDiskBtn.lock();
    if (rtd && k == rtd.get()) {
        renderToDisk();
        return true;
    }
    if (KnobButtonPtr b = _imp->openInRvBtn.lock())     { if (k == b.get()) { openInRV();     return true; } }
    if (KnobButtonPtr b = _imp->importRenderBtn.lock()) { if (k == b.get()) { importRender(); return true; } }
    if (KnobButtonPtr b = _imp->updateRenderBtn.lock()) { if (k == b.get()) { updateRender(); return true; } }

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
CyclesRenderPass::refreshObjectLists()
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
CyclesRenderPass::getPassName() const
{
    KnobStringPtr k = _imp->passName.lock();
    return k ? k->getValue() : "beauty";
}

void
CyclesRenderPass::discoverSceneObjects(std::vector<std::string>& outGeo,
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
CyclesRenderPass::getObjectVisibilityMap() const
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
CyclesRenderPass::getActiveLights() const
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
    // Centralised here so it applies both to CyclesRenderPass's own preview and to a
    // downstream CyclesRender that reads this selection (CyclesRender.cpp:906/966).
    if (lights.empty()) {
        lights.insert(std::string("\x01__renderpass_no_lights__"));
    }
    return lights;
}

std::map<std::string, LightRayVis>
CyclesRenderPass::getLightRayVisibility() const
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
CyclesRenderPass::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
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
CyclesRenderPass::getPreferredMetadata(NodeMetadata& metadata)
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
CyclesRenderPass::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
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
CyclesRenderPass::render(const RenderActionArgs& args)
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
    if ( EffectInstancePtr se = skipDots( getInput(2) ) ) {
        settings = dynamic_cast<const CyclesRenderSettings*>( se.get() );
    }
    const int renderSamples = settings
        ? settings->getSamples(args.time)
        : (_imp->samples.lock() ? _imp->samples.lock()->getValue() : 16);

    // --- Camera (input 1) → passed to the helper as cameraOverride ---
    const CameraProvider* cam = nullptr;
    if ( EffectInstancePtr ce = skipDots( getInput(1) ) ) {
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
    req.denoise         = _imp->denoise.lock() && _imp->denoise.lock()->getValue();
    req.integrator      = &integParams;
    if (dofParams.enabled) req.dof = &dofParams;
    if (mbParams.enabled)  req.mb  = &mbParams;
    req.cameraOverride  = cam;

    // Deep output (AOV tab)
    const bool deepEnabled = _imp->deepOutput.lock() && _imp->deepOutput.lock()->getValue();
    const bool deepAlphaOnly = _imp->deepChannels.lock()
                               && _imp->deepChannels.lock()->getValue() == 1;
    CyclesRenderer::DeepPixelData deepRaw;
    if (deepEnabled) {
        req.outDeep = &deepRaw;
        req.deepMaxSamples = _imp->deepMaxSamples.lock()
                             ? std::max(1, _imp->deepMaxSamples.lock()->getValue()) : 32;
        req.deepMergeThreshold = _imp->deepMergeThreshold.lock()
                             ? (float)_imp->deepMergeThreshold.lock()->getValue() : 0.001f;
        req.deepAlphaMergeThreshold = _imp->deepAlphaMergeThreshold.lock()
                             ? (float)_imp->deepAlphaMergeThreshold.lock()->getValue() : 0.01f;
    }

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

    // Publish the deep snapshot for the deep suite (immutable, provider contract)
    if (deepEnabled) {
        DeepImagePtr deep = deepImageFromCyclesDeepData(deepRaw, renderW, renderH,
                                                        deepAlphaOnly);
        if (deep) {
            {
                std::lock_guard<std::mutex> l(_deepMutex);
                _lastDeepImage = deep;
            }
            fprintf(stderr, "[CyclesDeep] pass published %zu deep samples (%dx%d%s)\n",
                    (size_t)deep->totalSamples(), renderW, renderH,
                    deepAlphaOnly ? ", A/Z/ZBack" : "");
        } else {
            setPersistentMessage(eMessageTypeWarning,
                tr("Deep Output enabled but no deep data came back from Cycles — "
                   "see console for details.").toStdString());
            { std::lock_guard<std::mutex> l(_deepMutex); _lastDeepImage.reset(); }
        }
    } else {
        { std::lock_guard<std::mutex> l(_deepMutex); _lastDeepImage.reset(); }
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

#ifdef NATRON_CYCLES
// Resolve <Output Path>/<Pass Name> as an absolute dir (creates nothing). Output Path
// comes from a connected CyclesRenderSettings (input 2), else the project folder.
// Returns empty if no base path is resolvable; fills passNameOut ("beauty" if unset).
static QString
resolvePassDir(CyclesRenderPass* self, std::string& passNameOut)
{
    std::string baseDir;
    if (EffectInstancePtr se = skipDots(self->getInput(2))) {
        if (const CyclesRenderSettings* s = dynamic_cast<const CyclesRenderSettings*>(se.get()))
            baseDir = s->getOutputPath(0.0);
    }
    if (baseDir.empty() && self->getApp() && self->getApp()->getProject())
        baseDir = self->getApp()->getProject()->getProjectPath().toStdString();
    if (baseDir.empty()) return QString();
    passNameOut = self->getPassName().empty() ? std::string("beauty") : self->getPassName();
    return QDir(QString::fromStdString(baseDir)).absoluteFilePath(QString::fromStdString(passNameOut));
}

// Highest existing v### in passDir, or 0 if none.
static int
maxVersionInDir(const QDir& passDir)
{
    int maxV = 0;
    const QStringList vs = passDir.entryList(QStringList() << QString::fromUtf8("v[0-9][0-9][0-9]*"),
                                             QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& v : vs) {
        bool ok = false;
        const int n = v.mid(1).toInt(&ok);
        if (ok && n > maxV) maxV = n;
    }
    return maxV;
}

// <passDir>/v###/<passName>.####.exr for a given version.
static QString
seqPatternForVersion(const QString& passDir, const std::string& passName, int version)
{
    const QString vname = QString::fromUtf8("v%1").arg(version, 3, 10, QLatin1Char('0'));
    const QString fname = QString::fromUtf8("%1.####.exr").arg(QString::fromStdString(passName));
    return QDir(passDir).absoluteFilePath(vname + QLatin1Char('/') + fname);
}

// Parse the v### number from a sequence path's version folder; -1 if not found.
static int
versionFromPath(const QString& filePath)
{
    const QString vdir = QFileInfo(filePath).dir().dirName();
    if (vdir.startsWith(QLatin1Char('v'))) {
        bool ok = false;
        const int n = vdir.mid(1).toInt(&ok);
        if (ok) return n;
    }
    return -1;
}

// Render one frame of this pass to pass buffers (Combined + every enabled AOV),
// mirroring render()'s request build but at full resolution. Used by renderToDisk().
static bool
renderPassFrameToBuffers(CyclesRenderPass* self,
                         CyclesRenderer& renderer,
                         double time, int w, int h, int fallbackSamples,
                         const std::vector<std::string>& requestedPasses, bool denoise,
                         std::map<std::string, std::vector<float>>& outBuffers,
                         std::string& errOut,
                         CyclesRenderer::DeepPixelData* outDeep = nullptr,
                         int deepMaxSamples = 32,
                         float deepMergeThreshold = 0.001f,
                         float deepAlphaMergeThreshold = 0.01f)
{
    const CyclesRenderSettings* settings = nullptr;
    if (EffectInstancePtr se = skipDots(self->getInput(2)))
        settings = dynamic_cast<const CyclesRenderSettings*>(se.get());
    const CameraProvider* cam = nullptr;
    if (EffectInstancePtr ce = skipDots(self->getInput(1)))
        cam = dynamic_cast<const CameraProvider*>(ce.get());

    const int samples = settings ? settings->getSamples(time) : fallbackSamples;

    CyclesRenderer::IntegratorParams integ;
    CyclesRenderer::DOFParams        dof;
    CyclesRenderer::MotionBlurParams mb;
    if (settings) {
        integ.maxBounces          = settings->getMaxBounces(time);
        integ.diffuseBounces      = settings->getDiffuseBounces(time);
        integ.glossyBounces       = settings->getGlossyBounces(time);
        integ.transmissionBounces = settings->getTransmissionBounces(time);
        if (settings->getDOFEnabled(time) && cam) {
            dof.enabled = true;
            double camFL = cam->getCameraFocalLength(time);
            double fstop = cam->getCameraFStop(time);
            if (fstop < 0.1) fstop = 0.1;
            dof.apertureSize  = (float)(camFL / (2.0 * fstop) / 1000.0);
            dof.focusDistance = (float)settings->getFocusDistance(time);
            dof.blades        = settings->getBokehBlades(time);
            dof.bladeRotation = (float)(settings->getBladeRotation(time) * M_PI / 180.0);
        }
        if (settings->getMotionBlurEnabled(time)) {
            mb.enabled         = true;
            mb.shutterTime     = (float)settings->getShutterTime(time);
            mb.shutterPosition = settings->getShutterPosition(time);
        }
    }

    CyclesPassRequest req;
    req.time            = time;
    req.view            = ViewIdx(0);
    req.width           = w;
    req.height          = h;
    req.samples         = samples;
    req.requestedPasses = requestedPasses;
    req.transparentBg   = false;
    req.denoise         = denoise;
    req.integrator      = &integ;
    if (dof.enabled) req.dof = &dof;
    if (mb.enabled)  req.mb  = &mb;
    req.cameraOverride  = cam;

    if (outDeep) {
        req.outDeep = outDeep;
        req.deepMaxSamples = deepMaxSamples;
        req.deepMergeThreshold = deepMergeThreshold;
        req.deepAlphaMergeThreshold = deepAlphaMergeThreshold;
    }

    CyclesPassPrepared prepared;
    if (!prepareCyclesPasses(self, req, prepared, errOut, /*sceneInputSlot=*/0))
        return false;

    std::map<std::string, ObjectVisibility> visMap       = self->getObjectVisibilityMap();
    std::set<std::string>                   activeLights = self->getActiveLights();
    std::map<std::string, LightRayVis>      lightRayVis  = self->getLightRayVisibility();
    if (!visMap.empty())       req.visMap       = &visMap;
    if (!activeLights.empty()) req.activeLights = &activeLights;
    if (!lightRayVis.empty())  req.lightRayVis  = &lightRayVis;

    return executeCyclesPasses(renderer, prepared, req, outBuffers, errOut);
}
#endif // NATRON_CYCLES

void
CyclesRenderPass::renderToDisk()
{
#ifdef NATRON_CYCLES
    clearPersistentMessage(false);

    // --- <base>/<passName> (Output Path from Settings, else project folder) ---
    std::string passName;
    const QString passDirPath = resolvePassDir(this, passName);
    if (passDirPath.isEmpty()) {
        setPersistentMessage(eMessageTypeError,
            "Render to Disk: no output path — connect a CyclesRenderSettings node (set its "
            "Output Path) or save the project first.");
        return;
    }
    QDir passDir(passDirPath);
    if (!passDir.exists() && !passDir.mkpath(QString::fromUtf8("."))) {
        setPersistentMessage(eMessageTypeError,
            std::string("Render to Disk: could not create ") + passDir.absolutePath().toStdString());
        return;
    }

    // --- Next free version folder (v001, v002, ...) ---
    const QString vname = QString::fromUtf8("v%1").arg(maxVersionInDir(passDir) + 1, 3, 10, QLatin1Char('0'));
    QDir versionDir( passDir.absoluteFilePath(vname) );
    if (!versionDir.mkpath(QString::fromUtf8("."))) {
        setPersistentMessage(eMessageTypeError,
            std::string("Render to Disk: could not create ") + versionDir.absolutePath().toStdString());
        return;
    }

    // --- Frame range + render params ---
    int fStart = _imp->frameStart.lock() ? _imp->frameStart.lock()->getValue() : 1;
    int fEnd   = _imp->frameEnd.lock()   ? _imp->frameEnd.lock()->getValue()   : fStart;
    int fInc   = _imp->frameInc.lock()   ? _imp->frameInc.lock()->getValue()   : 1;
    if (fInc < 1) fInc = 1;
    if (fEnd < fStart) std::swap(fStart, fEnd);

    int outW = _imp->outputWidth.lock()  ? _imp->outputWidth.lock()->getValue()  : 1920;
    int outH = _imp->outputHeight.lock() ? _imp->outputHeight.lock()->getValue() : 1080;
    if (outW <= 0) outW = 1920;
    if (outH <= 0) outH = 1080;
    const int  fallbackSamples = _imp->samples.lock() ? _imp->samples.lock()->getValue() : 16;
    const bool denoise = _imp->denoise.lock() && _imp->denoise.lock()->getValue();
    const std::vector<std::string> passes = getEnabledPassesRP(_imp.get());

    refreshObjectLists();

    // Cancel any in-flight preview so it doesn't contend with the disk render.
    if (_imp->activeRenderer) { _imp->activeRenderer->cancelRender(); _imp->activeRenderer.reset(); }

    CyclesRenderer::ExrOutputOptions opts;
    opts.bitDepth    = "16-bit Half";
    opts.compression = "ZIP";

    CyclesRenderer renderer;
    int nframes = 0, nok = 0;
    const int total = ((fEnd - fStart) / fInc) + 1;
    fprintf(stderr, "[CyclesRenderPass] Render to Disk '%s' -> %s  frames %d..%d step %d\n",
            passName.c_str(), versionDir.absolutePath().toStdString().c_str(), fStart, fEnd, fInc);

    // Native progress bar (with Cancel). progressUpdate pumps the event loop so the
    // UI stays responsive during the blocking range render; it returns false when
    // the user hits Cancel. Cancel is polled between frames (per-frame granularity).
    AppInstancePtr app  = getApp();
    NodePtr        node = getNode();
    const bool useProgress = (app && node);
    if (useProgress) {
        app->progressStart(node,
            std::string("Rendering '") + passName + "' to disk -> " + vname.toStdString()
                + " (" + std::to_string(total) + (total == 1 ? " frame)" : " frames)"),
            std::string());
    }

    // Deep sequence output (Deep on the AOV tab): each frame also writes
    // <passName>_deep.####.exr into the same version folder.
    const bool seqDeep = _imp->deepOutput.lock() && _imp->deepOutput.lock()->getValue();
    const bool seqDeepAlphaOnly = _imp->deepChannels.lock()
                                  && _imp->deepChannels.lock()->getValue() == 1;
    const int seqDeepMax = _imp->deepMaxSamples.lock()
                           ? std::max(1, _imp->deepMaxSamples.lock()->getValue()) : 32;
    const float seqDeepMerge = _imp->deepMergeThreshold.lock()
                           ? (float)_imp->deepMergeThreshold.lock()->getValue() : 0.001f;
    const float seqDeepAlphaMerge = _imp->deepAlphaMergeThreshold.lock()
                           ? (float)_imp->deepAlphaMergeThreshold.lock()->getValue() : 0.01f;
    std::string seqDeepCompression = "zips";
    if (_imp->deepCompression.lock()) {
        const int ci = _imp->deepCompression.lock()->getValue();
        seqDeepCompression = (ci == 1) ? "rle" : (ci == 2) ? "none" : "zips";
    }
    int nDeepOk = 0;

    bool canceled = false;
    for (int f = fStart; f <= fEnd && !canceled; f += fInc) {
        ++nframes;
        std::map<std::string, std::vector<float>> buffers;
        std::string err;
        CyclesRenderer::DeepPixelData deepRaw;
        if (renderPassFrameToBuffers(this, renderer, (double)f, outW, outH,
                                     fallbackSamples, passes, denoise, buffers, err,
                                     seqDeep ? &deepRaw : nullptr,
                                     seqDeepMax, seqDeepMerge, seqDeepAlphaMerge)) {
            const QString fname = QString::fromUtf8("%1.%2.exr")
                .arg(QString::fromStdString(passName)).arg(f, 4, 10, QLatin1Char('0'));
            const std::string filepath = versionDir.absoluteFilePath(fname).toStdString();
            if (CyclesRenderer::saveMultiLayerEXR(filepath, buffers, outW, outH, opts)) {
                ++nok;
                fprintf(stderr, "[CyclesRenderPass]   frame %d -> %s\n", f, filepath.c_str());
            } else {
                fprintf(stderr, "[CyclesRenderPass]   frame %d: EXR write FAILED: %s\n", f, filepath.c_str());
            }
            if (seqDeep) {
                DeepImagePtr deep = deepImageFromCyclesDeepData(deepRaw, outW, outH,
                                                                seqDeepAlphaOnly);
                const QString dname = QString::fromUtf8("%1_deep.%2.exr")
                    .arg(QString::fromStdString(passName)).arg(f, 4, 10, QLatin1Char('0'));
                const std::string dpath = versionDir.absoluteFilePath(dname).toStdString();
                std::string derr;
                if (deep && writeDeepImageEXR(deep, dpath, &derr, seqDeepCompression)) {
                    ++nDeepOk;
                    fprintf(stderr, "[CyclesRenderPass]   frame %d deep -> %s (%zu samples)\n",
                            f, dpath.c_str(), (size_t)deep->totalSamples());
                } else {
                    fprintf(stderr, "[CyclesRenderPass]   frame %d deep write FAILED: %s\n",
                            f, derr.empty() ? "no deep data" : derr.c_str());
                }
            }
        } else {
            fprintf(stderr, "[CyclesRenderPass]   frame %d FAILED: %s\n", f, err.c_str());
        }
        if (useProgress && !app->progressUpdate(node, (double)nframes / (double)total)) {
            canceled = true;
        }
    }

    if (useProgress) app->progressEnd(node);

    std::string summary = "Render to Disk: wrote " + std::to_string(nok) + "/" +
        std::to_string(total) + " frame(s) to " + versionDir.absolutePath().toStdString();
    if (seqDeep) {
        summary += "  (+" + std::to_string(nDeepOk) + " deep)";
    }
    if (canceled) summary += "  (canceled)";
    fprintf(stderr, "[CyclesRenderPass] %s\n", summary.c_str());
    if (canceled || nok != total) {
        setPersistentMessage(eMessageTypeWarning, summary);
    }

    // A newer version now exists on disk — flag the linked Read (if any) as outdated.
    refreshLinkStatus();
#endif // NATRON_CYCLES
}

#ifdef NATRON_CYCLES
NodePtr
CyclesRenderPass::getLinkedReadNode() const
{
    if (NodePtr n = _imp->linkedReadNode.lock()) return n;
    KnobStringPtr nameK = _imp->linkedReadName.lock();
    const std::string nm = nameK ? nameK->getValue() : std::string();
    if (nm.empty() || !getNode()) return NodePtr();
    NodeCollectionPtr grp = getNode()->getGroup();
    if (!grp) return NodePtr();
    const NodesList nodes = grp->getNodes();
    for (const NodePtr& n : nodes) {
        if (n && n->getScriptName_mt_safe() == nm) { _imp->linkedReadNode = n; return n; }
    }
    return NodePtr();
}

// Recompute the linked-Read status: compare the version baked into the Read's path
// against the latest version on disk. Outdated -> warning badge on the Read + status
// text here; current -> clear. No-op if nothing is linked.
void
CyclesRenderPass::refreshLinkStatus()
{
    KnobStringPtr statusK = _imp->linkStatus.lock();
    NodePtr readNode = getLinkedReadNode();
    if (!readNode) {
        if (statusK) statusK->setValue("No linked Read (use Import Render).");
        return;
    }
    const std::string readName = readNode->getScriptName_mt_safe();

    int readVer = -1;
    if (KnobIPtr fk = readNode->getKnobByName("filename")) {
        if (KnobFilePtr ff = std::dynamic_pointer_cast<KnobFile>(fk))
            readVer = versionFromPath(QString::fromStdString(ff->getValue()));
    }

    std::string passName;
    const QString passDirPath = resolvePassDir(this, passName);
    const int latestVer = passDirPath.isEmpty() ? 0 : maxVersionInDir(QDir(passDirPath));

    if (latestVer > 0 && readVer > 0 && latestVer > readVer) {
        const std::string m = "Linked Read '" + readName + "': OUTDATED (v"
            + std::to_string(readVer) + " -> v" + std::to_string(latestVer)
            + ") — press Update Render to adopt the latest.";
        if (statusK) statusK->setValue(m);
        readNode->getEffectInstance()->setPersistentMessage(eMessageTypeWarning,
            "Outdated render: showing v" + std::to_string(readVer) + ", v"
            + std::to_string(latestVer) + " available. Use 'Update Render' on the CyclesRenderPass.");
    } else {
        const std::string m = "Linked Read '" + readName + "': up to date"
            + (readVer > 0 ? " (v" + std::to_string(readVer) + ")." : ".");
        if (statusK) statusK->setValue(m);
        readNode->getEffectInstance()->clearPersistentMessage(false);
    }
}

void
CyclesRenderPass::openInRV()
{
    clearPersistentMessage(false);
    std::string passName;
    const QString passDirPath = resolvePassDir(this, passName);
    const int latestVer = passDirPath.isEmpty() ? 0 : maxVersionInDir(QDir(passDirPath));
    if (latestVer <= 0) {
        setPersistentMessage(eMessageTypeError, "Open in RV: nothing rendered yet — use Render to Disk first.");
        return;
    }
    std::string rvExe = _imp->rvPathKnob.lock() ? _imp->rvPathKnob.lock()->getValue() : std::string();
    if (rvExe.empty()) {
        if (const char* envRv = std::getenv("NATRON_RV_PATH")) { if (envRv[0]) rvExe = envRv; }
    }
    if (rvExe.empty()) {
        setPersistentMessage(eMessageTypeError,
            "Open in RV: set the \"RV Executable\" path or the NATRON_RV_PATH environment variable.");
        return;
    }
    const QString pattern = seqPatternForVersion(passDirPath, passName, latestVer);
    QStringList rvArgs; rvArgs << pattern;   // RV understands ####/%04d natively
    if (!QProcess::startDetached(QString::fromStdString(rvExe), rvArgs)) {
        setPersistentMessage(eMessageTypeError,
            "Open in RV: failed to launch. Verify the RV executable path.");
    }
}

void
CyclesRenderPass::importRender()
{
    clearPersistentMessage(false);
    std::string passName;
    const QString passDirPath = resolvePassDir(this, passName);
    const int latestVer = passDirPath.isEmpty() ? 0 : maxVersionInDir(QDir(passDirPath));
    if (latestVer <= 0) {
        setPersistentMessage(eMessageTypeError, "Import Render: nothing rendered yet — use Render to Disk first.");
        return;
    }
    const std::string pattern = seqPatternForVersion(passDirPath, passName, latestVer).toStdString();

    // Already linked + still exists -> just re-point it (acts as Update too).
    if (NodePtr existing = getLinkedReadNode()) {
        if (KnobIPtr fk = existing->getKnobByName("filename")) {
            if (KnobFilePtr ff = std::dynamic_pointer_cast<KnobFile>(fk)) {
                ff->setValue(pattern);
                existing->getEffectInstance()->clearPersistentMessage(false);
            }
        }
        refreshLinkStatus();
        return;
    }

    // Create a new Read pointing at the latest version, and remember it.
    CreateNodeArgs args(PLUGINID_NATRON_READ, getNode() ? getNode()->getGroup() : NodeCollectionPtr());
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
    NodePtr readNode = getApp() ? getApp()->createReader(pattern, args) : NodePtr();
    if (!readNode) {
        setPersistentMessage(eMessageTypeError, "Import Render: failed to create the Read node.");
        return;
    }
    readNode->setLabel(passName + "_read");
    _imp->linkedReadNode = readNode;
    if (KnobStringPtr nameK = _imp->linkedReadName.lock())
        nameK->setValue(readNode->getScriptName_mt_safe());
    refreshLinkStatus();
}

void
CyclesRenderPass::updateRender()
{
    clearPersistentMessage(false);
    NodePtr readNode = getLinkedReadNode();
    if (!readNode) {
        setPersistentMessage(eMessageTypeWarning, "Update Render: no linked Read — use Import Render first.");
        return;
    }
    std::string passName;
    const QString passDirPath = resolvePassDir(this, passName);
    const int latestVer = passDirPath.isEmpty() ? 0 : maxVersionInDir(QDir(passDirPath));
    if (latestVer <= 0) {
        setPersistentMessage(eMessageTypeWarning, "Update Render: nothing rendered yet.");
        return;
    }
    const std::string pattern = seqPatternForVersion(passDirPath, passName, latestVer).toStdString();
    if (KnobIPtr fk = readNode->getKnobByName("filename")) {
        if (KnobFilePtr ff = std::dynamic_pointer_cast<KnobFile>(fk))
            ff->setValue(pattern);
    }
    readNode->getEffectInstance()->clearPersistentMessage(false);
    refreshLinkStatus();
}
#else
NodePtr CyclesRenderPass::getLinkedReadNode() const { return NodePtr(); }
void CyclesRenderPass::refreshLinkStatus() {}
void CyclesRenderPass::openInRV() {}
void CyclesRenderPass::importRender() {}
void CyclesRenderPass::updateRender() {}
#endif // NATRON_CYCLES

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_CyclesRenderPass.cpp"
