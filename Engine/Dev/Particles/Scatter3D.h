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

#ifndef NATRON_ENGINE_SCATTER3D_H
#define NATRON_ENGINE_SCATTER3D_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <mutex>

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "ParticleData.h"
#include "ParticleProvider.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct Scatter3DPrivate;

/**
 * @brief Scatter points over the surface of a geo node.
 *
 * Input 0: the surface (any geo the 3D system renders: ReadGeo, GeoBuilder,
 * ReadAlembicArchive, Card3D, Sphere3D...). Input 1: optional Density mask,
 * looked up through the surface UVs (white = full density).
 *
 * Output: static particles (a ParticleProvider) that carry the surface normal,
 * so ParticleInstance can stand debris and foliage up along the ground.
 * Area-weighted, so a big triangle gets as many points per square metre as a
 * small one; Slope Limit and Min Distance thin the result.
 */
class Scatter3D
    : public EffectInstance
    , public ParticleProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Scatter3D(n); }

    Scatter3D(NodePtr node);
    virtual ~Scatter3D();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_SCATTER3D; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Scatter3D"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("Particles"); }

    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 0 ? "surface" : "density"; }

    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb != 0; }

    virtual bool isInputMask(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 1; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyFullySafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    /** ParticleProvider: the scattered points as static particles (immutable snapshot, cached). */
    virtual ParticleDataPtr getParticleData(double time) OVERRIDE;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    ParticleDataPtr compute(double time);

    std::unique_ptr<Scatter3DPrivate> _imp;

    mutable std::mutex _computeMutex;
    ParticleDataPtr _cachedData;
    U64 _cachedHash = 0;
    double _cachedTime = -1e300;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCATTER3D_H
