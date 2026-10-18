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

#include "ReadGeoImport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

NATRON_NAMESPACE_ENTER

// --- shared math helpers for the archive importers/evaluators ---------------

void
geoArchiveMatIdentity(float m[16])
{
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

// out = a * b (column-major; out must not alias a or b)
void
geoArchiveMatMul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int c = 0; c < 4; ++c) {
        for (int rw = 0; rw < 4; ++rw) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + rw] * b[c * 4 + k];
            }
            r[c * 4 + rw] = s;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

// Column-major TRS composition from translation + (x,y,z,w) quaternion +
// per-axis scale — the glTF node transform and the rest pose of FBX nodes.
void
geoArchiveComposeTRS(const float t[3], const float q[4], const float s[3], float out[16])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];

    const float xy = 2.0f * x * y, xz = 2.0f * x * z, yz = 2.0f * y * z;
    const float wx = 2.0f * w * x, wy = 2.0f * w * y, wz = 2.0f * w * z;
    const float xx = 2.0f * x * x, yy = 2.0f * y * y, zz = 2.0f * z * z;

    out[0]  = (1.0f - yy - zz) * s[0]; out[1]  = (xy + wz) * s[0];
    out[2]  = (xz - wy) * s[0];        out[3]  = 0.0f;
    out[4]  = (xy - wz) * s[1];        out[5]  = (1.0f - xx - zz) * s[1];
    out[6]  = (yz + wx) * s[1];        out[7]  = 0.0f;
    out[8]  = (xz + wy) * s[2];        out[9]  = (yz - wx) * s[2];
    out[10] = (1.0f - xx - yy) * s[2]; out[11] = 0.0f;
    out[12] = t[0]; out[13] = t[1]; out[14] = t[2]; out[15] = 1.0f;
}

