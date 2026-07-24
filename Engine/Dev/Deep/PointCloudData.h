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

#ifndef NATRON_ENGINE_POINTCLOUDDATA_H
#define NATRON_ENGINE_POINTCLOUDDATA_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <cmath>
#include <memory>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief Stores point cloud data for 3D viewport rendering.
 *
 * Each point has 6 floats: x, y, z, r, g, b.
 * Data is stored as an interleaved flat buffer suitable for
 * direct upload to a GL VBO.
 */
class PointCloudData
{
public:

    PointCloudData() : _numPoints(0)
    {
        for (int i = 0; i < 3; ++i) { _bboxMin[i] = 0; _bboxMax[i] = 0; }
    }

    void clear()
    {
        _data.clear();
        _ids.clear();
        _numPoints = 0;
    }

    void reserve(std::size_t numPoints)
    {
        _data.reserve(numPoints * 6);
    }

    void addPoint(float x, float y, float z, float r, float g, float b)
    {
        _data.push_back(x);
        _data.push_back(y);
        _data.push_back(z);
        _data.push_back(r);
        _data.push_back(g);
        _data.push_back(b);
        ++_numPoints;
    }

    /** Add a point carrying a source ID (e.g. the deep sample index it came
     *  from). Producers must be consistent: either every point of a cloud has
     *  an ID or none does — hasIds() relies on the counts matching. */
    void addPoint(float x, float y, float z, float r, float g, float b,
                  unsigned long long id)
    {
        addPoint(x, y, z, r, g, b);
        _ids.push_back(id);
    }

    /** True when every point carries a source ID (see addPoint with id).
     *  Filters (Blast) propagate IDs so a chain can map surviving points
     *  back to the original source (e.g. deep samples). */
    bool hasIds() const { return _numPoints > 0 && _ids.size() == _numPoints; }

    unsigned long long idAt(std::size_t i) const { return _ids[i]; }

    std::size_t numPoints() const { return _numPoints; }

    /// Returns pointer to interleaved [x,y,z,r,g,b, x,y,z,r,g,b, ...] data
    const float* data() const { return _data.empty() ? nullptr : _data.data(); }

    /// Stride in bytes between consecutive points (6 floats)
    static int stride() { return 6 * sizeof(float); }

    /// Size in bytes of the entire buffer
    std::size_t sizeInBytes() const { return _data.size() * sizeof(float); }

    /// Set bounding box
    void setBounds(float minX, float minY, float minZ,
                   float maxX, float maxY, float maxZ)
    {
        _bboxMin[0] = minX; _bboxMin[1] = minY; _bboxMin[2] = minZ;
        _bboxMax[0] = maxX; _bboxMax[1] = maxY; _bboxMax[2] = maxZ;
    }

    /// Get bounding box center
    void getCenter(float& cx, float& cy, float& cz) const
    {
        cx = (_bboxMin[0] + _bboxMax[0]) * 0.5f;
        cy = (_bboxMin[1] + _bboxMax[1]) * 0.5f;
        cz = (_bboxMin[2] + _bboxMax[2]) * 0.5f;
    }

    /// Get bounding box radius (half-diagonal)
    float getRadius() const
    {
        float dx = _bboxMax[0] - _bboxMin[0];
        float dy = _bboxMax[1] - _bboxMin[1];
        float dz = _bboxMax[2] - _bboxMin[2];
        return 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    }

private:

    std::vector<float> _data;
    std::vector<unsigned long long> _ids; // optional per-point source IDs
    std::size_t _numPoints;
    float _bboxMin[3];
    float _bboxMax[3];
};

typedef std::shared_ptr<PointCloudData> PointCloudDataPtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_POINTCLOUDDATA_H
