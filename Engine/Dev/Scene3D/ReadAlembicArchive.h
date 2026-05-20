/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_READALEMBICARCHIVE_H
#define NATRON_ENGINE_READALEMBICARCHIVE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <string>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "MaterialProvider.h"
#include "MeshData.h"

#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

struct ReadAlembicArchivePrivate;

/**
 * @brief One entry in the loaded Alembic archive's hierarchy, returned by
 * ReadAlembicArchive::getEntryTree() for UI consumers (AlembicTreeWidget).
 */
struct ArchiveTreeEntry
{
    std::string fullPath;        // "/Camera01Trackers/Tracker1"
    std::string name;            // leaf name, "Tracker1"
    int parentLocalIdx;          // -1 = archive-root; otherwise index into the same vector
    bool isMesh;                 // true => IPolyMesh, false => IXform locator
    int sampleCount;             // xform sample count (0 for meshes)
    int numVertices;             // mesh vertex count (0 for xforms)
};

/**
 * @brief One-node import of an entire Alembic archive's transform / mesh hierarchy.
 *
 * Walks the .abc file once on load, caches every IXform and IPolyMesh entry
 * with its parent index, and exposes the resulting subtree to the SceneGraph
 * builder via getSceneNodeCount / getSceneNodeAtTime.
 *
 * Phase A (current): locator-only — xforms emit SceneNodes of type
 * eSceneNodeTransform; meshes are visible in the entry tree but render
 * as locators too (no geometry yet). Sufficient for SynthEyes tracker
 * imports and pipeline-level testing of the multi-emit plumbing.
 *
 * Phase B (next): wire mesh data through SceneNode::meshData so cars,
 * character rigs, set props all import as one node with their geometry.
 */
class ReadAlembicArchive
    : public EffectInstance
    , public MaterialProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

Q_SIGNALS:

    /** @brief Emitted at the end of loadAlembicFile() — successful or not.
     *  The AlembicTreeWidget connects to this to refresh its display when the
     *  archive is reloaded (filename change, Reload button, filter change).
     */
    void archiveReloaded();

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ReadAlembicArchive(n); }

    ReadAlembicArchive(NodePtr node);
    virtual ~ReadAlembicArchive();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_READALEMBICARCHIVE; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ReadAlembicArchive"; }

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
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    // --- Scene-graph multi-emit API ---
    // SceneGraph::rebuild() calls these to emit one SceneNode per archive entry.

    /** @brief How many entries this archive currently exposes (after filtering). */
    int getSceneNodeCount() const;

    /** @brief Fill out the per-entry scene info for emission.
     *  @param idx Entry index in [0, getSceneNodeCount()).
     *  @param time Natron time at which to sample the entry's matrix.
     *  @param outName Full archive path of this entry (e.g. "/Camera01Trackers/Tracker1").
     *  @param outParentLocalIndex Parent index within this archive's entries (-1 if archive-root).
     *  @param outIsMesh true if the entry is a polymesh (Phase B will populate mesh data).
     *  @param outLocalMatrix Column-major 4x4 (OpenGL-style), the entry's local transform at `time`.
     *  @return true on success, false if idx is out of range or the file failed to load.
     */
    bool getSceneNodeAt(int idx, double time,
                        std::string& outName,
                        int& outParentLocalIndex,
                        bool& outIsMesh,
                        float outLocalMatrix[16]) const;

    /** @brief Fetch the rest-pose mesh data for a mesh entry.
     *  Returns nullptr if the entry isn't a mesh, or no mesh data was read.
     *  Mesh vertices are in the parent xform's local frame — SceneGraph composes
     *  the parent chain when computing the world transform.
     */
    MeshDataPtr getMeshDataAt(int idx) const;

    // MaterialProvider interface — every archive mesh entry shares a single
    // material taken from the optional Material3D input on input 0. When no
    // material is connected, returns sensible Principled-BSDF defaults.
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

    /** @brief Return the full archive hierarchy (BEFORE filtering).
     *  Used by the tree-view widget to display the archive's structure with
     *  checkboxes for selective import. The returned vector is in walk order:
     *  parents always come before their children. parentLocalIdx is the index
     *  within the returned vector.
     */
    std::vector<ArchiveTreeEntry> getEntryTree() const;

    /** @brief Compute the world-space transform for a visible entry at the given time.
     *
     *  Walks the parent chain inside this archive, multiplying local matrices.
     *  Output is column-major float[16] (OpenGL layout). Required by callers
     *  that don't go through SceneGraph (e.g. ScanlineRender), which would
     *  otherwise only see each entry's local transform.
     *
     *  @return true on success; false if idx is out of range.
     */
    bool getEntryWorldMatrix(int idx, double time, float outWorld[16]) const;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    void loadAlembicFile(const std::string& path);

    std::unique_ptr<ReadAlembicArchivePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READALEMBICARCHIVE_H
