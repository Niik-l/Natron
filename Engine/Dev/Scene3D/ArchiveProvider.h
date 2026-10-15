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

#ifndef NATRON_ENGINE_ARCHIVEPROVIDER_H
#define NATRON_ENGINE_ARCHIVEPROVIDER_H

// Format-neutral interface for multi-emit scene-graph sources: one Natron
// node that exposes a whole file's hierarchy as many SceneNodes.
//
// Implemented by ReadAlembicArchive (.abc) and ReadGeoArchive
// (.glb/.gltf/.fbx). Consumers (SceneGraph::rebuild, ScanlineRender,
// CyclesRenderer) dynamic_cast to this interface so any new archive format
// plugs in without touching the renderers.

#include <string>
#include <vector>

#include "MeshData.h"
#include "ReadGeoImport.h"

NATRON_NAMESPACE_ENTER

/** One entry in a loaded archive's hierarchy, returned by
 *  ArchiveProvider::getEntryTree() for UI consumers (ArchiveTreeWidget). */
struct ArchiveTreeEntry
{
    std::string fullPath;        // "/Camera01Trackers/Tracker1"
    std::string name;            // leaf name, "Tracker1"
    int parentLocalIdx;          // -1 = archive-root; otherwise index into the same vector
    bool isMesh;                 // true => mesh entry, false => locator/transform
    int sampleCount;             // transform sample count (0 for static formats)
    int numVertices;             // mesh vertex count (0 for transform entries)
};

class ArchiveProvider
{
public:
    virtual ~ArchiveProvider() {}

    /** @brief How many entries this archive currently exposes (after filtering). */
    virtual int getSceneNodeCount() const = 0;

    /** @brief Fill out the per-entry scene info for emission.
     *  @param idx Entry index in [0, getSceneNodeCount()).
     *  @param time Natron time at which to sample the entry's matrix.
     *  @param outName Full archive path of this entry (e.g. "/Camera01Trackers/Tracker1").
     *  @param outParentLocalIndex Parent index within this archive's visible entries
     *         (-1 if archive-root, or if the parent was filtered out).
     *  @param outIsMesh true if the entry carries mesh data.
     *  @param outLocalMatrix Column-major 4x4 (OpenGL-style), the entry's local
     *         transform at `time`.
     */
    virtual bool getSceneNodeAt(int idx, double time,
                                std::string& outName,
                                int& outParentLocalIndex,
                                bool& outIsMesh,
                                float outLocalMatrix[16]) const = 0;

    /** @brief Fetch the mesh data for a mesh entry at the given time.
     *  Returns nullptr if the entry isn't a mesh, or no mesh data was read.
     *  The returned shared_ptr is the entry's persistent mesh data (same
     *  address across calls). Mesh vertices are in the entry's local frame —
     *  SceneGraph composes the parent chain when computing the world transform.
     *  Pass time < 0 to skip the per-frame update.
     */
    virtual MeshDataPtr getMeshDataAt(int idx, double time = -1.0) const = 0;

    /** @brief Return the full archive hierarchy (BEFORE filtering), in walk
     *  order: parents always come before their children. parentLocalIdx is
     *  the index within the returned vector. */
    virtual std::vector<ArchiveTreeEntry> getEntryTree() const = 0;

    /** @brief Compute the world-space transform for a visible entry at the
     *  given time by walking the parent chain inside this archive and
     *  multiplying local matrices. Output is column-major float[16]. Required
     *  by callers that don't go through SceneGraph (e.g. ScanlineRender).
     *  @return true on success; false if idx is out of range. */
    virtual bool getEntryWorldMatrix(int idx, double time, float outWorld[16]) const = 0;

    /** @brief The archive's ROOT correction matrix (column-major float[16]) —
     *  the file-format-level normalization every entry's chain must compose
     *  under (e.g. the ufbx root node carrying target_axes + unit scale so
     *  cm-authored FBX node transforms resolve to meters). Identity when the
     *  format has none (glTF, Alembic). The provider's getEntryWorldMatrix
     *  composes it; SceneGraph multiplies it onto the archive-root SceneNode's
     *  local matrix so the emitted parent chain resolves in normalized space.
     *  @return false = identity (the default). */
    virtual bool getRootMatrix(float outRoot[16]) const { geoArchiveMatIdentity(outRoot); return false; }
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_ARCHIVEPROVIDER_H
