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

// glTF 2.0 (.gltf / .glb) ARCHIVE import for ReadGeoArchive, backed by the
// vendored cgltf parser (libs/cgltf, MIT).
//
// Shared archive parser for both ReadGeo and ReadGeoArchive (ReadGeoImport.h):
// the file's OWN hierarchy — node entries + per-primitive mesh entries in
// the node-local frame, skins (joint node indices + inverse bind matrices)
// and animation channels as plain copied data. Animation evaluation happens
// later, per frame, in evaluateGltfArchiveFrame() — scrubbing never
// re-parses the file.
//
// Indexing note: cgltf stores nodes/skins/materials in flat arrays, so any
// cgltf_node* maps to its flat index by pointer subtraction. Skins are a
// NODE property in glTF (node->skin); a node's primitives deform with that
// node's skin. Joint indices are stored flat and remapped to entry indices
// after the hierarchy walk.

#include "ReadGeoImport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cgltf.h"

NATRON_NAMESPACE_ENTER

namespace {

// --- accessor helpers ------------------------------------------------------

int
accessorNumComponents(const cgltf_accessor* acc)
{
    switch (acc->type) {
        case cgltf_type_scalar: return 1;
        case cgltf_type_vec2: return 2;
        case cgltf_type_vec3: return 3;
        case cgltf_type_vec4: return 4;
        default: return 0;
    }
}

// --- embedded-image extraction ---------------------------------------------

bool
decodeBase64(const std::string& in, std::vector<unsigned char>& out)
{
    static const int table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0x00-0x0F
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0x10-0x1F
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,-1,63, // 0x20-0x2F  '+' '/'
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1, // 0x30-0x3F  '0'-'9' '='
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // 0x40-0x4F  'A'-'O'
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1, // 0x50-0x5F  'P'-'Z'
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // 0x60-0x6F  'a'-'o'
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, // 0x70-0x7F  'p'-'z'
    };
    out.clear();
    out.reserve(in.size() / 4 * 3);
    unsigned int buf = 0;
    int bits = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const int v = table[(unsigned char)in[i]];
        if (v < 0) {
            if (in[i] == '=') break; // padding
            continue; // tolerate whitespace
        }
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((buf >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

// Resolve a cgltf image (file URI, data URI, or buffer view) to a usable
// texture file path. External URIs resolve relative to the .gltf directory;
// embedded content is written to a content-hashed temp file. Ported from the
// retired flat loader (ReadGeoGltf.cpp) so embedded GLB textures reach both
// node façades.
std::string
resolveCgltfImage(const cgltf_image* image, const std::string& dir)
{
    if (!image) return std::string();

    if (image->buffer_view) {
        const unsigned char* bytes = cgltf_buffer_view_data(image->buffer_view);
        if (bytes && image->buffer_view->size > 0) {
            return importWriteEmbeddedTexture(bytes, image->buffer_view->size, "gltf_tex");
        }
        return std::string();
    }

    if (!image->uri) return std::string();
    const std::string uri = image->uri;

    if (uri.rfind("data:", 0) == 0) {
        // data:image/png;base64,<payload>
        const std::size_t b64 = uri.find("base64,");
        if (b64 != std::string::npos) {
            std::vector<unsigned char> decoded;
            if (decodeBase64(uri.substr(b64 + 7), decoded)) {
                return importWriteEmbeddedTexture(decoded.data(), decoded.size(), "gltf_tex");
            }
        }
        return std::string();
    }

    // External file URI — resolve relative to the .gltf file's directory.
    // (Backslashes are tolerated; URL percent-encoding is not handled.)
    if (uri.empty()) return std::string();
    if (dir.empty()) return uri;
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') {
        return dir + uri;
    }
    return dir + "/" + uri;
}

// --- animation channel sampling ---------------------------------------------

// Sample an animation sampler at source time `t` (seconds). Writes nComp
// floats. Keys are ascending; times outside the key range hold the end keys.
void
sampleAnimSampler(const GeoArchiveAnimSamplerData& smp, double t, float* outVal)
{
    const std::vector<double>& times = smp.times;
    const int nc = smp.nComp;
    if (nc <= 0 || times.empty() || smp.values.empty()) {
        for (int k = 0; k < nc; ++k) outVal[k] = 0.0f;
        return;
    }

    if (t <= times.front()) {
        for (int c = 0; c < nc; ++c) outVal[c] = smp.values[c];
        return;
    }
    if (t >= times.back()) {
        const std::size_t last = (times.size() - 1) * (std::size_t)nc;
        for (int c = 0; c < nc; ++c) outVal[c] = smp.values[last + c];
        return;
    }

    // Bracket: last key with time <= t.
    std::size_t k0 = 0;
    for (std::size_t i = 0; i + 1 < times.size(); ++i) {
        if (times[i] <= t && t < times[i + 1]) { k0 = i; break; }
    }
    const std::size_t k1 = k0 + 1;
    const double span = times[k1] - times[k0];
    const double alpha = (span > 1e-12) ? (t - times[k0]) / span : 0.0;

    // Per keyframe storage stride: 1 value, or [inTangent, value, outTangent]
    // for cubic spline (glTF 2.0 spec layout).
    const std::size_t per = (smp.interpolation == GEO_ARCHIVE_INTERP_CUBICSPLINE) ? 3 : 1;
    const std::size_t b0 = k0 * (std::size_t)nc * per;
    const std::size_t stride = (std::size_t)nc * per;

    if (smp.interpolation == GEO_ARCHIVE_INTERP_STEP) {
        for (int c = 0; c < nc; ++c) outVal[c] = smp.values[b0 + c];
        return;
    }

    if (smp.interpolation == GEO_ARCHIVE_INTERP_CUBICSPLINE) {
        // Hermite: value(k0), dt*outTangent(k0), value(k1), dt*inTangent(k1).
        const double tt = alpha;
        const double h00 = 2.0*tt*tt*tt - 3.0*tt*tt + 1.0;
        const double h10 = tt*tt*tt - 2.0*tt*tt + tt;
        const double h01 = -2.0*tt*tt*tt + 3.0*tt*tt;
        const double h11 = tt*tt*tt - tt*tt;
        const double dt = span;
        for (int c = 0; c < nc; ++c) {
            const float p0 = smp.values[b0 + (std::size_t)nc + c];            // value @ k0
            const float m0 = smp.values[b0 + (std::size_t)2 * nc + c];        // outTangent @ k0
            const float p1 = smp.values[b0 + stride + (std::size_t)nc + c];   // value @ k1
            const float m1 = smp.values[b0 + stride + c];                     // inTangent @ k1
            outVal[c] = (float)(h00 * p0 + h10 * dt * m0 + h01 * p1 + h11 * dt * m1);
        }
        return;
    }

    // LINEAR (also the fallback for anything unrecognized).
    for (int c = 0; c < nc; ++c) {
        const float v0 = smp.values[b0 + c];
        const float v1 = smp.values[b0 + stride + c];
        outVal[c] = v0 + (v1 - v0) * (float)alpha;
    }
}

} // anonymous namespace

// --- parsing ---------------------------------------------------------------

GeoArchiveResult
loadGltfArchiveImport(const std::string& path)
{
    GeoArchiveResult result;

    std::string dir;
    {
        const std::size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) dir = path.substr(0, slash);
    }

