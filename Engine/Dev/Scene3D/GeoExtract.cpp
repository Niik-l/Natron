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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GeoExtract.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../EffectInstance.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../DotUtils.h"
#include "Card3D.h"
#include "Cube3D.h"
#include "Cylinder3D.h"
#include "MaterialProvider.h"
#include "MeshData.h"
#include "MeshProvider.h"
#include "ReadAlembicArchive.h"
#include "SceneGraph.h"
#include "Sphere3D.h"

NATRON_NAMESPACE_ENTER

void
geoMat4Identity(float m[16])
{
    for (int i = 0; i < 16; ++i) {
        m[i] = 0.f;
    }
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

void
geoMat4Mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; ++c) {
        for (int rw = 0; rw < 4; ++rw) {
            r[c * 4 + rw] = a[0 * 4 + rw] * b[c * 4 + 0] + a[1 * 4 + rw] * b[c * 4 + 1]
                          + a[2 * 4 + rw] * b[c * 4 + 2] + a[3 * 4 + rw] * b[c * 4 + 3];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void
geoMat4MulPoint(const float m[16], const float p[3], float out[3])
{
    const float x = p[0], y = p[1], z = p[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

void
geoMat4MulDir(const float m[16], const float d[3], float out[3])
{
    const float x = d[0], y = d[1], z = d[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z;
    out[1] = m[1] * x + m[5] * y + m[9]  * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
}

TriMesh::TriMesh()
    : material(NULL)
{
    geoMat4Identity(localMatrix);
    bboxMin[0] = bboxMin[1] = bboxMin[2] = 0.f;
    bboxMax[0] = bboxMax[1] = bboxMax[2] = 0.f;
}

void
TriMesh::computeVertexNormals()
{
    const size_t nv = numVerts();
    if (nv == 0 || normals.size() == nv * 3) {
        return;
    }
    normals.assign(nv * 3, 0.f);
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const int a = tris[t], b = tris[t + 1], c = tris[t + 2];
        if (a < 0 || b < 0 || c < 0 || a >= (int)nv || b >= (int)nv || c >= (int)nv) {
            continue;
        }
        const float* pa = &verts[(size_t)a * 3];
        const float* pb = &verts[(size_t)b * 3];
        const float* pc = &verts[(size_t)c * 3];
        const float ex = pb[0] - pa[0], ey = pb[1] - pa[1], ez = pb[2] - pa[2];
        const float fx = pc[0] - pa[0], fy = pc[1] - pa[1], fz = pc[2] - pa[2];
        // Cross product length = 2 x area: the sum weights big faces more.
        const float nx = ey * fz - ez * fy;
        const float ny = ez * fx - ex * fz;
        const float nz = ex * fy - ey * fx;
        const int idx[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) {
            float* n = &normals[(size_t)idx[k] * 3];
            n[0] += nx; n[1] += ny; n[2] += nz;
        }
    }
    for (size_t v = 0; v < nv; ++v) {
        float* n = &normals[v * 3];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12f) {
            n[0] /= len; n[1] /= len; n[2] /= len;
        } else {
            n[0] = 0.f; n[1] = 1.f; n[2] = 0.f;
        }
    }
}

namespace {

void
finishBounds(TriMesh& out)
{
    const size_t nv = out.numVerts();
    if (nv == 0) {
        return;
    }
    out.bboxMin[0] = out.bboxMax[0] = out.verts[0];
    out.bboxMin[1] = out.bboxMax[1] = out.verts[1];
    out.bboxMin[2] = out.bboxMax[2] = out.verts[2];
    for (size_t v = 1; v < nv; ++v) {
        for (int k = 0; k < 3; ++k) {
            const float c = out.verts[v * 3 + k];
            out.bboxMin[k] = std::min(out.bboxMin[k], c);
            out.bboxMax[k] = std::max(out.bboxMax[k], c);
        }
    }
}

// The primitive generators give per-vertex (x,y,z,u,v,nx,ny,nz). Expand the
// UVs to per corner and make each triangle's winding agree with its normals:
// the generators are not consistent even within one mesh (Cylinder3D winds its
// caps opposite to its wall) and every consumer takes the geometric normal
// from the winding. Same rule as CyclesRenderer's nodeMeshToCycles.
template <class V>
void
primitiveToTriMesh(const std::vector<V>& cv, std::vector<int>& tris, TriMesh& out)
{
    const size_t nv = cv.size();
    out.verts.resize(nv * 3);
    out.normals.resize(nv * 3);
    for (size_t i = 0; i < nv; ++i) {
        out.verts[i * 3 + 0] = cv[i].x; out.verts[i * 3 + 1] = cv[i].y; out.verts[i * 3 + 2] = cv[i].z;
        out.normals[i * 3 + 0] = cv[i].nx; out.normals[i * 3 + 1] = cv[i].ny; out.normals[i * 3 + 2] = cv[i].nz;
    }
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const int a = tris[t], b = tris[t + 1], c = tris[t + 2];
        if (a < 0 || b < 0 || c < 0 || a >= (int)nv || b >= (int)nv || c >= (int)nv) {
            continue;
        }
        const float ex = cv[b].x - cv[a].x, ey = cv[b].y - cv[a].y, ez = cv[b].z - cv[a].z;
        const float fx = cv[c].x - cv[a].x, fy = cv[c].y - cv[a].y, fz = cv[c].z - cv[a].z;
        const float gx = ey * fz - ez * fy, gy = ez * fx - ex * fz, gz = ex * fy - ey * fx;
        const float nx = cv[a].nx + cv[b].nx + cv[c].nx;
        const float ny = cv[a].ny + cv[b].ny + cv[c].ny;
        const float nz = cv[a].nz + cv[b].nz + cv[c].nz;
        if (gx * nx + gy * ny + gz * nz < 0.f) {
            std::swap(tris[t + 1], tris[t + 2]);
        }
    }
    out.tris = tris;
    out.uvs.resize(out.tris.size() * 2);
    for (size_t c = 0; c < out.tris.size(); ++c) {
        const int v = out.tris[c];
        const bool ok = v >= 0 && v < (int)nv;
        out.uvs[c * 2 + 0] = ok ? cv[v].u : 0.f;
        out.uvs[c * 2 + 1] = ok ? cv[v].v : 0.f;
    }
}

// Fan-triangulate a MeshData polygon soup; UVs come per face-vertex so they map
// straight onto triangle corners (no seam loss, unlike a per-vertex rewrite).
void
meshDataToTriMesh(const MeshData& mesh, const float* xform /* column-major or null */, TriMesh& out)
{
    const size_t baseV = out.numVerts();
    const size_t nv = mesh.numVertices;
    out.verts.resize((baseV + nv) * 3);
    for (size_t v = 0; v < nv; ++v) {
        const float* p = &mesh.vertices[v * 3];
        float* o = &out.verts[(baseV + v) * 3];
        if (xform) {
            o[0] = xform[0] * p[0] + xform[4] * p[1] + xform[8]  * p[2] + xform[12];
            o[1] = xform[1] * p[0] + xform[5] * p[1] + xform[9]  * p[2] + xform[13];
            o[2] = xform[2] * p[0] + xform[6] * p[1] + xform[10] * p[2] + xform[14];
        } else {
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        }
    }
    const bool hasUV = mesh.hasUVs && mesh.texCoordComponents == 2 && mesh.uvs.size() == mesh.faceIndices.size() * 2;
    const bool wantUV = hasUV && (out.tris.empty() || out.hasUVs());
    if (!wantUV && !out.uvs.empty()) {
        out.uvs.clear(); // a UV-less part joins: the whole mesh goes UV-less
    }
    auto pushTri = [&](size_t c0, size_t c1, size_t c2) {
        const int i0 = mesh.faceIndices[c0], i1 = mesh.faceIndices[c1], i2 = mesh.faceIndices[c2];
        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= (int)nv || i1 >= (int)nv || i2 >= (int)nv) {
            return;
        }
        out.tris.push_back((int)baseV + i0);
        out.tris.push_back((int)baseV + i1);
        out.tris.push_back((int)baseV + i2);
        if (wantUV) {
            const size_t cs[3] = {c0, c1, c2};
            for (int k = 0; k < 3; ++k) {
                out.uvs.push_back(mesh.uvs[cs[k] * 2 + 0]);
                out.uvs.push_back(mesh.uvs[cs[k] * 2 + 1]);
            }
        }
    };
    if (!mesh.faceCounts.empty()) {
        size_t off = 0;
        for (size_t f = 0; f < mesh.faceCounts.size(); ++f) {
            const int c = mesh.faceCounts[f];
            if (c < 3 || off + (size_t)c > mesh.faceIndices.size()) {
                off += (size_t)std::max(0, c);
                continue;
            }
            for (int i = 1; i + 1 < c; ++i) {
                pushTri(off, off + i, off + i + 1);
            }
            off += (size_t)c;
        }
    } else {
        for (size_t i = 0; i + 2 < mesh.faceIndices.size(); i += 3) {
            pushTri(i, i + 1, i + 2);
        }
    }
}

float
readKnobDouble(const EffectInstancePtr& eff, const char* name, double time, float fallback)
{
    KnobIPtr k = eff->getKnobByName(name);
    if (!k) {
        return fallback;
    }
    KnobDouble* kd = dynamic_cast<KnobDouble*>(k.get());
    return kd ? (float)kd->getValueAtTime(time) : fallback;
}

} // namespace

bool
extractTriangleMesh(const EffectInstancePtr& geoIn, double time, TriMesh& out)
{
    out = TriMesh();
    EffectInstancePtr geo = skipDots(geoIn);
    if (!geo) {
        return false;
    }
    {
        NodePtr n = geo->getNode();
        if (!n || n->isNodeDisabled()) {
            return false;
        }
        out.sourceName = n->getScriptName_mt_safe();
    }
    out.material = dynamic_cast<MaterialProvider*>(geo.get());

    if (Cube3D* cube = dynamic_cast<Cube3D*>(geo.get())) {
        std::vector<Cube3D::CubeVertex> cv;
        std::vector<int> tris;
        cube->generateCubeMesh(time, cv, tris);
        primitiveToTriMesh(cv, tris, out);
        double tx, ty, tz, rx, ry, rz, sx, sy, sz;
        cube->getCubeTransform(time, tx, ty, tz, rx, ry, rz, sx, sy, sz);
        SceneGraph::buildTRS((float)tx, (float)ty, (float)tz, (float)rx, (float)ry, (float)rz,
                             (float)sx, (float)sy, (float)sz, out.localMatrix);
    } else if (Sphere3D* sphere = dynamic_cast<Sphere3D*>(geo.get())) {
        std::vector<Sphere3D::SphereVertex> cv;
        std::vector<int> tris;
        sphere->generateSphereMesh(time, cv, tris);
        primitiveToTriMesh(cv, tris, out);
        double tx, ty, tz, rx, ry, rz, sx, sy, sz;
        sphere->getSphereTransform(time, tx, ty, tz, rx, ry, rz, sx, sy, sz);
        SceneGraph::buildTRS((float)tx, (float)ty, (float)tz, (float)rx, (float)ry, (float)rz,
                             (float)sx, (float)sy, (float)sz, out.localMatrix);
    } else if (Card3D* card = dynamic_cast<Card3D*>(geo.get())) {
        std::vector<Card3D::CardVertex> cv;
        std::vector<int> tris;
        card->generateCardMesh(time, cv, tris);
        primitiveToTriMesh(cv, tris, out);
        double tx, ty, tz, rx, ry, rz, sx, sy;
        card->getCardTransform(time, tx, ty, tz, rx, ry, rz, sx, sy);
        SceneGraph::buildTRS((float)tx, (float)ty, (float)tz, (float)rx, (float)ry, (float)rz,
                             (float)sx, (float)sy, 1.f, out.localMatrix);
    } else if (Cylinder3D* cyl = dynamic_cast<Cylinder3D*>(geo.get())) {
        std::vector<Cylinder3D::CylinderVertex> cv;
        std::vector<int> tris;
        cyl->generateCylinderMesh(time, cv, tris);
        primitiveToTriMesh(cv, tris, out);
        double tx, ty, tz, rx, ry, rz, sx, sy, sz;
        cyl->getCylinderTransform(time, tx, ty, tz, rx, ry, rz, sx, sy, sz);
        SceneGraph::buildTRS((float)tx, (float)ty, (float)tz, (float)rx, (float)ry, (float)rz,
                             (float)sx, (float)sy, (float)sz, out.localMatrix);
    } else if (ReadAlembicArchive* arch = dynamic_cast<ReadAlembicArchive*>(geo.get())) {
        // Every entry baked with its own composed transform into one soup. The
        // archive's user T/R/S is part of each entry's world matrix, so the
        // TriMesh transform stays identity.
        const int n = arch->getSceneNodeCount();
        for (int i = 0; i < n; ++i) {
            MeshDataPtr md = arch->getMeshDataAt(i, time);
            if (!md || md->numVertices == 0) {
                continue;
            }
            float w[16];
            if (!arch->getEntryWorldMatrix(i, time, w)) {
                geoMat4Identity(w);
            }
            meshDataToTriMesh(*md, w, out);
        }
    } else if (MeshProvider* mp = dynamic_cast<MeshProvider*>(geo.get())) {
        MeshDataPtr md = mp->getMeshData(time);
        if (!md || md->numVertices == 0) {
            return false;
        }
        meshDataToTriMesh(*md, NULL, out);
        // Node transform = user T/R/S knobs x the embedded (row-major) mesh
        // transform, exactly as SceneGraph::rebuild and ScanlineRender compose
        // it, so an instanced ReadGeo sits at the size its knobs say.
        float embedded[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                embedded[c * 4 + r] = md->transform[r * 4 + c];
            }
        }
        const float us = readKnobDouble(geo, "uniformScale", time, 1.f);
        float userTRS[16];
        SceneGraph::buildTRS(readKnobDouble(geo, "translateX", time, 0.f), readKnobDouble(geo, "translateY", time, 0.f),
                             readKnobDouble(geo, "translateZ", time, 0.f),
                             readKnobDouble(geo, "rotateX", time, 0.f), readKnobDouble(geo, "rotateY", time, 0.f),
                             readKnobDouble(geo, "rotateZ", time, 0.f),
                             readKnobDouble(geo, "scaleX", time, 1.f) * us, readKnobDouble(geo, "scaleY", time, 1.f) * us,
                             readKnobDouble(geo, "scaleZ", time, 1.f) * us, userTRS);
        geoMat4Mul(out.localMatrix, userTRS, embedded);
    } else {
        return false;
    }

    if (out.tris.empty() || out.verts.empty()) {
        return false;
    }
    if (!out.hasUVs()) {
        out.uvs.clear();
    }
    finishBounds(out);
    return true;
}

NATRON_NAMESPACE_EXIT
