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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Viewport3DMeshCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

NATRON_NAMESPACE_ENTER

Viewport3DMeshCache::Viewport3DMeshCache()
    : _entries()
    , _bytes(0)
    , _budget((std::size_t)1536 * 1024 * 1024)   // 1.5 GB of GPU buffers across all cached meshes
{
}

Viewport3DMeshCache::~Viewport3DMeshCache()
{
    // No GL calls here: the destructor runs without a guaranteed current
    // context (widget teardown). The context's death frees the buffers.
}

void
Viewport3DMeshCache::destroyEntry(Entry& e)
{
    if (e.cornerVbo) glDeleteBuffers(1, &e.cornerVbo);
    if (e.vertexVbo) glDeleteBuffers(1, &e.vertexVbo);
    if (e.edgeEbo)   glDeleteBuffers(1, &e.edgeEbo);
    if (e.colorVbo)  glDeleteBuffers(1, &e.colorVbo);
    e.cornerVbo = e.vertexVbo = e.edgeEbo = 0;
    e.colorVbo = 0;
    e.cornerCount = e.edgeIndexCount = 0;
    e.hasVColor = false;
    _bytes -= std::min(_bytes, e.bytes);
    e.bytes = 0;
}

void
Viewport3DMeshCache::forgetAll()
{
    _entries.clear();
    _bytes = 0;
}

void
Viewport3DMeshCache::releaseAll()
{
    for (std::map<const MeshData*, Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
        destroyEntry(it->second);
    }
    _entries.clear();
    _bytes = 0;
}

void
Viewport3DMeshCache::purgeIdle(int paintSerial, int maxIdlePaints)
{
    for (std::map<const MeshData*, Entry>::iterator it = _entries.begin(); it != _entries.end(); ) {
        if (it->second.owner.expired() || paintSerial - it->second.lastPaint > maxIdlePaints) {
            destroyEntry(it->second);
            it = _entries.erase(it);
        } else {
            ++it;
        }
    }
}

void
Viewport3DMeshCache::evictFor(std::size_t neededBytes, int paintSerial)
{
    // Least-recently-painted first, never an entry drawn in this paint.
    while (_bytes + neededBytes > _budget && !_entries.empty()) {
        std::map<const MeshData*, Entry>::iterator victim = _entries.end();
        for (std::map<const MeshData*, Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
            if (it->second.lastPaint == paintSerial) continue;
            if (victim == _entries.end() || it->second.lastPaint < victim->second.lastPaint) victim = it;
        }
        if (victim == _entries.end()) break;
        destroyEntry(victim->second);
        _entries.erase(victim);
    }
}

// Pack one triangle corner: position, flat face normal as signed bytes, uv.
static inline void
packCorner(unsigned char* dst, const float* p, const signed char n[4], float u, float v)
{
    std::memcpy(dst, p, 12);
    std::memcpy(dst + Viewport3DMeshCache::kNormalOffset, n, 4);
    std::memcpy(dst + Viewport3DMeshCache::kUVOffset, &u, 4);
    std::memcpy(dst + Viewport3DMeshCache::kUVOffset + 4, &v, 4);
}

static inline void
faceNormalBytes(const float* a, const float* b, const float* c, signed char out[4])
{
    const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float n[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                   e1[2] * e2[0] - e1[0] * e2[2],
                   e1[0] * e2[1] - e1[1] * e2[0] };
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-20f) {
        const float s = 127.0f / len;
        for (int k = 0; k < 3; ++k) {
            const float v = n[k] * s;
            out[k] = (signed char)(v >= 0.f ? (v + 0.5f) : (v - 0.5f));
        }
    } else {
        out[0] = out[1] = out[2] = 0;   // degenerate: lit at ambient only
    }
    out[3] = 0;
}

