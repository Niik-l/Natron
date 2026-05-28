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

#ifndef NATRON_ENGINE_PARTICLESOLVER_H
#define NATRON_ENGINE_PARTICLESOLVER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "ParticleModifier.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ParticleSolverPrivate;

/**
 * @brief Particle solver + collision node.
 *
 * THE SOLVER — owns the single authoritative particle position/velocity state.
 * Walks upstream to collect force nodes, then runs the simulation loop each frame:
 * spawn → forces → integrate → collide → expire.
 *
 * Input 0: Particle source (ParticleEmitter or force chain)
 * Input 1: Geometry for collision (optional — Cube3D, Sphere3D)
 *
 * Without geo connected, acts as a pure solver (forces + integration, no collision).
 * With geo connected, also applies ray-based collision with elasticity and friction.
 */
class ParticleSolver
    : public ParticleModifier
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ParticleSolver(n); }

    ParticleSolver(NodePtr node);
    virtual ~ParticleSolver();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_PARTICLESOLVER; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ParticleSolver"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (inputNb == 0) return "particles";
        if (inputNb == 1) return "collision geo";
        return std::string();
    }

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return (inputNb == 1);
    }

    virtual void applyForce(ParticleDataPtr data, double time) OVERRIDE FINAL;

    // THE SOLVER — walks upstream to collect forces and the emitter,
    // then runs the single simulation loop: spawn → forces → integrate → collide → expire.
    // This is the only node that owns particle position/velocity state.
    virtual ParticleDataPtr getParticleData(double time) OVERRIDE;

    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view,
                             double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;

    // Frame cache (multi-frame, RAM-resident).
    //   Phase A — skeleton (knobs + data in _imp).
    //   Phase B — wired into getParticleData: hit/miss/resume-from-nearest +
    //             hash-based invalidation. No eviction yet (unbounded).
    //   Phase C — adds LRU eviction.
    void clearFrameCache();
    void refreshCacheStatusLabels();
    U64  computeUpstreamHash() const;

    // Per-particle collision dispatch (called after integration). `dt` is
    // the substep size (1.0/numSubsteps); threaded through to bounceParticle
    // so the post-bounce displacement is scaled to one substep, not one full
    // frame.
    void applyCollision(Particle& p, double time, float dt);

    std::unique_ptr<ParticleSolverPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PARTICLESOLVER_H
