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

#ifndef NATRON_ENGINE_CYCLESPASSRENDER_H
#define NATRON_ENGINE_CYCLESPASSRENDER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

#include "CyclesRenderer.h"
#include "../Scene3D/RenderPass.h"     // ObjectVisibility, RenderPass*
#include "../Scene3D/CameraProvider.h" // CameraProvider (global scope)
#include "../Scene3D/SceneGraph.h"     // SceneGraph (held by CyclesPassPrepared)

NATRON_NAMESPACE_ENTER

// Stateless request describing one Cycles render submission.
//
// All per-pass tuning lives in this struct so that callers (CyclesRender
// for the live preview path, CyclesRenderPassManager for batched disk
// output) can build it up from whatever source they have — knobs, JSON,
// etc. — without sharing knob-reading code.
//
// Required: time, width, height, samples, requestedPasses. The optional
// pointer fields default to nullptr; the renderer falls back to
// reasonable defaults (no DOF, no motion blur, default integrator, no
// visibility filtering).
struct CyclesPassRequest
{
    // Time / view from the caller's render context.
    double  time = 0.0;
    ViewIdx view{0};

    // Output dimensions in pixels.
    int width  = 0;
    int height = 0;

    // Per-pass overrides — caller fills these.
    int samples = 64;
    std::vector<std::string> requestedPasses;  // pass-name list (e.g. "Combined", "Depth", "Mist")

    // Common render settings.
    bool transparentBg = false;

    // Optional features. Pass nullptr to leave each disabled / default.
    const CyclesRenderer::DOFParams*        dof        = nullptr;
    const CyclesRenderer::MotionBlurParams* mb         = nullptr;
    const CyclesRenderer::IntegratorParams* integrator = nullptr;

    // Optional scoping (sourced upstream from a RenderPass node when present).
    const std::map<std::string, ObjectVisibility>* visMap       = nullptr;
    const std::set<std::string>*                   activeLights = nullptr;

    // Per-pass camera override. When non-null, the helper reads camera
    // params from this CameraProvider INSTEAD of the effect's input slot 2.
    // The caller is responsible for validating the pointer (and producing a
    // clear error if the override was requested but no such node exists).
    const CameraProvider*                          cameraOverride = nullptr;
};

// Resolved inputs ready for Cycles execution. Filled by
// prepareCyclesPasses() so callers that need to inspect the resolved
// values (CyclesRender's hash-based cache) can do so before deciding
// whether to actually invoke Cycles.
//
// All fields except `sceneGraph` start at the renderer's hard-coded
// fallback values so a call site that never receives a real camera /
// scene still ends up with a defined struct.
struct CyclesPassPrepared
{
    SceneGraph sceneGraph;

    // Camera, resolved with priority: req.cameraOverride > effect input 2 >
    // these fallback defaults. `cameraResolved` is true iff a CameraProvider
    // was actually consulted (false → defaults were used).
    double camTX = 0.0,  camTY = 2.0,  camTZ = -8.0;
    double camRX = 0.0,  camRY = 0.0,  camRZ = 0.0;
    double camFL = 50.0, camHA = 24.576, camVA = 18.672;
    bool   cameraResolved = false;

    // RenderPass node found while walking input 1 (if any). Callers that
    // pull visibility / lights from the RenderPass node can read it from
    // here rather than re-walking input 1. Raw pointer; valid for the
    // lifetime of the EffectInstance graph the prepared struct was built
    // against.
    RenderPass* renderPass = nullptr;
};

// Step 1 of the two-step render. Walks the effect's obj input
// (slot 1) through any optional RenderPass node into Scene3D / Group3D
// containers, builds the scene graph, bakes Material3D input textures,
// then resolves the camera (override > input 2 > defaults). Returns
// false with a reason in `errOut` if the obj input is missing or the
// scene graph is empty after rebuild.
//
// PassManager calls this via `renderCyclesPassesForEffect()`. CyclesRender
// calls it directly so it can hash the resolved values + cache check
// before deciding whether to run step 2.
bool prepareCyclesPasses(EffectInstance*           effect,
                          const CyclesPassRequest&  req,
                          CyclesPassPrepared&       out,
                          std::string&              errOut);

// Step 2 of the two-step render. Invokes Cycles using a prepared scene
// and the request's per-pass parameters. The renderer is supplied by the
// caller so CyclesRender can keep a persistent renderer across frames
// for `cancelRender()` support; PassManager hands in a fresh local
// instance per batch via the convenience wrapper below.
bool executeCyclesPasses(CyclesRenderer&            renderer,
                          const CyclesPassPrepared&  prepared,
                          const CyclesPassRequest&   req,
                          std::map<std::string, std::vector<float>>& outBuffers,
                          std::string&               errOut);

// Convenience: prepare + execute in one call with a local renderer.
// Used by CyclesRenderPassManager — it doesn't need to peek between
// the two steps.
bool renderCyclesPassesForEffect(EffectInstance*            effect,
                                  const CyclesPassRequest&   req,
                                  std::map<std::string, std::vector<float>>& outBuffers,
                                  std::string&               errOut);

// One scene-graph Light3D entry — used by the CyclesRenderPassManager
// diagnostic dump so the user can see exactly what names to put in the
// light-scoping JSON fields (script name) and what light-group AOVs
// will be produced (Light Group knob value).
struct SceneLightInfo {
    std::string scriptName;   // Natron node script name — matches activeLights
    std::string lightGroup;   // Light3D's Light Group knob value — produces Combined_<group> AOV
};

// Walk the effect's obj input (input slot 1, traversing through optional
// RenderPass / Scene3D / Group3D containers) and collect every Light3D
// node found. Cheap — no SceneGraph rebuild, just dynamic_cast walks.
void enumerateSceneLights(EffectInstance*              effect,
                           double                       time,
                           std::vector<SceneLightInfo>& out);

// One scene-graph non-light entry (geo / particles / volumes / cameras).
// Used by per-pass object scoping in CyclesRenderPassManager — the
// renderer's visibilityMap is keyed by node script name, so callers need
// the same names that ccl::Object lookups will use.
struct SceneGeoInfo {
    std::string scriptName;
};

// Same traversal as enumerateSceneLights but collects every node that
// isn't a Light3D. Used to build the per-pass ObjectVisibility map.
void enumerateSceneGeo(EffectInstance*            effect,
                        double                     time,
                        std::vector<SceneGeoInfo>& out);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CYCLESPASSRENDER_H
