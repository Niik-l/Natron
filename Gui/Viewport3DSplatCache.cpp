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

#include "Viewport3DSplatCache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

NATRON_NAMESPACE_ENTER

Viewport3DSplatCache::Viewport3DSplatCache()
    : _entries()
    , _bytes(0)
    , _uploads(0)
{
}

Viewport3DSplatCache::~Viewport3DSplatCache()
{
    // No GL calls here: the destructor runs without a guaranteed current
    // context (widget teardown). The context's death frees the buffers.
}

void
Viewport3DSplatCache::destroyEntry(Entry& e)
{
    if (e.vbo) glDeleteBuffers(1, &e.vbo);
    e.vbo = 0;
    e.count = 0;
    _bytes -= std::min(_bytes, e.bytes);
    e.bytes = 0;
}

void
Viewport3DSplatCache::forgetAll()
{
    _entries.clear();
    _bytes = 0;
}

void
Viewport3DSplatCache::purgeIdle(int paintSerial, int maxIdlePaints)
{
    for (std::map<const SplatData*, Entry>::iterator it = _entries.begin(); it != _entries.end(); ) {
        if (it->second.owner.expired() || paintSerial - it->second.lastPaint > maxIdlePaints) {
            destroyEntry(it->second);
            it = _entries.erase(it);
        } else {
            ++it;
        }
    }
}

const Viewport3DSplatCache::Entry*
Viewport3DSplatCache::acquire(const SplatDataPtr& data, float opacityThreshold, std::size_t maxPoints,
                              float sizeClamp, int paintSerial)
{
    if (!data || data->count == 0) return NULL;

    std::map<const SplatData*, Entry>::iterator it = _entries.find( data.get() );
    if ( it != _entries.end() ) {
        Entry& e = it->second;
        const bool same = !e.owner.expired() && e.owner.lock() == data && e.version == data->version
                          && e.opacityThreshold == opacityThreshold && e.maxPoints == maxPoints
                          && e.sizeClamp == sizeClamp;
        if (same) {
            e.lastPaint = paintSerial;
            return e.count > 0 ? &e : NULL;
        }
        destroyEntry(e);
        _entries.erase(it);
    }

    Entry e;
    e.version = data->version;
    e.opacityThreshold = opacityThreshold;
    e.maxPoints = maxPoints;
    e.sizeClamp = sizeClamp;
    e.owner = data;
    e.lastPaint = paintSerial;

    // Display-referred colour: the viewport shows values as they are, and the
    // splat colours are already those of the (sRGB) training photographs.
    std::vector<float> xyz, rgb, dia;
    const std::size_t n = buildSplatPreviewPoints(*data, opacityThreshold, maxPoints, sizeClamp,
                                                  false /*linearColor*/, xyz, rgb, dia);
    if (n > 0) {
        std::vector<unsigned char> buf(n * (std::size_t)kStride);
        for (std::size_t i = 0; i < n; ++i) {
            unsigned char* dst = &buf[i * (std::size_t)kStride];
            std::memcpy(dst, &xyz[i * 3], 12);
            for (int k = 0; k < 3; ++k) {
                const float c = std::min( 1.f, std::max(0.f, rgb[i * 3 + k]) );
                dst[kColorOffset + k] = (unsigned char)(c * 255.f + 0.5f);
            }
            dst[kColorOffset + 3] = 255;
            std::memcpy(dst + kSizeOffset, &dia[i], 4);
        }
        // Free the float staging arrays before the driver makes its own copy.
        std::vector<float>().swap(xyz);
        std::vector<float>().swap(rgb);
        std::vector<float>().swap(dia);

        glGenBuffers(1, &e.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, e.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)buf.size(), buf.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        e.count = (GLsizei)n;
        e.bytes = buf.size();
        _bytes += e.bytes;
        ++_uploads;
        // One line per upload (not per paint): shows in a console launch that
        // repaints reuse the buffer, and what a scene costs in graphics memory.
        std::fprintf(stderr, "[Viewport3D] splat preview uploaded: %llu points, %.1f MB (upload %llu)\n",
                     (unsigned long long)n, (double)e.bytes / (1024.0 * 1024.0), _uploads);
        std::fflush(stderr);
    }
    // An empty result is cached too (count 0), so a scene with nothing above the
    // threshold is not rebuilt on every paint.
    Entry& stored = _entries[data.get()];
    stored = e;
    return stored.count > 0 ? &stored : NULL;
}

NATRON_NAMESPACE_EXIT
