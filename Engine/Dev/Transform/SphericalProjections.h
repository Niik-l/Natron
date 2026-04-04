/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef NATRON_ENGINE_SPHERICALPROJECTIONS_H
#define NATRON_ENGINE_SPHERICALPROJECTIONS_H

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace SphericalProjections {

enum ProjectionType {
    eProjectionLatLong = 0,
    eProjectionRectilinear,
    eProjectionFisheyeEquidistant,
    eProjectionFisheyeEquisolid,
    eProjectionFisheyeStereographic,
    eProjectionFisheyeOrthographic,
    eProjectionMirrorBall,
    eProjectionCubemap,
    eProjectionCount
};

// Cubemap packing layouts
enum CubemapPacking {
    eCubemapLLCross = 0,  // 4:3 cross layout
    eCubemap6x1,          // 6:1 horizontal strip
    eCubemap3x2,          // 3:2 grid
    eCubemapPackingCount
};

// ---- Rotation ----

// Build rotation matrix from pan (X), tilt (Y), roll (Z) in degrees.
// Nuke convention: x=pan (left/right), y=tilt (up/down, positive=up), z=roll.
// Order: roll * tilt * pan (ZYX).
inline void buildRotationMatrix(double panDeg, double tiltDeg, double rollDeg,
                                double m[9])
{
    double p = panDeg  * (M_PI / 180.0);
    double t = tiltDeg * (M_PI / 180.0);
    double r = rollDeg * (M_PI / 180.0);

    double cp = std::cos(p), sp = std::sin(p);
    double ct = std::cos(t), st = std::sin(t);
    double cr = std::cos(r), sr = std::sin(r);

    // ZXY rotation order
    m[0] = cr * cp - sr * st * sp;
    m[1] = -sr * ct;
    m[2] = cr * sp + sr * st * cp;
    m[3] = sr * cp + cr * st * sp;
    m[4] = cr * ct;
    m[5] = sr * sp - cr * st * cp;
    m[6] = -ct * sp;
    m[7] = st;
    m[8] = ct * cp;
}

// Build rotation matrix from Euler angles with specified rotation order.
// order: 0=XYZ, 1=XZY, 2=YXZ, 3=YZX, 4=ZXY, 5=ZYX
inline void buildRotationMatrixEuler(double xDeg, double yDeg, double zDeg,
                                     int order, double m[9])
{
    double ax = xDeg * (M_PI / 180.0);
    double ay = yDeg * (M_PI / 180.0);
    double az = zDeg * (M_PI / 180.0);

    // Individual axis rotation matrices (row-major 3x3)
    double cx = std::cos(ax), sx = std::sin(ax);
    double cy = std::cos(ay), sy = std::sin(ay);
    double cz = std::cos(az), sz = std::sin(az);

    // Rx
    double rx[9] = {1, 0, 0,  0, cx, -sx,  0, sx, cx};
    // Ry
    double ry[9] = {cy, 0, sy,  0, 1, 0,  -sy, 0, cy};
    // Rz
    double rz[9] = {cz, -sz, 0,  sz, cz, 0,  0, 0, 1};

    // Multiply two 3x3 matrices: out = a * b
    auto mul = [](const double a[9], const double b[9], double out[9]) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                out[i*3+j] = 0;
                for (int k = 0; k < 3; ++k)
                    out[i*3+j] += a[i*3+k] * b[k*3+j];
            }
    };

    double tmp[9];
    switch (order) {
    case 0: mul(rx, ry, tmp); mul(tmp, rz, m); break; // XYZ
    case 1: mul(rx, rz, tmp); mul(tmp, ry, m); break; // XZY
    case 2: mul(ry, rx, tmp); mul(tmp, rz, m); break; // YXZ
    case 3: mul(ry, rz, tmp); mul(tmp, rx, m); break; // YZX
    case 4: mul(rz, rx, tmp); mul(tmp, ry, m); break; // ZXY (default)
    case 5: mul(rz, ry, tmp); mul(tmp, rx, m); break; // ZYX
    default: mul(rz, rx, tmp); mul(tmp, ry, m); break; // ZXY fallback
    }
}

