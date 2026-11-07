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

#include "VRFilterCore.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>

#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5+

namespace VRFilterCore {

namespace {

const double kPi = 3.14159265358979323846;

// Kernel half-extent = this multiplier * sigma, by Quality (1..3).
double
qualityRadiusMult(int quality)
{
    switch (quality) {
    case 1:  return 2.0;
    case 3:  return 4.0;
    default: return 3.0;
    }
}

// Normalized gaussian kernel, taps = 2*radius+1.
void
buildGaussianKernel(double sigma, int radius, std::vector<double>* kernel)
{
    kernel->assign(2 * radius + 1, 0.0);
    if (sigma <= 1e-6) {
        (*kernel)[radius] = 1.0;
        return;
    }
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double v = std::exp(-(double)(i * i) * inv2s2);
        (*kernel)[i + radius] = v;
        sum += v;
    }
    if (sum > 0.0) {
        const double inv = 1.0 / sum;
        for (size_t i = 0; i < kernel->size(); ++i) (*kernel)[i] *= inv;
    }
}

// Horizontal sigma multiplier at buffer row y (0 = one pole, H-1 = the other).
// 1/cos(latitude), capped so the cost stays bounded near the poles.
double
latitudeSigmaMult(int row, int H, double maxStretch)
{
    if (H <= 1) return 1.0;
    const double v = ((double)row + 0.5) / (double)H; // 0..1 across the latitude range
    const double lat = kPi * (v - 0.5);               // -pi/2 .. pi/2
    double c = std::cos(lat);
    if (c < 0.0) c = 0.0;
    const double floor_ = 1.0 / std::max(1.0, maxStretch);
    if (c < floor_) c = floor_;
    return 1.0 / c;
}

// Horizontal-only 2^level box decimation with wrap. src has H rows of W pixels;
// dst has H rows of smallW = ceil(W/step) pixels.
void
decimateX(const float* src, int W, int H, int nComp, int step, float* dst, int smallW)
{
    for (int y = 0; y < H; ++y) {
        const float* srow = src + (size_t)y * W * nComp;
        float* drow = dst + (size_t)y * smallW * nComp;
        for (int x = 0; x < smallW; ++x) {
            double acc[4] = { 0.0, 0.0, 0.0, 0.0 };
            for (int k = 0; k < step; ++k) {
                int sx = x * step + k;
                sx %= W;
                if (sx < 0) sx += W;
                const float* p = srow + (size_t)sx * nComp;
                for (int c = 0; c < nComp; ++c) acc[c] += p[c];
            }
            float* d = drow + (size_t)x * nComp;
            const double inv = 1.0 / (double)step;
            for (int c = 0; c < nComp; ++c) d[c] = (float)(acc[c] * inv);
        }
    }
}

// Horizontal-only bilinear upsample from smallW back to W (wrap). src/dst row counts H.
void
upsampleX(const float* src, int smallW, int H, int nComp, float* dst, int W)
{
    if (smallW <= 0 || W <= 0) return;
    if (smallW == 1) {
        // degenerate: replicate the single column
        for (int y = 0; y < H; ++y) {
            const float* p = src + (size_t)y * nComp;
            float* drow = dst + (size_t)y * W * nComp;
            for (int x = 0; x < W; ++x) {
                for (int c = 0; c < nComp; ++c) drow[x * nComp + c] = p[c];
            }
        }
        return;
    }
    const double scale = (double)smallW / (double)W;
    for (int y = 0; y < H; ++y) {
        const float* srow = src + (size_t)y * smallW * nComp;
        float* drow = dst + (size_t)y * W * nComp;
        for (int x = 0; x < W; ++x) {
            const double sx = ((double)x + 0.5) * scale - 0.5;
            int x0 = (int)std::floor(sx);
            const double fx = sx - (double)x0;
            int x1 = x0 + 1;
            x0 %= smallW; if (x0 < 0) x0 += smallW;
            x1 %= smallW; if (x1 < 0) x1 += smallW;
            const float* p0 = srow + (size_t)x0 * nComp;
            const float* p1 = srow + (size_t)x1 * nComp;
            float* d = drow + (size_t)x * nComp;
            for (int c = 0; c < nComp; ++c) {
                d[c] = (float)(p0[c] * (1.0 - fx) + p1[c] * fx);
            }
        }
    }
}

// Vertical-only 2^step box decimation with pole clamp. src has H rows; dst has smallH rows.
void
decimateY(const float* src, int W, int H, int nComp, int step, float* dst, int smallH)
{
    const size_t rowLen = (size_t)W * nComp;
    for (int y = 0; y < smallH; ++y) {
        float* drow = dst + (size_t)y * rowLen;
        std::memset(drow, 0, rowLen * sizeof(float));
        for (int k = 0; k < step; ++k) {
            int sy = y * step + k;
            if (sy >= H) sy = H - 1; // pole clamp
            const float* prow = src + (size_t)sy * rowLen;
            for (size_t i = 0; i < rowLen; ++i) drow[i] += prow[i];
        }
        const double inv = 1.0 / (double)step;
        for (size_t i = 0; i < rowLen; ++i) drow[i] = (float)(drow[i] * inv);
    }
}

// Vertical-only bilinear upsample from smallH to H (pole clamp on y, x untouched).
void
upsampleY(const float* src, int smallH, int W, int nComp, float* dst, int H)
{
    if (smallH <= 0 || H <= 0) return;
    const size_t rowLen = (size_t)W * nComp;
    if (smallH == 1) {
        for (int y = 0; y < H; ++y) {
            std::memcpy(dst + (size_t)y * rowLen, src, rowLen * sizeof(float));
        }
        return;
    }
    const double scale = (double)smallH / (double)H;
    for (int y = 0; y < H; ++y) {
        const double sy = ((double)y + 0.5) * scale - 0.5;
        int y0 = (int)std::floor(sy);
        const double fy = sy - (double)y0;
        int y1 = y0 + 1;
        y0 = std::max(0, std::min(smallH - 1, y0));
        y1 = std::max(0, std::min(smallH - 1, y1));
        const float* r0 = src + (size_t)y0 * rowLen;
        const float* r1 = src + (size_t)y1 * rowLen;
        float* drow = dst + (size_t)y * rowLen;
        for (size_t i = 0; i < rowLen; ++i) {
            drow[i] = (float)(r0[i] * (1.0 - fy) + r1[i] * fy);
        }
    }
}

// One row: convolve horizontally with wrap into dst (dst row zeroed by caller).
void
blurRowWrapAccum(const float* srcRow, float* dstRow, int W, int nComp,
                 const std::vector<double>& kernel, int radius)
{
    const int taps = 2 * radius + 1;
    for (int t = 0; t < taps; ++t) {
        const int k = t - radius;
        int base = k % W;
        if (base < 0) base += W;
        const double w = kernel[t];
        for (int x = 0; x < W; ++x) {
            int sx = x + base;
            if (sx >= W) sx -= W;
            const float* p = srcRow + (size_t)sx * nComp;
            float* d = dstRow + (size_t)x * nComp;
            for (int c = 0; c < nComp; ++c) d[c] += (float)(p[c] * w);
        }
    }
}

// Process a run of rows [y0, y1) of src into the same rows of dst with per-row
// horizontal sigma (wrap). level 0 = direct FIR; level > 0 = decimate/blur/upsample.
void
blurHorizontalRun(const float* src, float* dst, int W, int /*H*/, int nComp,
                  int y0, int y1, const std::vector<double>& sigmaRows,
                  double qMult, int level, const std::function<bool()>& aborted)
{
    const int runH = y1 - y0;
    if (runH <= 0 || W <= 0) return;

    if (level <= 0) {
        // Direct per-row FIR at full resolution. All per-row state is local to the
        // lambda: rows run in parallel via QtConcurrent.
        auto processRow = [&](int y) {
            const double s = sigmaRows[y];
            const float* srcRow = src + (size_t)y * W * nComp;
            float* dstRow = dst + (size_t)y * W * nComp;
            if (s < 1e-4) {
                std::memcpy(dstRow, srcRow, (size_t)W * nComp * sizeof(float));
                return;
            }
            int radius = (int)std::ceil(qMult * s);
            if (radius > W - 1) radius = W - 1;
            if (radius < 1) radius = 1;
            std::vector<double> kernel;
            buildGaussianKernel(s, radius, &kernel);
            std::vector<float> accum((size_t)W * nComp, 0.0f);
            blurRowWrapAccum(srcRow, &accum[0], W, nComp, kernel, radius);
            std::memcpy(dstRow, &accum[0], (size_t)W * nComp * sizeof(float));
        };

        // Batched parallel rows (abort lives in the render thread's TLS, so the
        // poll runs here between batches — same pattern as SphericalTransform).
        const int kBatchRows = 64;
        for (int batchY = y0; batchY < y1; batchY += kBatchRows) {
            if (aborted && aborted()) return;
            const int batchEnd = std::min(batchY + kBatchRows, y1);
            std::vector<int> rows;
            rows.reserve(batchEnd - batchY);
            for (int y = batchY; y < batchEnd; ++y) rows.push_back(y);
            QtConcurrent::blockingMap(rows, processRow);
        }
        return;
    }

    // Decimated path: shrink x by 2^level, blur small, upsample back.
    const int step = 1 << level;
    const int smallW = (W + step - 1) / step;
    std::vector<float> smallBuf((size_t)smallW * runH * nComp);
    decimateX(src + (size_t)y0 * W * nComp, W, runH, nComp, step, &smallBuf[0], smallW);

    // Per-row kernel on the small buffer (radius capped at smallW-1).
    auto processSmallRow = [&](int ry) {
        const double s = sigmaRows[y0 + ry] / (double)step;
        const float* srcRow = &smallBuf[(size_t)ry * smallW * nComp];
        float* dstRow = &smallBuf[(size_t)ry * smallW * nComp]; // in-place via accum buffer
        std::vector<double> kernel;
        int radius = (int)std::ceil(qMult * s);
        if (radius > smallW - 1) radius = smallW - 1;
        std::vector<float> accum((size_t)smallW * nComp, 0.0f);
        if (radius >= 1 && s >= 1e-4) {
            buildGaussianKernel(s, radius, &kernel);
            blurRowWrapAccum(srcRow, &accum[0], smallW, nComp, kernel, radius);
            std::memcpy(dstRow, &accum[0], (size_t)smallW * nComp * sizeof(float));
        }
        // radius < 1: the decimation box is the whole blur — leave the row as is.
    };
    const int kBatchRows = 64;
    for (int batchY = 0; batchY < runH; batchY += kBatchRows) {
        if (aborted && aborted()) return;
        const int batchEnd = std::min(batchY + kBatchRows, runH);
        std::vector<int> rows;
        rows.reserve(batchEnd - batchY);
        for (int y = batchY; y < batchEnd; ++y) rows.push_back(y);
        QtConcurrent::blockingMap(rows, processSmallRow);
    }

    upsampleX(&smallBuf[0], smallW, runH, nComp, dst + (size_t)y0 * W * nComp, W);
}

} // anonymous namespace

