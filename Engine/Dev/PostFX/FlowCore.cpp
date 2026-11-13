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

#include "FlowCore.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace FlowCore {

namespace {

struct Level
{
    int w, h;
    std::vector<float> i0, i1;
};

inline int
clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bilinear sample with edge clamp.
inline float
sample(const float* img, int w, int h, float x, float y)
{
    if (x < 0.f) {
        x = 0.f;
    } else if (x > (float)(w - 1)) {
        x = (float)(w - 1);
    }
    if (y < 0.f) {
        y = 0.f;
    } else if (y > (float)(h - 1)) {
        y = (float)(h - 1);
    }
    const int x0 = (int)x, y0 = (int)y;
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = x - (float)x0, ty = y - (float)y0;
    const float* r0 = img + (std::size_t)y0 * w;
    const float* r1 = img + (std::size_t)y1 * w;
    const float a = r0[x0] + (r0[x1] - r0[x0]) * tx;
    const float b = r1[x0] + (r1[x1] - r1[x0]) * tx;

    return a + (b - a) * ty;
}

// Half-size image, each pixel the mean of a 2x2 block (odd sizes repeat the last row / column).
void
downsample(const std::vector<float>& src, int w, int h, std::vector<float>* dst, int* dw, int* dh)
{
    const int nw = (w + 1) / 2, nh = (h + 1) / 2;

    dst->resize( (std::size_t)nw * nh );
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int y = 0; y < nh; ++y) {
        const int ya = std::min(2 * y, h - 1), yb = std::min(2 * y + 1, h - 1);
        const float* ra = &src[(std::size_t)ya * w];
        const float* rb = &src[(std::size_t)yb * w];
        float* o = &(*dst)[(std::size_t)y * nw];
        for (int x = 0; x < nw; ++x) {
            const int xa = std::min(2 * x, w - 1), xb = std::min(2 * x + 1, w - 1);
            o[x] = 0.25f * (ra[xa] + ra[xb] + rb[xa] + rb[xb]);
        }
    }
    *dw = nw;
    *dh = nh;
}

// Resize a flow field to (nw, nh), scaling the vectors by the size ratio.
void
resizeFlow(const std::vector<float>& src, int w, int h, int nw, int nh, std::vector<float>* dst)
{
    dst->resize( (std::size_t)nw * nh * 2 );
    const float sx = (float)w / (float)nw, sy = (float)h / (float)nh;
    const float mx = (float)nw / (float)w, my = (float)nh / (float)h;
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int y = 0; y < nh; ++y) {
        float fy = ( (float)y + 0.5f ) * sy - 0.5f;
        fy = std::max( 0.f, std::min( fy, (float)(h - 1) ) );
        const int y0 = (int)fy, y1 = std::min(y0 + 1, h - 1);
        const float ty = fy - (float)y0;
        for (int x = 0; x < nw; ++x) {
            float fx = ( (float)x + 0.5f ) * sx - 0.5f;
            fx = std::max( 0.f, std::min( fx, (float)(w - 1) ) );
            const int x0 = (int)fx, x1 = std::min(x0 + 1, w - 1);
            const float tx = fx - (float)x0;
            for (int c = 0; c < 2; ++c) {
                const float a = src[( (std::size_t)y0 * w + x0 ) * 2 + c], b = src[( (std::size_t)y0 * w + x1 ) * 2 + c];
                const float d = src[( (std::size_t)y1 * w + x0 ) * 2 + c], e = src[( (std::size_t)y1 * w + x1 ) * 2 + c];
                const float top = a + (b - a) * tx, bot = d + (e - d) * tx;
                (*dst)[( (std::size_t)y * nw + x ) * 2 + c] = ( top + (bot - top) * ty ) * (c == 0 ? mx : my);
            }
        }
    }
}

