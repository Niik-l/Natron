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

// FBX importer for ReadGeo, backed by the vendored ufbx parser
// (libs/ufbx, MIT / public domain) — the GPL-compatible replacement for the
// Autodesk FBX SDK (see DEV_3D_SYSTEM_CHANGELOG.md).
//
// Stage-1 contract (ReadGeoImport.h): parse the whole file, return one
// GeoImportPart per renderable piece (mesh x material group) with the
// file-authored transform already baked into the vertex positions.
//
// Space conversion: FBX files carry their own axis system and unit
// (Blender/Maya FBX exports are Y-up centimeters). ufbx normalizes at load
// time via target_axes / target_unit_meters so every FBX lands in Natron's
// Y-up right-handed meters space — the same space glTF is natively in —
// instead of every user having to fix the 100x scale by hand.

#include "ReadGeoImport.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ufbx.h"

NATRON_NAMESPACE_ENTER

namespace {

std::string
ufbxToString(const ufbx_string& s)
{
    if (!s.data || s.length == 0) return std::string();
    return std::string(s.data, s.length);
}

// Resolve a texture to a usable file path: absolute path from the file,
// relative path resolved against the FBX directory, or the embedded content
// blob written to a content-hashed temp file.
std::string
resolveUfbxTexture(const ufbx_texture* tex, const std::string& dir)
{
    if (!tex) return std::string();

    if (tex->content.data && tex->content.size > 0) {
        std::string embedded = importWriteEmbeddedTexture(tex->content.data, tex->content.size, "fbx_tex");
        if (!embedded.empty()) return embedded;
    }

    std::string rel = ufbxToString(tex->relative_filename);
    std::string abs = ufbxToString(tex->absolute_filename);
    std::string file = ufbxToString(tex->filename);
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
mapUfbxMaterial(const ufbx_material* m, const std::string& dir)
{
    GeoImportMaterial out;
    if (!m) return out;
    out.present = true;

    const ufbx_material_map& baseColor = m->pbr.base_color;
    const ufbx_material_map& roughness = m->pbr.roughness;
    const ufbx_material_map& metalness = m->pbr.metalness;
    const ufbx_material_map& specFactor = m->pbr.specular_factor;
    const ufbx_material_map& specIor = m->pbr.specular_ior;
    const ufbx_material_map& transFactor = m->pbr.transmission_factor;
    const ufbx_material_map& emiColor = m->pbr.emission_color;
    const ufbx_material_map& emiFactor = m->pbr.emission_factor;

    if (baseColor.has_value && baseColor.value_components >= 3) {
        out.baseColor[0] = baseColor.value_vec3.x;
        out.baseColor[1] = baseColor.value_vec3.y;
        out.baseColor[2] = baseColor.value_vec3.z;
    } else if (!baseColor.texture) {
        // No color and no texture: leave the knob at its default instead of
        // stomping it with ufbx's "missing factor" default.
        out.present = false;
    }

    if (roughness.has_value) out.roughness = roughness.value_real;
    if (metalness.has_value) out.metallic = metalness.value_real;
    if (specFactor.has_value) out.specular = specFactor.value_real;
    if (specIor.has_value && specIor.value_real >= 1.0) out.ior = specIor.value_real;
    if (transFactor.has_value) out.transmission = transFactor.value_real;

    if (emiColor.has_value && emiColor.value_components >= 3) {
        out.emission[0] = emiColor.value_vec3.x;
        out.emission[1] = emiColor.value_vec3.y;
        out.emission[2] = emiColor.value_vec3.z;
        out.emissionStrength = emiFactor.has_value ? emiFactor.value_real : 1.0;
    }

    if (baseColor.texture) {
        out.diffuseMapPath = resolveUfbxTexture(baseColor.texture, dir);
    }

    return out;
}

struct FbxWalkCtx
{
    std::vector<GeoImportPart>* parts;
    const std::string* dir;
};

void
collectNodeMesh(FbxWalkCtx& ctx, const ufbx_node* node)
{
    const ufbx_mesh* mesh = node->mesh;
    if (!mesh || mesh->faces.count == 0) return;

    const std::string nodePath = ufbxToString(node->name);

    // Group faces by material index. Meshes with a single (or no) material
    // produce one part labeled with the node path; multi-material meshes get
    // one part per material labeled path/matname.
    std::map<uint32_t, std::vector<uint32_t>> facesByMat;
    const bool hasPerFaceMats = mesh->face_material.count == mesh->faces.count;
    for (size_t f = 0; f < mesh->faces.count; ++f) {
        const ufbx_face& face = mesh->faces.data[f];
        if (face.num_indices < 3) continue;
        const uint32_t matIdx = hasPerFaceMats ? mesh->face_material.data[f] : 0;
        facesByMat[matIdx].push_back((uint32_t)f);
    }
    if (facesByMat.empty()) return;

    const size_t numMats = mesh->materials.count;
    const bool singleMat = (facesByMat.size() == 1);

    for (const auto& kv : facesByMat) {
        GeoImportPart part;
        part.label = nodePath.empty() ? std::string("mesh") : nodePath;
        const uint32_t matIdx = kv.first;
        if (!singleMat && matIdx < numMats) {
            const std::string matName = ufbxToString(mesh->materials.data[matIdx]->name);
            part.label += "/";
            part.label += matName.empty() ? ("material " + std::to_string(matIdx + 1)) : matName;
        }
        part.material = mapUfbxMaterial(matIdx < numMats ? mesh->materials.data[matIdx] : NULL, *ctx.dir);

        // Bake node transforms once per unique position (the same position
        // index is shared by adjacent faces; a linear transform gives the
        // same result per corner, so transform in place per index).
        std::vector<char> posTouched(mesh->vertex_position.values.count, 0);
        part.vertices.resize((std::size_t)mesh->vertex_position.values.count * 3, 0.0f);

        part.faceIndices.reserve(kv.second.size() * 6);
        part.faceCounts.reserve(kv.second.size());
        bool anyUV = false;
        const ufbx_vertex_vec2* uvStream = NULL;
        if (mesh->uv_sets.count > 0 && mesh->uv_sets.data[0].vertex_uv.exists) {
            uvStream = &mesh->uv_sets.data[0].vertex_uv;
        }

        for (uint32_t f : kv.second) {
            const ufbx_face& face = mesh->faces.data[f];
            part.faceCounts.push_back((int)face.num_indices);
            for (uint32_t k = 0; k < face.num_indices; ++k) {
                const uint32_t ci = face.index_begin + k;
                const uint32_t pi = mesh->vertex_position.indices.data[ci];
                if (!posTouched[pi]) {
                    posTouched[pi] = 1;
                    const ufbx_vec3 pos = mesh->vertex_position.values.data[pi];
                    const ufbx_vec3 wp = ufbx_transform_position(&node->geometry_to_world, pos);
                    part.vertices[(std::size_t)pi * 3] = wp.x;
                    part.vertices[(std::size_t)pi * 3 + 1] = wp.y;
                    part.vertices[(std::size_t)pi * 3 + 2] = wp.z;
                }
                part.faceIndices.push_back((int)pi);
                if (uvStream) {
                    const ufbx_vec2 uv = uvStream->values.data[uvStream->indices.data[ci]];
                    part.uvs.push_back(uv.x);
                    part.uvs.push_back(uv.y);
                    anyUV = true;
                }
            }
        }

        // Vertex array may be larger than what faces reference — trim the
        // untouched tail so numVertices reflects real geometry. Every
        // referenced position index is < used, so faceIndices stay valid.
        std::size_t used = 0;
        for (std::size_t v = 0; v < posTouched.size(); ++v) {
            if (posTouched[v]) used = v + 1;
        }
        if (used < posTouched.size()) {
            part.vertices.resize(used * 3);
        }
        part.hasUVs = anyUV;

        ctx.parts->push_back(std::move(part));
    }
}

void
walkFbxNode(FbxWalkCtx& ctx, const ufbx_node* node, const std::string& parentPath)
{
    std::string path = parentPath.empty() ? ufbxToString(node->name)
                                          : (parentPath + "/" + ufbxToString(node->name));
    if (node->mesh) {
        collectNodeMesh(ctx, node);
    }
    for (size_t c = 0; c < node->children.count; ++c) {
        walkFbxNode(ctx, node->children.data[c], path);
    }
}

} // anonymous namespace

GeoImportResult
loadFbxGeoImport(const std::string& path)
{
    GeoImportResult result;

    std::string dir;
    {
        const std::size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) dir = path.substr(0, slash);
    }

