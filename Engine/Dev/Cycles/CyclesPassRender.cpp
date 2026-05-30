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

#include "../../Node.h"

#include "../Scene3D/CameraProvider.h"
#include "../Scene3D/Group3D.h"
#include "../Scene3D/Light3D.h"
#include "../Scene3D/Material3D.h"
#include "../Scene3D/MaterialProvider.h"
#include "../Scene3D/RenderPass.h"
#include "../Scene3D/Scene3D.h"
#include "../Scene3D/SceneGraph.h"

NATRON_NAMESPACE_ENTER

bool
renderCyclesPassesForEffect(EffectInstance*            effect,
                             const CyclesPassRequest&   req,
                             std::map<std::string, std::vector<float>>& outBuffers,
                             std::string&               errOut)
{
    outBuffers.clear();
    errOut.clear();

    if (!effect) {
        errOut = "null effect";
        return false;
    }
    if (req.width <= 0 || req.height <= 0) {
        errOut = "non-positive width/height";
        return false;
    }
    if (req.requestedPasses.empty()) {
        errOut = "empty requestedPasses list";
        return false;
    }

    // --- Resolve obj input + walk through optional RenderPass wrapper.
    // Mirrors CyclesRender::render() at the corresponding block — see
    // 5A.2 task to dedupe.
    EffectInstancePtr geoEffect = effect->getInput(1);
    if (!geoEffect) {
        errOut = "no obj/scene connected on slot 1";
        return false;
    }
    RenderPass* renderPass = dynamic_cast<RenderPass*>(geoEffect.get());
    if (renderPass) {
        geoEffect = renderPass->getInput(0);
        if (!geoEffect) {
            errOut = "RenderPass has nothing on its input 0 (scene)";
            return false;
        }
    }

    // --- Collect all top-level scene nodes, accounting for Scene3D /
    // Group3D containers that have many sub-inputs each.
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
    sceneGraph.rebuild(allNodes, req.time);
    if (sceneGraph.size() == 0) {
        errOut = "scene graph is empty after rebuild";
        return false;
    }

    // --- Bake Material3D input textures (Read → Grade → Material3D
    // input pipeline). Mirrors CyclesRender::render().
    {
        const std::vector<SceneNode>& sceneNodes = sceneGraph.nodes();
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
    // (per-pass camera selection), then input slot 2, then the renderer's
    // hard-coded defaults below. The caller is responsible for validating
    // the override pointer before passing it; we don't second-guess here.
    // Defaults match the CyclesRender path: t=(0,2,-8), 50mm/24×18 sensor.
    const CameraProvider* cam = req.cameraOverride;
    EffectInstancePtr camEffect;
    if (!cam) {
        camEffect = effect->getInput(2);
        if (camEffect) cam = dynamic_cast<const CameraProvider*>(camEffect.get());
    }
    double camTX = 0,  camTY = 2,  camTZ = -8;
    double camRX = 0,  camRY = 0,  camRZ = 0;
    double camFL = 50.0, camHA = 24.576, camVA = 18.672;
    if (cam) {
        cam->getCameraPosition(req.time, camTX, camTY, camTZ, camRX, camRY, camRZ);
        camFL = cam->getCameraFocalLength(req.time);
        camHA = cam->getCameraHAperture(req.time);
        camVA = cam->getCameraVAperture(req.time);
    }

    // --- Invoke Cycles. This is the same call CyclesRender::render()
    // makes at its own site.
    auto renderer = std::make_unique<CyclesRenderer>();
    const bool ok = renderer->renderToBufferWithCameraMultiPass(
        sceneGraph,
        camTX, camTY, camTZ,
        camRX, camRY, camRZ,
        camFL, camHA, camVA,
        req.requestedPasses,
        outBuffers,
        req.width, req.height, req.samples,
        req.time,
        req.visMap,
        req.activeLights,
        req.dof,
        req.mb,
        req.integrator);

    if (!ok || outBuffers.empty()) {
        errOut = "renderToBufferWithCameraMultiPass returned no buffers";
        outBuffers.clear();
        return false;
    }
    return true;
}

void
enumerateSceneLights(EffectInstance*              effect,
                      double                       /*time*/,
                      std::vector<SceneLightInfo>& out)
{
    out.clear();
    if (!effect) return;

    // Same traversal as the renderer: input 1 → through RenderPass →
    // Scene3D / Group3D containers. We collect every node we visit and
    // then filter to Light3D via dynamic_cast.
    EffectInstancePtr geoEffect = effect->getInput(1);
    if (!geoEffect) return;
    RenderPass* renderPass = dynamic_cast<RenderPass*>(geoEffect.get());
    if (renderPass) {
        geoEffect = renderPass->getInput(0);
        if (!geoEffect) return;
    }

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
                   std::vector<SceneGeoInfo>& out)
{
    out.clear();
    if (!effect) return;

    EffectInstancePtr geoEffect = effect->getInput(1);
    if (!geoEffect) return;
    RenderPass* renderPass = dynamic_cast<RenderPass*>(geoEffect.get());
    if (renderPass) {
        geoEffect = renderPass->getInput(0);
        if (!geoEffect) return;
    }

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

    for (const NodePtr& n : allNodes) {
        if (!n) continue;
        EffectInstancePtr fx = n->getEffectInstance();
        if (!fx) continue;
        // Skip lights — they're collected by enumerateSceneLights.
        if (dynamic_cast<Light3D*>(fx.get())) continue;
        SceneGeoInfo info;
        info.scriptName = n->getScriptName_mt_safe();
        out.push_back(std::move(info));
    }
}

NATRON_NAMESPACE_EXIT