    cgltf_options options;
    std::memset(&options, 0, sizeof(options));
    cgltf_data* data = NULL;
    cgltf_result res = cgltf_parse_file(&options, path.c_str(), &data);
    if (res != cgltf_result_success) {
        result.error = "cgltf failed to parse the file (not a valid glTF/GLB?)";
        return result;
    }
    struct FreeGuard {
        cgltf_data* d;
        ~FreeGuard() { if (d) cgltf_free(d); }
    } guard{ data };

    res = cgltf_load_buffers(&options, data, path.c_str());
    if (res != cgltf_result_success) {
        result.error = "glTF buffer data could not be loaded (missing .bin or corrupt GLB)";
        return result;
    }

    const cgltf_scene* scene = data->scene ? data->scene
                                           : (data->scenes_count ? &data->scenes[0] : NULL);
    if (!scene) {
        result.error = "glTF file has no scene";
        return result;
    }

    // Materials.
    result.materials.resize(data->materials_count);
    for (cgltf_size m = 0; m < data->materials_count; ++m) {
        GeoImportMaterial& mat = result.materials[m];
        const cgltf_material* cm = &data->materials[m];
        mat.present = true;
        const cgltf_pbr_metallic_roughness& pbr = cm->pbr_metallic_roughness;
        mat.baseColor[0] = pbr.base_color_factor[0];
        mat.baseColor[1] = pbr.base_color_factor[1];
        mat.baseColor[2] = pbr.base_color_factor[2];
        mat.metallic = pbr.metallic_factor;
        mat.roughness = pbr.roughness_factor;
        double strength = 1.0;
        if (cm->has_emissive_strength) strength = cm->emissive_strength.emissive_strength;
        mat.emission[0] = cm->emissive_factor[0];
        mat.emission[1] = cm->emissive_factor[1];
        mat.emission[2] = cm->emissive_factor[2];
        mat.emissionStrength =
            (cm->emissive_factor[0] == 0.0 && cm->emissive_factor[1] == 0.0 &&
             cm->emissive_factor[2] == 0.0) ? 0.0 : strength;
        if (cm->has_transmission) mat.transmission = cm->transmission.transmission_factor;
        if (cm->has_ior) mat.ior = cm->ior.ior;
        if (cm->has_specular) mat.specular = cm->specular.specular_factor;
        // Diffuse texture: external file URI relative to the .gltf directory,
        // or embedded content (data URI / buffer view) extracted to a
        // content-hashed temp file.
        if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
            mat.diffuseMapPath = resolveCgltfImage(pbr.base_color_texture.texture->image, dir);
        }
    }

    // Skins — flat joint node indices (remapped to entry indices after the
    // walk) + inverse bind matrices.
    std::vector<std::vector<int>> skinJointsFlat(data->skins_count);
    result.skinJoints.resize(data->skins_count);
    result.skinIBMs.resize(data->skins_count);
    for (cgltf_size s = 0; s < data->skins_count; ++s) {
        const cgltf_skin& skin = data->skins[s];
        skinJointsFlat[s].resize(skin.joints_count);
        for (cgltf_size j = 0; j < skin.joints_count; ++j) {
            skinJointsFlat[s][j] = (int)(skin.joints[j] ? (skin.joints[j] - data->nodes) : -1);
        }
        if (skin.inverse_bind_matrices) {
            result.skinIBMs[s].resize(skin.joints_count * 16);
            for (cgltf_size j = 0; j < skin.joints_count; ++j) {
                cgltf_accessor_read_float(skin.inverse_bind_matrices, j,
                                          &result.skinIBMs[s][j * 16], 16);
            }
        } else {
            // No IBMs authored — identity binds.
            result.skinIBMs[s].assign(skin.joints_count * 16, 0.0f);
            for (cgltf_size j = 0; j < skin.joints_count; ++j) {
                geoArchiveMatIdentity(&result.skinIBMs[s][j * 16]);
            }
        }
    }

    // Recursive walk: one transform entry per node, one mesh child entry per
    // triangle primitive (parents before children — walk order guarantees
    // this). Mirrors the Alembic archive walker's locator+mesh pair shape.
    struct Walk {
        std::vector<GeoArchiveNodeData>* nodes;
        std::vector<GeoArchiveMeshData>* meshes;
        std::vector<int>* flatToEntry;

        void run(const cgltf_node* node, int parentEntryIdx, const std::string& parentPath,
                 cgltf_data* data)
        {
            const std::string name = node->name ? node->name : "";
            std::string fullPath = parentPath.empty() ? std::string("/") + name
                                                      : parentPath + "/" + name;
            if (name.empty()) fullPath = parentPath + "/node";

            GeoArchiveNodeData nd;
            nd.fullPath = fullPath;
            nd.name = name.empty() ? "node" : name;
            nd.parentIndex = parentEntryIdx;
            nd.isMesh = false;
            geoArchiveMatIdentity(nd.localMatrix);

            if (node->has_matrix) {
                // Authored as a raw matrix — column-major in glTF, used as-is.
                // TRS animation channels can't address this layout in stage 2;
                // the node stays static.
                std::memcpy(nd.localMatrix, node->matrix, 16 * sizeof(float));
            } else {
                nd.hasTrs = true;
                std::memcpy(nd.translation, node->translation, 3 * sizeof(float));
                std::memcpy(nd.rotation, node->rotation, 4 * sizeof(float));
                std::memcpy(nd.scale, node->scale, 3 * sizeof(float));
                geoArchiveComposeTRS(nd.translation, nd.rotation, nd.scale, nd.localMatrix);
            }

            const int thisIdx = (int)nodes->size();
            (*flatToEntry)[node - data->nodes] = thisIdx;
            nodes->push_back(nd);

            const int skinIdx = node->skin ? (int)(node->skin - data->skins) : -1;

            if (node->mesh) {
                for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                    const cgltf_primitive& prim = node->mesh->primitives[p];
                    if (prim.type != cgltf_primitive_type_triangles) continue;

                    const cgltf_accessor* posAcc = NULL;
                    const cgltf_accessor* uvAcc = NULL;
                    const cgltf_accessor* jAcc = NULL;
                    const cgltf_accessor* wAcc = NULL;
                    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                        const cgltf_attribute& att = prim.attributes[a];
                        if (!std::strcmp(att.name, "POSITION") && !posAcc) posAcc = att.data;
                        else if (!std::strcmp(att.name, "TEXCOORD_0") && !uvAcc) uvAcc = att.data;
                        else if (!std::strcmp(att.name, "JOINTS_0") && !jAcc) jAcc = att.data;
                        else if (!std::strcmp(att.name, "WEIGHTS_0") && !wAcc) wAcc = att.data;
                    }
                    if (!posAcc || posAcc->count == 0) continue;

                    GeoArchiveMeshData md;
                    md.mesh = std::make_shared<MeshData>();
                    MeshData& mesh = *md.mesh;

                    // Rest positions (accessor read handles component types,
                    // normalizations and sparse accessors safely).
                    const cgltf_size numVerts = posAcc->count;
                    mesh.vertices.resize((std::size_t)numVerts * 3);
                    for (cgltf_size i = 0; i < numVerts; ++i) {
                        cgltf_accessor_read_float(posAcc, i, &mesh.vertices[i * 3], 3);
                    }
                    mesh.numVertices = (std::size_t)numVerts;

                    // Indices (implicit sequential when unindexed).
                    const cgltf_size indexCount = prim.indices ? prim.indices->count : numVerts;
                    std::vector<unsigned int> indices((std::size_t)indexCount);
                    if (prim.indices) {
                        for (cgltf_size i = 0; i < indexCount; ++i) {
                            cgltf_accessor_read_uint(prim.indices, i, &indices[i], 1);
                        }
                    } else {
                        for (cgltf_size i = 0; i < indexCount; ++i) indices[i] = (unsigned int)i;
                    }

                    // Faces as triangles; per-corner UVs with the glTF V-flip
                    // (glTF UV origin is top-left, Natron's bottom-left).
                    std::vector<float> uvPerVert;
                    if (uvAcc) {
                        uvPerVert.resize((std::size_t)uvAcc->count * 2);
                        for (cgltf_size i = 0; i < uvAcc->count; ++i) {
                            float uv[2];
                            cgltf_accessor_read_float(uvAcc, i, uv, 2);
                            uvPerVert[(std::size_t)i * 2] = uv[0];
                            uvPerVert[(std::size_t)i * 2 + 1] = 1.0f - uv[1];
                        }
                    }
                    const std::size_t numFaces = indexCount / 3;
                    mesh.faceCounts.reserve(numFaces);
                    mesh.faceIndices.reserve(indexCount);
                    if (uvAcc) mesh.uvs.reserve(indexCount * 2);
                    for (std::size_t f = 0; f < numFaces; ++f) {
                        const unsigned int i0 = indices[f * 3];
                        const unsigned int i1 = indices[f * 3 + 1];
                        const unsigned int i2 = indices[f * 3 + 2];
                        if (i0 >= numVerts || i1 >= numVerts || i2 >= numVerts) continue;
                        mesh.faceCounts.push_back(3);
                        mesh.faceIndices.push_back((int)i0);
                        mesh.faceIndices.push_back((int)i1);
                        mesh.faceIndices.push_back((int)i2);
                        if (uvAcc) {
                            for (int k = 0; k < 3; ++k) {
                                const unsigned int vi = indices[f * 3 + k];
                                mesh.uvs.push_back(uvPerVert[(std::size_t)vi * 2]);
                                mesh.uvs.push_back(uvPerVert[(std::size_t)vi * 2 + 1]);
                            }
                        }
                    }
                    if (mesh.faceCounts.empty()) continue;
                    mesh.numFaces = mesh.faceCounts.size();
                    mesh.hasUVs = (uvAcc != NULL);
                    mesh.texCoordComponents = mesh.hasUVs ? 2 : 0;
                    geoArchiveMatIdentity(mesh.transform);

                    md.materialIndex = prim.material ? (int)(prim.material - data->materials) : -1;
                    md.skinIndex = (node->skin && jAcc && wAcc &&
                                    jAcc->count == numVerts && wAcc->count == numVerts)
                                       ? skinIdx : -1;
                    if (md.skinIndex >= 0) {
                        md.joints.resize((std::size_t)numVerts * 4);
                        md.weights.resize((std::size_t)numVerts * 4);
                        for (cgltf_size i = 0; i < numVerts; ++i) {
                            // JOINTS_0 is a VEC4 of joint indices into the
                            // skin's joints array — read the WHOLE element
                            // (index = element, element_size = 4), not
                            // component-by-component (that would index past
                            // the accessor and silently read zeros).
                            cgltf_uint j4[4] = { 0, 0, 0, 0 };
                            cgltf_accessor_read_uint(jAcc, i, j4, 4);
                            for (int c = 0; c < 4; ++c) {
                                md.joints[(std::size_t)i * 4 + c] = (unsigned int)j4[c];
                            }
                            cgltf_accessor_read_float(wAcc, i, &md.weights[(std::size_t)i * 4], 4);
                        }
                    }

                    // Morph targets (shape keys): per-primitive POSITION
                    // deltas, dense (indices empty). Normals-only targets are
                    // kept as empty placeholders so weight indexing stays
                    // aligned. Rest weights: the node's per-instance weights
                    // win over the mesh's defaults (glTF spec).
                    const cgltf_mesh* cmesh = node->mesh;
                    if (prim.targets_count > 0) {
                        std::shared_ptr<GeoMorphSet> morphs(new GeoMorphSet());
                        morphs->targets.resize(prim.targets_count);
                        for (cgltf_size t = 0; t < prim.targets_count; ++t) {
                            GeoMorphTarget& tgt = morphs->targets[t];
                            if (cmesh->target_names && t < cmesh->target_names_count &&
                                cmesh->target_names[t]) {
                                tgt.name = cmesh->target_names[t];
                            } else {
                                tgt.name = "target " + std::to_string(t + 1);
                            }
                            const cgltf_accessor* mPosAcc = NULL;
                            const cgltf_morph_target& mt = prim.targets[t];
                            for (cgltf_size a = 0; a < mt.attributes_count; ++a) {
                                if (!std::strcmp(mt.attributes[a].name, "POSITION") &&
                                    mt.attributes[a].data) {
                                    mPosAcc = mt.attributes[a].data;
                                    break;
                                }
                            }
                            if (mPosAcc && mPosAcc->count == numVerts) {
                                tgt.deltas.resize((std::size_t)numVerts * 3);
                                for (cgltf_size i = 0; i < numVerts; ++i) {
                                    cgltf_accessor_read_float(mPosAcc, i, &tgt.deltas[i * 3], 3);
                                }
                            }
                        }
                        morphs->restWeights.assign(prim.targets_count, 0.0f);
                        if (node->weights && node->weights_count == prim.targets_count) {
                            for (cgltf_size t = 0; t < prim.targets_count; ++t) {
                                morphs->restWeights[t] = node->weights[t];
                            }
                        } else if (cmesh->weights && cmesh->weights_count == prim.targets_count) {
                            for (cgltf_size t = 0; t < prim.targets_count; ++t) {
                                morphs->restWeights[t] = cmesh->weights[t];
                            }
                        }
                        md.morphs = morphs;
                        md.morphNodeIndex = thisIdx;
                    }

                    // Mesh child entry: "name" for the first primitive,
                    // "name[2]" for subsequent ones (same material-part split
                    // shape the Alembic locator+mesh pairs use).
                    // glTF spec: the transform of a SKINNED mesh node is
                    // ignored for skinning — the deformed vertices are in the
                    // scene's global space. So a skinned mesh entry attaches
                    // to the archive root (parentIndex -1, identity local);
                    // the user transform on the archive still applies.
                    GeoArchiveNodeData me;
                    me.fullPath = fullPath + (p > 0 ? ("[" + std::to_string(p + 1) + "]") : "");
                    me.name = nd.name;
                    me.parentIndex = md.skinIndex >= 0 ? -1 : thisIdx;
                    me.isMesh = true;
                    me.meshIndex = (int)meshes->size();
                    geoArchiveMatIdentity(me.localMatrix);
                    meshes->push_back(std::move(md));
                    nodes->push_back(me);
                }
            }

            for (cgltf_size c = 0; c < node->children_count; ++c) {
                run(node->children[c], thisIdx, fullPath, data);
            }
        }
    } walk;
    std::vector<int> flatToEntry((std::size_t)data->nodes_count, -1);
    walk.nodes = &result.nodes;
    walk.meshes = &result.meshes;
    walk.flatToEntry = &flatToEntry;
    for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
        walk.run(scene->nodes[i], -1, "", data);
    }

    // Remap skins: flat joint node index → entry index (joints may not be
    // reachable from the default scene — those become -1 and don't deform).
    for (std::size_t s = 0; s < result.skinJoints.size(); ++s) {
        result.skinJoints[s].resize(skinJointsFlat[s].size());
        for (std::size_t j = 0; j < skinJointsFlat[s].size(); ++j) {
            const int flat = skinJointsFlat[s][j];
            result.skinJoints[s][j] = (flat >= 0 && flat < (int)flatToEntry.size())
                                          ? flatToEntry[(std::size_t)flat] : -1;
        }
    }

    // Animations: copy samplers/channels of EVERY animation; node channel
    // bindings reference anims[0] (typical exports carry one).
    result.anims.resize(data->animations_count);
    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& anim = data->animations[a];
        GeoArchiveAnimData& ad = result.anims[a];
        ad.name = anim.name ? anim.name : "";
        ad.samplers.resize(anim.samplers_count);
        for (cgltf_size s = 0; s < anim.samplers_count; ++s) {
            const cgltf_animation_sampler& sm = anim.samplers[s];
            GeoArchiveAnimSamplerData& sd = ad.samplers[s];
            sd.interpolation = (sm.interpolation == cgltf_interpolation_type_step)
                                   ? GEO_ARCHIVE_INTERP_STEP
                               : (sm.interpolation == cgltf_interpolation_type_cubic_spline)
                                   ? GEO_ARCHIVE_INTERP_CUBICSPLINE
                                   : GEO_ARCHIVE_INTERP_LINEAR;
            if (sm.input) {
                sd.times.resize(sm.input->count);
                for (cgltf_size i = 0; i < sm.input->count; ++i) {
                    cgltf_float t = 0.0f;
                    cgltf_accessor_read_float(sm.input, i, &t, 1);
                    sd.times[i] = (double)t;
                }
            }
            if (sm.output) {
                sd.nComp = accessorNumComponents(sm.output);
                sd.values.resize(sm.output->count * (cgltf_size)sd.nComp);
                for (cgltf_size i = 0; i < sm.output->count; ++i) {
                    cgltf_accessor_read_float(sm.output, i,
                                              &sd.values[i * (cgltf_size)sd.nComp], sd.nComp);
                }
            }
        }
        ad.channels.resize(anim.channels_count);
        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            GeoArchiveAnimChannelData& cd = ad.channels[c];
            if (ch.target_node && ch.target_node >= data->nodes &&
                ch.target_node < data->nodes + data->nodes_count) {
                const int flat = (int)(ch.target_node - data->nodes);
                cd.nodeIndex = (flat < (int)flatToEntry.size()) ? flatToEntry[(std::size_t)flat] : -1;
            }
            switch (ch.target_path) {
                case cgltf_animation_path_type_translation: cd.path = GEO_ARCHIVE_CHAN_TRANSLATION; break;
                case cgltf_animation_path_type_rotation: cd.path = GEO_ARCHIVE_CHAN_ROTATION; break;
                case cgltf_animation_path_type_scale: cd.path = GEO_ARCHIVE_CHAN_SCALE; break;
                case cgltf_animation_path_type_weights: cd.path = GEO_ARCHIVE_CHAN_WEIGHTS; break;
                default: cd.path = 0; break;
            }
            cd.sampler = ch.sampler ? (int)(ch.sampler - anim.samplers) : -1;
        }
    }

    // Weights samplers (morph target animation) were read with the output
    // accessor's flat component count (SCALAR → nComp 1); the real per-key
    // stride is the TARGET COUNT of the target node's mesh. Fix nComp so
    // sampleAnimSampler interpolates all targets of a key together. The
    // glTF validator (cgltf) already ties the output count to
    // keys * targets_count; a sampler that doesn't fit stays nComp 1 and the
    // binding below rejects it.
    if (!result.anims.empty()) {
        GeoArchiveAnimData& anim = result.anims.front();
        std::vector<char> fixed(anim.samplers.size(), 0);
        for (const GeoArchiveAnimChannelData& ch : anim.channels) {
            if (ch.path != GEO_ARCHIVE_CHAN_WEIGHTS || ch.sampler < 0 ||
                ch.sampler >= (int)anim.samplers.size() || ch.nodeIndex < 0 ||
                ch.nodeIndex >= (int)result.nodes.size()) {
                continue;
            }
            // Recover the target count from the owning node's mesh pieces
            // (morphNodeIndex was stored at parse — works for skinned mesh
            // entries too, whose parent is the archive root).
            int numTargets = 0;
            for (const GeoArchiveMeshData& md : result.meshes) {
                if (md.morphs && md.morphNodeIndex == ch.nodeIndex) {
                    numTargets = (int)md.morphs->targets.size();
                    break;
                }
            }
            if (numTargets <= 0) continue;
            GeoArchiveAnimSamplerData& sd = anim.samplers[ch.sampler];
            if (fixed[ch.sampler]) continue;
            if (sd.nComp == 1 && !sd.times.empty() &&
                sd.values.size() == sd.times.size() * (std::size_t)numTargets) {
                sd.nComp = numTargets;
                fixed[ch.sampler] = 1;
            }
        }
    }

    // Bind anims[0] channels onto nodes: sampler index per channel path.
    if (!result.anims.empty()) {
        const GeoArchiveAnimData& anim = result.anims.front();
        double tBegin = 0.0, tEnd = 0.0;
        for (const GeoArchiveAnimSamplerData& smp : anim.samplers) {
            if (!smp.times.empty()) {
                tBegin = std::min(tBegin, smp.times.front());
                tEnd = std::max(tEnd, smp.times.back());
            }
        }
        result.animTimeBegin = tBegin;
        result.animTimeEnd = tEnd;

        for (const GeoArchiveAnimChannelData& ch : anim.channels) {
            if (ch.nodeIndex < 0 || ch.nodeIndex >= (int)result.nodes.size()) continue;
            GeoArchiveNodeData& nd = result.nodes[ch.nodeIndex];
            if (ch.path == GEO_ARCHIVE_CHAN_TRANSLATION) nd.chanTr = ch.sampler;
            else if (ch.path == GEO_ARCHIVE_CHAN_ROTATION) nd.chanRot = ch.sampler;
            else if (ch.path == GEO_ARCHIVE_CHAN_SCALE) nd.chanSc = ch.sampler;
            else if (ch.path == GEO_ARCHIVE_CHAN_WEIGHTS) nd.chanWeights = ch.sampler;
        }
        if (std::getenv("NATRON_DEBUG_GEOIO")) {
            for (std::size_t i = 0; i < result.nodes.size(); ++i) {
                const GeoArchiveNodeData& nd = result.nodes[i];
                if (nd.chanTr >= 0 || nd.chanRot >= 0 || nd.chanSc >= 0 ||
                    nd.chanWeights >= 0) {
                    std::fprintf(stderr, "[GEOIO-ARCH] bind node %d '%s': tr=%d rot=%d sc=%d w=%d\n",
                                 (int)i, nd.name.c_str(), nd.chanTr, nd.chanRot, nd.chanSc,
                                 nd.chanWeights);
                }
            }
        }
    }

    result.ok = true;
    return result;
}