// One pyramid level: refine `flow` (w * h * 2, holding the coarser level's answer) in place.
void
refineLevel(const Level& L, const Params& params, bool smooth, std::vector<float>* flowIO, std::vector<float>* confOut)
{
    const int w = L.w, h = L.h;
    const float* I0 = &L.i0[0];
    const float* I1 = &L.i1[0];
    std::vector<float>& flow = *flowIO;
    const int P = std::max( 4, std::min( params.patchSize, std::min(w, h) ) );
    const int S = std::max(1, P / 2);
    const int nx = std::max(1, (w - P) / S + 1), ny = std::max(1, (h - P) / S + 1);
    const int maxIter = std::max(1, params.iterations);
    const float n = (float)(P * P);

    // gradients of the template image
    std::vector<float> gx( (std::size_t)w * h ), gy( (std::size_t)w * h );
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int y = 0; y < h; ++y) {
        const int ym = std::max(y - 1, 0), yp = std::min(y + 1, h - 1);
        for (int x = 0; x < w; ++x) {
            const int xm = std::max(x - 1, 0), xp = std::min(x + 1, w - 1);
            gx[(std::size_t)y * w + x] = 0.5f * (I0[(std::size_t)y * w + xp] - I0[(std::size_t)y * w + xm]);
            gy[(std::size_t)y * w + x] = 0.5f * (I0[(std::size_t)yp * w + x] - I0[(std::size_t)ym * w + x]);
        }
    }

    // per patch: displacement and the brightness offset between the two matched patches
    std::vector<float> pu( (std::size_t)nx * ny ), pv( (std::size_t)nx * ny ), pOff( (std::size_t)nx * ny );

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int j = 0; j < ny; ++j) {
        std::vector<float> T(P * P), GX(P * P), GY(P * P);
        for (int i = 0; i < nx; ++i) {
            const int px = std::min(i * S, w - P), py = std::min(j * S, h - P);
            const int cx = px + P / 2, cy = py + P / 2;
            const float u0 = flow[( (std::size_t)cy * w + cx ) * 2], v0 = flow[( (std::size_t)cy * w + cx ) * 2 + 1];
            // template, gradients, and the normal matrix
            float sumT = 0.f, sgx = 0.f, sgy = 0.f, hxx = 0.f, hxy = 0.f, hyy = 0.f;
            for (int yy = 0; yy < P; ++yy) {
                const std::size_t row = (std::size_t)(py + yy) * w + px;
                for (int xx = 0; xx < P; ++xx) {
                    const int k = yy * P + xx;
                    T[k] = I0[row + xx];
                    GX[k] = gx[row + xx];
                    GY[k] = gy[row + xx];
                    sumT += T[k];
                    sgx += GX[k];
                    sgy += GY[k];
                    hxx += GX[k] * GX[k];
                    hxy += GX[k] * GY[k];
                    hyy += GY[k] * GY[k];
                }
            }
            if (params.meanNormalize) {
                hxx -= sgx * sgx / n;
                hxy -= sgx * sgy / n;
                hyy -= sgy * sgy / n;
            }
            const float meanT = sumT / n;
            const float det = hxx * hyy - hxy * hxy;
            float u = u0, v = v0;
            float firstCost = -1.f, cost = 0.f, off = 0.f;
            // a patch without texture in both directions cannot be located: keep the prior
            const bool textured = det > 1e-3f * n * n && (hxx + hyy) > 0.5f * n;
            const int iters = textured ? maxIter : 1;
            for (int it = 0; it < iters; ++it) {
                float sumW = 0.f;
                float bx = 0.f, by = 0.f, ss = 0.f, sr = 0.f;
                // first pass: mean of the warped patch
                float W1[32 * 32];
                for (int yy = 0; yy < P; ++yy) {
                    for (int xx = 0; xx < P; ++xx) {
                        const float s = sample(I1, w, h, (float)(px + xx) + u, (float)(py + yy) + v);
                        W1[yy * P + xx] = s;
                        sumW += s;
                    }
                }
                const float dmean = params.meanNormalize ? (sumW / n - meanT) : 0.f;
                for (int k = 0; k < P * P; ++k) {
                    const float r = W1[k] - T[k] - dmean;
                    bx += r * GX[k];
                    by += r * GY[k];
                    ss += r * r;
                    sr += r;
                }
                (void)sr;
                cost = ss;
                off = dmean;
                if (firstCost < 0.f) {
                    firstCost = ss;
                }
                if (!textured) {
                    break;
                }
                const float du = ( hyy * bx - hxy * by) / det;
                const float dv = (-hxy * bx + hxx * by) / det;
                u -= du;
                v -= dv;
                if (du * du + dv * dv < 0.0004f) {
                    break;
                }
            }
            if (textured) {
                // re-evaluate at the final position; fall back to the prior if the search wandered off
                float sumW = 0.f, ss = 0.f;
                float W1[32 * 32];
                for (int yy = 0; yy < P; ++yy) {
                    for (int xx = 0; xx < P; ++xx) {
                        const float s = sample(I1, w, h, (float)(px + xx) + u, (float)(py + yy) + v);
                        W1[yy * P + xx] = s;
                        sumW += s;
                    }
                }
                const float dmean = params.meanNormalize ? (sumW / n - meanT) : 0.f;
                for (int k = 0; k < P * P; ++k) {
                    const float r = W1[k] - T[k] - dmean;
                    ss += r * r;
                }
                const float moved2 = (u - u0) * (u - u0) + (v - v0) * (v - v0);
                if ( (ss > firstCost) || ( moved2 > (float)(P * P) ) || (u != u) || (v != v) ) {
                    u = u0;
                    v = v0;
                } else {
                    cost = ss;
                    off = dmean;
                }
            }
            (void)cost;
            pu[(std::size_t)j * nx + i] = u;
            pv[(std::size_t)j * nx + i] = v;
            pOff[(std::size_t)j * nx + i] = off;
        }
    }

    // densify: every pixel blends the patches that cover it, trusting those that explain it best
    std::vector<float> dense( (std::size_t)w * h * 2 ), conf( (std::size_t)w * h );
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int y = 0; y < h; ++y) {
        const int j0 = clampi( (y - P + S) / S, 0, ny - 1 ), j1 = clampi(y / S, 0, ny - 1);
        for (int x = 0; x < w; ++x) {
            const int i0 = clampi( (x - P + S) / S, 0, nx - 1 ), i1 = clampi(x / S, 0, nx - 1);
            const float ref = I0[(std::size_t)y * w + x];
            float su = 0.f, sv = 0.f, sw = 0.f;
            int cnt = 0;
            for (int j = std::min(j0, j1); j <= j1; ++j) {
                for (int i = std::min(i0, i1); i <= i1; ++i) {
                    const std::size_t k = (std::size_t)j * nx + i;
                    const float d = sample(I1, w, h, (float)x + pu[k], (float)y + pv[k]) - ref - pOff[k];
                    const float wgt = 1.f / std::max(1.f, std::fabs(d) * 0.5f);
                    su += wgt * pu[k];
                    sv += wgt * pv[k];
                    sw += wgt;
                    ++cnt;
                }
            }
            dense[( (std::size_t)y * w + x ) * 2] = su / sw;
            dense[( (std::size_t)y * w + x ) * 2 + 1] = sv / sw;
            conf[(std::size_t)y * w + x] = sw / (float)cnt;
        }
    }

    // smooth the field with its neighbours, weighted by confidence and by similarity in the image
    // (so the smoothing stops at object edges)
    const int R = smooth ? (int)std::floor(params.smoothness * 6. + 0.5) : 0;
    if (R > 0) {
        const float sigS = 0.5f * (float)R + 0.5f, sigC = 12.f;
        std::vector<float> gs(R + 1);
        for (int k = 0; k <= R; ++k) {
            gs[k] = std::exp( -(float)(k * k) / (2.f * sigS * sigS) );
        }
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float ref = I0[(std::size_t)y * w + x];
                float su = 0.f, sv = 0.f, sw = 0.f;
                for (int dy = -R; dy <= R; ++dy) {
                    const int yy = clampi(y + dy, 0, h - 1);
                    for (int dx = -R; dx <= R; ++dx) {
                        const int xx = clampi(x + dx, 0, w - 1);
                        const std::size_t q = (std::size_t)yy * w + xx;
                        const float dc = I0[q] - ref;
                        const float wgt = gs[dx < 0 ? -dx : dx] * gs[dy < 0 ? -dy : dy]
                                          * std::exp( -dc * dc / (2.f * sigC * sigC) ) * (conf[q] * conf[q] + 1e-4f);
                        su += wgt * dense[q * 2];
                        sv += wgt * dense[q * 2 + 1];
                        sw += wgt;
                    }
                }
                flow[( (std::size_t)y * w + x ) * 2] = su / sw;
                flow[( (std::size_t)y * w + x ) * 2 + 1] = sv / sw;
            }
        }
    } else {
        flow.swap(dense);
    }
    if (confOut) {
        confOut->swap(conf);
    }
} // refineLevel