// Build rotation from Look mode: direction x (pan), direction y (tilt), angle (roll).
// Equivalent to Pan-Tilt-Roll with the same values.
inline void buildRotationMatrixLook(double dirXDeg, double dirYDeg, double angleDeg, double m[9])
{
    buildRotationMatrix(dirXDeg, dirYDeg, angleDeg, m);
}

inline void applyRotation(const double m[9],
                          double x, double y, double z,
                          double& ox, double& oy, double& oz)
{
    ox = m[0] * x + m[1] * y + m[2] * z;
    oy = m[3] * x + m[4] * y + m[5] * z;
    oz = m[6] * x + m[7] * y + m[8] * z;
}

// ---- LatLong (Equirectangular) ----
// u,v in [0,1]. u=0 is left edge, v=0 is top.
// Direction: right-handed, Y-up. Front is +Z.

inline void directionFromLatLong(double u, double v,
                                 double& x, double& y, double& z)
{
    double theta = (u - 0.5) * 2.0 * M_PI;  // longitude: [-pi, pi]
    double phi   = (0.5 - v) * M_PI;         // latitude:  [-pi/2, pi/2]
    double cp = std::cos(phi);
    x = cp * std::sin(theta);
    y = std::sin(phi);
    z = cp * std::cos(theta);
}

inline bool latLongFromDirection(double x, double y, double z,
                                 double& u, double& v)
{
    double theta = std::atan2(x, z);         // longitude
    double phi   = std::asin(std::max(-1.0, std::min(1.0, y)));  // latitude
    u = theta / (2.0 * M_PI) + 0.5;
    v = 0.5 - phi / M_PI;
    return true;
}

// ---- Rectilinear (Pinhole / Gnomonic) ----
// Models a standard perspective camera.
// focal in mm, sensorW/sensorH in mm.

inline void directionFromRectilinear(double u, double v,
                                     double focal, double sensorW, double sensorH,
                                     double& x, double& y, double& z)
{
    // u,v in [0,1]. Center of image is (0.5, 0.5).
    double px = (u - 0.5) * sensorW / focal;
    double py = (0.5 - v) * sensorH / focal;  // Y-up
    double len = std::sqrt(px * px + py * py + 1.0);
    x = px / len;
    y = py / len;
    z = 1.0 / len;
}

inline bool rectilinearFromDirection(double x, double y, double z,
                                     double focal, double sensorW, double sensorH,
                                     double& u, double& v)
{
    if (z <= 1e-10) return false;  // behind camera
    double px = x / z;
    double py = y / z;
    u = px * focal / sensorW + 0.5;
    v = 0.5 - py * focal / sensorH;
    return true;
}

// ---- Fisheye Projections ----
// All fisheye models map incidence angle theta to radial distance R.
// theta = angle from optical axis (z-axis).

inline void directionFromFisheye(double u, double v,
                                 double focal, double sensorW, double sensorH,
                                 ProjectionType model,
                                 double& x, double& y, double& z)
{
    // Pixel offset from center in mm
    double px = (u - 0.5) * sensorW;
    double py = (0.5 - v) * sensorH;
    double R = std::sqrt(px * px + py * py);

    if (R < 1e-12) {
        x = 0; y = 0; z = 1;
        return;
    }

    // R → theta (invert the fisheye formula)
    double theta = 0;
    switch (model) {
    case eProjectionFisheyeEquidistant:
        // R = f * theta
        theta = R / focal;
        break;
    case eProjectionFisheyeEquisolid:
        // R = 2f * sin(theta/2)
        { double val = R / (2.0 * focal);
          val = std::max(-1.0, std::min(1.0, val));
          theta = 2.0 * std::asin(val); }
        break;
    case eProjectionFisheyeStereographic:
        // R = 2f * tan(theta/2)
        theta = 2.0 * std::atan(R / (2.0 * focal));
        break;
    case eProjectionFisheyeOrthographic:
        // R = f * sin(theta)
        { double val = R / focal;
          val = std::max(-1.0, std::min(1.0, val));
          theta = std::asin(val); }
        break;
    default:
        theta = R / focal;
        break;
    }

    if (theta > M_PI) {
        x = 0; y = 0; z = -1;
        return;
    }

    double sinT = std::sin(theta);
    double cosT = std::cos(theta);
    // Direction in camera space
    x = sinT * (px / R);
    y = sinT * (py / R);
    z = cosT;
}

