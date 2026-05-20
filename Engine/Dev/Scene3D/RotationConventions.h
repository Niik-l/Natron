/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_ROTATIONCONVENTIONS_H
#define NATRON_ENGINE_ROTATIONCONVENTIONS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

NATRON_NAMESPACE_ENTER

// One place to define the rotation convention used everywhere in the Dev/ 3D
// system: **extrinsic XYZ, column-vector** —
//
//     M = Rz(rz) * Ry(ry) * Rx(rx)
//
// applied to a column vector as v' = M * v. This matches the default
// rotateOrder of Maya, Blender, Houdini, and the on-disk matrix layout in
// Alembic, so cameras and transforms imported from those DCCs orient
// correctly in Natron.
//
// All angles are in DEGREES. All matrices are 3x3 row-major (m[row][col])
// with column-vector semantics. Translation and scale are handled at the
// call sites — these helpers do rotation only.
//
// Pass 2 (per-camera rotateOrder knob) will extend these functions with an
// `Order` enum parameter; every call site only needs to read the knob.

namespace RotationConventions {

// Build the rotation matrix M = Rz * Ry * Rx (column-vector, extrinsic XYZ).
void compose(double rxDeg, double ryDeg, double rzDeg, double m[3][3]);

// Build the inverse, M^-1 = Rx(-rx) * Ry(-ry) * Rz(-rz), for camera view matrices.
void composeInverse(double rxDeg, double ryDeg, double rzDeg, double m[3][3]);

// Extract (rx, ry, rz) in degrees from a column-vector M = Rz*Ry*Rx matrix.
// Includes gimbal-lock fallback at sy = +/-1.
void decompose(const double m[3][3],
               double& rxDeg, double& ryDeg, double& rzDeg);

// NOTE on the gizmo boundary: turns out ImGuizmo's matMul does standard A*B
// on row-major storage, so its rot[0]*rot[1]*rot[2] = Rx_rv*Ry_rv*Rz_rv =
// Rz_col*Ry_col*Rx_col — the same extrinsic XYZ convention used here.
// No conversion is needed when feeding our knob values to ImGuizmo or reading
// them back. The audit's "ImGuizmo uses intrinsic XYZ" claim was wrong.

} // namespace RotationConventions

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_ROTATIONCONVENTIONS_H