// Variational refinement of one level (Brox et al. 2004, as used by Dense Inverse Search): moves
// the field to a minimum of a brightness term + a gradient term + a smoothness term. A patch that
// only sees one straight edge can measure motion across that edge but not along it; here the
// smoothness term supplies the missing component from the neighbours, and flat areas are filled in.
void
variationalRefine(const Level& L, const Params& params, std::vector<float>* flowIO)
{
    const int w = L.w, h = L.h;
    const std::size_t N = (std::size_t)w * h;
    const float alpha = (float)(params.refineAlpha * params.smoothness * 2.);

    if ( (w < 4) || (h < 4) || (alpha <= 0.f) || (params.refineIterations < 1) ) {
        return;
    }
    const float delta = (float)params.refineDelta, gamma = (float)params.refineGamma;
    const float zeta2 = (float)(params.refineZeta * params.refineZeta), eps2 = 1e-6f, omega = 1.6f;
    const float* I0 = &L.i0[0];
    const float* I1 = &L.i1[0];
    std::vector<float>& flow = *flowIO;
    std::vector<float> Iz(N), Ix(N), Iy(N), Ixx(N), Ixy(N), Iyy(N), Ixz(N), Iyz(N), valid(N);
    std::vector<float> du(N, 0.f), dv(N, 0.f), ws(N), a11(N), a12(N), a22(N), b1(N), b2(N);
    {
        std::vector<float> avg(N);
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t k = (std::size_t)y * w + x;
                const float sx = (float)x + flow[2 * k], sy = (float)y + flow[2 * k + 1];
                const float s = sample(I1, w, h, sx, sy);
                valid[k] = (sx >= 0.f && sy >= 0.f && sx <= (float)(w - 1) && sy <= (float)(h - 1) ) ? 1.f : 0.f;
                avg[k] = 0.5f * (s + I0[k]);
                Iz[k] = s - I0[k];
            }
        }
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int y = 0; y < h; ++y) {
            const int ym = std::max(y - 1, 0), yp = std::min(y + 1, h - 1);
            for (int x = 0; x < w; ++x) {
                const int xm = std::max(x - 1, 0), xp = std::min(x + 1, w - 1);
                const std::size_t k = (std::size_t)y * w + x;
                Ix[k] = 0.5f * (avg[(std::size_t)y * w + xp] - avg[(std::size_t)y * w + xm]);
                Iy[k] = 0.5f * (avg[(std::size_t)yp * w + x] - avg[(std::size_t)ym * w + x]);
                Ixz[k] = 0.5f * (Iz[(std::size_t)y * w + xp] - Iz[(std::size_t)y * w + xm]);
                Iyz[k] = 0.5f * (Iz[(std::size_t)yp * w + x] - Iz[(std::size_t)ym * w + x]);
            }
        }
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int y = 0; y < h; ++y) {
            const int ym = std::max(y - 1, 0), yp = std::min(y + 1, h - 1);
            for (int x = 0; x < w; ++x) {
                const int xm = std::max(x - 1, 0), xp = std::min(x + 1, w - 1);
                const std::size_t k = (std::size_t)y * w + x;
                Ixx[k] = 0.5f * (Ix[(std::size_t)y * w + xp] - Ix[(std::size_t)y * w + xm]);
                Ixy[k] = 0.5f * (Ix[(std::size_t)yp * w + x] - Ix[(std::size_t)ym * w + x]);
                Iyy[k] = 0.5f * (Iy[(std::size_t)yp * w + x] - Iy[(std::size_t)ym * w + x]);
            }
        }
    }

    for (int fp = 0; fp < params.refineIterations; ++fp) {
        // data terms and smoothness weights, with the robust weights from the current estimate
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int y = 0; y < h; ++y) {
            const int yp = std::min(y + 1, h - 1);
            for (int x = 0; x < w; ++x) {
                const int xp = std::min(x + 1, w - 1);
                const std::size_t k = (std::size_t)y * w + x;
                const std::size_t kx = (std::size_t)y * w + xp, ky = (std::size_t)yp * w + x;
                float A11 = 0.01f, A12 = 0.f, A22 = 0.01f, B1 = 0.f, B2 = 0.f;
                if (valid[k] > 0.f) {
                    if (delta > 0.f) {
                        const float nrm = Ix[k] * Ix[k] + Iy[k] * Iy[k] + zeta2;
                        const float r = Iz[k] + Ix[k] * du[k] + Iy[k] * dv[k];
                        const float wgt = delta / std::sqrt(r * r / nrm + eps2) / nrm;
                        A11 += wgt * Ix[k] * Ix[k];
                        A12 += wgt * Ix[k] * Iy[k];
                        A22 += wgt * Iy[k] * Iy[k];
                        B1 -= wgt * Iz[k] * Ix[k];
                        B2 -= wgt * Iz[k] * Iy[k];
                    }
                    if (gamma > 0.f) {
                        const float n1 = Ixx[k] * Ixx[k] + Ixy[k] * Ixy[k] + zeta2;
                        const float n2 = Iyy[k] * Iyy[k] + Ixy[k] * Ixy[k] + zeta2;
                        const float r1 = Ixz[k] + Ixx[k] * du[k] + Ixy[k] * dv[k];
                        const float r2 = Iyz[k] + Ixy[k] * du[k] + Iyy[k] * dv[k];
                        const float wgt = gamma / std::sqrt(r1 * r1 / n1 + r2 * r2 / n2 + eps2);
                        A11 += wgt * (Ixx[k] * Ixx[k] / n1 + Ixy[k] * Ixy[k] / n2);
                        A12 += wgt * (Ixx[k] * Ixy[k] / n1 + Ixy[k] * Iyy[k] / n2);
                        A22 += wgt * (Ixy[k] * Ixy[k] / n1 + Iyy[k] * Iyy[k] / n2);
                        B1 -= wgt * (Ixx[k] * Ixz[k] / n1 + Ixy[k] * Iyz[k] / n2);
                        B2 -= wgt * (Ixy[k] * Ixz[k] / n1 + Iyy[k] * Iyz[k] / n2);
                    }
                }
                a11[k] = A11; a12[k] = A12; a22[k] = A22; b1[k] = B1; b2[k] = B2;
                const float ux = flow[2 * kx] + du[kx] - flow[2 * k] - du[k];
                const float uy = flow[2 * ky] + du[ky] - flow[2 * k] - du[k];
                const float vx = flow[2 * kx + 1] + dv[kx] - flow[2 * k + 1] - dv[k];
                const float vy = flow[2 * ky + 1] + dv[ky] - flow[2 * k + 1] - dv[k];
                ws[k] = alpha / std::sqrt(ux * ux + uy * uy + vx * vx + vy * vy + eps2);
            }
        }
        // successive over-relaxation, red / black so the rows can run in parallel
        for (int it = 0; it < params.refineSor * 2; ++it) {
            const int colour = it & 1;
#ifdef _OPENMP
#pragma omp parallel for
#endif
            for (int y = 0; y < h; ++y) {
                for (int x = (y + colour) & 1; x < w; x += 2) {
                    const std::size_t k = (std::size_t)y * w + x;
                    float sw = 0.f, su = 0.f, sv = 0.f;
                    const float u0 = flow[2 * k], v0 = flow[2 * k + 1];
#define NATRON_FLOW_NB(q) { const float wn = 0.5f * (ws[k] + ws[q]); sw += wn; \
                            su += wn * (flow[2 * (q)] + du[q] - u0); sv += wn * (flow[2 * (q) + 1] + dv[q] - v0); }
                    if (x > 0) NATRON_FLOW_NB(k - 1)
                    if (x < w - 1) NATRON_FLOW_NB(k + 1)
                    if (y > 0) NATRON_FLOW_NB(k - w)
                    if (y < h - 1) NATRON_FLOW_NB(k + w)
#undef NATRON_FLOW_NB
                    const float nu = (b1[k] + su - a12[k] * dv[k]) / (a11[k] + sw);
                    du[k] += omega * (nu - du[k]);
                    const float nv = (b2[k] + sv - a12[k] * du[k]) / (a22[k] + sw);
                    dv[k] += omega * (nv - dv[k]);
                }
            }
        }
    }
    for (std::size_t k = 0; k < N; ++k) {
        if ( (du[k] == du[k]) && (dv[k] == dv[k]) ) {
            flow[2 * k] += du[k];
            flow[2 * k + 1] += dv[k];
        }
    }
} // variationalRefine

} // namespace

