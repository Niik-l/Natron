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

#ifndef NATRON_ENGINE_READGEO_H
#define NATRON_ENGINE_READGEO_H

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
#include "SubdivisionProvider.h"
#include "MeshData.h"
#include "ReadGeoImport.h"

NATRON_NAMESPACE_ENTER

struct ReadGeoPrivate;

/**
 * @brief Import geometry from .glb/.gltf (glTF), .fbx, .abc (Alembic) or
 * .obj (Wavefront) files.
 * Parser dispatches on file extension. Displays meshes in the 3D viewport.
 */
class ReadGeo
    : public EffectInstance
    , public MaterialProvider
    , public SubdivisionProvider
    , public MeshProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ReadGeo(n); }

    ReadGeo(NodePtr node);
    virtual ~ReadGeo();

    // SubdivisionProvider: Subdivision (Cycles) group, render-time dicing
    virtual bool getSubdivisionParams(double time, SubdivisionParams* out) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_READGEO; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ReadGeo"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("3D"); }

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

    /**
     * @brief Get the mesh data for 3D viewport rendering.
     * @param time Current time for animated transforms. Pass -1 for frame 0.
     */
    virtual MeshDataPtr getMeshData(double time = -1) const OVERRIDE;

    /**
     * @brief Update the transform matrix for the given time.
     */
    void updateTransformAtTime(MeshData* mesh, double time) const;

    /**
     * @brief Update the vertex positions for the given time. Used by animated
     * (deforming) meshes — for static meshes this is a no-op. Topology stays
     * constant across samples; only positions change.
     */
    void updateVerticesAtTime(MeshData* mesh, double time) const;

    // MaterialProvider interface
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const OVERRIDE;
    virtual double getMaterialRoughness(double time) const OVERRIDE;
    virtual double getMaterialMetallic(double time) const OVERRIDE;
    virtual double getMaterialSpecular(double time) const OVERRIDE;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const OVERRIDE;
    virtual double getMaterialTransmission(double time) const OVERRIDE;
    virtual double getMaterialIOR(double time) const OVERRIDE;
    virtual std::string getMaterialTextureFile() const OVERRIDE;
    virtual std::string getMaterialDiffuseColorspace() const OVERRIDE;
    virtual bool hasMaterialInput() const OVERRIDE;
    virtual MaterialProvider* getConnectedMaterial() const OVERRIDE;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    // Project load: recreate the morph-weight knobs from the saved target
    // list BEFORE loadKnobs runs, so the saved values + keyframes restore by
    // knob name (dynamic knobs are absent from the holder during loadKnobs
    // otherwise).
    virtual void onKnobsAboutToBeLoaded(const NodeSerializationPtr& serialization) OVERRIDE FINAL;
    // Called once after all knobs are restored from a saved project. Loads the
    // geometry from the restored file path so the scene shows up without the
    // user having to hit "Reload" manually.
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    // --- Morph Weights (glTF shape keys / FBX blend shapes) ---
    // Enumerate the file's distinct morph sets into an ordered target list
    // (caller holds _meshMutex; reads rt.data).
    void collectMorphTargets(std::vector<GeoMorphTargetRef>* out) const;
    // Sync the per-target weight knobs to `targets`: re-label existing
    // morphW<i> knobs, create missing ones, delete extras. When the list
    // changed since the last sync, knob values reset to the new rest weights.
    // Returns true when the knob structure changed (the caller then calls
    // recreateKnobs(true) — ReadGeoArchive must do that outside its mutex).
    // Caller holds _meshMutex; GUI-thread only.
    bool syncMorphWeightKnobs(const std::vector<GeoMorphTargetRef>& targets);
    // Fill the runtime's override store from the weight knobs at `time`
    // (caller holds _meshMutex). No-op when Use File Morph Weights is on.
    void applyMorphOverrideAt(double time) const;

    void loadAlembicGeo(const std::string& path);
    void loadObjGeo(const std::string& path);
    // glTF 2.0 (.gltf/.glb) via cgltf and FBX (.fbx) via ufbx, read through
    // the shared GeoArchiveRuntime (ReadGeoImport.h) — the same parse +
    // per-frame evaluation machinery ReadGeoArchive uses. Loads the whole
    // scene once, fills the Object dropdown ("All parts (merged)" + one entry
    // per mesh part) and publishes the initial mesh; per-frame fetches go
    // through buildSceneMesh().
    void loadSceneGeo(const std::string& path);
    // Build the MeshData for the current selection at `time` (caller holds
    // _meshMutex): merged = every mesh entry's evaluated vertices baked
    // through its world matrix, concatenated; single part = that part's
    // vertices with its world matrix either baked (Bake Transforms on) or
    // carried in MeshData::transform (off). Cached per (frame, selection,
    // bake) — a merged animated asset costs one bake per visited frame.
    MeshDataPtr buildSceneMesh(double time) const;
    // Write file-authored PBR values into the material knobs (Material page +
    // Texture Maps page). Only called when the file actually had a material.
    void applyImportedMaterialKnobs(const GeoImportMaterial& material);
    // Dispatch on file extension (.obj/.abc), guarded by _imp->isLoading, then
    // refresh metadata. Shared by knobChanged (file/reload/object change) and
    // onKnobsLoaded (project load).
    void loadGeoFromFile(const std::string& path);

    std::unique_ptr<ReadGeoPrivate> _imp;
    // Guards _lastMeshData and the loaded animation-sample state: file (re)load
    // runs on the GUI thread (knobChanged/onKnobsLoaded) while getMeshData is
    // called from render workers and the 3D-viewport paint.
    mutable std::mutex _meshMutex;
    mutable MeshDataPtr _lastMeshData;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READGEO_H
