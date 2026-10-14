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

#ifndef NATRON_ENGINE_READGEOARCHIVE_H
#define NATRON_ENGINE_READGEOARCHIVE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>
#include <string>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"
#include "ArchiveProvider.h"
#include "MaterialProvider.h"
#include "MeshData.h"
#include "ReadGeoImport.h"

NATRON_NAMESPACE_ENTER

struct ReadGeoArchivePrivate;

/**
 * @brief One-node import of a whole scene-format file's hierarchy and
 * animation — the scene-graph counterpart of ReadGeo.
 *
 * Formats: .glb / .gltf (glTF 2.0, cgltf) and .fbx (ufbx) — the name is
 * deliberately format-neutral so future formats (USD, ...) plug into the
 * same node via the ReadGeoImport archive contract (loadXxxArchiveImport +
 * evaluateXxxArchiveFrame).
 *
 * Walks the file once on load into node entries + per-material mesh entries
 * (rest pose, node-local frame), then exposes the subtree to the SceneGraph
 * builder through the ArchiveProvider multi-emit API — one SceneNode per
 * entry with the parent chain preserved.
 *
 * Animation (the ReadGeo/Alembic sample machinery, applied to scene formats):
 * per-node TRS channels and CPU-skinned vertices are evaluated per frame into
 * a frame cache (keyed by integer Natron frame, filled lazily on first
 * request, guarded by a mutex). Rigid node transforms interpolate linearly
 * between cached frames so sub-frame motion-blur sampling works; skinned
 * vertices currently snap per frame (the Alembic machinery's in-place
 * pattern — the G1 copy-on-read caveat applies, tracked separately).
 * Declaring the node frame-varying (getPreferredMetadata) keeps the
 * ScanlineRender cache from freezing animated output.
 */
class ReadGeoArchive
    : public EffectInstance
    , public MaterialProvider
    , public ArchiveProvider
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

Q_SIGNALS:

    /** @brief Emitted at the end of loadArchiveFile() — successful or not. */
    void archiveReloaded();

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ReadGeoArchive(n); }

    ReadGeoArchive(NodePtr node);
    virtual ~ReadGeoArchive();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_READGEOARCHIVE; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ReadGeoArchive"; }

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

    // --- ArchiveProvider interface (multi-emit) ---
    int getSceneNodeCount() const OVERRIDE;
    bool getSceneNodeAt(int idx, double time,
                        std::string& outName,
                        int& outParentLocalIndex,
                        bool& outIsMesh,
                        float outLocalMatrix[16]) const OVERRIDE;
    MeshDataPtr getMeshDataAt(int idx, double time = -1.0) const OVERRIDE;
    std::vector<ArchiveTreeEntry> getEntryTree() const OVERRIDE;
    bool getEntryWorldMatrix(int idx, double time, float outWorld[16]) const OVERRIDE;

    // MaterialProvider interface — forwarded to the Material3D on input 0 when
    // connected; otherwise the first PBR material found in the file.
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

    // FPS for animation time mapping: the fps knob when set, else the project
    // frame rate. Shared by every per-frame accessor.
    double archiveFps() const;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    // Auto-load on project restore (the file knob is restored but knobChanged
    // does not fire for it).
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    void loadArchiveFile(const std::string& path);

    std::unique_ptr<ReadGeoArchivePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READGEOARCHIVE_H
