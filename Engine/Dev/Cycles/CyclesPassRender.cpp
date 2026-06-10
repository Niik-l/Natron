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

#include "CyclesPassRender.h"

#include <memory>
#include <set>

#include "../../Node.h"

#include "../DotUtils.h"
#include "../Scene3D/CameraProvider.h"
#include "../Scene3D/Group3D.h"
#include "../Scene3D/Light3D.h"
#include "../Scene3D/Material3D.h"
#include "../Scene3D/MaterialProvider.h"
#include "../Scene3D/CyclesRenderPass.h"
#include "../Scene3D/Scene3D.h"
#include "../Scene3D/SceneGraph.h"
#include "../Scene3D/GeoMaterialOverride.h"

NATRON_NAMESPACE_ENTER

// Recursively collect a scene container's nodes. Scene3D / Group3D nest
// arbitrarily (e.g. Lights -> Group3D -> Scene3D -> render), so we must descend
// through EVERY container input — not just one level — or nodes inside a nested
// group never reach the SceneGraph and silently don't render. Non-container
// nodes (lights, geo) are added but not descended into. `seen` dedups nodes
// reachable via multiple paths and guards against cycles.
// (skipDots lives in ../DotUtils.h now — shared with the camera/settings/material
// input resolution across the 3D nodes.)
static void
collectSceneNodes(EffectInstance* eff, NodesList& out, std::set<EffectInstance*>& seen)
{
    if (!eff || seen.count(eff)) return;
    seen.insert(eff);

    // See through Dot routing nodes — they're transparent (input 0), so don't
    // add the Dot itself, just continue from what it points at.
    if (eff->getPluginID() == PLUGINID_NATRON_DOT) {
        EffectInstancePtr inp = eff->getInput(0);
        if (inp) collectSceneNodes(inp.get(), out, seen);
        return;
    }

    if (NodePtr n = eff->getNode()) out.push_back(n);

    // GeoMaterialOverride is a decorator in the geo chain: descend its geo input
    // (0) so the archive behind it is still collected. Its material assignment is
    // applied in SceneGraph::rebuild (the node itself was pushed just above so the
    // rebuild pre-scan sees it). Don't descend input 1 — that's the material.
    if (dynamic_cast<GeoMaterialOverride*>(eff)) {
        EffectInstancePtr inp = eff->getInput(0);
        if (inp) collectSceneNodes(inp.get(), out, seen);
        return;
    }

    Scene3D* s = dynamic_cast<Scene3D*>(eff);
    Group3D* g = dynamic_cast<Group3D*>(eff);
    const int maxIn = s ? SCENE3D_MAX_INPUTS : (g ? GROUP3D_MAX_INPUTS : 0);
    for (int i = 0; i < maxIn; ++i) {
        EffectInstancePtr inp = eff->getInput(i);
        if (inp) collectSceneNodes(inp.get(), out, seen);
    }
}