void
wrapBlur(const float* src, float* dst, int W, int H, int nComp, const BlurParams& p,
         const std::function<bool()>& aborted)
{
    if (W <= 0 || H <= 0 || nComp <= 0) return;
    if (nComp > 4) nComp = 4;

    const double qMult = qualityRadiusMult(p.quality);
    const double sigmaX0 = std::max(0.0, p.sigmaX) * std::max(0.0, p.sizeXFactor);
    const double sigmaY = std::max(0.0, p.sigmaY);

    if (sigmaX0 < 1e-4 && sigmaY < 1e-4) {
        std::memcpy(dst, src, (size_t)W * H * nComp * sizeof(float));
        return;
    }

    // Per-row horizontal sigma (latitude compensation).
    std::vector<double> sigmaRows(H, sigmaX0);
    double maxSigmaX = sigmaX0;
    if (p.latitudeComp && sigmaX0 >= 1e-4) {
        maxSigmaX = 0.0;
        for (int y = 0; y < H; ++y) {
            sigmaRows[y] = sigmaX0 * latitudeSigmaMult(y, H, p.latMaxStretch);
            if (sigmaRows[y] > maxSigmaX) maxSigmaX = sigmaRows[y];
        }
    }

    std::vector<float> tmp((size_t)W * H * nComp);

    // ---- horizontal pass: src -> tmp, decomposed into octave runs of similar sigma
    {
        const int maxLevelW = (W > 16) ? (int)std::floor(std::log2((double)W / 4.0)) : 0;
        int y = 0;
        while (y < H) {
            if (aborted && aborted()) return;
            const double s0 = sigmaRows[y];
            int yEnd = y + 1;
            while (yEnd < H) {
                const double s = sigmaRows[yEnd];
                if (s > 1.5 * s0 || s < s0 / 1.5) break;
                ++yEnd;
            }
            double runMax = 0.0;
            for (int yy = y; yy < yEnd; ++yy) runMax = std::max(runMax, sigmaRows[yy]);

            int level = 0;
            if (runMax > 4.0) {
                level = (int)std::ceil(std::log2(runMax / 4.0));
                if (level > maxLevelW) level = maxLevelW;
                if (level < 1) level = 1;
            }
            blurHorizontalRun(src, &tmp[0], W, H, nComp, y, yEnd, sigmaRows, qMult, level, aborted);
            y = yEnd;
        }
    }

    // ---- vertical pass: tmp -> dst, uniform sigma, pole clamp
    {
        const int maxLevelH = (H > 16) ? (int)std::floor(std::log2((double)H / 4.0)) : 0;
        int level = 0;
        if (sigmaY > 4.0) {
            level = (int)std::ceil(std::log2(sigmaY / 4.0));
            if (level > maxLevelH) level = maxLevelH;
            if (level < 1) level = 1;
        }

        if (level <= 0) {
            std::vector<double> kernel;
            int radius = (int)std::ceil(qMult * sigmaY);
            if (radius > H - 1) radius = H - 1;
            if (radius < 1) radius = 1;
            buildGaussianKernel(sigmaY, radius, &kernel);
            const size_t rowLen = (size_t)W * nComp;

            auto processRow = [&](int y) {
                float* dstRow = dst + (size_t)y * rowLen;
                std::memset(dstRow, 0, rowLen * sizeof(float));
                const int taps = 2 * radius + 1;
                for (int t = 0; t < taps; ++t) {
                    const int k = t - radius;
                    int sy = y + k;
                    if (sy < 0) sy = 0;
                    else if (sy >= H) sy = H - 1;
                    const float* prow = tmp.data() + (size_t)sy * rowLen;
                    const double w = kernel[t];
                    for (size_t i = 0; i < rowLen; ++i) {
                        dstRow[i] += (float)(prow[i] * w);
                    }
                }
            };
            const int kBatchRows = 64;
            for (int batchY = 0; batchY < H; batchY += kBatchRows) {
                if (aborted && aborted()) return;
                const int batchEnd = std::min(batchY + kBatchRows, H);
                std::vector<int> rows;
                rows.reserve(batchEnd - batchY);
                for (int y = batchY; y < batchEnd; ++y) rows.push_back(y);
                QtConcurrent::blockingMap(rows, processRow);
            }
        } else {
            const int step = 1 << level;
            const int smallH = (H + step - 1) / step;
            std::vector<float> smallBuf((size_t)W * smallH * nComp);
            decimateY(&tmp[0], W, H, nComp, step, &smallBuf[0], smallH);

            const double sSmall = sigmaY / (double)step;
            int radius = (int)std::ceil(qMult * sSmall);
            if (radius > smallH - 1) radius = smallH - 1;
            if (radius >= 1 && sSmall >= 1e-4) {
                std::vector<double> kernel;
                buildGaussianKernel(sSmall, radius, &kernel);
                const size_t rowLen = (size_t)W * nComp;
                auto processRow = [&](int y) {
                    float* dstRow = &smallBuf[(size_t)y * rowLen];
                    std::vector<float> accum(rowLen, 0.0f);
                    const int taps = 2 * radius + 1;
                    for (int t = 0; t < taps; ++t) {
                        const int k = t - radius;
                        int sy = y + k;
                        if (sy < 0) sy = 0;
                        else if (sy >= smallH) sy = smallH - 1;
                        const float* prow = &smallBuf[(size_t)sy * rowLen];
                        const double w = kernel[t];
                        for (size_t i = 0; i < rowLen; ++i) accum[i] += (float)(prow[i] * w);
                    }
                    std::memcpy(dstRow, &accum[0], rowLen * sizeof(float));
                };
                const int kBatchRows = 64;
                for (int batchY = 0; batchY < smallH; batchY += kBatchRows) {
                    if (aborted && aborted()) return;
                    const int batchEnd = std::min(batchY + kBatchRows, smallH);
                    std::vector<int> rows;
                    rows.reserve(batchEnd - batchY);
                    for (int y = batchY; y < batchEnd; ++y) rows.push_back(y);
                    QtConcurrent::blockingMap(rows, processRow);
                }
            }
            upsampleY(&smallBuf[0], smallH, W, nComp, dst, H);
        }
    }
}

