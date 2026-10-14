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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

NATRON_NAMESPACE_ENTER

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

void
importAppendPart(const GeoImportPart& part, MeshData* mesh)
{
    const std::size_t vertexOffset = mesh->numVertices;

    mesh->vertices.insert(mesh->vertices.end(), part.vertices.begin(), part.vertices.end());
    mesh->numVertices += part.vertices.size() / 3;

    const std::size_t faceIdxBegin = mesh->faceIndices.size();
    for (std::size_t i = 0; i < part.faceIndices.size(); ++i) {
        mesh->faceIndices.push_back(part.faceIndices[i] + (int)vertexOffset);
    }
    mesh->faceCounts.insert(mesh->faceCounts.end(), part.faceCounts.begin(), part.faceCounts.end());
    mesh->numFaces += part.faceCounts.size();

    // UVs are per face-vertex, parallel to faceIndices. Parts without UVs
    // contribute zero-padded slots so the interleaving stays aligned.
    if (part.hasUVs) {
        mesh->hasUVs = true;
        mesh->texCoordComponents = 2;
    }
    if (mesh->hasUVs && mesh->texCoordComponents == 2) {
        mesh->uvs.resize(faceIdxBegin * 2, 0.0f);
        if (part.hasUVs) {
            mesh->uvs.insert(mesh->uvs.end(), part.uvs.begin(), part.uvs.end());
        } else {
            mesh->uvs.resize(faceIdxBegin * 2 + part.faceIndices.size() * 2, 0.0f);
        }
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

NATRON_NAMESPACE_EXIT
