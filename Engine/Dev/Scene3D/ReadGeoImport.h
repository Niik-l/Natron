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

#ifndef NATRON_ENGINE_READGEOIMPORT_H
#define NATRON_ENGINE_READGEOIMPORT_H

// Shared interface for the ReadGeo / ReadGeoArchive scene-format importers,
// backed by vendored third-party parsers:
//
//   glTF 2.0 / GLB -> cgltf   (libs/cgltf, MIT)  - ReadGeoArchiveGltf.cpp
//   FBX            -> ufbx    (libs/ufbx,  MIT)  - ReadGeoArchiveFbx.cpp
//
// Both formats are scene formats: they carry a node hierarchy, per-node
// transforms, per-part PBR materials, skins and animation channels. The
// parsers return the FILE'S OWN hierarchy (GeoArchiveResult below); the two
// node façades consume it:
//
//   - ReadGeoArchive (ArchiveProvider) emits every entry with its parent
//     chain intact — transforms are carried structurally, never baked;
//   - ReadGeo (MeshProvider) builds ONE MeshData per fetch: "All parts
//     (merged)" bakes each part's evaluated world matrix into the vertex
//     positions (so a multi-part asset appears assembled), single-part
//     selection keeps node-local vertices and rides the part's world matrix
//     in MeshData::transform.
//
// A single frame evaluation (GeoArchiveFrameEval) is cached per integer
// frame by GeoArchiveRuntime and shared by both façades.

#include "MeshData.h"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/** PBR material values read from the file, applied to ReadGeo's material
 *  knobs by ReadGeo::applyImportedMaterialKnobs(). Values are linear
 *  (glTF/FBX PBR factors are linear; ReadGeo/Cycles consume linear). */
struct GeoImportMaterial
{
    bool present = false;   // the part actually had a material in the file
    double baseColor[3] = { 0.8, 0.8, 0.8 };
    double roughness = 0.5;
    double metallic = 0.0;
    double specular = 0.5;
    double emission[3] = { 1.0, 1.0, 1.0 };
    double emissionStrength = 0.0;
    double transmission = 0.0;
    double ior = 1.45;
    std::string diffuseMapPath;   // resolved texture file path (empty = none)
};

// --- Archive import (hierarchy-preserving) ----------------------------------
//
// The parsers return the FILE'S OWN hierarchy: node entries (parents before
// children) + mesh entries (one per glTF primitive / FBX material part,
// attached to their owning node), rest-pose vertex data in the node-local
// frame, per-part materials, skins (joint node indices + inverse bind
// matrices) and animation channels as plain data. Animation evaluation
// happens at render time (evaluateXxxArchiveFrame below) so scrubbing never
// re-parses the file.

/** Animation channel target paths (bitmask for node channels). */
enum GeoArchiveChannelPath {
    GEO_ARCHIVE_CHAN_TRANSLATION = 1,
    GEO_ARCHIVE_CHAN_ROTATION = 2,
    GEO_ARCHIVE_CHAN_SCALE = 4,
};

/** Animation keyframe interpolation. */
enum GeoArchiveInterpolation {
    GEO_ARCHIVE_INTERP_LINEAR = 0,
    GEO_ARCHIVE_INTERP_STEP = 1,
    GEO_ARCHIVE_INTERP_CUBICSPLINE = 2,
};

/** One node (transform) or mesh piece of the archive hierarchy. Entries are
 *  parents-before-children, so parentIndex always refers backwards. */
struct GeoArchiveNodeData
{
    std::string fullPath;   // "/Car/Body" — as shown in the Object tree
    std::string name;       // leaf name, "Body"
    int parentIndex = -1;   // index into nodes[]; -1 = archive root
    bool isMesh = false;    // true => a mesh piece (meshIndex valid)
    int meshIndex = -1;     // index into meshes[]

    // Rest local transform. For transform nodes: TRS when hasTrs, else the
    // raw composed matrix in localMatrix. glTF/FBX nodes authored as a bare
    // matrix (hasTrs == false) cannot receive TRS animation channels in
    // stage 2 — they stay static.
    bool hasTrs = false;
    float translation[3] = { 0.f, 0.f, 0.f };
    float rotation[4] = { 0.f, 0.f, 0.f, 1.f };   // x,y,z,w
    float scale[3] = { 1.f, 1.f, 1.f };
    float localMatrix[16];                        // column-major rest matrix

