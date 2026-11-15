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

#ifndef NATRON_GUI_VIEWPORT3DSPLATCACHE_H
#define NATRON_GUI_VIEWPORT3DSPLATCACHE_H

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

#include "Engine/Dev/Scene3D/SplatData.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief GPU-resident point preview of a Gaussian splat scene for the 3D viewport.
 *
 * A splat scene is millions of points. Building and sending them on every paint
 * (what the particle preview does) would make the viewport unusable, so the
 * preview points are uploaded once into one vertex buffer and drawn with a
 * single glDrawArrays until the scene or a setting baked into the buffer
 * changes. Same life-cycle rules as Viewport3DMeshCache.
 *
 * Keys: the SplatData address, guarded by a weak_ptr (a recycled address never
 * aliases a stale entry) and by SplatData::version, plus the settings baked
 * into the buffer (opacity threshold, point limit, size limit). Point Scale and
 * the node transform are NOT baked: they are a uniform and a matrix at draw
 * time, so dragging them costs nothing.
 *
 * GL names belong to the viewport's current context: call forgetAll() when the
 * context is recreated (initializeGL) and purgeIdle() from paintGL.
 */
class Viewport3DSplatCache
{
public:
    // Point layout: position float3 (12 B) + colour GLubyte x4 normalized (4 B) + diameter float (4 B).
    static const int kStride = 20;
    static const int kColorOffset = 12;
    static const int kSizeOffset = 16;

    struct Entry
    {
        GLuint vbo = 0;
        GLsizei count = 0;
        unsigned long long version = 0;
        float opacityThreshold = 0.f;
        std::size_t maxPoints = 0;
        float sizeClamp = 0.f;
        std::weak_ptr<const SplatData> owner;
        std::size_t bytes = 0;
        int lastPaint = 0;
    };

    Viewport3DSplatCache();
    ~Viewport3DSplatCache();

    /** Return the GPU copy of the preview points of `data`, uploading or
     *  refreshing it as needed. NULL when there is nothing to draw. Requires a
     *  current context. */
    const Entry* acquire(const SplatDataPtr& data, float opacityThreshold, std::size_t maxPoints,
                         float sizeClamp, int paintSerial);

    /** Delete entries not used for more than `maxIdlePaints` paints. */
    void purgeIdle(int paintSerial, int maxIdlePaints);

    /** Drop every entry WITHOUT deleting GL objects: the context that owned
     *  them is gone (QOpenGLWidget::initializeGL re-entry). */
    void forgetAll();

    std::size_t bytesInUse() const { return _bytes; }

    /** Number of buffer uploads since creation (lets a test prove a repaint reused the buffer). */
    unsigned long long uploadCount() const { return _uploads; }

private:
    void destroyEntry(Entry& e);

    std::map<const SplatData*, Entry> _entries;
    std::size_t _bytes;
    unsigned long long _uploads;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORT3DSPLATCACHE_H
