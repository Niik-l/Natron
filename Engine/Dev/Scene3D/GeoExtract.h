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

#ifndef NATRON_ENGINE_GEOEXTRACT_H
#define NATRON_ENGINE_GEOEXTRACT_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <string>
#include <vector>

#include "../../../Global/Macros.h"
#include "../../EngineFwd.h"

class MaterialProvider; // global namespace, see MaterialProvider.h

NATRON_NAMESPACE_ENTER

/**
 * @brief A geo node flattened to one triangle soup, in the node's local space.
 *
 * This is the one place that knows how to turn any renderable geo node
 * (Cube3D / Sphere3D / Card3D / Cylinder3D, a MeshProvider such as ReadGeo or
 * GeoBuilder, or a ReadAlembicArchive) into plain triangles. ParticleInstance
 * uses it for its prototypes (so a Megascans ReadGeo can be instanced with its
 * material), Scatter3D uses it for the surface it scatters on.
 */
struct TriMesh
{
    std::vector<float> verts;     // x,y,z per vertex
    std::vector<int>   tris;      // 3 vertex indices per triangle
    std::vector<float> uvs;       // u,v per triangle CORNER (tris.size() * 2); empty when the geo has no UVs
    std::vector<float> normals;   // x,y,z per vertex; empty when the source has none (see computeVertexNormals)
    float localMatrix[16];        // the node's own transform (T/R/S knobs x embedded mesh transform), column-major
    float bboxMin[3], bboxMax[3]; // local-space bounds of `verts`
    MaterialProvider* material;   // the geo node as a material source (its Material3D input is resolved by the renderers); may be null
    std::string sourceName;       // script name of the geo node, for messages

    TriMesh();

    size_t numTris() const { return tris.size() / 3; }
    size_t numVerts() const { return verts.size() / 3; }
    bool hasUVs() const { return uvs.size() == tris.size() * 2; }

    /** Area-weighted per-vertex normals from the triangle winding. No-op when normals are already set. */
    void computeVertexNormals();
};

/**
 * Flatten `geo` (Dots are skipped) at `time`. Returns false when the node is
 * not a geo we know, is disabled, or has no triangles.
 */
bool extractTriangleMesh(const EffectInstancePtr& geo, double time, TriMesh& out);

/** Column-major 4x4 helpers (GL layout, translation in [12..14]). geoMat4Mul: out = a * b, so b is applied first. */
void geoMat4Identity(float m[16]);
void geoMat4Mul(float out[16], const float a[16], const float b[16]);
/** Transform a point (with translation) or a direction (without) by a column-major matrix. */
void geoMat4MulPoint(const float m[16], const float p[3], float out[3]);
void geoMat4MulDir(const float m[16], const float d[3], float out[3]);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GEOEXTRACT_H