void
importBakeXform(std::vector<float>& verts, const float m[16])
{
    // Column-major: x' = m[0]*x + m[4]*y + m[8]*z + m[12], etc. W is assumed 1
    // (no projective transforms come out of glTF nodes / ufbx matrices).
    for (std::size_t i = 0; i + 2 < verts.size(); i += 3) {
        const float x = verts[i];
        const float y = verts[i + 1];
        const float z = verts[i + 2];
        verts[i]     = m[0] * x + m[4] * y + m[8] * z + m[12];
        verts[i + 1] = m[1] * x + m[5] * y + m[9] * z + m[13];
        verts[i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    }
}

void
applyGeoMorphSet(MeshData& mesh, const GeoMorphSet& morphs, const float* weights)
{
    if (!weights || morphs.targets.empty()) return;
    const std::size_t numVerts = mesh.numVertices;
    float* verts = mesh.vertices.empty() ? NULL : mesh.vertices.data();
    if (!verts) return;

    for (std::size_t t = 0; t < morphs.targets.size(); ++t) {
        const float w = weights[t];
        if (w == 0.0f) continue;
        const GeoMorphTarget& tgt = morphs.targets[t];
        if (tgt.indices.empty()) {
            // Dense target (glTF): deltas for every vertex.
            const std::size_t n = std::min(numVerts, tgt.deltas.size() / 3);
            for (std::size_t v = 0; v < n; ++v) {
                verts[v * 3]     += w * tgt.deltas[v * 3];
                verts[v * 3 + 1] += w * tgt.deltas[v * 3 + 1];
                verts[v * 3 + 2] += w * tgt.deltas[v * 3 + 2];
            }
        } else {
            // Sparse target (FBX blend shape): parallel index/delta arrays.
            const std::size_t n = tgt.indices.size();
            for (std::size_t i = 0; i < n; ++i) {
                const unsigned int vi = tgt.indices[i];
                if ((std::size_t)vi >= numVerts) continue;
                verts[(std::size_t)vi * 3]     += w * tgt.deltas[i * 3];
                verts[(std::size_t)vi * 3 + 1] += w * tgt.deltas[i * 3 + 1];
                verts[(std::size_t)vi * 3 + 2] += w * tgt.deltas[i * 3 + 2];
            }
        }
    }
}

void
importBuildEdgeIndices(MeshData* mesh)
{
    mesh->edgeIndices.clear();
    mesh->edgeIndices.reserve(mesh->faceIndices.size() * 2);
    const std::size_t nv = mesh->numVertices;
    std::size_t off = 0;
    for (std::size_t f = 0; f < mesh->faceCounts.size(); ++f) {
        const int c = mesh->faceCounts[f];
        if (c < 2 || off + (std::size_t)c > mesh->faceIndices.size()) {
            off += (std::size_t)std::max(0, c);
            continue;
        }
        for (int i = 0; i < c; ++i) {
            const int a = mesh->faceIndices[off + i];
            const int b = mesh->faceIndices[off + ((i + 1) % c)];
            if (a >= 0 && (std::size_t)a < nv && b >= 0 && (std::size_t)b < nv) {
                mesh->edgeIndices.push_back(a);
                mesh->edgeIndices.push_back(b);
            }
        }
        off += (std::size_t)c;
    }
}

std::string
importWriteEmbeddedTexture(const void* bytes, std::size_t size, const char* tag)
{
    if (!bytes || size < 8) return std::string();

    // Sniff the image type from the magic bytes (covers GLB buffer views and
    // FBX embedded content, which may or may not carry a mime type).
    const unsigned char* b = (const unsigned char*)bytes;
    const char* ext;
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        ext = "png";
    } else if (b[0] == 0xFF && b[1] == 0xD8) {
        ext = "jpg";
    } else {
        return std::string(); // WebP/Basisu etc. need a transcoder — not staged yet
    }

    // FNV-1a 64-bit over the content: identical textures map to one temp file.
    unsigned long long h = 1469598103934665603ULL;
    const unsigned char* p = (const unsigned char*)bytes;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ULL;
    }

    const char* tmpDir = std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp";
    char tmpPath[512];
    std::snprintf(tmpPath, sizeof(tmpPath), "%s/natron_%s_%016llx.%s",
                  tmpDir, tag, (unsigned long long)h, ext);

    // Reuse an existing file with the same content hash (no rewrite).
    FILE* probe = fopen(tmpPath, "rb");
    if (probe) {
        fclose(probe);
        return std::string(tmpPath);
    }

    FILE* f = fopen(tmpPath, "wb");
    if (!f) return std::string();
    const std::size_t written = fwrite(bytes, 1, size, f);
    fclose(f);
    if (written != size) {
        remove(tmpPath);
        return std::string();
    }
    return std::string(tmpPath);
}

// --- GeoArchiveRuntime ------------------------------------------------------

// Shared per-frame runtime for the archive-backed nodes (ReadGeo,
// ReadGeoArchive). The archive data itself is guarded by the OWNING NODE's
// data mutex (ReadGeoArchive's archiveMutex / ReadGeo's _meshMutex); only the
// frame cache is guarded here.

GeoArchiveRuntime::GeoArchiveRuntime() = default;

GeoArchiveRuntime::~GeoArchiveRuntime()
{
    if (fbxScene) freeFbxArchiveScene(fbxScene);
}

void
GeoArchiveRuntime::clear()
{
    if (fbxScene) {
        freeFbxArchiveScene(fbxScene);
        fbxScene = NULL;
    }
    isFbx = false;
    hasAnimation = false;
    animTimeBegin = 0.0;
    animTimeEnd = 0.0;
    data = GeoArchiveResult();
    {
        std::lock_guard<std::mutex> fl(frameMutex);
        frameCache.clear();
    }
}

void
GeoArchiveRuntime::clearFrameCache()
{
    std::lock_guard<std::mutex> fl(frameMutex);
    frameCache.clear();
}