bool
Viewport3DMeshCache::upload(const MeshData& m, Entry& e)
{
    const int nv = (int)m.numVertices;
    if (nv <= 0 || m.vertices.size() < (std::size_t)nv * 3) return false;

    // Count triangles (fan per polygon, same rule as the immediate-mode path).
    std::size_t nTris = 0;
    if (!m.faceCounts.empty()) {
        std::size_t off = 0;
        for (std::size_t f = 0; f < m.faceCounts.size(); ++f) {
            const int c = m.faceCounts[f];
            if (c < 3 || off + (std::size_t)c > m.faceIndices.size()) { off += (std::size_t)std::max(0, c); continue; }
            nTris += (std::size_t)(c - 2);
            off += (std::size_t)c;
        }
    } else {
        nTris = m.faceIndices.size() / 3;
    }
    if (nTris == 0) return false;

    const bool perCornerUV = (m.texCoordComponents == 2) && (m.uvs.size() >= m.faceIndices.size() * 2);
    const bool perCornerColor = m.hasVertexColors &&
                                m.vertexColors.size() >= m.faceIndices.size() * 3;
    std::vector<unsigned char> corners;
    corners.resize(nTris * 3 * (std::size_t)kCornerStride);
    std::vector<unsigned char> cornerColors;
    if (perCornerColor) {
        cornerColors.reserve(nTris * 3 * 4);
    }
    unsigned char* dst = corners.data();
    std::size_t written = 0;

    auto emitTri = [&](int v0, int v1, int v2, std::size_t f0, std::size_t f1, std::size_t f2) {
        if (v0 < 0 || v0 >= nv || v1 < 0 || v1 >= nv || v2 < 0 || v2 >= nv) return;
        const float* p0 = &m.vertices[(std::size_t)v0 * 3];
        const float* p1 = &m.vertices[(std::size_t)v1 * 3];
        const float* p2 = &m.vertices[(std::size_t)v2 * 3];
        signed char n[4];
        faceNormalBytes(p0, p1, p2, n);
        float uv[6] = { 0, 0, 0, 0, 0, 0 };
        if (perCornerUV) {
            uv[0] = m.uvs[f0 * 2]; uv[1] = m.uvs[f0 * 2 + 1];
            uv[2] = m.uvs[f1 * 2]; uv[3] = m.uvs[f1 * 2 + 1];
            uv[4] = m.uvs[f2 * 2]; uv[5] = m.uvs[f2 * 2 + 1];
        }
        packCorner(dst + (written    ) * kCornerStride, p0, n, uv[0], uv[1]);
        packCorner(dst + (written + 1) * kCornerStride, p1, n, uv[2], uv[3]);
        packCorner(dst + (written + 2) * kCornerStride, p2, n, uv[4], uv[5]);
        if (perCornerColor) {
            // One face color per triangle corner (the bake repeats it per
            // face-vertex); quantized to normalized GLubyte for the preview.
            const std::size_t fc[3] = { f0, f1, f2 };
            for (int k = 0; k < 3; ++k) {
                const float* c = &m.vertexColors[fc[k] * 3];
                cornerColors.push_back((unsigned char)std::min(255.f, std::max(0.f, c[0] * 255.f + 0.5f)));
                cornerColors.push_back((unsigned char)std::min(255.f, std::max(0.f, c[1] * 255.f + 0.5f)));
                cornerColors.push_back((unsigned char)std::min(255.f, std::max(0.f, c[2] * 255.f + 0.5f)));
                cornerColors.push_back(255);
            }
        }
        written += 3;
    };
    if (!m.faceCounts.empty()) {
        std::size_t off = 0;
        for (std::size_t f = 0; f < m.faceCounts.size(); ++f) {
            const int c = m.faceCounts[f];
            if (c < 3 || off + (std::size_t)c > m.faceIndices.size()) { off += (std::size_t)std::max(0, c); continue; }
            const int v0 = m.faceIndices[off];
            for (int i = 1; i + 1 < c; ++i) {
                emitTri(v0, m.faceIndices[off + i], m.faceIndices[off + i + 1], off, off + i, off + i + 1);
            }
            off += (std::size_t)c;
        }
    } else {
        for (std::size_t i = 0; i + 2 < m.faceIndices.size(); i += 3) {
            emitTri(m.faceIndices[i], m.faceIndices[i + 1], m.faceIndices[i + 2], i, i + 1, i + 2);
        }
    }
    if (written == 0) return false;

    // Wire pass: validated edge index pairs into the original vertex array.
    std::vector<unsigned int> edges;
    edges.reserve(m.edgeIndices.size());
    for (std::size_t i = 0; i + 1 < m.edgeIndices.size(); i += 2) {
        const int i0 = m.edgeIndices[i], i1 = m.edgeIndices[i + 1];
        if (i0 >= 0 && i0 < nv && i1 >= 0 && i1 < nv) { edges.push_back((unsigned int)i0); edges.push_back((unsigned int)i1); }
    }

    const std::size_t cornerBytes = written * (std::size_t)kCornerStride;
    const std::size_t colorBytes  = cornerColors.size();
    const std::size_t vertexBytes = edges.empty() ? 0 : (std::size_t)nv * 12;
    const std::size_t edgeBytes   = edges.size() * sizeof(unsigned int);

    glGenBuffers(1, &e.cornerVbo);
    glBindBuffer(GL_ARRAY_BUFFER, e.cornerVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cornerBytes, corners.data(), GL_STATIC_DRAW);
    e.cornerCount = (GLsizei)written;
    e.hasUV = perCornerUV;
    e.hasVColor = false;
    if (perCornerColor && cornerColors.size() == written * 4) {
        glGenBuffers(1, &e.colorVbo);
        glBindBuffer(GL_ARRAY_BUFFER, e.colorVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)colorBytes, cornerColors.data(), GL_STATIC_DRAW);
        e.hasVColor = true;
    }
    if (!edges.empty()) {
        glGenBuffers(1, &e.vertexVbo);
        glBindBuffer(GL_ARRAY_BUFFER, e.vertexVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertexBytes, m.vertices.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &e.edgeEbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, e.edgeEbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)edgeBytes, edges.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        e.edgeIndexCount = (GLsizei)edges.size();
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    e.bytes = cornerBytes + colorBytes + vertexBytes + edgeBytes;
    _bytes += e.bytes;
    return true;
}