void
thresholdGain(const float* src, float* dst, size_t count, int nComp,
              float threshold, float softness, float gain)
{
    if (nComp > 4) nComp = 4;
    const bool hasAlpha = (nComp == 4);
    const int nColor = hasAlpha ? 3 : nComp;
    const float denom = (1.0f - threshold) > 1e-6f ? (1.0f - threshold) : 1e-6f;
    const bool smooth = softness > 0.0f;

    for (size_t i = 0; i < count; ++i) {
        const float* in = src + i * nComp;
        float* out = dst + i * nComp;
        for (int c = 0; c < nColor; ++c) {
            float v = (in[c] - threshold) / denom;
            if (v < 0.0f) v = 0.0f;
            else if (v > 1.0f) v = 1.0f;
            if (smooth) {
                const float s = v * v * (3.0f - 2.0f * v);
                v = v * (1.0f - softness) + s * softness;
            }
            out[c] = v * gain;
        }
        if (hasAlpha) out[3] = in[3];
    }
}

void
downsample2xWrap(const float* src, float* dst, int W, int H, int nComp,
                 int* outW, int* outH)
{
    const int ow = std::max(1, W / 2);
    const int oh = std::max(1, H / 2);
    *outW = ow;
    *outH = oh;
    for (int y = 0; y < oh; ++y) {
        const int y0 = std::min(2 * y, H - 1);
        const int y1 = std::min(2 * y + 1, H - 1);
        const float* r0 = src + (size_t)y0 * W * nComp;
        const float* r1 = src + (size_t)y1 * W * nComp;
        for (int x = 0; x < ow; ++x) {
            int x0 = 2 * x % W;
            int x1 = (2 * x + 1) % W;
            const float* p00 = r0 + (size_t)x0 * nComp;
            const float* p01 = r0 + (size_t)x1 * nComp;
            const float* p10 = r1 + (size_t)x0 * nComp;
            const float* p11 = r1 + (size_t)x1 * nComp;
            float* d = dst + ((size_t)y * ow + x) * nComp;
            for (int c = 0; c < nComp; ++c) {
                d[c] = 0.25f * (p00[c] + p01[c] + p10[c] + p11[c]);
            }
        }
    }
}