    ufbx_load_opts opts;
    std::memset(&opts, 0, sizeof(opts));
    // Normalize to Natron's 3D space: Y-up right-handed, one unit = 1 meter.
    opts.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    opts.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    opts.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
    opts.target_unit_meters = 1.0f;
    // Better PBR mapping for Blender-authored files (Metallic/Roughness).
    opts.use_blender_pbr_material = true;
    // Missing external textures must not abort the geometry load.
    opts.ignore_missing_external_files = true;
    // Animation/skinning deferred (stage 2).
    opts.evaluate_skinning = false;
    opts.evaluate_caches = false;

    ufbx_error err;
    std::memset(&err, 0, sizeof(err));
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &err);
    if (!scene) {
        result.error = "ufbx failed to load the FBX file: " + ufbxToString(err.description);
        return result;
    }
    struct FreeGuard {
        ufbx_scene* s;
        ~FreeGuard() { if (s) ufbx_free_scene(s); }
    } guard{ scene };

    FbxWalkCtx ctx;
    ctx.parts = &result.parts;
    ctx.dir = &dir;

    for (size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node* node = scene->nodes.data[i];
        if (node->is_root) {
            for (size_t c = 0; c < node->children.count; ++c) {
                walkFbxNode(ctx, node->children.data[c], "");
            }
        }
    }

    if (result.parts.empty()) {
        result.error = "No geometry found in the FBX file";
        return result;
    }

    result.numMaterials = (int)scene->materials.count;
    result.ok = true;
    return result;
}

NATRON_NAMESPACE_EXIT
