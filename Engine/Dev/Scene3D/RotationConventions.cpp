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

#include "RotationConventions.h"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

namespace RotationConventions {
namespace {

constexpr double DEG2RAD = M_PI / 180.0;
constexpr double RAD2DEG = 180.0 / M_PI;

// Build M = Rz * Ry * Rx column-vector (extrinsic XYZ; Natron's primary
// convention, matches Maya/Blender/Houdini).
void buildExtrinsicXYZ(double rxRad, double ryRad, double rzRad, double m[3][3])
{
    const double cx = std::cos(rxRad), sx = std::sin(rxRad);
    const double cy = std::cos(ryRad), sy = std::sin(ryRad);
    const double cz = std::cos(rzRad), sz = std::sin(rzRad);
    m[0][0] =  cz * cy;
    m[0][1] = -sz * cx + cz * sy * sx;
    m[0][2] =  sz * sx + cz * sy * cx;
    m[1][0] =  sz * cy;
    m[1][1] =  cz * cx + sz * sy * sx;
    m[1][2] = -cz * sx + sz * sy * cx;
    m[2][0] = -sy;
    m[2][1] =  cy * sx;
    m[2][2] =  cy * cx;
}

// Extract (rx, ry, rz) radians from M = Rz*Ry*Rx column-vector.
void extractExtrinsicXYZ(const double m[3][3],
                         double& rxRad, double& ryRad, double& rzRad)
{
    // m[2][0] = -sy
    double sy = -m[2][0];
    sy = std::max(-1.0, std::min(1.0, sy));
    ryRad = std::asin(sy);
    const double cy = std::cos(ryRad);
    if (std::abs(cy) > 1e-6) {
        // m[2][1] = cy*sx, m[2][2] = cy*cx
        rxRad = std::atan2(m[2][1], m[2][2]);
        // m[1][0] = sz*cy, m[0][0] = cz*cy
        rzRad = std::atan2(m[1][0], m[0][0]);
    } else {
        // Gimbal lock: cy ~ 0, so sy ~ +/-1. rx and rz are coupled; pick rz=0.
        // At sy=+1:  m[0][1] = sx, m[1][1] = cx   -> rx = atan2(m[0][1], m[1][1])
        // At sy=-1:  m[0][1] = -sx, m[1][1] = cx  -> rx = atan2(-m[0][1], m[1][1])
        rzRad = 0.0;
        if (sy > 0.0) {
            rxRad = std::atan2(m[0][1], m[1][1]);
        } else {
            rxRad = std::atan2(-m[0][1], m[1][1]);
        }
    }
}

} // anonymous namespace

void compose(double rxDeg, double ryDeg, double rzDeg, double m[3][3])
{
    buildExtrinsicXYZ(rxDeg * DEG2RAD, ryDeg * DEG2RAD, rzDeg * DEG2RAD, m);
}

void composeInverse(double rxDeg, double ryDeg, double rzDeg, double m[3][3])
{
    // M^-1 = M^T for rotation matrices. Build the forward, then transpose.
    double mFwd[3][3];
    buildExtrinsicXYZ(rxDeg * DEG2RAD, ryDeg * DEG2RAD, rzDeg * DEG2RAD, mFwd);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i][j] = mFwd[j][i];
        }
    }
}

void decompose(const double m[3][3],
               double& rxDeg, double& ryDeg, double& rzDeg)
{
    double rxRad, ryRad, rzRad;
    extractExtrinsicXYZ(m, rxRad, ryRad, rzRad);
    rxDeg = rxRad * RAD2DEG;
    ryDeg = ryRad * RAD2DEG;
    rzDeg = rzRad * RAD2DEG;
}

} // namespace RotationConventions

NATRON_NAMESPACE_EXIT