inline bool fisheyeFromDirection(double x, double y, double z,
                                 double focal, double sensorW, double sensorH,
                                 ProjectionType model,
                                 double& u, double& v)
{
    double theta = std::acos(std::max(-1.0, std::min(1.0, z)));
    if (theta < 1e-12) {
        u = 0.5;
        v = 0.5;
        return true;
    }

    // theta → R
    double R = 0;
    switch (model) {
    case eProjectionFisheyeEquidistant:
        R = focal * theta;
        break;
    case eProjectionFisheyeEquisolid:
        R = 2.0 * focal * std::sin(theta / 2.0);
        break;
    case eProjectionFisheyeStereographic:
        R = 2.0 * focal * std::tan(theta / 2.0);
        break;
    case eProjectionFisheyeOrthographic:
        if (theta >= M_PI / 2.0) return false;  // 180 degree limit
        R = focal * std::sin(theta);
        break;
    default:
        R = focal * theta;
        break;
    }

    double sinT = std::sin(theta);
    double px = R * (x / sinT);
    double py = R * (y / sinT);

    u = px / sensorW + 0.5;
    v = 0.5 - py / sensorH;
    return true;
}

// ---- Mirror Ball (Chrome Sphere) ----

inline void directionFromMirrorBall(double u, double v,
                                    double& x, double& y, double& z)
{
    // u,v in [0,1], mirror ball is a circle inscribed in the square
    double mx = 2.0 * u - 1.0;  // [-1, 1]
    double my = 1.0 - 2.0 * v;  // [-1, 1], Y-up
    double r2 = mx * mx + my * my;

    if (r2 > 0.9995) {  // Slightly inside the circle to avoid edge artifacts
        x = 0; y = 0; z = -1;
        return;
    }

    double mz = std::sqrt(1.0 - r2);
    // Mirror ball captures reflection direction: D = 2N(N·V) - V
    // With orthographic view V = (0,0,1), N = (mx, my, mz):
    // D = 2*(mx, my, mz)*mz - (0,0,1) = (2*mx*mz, 2*my*mz, 2*mz*mz - 1)
    x = 2.0 * mx * mz;
    y = 2.0 * my * mz;
    z = 2.0 * mz * mz - 1.0;
}

inline bool mirrorBallFromDirection(double x, double y, double z,
                                    double& u, double& v)
{
    // Invert: find normal N on sphere such that reflection of (0,0,1) = (x,y,z)
    // N = normalize(D + V) = normalize(x, y, z+1)
    double denom = std::sqrt(x * x + y * y + (z + 1.0) * (z + 1.0));
    if (denom < 1e-6) return false;  // back of sphere

    double nx = x / denom;
    double ny = y / denom;

    // Check if the resulting point is inside the circle
    if (nx * nx + ny * ny > 0.9995) return false;

    u = nx * 0.5 + 0.5;
    v = 0.5 - ny * 0.5;
    return true;
}

// ---- Cubemap ----
// 6 faces: +X, -X, +Y, -Y, +Z, -Z (indices 0-5)
// Each face is a 90-degree rectilinear projection.

