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
// Embedded content is staged next to the source FBX in its same-named ".fbm"
// side folder (DCC convention) when fbmDir is non-empty, else to TEMP.
std::string
resolveUfbxTextureArc(const ufbx_texture* tex, const std::string& dir,
                      const std::string& fbmDir)
{
    if (!tex) return std::string();
    if (tex->content.data && tex->content.size > 0) {
        std::string embedded =
            importWriteEmbeddedTexture(tex->content.data, tex->content.size, "fbx_tex",
                                       fbmDir, arcUfbxToString(tex->name));
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
mapUfbxMaterialArc(const ufbx_material* m, const std::string& dir,
                   const std::string& fbmDir)
{
    GeoImportMaterial out;
    if (!m) return out;
    out.present = true;

    // Shading model class: FBX builtin (Lambert/Phong/Blender-Phong) materials
    // follow the LEGACY transparency convention (below) and ship no PBR
    // channel values; genuine PBR models map their own conventions.
    const bool builtinShading =
        m->shader_type == UFBX_SHADER_UNKNOWN ||
        m->shader_type == UFBX_SHADER_FBX_LAMBERT ||
        m->shader_type == UFBX_SHADER_FBX_PHONG ||
        m->shader_type == UFBX_SHADER_BLENDER_PHONG;

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
    if (baseColor.texture) {
        out.diffuseMapPath = resolveUfbxTextureArc(baseColor.texture, dir, fbmDir);
    }

    // Roughness/metallic texture + normal map, staged like the diffuse map —
    // glTF parity (the same GeoImportMaterial fields feed the per-entry
    // provider and the Re-build Materials Material3D chains; CyclesRenderer
    // channel-splits the ORM file G=roughness / B=metallic, grayscale gloss
    // maps are channel-equal). FBX builtin Phong carries its gloss/shininess
    // map here (ufbx re-derives the roughness factor from the exponent);
    // genuine PBR shading models bring a true roughness map directly.
    // ormMetallicEnabled stays FALSE for FBX: its ORM B channel is gloss, not
    // metalness — letting it drive metallic shaded the surface half-metal
    // (washed pastel) where the same model through Blender's glTF export
    // renders as the file's own metallic factor (0 = dielectric).
    if (m->pbr.roughness.texture) {
        out.metallicRoughnessMapPath =
            resolveUfbxTextureArc(m->pbr.roughness.texture, dir, fbmDir);
        if (m->features.metalness.enabled) out.ormMetallicEnabled = true;
    }
    if (m->pbr.normal_map.texture) {
        out.normalMapPath = resolveUfbxTextureArc(m->pbr.normal_map.texture, dir, fbmDir);
    }

    // Opacity (alpha cutout) — FBX builtin shading only. ufbx leaves
    // pbr.opacity EMPTY for FBX-Phong (its mapping table has no opacity
    // channel): Mixamo serializes the "..._Opacity" masks under the legacy
    // TransparentColor property instead (same slot family as
    // TransparencyFactor, verified with the standalone probe: hair/eyelash
    // carry the mask there with factor 1.0 = opaque). Legacy convention says
    // the mask reads OPACITY — white is opaque, no invert (the same asset's
    // Blender glTF export produced a straight-alpha hair texture that renders
    // hair strands opaque, gaps transparent; reading it as transparency would
    // vanish the hair outright). Genuine PBR shading models keep the default
    // fully opaque and map nothing: their transparency conventions differ and
    // the pbr.opacity channel would need per-shader handling.
    if (builtinShading && m->fbx.transparency_color.texture) {
        out.opacityMapPath =
            resolveUfbxTextureArc(m->fbx.transparency_color.texture, dir, fbmDir);
    }

    // FBX builtin shading models (Lambert/Phong/Blender-Phong) have no PBR
    // transmission: ufbx fills their pbr.transmission_factor from the FBX
    // LEGACY TransparencyFactor, whose convention is INVERTED (1.0 = fully
    // opaque). Mapping it through made every Mixamo FIGURE material glass
    // (dark refractive + fireflies) while the same model exported to glTF
    // renders correctly (cgltf has_transmission false). Only trust
    // transmission from shading models that actually define it, and mirror
    // the glTF emission rule (all-zero emissive color = no emission).
    if (!builtinShading && m->pbr.transmission_factor.has_value) {
        out.transmission = m->pbr.transmission_factor.value_real;
    }
    if (m->pbr.emission_color.has_value && m->pbr.emission_color.value_components >= 3) {
        const bool zero = m->pbr.emission_color.value_vec3.x == 0.0 &&
                          m->pbr.emission_color.value_vec3.y == 0.0 &&
                          m->pbr.emission_color.value_vec3.z == 0.0;
        if (!zero) {
            out.emission[0] = m->pbr.emission_color.value_vec3.x;
            out.emission[1] = m->pbr.emission_color.value_vec3.y;
            out.emission[2] = m->pbr.emission_color.value_vec3.z;
            out.emissionStrength = m->pbr.emission_factor.has_value
                                       ? m->pbr.emission_factor.value_real : 1.0;
        }
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

    // FBM side folder for embedded textures: chair.fbx -> chair.fbm next to
    // the source file (DCC convention). Empty when the file has no directory.
    std::string fbmDir;
    {
        const std::size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) {
            const std::size_t dot = path.find_last_of('.');
            if (dot != std::string::npos && dot > slash + 1) fbmDir = path.substr(slash + 1, dot - slash - 1);
            else fbmDir = path.substr(slash + 1);
        } else {
            const std::size_t dot = path.find_last_of('.');
            fbmDir = dot != std::string::npos ? path.substr(0, dot) : path;
        }
        fbmDir = dir + "/" + fbmDir + ".fbm";
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
        result.materials[m] = mapUfbxMaterialArc(scene->materials.data[m], dir, fbmDir);
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

                // Blend shapes (morph targets): one shared set for all
                // material pieces of this node mesh. Targets are the blend
                // deformer channels in order; the per-frame evaluator walks
                // the SAME lists with the SAME skip rule (channels without a
                // usable target shape are skipped on both sides), so target
                // index k and evaluated weight k stay aligned. Offsets are
                // sparse (the file lists only moved vertices) and relative to
                // the mesh's vertex pool — the pieces keep pool indexing.
                std::shared_ptr<GeoMorphSet> morphSet;
                if (mesh->blend_deformers.count > 0) {
                    morphSet.reset(new GeoMorphSet());
                    for (std::size_t d = 0; d < mesh->blend_deformers.count; ++d) {
                        const ufbx_blend_deformer* def = mesh->blend_deformers.data[d];
                        if (!def) continue;
                        for (std::size_t c = 0; c < def->channels.count; ++c) {
                            const ufbx_blend_channel* chan = def->channels.data[c];
                            if (!chan) continue;
                            const ufbx_blend_shape* shape = chan->target_shape;
                            if (!shape || shape->num_offsets == 0) continue;

                            GeoMorphTarget tgt;
                            tgt.name = arcUfbxToString(chan->name);
                            if (tgt.name.empty()) tgt.name = arcUfbxToString(shape->name);
                            if (tgt.name.empty()) {
                                tgt.name = "shape " + std::to_string(morphSet->targets.size() + 1);
                            }
                            tgt.indices.reserve(shape->num_offsets);
                            tgt.deltas.reserve(shape->num_offsets * 3);
                            for (std::size_t o = 0; o < shape->num_offsets; ++o) {
                                const uint32_t vi = shape->offset_vertices.data[o];
                                ufbx_vec3 off = shape->position_offsets.data[o];
                                if (o < shape->offset_weights.count) {
                                    const ufbx_real ow = shape->offset_weights.data[o];
                                    off.x *= ow; off.y *= ow; off.z *= ow;
                                }
                                tgt.indices.push_back(vi);
                                tgt.deltas.push_back((float)off.x);
                                tgt.deltas.push_back((float)off.y);
                                tgt.deltas.push_back((float)off.z);
                            }
                            morphSet->targets.push_back(std::move(tgt));
                            morphSet->restWeights.push_back((float)chan->weight);
                        }
                    }
                    if (morphSet->targets.empty()) morphSet.reset();
                    else morphSet->id = result->numMorphSets++;
                }

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
                        md.morphs = morphSet;

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

    // Animation range from the animation stack with actual channel content.
    // ufbx's scene->anim is the DEFAULT stack ("Take 001"), which Mixamo
    // exports ship EMPTY — the real clip lives in a second stack named
    // "mixamo.com". Evaluating the empty default yields the rest pose at
    // every time (the static T-pose bug), so pick the stack carrying the
    // most anim_values; fall back to the default when none has content.
    // The chosen stack index rides out through GeoArchiveResult::fbxAnimStack
    // and the frame evaluator drives THAT stack.
    if (scene->anim_stacks.count > 0) {
        std::size_t bestStack = scene->anim_stacks.count;
        std::size_t bestValues = 0;
        for (std::size_t s = 0; s < scene->anim_stacks.count; ++s) {
            const ufbx_anim_stack* st = scene->anim_stacks.data[s];
            if (!st || !st->anim) continue;
            std::size_t values = 0;
            for (std::size_t l = 0; l < st->anim->layers.count; ++l) {
                const ufbx_anim_layer* layer = st->anim->layers.data[l];
                if (layer) values += (std::size_t)layer->anim_values.count;
            }
            if (values > bestValues) {
                bestValues = values;
                bestStack = s;
            }
        }
        const ufbx_anim* rangeAnim = scene->anim;
        if (bestStack < scene->anim_stacks.count) {
            result.fbxAnimStack = (int)bestStack;
            rangeAnim = scene->anim_stacks.data[bestStack]->anim;
        }
        if (rangeAnim) {
            result.animTimeBegin = rangeAnim->time_begin;
            result.animTimeEnd = rangeAnim->time_end;
        }
        if (std::getenv("NATRON_DEBUG_GEOIO")) {
            std::fprintf(stderr,
                "[GEOIO-ARCH-FBX] anim stacks=%d chosen=%d values=%d time=%.3f..%.3f (default '%s')\n",
                (int)scene->anim_stacks.count, result.fbxAnimStack,
                (int)bestValues, result.animTimeBegin, result.animTimeEnd,
                scene->anim && scene->anim->layers.count > 0 &&
                    scene->anim->layers.data[0]
                    ? (scene->anim->layers.data[0]->name.data ? scene->anim->layers.data[0]->name.data : "?")
                    : "?");
        }
    } else if (scene->anim) {
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
                        std::string& err,
                        const GeoArchiveMorphOverride* morphOverride)
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

    // Drive the loader-chosen animation stack (GeoArchiveResult::fbxAnimStack
    // — the non-empty one; ufbx's default scene->anim is EMPTY in Mixamo
    // files). Falls back to the default when the index is out of range or
    // the file had no non-empty stack.
    ufbx_anim* evalAnim = scene->anim;
    if (data.fbxAnimStack >= 0 &&
        (std::size_t)data.fbxAnimStack < scene->anim_stacks.count) {
        const ufbx_anim_stack* chosen = scene->anim_stacks.data[data.fbxAnimStack];
        if (chosen && chosen->anim) evalAnim = chosen->anim;
    }

    ufbx_error e;
    std::memset(&e, 0, sizeof(e));
    ufbx_scene* ev = ufbx_evaluate_scene(scene, evalAnim, srcTime, &eopts, &e);
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
        const ufbx_scene* origScene;   // original scene: blend channels + anim
        double srcTime;
        const GeoArchiveMorphOverride* morphOverride;  // user weight override
        std::size_t entryIdx;   // next expected node index (entry counter)
        bool orderBroken;

        void run(const ufbx_node* node, const ufbx_node* origNode)
        {
            if (orderBroken) return;
            const std::size_t i = entryIdx++;
            if (i >= data->nodes.size()) { orderBroken = true; return; }
            if (!origNode || origNode->is_root ||
                (origNode->mesh ? 1 : 0) != (node->mesh ? 1 : 0) ||
                origNode->children.count != node->children.count) {
                // The evaluated tree must mirror the original 1:1 (it does by
                // construction; this guard keeps a mismatch from silently
                // zipping the wrong nodes).
                orderBroken = true; return;
            }

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
                    // geometry space. Blend shape deltas (below) and the
                    // geometry->node bake both live in that space too, so the
                    // order is: copy raw → add morph offsets → bake.
                    const ufbx_vertex_vec3* srcPos =
                        (mesh->skinned_position.exists ? &mesh->skinned_position
                                                       : &mesh->vertex_position);

                    // ufbx CPU skinning evaluates in WORLD space
                    // (ufbx_get_skin_vertex_matrix composes bone to_world ×
                    // inv_bind; ufbx sets skinned_is_local = false for those).
                    // This evaluator's contract is GEOMETRY-space payloads —
                    // the node chain (root normalization folded at the top)
                    // must compose exactly once downstream — so map the
                    // skinned positions back through inv(geometry_to_world).
                    // Without this the chain applies a SECOND time and skinned
                    // meshes land at the wrong scale/position (the Mixamo
                    // skeleton/mesh 100x mismatch: bones at meters, mesh at
                    // centimeters — or the reverse).
                    const bool skinnedWorld =
                        mesh->skinned_position.exists && !mesh->skinned_is_local;
                    ufbx_matrix worldToGeometry;
                    if (skinnedWorld) {
                        const ufbx_matrix geometryToWorld = node->geometry_to_world;
                        worldToGeometry = ufbx_matrix_invert(&geometryToWorld);
                        if (std::getenv("NATRON_DEBUG_GEOIO")) {
                            std::fprintf(stderr,
                                "[GEOIO-ARCH-FBX]   skin world->geometry remap g2wDiag=(%.4f,%.4f,%.4f)\n",
                                geometryToWorld.m00, geometryToWorld.m11,
                                geometryToWorld.m22);
                        }
                    }
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
                        if (skinnedWorld) {
                            p = ufbx_transform_position(&worldToGeometry, p);
                        }
                        def->vertices[v * 3] = (float)p.x;
                        def->vertices[v * 3 + 1] = (float)p.y;
                        def->vertices[v * 3 + 2] = (float)p.z;
                    }

                    // Blend shapes: per-channel weights evaluated from the
                    // file's animation (the DeformPercent property; 1.0 =
                    // full weight, unanimated channels fall back to their
                    // load-time weight). Offsets are relative to the rest
                    // pool — applied in geometry space, before the bake. On a
                    // SKINNED mesh the offsets land on the skinned positions
                    // (exact for the common skeleton-free shape-key rig; the
                    // pre-skin deformer order would need a re-skin here).
                    const GeoArchiveMeshData& md = data->meshes[meshIdx];
                    if (md.morphs && !md.morphs->targets.empty() && origNode->mesh) {
                        const ufbx_mesh* origMesh = origNode->mesh;
                        std::vector<float> w(md.morphs->targets.size(), 0.0f);
                        std::size_t k = 0;
                        for (std::size_t d = 0;
                             d < origMesh->blend_deformers.count && k < w.size(); ++d) {
                            const ufbx_blend_deformer* defo = origMesh->blend_deformers.data[d];
                            if (!defo) continue;
                            for (std::size_t c = 0;
                                 c < defo->channels.count && k < w.size(); ++c) {
                                const ufbx_blend_channel* chan = defo->channels.data[c];
                                if (!chan) continue;
                                const ufbx_blend_shape* shape = chan->target_shape;
                                if (!shape || shape->num_offsets == 0) continue;
                                w[k++] = (float)ufbx_evaluate_blend_weight(
                                    origScene->anim, chan, srcTime);
                            }
                        }
                        // User override (ReadGeo/ReadGeoArchive Morph Weights
                        // knobs): when enabled and this set has an entry, its
                        // per-target weights replace the file's evaluated blend
                        // weights (same target order as the walk above).
                        if (morphOverride && morphOverride->enabled &&
                            morphOverride->weights && md.morphs->id >= 0) {
                            std::map<int, std::vector<float>>::const_iterator oit =
                                morphOverride->weights->find(md.morphs->id);
                            if (oit != morphOverride->weights->end() &&
                                oit->second.size() == w.size()) {
                                std::copy(oit->second.begin(), oit->second.end(), w.begin());
                            }
                        }
                        applyGeoMorphSet(*def, *md.morphs, w.data());
                        if (std::getenv("NATRON_DEBUG_GEOIO") && k > 0) {
                            std::fprintf(stderr,
                                         "[GEOIO-ARCH-FBX]   morph mesh %d t=%.3f w0=%.4f targets=%d\n",
                                         (int)meshIdx, srcTime, w[0], (int)w.size());
                        }
                    }

                    if (bake) {
                        for (std::size_t v = 0; v < nv; ++v) {
                            ufbx_vec3 p;
                            p.x = def->vertices[v * 3];
                            p.y = def->vertices[v * 3 + 1];
                            p.z = def->vertices[v * 3 + 2];
                            const ufbx_vec3 bp =
                                ufbx_transform_position(&node->geometry_to_node, p);
                            def->vertices[v * 3] = (float)bp.x;
                            def->vertices[v * 3 + 1] = (float)bp.y;
                            def->vertices[v * 3 + 2] = (float)bp.z;
                        }
                    }
                    geoArchiveMatIdentity(def->transform);
                    out->deformed[meshIdx] = def;
                }
            }

            for (std::size_t c = 0; c < node->children.count; ++c) {
                run(node->children.data[c], origNode->children.data[c]);
            }
        }
    } evw;
    evw.out = &out;
    evw.data = &data;
    evw.origScene = scene;
    evw.srcTime = srcTime;
    evw.morphOverride = morphOverride;
    evw.entryIdx = 0;
    evw.orderBroken = false;

    for (std::size_t i = 0; i < ev->nodes.count; ++i) {
        const ufbx_node* node = ev->nodes.data[i];
        if (node->is_root) {
            const ufbx_node* origRoot = scene->nodes.data[i];
            if (!origRoot->is_root || origRoot->children.count != node->children.count) {
                err = "FBX structure changed between parse and evaluation";
                return false;
            }
            for (std::size_t c = 0; c < node->children.count; ++c) {
                evw.run(node->children.data[c], origRoot->children.data[c]);
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