namespace {

// Compose the entry's world matrix from an ALREADY-EVALUATED frame's node
// locals (chain walk using data.nodes' parent indices), with the file's root
// normalization composed once at the top — the same arithmetic worldMatrixAt
// does — used by the bake post-pass below, which must not re-enter
// ensureFrameState.
void
arcWorldFromLocals(const GeoArchiveResult& data,
                   const std::vector<float>& nodeLocals,
                   int nodeIdx, float outWorld[16])
{
    geoArchiveMatIdentity(outWorld);
    if (nodeIdx < 0 || nodeIdx >= (int)data.nodes.size()) return;

    std::vector<int> chain;
    int cur = nodeIdx;
    while (cur >= 0) {
        chain.push_back(cur);
        cur = (cur < (int)data.nodes.size()) ? data.nodes[cur].parentIndex : -1;
    }
    for (int ci = (int)chain.size() - 1; ci >= 0; --ci) {
        const float* local = &nodeLocals[(std::size_t)chain[ci] * 16];
        float r[16];
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += outWorld[k * 4 + row] * local[col * 4 + k];
                }
                r[col * 4 + row] = sum;
            }
        }
        std::memcpy(outWorld, r, sizeof(r));
    }
    if (data.rootMatrix.size() == 16) {
        float r[16];
        geoArchiveMatMul(data.rootMatrix.data(), outWorld, r);
        std::memcpy(outWorld, r, sizeof(r));
    }
}

} // anonymous namespace

std::shared_ptr<const GeoArchiveFrameEval>
GeoArchiveRuntime::ensureFrameState(double fps, int key) const
{
    {
        std::lock_guard<std::mutex> fl(frameMutex);
        std::map<int, std::shared_ptr<const GeoArchiveFrameEval>>::const_iterator it =
            frameCache.find(key);
        if (it != frameCache.end()) return it->second;
    }

    GeoArchiveFrameEval* st = new GeoArchiveFrameEval();
    std::shared_ptr<const GeoArchiveFrameEval> state(st);

    const double srcTime = (double)key / (fps > 0.0 ? fps : 24.0);

    if (isFbx) {
        std::string err;
        if (!evaluateFbxArchiveFrame(fbxScene, data, srcTime, *st, err)) {
            // Evaluation failure: identity locals + rest meshes — never a
            // half-evaluated scene.
            st->nodeLocals.assign(data.nodes.size() * 16, 0.0f);
            for (std::size_t i = 0; i < data.nodes.size(); ++i) {
                geoArchiveMatIdentity(&st->nodeLocals[i * 16]);
            }
            st->deformed.resize(data.meshes.size());
            for (std::size_t m = 0; m < data.meshes.size(); ++m) {
                st->deformed[m] = data.meshes[m].mesh;
            }
        }
    } else {
        evaluateGltfArchiveFrame(data, srcTime, *st);
    }

    // NOTE: the file's root normalization (FBX unit/axes compensation) is NOT
    // folded into the locals here — it must compose ONCE at the top of every
    // chain, and folding it into root-level LOCALS in place would scale the
    // file-space translations of their descendants twice (the deep locals are
    // still in file units). The owning node exposes it through
    // ArchiveProvider::getRootMatrix so every chain — SceneGraph emission,
    // worldMatrixAt, the bake post-pass — composes rootMatrix * chain the
    // same way. ensureFrameState therefore serves raw FILE-SPACE locals +
    // payloads; the bake post-pass below is the only consumer-side fold.

    // Bake post-pass: when enabled, transform every mesh payload through its
    // entry's world matrix (composed from THIS frame's node locals) so the
    // mesh data is already in world space. Identity worlds keep the input
    // payload (skinned glTF payloads are already world-space; rigid parts at
    // the origin don't need a copy).
    if (bakeMeshWorld && !data.nodes.empty()) {
        std::vector<int> meshOwner(data.meshes.size(), -1);
        for (std::size_t i = 0; i < data.nodes.size(); ++i) {
            const GeoArchiveNodeData& nd = data.nodes[i];
            if (nd.isMesh && nd.meshIndex >= 0 &&
                nd.meshIndex < (int)meshOwner.size() && meshOwner[nd.meshIndex] < 0) {
                meshOwner[nd.meshIndex] = (int)i;
            }
        }
        for (std::size_t m = 0; m < data.meshes.size(); ++m) {
            if (!st->deformed[m] || meshOwner[m] < 0) continue;
            float world[16];
            arcWorldFromLocals(data, st->nodeLocals, meshOwner[m], world);
            bool identity = true;
            for (int i = 0; i < 16; ++i) {
                if ((i % 5 == 0) ? (world[i] != 1.0f) : (world[i] != 0.0f)) { identity = false; break; }
            }
            if (identity) continue;
            MeshDataPtr baked(new MeshData(*st->deformed[m]));
            importBakeXform(baked->vertices, world);
            st->deformed[m] = baked;
        }
    }

    std::lock_guard<std::mutex> fl(frameMutex);
    std::pair<std::map<int, std::shared_ptr<const GeoArchiveFrameEval>>::iterator, bool> ins =
        frameCache.insert(std::make_pair(key, state));
    return ins.first->second;
}

