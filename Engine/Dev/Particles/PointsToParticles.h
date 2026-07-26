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

#ifndef NATRON_ENGINE_POINTSTOPARTICLES_H
#define NATRON_ENGINE_POINTSTOPARTICLES_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>
#include <mutex>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "ParticleProvider.h"
#include "ParticleData.h"

NATRON_NAMESPACE_ENTER

struct PointsToParticlesPrivate;

/**
 * @brief Adapter: PointCloudProvider -> ParticleProvider.
 *
 * Converts an upstream point cloud (DeepToPoints, Blast, CameraTracker,
 * PointCloudGenerator) into static particles, unlocking the particle
 * ecosystem for point clouds: ScanlineRender's particle modes
 * (point/disc/sphere/sprite, additive/over), CyclesRender's native point
 * rendering, and ParticleInstance (instance geometry onto point positions).
 *
 * Classic use: DeepRecolor -> DeepToPoints (world-space via camera input)
 * -> PointsToParticles -> Scene3D -> ScanlineRender = re-render a deep
 * image's samples from a different camera (2.5D re-projection).
 */
class PointsToParticles
    : public EffectInstance
    , public ParticleProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new PointsToParticles(n); }

    PointsToParticles(NodePtr node);
    virtual ~PointsToParticles();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_POINTSTOPARTICLES; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "PointsToParticles"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("Particles"); }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "points"; }

    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return false; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyFullySafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    /** ParticleProvider: the converted cloud as static particles. Published
     *  immutable snapshot under a mutex (P0 provider contract). */
    virtual ParticleDataPtr getParticleData(double time) OVERRIDE;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    std::unique_ptr<PointsToParticlesPrivate> _imp;

    // Published immutable snapshot + the source cloud it was built from
    // (pointer identity = cheap cache key; providers publish immutable
    // snapshots, so an unchanged pointer means unchanged data).
    mutable std::mutex _computeMutex;
    mutable ParticleDataPtr _cachedData;
    mutable const void* _cachedCloudKey = nullptr;
    mutable double _cachedSize = -1.0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_POINTSTOPARTICLES_H
