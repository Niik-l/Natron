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

#ifndef NATRON_GUI_VIEWPORT3DMESHCACHE_H
#define NATRON_GUI_VIEWPORT3DMESHCACHE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include "Global/GLIncludes.h"

#include <cstddef>
#include <map>
#include <memory>

#include "Engine/Dev/Scene3D/MeshData.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief GPU-resident copy of a MeshData for the 3D viewport.
 *
 * The viewport used to re-send every triangle of every mesh in immediate mode
 * on each paint (about ten GL calls per triangle plus a CPU headlight per
 * face), which is what made 1.5M-triangle scans sluggish to orbit. This cache
 * uploads a mesh once as
 *   - a flat corner array (position, face normal, uv per triangle corner) for
 *     the shaded / flat fill, drawn with one glDrawArrays, and
 *   - the original vertex array + the polygon edge list as an index buffer for
 *     the wireframe pass, drawn with one glDrawElements,
 * and reuses it until the mesh changes.
 *
 * Keys: the MeshData address, guarded by a weak_ptr (a recycled address with a
 * different MeshData never aliases a stale entry) and by MeshData::version,
 * which the producers bump whenever they rewrite vertices in place (animated /
 * deforming ReadGeo and Alembic meshes, Reverse Normals).
 *
 * GL names belong to the viewport's current context: call forgetAll() when the
 * context is recreated (initializeGL) and purgeIdle() from paintGL.
 */
class Viewport3DMeshCache
{
public:
    // Corner layout: position float3 (12 B) + normal GL_BYTE x4 normalized (4 B) + uv float2 (8 B).
    static const int kCornerStride = 24;
    static const int kNormalOffset = 12;
    static const int kUVOffset = 16;

    struct Entry
    {
        GLuint cornerVbo = 0;
        GLsizei cornerCount = 0;     // vertices in the corner array (3 per triangle)
        bool hasUV = false;          // per-corner uvs were available
        GLuint vertexVbo = 0;        // original vertex positions (wire pass)
        GLuint edgeEbo = 0;          // GL_UNSIGNED_INT index pairs into vertexVbo
        GLsizei edgeIndexCount = 0;
        GLuint colorVbo = 0;         // per-corner RGB (3x normalized GLubyte + pad), when the mesh bakes face colors
        bool hasVColor = false;      // colorVbo carries real per-face file colors
        unsigned long long version = 0;
        std::weak_ptr<MeshData> owner;
        std::size_t bytes = 0;
        int lastPaint = 0;
    };

    Viewport3DMeshCache();
    ~Viewport3DMeshCache();

    /** Return the GPU copy of `mesh`, uploading or refreshing it as needed.
     *  NULL when the mesh is empty or would not fit the memory budget (the
     *  caller then falls back to immediate mode). Requires a current context. */
    const Entry* acquire(const MeshDataPtr& mesh, int paintSerial);

    /** Delete entries not used for more than `maxIdlePaints` paints. */
    void purgeIdle(int paintSerial, int maxIdlePaints);

    /** Drop every entry WITHOUT deleting GL objects: the context that owned
     *  them is gone (QOpenGLWidget::initializeGL re-entry). */
    void forgetAll();

    /** Delete every entry's GL objects (context must be current). */
    void releaseAll();

    std::size_t bytesInUse() const { return _bytes; }
    std::size_t budget() const { return _budget; }
    void setBudget(std::size_t bytes) { _budget = bytes; }

private:
    void destroyEntry(Entry& e);
    bool upload(const MeshData& mesh, Entry& e);
    void evictFor(std::size_t neededBytes, int paintSerial);

    std::map<const MeshData*, Entry> _entries;
    std::size_t _bytes;
    std::size_t _budget;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORT3DMESHCACHE_H