    // Animation: sampler index per channel path into anims[0].samplers
    // (translation / rotation / scale), -1 = not animated.
    int chanTr = -1, chanRot = -1, chanSc = -1;
};

struct GeoArchiveMeshData
{
    MeshDataPtr mesh;                     // rest pose, node-local frame
    int materialIndex = -1;               // index into materials[]
    int skinIndex = -1;                   // index into skins, -1 = rigid

    // glTF skinning: per-vertex joint node indices + weights (4 per vertex,
    // parallel to mesh->numVertices). ufbx files leave these empty — their
    // skinning is evaluated inside ufbx instead.
    std::vector<unsigned int> joints;     // numVerts * 4
    std::vector<float> weights;           // numVerts * 4
};

struct GeoArchiveAnimSamplerData
{
    std::vector<double> times;   // seconds, ascending
    std::vector<float> values;   // times.size() * nComp
    int nComp = 0;               // 3 (translation/scale) or 4 (rotation)
    int interpolation = GEO_ARCHIVE_INTERP_LINEAR;
};

struct GeoArchiveAnimChannelData
{
    int nodeIndex = -1;          // index into nodes[]
    int path = GEO_ARCHIVE_CHAN_TRANSLATION;  // GeoArchiveChannelPath bits (single)
    int sampler = -1;            // index into the owning anim's samplers[]
};

struct GeoArchiveAnimData
{
    std::string name;
    std::vector<GeoArchiveAnimSamplerData> samplers;
    std::vector<GeoArchiveAnimChannelData> channels;
};

/** One evaluated frame: per-node local matrices + per-mesh deformed payloads.
 *  Filled by evaluateGltfArchiveFrame / evaluateFbxArchiveFrame at render
 *  time; cached per integer frame by the node. */
struct GeoArchiveFrameEval
{
    std::vector<float> nodeLocals;    // nodes.size() * 16, column-major
    std::vector<MeshDataPtr> deformed; // meshes.size() entries (may alias rest)
};

struct GeoArchiveResult
{
    bool ok = false;
    std::string error;
    std::vector<GeoArchiveNodeData> nodes;
    std::vector<GeoArchiveMeshData> meshes;
    std::vector<GeoImportMaterial> materials;
    // Per skin: joint node indices + inverse bind matrices (16 floats each).
    std::vector<std::vector<int>> skinJoints;
    std::vector<std::vector<float>> skinIBMs;
    std::vector<GeoArchiveAnimData> anims;
    double animTimeBegin = 0.0;   // source-space seconds of anims[0]
    double animTimeEnd = 0.0;
};

/** glTF 2.0 archive import via cgltf. Implemented in ReadGeoArchiveGltf.cpp. */
GeoArchiveResult loadGltfArchiveImport(const std::string& path);

/** Evaluate one frame of the glTF archive: per-node local matrices from the
 *  animation channels (linear/step/cubic-spline) + skinned vertex positions.
 *  Thread-safe (reads only the const parse result). Same file. */
void evaluateGltfArchiveFrame(const GeoArchiveResult& data,
                              double srcTime,
                              GeoArchiveFrameEval& out);

/** FBX archive import via ufbx. Implemented in ReadGeoArchiveFbx.cpp.
 *  On success *outScene receives the live ufbx_scene — the archive node owns
 *  it (free with freeFbxArchiveScene) because per-frame animation evaluation
 *  (ufbx_evaluate_scene with CPU skinning) runs against it at render time. */
GeoArchiveResult loadFbxArchiveImport(const std::string& path, void** outScene);

/** Evaluate one frame of the FBX archive via ufbx_evaluate_scene (with CPU
 *  skinning). Returns false on evaluation failure (err filled). */
bool evaluateFbxArchiveFrame(void* scene,
                             const GeoArchiveResult& data,
                             double srcTime,
                             GeoArchiveFrameEval& out,
                             std::string& err);

/** Free a scene handed out by loadFbxArchiveImport. Implemented in
 *  ReadGeoArchiveFbx.cpp (keeps ufbx.h out of this header). */
