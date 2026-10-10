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

#ifndef NATRON_ENGINE_ZAP3D_H
#define NATRON_ENGINE_ZAP3D_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>
#include <mutex>
#include <vector>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "MaterialProvider.h"
#include "MeshProvider.h"

NATRON_NAMESPACE_ENTER

struct Zap3DPrivate;

/**
 * @brief Zap3D: a procedural lightning bolt as 3D geometry.
 *
 * The 3D sibling of the Nik Kit "Zap" OFX node. The same midpoint-displacement
 * bolt (branching, smooth Wiggle, Restrike, Progress, tip taper) is generated
 * between two points in the node's local space, or between the `from` / `to`
 * inputs (any node with translate knobs, or a Light3D), and turned into a thin
 * tube mesh with an emissive material. Because it is a MeshProvider +
 * MaterialProvider it renders everywhere ReadGeo does: the 3D viewport (grey
 * preview), ScanlineRender, and Cycles - where the emission makes it a real
 * light source that casts shadows and lights volumes. FastVolumeRender reads
 * getLightSamples() to place a chain of point lights along the bolt.
 */
class Zap3D
    : public EffectInstance
    , public MaterialProvider
    , public MeshProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static EffectInstance* BuildEffect(NodePtr n) { return new Zap3D(n); }

    Zap3D(NodePtr node);
    virtual ~Zap3D();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 3; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_ZAP3D; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Zap3D"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return true; }
    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;
    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyInstanceSafe; }
    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL;

    // MeshProvider: the bolt tubes in the node's local space (the Transform
    // page is applied by the consumers, like every other MeshProvider).
    virtual MeshDataPtr getMeshData(double time) const OVERRIDE;

    /** Point-light samples along the bolts, in the node's LOCAL space, for
     *  FastVolumeRender. Up to `maxN` samples spread along the channels; each
     *  carries a weight (segment length x brightness) summing to 1 so the
     *  caller can hand out a total intensity. Empty when "As Lights" is off. */
    struct LightSample {
        float pos[3];
        float weight;
    };
    void getLightSamples(double time, int maxN, std::vector<LightSample>& out) const;
    bool getAsLights() const;

    /** Start / End in world space at `time` (inputs win over the knobs). */
    void getEndpoints(double time, double* start, double* end) const { endpoints(time, start, end); }
    /** Which endpoint the Point X/Y/Z knobs and the viewport gizmo move: 0 = Start, 1 = End. */
    int selectedEndpoint() const;
    void setSelectedEndpoint(int index);
    double getLightIntensity(double time) const;
    double getLightRange(double time) const;

    // MaterialProvider interface (one emissive material for the whole bolt)
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const OVERRIDE;
    virtual double getMaterialRoughness(double time) const OVERRIDE;
    virtual double getMaterialMetallic(double time) const OVERRIDE;
    virtual double getMaterialSpecular(double time) const OVERRIDE;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const OVERRIDE;
    virtual double getMaterialTransmission(double time) const OVERRIDE;
    virtual double getMaterialIOR(double time) const OVERRIDE;
    virtual std::string getMaterialTextureFile() const OVERRIDE;
    virtual bool hasMaterialInput() const OVERRIDE;
    virtual MaterialProvider* getConnectedMaterial() const OVERRIDE;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    void syncEditor();                        // selected endpoint -> Point X/Y/Z (translate) knobs
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    struct Snapshot;
    typedef std::shared_ptr<const Snapshot> SnapshotPtr;
    SnapshotPtr snapshot(double time) const;   // builds (or returns the cached) bolt for `time`
    SnapshotPtr buildSnapshot(double time) const;
    unsigned long long paramHash(double time) const;
    void endpoints(double time, double* a, double* b) const;
    double flickerFactor(double time) const;

    std::unique_ptr<Zap3DPrivate> _imp;
    mutable std::mutex _snapMutex;
    mutable SnapshotPtr _snap;
    mutable unsigned long long _snapKey = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_ZAP3D_H
