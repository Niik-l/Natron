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

// glTF 2.0 (.gltf / .glb) importer for ReadGeo, backed by the vendored
// cgltf parser (libs/cgltf, MIT).
//
// Stage-1 contract (ReadGeoImport.h): parse the whole file, return one
// GeoImportPart per renderable piece with the file-authored node transform
// already baked into the vertex positions, plus one material per part.
//
// glTF is Y-up right-handed, meters — the same convention Natron's 3D
// system uses, so positions are used as-is (no axis conversion needed).

#include "ReadGeoImport.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cgltf.h"

NATRON_NAMESPACE_ENTER

namespace {

// --- column-major 4x4 helpers --------------------------------------------

void
matIdentity(float m[16])
{
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

// out = a * b (column-major, out may alias neither)
void
matMul(const float a[16], const float b[16], float out[16])
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

// glTF node local transform: T * R * S (column-major)
void
matNodeLocal(const cgltf_node* node, float out[16])
{
    if (node->has_matrix) {
        // glTF node matrix is already column-major.
        std::memcpy(out, node->matrix, 16 * sizeof(float));
        return;
    }

    const float* t = node->translation;
    const float* q = node->rotation;
    const float* s = node->scale;

    // Quaternion (x,y,z,w) -> rotation matrix, column-major.
    const float x = node->has_rotation ? q[0] : 0.0f;
    const float y = node->has_rotation ? q[1] : 0.0f;
    const float z = node->has_rotation ? q[2] : 0.0f;
    const float w = node->has_rotation ? q[3] : 1.0f;
    const float sx = node->has_scale ? s[0] : 1.0f;
    const float sy = node->has_scale ? s[1] : 1.0f;
    const float sz = node->has_scale ? s[2] : 1.0f;

    // R[c*4+r] layout; entries then scaled per axis (R*S = R with columns
    // scaled by sx,sy,sz).
    const float xy = 2.0f * x * y, xz = 2.0f * x * z, yz = 2.0f * y * z;
    const float wx = 2.0f * w * x, wy = 2.0f * w * y, wz = 2.0f * w * z;
    const float xx = 2.0f * x * x, yy = 2.0f * y * y, zz = 2.0f * z * z;

    out[0]  = (1.0f - yy - zz) * sx; out[1]  = (xy + wz) * sx;
    out[2]  = (xz - wy) * sx;        out[3]  = 0.0f;
    out[4]  = (xy - wz) * sy;        out[5]  = (1.0f - xx - zz) * sy;
    out[6]  = (yz + wx) * sy;        out[7]  = 0.0f;
    out[8]  = (xz + wy) * sz;        out[9]  = (yz - wx) * sz;
    out[10] = (1.0f - xx - yy) * sz; out[11] = 0.0f;
    out[12] = node->has_translation ? t[0] : 0.0f;
    out[13] = node->has_translation ? t[1] : 0.0f;
    out[14] = node->has_translation ? t[2] : 0.0f;
    out[15] = 1.0f;
}

void
transformPoint(const float m[16], const float p[3], float out[3])
{
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

// --- embedded-image extraction -------------------------------------------

bool
decodeBase64(const std::string& in, std::vector<unsigned char>& out)
{
    static const int table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0x00-0x0F
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0x10-0x1F
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, // 0x20-0x2F  '+' '/'
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
// embedded content is written to a content-hashed temp file.
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

// --- material mapping ------------------------------------------------------

GeoImportMaterial
mapCgltfMaterial(const cgltf_material* m, const std::string& dir)
{
    GeoImportMaterial out;
    if (!m) return out;
    out.present = true;

    const cgltf_pbr_metallic_roughness& pbr = m->pbr_metallic_roughness;
    out.baseColor[0] = pbr.base_color_factor[0];
    out.baseColor[1] = pbr.base_color_factor[1];
    out.baseColor[2] = pbr.base_color_factor[2];
    out.metallic = pbr.metallic_factor;
    out.roughness = pbr.roughness_factor;

    // glTF emission is (color * strength); ReadGeo splits color and strength.
    double strength = 1.0;
    if (m->has_emissive_strength) {
        strength = m->emissive_strength.emissive_strength;
    }
    out.emission[0] = m->emissive_factor[0];
    out.emission[1] = m->emissive_factor[1];
    out.emission[2] = m->emissive_factor[2];
    // Keep emission black unless the file actually asks for it: emissive
    // factor defaults to [0,0,0] in glTF, so only report it when non-zero.
    if (m->emissive_factor[0] == 0.0 && m->emissive_factor[1] == 0.0 &&
        m->emissive_factor[2] == 0.0) {
        out.emissionStrength = 0.0;
    } else {
        out.emissionStrength = strength;
    }

    if (m->has_transmission) {
        out.transmission = m->transmission.transmission_factor;
    }
    if (m->has_ior) {
        out.ior = m->ior.ior;
    }
    if (m->has_specular) {
        out.specular = m->specular.specular_factor;
    }

    if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
        out.diffuseMapPath = resolveCgltfImage(pbr.base_color_texture.texture->image, dir);
    }

    return out;
}

// --- scene walk --------------------------------------------------------------

struct GltfWalkCtx
{
    std::vector<GeoImportPart>* parts;
    const std::string* dir;
    int skippedPrims = 0;
};

void
collectPrimitive(GltfWalkCtx& ctx, const cgltf_node* node, const cgltf_primitive* prim,
                 const std::string& basePath, int primIndex, const float world[16])
{
    if (prim->type != cgltf_primitive_type_triangles) {
        ++ctx.skippedPrims;
        return;
    }
    if (!prim->indices && prim->attributes_count == 0) {
        ++ctx.skippedPrims;
        return;
    }

    const cgltf_accessor* posAcc = NULL;
    const cgltf_accessor* uvAcc = NULL;
    for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
        const cgltf_attribute& att = prim->attributes[a];
        if (!std::strcmp(att.name, "POSITION") && !posAcc) {
            posAcc = att.data;
        } else if (!std::strcmp(att.name, "TEXCOORD_0") && !uvAcc) {
            uvAcc = att.data;
        }
    }
    if (!posAcc) {
        ++ctx.skippedPrims;
        return;
    }

    GeoImportPart part;
    part.label = basePath;
    if (primIndex > 0) {
        part.label += "[";
        part.label += std::to_string(primIndex + 1);
        part.label += "]";
    }
    part.material = mapCgltfMaterial(prim->material, *ctx.dir);

    // Positions (baked with the accumulated node transform below).
    const cgltf_size numVerts = posAcc->count;
    std::vector<float> rawPos((std::size_t)numVerts * 3);
    for (cgltf_size i = 0; i < numVerts; ++i) {
        cgltf_accessor_read_float(posAcc, i, &rawPos[i * 3], 3);
    }
    // The parsed positions BECOME the part's vertices (the bake below
    // transforms them in place); part.vertices starts out zero-filled and
    // must not be handed to the baking step empty.
    part.vertices = std::move(rawPos);

    // Indices (or implicit sequential when the primitive is unindexed).
    const cgltf_size indexCount = prim->indices ? prim->indices->count : numVerts;
    std::vector<unsigned int> indices(indexCount);
    if (prim->indices) {
        for (cgltf_size i = 0; i < indexCount; ++i) {
            cgltf_accessor_read_uint(prim->indices, i, &indices[i], 1);
        }
    } else {
        for (cgltf_size i = 0; i < indexCount; ++i) {
            indices[i] = (unsigned int)i;
        }
    }

    // Per-vertex UVs, expanded per face corner with the glTF V-flip
    // (glTF UV origin is top-left, OpenGL/Natron's is bottom-left).
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

    // Faces: glTF primitives are triangle lists (STRIP/FAN are deprecated and
    // rare — counted as skipped rather than decoded).
    const std::size_t numFaces = indexCount / 3;
    part.faceCounts.reserve(numFaces);
    part.faceIndices.reserve(indexCount);
    if (uvAcc) {
        part.uvs.reserve(indexCount * 2);
    }
    for (std::size_t f = 0; f < numFaces; ++f) {
        const unsigned int i0 = indices[f * 3];
        const unsigned int i1 = indices[f * 3 + 1];
        const unsigned int i2 = indices[f * 3 + 2];
        if (i0 >= numVerts || i1 >= numVerts || i2 >= numVerts) continue;
        part.faceCounts.push_back(3);
        part.faceIndices.push_back((int)i0);
        part.faceIndices.push_back((int)i1);
        part.faceIndices.push_back((int)i2);
        if (uvAcc) {
            for (int k = 0; k < 3; ++k) {
                const unsigned int vi = indices[f * 3 + k];
                part.uvs.push_back(uvPerVert[(std::size_t)vi * 2]);
                part.uvs.push_back(uvPerVert[(std::size_t)vi * 2 + 1]);
            }
        }
    }
    if (part.faceCounts.empty()) {
        ++ctx.skippedPrims;
        return;
    }
    part.hasUVs = (uvAcc != NULL);

    // Bake the accumulated world transform into the part's vertices.
    importBakeXform(part.vertices, world);

    ctx.parts->push_back(std::move(part));
}

void
walkGltfNode(GltfWalkCtx& ctx, const cgltf_node* node, const float parentWorld[16],
             const std::string& parentPath)
{
    float local[16];
    matNodeLocal(node, local);
    float world[16];
    matMul(parentWorld, local, world);

    std::string name = node->name ? node->name : "";
    std::string path = parentPath.empty() ? name : (parentPath + "/" + name);
    if (path.empty()) {
        path = "node"; // anonymous nodes still need a dropdown entry
    }

    if (node->mesh) {
        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            collectPrimitive(ctx, node, &node->mesh->primitives[p], path, (int)p, world);
        }
    }

    for (cgltf_size c = 0; c < node->children_count; ++c) {
        walkGltfNode(ctx, node->children[c], world, path);
    }
}

} // anonymous namespace

GeoImportResult
loadGltfGeoImport(const std::string& path)
{
    GeoImportResult result;

    // Directory of the .gltf/.glb, for resolving external texture URIs.
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

    GltfWalkCtx ctx;
    ctx.parts = &result.parts;
    ctx.dir = &dir;

    float identity[16];
    matIdentity(identity);
    for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
        walkGltfNode(ctx, scene->nodes[i], identity, "");
    }

    if (result.parts.empty()) {
        result.error = "No triangle geometry found in the glTF file";
        if (ctx.skippedPrims > 0) {
            result.error += " (" + std::to_string(ctx.skippedPrims) +
                            " non-triangle or empty primitives skipped)";
        }
        return result;
    }
    if (ctx.skippedPrims > 0) {
        result.error = std::to_string(ctx.skippedPrims) +
                       " non-triangle/empty primitives were skipped.";
    }

    result.numMaterials = (int)data->materials_count;
    result.ok = true;
    return result;
}

NATRON_NAMESPACE_EXIT