void freeFbxArchiveScene(void* scene);

// --- shared per-frame runtime (GeoArchiveRuntime, ReadGeoImport.cpp) -------
//
// Both ReadGeo and ReadGeoArchive embed one: the parsed archive, the live
// ufbx scene, animation flags, the lazily-filled per-frame evaluation cache
// and the time-mapping helpers. Locking discipline: the frame cache has its
// own mutex (frameMutex); the archive data (data / fbxScene / isFbx /
// animation flags) is guarded by the OWNING NODE's data mutex — every
// method that reads `data` requires the caller to hold it. ReadGeoArchive
// holds its archiveMutex for the duration of loadArchiveFile and around
// every accessor; ReadGeo holds its _meshMutex (its load path and
// getMeshData already serialise on it).

struct GeoArchiveRuntime
{
    // Archive data + format backend. Guarded by the owning node's data mutex.
    GeoArchiveResult data;
    void* fbxScene = NULL;    // live ufbx_scene when the file is .fbx (owned)
    bool isFbx = false;
    bool hasAnimation = false;
    double animTimeBegin = 0.0;
    double animTimeEnd = 0.0;

    GeoArchiveRuntime();
    ~GeoArchiveRuntime();
    GeoArchiveRuntime(const GeoArchiveRuntime&) = delete;
    GeoArchiveRuntime& operator=(const GeoArchiveRuntime&) = delete;

    /** Free the ufbx scene, reset the parsed data, flags and frame cache. */
    void clear();

    /** Frame state for integer cache key `key` (frame 1 = key 0;
     *  srcTime = key / fps). Filled lazily on first request and kept for the
     *  session. Caller holds the data guard. */
    std::shared_ptr<const GeoArchiveFrameEval>
    ensureFrameState(double fps, int key) const;

    /** The entry's node-local matrix at Natron `time` — frameOffset applied,
     *  linearly interpolated between the bracket frames for sub-frame
     *  sampling (motion blur). Caller holds the data guard. */
    void nodeLocalAt(int nodeIdx, double time, double fps, int frameOffset,
                     float out[16]) const;

    /** The entry's world matrix: its parent chain composed root -> ... ->
     *  entry (parents filtered out of the node's visible list still
     *  contribute — they remain in data.nodes). Caller holds the data guard. */
    void worldMatrixAt(int nodeIdx, double time, double fps, int frameOffset,
                       float out[16]) const;

    /** Parse + store `path` (extension dispatch: .glb/.gltf via cgltf, .fbx
     *  via ufbx). Flips every rest mesh's winding when reverseWinding.
     *  Detects animation into hasAnimation / animTimeBegin / animTimeEnd
     *  (channel bindings, skinned payloads, or an FBX animation stack).
     *  Caller holds the data guard. Returns false with `err` filled when
     *  the file cannot be read or parsed. */
    bool loadFile(const std::string& path, bool reverseWinding, std::string& err);

private:
    mutable std::mutex frameMutex;
    mutable std::map<int, std::shared_ptr<const GeoArchiveFrameEval>> frameCache;
};

// --- helpers shared by the importers (ReadGeoImport.cpp) -------------------

/** Transform every xyz position in `verts` in place by the column-major 4x4
 *  matrix `m` (bakes the file-authored node transform into the vertices). */
void importBakeXform(std::vector<float>& verts, const float m[16]);

/** Rebuild mesh->edgeIndices from faceIndices/faceCounts (wireframe). */
void importBuildEdgeIndices(MeshData* mesh);

/** Write an embedded texture blob (raw PNG/JPEG bytes) to
 *  <TEMP>/natron_<tag>_<fnv1a-hash>.<ext>, sniffing the type from the magic
 *  bytes. Returns the path, or empty on failure. Reuses an existing file for
 *  identical content so repeated loads don't rewrite the temp file. */
std::string importWriteEmbeddedTexture(const void* bytes, std::size_t size, const char* tag);

// --- small math helpers shared by the archive parsers/evaluators -----------

void geoArchiveMatIdentity(float m[16]);
void geoArchiveMatMul(const float a[16], const float b[16], float out[16]);
void geoArchiveComposeTRS(const float t[3], const float q[4], const float s[3], float out[16]);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READGEOIMPORT_H