// --- frame evaluation --------------------------------------------------------

void
evaluateGltfArchiveFrame(const GeoArchiveResult& data,
                         double srcTime,
                         GeoArchiveFrameEval& out)
{
    const std::size_t nNodes = data.nodes.size();

    // Pass 1: per-node local matrices (rest TRS overridden by channels), and
    // world matrices (skin joints need them; parents come before children).
    out.nodeLocals.resize(nNodes * 16);
    std::vector<float> world(nNodes * 16);
    for (std::size_t i = 0; i < nNodes; ++i) {
        const GeoArchiveNodeData& nd = data.nodes[i];
        float* local = &out.nodeLocals[i * 16];

        if (nd.hasTrs) {
            float t[3], q[4], s[3];
            std::memcpy(t, nd.translation, 3 * sizeof(float));
            std::memcpy(q, nd.rotation, 4 * sizeof(float));
            std::memcpy(s, nd.scale, 3 * sizeof(float));
            float val[4];
            if (nd.chanTr >= 0) {
                sampleAnimSampler(data.anims.front().samplers[nd.chanTr], srcTime, val);
                std::memcpy(t, val, 3 * sizeof(float));
            }
            if (nd.chanRot >= 0) {
                sampleAnimSampler(data.anims.front().samplers[nd.chanRot], srcTime, val);
                std::memcpy(q, val, 4 * sizeof(float));
            }
            if (nd.chanSc >= 0) {
                sampleAnimSampler(data.anims.front().samplers[nd.chanSc], srcTime, val);
                std::memcpy(s, val, 3 * sizeof(float));
            }
            geoArchiveComposeTRS(t, q, s, local);
        } else {
            std::memcpy(local, nd.localMatrix, 16 * sizeof(float));
        }

        // World (parents-before-children walk order).
        if (nd.parentIndex >= 0 && (std::size_t)nd.parentIndex < i) {
            geoArchiveMatMul(&world[(std::size_t)nd.parentIndex * 16], local, &world[i * 16]);
        } else {
            std::memcpy(&world[i * 16], local, 16 * sizeof(float));
        }
    }

    // Pass 2: per-mesh payloads — morph targets deform the rest pose first
    // (glTF order: morph, then skin), skinned entries then deform against the
    // joint world matrices; rigid entries alias the result.
    out.deformed.resize(data.meshes.size());

    // Diagnostics gate (same pattern as ReadGeo's NATRON_DEBUG_GEOIO): how
    // many nodes have channels + the first skinned mesh's deformed bbox, so
    // headless tests can tell "channels didn't bind" from "skinning produced
    // identity".
    const bool dbgGeo = (std::getenv("NATRON_DEBUG_GEOIO") != NULL);
    int chanNodes = 0;
    for (const GeoArchiveNodeData& nd : data.nodes) {
        if (nd.chanTr >= 0 || nd.chanRot >= 0 || nd.chanSc >= 0 ||
            nd.chanWeights >= 0) ++chanNodes;
    }
    if (dbgGeo) {
        std::fprintf(stderr, "[GEOIO-ARCH] glTF t=%.3f chanNodes=%d skins=%d meshes=%d\n",
                     srcTime, chanNodes, (int)data.skinJoints.size(), (int)data.meshes.size());
    }

    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        const GeoArchiveMeshData& md = data.meshes[m];

        // Morph pass: build the morph-applied base (a copy when any weight is
        // non-zero, the rest pose otherwise). Weights: the owning node's
        // weights channel; without one, the file's rest weights.
        MeshDataPtr base;
        if (md.morphs && !md.morphs->targets.empty()) {
            std::vector<float> w(md.morphs->targets.size(), 0.0f);
            bool haveW = false;
            if (md.morphNodeIndex >= 0 && (std::size_t)md.morphNodeIndex < nNodes &&
                data.nodes[md.morphNodeIndex].chanWeights >= 0 && !data.anims.empty()) {
                const GeoArchiveAnimSamplerData& smp =
                    data.anims.front().samplers[data.nodes[md.morphNodeIndex].chanWeights];
                if ((int)w.size() == smp.nComp) {
                    sampleAnimSampler(smp, srcTime, w.data());
                    haveW = true;
                    if (dbgGeo) {
                        std::fprintf(stderr, "[GEOIO-ARCH]   morph mesh %d t=%.3f w=[", (int)m, srcTime);
                        for (std::size_t t = 0; t < w.size(); ++t) {
                            std::fprintf(stderr, "%s%.4f", t ? "," : "", w[t]);
                        }
                        std::fprintf(stderr, "]\n");
                    }
                }
            }
            if (!haveW && md.morphs->restWeights.size() == w.size()) {
                w = md.morphs->restWeights;
                haveW = true;
            }
            bool anyW = false;
            for (float ww : w) { if (ww != 0.0f) { anyW = true; break; } }
            if (anyW) {
                base.reset(new MeshData(*md.mesh));
                applyGeoMorphSet(*base, *md.morphs, w.data());
            }
        }

        if (md.skinIndex < 0 || md.skinIndex >= (int)data.skinJoints.size() ||
            md.joints.empty()) {
            out.deformed[m] = base ? base : md.mesh;
            continue;
        }

        const std::vector<int>& jointNodes = data.skinJoints[md.skinIndex];
        const std::vector<float>& ibms = data.skinIBMs[md.skinIndex];
        const std::size_t nJoints = jointNodes.size();

        // Skin matrices: S[j] = worldJoint[j] * IBM[j] (column-major).
        std::vector<float> skin((std::size_t)nJoints * 16, 0.0f);
        for (std::size_t j = 0; j < nJoints; ++j) {
            const int ji = jointNodes[j];
            if (ji < 0 || (std::size_t)ji >= nNodes) continue;
            geoArchiveMatMul(&world[(std::size_t)ji * 16], &ibms[j * 16], &skin[j * 16]);
        }
        // Deform the morph-applied base (or the rest pose) in place: each
        // vertex is read before it is written, so an in-place copy is safe.
        MeshDataPtr def(new MeshData(base ? *base : *md.mesh));
        const std::size_t numVerts = md.mesh->numVertices;
        const std::vector<unsigned int>& joints = md.joints;
        const std::vector<float>& weights = md.weights;
        float* dst = def->vertices.data();
        const float* src = def->vertices.data();

        for (std::size_t v = 0; v < numVerts; ++v) {
            // Linear blend skinning: p' = (sum_k w_k * S[j_k]) * p.
            float acc[16] = { 0.f };
            float wSum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float w = weights[(std::size_t)v * 4 + k];
                if (w <= 0.0f) continue;
                const unsigned int ji = joints[(std::size_t)v * 4 + k];
                if (ji >= nJoints) continue;
                const float* sm = &skin[(std::size_t)ji * 16];
                for (int e = 0; e < 16; ++e) acc[e] += w * sm[e];
                wSum += w;
            }
            if (wSum > 1e-8f && std::abs(wSum - 1.0f) > 1e-4f) {
                for (int e = 0; e < 16; ++e) acc[e] /= wSum;
            }
            const float x = src[v * 3], y = src[v * 3 + 1], z = src[v * 3 + 2];
            dst[v * 3]     = acc[0]*x + acc[4]*y + acc[8]*z  + acc[12];
            dst[v * 3 + 1] = acc[1]*x + acc[5]*y + acc[9]*z  + acc[13];
            dst[v * 3 + 2] = acc[2]*x + acc[6]*y + acc[10]*z + acc[14];
        }
        out.deformed[m] = def;

        if (dbgGeo) {
            // Per-frame: checksum of the deformed verts + vertex 0's raw
            // skin data (joints/weights) and its deformed position. A frozen
            // checksum with moving skin matrices means the per-vertex data
            // or the accumulation is broken.
            double checksum = 0.0;
            for (float f : def->vertices) checksum += f;
            std::fprintf(stderr, "[GEOIO-ARCH]   skin mesh %d: checksum=%.6f v0rest=(%.3f,%.3f,%.3f) v0def=(%.3f,%.3f,%.3f) j0=(%u,%u,%u,%u)w=(%.3f,%.3f,%.3f,%.3f)\n",
                         (int)m, checksum,
                         md.mesh->vertices[0], md.mesh->vertices[1], md.mesh->vertices[2],
                         def->vertices[0], def->vertices[1], def->vertices[2],
                         md.joints[0], md.joints[1], md.joints[2], md.joints[3],
                         md.weights[0], md.weights[1], md.weights[2], md.weights[3]);
        }
    }
}

NATRON_NAMESPACE_EXIT
