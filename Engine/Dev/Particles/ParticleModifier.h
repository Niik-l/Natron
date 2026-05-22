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

#ifndef NATRON_ENGINE_PARTICLEMODIFIER_H
#define NATRON_ENGINE_PARTICLEMODIFIER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <vector>

#include "../../EffectInstance.h"
#include "ParticleData.h"
#include "ParticleProvider.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Abstract base class for particle force/modifier nodes.
 *
 * Force nodes are STATELESS — they modify velocities via applyForce()
 * but do not own particle positions or run integration.
 *
 * The simulation loop (spawn → forces → integrate → collide → expire)
 * is owned by the solver node at the end of the chain (ParticleSolver).
 *
 * When a viewer is connected directly to a force node, getParticleData()
 * returns upstream positions with this force's velocity modification applied.
 */
class ParticleModifier
    : public EffectInstance
    , public ParticleProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    ParticleModifier(NodePtr node);
    virtual ~ParticleModifier();

    virtual int getNInputs() const OVERRIDE WARN_UNUSED_RETURN { return 1; }
    virtual bool getCanTransform() const OVERRIDE WARN_UNUSED_RETURN { return false; }

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE
    { grouping->push_back("Particles"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE WARN_UNUSED_RETURN;

    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE WARN_UNUSED_RETURN
    { return false; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }

    virtual bool supportsTiles() const OVERRIDE WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief Stateless pass-through: gets upstream data, applies this force,
     * returns modified copy. No integration, no cache.
     * Used for preview when a viewer is connected to a force node directly.
     */
    virtual ParticleDataPtr getParticleData(double time) OVERRIDE;

    /**
     * @brief Pure virtual — subclasses implement their specific force logic.
     * Modify particle velocities in-place. Must be stateless.
     */
    virtual void applyForce(ParticleDataPtr data, double time) = 0;

    /**
     * @brief Walk upstream from startNode, collecting ParticleModifier force
     * nodes and finding the emitter. Returns forces in chain order
     * (closest to emitter first).
     */
    static void collectUpstreamForces(EffectInstance* startNode,
                                      std::vector<ParticleModifier*>& outForces,
                                      ParticleProvider*& outEmitter);

private:

    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PARTICLEMODIFIER_H
