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

#ifndef NATRON_ENGINE_CAMERAMATH_H
#define NATRON_ENGINE_CAMERAMATH_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

NATRON_NAMESPACE_ENTER

// One place to define Natron's perspective-projection math for Dev/ camera
// consumers. Independently honors horizontal and vertical aperture — does NOT
// derive one axis from image aspect (which is the bug present in the original
// per-renderer copies of this code: both Scanline and Project3D used a single
// FOV from hAperture and forced Y scaling = X scaling × image aspect, which is
// only correct when the sensor aspect happens to equal the image aspect).
//
// All apertures and focal lengths must be in the same unit (millimeters in
// Natron's UI). The ratios cancel.
//
// Future extensions (sensor shift / lens offset / pixel-aspect-ratio) land here.

namespace CameraMath {

// Compute horizontal and vertical FOV (in RADIANS) from focal length and the
// two sensor apertures.
void computeFovs(double focalLength,
                 double hAperture, double vAperture,
                 double& fovHrad, double& fovVrad);

// Build an OpenGL-style column-major float[16] perspective projection matrix
// using the independent fovH/fovV derived from both apertures. Does NOT use
// image aspect — by intent.
void composeProjectionMatrix(double focalLength,
                             double hAperture, double vAperture,
                             float nearZ, float farZ,
                             float out[16]);

} // namespace CameraMath

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CAMERAMATH_H
