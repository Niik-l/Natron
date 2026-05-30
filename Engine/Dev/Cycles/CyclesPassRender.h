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
#include "../Scene3D/RenderPass.h"  // ObjectVisibility

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

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CYCLESPASSRENDER_H
