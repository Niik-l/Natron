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

#ifndef NATRON_ENGINE_WRITEALEMBICPARTICLES_H
#define NATRON_ENGINE_WRITEALEMBICPARTICLES_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "ParticleModifier.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct WriteAlembicParticlesPrivate;

/**
 * @brief Bake a particle stream to an Alembic .abc point-cloud file.
 *
 * Passthrough particle node: input 0 (particle stream) flows downstream
 * unmodified. Clicking the "Bake" knob iterates the project's frame
 * range, pulls particle data for each frame from input 0, and writes an
 * OPoints sample per frame. The resulting .abc carries positions, IDs,
 * velocities, RGBA color, and per-particle size.
 *
 * Intended placement: after a ParticleSolver so the baked sim is
 * deterministic. Can also sit mid-chain — applyForce is a no-op so it
 * does not interfere with the upstream-force walk.
 */
class WriteAlembicParticles
    : public ParticleModifier
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new WriteAlembicParticles(n); }

    WriteAlembicParticles(NodePtr node);
    virtual ~WriteAlembicParticles();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_WRITEALEMBICPARTICLES; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "WriteAlembicParticles"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // No-op: this node does not modify particle velocities. Required by
    // the ParticleModifier interface so the node can sit in a particle
    // chain (passthrough). The actual disk write is triggered by the
    // Bake knob, not by getParticleData.
    virtual void applyForce(ParticleDataPtr /*data*/, double /*time*/) OVERRIDE FINAL {}

    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view,
                             double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;

    // Iterate the project's frame range, pull particles from input 0,
    // write an OPoints sample per frame. Synchronous (blocks the UI
    // thread until done).
    void bake();

    std::unique_ptr<WriteAlembicParticlesPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_WRITEALEMBICPARTICLES_H