// Determine which cube face a direction vector hits, and return face-local UV in [0,1].
inline int cubeFaceFromDirection(double x, double y, double z,
                                  double& fu, double& fv)
{
    double ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    int face;
    double sc, tc, ma; // s-coord, t-coord, major axis

    // Nuke convention: Y-up, right-handed
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (x > 0) { face = 0; sc = -z; tc = -y; }    // +X: right=-Z, down=-Y
        else        { face = 1; sc =  z; tc = -y; }    // -X: right=+Z, down=-Y
    } else if (ay >= ax && ay >= az) {
        ma = ay;
        if (y > 0) { face = 2; sc =  x; tc =  z; }    // +Y (top): right=+X, down=+Z
        else        { face = 3; sc =  x; tc = -z; }    // -Y (bottom): right=+X, down=-Z
    } else {
        ma = az;
        if (z > 0) { face = 4; sc =  x; tc = -y; }    // +Z (front): right=+X, down=-Y
        else        { face = 5; sc = -x; tc = -y; }    // -Z (back): right=-X, down=-Y
    }

    fu = (sc / ma + 1.0) * 0.5;
    fv = (tc / ma + 1.0) * 0.5;
    return face;
}

// Given a cube face index and face-local UV, return the 3D direction.
inline void directionFromCubeFace(int face, double fu, double fv,
                                  double& x, double& y, double& z)
{
    double sc = fu * 2.0 - 1.0;
    double tc = fv * 2.0 - 1.0;

    // Must be inverse of cubeFaceFromDirection axis mapping
    switch (face) {
    case 0: x =  1; y = -tc; z = -sc; break;  // +X
    case 1: x = -1; y = -tc; z =  sc; break;  // -X
    case 2: x = sc; y =  1;  z =  tc; break;  // +Y (top)
    case 3: x = sc; y = -1;  z = -tc; break;  // -Y (bottom)
    case 4: x = sc; y = -tc; z =  1;  break;  // +Z (front)
    case 5: x = -sc; y = -tc; z = -1; break;  // -Z (back)
    }

    double len = std::sqrt(x*x + y*y + z*z);
    x /= len; y /= len; z /= len;
}

// LL-Cross layout (4 cols x 3 rows):
//          [+Y]
//   [-X] [+Z] [+X] [-Z]
//          [-Y]
// Face positions (col, row): +X=(2,1), -X=(0,1), +Y=(1,2), -Y=(1,0), +Z=(1,1), -Z=(3,1)

static const int kLLCrossFaceCol[] = {2, 0, 1, 1, 1, 3};  // +X,-X,+Y,-Y,+Z,-Z
static const int kLLCrossFaceRow[] = {1, 1, 2, 0, 1, 1};

// 6x1 layout: faces left-to-right: +X, -X, +Y, -Y, +Z, -Z
static const int k6x1FaceCol[] = {0, 1, 2, 3, 4, 5};
static const int k6x1FaceRow[] = {0, 0, 0, 0, 0, 0};

// 3x2 layout (3 cols x 2 rows):
// Row 1: +X, -X, +Y
// Row 0: -Y, +Z, -Z
static const int k3x2FaceCol[] = {0, 1, 2, 0, 1, 2};
static const int k3x2FaceRow[] = {1, 1, 1, 0, 0, 0};

