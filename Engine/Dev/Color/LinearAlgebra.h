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

#ifndef NATRON_ENGINE_LINEARALGEBRA_H
#define NATRON_ENGINE_LINEARALGEBRA_H

#include <cmath>
#include <vector>

namespace ColorChartMath {

// 3x3 matrix stored row-major: m[row][col]
struct Mat3 {
    double m[3][3];

    Mat3() {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i][j] = (i == j) ? 1.0 : 0.0;
    }
};

inline Mat3 mat3Multiply(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = 0;
            for (int k = 0; k < 3; ++k)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
        }
    return r;
}

inline Mat3 mat3Transpose(const Mat3& a)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[j][i];
    return r;
}

// Invert a 3x3 matrix using cofactor expansion. Returns false if singular.
inline bool mat3Invert(const Mat3& a, Mat3& inv)
{
    double det = a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1])
               - a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0])
               + a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);

    if (std::fabs(det) < 1e-15) return false;

    double invDet = 1.0 / det;

    inv.m[0][0] =  (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) * invDet;
    inv.m[0][1] = -(a.m[0][1] * a.m[2][2] - a.m[0][2] * a.m[2][1]) * invDet;
    inv.m[0][2] =  (a.m[0][1] * a.m[1][2] - a.m[0][2] * a.m[1][1]) * invDet;
    inv.m[1][0] = -(a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) * invDet;
    inv.m[1][1] =  (a.m[0][0] * a.m[2][2] - a.m[0][2] * a.m[2][0]) * invDet;
    inv.m[1][2] = -(a.m[0][0] * a.m[1][2] - a.m[0][2] * a.m[1][0]) * invDet;
    inv.m[2][0] =  (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]) * invDet;
    inv.m[2][1] = -(a.m[0][0] * a.m[2][1] - a.m[0][1] * a.m[2][0]) * invDet;
    inv.m[2][2] =  (a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0]) * invDet;

    return true;
}

/**
 * Compute the best-fit 3x3 color correction matrix M such that:
 *   target ≈ M * source
 *
 * Uses least-squares via the normal equations:
 *   M = T * S^T * (S * S^T)^(-1)
 *
 * Where S is 3xN source, T is 3xN target.
 *
 * This matches NumPy's: M = transpose(lstsq(src_values, tgt_values)[0])
 * (NumPy's lstsq solves row-vectors, so the transpose is needed.)
 *
 * @param srcRGB  Array of N source RGB triplets (linear)
 * @param tgtRGB  Array of N target RGB triplets (linear)
 * @param n       Number of patches
 * @param result  Output 3x3 matrix
 * @return true on success, false if the system is singular
 */
inline bool computeColorMatrix(const double srcRGB[][3],
                               const double tgtRGB[][3],
                               int n,
                               Mat3& result)
{
    if (n < 3) return false;

    // Build S * S^T (3x3)
    Mat3 SSt;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            SSt.m[i][j] = 0;
            for (int k = 0; k < n; ++k)
                SSt.m[i][j] += srcRGB[k][i] * srcRGB[k][j];
        }

    // Invert S * S^T
    Mat3 SStInv;
    if (!mat3Invert(SSt, SStInv)) return false;

    // Build T * S^T (3x3)
    Mat3 TSt;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            TSt.m[i][j] = 0;
            for (int k = 0; k < n; ++k)
                TSt.m[i][j] += tgtRGB[k][i] * srcRGB[k][j];
        }

    // M = T * S^T * (S * S^T)^(-1)
    result = mat3Multiply(TSt, SStInv);
    return true;
}

} // namespace ColorChartMath

#endif // NATRON_ENGINE_LINEARALGEBRA_H