bool
prepareCyclesPasses(EffectInstance*           effect,
                     const CyclesPassRequest&  req,
                     CyclesPassPrepared&       out,
                     std::string&              errOut,
                     int                       sceneInputSlot)
{
    errOut.clear();
    out = CyclesPassPrepared(); // reset to defaults

    if (!effect) {
        errOut = "null effect";
        return false;
    }

    // --- Walk obj input (default slot 1; CyclesRenderPass self-preview uses slot 0)
    // + optional CyclesRenderPass wrapper, seeing through any Dot routing nodes at
    // each hop.
    EffectInstancePtr geoEffect = skipDots(effect->getInput(sceneInputSlot));
    if (!geoEffect) {
        errOut = "no obj/scene connected on the scene input slot";
        return false;
    }
    out.renderPass = dynamic_cast<CyclesRenderPass*>(geoEffect.get());
    if (out.renderPass) {
        geoEffect = skipDots(out.renderPass->getInput(0));
        if (!geoEffect) {
            errOut = "CyclesRenderPass has nothing on its input 0 (scene)";
            return false;
        }
    }

    // --- Collect all scene nodes, descending recursively through nested
    // Scene3D / Group3D containers (Lights -> Group3D -> Scene3D -> render).
    // A one-level walk missed nodes inside a nested group, so grouped lights /
    // geo never rendered.
    NodesList allNodes;
    std::set<EffectInstance*> seen;
    collectSceneNodes(geoEffect.get(), allNodes, seen);

    // --- Holdout input (CyclesRender slot 4): geo wired here is added to the
    // scene AND flagged as a Cycles holdout, so it punches a transparent matte
    // of its shape. Other render entry points (PassManager) have no slot 4 →
    // getInput(4) is null → no holdouts. Names are always flagged; nodes are
    // added to the scene via the shared `seen` set so a node present on both
    // the scene and holdout inputs isn't duplicated.
    if (EffectInstancePtr holdoutEff = effect->getInput(4)) {
        NodesList holdoutNodes;
        std::set<EffectInstance*> hseen;
        collectSceneNodes(holdoutEff.get(), holdoutNodes, hseen);
        for (NodesList::const_iterator it = holdoutNodes.begin(); it != holdoutNodes.end(); ++it) {
            if (!*it) continue;
            out.holdoutNames.insert((*it)->getScriptName());     // always flag
            EffectInstancePtr fx = (*it)->getEffectInstance();
            if (fx && seen.insert(fx.get()).second) {
                allNodes.push_back(*it);                         // add once
            }
        }
    }

    out.sceneGraph.rebuild(allNodes, req.time);
    if (out.sceneGraph.size() == 0) {
        errOut = "scene graph is empty after rebuild";
        return false;
    }

    // --- Bake Material3D input textures (Read → Grade → Material3D
    // input pipeline).
    {
        const std::vector<SceneNode>& sceneNodes = out.sceneGraph.nodes();
        for (size_t i = 0; i < sceneNodes.size(); ++i) {
            NodePtr srcNode = sceneNodes[i].sourceNode.lock();
            if (!srcNode) continue;
            MaterialProvider* matProv =
                dynamic_cast<MaterialProvider*>(srcNode->getEffectInstance().get());
            if (!matProv) continue;
            MaterialProvider* resolved = matProv;
            if (matProv->hasMaterialInput()) {
                MaterialProvider* connected = matProv->getConnectedMaterial();
                if (connected) resolved = connected;
            }
            Material3D* mat3d = dynamic_cast<Material3D*>(resolved);
            if (mat3d) {
                mat3d->bakeInputTextures(req.time);
            }
        }
    }

    // --- Resolve camera. Priority: explicit override on the request
    // (per-pass camera selection), then input slot 2, then the hard-coded
    // defaults already in out.cam*. The caller is responsible for
    // validating the override pointer before passing it.
    const CameraProvider* cam = req.cameraOverride;
    EffectInstancePtr camEffect;
    if (!cam) {
        camEffect = skipDots(effect->getInput(2));
        if (camEffect) cam = dynamic_cast<const CameraProvider*>(camEffect.get());
    }
    if (cam) {
        cam->getCameraPosition(req.time, out.camTX, out.camTY, out.camTZ,
                                          out.camRX, out.camRY, out.camRZ);
        out.camFL = cam->getCameraFocalLength(req.time);
        out.camHA = cam->getCameraHAperture(req.time);
        out.camVA = cam->getCameraVAperture(req.time);
        out.cameraResolved = true;
    }

    return true;
}

bool
executeCyclesPasses(CyclesRenderer&            renderer,
                     const CyclesPassPrepared&  prepared,
                     const CyclesPassRequest&   req,
                     std::map<std::string, std::vector<float>>& outBuffers,
                     std::string&               errOut)
{
    outBuffers.clear();
    errOut.clear();

    if (req.width <= 0 || req.height <= 0) {
        errOut = "non-positive width/height";
        return false;
    }
    if (req.requestedPasses.empty()) {
        errOut = "empty requestedPasses list";
        return false;
    }

    // Per-light ray-visibility overrides (e.g. keep a dome/area light out of
    // reflections). Null clears any previous override on the renderer.
    renderer.setLightRayVisibility(req.lightRayVis);

    // OpenImageDenoise toggle — applied to the integrator in syncSceneWithCamera.
    renderer.setDenoise(req.denoise);

    const bool ok = renderer.renderToBufferWithCameraMultiPass(
        prepared.sceneGraph,
        prepared.camTX, prepared.camTY, prepared.camTZ,
        prepared.camRX, prepared.camRY, prepared.camRZ,
        prepared.camFL, prepared.camHA, prepared.camVA,
        req.requestedPasses,
        outBuffers,
        req.width, req.height, req.samples,
        req.time,
        req.visMap,
        req.activeLights,
        req.dof,
        req.mb,
        req.integrator,
        req.materialOverride,
        prepared.holdoutNames.empty() ? nullptr : &prepared.holdoutNames);

    if (!ok || outBuffers.empty()) {
        errOut = "renderToBufferWithCameraMultiPass returned no buffers";
        outBuffers.clear();
        return false;
    }
    return true;
}