void
GeoArchiveRuntime::nodeLocalAt(int nodeIdx, double time, double fps,
                               int frameOffset, float out[16]) const
{
    geoArchiveMatIdentity(out);
    if (nodeIdx < 0 || nodeIdx >= (int)data.nodes.size()) return;

    const double f = time - 1.0 - (double)frameOffset;
    const int k0 = (int)std::floor(f);
    const double frac = f - (double)k0;

    std::shared_ptr<const GeoArchiveFrameEval> st0 = ensureFrameState(fps, k0);
    std::shared_ptr<const GeoArchiveFrameEval> st1 = st0;
    if (frac > 1e-9) {
        st1 = ensureFrameState(fps, k0 + 1);
    }

    const float* a = &st0->nodeLocals[(std::size_t)nodeIdx * 16];
    const float* b = &st1->nodeLocals[(std::size_t)nodeIdx * 16];
    if (st0 == st1 || frac < 1e-9) {
        std::memcpy(out, a, 16 * sizeof(float));
    } else {
        const float al = (float)frac;
        const float oneMinusA = 1.0f - al;
        for (int i = 0; i < 16; ++i) {
            out[i] = a[i] * oneMinusA + b[i] * al;
        }
    }
}

void
GeoArchiveRuntime::worldMatrixAt(int nodeIdx, double time, double fps,
                                 int frameOffset, float outWorld[16]) const
{
    geoArchiveMatIdentity(outWorld);
    if (nodeIdx < 0 || nodeIdx >= (int)data.nodes.size()) return;

    // Walk the parent chain from the entry to the root, then compose
    // rootMatrix * (root -> ... -> entry) (column-major). The root matrix
    // (FBX unit/axes normalization) composes once at the chain top, so the
    // file-space translations deeper in the chain are converted at the end
    // — the same arithmetic SceneGraph does with the root matrix on the
    // archive-root SceneNode.
    std::vector<int> chain;
    int cur = nodeIdx;
    while (cur >= 0) {
        chain.push_back(cur);
        cur = (cur < (int)data.nodes.size()) ? data.nodes[cur].parentIndex : -1;
    }

    for (int ci = (int)chain.size() - 1; ci >= 0; --ci) {
        float local[16];
        nodeLocalAt(chain[ci], time, fps, frameOffset, local);

        // outWorld = outWorld * local (column-major).
        float r[16];
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += outWorld[k * 4 + row] * local[col * 4 + k];
                }
                r[col * 4 + row] = sum;
            }
        }
        std::memcpy(outWorld, r, sizeof(r));
    }

    if (data.rootMatrix.size() == 16) {
        float r[16];
        geoArchiveMatMul(data.rootMatrix.data(), outWorld, r);
        std::memcpy(outWorld, r, sizeof(r));
    }
}

