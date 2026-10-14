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

// Shared interface for the ReadGeo file-format importers that are backed by
// vendored third-party parsers:
//
//   glTF 2.0 / GLB -> cgltf   (libs/cgltf, MIT)  - ReadGeoGltf.cpp
//   FBX            -> ufbx    (libs/ufbx,  MIT)  - ReadGeoFbx.cpp
//
// Both formats are scene formats: they carry a node hierarchy, per-node
// transforms, and per-part PBR materials. The stage-1 contract with ReadGeo
// is:
//
//   - the loader parses the WHOLE file and returns every renderable part
//     (a part = one glTF primitive / one FBX mesh x material) with its
//     file-authored transform ALREADY BAKED into the vertex positions, so
//     multi-part assets appear assembled exactly as in the authoring DCC;
//   - ReadGeo fills the Object dropdown with "All parts (merged)" plus one
//     entry per part, and builds the requested MeshData from the part list;
//   - MeshData::transform stays identity — the node's own Transform page
//     applies user offsets on top (same policy as the OBJ path).

#include "MeshData.h"

#include <cstddef>
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

/** One renderable part of the file (glTF primitive / FBX mesh+material). */
struct GeoImportPart
{
    std::string label;               // scene path, e.g. "Car/Body" or "Car/Body/Steel"
    std::vector<float> vertices;     // x,y,z interleaved, world-space (baked)
    std::vector<int> faceIndices;    // flattened polygon-vertex indices
    std::vector<int> faceCounts;     // per-face vertex count (n-gon capable)
    std::vector<float> uvs;          // per face-vertex (u,v), parallel to faceIndices
    bool hasUVs = false;
    GeoImportMaterial material;
};

struct GeoImportResult
{
    bool ok = false;
    std::string error;
    std::vector<GeoImportPart> parts;
    int numMaterials = 0;            // total materials in the file (for the Info line)
};

/** glTF 2.0 (ASCII .gltf + binary .glb) via cgltf. Implemented in ReadGeoGltf.cpp. */
GeoImportResult loadGltfGeoImport(const std::string& path);

/** FBX (binary + ASCII) via ufbx. Implemented in ReadGeoFbx.cpp. */
GeoImportResult loadFbxGeoImport(const std::string& path);

// --- helpers shared by the importers (ReadGeoImport.cpp) -------------------

/** Transform every xyz position in `verts` in place by the column-major 4x4
 *  matrix `m` (bakes the file-authored node transform into the vertices). */
void importBakeXform(std::vector<float>& verts, const float m[16]);

/** Append `part`'s geometry to `mesh`, re-basing vertex indices by
 *  mesh->numVertices and expanding part UVs (or zero-padding when the part
 *  has none). Call importBuildEdgeIndices() once after all parts are added. */
void importAppendPart(const GeoImportPart& part, MeshData* mesh);

/** Rebuild mesh->edgeIndices from faceIndices/faceCounts (wireframe). */
void importBuildEdgeIndices(MeshData* mesh);

/** Write an embedded texture blob (raw PNG/JPEG bytes) to
 *  <TEMP>/natron_<tag>_<fnv1a-hash>.<ext>, sniffing the type from the magic
 *  bytes. Returns the path, or empty on failure. Reuses an existing file for
 *  identical content so repeated loads don't rewrite the temp file. */
std::string importWriteEmbeddedTexture(const void* bytes, std::size_t size, const char* tag);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_READGEOIMPORT_H
