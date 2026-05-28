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

#ifndef NATRON_ENGINE_READALEMBICPARTICLES_H
#define NATRON_ENGINE_READALEMBICPARTICLES_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "ParticleData.h"
#include "ParticleProvider.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ReadAlembicParticlesPrivate;

/**
 * @brief Source node — read a baked Alembic OPoints stream back into Natron.
 *
 * Counterpart to WriteAlembicParticles. Opens an .abc archive, finds the
 * first IPoints object, and exposes its per-sample data as a
 * ParticleProvider stream. Per-frame samples (positions, IDs, velocities,
 * Cd color, size) are pre-loaded on file open so getParticleData() at
 * runtime is a cheap copy.
 *
 * Multi-points archives: V1 picks the first IPoints found. A "Points
 * Object" dropdown is future work.
 *
 * Designed for the bake-once round-trip: ParticleSolver →
 * WriteAlembicParticles → (edit in Houdini/Blender) → ReadAlembicParticles
 * → downstream (render, instance, etc).
 */
class ReadAlembicParticles
    : public EffectInstance
    , public ParticleProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ReadAlembicParticles(n); }

    ReadAlembicParticles(NodePtr node);
    virtual ~ReadAlembicParticles();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs()      const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_READALEMBICPARTICLES; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ReadAlembicParticles"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("Particles"); }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }
    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // ParticleProvider — sample lookup from the pre-loaded per-frame arrays.
    virtual ParticleDataPtr getParticleData(double time) OVERRIDE;

    // Mark this node frame-varying when the loaded archive carries multiple
    // samples — without this Scene3D/ScanlineRender freezes on a single frame.
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view,
                             double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale,
                                             ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    // Open the archive, find the first IPoints, pre-load all samples.
    // Returns true on success; on failure clears any partial state.
    bool loadAlembicFile(const std::string& path);

    std::unique_ptr<ReadAlembicParticlesPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READALEMBICPARTICLES_H
