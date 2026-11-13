// Standalone accuracy check for Engine/Dev/PostFX/FlowCore: synthetic image pairs whose true
// motion is known exactly. Build and run (MSYS2 MINGW64 or Linux), from the repo root:
//   g++ -O2 -fopenmp -I Engine/Dev/PostFX tools/flow-tests/flow_test.cpp Engine/Dev/PostFX/FlowCore.cpp -o flow_test && ./flow_test
// Prints the mean end-point error (pixels) per case; exits non-zero if a case is over its limit.
#include "FlowCore.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
const int W = 640, H = 360;

// a smooth, non-repeating texture defined everywhere, so shifted copies are exact
double hashd(int x, int y, int s)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)s * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ( (h ^ (h >> 16)) & 0xffffff ) / (double)0xffffff;
}

double vnoise(double x, double y, int s)
{
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    double tx = x - xi, ty = y - yi;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
    const double a = hashd(xi, yi, s), b = hashd(xi + 1, yi, s), c = hashd(xi, yi + 1, s), d = hashd(xi + 1, yi + 1, s);
    return a + (b - a) * tx + (c - a + (a - b - c + d) * tx) * ty;
}

double tex(double x, double y, int seed)
{
    double v = 0., amp = 0.5, f = 1. / 48.;
    for (int o = 0; o < 5; ++o) {
        v += amp * vnoise(x * f, y * f, seed + o);
        amp *= 0.55; f *= 2.1;
    }
    return v; // roughly 0..1
}

struct Case
{
    const char* name;
    double limit;
    // true flow at (x, y), and whether the pixel is scored
    void (*truth)(double x, double y, double* u, double* v, bool* scored);
    bool object; // a foreground square on top of the background
    double gain; // brightness change of frame 1
};

void tSmall(double, double, double* u, double* v, bool* s) { *u = 3.3; *v = -1.7; *s = true; }
void tLarge(double, double, double* u, double* v, bool* s) { *u = 26.; *v = 11.; *s = true; }
void tZoomRot(double x, double y, double* u, double* v, bool* s)
{
    const double cx = W / 2., cy = H / 2., a = 0.02, z = 1.03;
    const double dx = x - cx, dy = y - cy;
    *u = z * (std::cos(a) * dx - std::sin(a) * dy) - dx;
    *v = z * (std::sin(a) * dx + std::cos(a) * dy) - dy;
    *s = true;
}
const double OX = 250., OY = 130., OS = 110., OU = 14., OV = -6.; // moving square
void tObject(double x, double y, double* u, double* v, bool* s)
{
    const bool in = x >= OX && x < OX + OS && y >= OY && y < OY + OS;
    *u = in ? OU : 1.5; *v = in ? OV : 0.5;
    // do not score the band the square uncovers / the few pixels at its edge
    const bool nearEdge = x >= OX - 4 && x < OX + OS + 4 && y >= OY - 4 && y < OY + OS + 4
                          && !(x >= OX + 4 && x < OX + OS - 4 && y >= OY + 4 && y < OY + OS - 4);
    *s = !nearEdge;
}

// a smooth-shaded sphere without any texture, moving over an empty black background: the CG case
// (ScanlineRender straight into VectorGenerator). The background must come out still.
const double SX = 300., SY = 180., SR = 120., SU = 15., SV = 0.;
double sphereShade(double x, double y)
{
    const double dx = (x - SX) / SR, dy = (y - SY) / SR, r2 = dx * dx + dy * dy;
    if (r2 >= 1.) {
        return 0.;
    }
    const double nz = std::sqrt(1. - r2);
    return 0.05 + 0.9 * std::max(0., 0.45 * dx - 0.5 * dy + 0.74 * nz);
}
void tSphere(double x, double y, double* u, double* v, bool* s)
{
    const double d0 = std::hypot(x - SX, y - SY), d1 = std::hypot(x - SX - SU, y - SY - SV);
    const bool in = d0 < SR - 6.;
    const bool far = d0 > SR + 40. && d1 > SR + 40.;
    *u = in ? SU : 0.; *v = in ? SV : 0.;
    *s = in || far;
}