void
upsampleBilinearWrap(const float* src, int sw, int sh, float* dst, int dw, int dh, int nComp)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    const double sxScale = (double)sw / (double)dw;
    const double syScale = (double)sh / (double)dh;
    for (int y = 0; y < dh; ++y) {
        const double sy = ((double)y + 0.5) * syScale - 0.5;
        int y0 = (int)std::floor(sy);
        const double fy = sy - (double)y0;
        int y1 = y0 + 1;
        y0 = std::max(0, std::min(sh - 1, y0));
        y1 = std::max(0, std::min(sh - 1, y1));
        const float* r0 = src + (size_t)y0 * sw * nComp;
        const float* r1 = src + (size_t)y1 * sw * nComp;
        float* drow = dst + (size_t)y * dw * nComp;
        for (int x = 0; x < dw; ++x) {
            const double sx = ((double)x + 0.5) * sxScale - 0.5;
            int x0 = (int)std::floor(sx);
            const double fx = sx - (double)x0;
            int x1 = x0 + 1;
            x0 %= sw; if (x0 < 0) x0 += sw;
            x1 %= sw; if (x1 < 0) x1 += sw;
            const float* p00 = r0 + (size_t)x0 * nComp;
            const float* p10 = r0 + (size_t)x1 * nComp;
            const float* p01 = r1 + (size_t)x0 * nComp;
            const float* p11 = r1 + (size_t)x1 * nComp;
            float* d = drow + (size_t)x * nComp;
            for (int c = 0; c < nComp; ++c) {
                const float a = p00[c] * (float)(1.0 - fx) + p10[c] * (float)fx;
                const float b = p01[c] * (float)(1.0 - fx) + p11[c] * (float)fx;
                d[c] = a * (float)(1.0 - fy) + b * (float)fy;
            }
        }
    }
}