bool
GeoArchiveRuntime::loadFile(const std::string& path, bool reverseWinding, std::string& err)
{
    clear();

    // Format dispatch on the lowercase extension.
    std::string ext;
    {
        const std::size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) {
            ext = path.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
        }
    }

    if (ext == ".glb" || ext == ".gltf") {
        data = loadGltfArchiveImport(path);
    } else if (ext == ".fbx") {
        isFbx = true;
        data = loadFbxArchiveImport(path, &fbxScene);
    } else {
        err = "Unsupported file extension for the scene-format reader. Use .glb, .gltf or .fbx.";
        return false;
    }

    if (!data.ok) {
        err = data.error.empty() ? std::string("The file could not be parsed.") : data.error;
        return false;
    }

    if (reverseWinding) {
        for (GeoArchiveMeshData& md : data.meshes) {
            if (md.mesh) reverseMeshWinding(*md.mesh);
        }
    }

    // Animation detection: glTF channel bindings (TRS or morph weights),
    // skinned payloads, or an FBX file with an animation stack (FBX marks
    // every node animated — the evaluated scene serves the whole tree;
    // conservative and cheap).
    bool anyAnimated = false;
    for (const GeoArchiveNodeData& nd : data.nodes) {
        if (nd.chanTr >= 0 || nd.chanRot >= 0 || nd.chanSc >= 0 ||
            nd.chanWeights >= 0) { anyAnimated = true; break; }
    }
    if (!anyAnimated) {
        for (const GeoArchiveMeshData& md : data.meshes) {
            if (md.skinIndex >= 0) { anyAnimated = true; break; }
        }
    }
    if (isFbx && data.animTimeEnd > data.animTimeBegin) {
        anyAnimated = true;
    }
    hasAnimation = anyAnimated;
    animTimeBegin = data.animTimeBegin;
    animTimeEnd = data.animTimeEnd;

    // Geometry-loader diagnostics: root normalization presence is the #1
    // suspect for "everything renders giant/black" — dump it at load.
    if (std::getenv("NATRON_DEBUG_GEOIO")) {
        std::fprintf(stderr,
                     "[GEOIO] runtime load fmt=%s nodes=%d meshes=%d anims=%d "
                     "rootMatrix=%s\n",
                     isFbx ? "FBX" : "glTF", (int)data.nodes.size(),
                     (int)data.meshes.size(), (int)data.anims.size(),
                     data.rootMatrix.size() == 16
                         ? "present"
                         : "ABSENT");
        if (data.rootMatrix.size() == 16) {
            std::fprintf(stderr,
                         "[GEOIO]   rootMatrix diag=(%.4f,%.4f,%.4f)\n",
                         data.rootMatrix[0], data.rootMatrix[5],
                         data.rootMatrix[10]);
        }
        for (std::size_t i = 0; i < data.nodes.size(); ++i) {
            const GeoArchiveNodeData& nd = data.nodes[i];
            std::fprintf(stderr,
                         "[GEOIO]   entry %d '%s' isMesh=%d parent=%d "
                         "hasTrs=%d localDiag=(%.4f,%.4f,%.4f) "
                         "tr=(%.3f,%.3f,%.3f) sc=(%.3f,%.3f,%.3f)\n",
                         (int)i, nd.fullPath.c_str(), nd.isMesh ? 1 : 0,
                         nd.parentIndex, nd.hasTrs ? 1 : 0,
                         nd.localMatrix[0], nd.localMatrix[5],
                         nd.localMatrix[10], nd.translation[0],
                         nd.translation[1], nd.translation[2], nd.scale[0],
                         nd.scale[1], nd.scale[2]);
        }
    }
    return true;
}

NATRON_NAMESPACE_EXIT
