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

#ifndef NATRON_ENGINE_MASKMESHBUILDER_H
#define NATRON_ENGINE_MASKMESHBUILDER_H

// MaskMeshBuilder: shape contours -> MeshData, shared by MaskToGeo (roto
// Beziers) and TraceMask (traced contours). Extracted from MaskToGeo.cpp so
// both nodes emit identical topology (union fill, nonzero winding, CCW-from-
// outside winding, per-corner UVs).

#include "../../../Global/Macros.h"

#include <utility>
#include <vector>

#include "Dev/Scene3D/MeshData.h"

NATRON_NAMESPACE_ENTER

namespace MaskMesh {

/// One closed contour in canonical (mask/format) pixel coordinates.
/// `op` is a Natron::MergingFunctionEnum value (or -1 = default plus);
/// minus/XOR contours subtract coverage from the union.
struct Contour
{
    std::vector<std::pair<double, double> > points;
    int op; // -1 = plus
};

/// True for roto operators that remove coverage instead of adding it.
bool isSubtractiveOperator(int op);

/// Drop consecutive points closer than 0.02px + collapse the closure point.
/// Returns false if fewer than 3 points remain.
bool cleanContour(std::vector<std::pair<double, double> >* contour);

/**
 * @brief Build the flat (depth == 0) or extruded mesh from closed contours
 * in canonical pixel space.
 *
 *   s          world units per mask pixel (format height -> World Scale)
 *   (cx, cy)   project format center
 *   formatW/H  project format size (UV normalization)
 *   depth      0 = flat fill (all contours unioned in one tessellation,
 *              minus/XOR punch holes); > 0 = extruded families: each
 *              additive contour is a slab with bottom cap, side walls and
 *              offset top cap, and the subtractive contours it contains
 *              punch through both caps (their walls line the hole).
 *              Overlapping additive shapes stay separate slabs.
 *   bevel      world-unit 45-degree chamfer on the extruded edges (needs
 *              depth > 2*bevel; ignored on the flat fill). Caps keep the
 *              silhouette, the middle wall band insets by the bevel, hole
 *              edges countersink.
 */
MeshDataPtr buildMesh(const std::vector<Contour>& contours,
                      double s, double cx, double cy,
                      double formatW, double formatH,
                      double depth, double bevel);

} // namespace MaskMesh

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_MASKMESHBUILDER_H