// Convert cubemap pixel (u,v in [0,1] over full image) to direction
inline void directionFromCubemap(double u, double v, CubemapPacking packing,
                                 double& x, double& y, double& z)
{
    int numCols, numRows;
    const int* faceCols;
    const int* faceRows;

    switch (packing) {
    case eCubemapLLCross: numCols = 4; numRows = 3; faceCols = kLLCrossFaceCol; faceRows = kLLCrossFaceRow; break;
    case eCubemap6x1:     numCols = 6; numRows = 1; faceCols = k6x1FaceCol;     faceRows = k6x1FaceRow;     break;
    case eCubemap3x2:     numCols = 3; numRows = 2; faceCols = k3x2FaceCol;     faceRows = k3x2FaceRow;     break;
    default:              numCols = 4; numRows = 3; faceCols = kLLCrossFaceCol; faceRows = kLLCrossFaceRow; break;
    }

    // Which cell are we in?
    double cellW = 1.0 / numCols;
    double cellH = 1.0 / numRows;
    int col = (int)(u / cellW);
    int row = (int)(v / cellH);
    if (col >= numCols) col = numCols - 1;
    if (row >= numRows) row = numRows - 1;

    // Find which face this cell belongs to
    int face = -1;
    for (int f = 0; f < 6; ++f) {
        if (faceCols[f] == col && faceRows[f] == row) {
            face = f;
            break;
        }
    }

    if (face < 0) {
        // Empty cell in cross layout — return invalid direction
        x = 0; y = 0; z = 0;
        return;
    }

    // Face-local UV
    double fu = (u - col * cellW) / cellW;
    double fv = (v - row * cellH) / cellH;

    directionFromCubeFace(face, fu, fv, x, y, z);
}

// Convert direction to cubemap pixel (u,v in [0,1] over full image)
inline bool cubemapFromDirection(double x, double y, double z, CubemapPacking packing,
                                 double& u, double& v)
{
    int numCols, numRows;
    const int* faceCols;
    const int* faceRows;

    switch (packing) {
    case eCubemapLLCross: numCols = 4; numRows = 3; faceCols = kLLCrossFaceCol; faceRows = kLLCrossFaceRow; break;
    case eCubemap6x1:     numCols = 6; numRows = 1; faceCols = k6x1FaceCol;     faceRows = k6x1FaceRow;     break;
    case eCubemap3x2:     numCols = 3; numRows = 2; faceCols = k3x2FaceCol;     faceRows = k3x2FaceRow;     break;
    default:              numCols = 4; numRows = 3; faceCols = kLLCrossFaceCol; faceRows = kLLCrossFaceRow; break;
    }

    double fu, fv;
    int face = cubeFaceFromDirection(x, y, z, fu, fv);

    int col = faceCols[face];
    int row = faceRows[face];

    double cellW = 1.0 / numCols;
    double cellH = 1.0 / numRows;

    u = (col + fu) * cellW;
    v = (row + fv) * cellH;
    return true;
}

// ---- Unified dispatch ----

inline void pixelToDirection(ProjectionType proj,
                             double u, double v,
                             double focal, double sensorW, double sensorH,
                             CubemapPacking cubePacking,
                             double& x, double& y, double& z)
{
    switch (proj) {
    case eProjectionLatLong:
        directionFromLatLong(u, v, x, y, z);
        break;
    case eProjectionRectilinear:
        directionFromRectilinear(u, v, focal, sensorW, sensorH, x, y, z);
        break;
    case eProjectionCubemap:
        directionFromCubemap(u, v, cubePacking, x, y, z);
        break;
    case eProjectionFisheyeEquidistant:
    case eProjectionFisheyeEquisolid:
    case eProjectionFisheyeStereographic:
    case eProjectionFisheyeOrthographic:
        directionFromFisheye(u, v, focal, sensorW, sensorH, proj, x, y, z);
        break;
    case eProjectionMirrorBall:
        directionFromMirrorBall(u, v, x, y, z);
        break;
    default:
        directionFromLatLong(u, v, x, y, z);
        break;
    }
}

inline bool directionToPixel(ProjectionType proj,
                             double x, double y, double z,
                             double focal, double sensorW, double sensorH,
                             CubemapPacking cubePacking,
                             double& u, double& v)
{
    switch (proj) {
    case eProjectionLatLong:
        return latLongFromDirection(x, y, z, u, v);
    case eProjectionRectilinear:
        return rectilinearFromDirection(x, y, z, focal, sensorW, sensorH, u, v);
    case eProjectionCubemap:
        return cubemapFromDirection(x, y, z, cubePacking, u, v);
    case eProjectionFisheyeEquidistant:
    case eProjectionFisheyeEquisolid:
    case eProjectionFisheyeStereographic:
    case eProjectionFisheyeOrthographic:
        return fisheyeFromDirection(x, y, z, focal, sensorW, sensorH, proj, u, v);
    case eProjectionMirrorBall:
        return mirrorBallFromDirection(x, y, z, u, v);
    default:
        return latLongFromDirection(x, y, z, u, v);
    }
}