double frame0(const Case& c, double x, double y)
{
    if (c.truth == tSphere) {
        return sphereShade(x, y);
    }
    if (c.object && x >= OX && x < OX + OS && y >= OY && y < OY + OS) {
        return tex(x * 1.7, y * 1.7, 90);
    }
    return tex(x, y, 7);
}

// frame 1 by inverting the motion: fixed-point iteration on the smooth fields
double frame1(const Case& c, double x, double y)
{
    if (c.truth == tSphere) {
        return sphereShade(x - SU, y - SV);
    }
    if (c.object) {
        const double sx = x - OU, sy = y - OV;
        if (sx >= OX && sx < OX + OS && sy >= OY && sy < OY + OS) {
            return tex(sx * 1.7, sy * 1.7, 90);
        }
        return tex(x - 1.5, y - 0.5, 7);
    }
    double px = x, py = y;
    for (int k = 0; k < 20; ++k) {
        double u, v; bool s;
        c.truth(px, py, &u, &v, &s);
        px = x - u; py = y - v;
    }
    return tex(px, py, 7);
}
} // namespace

int main()
{
    const Case cases[] = {
        { "small translation (3.3,-1.7)", 0.15, tSmall, false, 1. },
        { "large translation (26,11)", 0.3, tLarge, false, 1. },
        { "zoom 3% + rotate", 0.3, tZoomRot, false, 1. },
        { "small translation, 20% brighter", 0.2, tSmall, false, 1.2 },
        { "moving square over moving background", 0.6, tObject, true, 1. },
        { "untextured shaded sphere on black", 1.0, tSphere, false, 1. },
    };
    const double details[] = { 1., 0.5, 0.25 };
    int failures = 0;
    for (unsigned ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
        const Case& c = cases[ci];
        std::vector<float> a( (size_t)W * H ), b( (size_t)W * H );
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                a[(size_t)y * W + x] = FlowCore::matchEncode( (float)frame0(c, x, y) );
                b[(size_t)y * W + x] = FlowCore::matchEncode( (float)(frame1(c, x, y) * c.gain) );
            }
        }
        for (unsigned di = 0; di < 3; ++di) {
            FlowCore::Params p;
            p.vectorDetail = details[di];
            std::vector<float> flow;
            const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            const bool ok = FlowCore::computeFlow(&a[0], &b[0], W, H, p, &flow);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            double sum = 0., worst = 0.; long cnt = 0, bad = 0;
            double sumMoving = 0., sumStill = 0.; long cntMoving = 0, cntStill = 0;
            for (int y = 12; y < H - 12; ++y) {
                for (int x = 12; x < W - 12; ++x) {
                    double u, v; bool s;
                    c.truth(x, y, &u, &v, &s);
                    if (!s) {
                        continue;
                    }
                    const double e = std::hypot(flow[( (size_t)y * W + x ) * 2] - u, flow[( (size_t)y * W + x ) * 2 + 1] - v);
                    sum += e; worst = std::max(worst, e); ++cnt; bad += e > 1.;
                    if (u != 0. || v != 0.) { sumMoving += e; ++cntMoving; } else { sumStill += e; ++cntStill; }
                }
            }
            const double epe = sum / cnt;
            const bool pass = ok && (di > 0 || epe <= c.limit);
            failures += !pass;
            std::printf("%-40s detail %.2f  EPE %.3f px  >1px %5.2f%%  worst %6.2f  %6.0f ms  %s\n", c.name, details[di], epe,
                        100. * bad / cnt, worst, ms, di > 0 ? "" : (pass ? "ok" : "FAIL"));
            if (cntStill > 100 && cntMoving > 0 && di == 0) {
                std::printf("    moving part EPE %.3f px, still part EPE %.3f px\n", sumMoving / cntMoving, sumStill / cntStill);
            }
        }
    }
    return failures ? 1 : 0;
}
