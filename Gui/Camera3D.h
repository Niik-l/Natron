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

#ifndef NATRON_GUI_CAMERA3D_H
#define NATRON_GUI_CAMERA3D_H

#include "Global/Macros.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief Orbit camera for the 3D viewport.
 *
 * Provides perspective projection and view matrices computed from
 * orbit parameters (theta, phi, radius) around a target point.
 *
 * Matrices are stored as column-major float[16] arrays suitable
 * for direct use with glLoadMatrixf / glMultMatrixf or as shader uniforms.
 */
class Camera3D
{
public:

    Camera3D()
        : _theta(0.5f)
        , _phi(0.4f)
        , _radius(8.0f)
        , _targetX(0.0f)
        , _targetY(0.0f)
        , _targetZ(0.0f)
        , _fovY(45.0f)
        , _nearClip(0.1f)
        , _farClip(500.0f)
        , _aspect(1.0f)
    {
    }

    // --- Orbit controls ---

    void orbit(float dTheta, float dPhi)
    {
        _theta += dTheta;
        _phi += dPhi;
        // Clamp phi to avoid gimbal flip
        _phi = std::max(-1.5f, std::min(1.5f, _phi));
    }

    void pan(float dx, float dy)
    {
        float scale = _radius * 0.002f;
        float rightX = cosf(_theta);
        float rightZ = -sinf(_theta);
        _targetX -= dx * scale * rightX;
        _targetZ -= dx * scale * rightZ;
        _targetY += dy * scale;
    }

    void dolly(float delta)
    {
        _radius *= (1.0f - delta * 0.1f);
        _radius = std::max(0.5f, std::min(200.0f, _radius));
    }

    void reset()
    {
        _theta = 0.5f;
        _phi = 0.4f;
        _radius = 8.0f;
        _targetX = _targetY = _targetZ = 0.0f;
    }

    // --- Setters ---

    void setAspect(float aspect) { _aspect = aspect; }
    void setFovY(float fov) { _fovY = fov; }
    void setClipPlanes(float nearZ, float farZ) { _nearClip = nearZ; _farClip = farZ; }

    void setTarget(float x, float y, float z) { _targetX = x; _targetY = y; _targetZ = z; }
    void setOrbit(float theta, float phi, float radius)
    {
        _theta = theta; _phi = phi; _radius = radius;
    }

    // --- Getters ---

    void getEyePosition(float& ex, float& ey, float& ez) const
    {
        ex = _targetX + _radius * cosf(_phi) * sinf(_theta);
        ey = _targetY + _radius * sinf(_phi);
        ez = _targetZ + _radius * cosf(_phi) * cosf(_theta);
    }

    void getTarget(float& tx, float& ty, float& tz) const
    {
        tx = _targetX; ty = _targetY; tz = _targetZ;
    }

    float getRadius() const { return _radius; }
    float getTheta() const { return _theta; }
    float getPhi() const { return _phi; }

    // --- Matrix computation ---

    /**
     * @brief Compute the projection matrix (column-major float[16]).
     */
    void getProjectionMatrix(float m[16]) const
    {
        float f = 1.0f / tanf(_fovY * 0.5f * (float)M_PI / 180.0f);
        for (int i = 0; i < 16; ++i) m[i] = 0.0f;
        m[0]  = f / _aspect;
        m[5]  = f;
        m[10] = (_farClip + _nearClip) / (_nearClip - _farClip);
        m[11] = -1.0f;
        m[14] = (2.0f * _farClip * _nearClip) / (_nearClip - _farClip);
    }

    /**
     * @brief Compute the view (lookAt) matrix (column-major float[16]).
     */
    void getViewMatrix(float mat[16]) const
    {
        float ex, ey, ez;
        getEyePosition(ex, ey, ez);

        float fx = _targetX - ex, fy = _targetY - ey, fz = _targetZ - ez;
        float len = sqrtf(fx*fx + fy*fy + fz*fz);
        if (len > 0.0001f) { fx /= len; fy /= len; fz /= len; }

        // up = (0,1,0)
        float ux = 0, uy = 1, uz = 0;

        // s = f x up
        float sx = fy*uz - fz*uy;
        float sy = fz*ux - fx*uz;
        float sz = fx*uy - fy*ux;
        len = sqrtf(sx*sx + sy*sy + sz*sz);
        if (len > 0.0001f) { sx /= len; sy /= len; sz /= len; }

        // u = s x f
        ux = sy*fz - sz*fy;
        uy = sz*fx - sx*fz;
        uz = sx*fy - sy*fx;

        // Column-major rotation
        float r[16] = {
            sx, ux, -fx, 0,
            sy, uy, -fy, 0,
            sz, uz, -fz, 0,
            0,  0,   0,  1
        };

        // Translation
        float tx = -(sx*ex + sy*ey + sz*ez);
        float ty = -(ux*ex + uy*ey + uz*ez);
        float tz = -(-fx*ex + -fy*ey + -fz*ez);

        for (int i = 0; i < 16; ++i) mat[i] = r[i];
        mat[12] = tx;
        mat[13] = ty;
        mat[14] = tz;
    }

    /**
     * @brief Apply projection and view to the current GL fixed-function pipeline.
     */
    void applyGL() const;

private:

    float _theta;     // horizontal orbit angle (radians)
    float _phi;       // vertical orbit angle (radians)
    float _radius;    // distance from target

    float _targetX, _targetY, _targetZ;

    float _fovY;      // field of view (degrees)
    float _nearClip;
    float _farClip;
    float _aspect;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_CAMERA3D_H
