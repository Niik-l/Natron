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

// FBX ARCHIVE import for ReadGeoArchive, backed by the vendored ufbx parser
// (libs/ufbx, MIT / public domain).
//
// Load once: node entries (every FBX node) + per-material mesh entries in
// node-local space, materials mapped to PBR, skins recorded as skin indices
// (the per-vertex weights stay INSIDE ufbx — ufbx evaluates CPU skinning for
// us at frame-evaluation time, so unlike glTF no weight arrays are copied).
//
// The ufbx_scene stays alive with the node (loadFbxArchiveImport hands it
// out through void*): evaluateFbxArchiveFrame() runs ufbx_evaluate_scene at
// each requested frame with evaluate_skinning, and copies the evaluated
// local transforms + skinned positions into the format-neutral frame state.
//
// Walk order matters: both the load walk and the evaluate walk traverse the
// scene tree identically (pre-order, parents before children), so entry
// indices correspond 1:1 between parse and evaluation.

#include "ReadGeoImport.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ufbx.h"

#include <algorithm>

NATRON_NAMESPACE_ENTER

namespace {

std::string
arcUfbxToString(const ufbx_string& s)
{
    if (!s.data || s.length == 0) return std::string();
    return std::string(s.data, s.length);
}

// ufbx_matrix -> column-major float[16] for GL. ufbx stores column-major
// 3x4 with an implicit bottom row (0,0,0,1): cols[4] of vec3, named
// m<row><col> — m00,m10,m20 = column 0. GL wants out[c*4+r] with the bottom
// row explicit.
void
ufbxMatToColMajor(const ufbx_matrix& m, float out[16])
{
    out[0]  = (float)m.m00; out[1]  = (float)m.m10; out[2]  = (float)m.m20; out[3]  = 0.0f;
    out[4]  = (float)m.m01; out[5]  = (float)m.m11; out[6]  = (float)m.m21; out[7]  = 0.0f;
    out[8]  = (float)m.m02; out[9]  = (float)m.m12; out[10] = (float)m.m22; out[11] = 0.0f;
    out[12] = (float)m.m03; out[13] = (float)m.m13; out[14] = (float)m.m23; out[15] = 1.0f;
}

// Resolve a texture to a usable file path (mirrors the flat loader's logic).
std::string
resolveUfbxTextureArc(const ufbx_texture* tex, const std::string& dir)
{
    if (!tex) return std::string();
    if (tex->content.data && tex->content.size > 0) {
        std::string embedded = importWriteEmbeddedTexture(tex->content.data, tex->content.size, "fbx_tex");
        if (!embedded.empty()) return embedded;
    }
    std::string rel = arcUfbxToString(tex->relative_filename);
    std::string abs = arcUfbxToString(tex->absolute_filename);
    std::string file = arcUfbxToString(tex->filename);
    if (!abs.empty()) return abs;
    if (!rel.empty()) {
        if (dir.empty()) return rel;
        const char last = dir[dir.size() - 1];
        if (last == '/' || last == '\\') return dir + rel;
        return dir + "/" + rel;
    }
    if (!file.empty()) return file;
    return std::string();
}

GeoImportMaterial
mapUfbxMaterialArc(const ufbx_material* m, const std::string& dir)
{
    GeoImportMaterial out;
    if (!m) return out;
    out.present = true;

    const ufbx_material_map& baseColor = m->pbr.base_color;
    if (baseColor.has_value && baseColor.value_components >= 3) {
        out.baseColor[0] = baseColor.value_vec3.x;
        out.baseColor[1] = baseColor.value_vec3.y;
        out.baseColor[2] = baseColor.value_vec3.z;
    } else if (!baseColor.texture) {
        out.present = false; // nothing authored — leave the default material
    }
    if (m->pbr.roughness.has_value) out.roughness = m->pbr.roughness.value_real;
    if (m->pbr.metalness.has_value) out.metallic = m->pbr.metalness.value_real;
    if (m->pbr.specular_factor.has_value) out.specular = m->pbr.specular_factor.value_real;
    if (m->pbr.specular_ior.has_value && m->pbr.specular_ior.value_real >= 1.0) {
        out.ior = m->pbr.specular_ior.value_real;
    }
    if (m->pbr.transmission_factor.has_value) {
        out.transmission = m->pbr.transmission_factor.value_real;
    }
    if (m->pbr.emission_color.has_value && m->pbr.emission_color.value_components >= 3) {
        out.emission[0] = m->pbr.emission_color.value_vec3.x;
        out.emission[1] = m->pbr.emission_color.value_vec3.y;
        out.emission[2] = m->pbr.emission_color.value_vec3.z;
        out.emissionStrength = m->pbr.emission_factor.has_value
                                   ? m->pbr.emission_factor.value_real : 1.0;
    }
    if (baseColor.texture) {
        out.diffuseMapPath = resolveUfbxTextureArc(baseColor.texture, dir);
    }
    return out;
}

// One mesh piece: a ufbx mesh restricted to one material group, in the
// node-local (geometry->node) frame. Returns false when the group is empty.
bool
buildMeshPiece(const ufbx_node* node, const ufbx_mesh* mesh, uint32_t matIdx,
               const std::vector<uint32_t>& faces, GeoArchiveMeshData& out)
{
    if (faces.empty()) return false;

    // geometry->node: bakes the FBX "geometric transform" (pivot offset) into
    // the node-local frame the SceneGraph parent chain composes from.
    const bool bake = (node->geometry_to_node.m00 != 1.0f || node->geometry_to_node.m11 != 1.0f ||
                       node->geometry_to_node.m22 != 1.0f || node->geometry_to_node.m03 != 0.0f ||
                       node->geometry_to_node.m13 != 0.0f || node->geometry_to_node.m23 != 0.0f);

    out.mesh = std::make_shared<MeshData>();
    MeshData& m = *out.mesh;

    std::vector<char> touched(mesh->vertex_position.values.count, 0);
    m.vertices.resize((std::size_t)mesh->vertex_position.values.count * 3, 0.0f);
    m.faceIndices.reserve(faces.size() * 4);
    m.faceCounts.reserve(faces.size());
    const ufbx_vertex_vec2* uvStream = NULL;
    if (mesh->uv_sets.count > 0 && mesh->uv_sets.data[0].vertex_uv.exists) {
        uvStream = &mesh->uv_sets.data[0].vertex_uv;
    }
    bool anyUV = false;

    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        const ufbx_face& face = mesh->faces.data[faces[fi]];
        if (face.num_indices < 3) continue;
        m.faceCounts.push_back((int)face.num_indices);
        for (uint32_t k = 0; k < face.num_indices; ++k) {
            const uint32_t ci = face.index_begin + k;
            const uint32_t pi = mesh->vertex_position.indices.data[ci];
            if (!touched[pi]) {
                touched[pi] = 1;
                const ufbx_vec3 pos = mesh->vertex_position.values.data[pi];
                const ufbx_vec3 lp = bake ? ufbx_transform_position(&node->geometry_to_node, pos) : pos;
                m.vertices[(std::size_t)pi * 3] = lp.x;
                m.vertices[(std::size_t)pi * 3 + 1] = lp.y;
                m.vertices[(std::size_t)pi * 3 + 2] = lp.z;
            }
            m.faceIndices.push_back((int)pi);
            if (uvStream) {
                const ufbx_vec2 uv = uvStream->values.data[uvStream->indices.data[ci]];
                m.uvs.push_back(uv.x);
                m.uvs.push_back(uv.y);
                anyUV = true;
            }
        }
    }
    if (m.faceCounts.empty()) return false;