// ---- Bilinear sampling ----

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Sample RGBA from a float image with bilinear interpolation.
// pixels: float* to pixel data, stride: bytes per row, bounds: image rect
inline void sampleBilinear(const float* pixels, int stride,
                           int x1, int y1, int x2, int y2,
                           double fx, double fy,
                           bool blackOutside,
                           float out[4])
{
    // Convert to pixel-relative coordinates
    double px = fx - 0.5;
    double py = fy - 0.5;

    int ix = (int)std::floor(px);
    int iy = (int)std::floor(py);
    double dx = px - ix;
    double dy = py - iy;

    auto getPixel = [&](int cx, int cy) -> const float* {
        if (cx < x1 || cx >= x2 || cy < y1 || cy >= y2) {
            return nullptr;
        }
        return (const float*)((const char*)pixels + (cy - y1) * stride) + (cx - x1) * 4;
    };

    const float* p00 = getPixel(ix,     iy);
    const float* p10 = getPixel(ix + 1, iy);
    const float* p01 = getPixel(ix,     iy + 1);
    const float* p11 = getPixel(ix + 1, iy + 1);

    static const float zero[4] = {0, 0, 0, 0};
    if (blackOutside) {
        if (!p00) p00 = zero;
        if (!p10) p10 = zero;
        if (!p01) p01 = zero;
        if (!p11) p11 = zero;
    } else {
        // Clamp: use nearest valid pixel
        if (!p00) p00 = p10 ? p10 : (p01 ? p01 : (p11 ? p11 : zero));
        if (!p10) p10 = p00;
        if (!p01) p01 = p00;
        if (!p11) p11 = p10;
    }

    float w00 = (float)((1.0 - dx) * (1.0 - dy));
    float w10 = (float)(dx * (1.0 - dy));
    float w01 = (float)((1.0 - dx) * dy);
    float w11 = (float)(dx * dy);

    for (int c = 0; c < 4; ++c) {
        out[c] = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
    }
}

// ---- Bicubic (Catmull-Rom) sampling ----

// Catmull-Rom cubic basis: continuous, C1, passes through control points
inline float cubicWeight(float t)
{
    float at = std::fabs(t);
    if (at <= 1.0f) {
        return (1.5f * at - 2.5f) * at * at + 1.0f;
    } else if (at < 2.0f) {
        return ((-0.5f * at + 2.5f) * at - 4.0f) * at + 2.0f;
    }
    return 0.0f;
}

// Mitchell-Netravali (B=1/3, C=1/3) — good balance of blur/ringing
inline float mitchellWeight(float t)
{
    float at = std::fabs(t);
    if (at < 1.0f) {
        return ((12.0f - 9.0f/3.0f - 6.0f/3.0f) * at * at * at
              + (-18.0f + 12.0f/3.0f + 6.0f/3.0f) * at * at + (6.0f - 2.0f/3.0f)) / 6.0f;
    } else if (at < 2.0f) {
        return ((-1.0f/3.0f - 6.0f/3.0f) * at * at * at
              + (6.0f/3.0f + 30.0f/3.0f) * at * at
              + (-12.0f/3.0f - 48.0f/3.0f) * at
              + (8.0f/3.0f + 24.0f/3.0f)) / 6.0f;
    }
    return 0.0f;
}

} // namespace SphericalProjections

#endif // NATRON_ENGINE_SPHERICALPROJECTIONS_H