float
matchEncode(float v)
{
    if ( !(v > 0.f) ) {
        return 0.f;
    }
    if (v <= 1.f) {
        return 255.f * std::pow(v, 0.4545f);
    }

    return 255.f * ( 1.f + 0.4545f * std::log(v) );
}

bool
computeFlow(const float* i0,
            const float* i1,
            int width,
            int height,
            const Params& paramsIn,
            std::vector<float>* flow,
            std::vector<float>* confidence,
            const volatile bool* abort)
{
    if ( !i0 || !i1 || !flow || (width < 1) || (height < 1) ) {
        return false;
    }
    Params params = paramsIn;
    params.patchSize = clampi(params.patchSize, 4, 32);
    params.smoothness = std::max( 0., std::min(1., params.smoothness) );
    const double detail = std::max( 1. / 64., std::min(1., params.vectorDetail) );

    // pyramid: level 0 is the full image, each further level half the size
    std::vector<Level> pyr(1);
    pyr[0].w = width;
    pyr[0].h = height;
    pyr[0].i0.assign(i0, i0 + (std::size_t)width * height);
    pyr[0].i1.assign(i1, i1 + (std::size_t)width * height);
    while (std::min(pyr.back().w, pyr.back().h) >= 4 * params.patchSize && pyr.size() < 12) {
        Level next;
        const Level& prev = pyr.back();
        int w2, h2;
        downsample(prev.i0, prev.w, prev.h, &next.i0, &next.w, &next.h);
        downsample(prev.i1, prev.w, prev.h, &next.i1, &w2, &h2);
        pyr.push_back(next);
    }
    // the finest level actually solved, from Vector Detail
    int finest = (int)std::floor(std::log(1. / detail) / std::log(2.) + 0.5);
    finest = clampi(finest, 0, (int)pyr.size() - 1);

    std::vector<float> f, conf;
    int fw = 0, fh = 0;
    for (int l = (int)pyr.size() - 1; l >= finest; --l) {
        if (abort && *abort) {
            return false;
        }
        const Level& L = pyr[l];
        if ( f.empty() ) {
            f.assign( (std::size_t)L.w * L.h * 2, 0.f );
        } else {
            std::vector<float> up;
            resizeFlow(f, fw, fh, L.w, L.h, &up);
            f.swap(up);
        }
        fw = L.w;
        fh = L.h;
        refineLevel(L, params, params.patchSmooth, &f, &conf);
        variationalRefine(L, params, &f);
    }
    if (abort && *abort) {
        return false;
    }
    if ( (fw != width) || (fh != height) ) {
        resizeFlow(f, fw, fh, width, height, flow);
        if (confidence) {
            // resizeFlow works on 2-channel data: widen, resize, narrow
            std::vector<float> c2( (std::size_t)fw * fh * 2 ), c2o;
            for (std::size_t k = 0; k < (std::size_t)fw * fh; ++k) {
                c2[2 * k] = conf[k] / ( (float)width / (float)fw );
                c2[2 * k + 1] = 0.f;
            }
            resizeFlow(c2, fw, fh, width, height, &c2o);
            confidence->resize( (std::size_t)width * height );
            for (std::size_t k = 0; k < (std::size_t)width * height; ++k) {
                (*confidence)[k] = c2o[2 * k];
            }
        }
    } else {
        flow->swap(f);
        if (confidence) {
            confidence->swap(conf);
        }
    }

    // Where nothing changed between the two frames there is no motion to report. Without this an
    // empty background (a CG element over black) inherits the motion of whatever moves in front
    // of it, because a flat area has nothing of its own to track. Only unchanged areas that reach
    // the frame edge count: an unchanged island inside a moving object (a black square of a
    // texture, in shadow) keeps the motion of its surroundings.
    {
        const int R = 2;
        const float tol = 0.02f; // in 0..255 match units: only identical areas qualify, not a slowly
                                 // changing highlight inside a moving object
        std::vector<unsigned char> changed( (std::size_t)width * height ), rowMax( (std::size_t)width * height );
        std::vector<unsigned char> still( (std::size_t)width * height );
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t k = (std::size_t)y * width + x;
                changed[k] = std::fabs(i0[k] - i1[k]) > tol;
            }
            for (int x = 0; x < width; ++x) {
                unsigned char m = 0;
                for (int xx = std::max(0, x - R); xx <= std::min(width - 1, x + R); ++xx) {
                    m |= changed[(std::size_t)y * width + xx];
                }
                rowMax[(std::size_t)y * width + x] = m;
            }
        }
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned char m = 0;
                for (int yy = std::max(0, y - R); yy <= std::min(height - 1, y + R); ++yy) {
                    m |= rowMax[(std::size_t)yy * width + x];
                }
                still[(std::size_t)y * width + x] = !m;
            }
        }
        // flood from the frame edge through the unchanged pixels
        std::vector<std::size_t> stack;
        for (int x = 0; x < width; ++x) {
            stack.push_back( (std::size_t)x );
            stack.push_back( (std::size_t)(height - 1) * width + x );
        }
        for (int y = 0; y < height; ++y) {
            stack.push_back( (std::size_t)y * width );
            stack.push_back( (std::size_t)y * width + width - 1 );
        }
        while ( !stack.empty() ) {
            const std::size_t k = stack.back();
            stack.pop_back();
            if (still[k] != 1) {
                continue;
            }
            still[k] = 2;
            (*flow)[2 * k] = 0.f;
            (*flow)[2 * k + 1] = 0.f;
            const int x = (int)(k % width), y = (int)(k / width);
            if (x > 0) stack.push_back(k - 1);
            if (x < width - 1) stack.push_back(k + 1);
            if (y > 0) stack.push_back(k - width);
            if (y < height - 1) stack.push_back(k + width);
        }
    }

    return true;
} // computeFlow

} // namespace FlowCore
