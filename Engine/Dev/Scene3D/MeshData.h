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

#ifndef NATRON_ENGINE_MESHDATA_H
#define NATRON_ENGINE_MESHDATA_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief Mesh data for 3D viewport / Cycles / Scanline rendering.
 *
 * Lives outside any individual node so that anything contributing geometry
 * to the SceneGraph (ReadGeo for a single mesh, ReadAlembicArchive for a
 * whole hierarchy, future procedural-mesh nodes) can hand the same payload
 * to consumers.
 *
 * Stores vertices, polygon-soup face indices + per-face vertex counts, an
 * edge list for wireframe drawing, optional UVs, and a local-to-world
 * transform (used when the mesh carries its own transform; archive entries
 * typically leave this identity and let the SceneGraph chain handle it).
 */
struct MeshData
{
    std::vector<float> vertices;     // x,y,z interleaved
    std::vector<int> faceIndices;    // flattened polygon-vertex indices
    std::vector<int> faceCounts;     // per-face vertex count (for n-gon support)
    std::vector<int> edgeIndices;    // line index pairs for wireframe

    // Texture coordinates. Two paths depending on what produced the mesh:
    //
    //   texCoordComponents == 2 (default): standard (u, v) pairs in `uvs`. All
    //     existing producers (ReadGeo, ReadAlembicArchive, primitives) use this
    //     path. ScanlineRender emits glTexCoord2f per vertex.
    //
    //   texCoordComponents == 3: projective (s, t, w) triples in `texCoords`.
    //     Only UVProject in Perspective mode with Generate Perspective ON writes
    //     this. `uvs` is unused; ScanlineRender emits glTexCoord4f(s, t, 0, w)
    //     so GL does perspective-correct interpolation + fragment-level divide.
    //
    //   texCoordComponents == 0: no texture coords (hasUVs == false).
    std::vector<float> uvs;          // u,v per face-vertex (texCoordComponents == 2)
    std::vector<float> texCoords;    // s,t,w per face-vertex (texCoordComponents == 3)
    int texCoordComponents;          // 0 / 2 / 3
    bool hasUVs;                     // true when texCoordComponents > 0

    float transform[16];             // 4x4 column-major transform
    std::size_t numVertices;
    std::size_t numFaces;

    MeshData() : texCoordComponents(0), hasUVs(false), numVertices(0), numFaces(0)
    {
        for (int i = 0; i < 16; ++i) transform[i] = (i % 5 == 0) ? 1.0f : 0.0f; // identity
    }
};

typedef std::shared_ptr<MeshData> MeshDataPtr;

/** Flip every face's winding order (and the parallel per-face-vertex uv /
 *  texCoord slots, which are indexed by position within faceIndices) so that
 *  derived normals point the other way. Every consumer — viewport lighting,
 *  ScanlineRender, Cycles, Project3D front/back culling — takes normals from
 *  winding, so this is the single place a "Reverse Normals" knob needs to
 *  touch. edgeIndices are direction-agnostic and stay as-is. */
inline void reverseMeshWinding(MeshData& m)
{
    const std::size_t nIdx = m.faceIndices.size();
    auto reverseRange = [&m](std::size_t off, int c) {
        for (int i = 0, j = c - 1; i < j; ++i, --j) {
            std::swap(m.faceIndices[off + i], m.faceIndices[off + j]);
            if (m.texCoordComponents == 2 && ((off + j) * 2 + 1) < m.uvs.size()) {
                std::swap(m.uvs[(off + i) * 2],     m.uvs[(off + j) * 2]);
                std::swap(m.uvs[(off + i) * 2 + 1], m.uvs[(off + j) * 2 + 1]);
            } else if (m.texCoordComponents == 3 && ((off + j) * 3 + 2) < m.texCoords.size()) {
                for (int k = 0; k < 3; ++k) {
                    std::swap(m.texCoords[(off + i) * 3 + k], m.texCoords[(off + j) * 3 + k]);
                }
            }
        }
    };
    if (!m.faceCounts.empty()) {
        std::size_t off = 0;
        for (std::size_t f = 0; f < m.faceCounts.size(); ++f) {
            const int c = m.faceCounts[f];
            if (c < 2 || off + (std::size_t)c > nIdx) {
                off += (std::size_t)std::max(0, c);
                continue;
            }
            reverseRange(off, c);
            off += (std::size_t)c;
        }
    } else {
        for (std::size_t off = 0; off + 2 < nIdx; off += 3) {
            reverseRange(off, 3);
        }
    }
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_MESHDATA_H