    // Trim untouched tail.
    std::size_t used = 0;
    for (std::size_t v = 0; v < touched.size(); ++v) {
        if (touched[v]) used = v + 1;
    }
    if (used < touched.size()) m.vertices.resize(used * 3);

    m.numVertices = m.vertices.size() / 3;
    m.numFaces = m.faceCounts.size();
    m.hasUVs = anyUV;
    m.texCoordComponents = anyUV ? 2 : 0;
    geoArchiveMatIdentity(m.transform);
    return true;
}

} // anonymous namespace

// --- parsing ---------------------------------------------------------------

GeoArchiveResult
loadFbxArchiveImport(const std::string& path, void** outScene)
{
    GeoArchiveResult result;
    if (outScene) *outScene = NULL;

    std::string dir;
    {
        const std::size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) dir = path.substr(0, slash);
    }

    ufbx_load_opts opts;
    std::memset(&opts, 0, sizeof(opts));
    opts.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    opts.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    opts.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
    opts.target_unit_meters = 1.0f;
    opts.use_blender_pbr_material = true;
    opts.ignore_missing_external_files = true;
    // Skinning is evaluated per frame at render time (ufbx_evaluate_scene);
    // don't pay for the rest-pose evaluation here.
    opts.evaluate_skinning = false;
    opts.evaluate_caches = false;

    ufbx_error err;
    std::memset(&err, 0, sizeof(err));
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &err);
    if (!scene) {
        result.error = "ufbx failed to load the FBX file: " + arcUfbxToString(err.description);
        return result;
    }
    if (outScene) *outScene = scene;

    // Materials (all of them; mesh entries reference by index).
    result.materials.resize(scene->materials.count);
    for (std::size_t m = 0; m < scene->materials.count; ++m) {
        result.materials[m] = mapUfbxMaterialArc(scene->materials.data[m], dir);
    }

    // Hierarchy walk (pre-order). ufbx_node* -> entry index for skin bindings.
    std::map<const ufbx_node*, int> nodeIndexMap;

    struct Walk {
        GeoArchiveResult* result;
        std::map<const ufbx_node*, int>* nodeIndexMap;
        const std::string* dir;
        const ufbx_scene* scene;

        void run(const ufbx_node* node, int parentEntryIdx, const std::string& parentPath)
        {
            const std::string name = arcUfbxToString(node->name);
            std::string fullPath = parentPath.empty() ? std::string("/") + name
                                                      : parentPath + "/" + name;
            if (name.empty()) fullPath = parentPath + "/node";

            GeoArchiveNodeData nd;
            nd.fullPath = fullPath;
            nd.name = name.empty() ? "node" : name;
            nd.parentIndex = parentEntryIdx;
            nd.isMesh = false;
            geoArchiveMatIdentity(nd.localMatrix);
            // FBX nodes compose TRS: keep the rest TRS so animation
            // evaluation has a base. (ufbx local_transform is T*R*S.)
            nd.hasTrs = true;
            nd.translation[0] = (float)node->local_transform.translation.x;
            nd.translation[1] = (float)node->local_transform.translation.y;
            nd.translation[2] = (float)node->local_transform.translation.z;
            nd.rotation[0] = (float)node->local_transform.rotation.x;
            nd.rotation[1] = (float)node->local_transform.rotation.y;
            nd.rotation[2] = (float)node->local_transform.rotation.z;
            nd.rotation[3] = (float)node->local_transform.rotation.w;
            nd.scale[0] = (float)node->local_transform.scale.x;
            nd.scale[1] = (float)node->local_transform.scale.y;
            nd.scale[2] = (float)node->local_transform.scale.z;
            geoArchiveComposeTRS(nd.translation, nd.rotation, nd.scale, nd.localMatrix);

            const int thisIdx = (int)result->nodes.size();
            (*nodeIndexMap)[node] = thisIdx;
            result->nodes.push_back(nd);

            if (node->mesh) {
                const ufbx_mesh* mesh = node->mesh;
                // Group faces by material (same split the flat loader uses).
                std::map<uint32_t, std::vector<uint32_t>> facesByMat;
                const bool hasPerFaceMats = mesh->face_material.count == mesh->faces.count;
                for (std::size_t f = 0; f < mesh->faces.count; ++f) {
                    const ufbx_face& face = mesh->faces.data[f];
                    if (face.num_indices < 3) continue;
                    const uint32_t matIdx = hasPerFaceMats ? mesh->face_material.data[f] : 0;
                    facesByMat[matIdx].push_back((uint32_t)f);
                }
                const bool singleMat = (facesByMat.size() == 1);
                for (const auto& kv : facesByMat) {
                    GeoArchiveMeshData md;
                    if (buildMeshPiece(node, mesh, kv.first, kv.second, md)) {
                        // mesh->materials is an array of POINTERS into the
                        // scene's material array (order not guaranteed) —
                        // resolve the index by identity scan.
                        int matIndex = -1;
                        if (kv.first < mesh->materials.count && mesh->materials.data[kv.first]) {
                            const ufbx_material* mat = mesh->materials.data[kv.first];
                            for (std::size_t mi = 0; mi < scene->materials.count; ++mi) {
                                if (scene->materials.data[mi] == mat) { matIndex = (int)mi; break; }
                            }
                        }
                        md.materialIndex = matIndex;
                        // Skin binding: ufbx keeps the weights; the frame
                        // evaluator gets skinned positions straight from
                        // ufbx_evaluate_scene(evaluate_skinning=true).
                        md.skinIndex = mesh->skin_deformers.count > 0 ? 0 : -1;

                        GeoArchiveNodeData me;
                        me.fullPath = singleMat
                                          ? fullPath
                                          : (fullPath + "/" +
                                             (kv.first < mesh->materials.count
                                                  ? (arcUfbxToString(mesh->materials.data[kv.first]->name).empty()
                                                         ? "material " + std::to_string(kv.first + 1)
                                                         : arcUfbxToString(mesh->materials.data[kv.first]->name))
                                                  : "material"));
                        me.name = nd.name;
                        me.parentIndex = thisIdx;
                        me.isMesh = true;
                        me.meshIndex = (int)result->meshes.size();
                        geoArchiveMatIdentity(me.localMatrix);
                        result->meshes.push_back(std::move(md));
                        result->nodes.push_back(me);
                    }
                }
            }

            for (std::size_t c = 0; c < node->children.count; ++c) {
                run(node->children.data[c], thisIdx, fullPath);
            }
        }
    } walk;
    walk.result = &result;
    walk.nodeIndexMap = &nodeIndexMap;
    walk.dir = &dir;
    walk.scene = scene;

    for (std::size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node* node = scene->nodes.data[i];
        if (node->is_root) {
            // The ufbx root node carries the load-time normalization
            // (target_axes rotation + target_unit_meters scale, e.g. 0.01
            // compensating cm-authored x100 node scales). The hierarchy walk
            // starts from the root's CHILDREN, so capture the root transform
            // here — ensureFrameState folds it into every root-level local.
            if (result.rootMatrix.empty()) {
                result.rootMatrix.assign(16, 0.0f);
                ufbxMatToColMajor(
                    ufbx_transform_to_matrix(&node->local_transform),
                    result.rootMatrix.data());
            }
            for (std::size_t c = 0; c < node->children.count; ++c) {
                walk.run(node->children.data[c], -1, "");
            }
        }
    }

    // Animation range from the default animation (anims stay empty — FBX
    // channels are evaluated live inside ufbx at frame-evaluation time).
    if (scene->anim) {
        result.animTimeBegin = scene->anim->time_begin;
        result.animTimeEnd = scene->anim->time_end;
    }

    result.ok = true;
    return result;
}