static std::size_t
estimateTriangles(const MeshData& m)
{
    if (m.faceCounts.empty()) return m.faceIndices.size() / 3;
    std::size_t n = 0;
    for (std::size_t f = 0; f < m.faceCounts.size(); ++f) {
        if (m.faceCounts[f] >= 3) n += (std::size_t)(m.faceCounts[f] - 2);
    }
    return n;
}

const Viewport3DMeshCache::Entry*
Viewport3DMeshCache::acquire(const MeshDataPtr& mesh, int paintSerial)
{
    if (!mesh || mesh->numVertices == 0) return NULL;
    const MeshData* key = mesh.get();
    std::map<const MeshData*, Entry>::iterator it = _entries.find(key);
    if (it != _entries.end()) {
        Entry& e = it->second;
        const bool sameOwner = !e.owner.expired() && e.owner.lock().get() == key;
        if (sameOwner && e.version == mesh->version && e.cornerVbo) {
            e.lastPaint = paintSerial;
            return &e;
        }
        destroyEntry(e);
        _entries.erase(it);
    }

    // Rough size before building so a mesh that can never fit is skipped cheaply.
    const std::size_t estTris = estimateTriangles(*mesh);
    const bool estColors = mesh->hasVertexColors &&
                           mesh->vertexColors.size() >= mesh->faceIndices.size() * 3;
    const std::size_t est = estTris * 3 * (std::size_t)kCornerStride
                            + (estColors ? estTris * 3 * 4 : 0)
                            + mesh->numVertices * 12 + mesh->edgeIndices.size() * 4;
    if (est > _budget) return NULL;
    evictFor(est, paintSerial);
    if (_bytes + est > _budget) return NULL;

    Entry e;
    e.owner = mesh;
    e.version = mesh->version;
    e.lastPaint = paintSerial;
    if (!upload(*mesh, e)) {
        destroyEntry(e);
        return NULL;
    }
    std::pair<std::map<const MeshData*, Entry>::iterator, bool> ins = _entries.insert(std::make_pair(key, e));
    return &ins.first->second;
}

NATRON_NAMESPACE_EXIT
