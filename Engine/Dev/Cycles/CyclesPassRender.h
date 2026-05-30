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
#include "../Scene3D/RenderPass.h"     // ObjectVisibility
#include "../Scene3D/CameraProvider.h" // CameraProvider (global scope)

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

// Render one Cycles submission for the given effect. The effect provides
// the inputs (slot 1 = obj/scene, slot 2 = cam) via its EffectInstance
// interface, and the request describes what to render.
//
// On success returns true and fills `outBuffers` (one entry per pass name
// in `req.requestedPasses`, each 4-channel RGBA float matching
// width × height × 4). On failure returns false with a short reason in
// `errOut`.
//
// The function is stateless — no caching, no shared scene reuse. Callers
// that need caching (CyclesRender's hash-based cache) layer it on top.
//
// 5A.1: helper file lives alongside CyclesRender.cpp's existing render()
// which still duplicates this logic. 5A.2 will collapse the duplication
// by rewiring CyclesRender::render() to call this function too.
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
