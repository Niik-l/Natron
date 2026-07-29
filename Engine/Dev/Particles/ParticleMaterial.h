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

#ifndef NATRON_ENGINE_PARTICLEMATERIAL_H
#define NATRON_ENGINE_PARTICLEMATERIAL_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "ParticleProvider.h"
#include "ParticleData.h"
#include "../Scene3D/MaterialProvider.h" // global-namespace interface — do NOT forward-declare inside NATRON_NAMESPACE

NATRON_NAMESPACE_ENTER

struct ParticleMaterialPrivate;

/**
 * @brief Particle shading override: a pass-through in the particle chain
 * that carries a material (Material3D etc. on the "mat" input) and shading
 * knobs down to the renderer.
 *
 * CyclesRender's particle branch walks the particle chain for this node:
 * with a material connected, particles render with the full PBR material
 * (optionally tinted per-particle by the vertex color); without one, the
 * default per-particle color shader is used with this node's Emission
 * Strength / Roughness / Metallic knobs instead of the hardcoded values.
 */
class ParticleMaterial
    : public EffectInstance
    , public ParticleProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ParticleMaterial(n); }

    ParticleMaterial(NodePtr node);
    virtual ~ParticleMaterial();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_PARTICLEMATERIAL; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ParticleMaterial"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("Particles"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return (inputNb == 0) ? "particles" : "mat"; }

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 1; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyFullySafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    /** ParticleProvider: pass-through from the "particles" input. */
    virtual ParticleDataPtr getParticleData(double time) OVERRIDE;

    /** The material connected on the "mat" input (through Dots), or null. */
    MaterialProvider* getParticleMaterialProvider() const;

    // Fallback-shader overrides (used when no material is connected)
    double getEmissionStrength(double time) const;
    double getRoughness(double time) const;
    double getMetallic(double time) const;
    /** With a material connected: multiply the per-particle color into the
     *  material's base color (and drive alpha by particle age fade). */
    bool getTintWithParticleColor() const;

    // Trails (Cycles): render particles as curve ribbons through their past
    // positions instead of points. Read by CyclesRenderer's particle branch.
    bool getTrailsEnabled() const;
    int getTrailLength(double time) const;
    double getTrailHeadRadius(double time) const;
    double getTrailTailRadius(double time) const;
    double getTrailTailFade(double time) const;
    void getTrailTailTint(double time, double& r, double& g, double& b) const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<ParticleMaterialPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PARTICLEMATERIAL_H
