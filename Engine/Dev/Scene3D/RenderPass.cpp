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

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

#include "Scene3D.h"
#include "Group3D.h"
#include "Light3D.h"

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

    // Lights
    KnobGroupWPtr lightsGroup;
    KnobBoolWPtr  activeLightsKnobs[RENDERPASS_MAX_OBJECTS];

    // Cached discovered names (index matches knob index)
    std::vector<std::string> geoNames;
    std::vector<std::string> lightNames;
};


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

    createObjectBoolGroup(this, page, tr("Shadow Catchers"), "shadowCatch",
                          tr("Objects that receive shadows but render transparent."),
                          _imp->shadowCatcherGroup, _imp->shadowCatchers, RENDERPASS_MAX_OBJECTS);

    createObjectBoolGroup(this, page, tr("Excluded Objects"), "exclObj",
                          tr("Objects completely removed from the render."),
                          _imp->excludedGroup, _imp->excludedObjects, RENDERPASS_MAX_OBJECTS);

    // --- Lights page ---
    KnobPagePtr lightsPage = AppManager::createKnob<KnobPage>(this, tr("Lights"));

    createObjectBoolGroup(this, lightsPage, tr("Active Lights"), "activeLight",
                          tr("Which lights contribute to this pass. If none checked, all lights are used."),
                          _imp->lightsGroup, _imp->activeLightsKnobs, RENDERPASS_MAX_OBJECTS);
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

    // Update lights
    updateBoolKnobs(_imp->activeLightsKnobs, RENDERPASS_MAX_OBJECTS, lights, false);
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

    // Get the connected Scene input and traverse its inputs to find all nodes
    EffectInstancePtr sceneEffect = getInput(0);
    if (!sceneEffect) return;

    // Check if it's a Scene3D or Group3D
    Scene3D* scene3d = dynamic_cast<Scene3D*>(sceneEffect.get());
    Group3D* group3d = dynamic_cast<Group3D*>(sceneEffect.get());

    // Collect all connected nodes from the Scene
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

        // Check if it's a light
        Light3D* light = dynamic_cast<Light3D*>(eff.get());
        if (light) {
            outLights.push_back(name);
        } else {
            outGeo.push_back(name);
        }
    }
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

    // Empty set = all lights active (convention from the plan)
    return lights;
}

// ---------------------------------------------------------------------------
// Render (pass-through)
// ---------------------------------------------------------------------------

StatusEnum
RenderPass::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                   ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
RenderPass::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_RenderPass.cpp"
