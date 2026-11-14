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

#ifndef NATRON_ENGINE_SPLATDATA_H
#define NATRON_ENGINE_SPLATDATA_H

#include "../../../Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief A 3D Gaussian splat scene (3DGS) held in memory, as read by ReadSplats.
 *
 * Structure of arrays, one entry per splat, all in the file's own space (the
 * node's transform is NOT applied). Values are decoded from the file's training
 * encoding once at load, so consumers never repeat it:
 *   - scale    = exp(file scale)              (standard deviation per local axis)
 *   - opacity  = sigmoid(file opacity)        (0..1)
 *   - rotation = normalised quaternion, order w, x, y, z
 * Colour stays as spherical-harmonic coefficients, because that is what a splat
 * renderer needs: dc holds band 0; shRest holds bands 1..shDegree, per splat as
 * all red coefficients, then all green, then all blue (the file's own order).
 * A display colour for band 0 alone is 0.5 + 0.28209479 * dc, in the colourspace
 * of the photographs the scene was trained on (normally sRGB).
 *
 * A plain coloured point-cloud PLY loads into the same structure with
 * hasGaussians == false: scale, rotation and opacity are then filled with
 * neutral values and dc is derived from the file's red / green / blue.
 *
 * Instances are immutable once published: a new load makes a new object with a
 * new version, so a consumer may cache on (address, version).
 */
struct SplatData
{
    std::size_t count = 0;

    std::vector<float> pos;       // 3 per splat
    std::vector<float> scale;     // 3 per splat, linear
    std::vector<float> rot;       // 4 per splat, w x y z, normalised
    std::vector<float> opacity;   // 1 per splat, 0..1
    std::vector<float> dc;        // 3 per splat, SH band 0 coefficients
    std::vector<float> shRest;    // 3 * shRestPerChannel() per splat (empty when shDegree == 0)

    int shDegree = 0;             // bands kept in memory (0..3)
    int fileShDegree = 0;         // bands present in the file (0..3)
    bool hasGaussians = false;    // false = plain point cloud (no scale / rotation / opacity in the file)

    float bboxMin[3] = {0.f, 0.f, 0.f};
    float bboxMax[3] = {0.f, 0.f, 0.f};
    float medianSize = 0.f;       // median over splats of the geometric mean of the three scales
    std::size_t droppedNonFinite = 0;  // splats skipped at load because a value was NaN / infinite

    unsigned long long version = 0;    // unique per load, never reused within a session

    int shRestPerChannel() const { return (shDegree + 1) * (shDegree + 1) - 1; }

    /** Geometric mean of the three scales of splat i: its size as one number. */
    float sizeOf(std::size_t i) const;

    std::size_t bytesInMemory() const
    {
        return ( pos.capacity() + scale.capacity() + rot.capacity() + opacity.capacity()
                 + dc.capacity() + shRest.capacity() ) * sizeof(float);
    }
};

typedef std::shared_ptr<const SplatData> SplatDataPtr;

/**
 * @brief Read a 3DGS binary PLY (or a plain binary point-cloud PLY).
 *
 * @param path           UTF-8 file path
 * @param maxShDegree    highest SH band to keep (0..3); higher bands in the file are skipped
 * @param error          receives a message for the user when the read fails
 * @return the scene, or null on failure
 */
SplatDataPtr readSplatPly(const std::string& path, int maxShDegree, std::string* error);

/**
 * @brief Build the point preview of a splat scene: one coloured, sized point per splat.
 *
 * Shared by the 3D viewport and ScanlineRender so both show the same thing.
 * Splats below `opacityThreshold` are left out. When more than `maxPoints`
 * remain (0 = no limit), an even subset is taken across the file. The point
 * diameter is twice the splat's size, limited to `sizeClamp` times the scene's
 * median (0 = no limit), which keeps the few huge background splats of a real
 * scan from covering the picture.
 *
 * @param linearColor   true: convert the colour from sRGB to linear (renders);
 *                      false: leave it display-referred (the viewport)
 * @param outXyz        3 floats per point
 * @param outRgb        3 floats per point, not clamped above 1
 * @param outDiameter   1 float per point, in the file's units
 * @return number of points written
 */
std::size_t buildSplatPreviewPoints(const SplatData& data,
                                    float opacityThreshold,
                                    std::size_t maxPoints,
                                    float sizeClamp,
                                    bool linearColor,
                                    std::vector<float>& outXyz,
                                    std::vector<float>& outRgb,
                                    std::vector<float>& outDiameter);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SPLATDATA_H
