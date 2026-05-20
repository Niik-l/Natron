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

#include "CameraMath.h"

#include <cmath>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace CameraMath {

void
computeFovs(double focalLength, double hAperture, double vAperture,
            double& fovHrad, double& fovVrad)
{
    // Standard pinhole FOV. Each axis is independent of the other.
    // Guard against pathological zero focal length.
    const double f = (focalLength > 1e-6) ? focalLength : 1e-6;
    fovHrad = 2.0 * std::atan(hAperture / (2.0 * f));
    fovVrad = 2.0 * std::atan(vAperture / (2.0 * f));
}

void
composeProjectionMatrix(double focalLength, double hAperture, double vAperture,
                        float nearZ, float farZ, float out[16])
{
    double fovHrad, fovVrad;
    computeFovs(focalLength, hAperture, vAperture, fovHrad, fovVrad);

    // 1 / tan(fov/2) per axis. Each derived from its own aperture; the image
    // aspect ratio is NOT involved (and must not be — that was the bug).
    const float fX = 1.0f / std::tan((float)fovHrad * 0.5f);
    const float fY = 1.0f / std::tan((float)fovVrad * 0.5f);

    std::memset(out, 0, 16 * sizeof(float));
    out[0]  = fX;
    out[5]  = fY;
    out[10] = (farZ + nearZ) / (nearZ - farZ);
    out[11] = -1.0f;
    out[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
}

} // namespace CameraMath

NATRON_NAMESPACE_EXIT