// --- frame evaluation --------------------------------------------------------

void
freeFbxArchiveScene(void* scene)
{
    if (scene) ufbx_free_scene((ufbx_scene*)scene);
}

bool
evaluateFbxArchiveFrame(void* sceneV,
                        const GeoArchiveResult& data,
                        double srcTime,
                        GeoArchiveFrameEval& out,
                        std::string& err)
{
    ufbx_scene* scene = (ufbx_scene*)sceneV;
    if (!scene) {
        out.nodeLocals.assign(data.nodes.size() * 16, 0.0f);
        for (std::size_t i = 0; i < data.nodes.size(); ++i) {
            geoArchiveMatIdentity(&out.nodeLocals[i * 16]);
        }
        out.deformed.assign(data.meshes.size(), MeshDataPtr());
        return true;
    }

    ufbx_evaluate_opts eopts;
    std::memset(&eopts, 0, sizeof(eopts));
    eopts.evaluate_skinning = true;   // CPU skinning for skinned meshes

    ufbx_error e;
    std::memset(&e, 0, sizeof(e));
    ufbx_scene* ev = ufbx_evaluate_scene(scene, scene->anim, srcTime, &eopts, &e);
    if (!ev) {
        err = "ufbx_evaluate_scene failed: " + arcUfbxToString(e.description);
        return false;
    }
    struct FreeGuard {
        ufbx_scene* s;
        ~FreeGuard() { if (s) ufbx_free_scene(s); }
    } guard{ ev };

    // Walk the EVALUATED tree in the same pre-order as the load walk, so
    // entry indices correspond 1:1. Per node: evaluated local transform; per
    // mesh entry: skinned (or raw) positions moved into the node-local frame.
    out.nodeLocals.assign(data.nodes.size() * 16, 0.0f);
    out.deformed.assign(data.meshes.size(), MeshDataPtr());

    if (std::getenv("NATRON_DEBUG_GEOIO")) {
        for (std::size_t i = 0; i < ev->nodes.count; ++i) {
            const ufbx_node* rn = ev->nodes.data[i];
            if (rn->is_root) {
                std::fprintf(stderr,
                    "[GEOIO-ARCH-FBX]   ROOT t=(%.3f,%.3f,%.3f) diag=(%.3f,%.3f,%.3f) rotation=(%.4f,%.4f,%.4f,%.4f)\n",
                    (float)rn->local_transform.translation.x,
                    (float)rn->local_transform.translation.y,
                    (float)rn->local_transform.translation.z,
                    ufbx_transform_to_matrix(&rn->local_transform).m00,
                    ufbx_transform_to_matrix(&rn->local_transform).m11,
                    ufbx_transform_to_matrix(&rn->local_transform).m22,
                    (float)rn->local_transform.rotation.x,
                    (float)rn->local_transform.rotation.y,
                    (float)rn->local_transform.rotation.z,
                    (float)rn->local_transform.rotation.w);
            }
        }
    }

    struct EvalWalk {
        GeoArchiveFrameEval* out;
        const GeoArchiveResult* data;
        std::size_t entryIdx;   // next expected node index (entry counter)
        bool orderBroken;

        void run(const ufbx_node* node)
        {
            if (orderBroken) return;
            const std::size_t i = entryIdx++;
            if (i >= data->nodes.size()) { orderBroken = true; return; }

            // NOTE: the load walk emits the node entry AND THEN its mesh
            // child entries before recursing into children — replicate that
            // exact emission order here. This node visit writes the
            // transform entry's matrix; mesh entries follow immediately.
            const ufbx_matrix lm = ufbx_transform_to_matrix(&node->local_transform);
            ufbxMatToColMajor(lm, &out->nodeLocals[i * 16]);

            if (std::getenv("NATRON_DEBUG_GEOIO")) {
                std::fprintf(stderr,
                    "[GEOIO-ARCH-FBX]   node %d '%s' t=(%.3f,%.3f,%.3f) diag=(%.3f,%.3f,%.3f) rest_diag=(%.3f,%.3f,%.3f)\n",
                    (int)i, arcUfbxToString(node->name).c_str(),
                    (float)node->local_transform.translation.x,
                    (float)node->local_transform.translation.y,
                    (float)node->local_transform.translation.z,
                    lm.m00, lm.m11, lm.m22,
                    data->nodes[i].localMatrix[0], data->nodes[i].localMatrix[5],
                    data->nodes[i].localMatrix[10]);
            }

            // Mesh child entries for this node: emitted by the LOAD walk
            // between this node's entry and its first child's entry. Reproduce
            // the same material grouping to locate the entry slots.
            if (node->mesh) {
                const ufbx_mesh* mesh = node->mesh;
                std::map<uint32_t, std::vector<uint32_t>> facesByMat;
                const bool hasPerFaceMats = mesh->face_material.count == mesh->faces.count;
                for (std::size_t f = 0; f < mesh->faces.count; ++f) {
                    const ufbx_face& face = mesh->faces.data[f];
                    if (face.num_indices < 3) continue;
                    const uint32_t matIdx = hasPerFaceMats ? mesh->face_material.data[f] : 0;
                    facesByMat[matIdx].push_back((uint32_t)f);
                }
                for (const auto& kv : facesByMat) {
                    const std::size_t i2 = entryIdx++;
                    if (i2 >= data->nodes.size() || !data->nodes[i2].isMesh ||
                        data->nodes[i2].meshIndex >= (int)out->deformed.size()) {
                        orderBroken = true; return;
                    }
                    // Mesh child entries carry no transform of their own —
                    // the owning node's chain applies above them. Without
                    // this they'd stay ZERO (the assign() init) and the
                    // composed world matrix would collapse the object.
                    geoArchiveMatIdentity(&out->nodeLocals[i2 * 16]);
                    const int meshIdx = data->nodes[i2].meshIndex;

                    // Evaluated geometry: skinned positions when the mesh has
                    // a skin deformer, raw positions otherwise — both in the
                    // geometry space; move into the node-local frame.
                    const ufbx_vertex_vec3* srcPos =
                        (mesh->skinned_position.exists ? &mesh->skinned_position
                                                       : &mesh->vertex_position);
                    const bool bake = (node->geometry_to_node.m00 != 1.0f ||
                                       node->geometry_to_node.m11 != 1.0f ||
                                       node->geometry_to_node.m22 != 1.0f ||
                                       node->geometry_to_node.m03 != 0.0f ||
                                       node->geometry_to_node.m13 != 0.0f ||
                                       node->geometry_to_node.m23 != 0.0f);

                    MeshDataPtr def(new MeshData(*data->meshes[meshIdx].mesh));
                    const std::size_t nv = def->numVertices;
                    const std::size_t srcCount = srcPos->values.count;
                    for (std::size_t v = 0; v < nv; ++v) {
                        if (v >= srcCount) break;
                        ufbx_vec3 p = srcPos->values.data[v];
                        if (bake) p = ufbx_transform_position(&node->geometry_to_node, p);
                        def->vertices[v * 3] = (float)p.x;
                        def->vertices[v * 3 + 1] = (float)p.y;
                        def->vertices[v * 3 + 2] = (float)p.z;
                    }
                    geoArchiveMatIdentity(def->transform);
                    out->deformed[meshIdx] = def;
                }
            }

            for (std::size_t c = 0; c < node->children.count; ++c) {
                run(node->children.data[c]);
            }
        }
    } evw;
    evw.out = &out;
    evw.data = &data;
    evw.entryIdx = 0;
    evw.orderBroken = false;

    for (std::size_t i = 0; i < ev->nodes.count; ++i) {
        const ufbx_node* node = ev->nodes.data[i];
        if (node->is_root) {
            for (std::size_t c = 0; c < node->children.count; ++c) {
                evw.run(node->children.data[c]);
            }
        }
    }

    if (evw.orderBroken) {
        // Structure changed between parse and evaluation (shouldn't happen) —
        // serve the rest pose rather than a half-evaluated scene.
        err = "FBX structure changed between parse and evaluation";
        return false;
    }

    // Fill any mesh entries the walk never reached (skipped/empty) with rest.
    for (std::size_t m = 0; m < out.deformed.size(); ++m) {
        if (!out.deformed[m]) out.deformed[m] = data.meshes[m].mesh;
    }

    // Diagnostics gate: entry walk integrity + first mesh's deformed bbox.
    if (std::getenv("NATRON_DEBUG_GEOIO")) {
        std::fprintf(stderr, "[GEOIO-ARCH-FBX] t=%.3f entries=%d walked=%d broken=%d filled=%d\n",
                     srcTime, (int)data.nodes.size(), (int)evw.entryIdx,
                     (int)evw.orderBroken,
                     (int)std::count_if(out.deformed.begin(), out.deformed.end(),
                                        [](const MeshDataPtr& p) { return (bool)p; }));
        if (!out.deformed.empty() && out.deformed[0]) {
            float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            const std::vector<float>& vtx = out.deformed[0]->vertices;
            for (std::size_t v = 0; v + 2 < vtx.size(); v += 3) {
                for (int c = 0; c < 3; ++c) {
                    mn[c] = std::min(mn[c], vtx[v + c]);
                    mx[c] = std::max(mx[c], vtx[v + c]);
                }
            }
            std::fprintf(stderr, "[GEOIO-ARCH-FBX]   mesh 0 bbox=(%.2f..%.2f, %.2f..%.2f, %.2f..%.2f)\n",
                         mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
        }
    }
    return true;
}

NATRON_NAMESPACE_EXIT