bool
renderCyclesPassesForEffect(EffectInstance*            effect,
                             const CyclesPassRequest&   req,
                             std::map<std::string, std::vector<float>>& outBuffers,
                             std::string&               errOut)
{
    CyclesPassPrepared prepared;
    if (!prepareCyclesPasses(effect, req, prepared, errOut)) {
        outBuffers.clear();
        return false;
    }
    CyclesRenderer renderer;
    return executeCyclesPasses(renderer, prepared, req, outBuffers, errOut);
}

void
enumerateSceneLights(EffectInstance*              effect,
                      double                       /*time*/,
                      std::vector<SceneLightInfo>& out,
                      int                          sceneInputSlot)
{
    out.clear();
    if (!effect) return;

    // Same traversal as the renderer: scene input slot → through CyclesRenderPass →
    // Scene3D / Group3D containers. We collect every node we visit and
    // then filter to Light3D via dynamic_cast.
    EffectInstancePtr geoEffect = skipDots(effect->getInput(sceneInputSlot));
    if (!geoEffect) return;
    CyclesRenderPass* renderPass = dynamic_cast<CyclesRenderPass*>(geoEffect.get());
    if (renderPass) {
        geoEffect = skipDots(renderPass->getInput(0));
        if (!geoEffect) return;
    }

    NodesList allNodes;
    {
        std::set<EffectInstance*> seen;
        collectSceneNodes(geoEffect.get(), allNodes, seen);
    }

    for (const NodePtr& n : allNodes) {
        if (!n) continue;
        EffectInstancePtr fx = n->getEffectInstance();
        if (!fx) continue;
        Light3D* light = dynamic_cast<Light3D*>(fx.get());
        if (!light) continue;
        SceneLightInfo info;
        info.scriptName = n->getScriptName_mt_safe();
        info.lightGroup = light->getLightGroup();
        out.push_back(std::move(info));
    }
}

void
enumerateSceneGeo(EffectInstance*            effect,
                   double                     /*time*/,
                   std::vector<SceneGeoInfo>& out,
                   int                        sceneInputSlot)
{
    out.clear();
    if (!effect) return;

    EffectInstancePtr geoEffect = skipDots(effect->getInput(sceneInputSlot));
    if (!geoEffect) return;
    CyclesRenderPass* renderPass = dynamic_cast<CyclesRenderPass*>(geoEffect.get());
    if (renderPass) {
        geoEffect = skipDots(renderPass->getInput(0));
        if (!geoEffect) return;
    }

    NodesList allNodes;
    {
        std::set<EffectInstance*> seen;
        collectSceneNodes(geoEffect.get(), allNodes, seen);
    }

    for (const NodePtr& n : allNodes) {
        if (!n) continue;
        EffectInstancePtr fx = n->getEffectInstance();
        if (!fx) continue;
        // Skip lights — they're collected by enumerateSceneLights.
        if (dynamic_cast<Light3D*>(fx.get())) continue;
        // Skip container / decorator nodes — they're not renderable geo (they
        // just group or annotate it). collectSceneNodes pushes them while
        // descending, so filter them out of the object list here.
        if (dynamic_cast<Scene3D*>(fx.get())) continue;
        if (dynamic_cast<Group3D*>(fx.get())) continue;
        if (dynamic_cast<GeoMaterialOverride*>(fx.get())) continue;
        SceneGeoInfo info;
        info.scriptName = n->getScriptName_mt_safe();
        out.push_back(std::move(info));
    }
}

NATRON_NAMESPACE_EXIT