void
accumulate(float* dst, const float* srcAdd, size_t count, float weight)
{
    for (size_t i = 0; i < count; ++i) {
        dst[i] += srcAdd[i] * weight;
    }
}

void
upsampleAccumulateWrap(const float* src, int sw, int sh, float* dst, int dw, int dh,
                       int nComp, float weight)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    const double sxScale = (double)sw / (double)dw;
    const double syScale = (double)sh / (double)dh;
    for (int y = 0; y < dh; ++y) {
        const double sy = ((double)y + 0.5) * syScale - 0.5;
        int y0 = (int)std::floor(sy);
        const double fy = sy - (double)y0;
        int y1 = y0 + 1;
        y0 = std::max(0, std::min(sh - 1, y0));
        y1 = std::max(0, std::min(sh - 1, y1));
        const float* r0 = src + (size_t)y0 * sw * nComp;
        const float* r1 = src + (size_t)y1 * sw * nComp;
        float* drow = dst + (size_t)y * dw * nComp;
        for (int x = 0; x < dw; ++x) {
            const double sx = ((double)x + 0.5) * sxScale - 0.5;
            int x0 = (int)std::floor(sx);
            const double fx = sx - (double)x0;
            int x1 = x0 + 1;
            x0 %= sw; if (x0 < 0) x0 += sw;
            x1 %= sw; if (x1 < 0) x1 += sw;
            const float* p00 = r0 + (size_t)x0 * nComp;
            const float* p10 = r0 + (size_t)x1 * nComp;
            const float* p01 = r1 + (size_t)x0 * nComp;
            const float* p11 = r1 + (size_t)x1 * nComp;
            float* d = drow + (size_t)x * nComp;
            for (int c = 0; c < nComp; ++c) {
                const float a = p00[c] * (float)(1.0 - fx) + p10[c] * (float)fx;
                const float b = p01[c] * (float)(1.0 - fx) + p11[c] * (float)fx;
                d[c] += weight * (a * (float)(1.0 - fy) + b * (float)fy);
            }
        }
    }
}

std::vector<std::vector<float> >
buildPyramid(const float* src, int W, int H, int nComp,
             int maxLevels, std::vector<std::pair<int, int> >* sizes)
{
    std::vector<std::vector<float> > levels;
    sizes->clear();
    if (W <= 0 || H <= 0 || nComp <= 0) return levels;

    levels.reserve(maxLevels + 1);
    levels.push_back(std::vector<float>((size_t)W * H * nComp));
    std::memcpy(&levels[0][0], src, (size_t)W * H * nComp * sizeof(float));
    sizes->push_back(std::make_pair(W, H));

    int cw = W, ch = H;
    for (int i = 0; i < maxLevels; ++i) {
        if (cw <= 2 || ch <= 2) break;
        int nw = 0, nh = 0;
        levels.push_back(std::vector<float>());
        levels.back().resize((size_t)(cw / 2) * (ch / 2) * nComp);
        downsample2xWrap(&levels[i][0], &levels.back()[0], cw, ch, nComp, &nw, &nh);
        sizes->push_back(std::make_pair(nw, nh));
        cw = nw;
        ch = nh;
    }
    return levels;
}

} // namespace VRFilterCore
