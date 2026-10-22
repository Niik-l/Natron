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

#include "LensTrace.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <thread>

namespace defocus {

namespace {

// Built-in prescriptions: pbrt / Kolb format rows (radius, thickness, n_d, clear diameter; mm),
// as published with pbrt's example scenes, which cite the original designs in W. J. Smith,
// "Modern Lens Design". The data have no Abbe numbers: estimated from the index (see abbeFor).
struct BuiltinRow
{
    double r, t, n, ap;
};

struct Builtin
{
    const char* id;
    const char* name;
    const char* source;
    std::vector<BuiltinRow> rows;
};

const std::vector<Builtin>&
builtins()
{
    static const std::vector<Builtin> b = {
        {"dgauss50", "Double-Gauss 50mm f/2",
         "Double-Gauss f/2, 22 deg half field. US patent 2,673,491 (Tronnier); W. J. Smith, Modern Lens Design p. 312; scaled to 50 mm",
         {{29.475, 3.76, 1.67, 25.2}, {84.83, 0.12, 1, 25.2}, {19.275, 4.025, 1.67, 23}, {40.77, 3.275, 1.699, 23},
          {12.75, 5.705, 1, 18}, {0, 4.5, 0, 17.1}, {-14.495, 1.18, 1.603, 17}, {40.77, 6.065, 1.658, 20},
          {-20.385, 0.19, 1, 20}, {437.065, 3.22, 1.717, 20}, {-39.73, 0, 1, 20}}},
        {"wide22", "Wide-angle 22mm (38 deg)",
         "Wide-angle, 38 deg (Nakamura). W. J. Smith, Modern Lens Design p. 360; scaled to 22 mm",
         {{35.98738, 1.21638, 1.54, 23.716}, {11.69718, 9.9957, 1, 17.996}, {13.08714, 5.12622, 1.772, 12.364},
          {-22.63294, 1.76924, 1.617, 9.812}, {71.05802, 0.8184, 1, 9.152}, {0, 2.27766, 0, 8.756},
          {-9.58584, 2.43254, 1.617, 8.184}, {-11.28864, 0.11506, 1, 9.152}, {-166.7765, 3.09606, 1.713, 10.648},
          {-7.5911, 1.32682, 1.805, 11.44}, {-16.7662, 3.98068, 1, 12.276}, {-7.70286, 1.21638, 1.617, 13.42},
          {-11.97328, 0, 1, 17.996}}},
        {"tele250", "Telephoto 250mm f/5.6",
         "Super-achromat telephoto f/5.6 (Sigler). W. J. Smith, Modern Lens Design p. 175; scaled to 250 mm",
         {{54.6275, 12.52, 1.529, 47.5}, {-86.365, 3.755, 1.599, 44.5}, {271.7625, 2.8175, 1, 41.5},
          {0, 67.4125, 0, 40.5}, {-32.13, 3.755, 1.613, 31.5}, {49.5325, 12.52, 1.603, 33.5}, {-50.945, 0, 1, 37}}},
        {"fisheye10", "Fisheye 10mm (156 deg)",
         "Fisheye 16mm f/4, 155.9 deg (Muller). W. J. Smith, Modern Lens Design p. 164; scaled to 10 mm",
         {{30.2249, 0.8335, 1.62, 30.34}, {11.3931, 7.4136, 1, 20.68}, {75.2019, 1.0654, 1.639, 17.8},
          {8.3349, 11.1549, 1, 13.42}, {9.5882, 2.0054, 1.654, 9.02}, {43.8677, 5.3895, 1, 8.14},
          {0, 1.4163, 0, 6.08}, {29.4541, 2.1934, 1.517, 5.96}, {-5.2265, 0.9714, 1.805, 5.84},
          {-14.2884, 0.0627, 1, 5.96}, {-22.3726, 0.94, 1.673, 5.96}, {-15.0404, 0, 1, 6.52}}},
    };
    return b;
}

// Abbe number from the index along the line of ordinary optical glasses (BK7 1.517/64, SK16
// 1.620/60, F2 1.620/36, SF6 1.805/25: crowns sit above, flints below). An estimate for
// prescriptions published without dispersion.
double
abbeFor(double n)
{
    return std::max(22., std::min(66., 64. - 135. * (n - 1.517)));
}

// Index at a wavelength (nm) from n_d and the Abbe number: n(l) = n_d + B (1/l^2 - 1/l_d^2).
double
indexAt(double nd, double abbe, double nm)
{
    if (nd <= 1.0001 || abbe <= 0.) {
        return nd <= 0. ? 1. : nd;
    }
    const double ld = 0.5876, lF = 0.4861, lC = 0.6563, l = nm * 1e-3;
    const double B = ((nd - 1.) / abbe) / (1. / (lF * lF) - 1. / (lC * lC));
    return nd + B * (1. / (l * l) - 1. / (ld * ld));
}

struct Vec3
{
    double x = 0., y = 0., z = 0.;
};

// The lens laid out on the axis for one wavelength.
struct Optics
{
    std::vector<double> z, r, semi, nAfter;   // vertex z, radius, clear radius, index after
    int stop = -1;
    double stopRadius = 0.;                    // current iris radius
    double sensorZ = 0.;

    // Traces a ray through every surface (apertures are recorded, not enforced). Returns false if
    // it misses a surface or is totally reflected. stopXY: where it crosses the stop; blocked:
    // bit k set if it passed outside surface k's clear aperture.
    bool trace(Vec3 o, Vec3 d, Vec3* sensorHit, double stopXY[2], unsigned* blocked, Vec3* outDir = nullptr) const
    {
        double nPrev = 1.;
        unsigned blk = 0;
        for (size_t i = 0; i < z.size(); ++i) {
            double t;
            Vec3 nrm;
            if (r[i] == 0.) {
                if (std::fabs(d.z) < 1e-12) {
                    return false;
                }
                t = (z[i] - o.z) / d.z;
                nrm.z = -1.;
            } else {
                const Vec3 c{0., 0., z[i] + r[i]};
                const Vec3 oc{o.x - c.x, o.y - c.y, o.z - c.z};
                const double B = oc.x * d.x + oc.y * d.y + oc.z * d.z;
                const double C = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - r[i] * r[i];
                const double disc = B * B - C;
                if (disc < 0.) {
                    return false;
                }
                const double sq = std::sqrt(disc);
                const double t0 = -B - sq, t1 = -B + sq;
                const bool closer = (d.z > 0.) != (r[i] < 0.);
                t = closer ? t0 : t1;
                const Vec3 h{o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
                nrm = Vec3{(h.x - c.x) / r[i], (h.y - c.y) / r[i], (h.z - c.z) / r[i]};
            }
            if (t < -1e-9) {
                return false;
            }
            o = Vec3{o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
            const double h2 = o.x * o.x + o.y * o.y;
            if ((int)i == stop) {
                stopXY[0] = o.x / stopRadius;
                stopXY[1] = o.y / stopRadius;
            } else if (h2 > semi[i] * semi[i] && i < 32) {
                blk |= 1u << i;
            }
            if (r[i] != 0.) {
                // Refract (Snell), normal facing the incoming ray.
                double cosi = -(nrm.x * d.x + nrm.y * d.y + nrm.z * d.z);
                if (cosi < 0.) {
                    nrm = Vec3{-nrm.x, -nrm.y, -nrm.z};
                    cosi = -cosi;
                }
                const double eta = nPrev / nAfter[i];
                const double sin2t = eta * eta * (1. - cosi * cosi);
                if (sin2t >= 1.) {
                    return false;
                }
                const double k = eta * cosi - std::sqrt(1. - sin2t);
                d = Vec3{eta * d.x + k * nrm.x, eta * d.y + k * nrm.y, eta * d.z + k * nrm.z};
            }
            nPrev = nAfter[i];
        }
        if (d.z <= 1e-12) {
            return false;
        }
        const double t = (sensorZ - o.z) / d.z;
        if (sensorHit) {
            *sensorHit = Vec3{o.x + t * d.x, o.y + t * d.y, sensorZ};
        }
        if (outDir) {
            *outDir = d;
        }
        if (blocked) {
            *blocked = blk;
        }
        return true;
    }

    // Exit ray of a ray (no sensor): returns its last point and direction.
    bool exitRay(Vec3 o, Vec3 d, Vec3* po, Vec3* pd) const
    {
        Optics tmp = *this;
        tmp.sensorZ = z.back();
        double s[2];
        Vec3 hit, dir;
        if (!tmp.trace(o, d, &hit, s, nullptr, &dir)) {
            return false;
        }
        *po = hit;
        *pd = dir;
        return true;
    }
};

Optics
layout(const LensPrescription& lens, double scale, double nm)
{
    Optics o;
    double zc = 0.;
    for (size_t i = 0; i < lens.surfaces.size(); ++i) {
        const LensSurface& s = lens.surfaces[i];
        o.z.push_back(zc);
        o.r.push_back(s.stop ? 0. : s.radius * scale);
        o.semi.push_back(s.semiAperture * scale);
        const double nd = s.ior <= 0. ? 1. : s.ior;
        o.nAfter.push_back(indexAt(nd, s.abbe > 0. ? s.abbe : abbeFor(nd), nm));
        if (s.stop) {
            o.stop = (int)i;
            o.stopRadius = s.semiAperture * scale;
        }
        zc += s.thickness * scale;
    }
    return o;
}

// Where a ray from the left crosses the axis after the lens (z), or NaN.
double
axisCrossing(const Optics& op, Vec3 o, Vec3 d)
{
    Vec3 po, pd;
    if (!op.exitRay(o, d, &po, &pd) || std::fabs(pd.x) < 1e-15) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return po.z - po.x * pd.z / pd.x;
}

// The rays traced from one object point (see LensTable::build).
struct PointRays
{
    int grid = 0;
    std::vector<double> sx, sy, hx, hy;   // stop position (unit = stop radius), sensor hit - chief hit (mm)
    std::vector<unsigned> blk;            // rims missed
    std::vector<unsigned char> val;       // traced at all
};

Vec3
normalised(Vec3 v)
{
    const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vec3{v.x / l, v.y / l, v.z / l};
}

} // anonymous namespace

std::vector<std::string>
builtinLensIds()
{
    std::vector<std::string> v;
    for (const Builtin& b : builtins()) {
        v.push_back(b.id);
    }
    return v;
}

bool
builtinLens(const std::string& id, LensPrescription* out)
{
    for (const Builtin& b : builtins()) {
        if (id != b.id) {
            continue;
        }
        LensPrescription p;
        p.id = b.id;
        p.name = b.name;
        p.source = b.source;
        p.abbeEstimated = true;
        for (const BuiltinRow& r : b.rows) {
            LensSurface s;
            s.stop = r.r == 0. && r.n == 0.;
            s.radius = r.r;
            s.thickness = r.t;
            s.ior = r.n <= 0. ? 1. : r.n;
            s.semiAperture = 0.5 * r.ap;
            p.surfaces.push_back(s);
        }
        *out = p;
        return true;
    }
    return false;
}

bool
parseLensFile(const std::string& text, LensPrescription* out, std::string* error)
{
    LensPrescription p;
    std::istringstream in(text);
    std::string line;
    bool anyAbbe = false;
    while (std::getline(in, line)) {
        const size_t h = line.find('#');
        if (h != std::string::npos) {
            if (p.name.empty()) {
                std::string c = line.substr(h + 1);
                c.erase(0, c.find_first_not_of(" \t"));
                if (!c.empty() && c.find_first_of("0123456789") != 0) {
                    p.name = c;
                }
            }
            line = line.substr(0, h);
        }
        std::istringstream ls(line);
        std::vector<double> v;
        double x;
        while (ls >> x) {
            v.push_back(x);
        }
        if (v.empty()) {
            continue;
        }
        if (v.size() < 4) {
            if (error) {
                *error = "lens file: a row needs radius, thickness, index and aperture";
            }
            return false;
        }
        LensSurface s;
        s.radius = v[0];
        s.thickness = v[1];
        s.ior = v[2] <= 0. ? 1. : v[2];
        s.semiAperture = 0.5 * v[3];
        s.stop = v[0] == 0. && v[2] <= 0.;
        if (v.size() >= 5 && v[4] > 0.) {
            s.abbe = v[4];
            anyAbbe = true;
        }
        p.surfaces.push_back(s);
    }
    int stops = 0;
    for (const LensSurface& s : p.surfaces) {
        stops += s.stop;
    }
    if (p.surfaces.size() < 2 || stops != 1) {
        if (error) {
            *error = stops != 1 ? "lens file: exactly one row must be the stop (radius 0, index 0)"
                                : "lens file: no surfaces";
        }
        return false;
    }
    p.abbeEstimated = !anyAbbe;
    *out = p;
    return true;
}

struct LensTableBuilder
{
    static void fitKernel(const std::vector<double>& sx, const std::vector<double>& sy,
                          const std::vector<double>& hx, const std::vector<double>& hy,
                          const std::vector<unsigned>& blocked, const std::vector<unsigned char>& valid,
                          int grid, double rThinMm, TracedKernel* k);
};

void
LensTableBuilder::fitKernel(const std::vector<double>& sx, const std::vector<double>& sy,
                            const std::vector<double>& hx, const std::vector<double>& hy,
                            const std::vector<unsigned>& blocked, const std::vector<unsigned char>& valid,
                            int grid, double rThinMm, TracedKernel* k)
{
    // 1. Affine map stop -> sensor over the rays that get through (least squares, normal
    // equations). Rays a rim blocks are traced on regardless and land anywhere: not in the fit.
    double S[3][3] = {{0}}, Bx[3] = {0}, By[3] = {0};
    int nIn = 0, nFit = 0;
    for (size_t i = 0; i < sx.size(); ++i) {
        if (!valid[i] || sx[i] * sx[i] + sy[i] * sy[i] > 1.) {
            continue;
        }
        ++nIn;
        if (blocked[i]) {
            continue;
        }
        const double f[3] = {sx[i], sy[i], 1.};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                S[a][b] += f[a] * f[b];
            }
            Bx[a] += f[a] * hx[i];
            By[a] += f[a] * hy[i];
        }
        ++nFit;
    }
    *k = TracedKernel();
    if (nFit < 12 || !(std::fabs(rThinMm) > 1e-12)) {
        return;
    }
    auto solve3 = [](double M[3][3], const double v[3], double x[3]) {
        double a[3][4];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                a[r][c] = M[r][c];
            }
            a[r][3] = v[r];
        }
        for (int c = 0; c < 3; ++c) {
            int p = c;
            for (int r = c + 1; r < 3; ++r) {
                if (std::fabs(a[r][c]) > std::fabs(a[p][c])) {
                    p = r;
                }
            }
            for (int q = 0; q < 4; ++q) {
                std::swap(a[c][q], a[p][q]);
            }
            if (std::fabs(a[c][c]) < 1e-18) {
                x[0] = x[1] = x[2] = 0.;
                return;
            }
            for (int r = 0; r < 3; ++r) {
                if (r != c) {
                    const double f = a[r][c] / a[c][c];
                    for (int q = 0; q < 4; ++q) {
                        a[r][q] -= f * a[c][q];
                    }
                }
            }
        }
        for (int r = 0; r < 3; ++r) {
            x[r] = a[r][3] / a[r][r];
        }
    };
    double px[3], py[3];
    solve3(S, Bx, px);
    solve3(S, By, py);
    double A[4] = {px[0], px[1], py[0], py[1]};
    const double bx = px[2];
    const double det = A[0] * A[3] - A[1] * A[2];
    if (!(std::fabs(det) > 1e-18)) {
        return;
    }
    const double inv[4] = {A[3] / det, -A[1] / det, -A[2] / det, A[0] / det};

    // 2. Radial profile: where the passing rays land, in the affine frame (q = 1 is the stop rim
    // under a perfect lens). Spherical aberration bunches them at the rim on one side of focus.
    std::vector<double> q;
    q.reserve(nIn);
    unsigned anyBlock = 0;
    int nPass = 0;
    for (size_t i = 0; i < sx.size(); ++i) {
        if (!valid[i] || sx[i] * sx[i] + sy[i] * sy[i] > 1.) {
            continue;
        }
        anyBlock |= blocked[i];
        if (blocked[i]) {
            continue;
        }
        ++nPass;
        const double ex = hx[i] - bx, ey = hy[i] - py[2];
        const double ux = inv[0] * ex + inv[1] * ey, uy = inv[2] * ex + inv[3] * ey;
        q.push_back(std::sqrt(ux * ux + uy * uy));
    }
    k->energy = (double)nPass / nIn;
    double qm = 1.;
    if (!q.empty()) {
        std::vector<double> qs = q;
        const size_t i98 = std::min(qs.size() - 1, (size_t)(0.98 * (qs.size() - 1)));
        std::nth_element(qs.begin(), qs.begin() + i98, qs.end());
        qm = std::max(1., std::min(1.6, qs[i98]));
    }
    const int nb = TracedKernel::kRings;
    std::vector<double> cntQ(nb, 0.), cntS(nb, 0.), allS(nb, 0.);
    for (size_t i = 0, j = 0; i < sx.size(); ++i) {
        if (!valid[i]) {
            continue;
        }
        const double rs = std::sqrt(sx[i] * sx[i] + sy[i] * sy[i]);
        if (rs > 1.) {
            continue;
        }
        const int bs = std::min(nb - 1, (int)(rs / qm * nb));
        allS[bs] += 1.;
        if (blocked[i]) {
            continue;
        }
        cntS[bs] += 1.;
        cntQ[std::min(nb - 1, (int)(q[j++] / qm * nb))] += 1.;
    }
    // Density per annulus area, corrected for the part of each annulus the clip discs remove.
    // Regularised: the kernel changes from pixel to pixel, so a noisy or extreme profile shows as
    // uneven coverage (alpha seams) where neighbouring samples use different entries. Near a line
    // focus (astigmatism: the affine map nearly singular) q means little: uniform.
    double prof[TracedKernel::kRings];
    const double nA2 = A[0] * A[0] + A[1] * A[1] + A[2] * A[2] + A[3] * A[3];
    const bool degenerate = std::fabs(det) < 0.05 * nA2 || q.size() < 200;
    for (int b = 0; b < nb; ++b) {
        const double area = ((b + 1.) * (b + 1.) - (double)b * b) / (nb * nb);
        const double kept = allS[b] > 0. ? cntS[b] / allS[b] : 1.;
        prof[b] = degenerate ? 1. : cntQ[b] / area / std::max(0.15, kept);
    }
    double sm[TracedKernel::kRings];
    for (int b = 0; b < nb; ++b) {
        const double l = prof[std::max(0, b - 1)], r = prof[std::min(nb - 1, b + 1)];
        sm[b] = 0.25 * l + 0.5 * prof[b] + 0.25 * r;
    }
    auto normalise = [&](double* d) {
        double tot = 0.;
        for (int b = 0; b < nb; ++b) {
            tot += d[b] * ((b + 1.) * (b + 1.) - (double)b * b) / (nb * nb);
        }
        for (int b = 0; b < nb; ++b) {
            d[b] = tot > 1e-12 ? d[b] / tot : 1.;
        }
    };
    normalise(sm);
    for (int b = 0; b < nb; ++b) {
        sm[b] = std::min(3., sm[b]);
    }
    normalise(sm);
    for (int b = 0; b < nb; ++b) {
        k->density[b] = sm[b];
        k->ringScale.push_back((b + 1.) / nb);
        k->ringWeight.push_back(sm[b] - (b + 1 < nb ? sm[b + 1] : 0.));
    }
    k->valid = true;

    // 3. Clip discs: each rim that blocks rays inside the stop cuts a disc-shaped region out of it
    // (in stop coordinates; centre on the tangential axis by symmetry). Fitted to the boundary
    // between neighbouring grid rays that pass / fail that rim: x^2 + y^2 = 2 cx x + K.
    struct Cand { double cx, R, frac; int rim; };
    std::vector<Cand> cands;
    for (int sbit = 0; sbit < 32; ++sbit) {
        const unsigned m = 1u << sbit;
        if (!(anyBlock & m)) {
            continue;
        }
        double M[2][2] = {{0, 0}, {0, 0}}, V[2] = {0, 0};
        int nb2 = 0, nFail = 0;
        for (int gy = 0; gy < grid; ++gy) {
            for (int gx = 0; gx < grid; ++gx) {
                const int i = gy * grid + gx;
                if (!valid[i]) {
                    continue;
                }
                if (sx[i] * sx[i] + sy[i] * sy[i] <= 1. && (blocked[i] & m)) {
                    ++nFail;
                }
                const int nbrs[2] = {gx + 1 < grid ? i + 1 : -1, gy + 1 < grid ? i + grid : -1};
                for (int nIdx : nbrs) {
                    if (nIdx < 0 || !valid[nIdx] || ((blocked[i] & m) != 0) == ((blocked[nIdx] & m) != 0)) {
                        continue;
                    }
                    const double x = 0.5 * (sx[i] + sx[nIdx]), y = 0.5 * (sy[i] + sy[nIdx]);
                    if (x * x + y * y > 2.25) {
                        continue;
                    }
                    // rows: [2x, 1] . [cx, K] = x^2 + y^2
                    const double f0 = 2. * x, f1 = 1., rhs = x * x + y * y;
                    M[0][0] += f0 * f0; M[0][1] += f0 * f1; M[1][1] += f1 * f1;
                    V[0] += f0 * rhs; V[1] += f1 * rhs;
                    ++nb2;
                }
            }
        }
        if (nb2 < 6 || nFail == 0) {
            continue;
        }
        M[1][0] = M[0][1];
        const double d2 = M[0][0] * M[1][1] - M[0][1] * M[1][0];
        if (std::fabs(d2) < 1e-12) {
            continue;
        }
        const double cx = (V[0] * M[1][1] - V[1] * M[0][1]) / d2;
        const double K = (M[0][0] * V[1] - M[1][0] * V[0]) / d2;
        const double R2 = K + cx * cx;
        if (!(R2 > 1e-6)) {
            continue;
        }
        cands.push_back(Cand{cx, std::sqrt(R2), (double)nFail / nIn, sbit});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.frac > b.frac; });
    for (const Cand& c : cands) {
        if (k->nClip >= 4) {
            break;
        }
        // The passing region must be inside the circle (the stop centre, or the side away from
        // the cut).
        if (c.R < std::fabs(c.cx) && c.R < 1.) {
            continue;
        }
        // Stop coordinates are the outline's unit coordinates at the rim (rim rays land at qm,
        // the scaled outline's edge), so the discs need no rescaling.
        k->clipX[k->nClip] = c.cx;
        k->clipR[k->nClip] = c.R;
        k->clipRim[k->nClip] = c.rim;
        ++k->nClip;
    }

    // Centre of the drawn outline (unit disc with the profile, minus the clip discs): the scatter
    // keeps each kernel centred on its pixel, since a centre that moves with depth and field leaves
    // uneven coverage (seams) in a surface wherever its rate of change steps.
    {
        double sw = 0., sx = 0.;
        const int g = 64;
        for (int gy = 0; gy < g; ++gy) {
            for (int gx = 0; gx < g; ++gx) {
                const double x = 2. * (gx + 0.5) / g - 1., y = 2. * (gy + 0.5) / g - 1.;
                const double rr = std::sqrt(x * x + y * y);
                if (rr > 1.) {
                    continue;
                }
                bool in = true;
                for (int i = 0; i < k->nClip && in; ++i) {
                    in = (x - k->clipX[i]) * (x - k->clipX[i]) + y * y <= k->clipR[i] * k->clipR[i];
                }
                if (!in) {
                    continue;
                }
                const double d = k->density[std::min(nb - 1, (int)(rr * nb))];
                sw += d;
                sx += d * x;
            }
        }
        k->centroid = sw > 0. ? sx / sw : 0.;
    }

    // 4. Relative to the thin-lens radius; the outline grows by qm so the profile fits inside.
    for (int i = 0; i < 4; ++i) {
        k->a[i] = A[i] * qm / rThinMm;
    }
    k->shift = bx / rThinMm;
}

std::shared_ptr<LensTable>
LensTable::build(const LensPrescription& lens, const LensTableParams& p, std::string* error)
{
    auto fail = [&](const char* m) {
        if (error) {
            *error = m;
        }
        return std::shared_ptr<LensTable>();
    };
    if (lens.surfaces.size() < 2) {
        return fail("lens: no surfaces");
    }
    const double nmG = p.wavelengthNm[1];

    // Focal length of the prescription (paraxial: a ray parallel to the axis at small height).
    Optics op = layout(lens, 1., nmG);
    if (op.stop < 0) {
        return fail("lens: no aperture stop");
    }
    const double h0 = 1e-3 * std::max(1e-6, op.semi[0]);
    Vec3 po, pd;
    if (!op.exitRay(Vec3{h0, 0., -1.}, Vec3{0., 0., 1.}, &po, &pd) || !(pd.x < 0.)) {
        return fail("lens: does not focus parallel light (diverging, or bad data)");
    }
    const double efl = -h0 * pd.z / pd.x;
    const double scale = p.focalLengthMm / efl;

    std::shared_ptr<LensTable> tab(new LensTable());
    tab->_p = p;

    // Per channel layouts; focus and pupil from green.
    Optics ch[3];
    for (int c = 0; c < 3; ++c) {
        ch[c] = layout(lens, scale, p.wavelengthNm[c]);
    }
    tab->_chromatic = true;

    // Iris: entrance pupil radius f / 2N, mapped to the stop by the paraxial pupil ratio.
    Optics& g = ch[1];
    {
        const double hh = 1e-3 * g.semi[0];
        double s[2];
        Vec3 hit;
        Optics t = g;
        t.stopRadius = 1.;
        t.sensorZ = t.z.back() + 1.;
        if (!t.trace(Vec3{hh, 0., -1.}, Vec3{0., 0., 1.}, &hit, s, nullptr)) {
            return fail("lens: paraxial trace failed");
        }
        const double ratio = std::fabs(s[0]) / hh;   // stop height per entrance height
        const double maxStop = lens.surfaces[g.stop].semiAperture * scale;
        double stopR = p.focalLengthMm / (2. * std::max(0.1, p.fNumber)) * ratio;
        tab->_widestF = p.focalLengthMm / (2. * maxStop / ratio);
        if (stopR > maxStop) {
            stopR = maxStop;
        }
        tab->_fNumber = p.focalLengthMm / (2. * stopR / ratio);
        for (int c = 0; c < 3; ++c) {
            ch[c].stopRadius = stopR;
        }
    }

    // Entrance pupil (axial position): a ray through the stop centre, extended back to the axis.
    double zEP;
    {
        auto stopX = [&](double ze) {
            const double a = 1e-3;
            const double z0 = -10.;
            Vec3 o{(z0 - ze) * a, 0., z0};
            double s[2] = {0., 0.};
            Vec3 hit;
            Optics t = g;
            t.stopRadius = 1.;
            t.sensorZ = t.z.back() + 1.;
            if (!t.trace(o, normalised(Vec3{a, 0., 1.}), &hit, s, nullptr)) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return s[0];
        };
        const double z1 = 0., z2 = g.z[g.stop];
        const double x1 = stopX(z1), x2 = stopX(z2);
        if (!std::isfinite(x1) || !std::isfinite(x2) || x1 == x2) {
            return fail("lens: entrance pupil not found");
        }
        zEP = z1 - x1 * (z2 - z1) / (x2 - x1);
    }

    // Focus: sensor where an axial point at the focus distance images (green).
    {
        const double S = std::max(p.focusDistanceMm, 1.01 * p.focalLengthMm);
        const Vec3 o{0., 0., zEP - S};
        const double a = 1e-3 * g.semi[0] / S;
        const double zc = axisCrossing(g, o, normalised(Vec3{a, 0., 1.}));
        if (!std::isfinite(zc) || zc <= g.z.back()) {
            return fail("lens: cannot focus at that distance");
        }
        // Best focus, not paraxial: a real camera focuses on the sharpest image, which with
        // spherical aberration sits in front of the paraxial focus. Minimise the RMS spot of the
        // axial point over the open stop (golden section around the paraxial position).
        auto spotRms = [&](double zs) {
            Optics t = g;
            t.sensorZ = zs;
            double sum = 0.;
            int n = 0;
            const int G = 24;
            for (int gy = 0; gy < G; ++gy) {
                for (int gx = 0; gx < G; ++gx) {
                    // Through a grid on the entrance pupil plane (the open aperture as seen from
                    // the object).
                    const double rep = 1.25 * p.focalLengthMm / (2. * tab->_fNumber);
                    const double fx = rep * (2. * (gx + 0.5) / G - 1.), fy = rep * (2. * (gy + 0.5) / G - 1.);
                    double s[2];
                    unsigned b = 0;
                    Vec3 hit;
                    if (!t.trace(o, normalised(Vec3{fx, fy, zEP - o.z}), &hit, s, &b) || b || s[0] * s[0] + s[1] * s[1] > 1.) {
                        continue;
                    }
                    sum += hit.x * hit.x + hit.y * hit.y;
                    ++n;
                }
            }
            return n > 4 ? sum / n : std::numeric_limits<double>::infinity();
        };
        double lo = zc - 0.03 * (zc - g.z.back() + p.focalLengthMm * 0.2), hi = zc + 0.01 * p.focalLengthMm;
        const double gr = 0.6180339887;
        double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
        double f1 = spotRms(x1), f2 = spotRms(x2);
        for (int it = 0; it < 40; ++it) {
            if (f1 < f2) {
                hi = x2; x2 = x1; f2 = f1;
                x1 = hi - gr * (hi - lo); f1 = spotRms(x1);
            } else {
                lo = x1; x1 = x2; f1 = f2;
                x2 = lo + gr * (hi - lo); f2 = spotRms(x2);
            }
        }
        const double zBest = std::isfinite(f1) ? 0.5 * (lo + hi) : zc;
        for (int c = 0; c < 3; ++c) {
            ch[c].sensorZ = zBest;
        }
        tab->_bfd = zBest - g.z.back();
    }

    // Table axes. u = r / rInf: far side (0, 1), near side (-inf, 0).
    tab->_u = {-4., -3., -2., -1.5, -1., -0.7, -0.5, -0.35, -0.25, -0.15, -0.08,
               0.08, 0.15, 0.25, 0.35, 0.5, 0.65, 0.8, 0.9, 0.97};
    tab->_rho = {0., 0.15, 0.3, 0.45, 0.6, 0.7, 0.8, 0.9, 1.};
    const double S = std::max(p.focusDistanceMm, 1.01 * p.focalLengthMm);
    const double rInfMm = 0.5 * p.focalLengthMm * p.focalLengthMm / (std::max(0.1, p.fNumber) * (S - p.focalLengthMm));
    const size_t nU = tab->_u.size(), nR = tab->_rho.size();
    tab->_k.resize(3 * nU * nR);

    // Rays from one object point: the point at signed defocus u whose chief ray (through the stop
    // centre) images at height hTarget on the sensor. Kept on the table (by value) so the testbed
    // can trace reference PSFs with the same optics. Returns 0, or 1 / 2 when the chief ray or the
    // sampling fails (outside the image circle).
    const Optics chCopy[3] = {ch[0], ch[1], ch[2]};
    const double focal = p.focalLengthMm;
    tab->_sampler = [chCopy, zEP, S, focal](int c, double u, double hTarget, int grid, void* outv) -> int {
        PointRays& out = *static_cast<PointRays*>(outv);
        const Optics& oc = chCopy[c];
        const double d = S / (1. - u);               // object depth from the entrance pupil
        const double zObj = zEP - d;
        // Chief ray whose image lands at hTarget: solve the object height by secant, aiming each
        // chief ray at the stop centre by secant on its slope.
        auto chief = [&](double X, double* hImg, Vec3* front) -> bool {
            Optics t = oc;
            t.stopRadius = 1.;
            auto sx = [&](double slope, Vec3* hit) {
                double s[2] = {0., 0.};
                if (!t.trace(Vec3{X, 0., zObj}, normalised(Vec3{slope, 0., 1.}), hit, s, nullptr)) {
                    return std::numeric_limits<double>::quiet_NaN();
                }
                return s[0];
            };
            // Secant on the slope; a step that misses a surface is halved back towards the last
            // good slope (steep rays near the edge of the field).
            double s0 = -X / (zEP - zObj);
            Vec3 hit;
            double f0 = sx(s0, &hit);
            if (!std::isfinite(f0)) {
                return false;
            }
            double s1 = s0 * 0.995 - 1e-6;
            double f1 = sx(s1, &hit);
            for (int h = 0; h < 20 && !std::isfinite(f1); ++h) {
                s1 = 0.5 * (s0 + s1);
                f1 = sx(s1, &hit);
            }
            for (int it = 0; it < 20 && std::isfinite(f1) && std::fabs(f1) > 1e-9; ++it) {
                if (f1 == f0) {
                    break;
                }
                double s2 = s1 - f1 * (s1 - s0) / (f1 - f0);
                double f2 = sx(s2, &hit);
                for (int h = 0; h < 20 && !std::isfinite(f2); ++h) {
                    s2 = 0.5 * (s1 + s2);
                    f2 = sx(s2, &hit);
                }
                s0 = s1; f0 = f1;
                s1 = s2; f1 = f2;
            }
            if (std::isfinite(f1)) {
                f1 = sx(s1, &hit);   // leave hit for the final slope
            }
            if (!std::isfinite(f1)) {
                return false;
            }
            *hImg = hit.x;
            const Vec3 dd = normalised(Vec3{s1, 0., 1.});
            const double tf = (0. - zObj) / dd.z;   // where it crosses the front vertex plane
            *front = Vec3{X + tf * dd.x, 0., 0.};
            return true;
        };
        double X = 0., hc = 0.;
        Vec3 front{0., 0., 0.};
        if (hTarget > 0.) {
            double X0 = hTarget * d / focal, X1 = X0 * 1.02;
            double h0c, h1c;
            Vec3 f0v, f1v;
            if (!chief(X0, &h0c, &f0v) || !chief(X1, &h1c, &f1v)) {
                return 1;
            }
            for (int it = 0; it < 10 && std::fabs(h1c - hTarget) > 1e-4; ++it) {
                if (h1c == h0c) {
                    break;
                }
                const double X2 = X1 - (h1c - hTarget) * (X1 - X0) / (h1c - h0c);
                X0 = X1; h0c = h1c;
                X1 = X2;
                if (!chief(X1, &h1c, &f1v)) {
                    return 1;
                }
            }
            X = X1;
            hc = h1c;
            front = f1v;
        } else {
            Vec3 fv;
            if (!chief(0., &hc, &fv)) {
                return 1;
            }
        }
        // Rays from the object point to a grid on the front vertex plane, traced without enforcing
        // apertures; each records stop position, blocking rims and hit (relative to the chief
        // ray's). Two passes: a coarse grid over the whole front element finds where the rays that
        // reach the stop enter (a small patch for a fisheye), then the fine grid covers that patch.
        out.grid = grid;
        out.sx.assign(grid * grid, 0.);
        out.sy.assign(grid * grid, 0.);
        out.hx.assign(grid * grid, 0.);
        out.hy.assign(grid * grid, 0.);
        out.blk.assign(grid * grid, 0);
        out.val.assign(grid * grid, 0);
        auto sample = [&](double cx, double cy, double hx, double hy) {
            for (int gy = 0; gy < grid; ++gy) {
                for (int gx = 0; gx < grid; ++gx) {
                    const int i = gy * grid + gx;
                    const double fx = cx + hx * (2. * (gx + 0.5) / grid - 1.);
                    const double fy = cy + hy * (2. * (gy + 0.5) / grid - 1.);
                    const Vec3 dd = normalised(Vec3{fx - X, fy, 0. - zObj});
                    double s[2] = {0., 0.};
                    Vec3 hit;
                    unsigned b = 0;
                    out.val[i] = 0;
                    if (oc.trace(Vec3{X, 0., zObj}, dd, &hit, s, &b)) {
                        out.val[i] = 1;
                        out.sx[i] = s[0];
                        out.sy[i] = s[1];
                        out.hx[i] = hit.x - hc;
                        out.hy[i] = hit.y;
                        out.blk[i] = b;
                    }
                }
            }
        };
        const double gr = 1.6 * oc.semi[0];
        sample(front.x, 0., gr, gr);
        double bx0 = 1e30, bx1 = -1e30, by1 = 0.;
        for (int gy = 0; gy < grid; ++gy) {
            for (int gx = 0; gx < grid; ++gx) {
                const int i = gy * grid + gx;
                if (out.val[i] && out.sx[i] * out.sx[i] + out.sy[i] * out.sy[i] <= 2.25) {
                    const double fx = front.x + gr * (2. * (gx + 0.5) / grid - 1.);
                    const double fy = gr * (2. * (gy + 0.5) / grid - 1.);
                    bx0 = std::min(bx0, fx);
                    bx1 = std::max(bx1, fx);
                    by1 = std::max(by1, std::fabs(fy));
                }
            }
        }
        if (bx1 < bx0) {
            return 2;
        }
        const double cell = 2. * gr / grid;
        sample(0.5 * (bx0 + bx1), 0., 0.5 * (bx1 - bx0) + cell, by1 + cell);
        return 0;
    };

    const int grid = 56;
    auto entry = [&](int c, int iu, int ir) {
        const double u = tab->_u[iu];
        PointRays pr;
        const int r = tab->_sampler(c, u, tab->_rho[ir] * p.fieldRadiusMm, grid, &pr);
        if (r == 1) {
            ++tab->_failChief;
            return;
        }
        if (r == 2) {
            ++tab->_failSample;
            return;
        }
        TracedKernel& k = tab->_k[((size_t)c * nU + iu) * nR + ir];
        LensTableBuilder::fitKernel(pr.sx, pr.sy, pr.hx, pr.hy, pr.blk, pr.val, grid, rInfMm * u, &k);
        if (!k.valid) {
            ++tab->_failFit;
        }
    };

    // Entries in parallel (independent).
    const int nEntries = (int)(3 * nU * nR);
    const int nT = std::max(1, std::min(16, (int)std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    std::atomic<int> next(0);
    for (int t = 0; t < nT; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                const int e = next.fetch_add(1);
                if (e >= nEntries) {
                    return;
                }
                const int c = e / (int)(nU * nR), rem = e % (int)(nU * nR);
                entry(c, rem / (int)nR, rem % (int)nR);
            }
        });
    }
    for (std::thread& t : pool) {
        t.join();
    }
    // Field positions the lens can't reach (outside its image circle on this film back): use the
    // outermost traced one rather than a plain disc.
    for (int c = 0; c < 3; ++c) {
        for (size_t iu = 0; iu < nU; ++iu) {
            int lastGood = -1;
            for (size_t ir = 0; ir < nR; ++ir) {
                TracedKernel& k = tab->_k[((size_t)c * nU + iu) * nR + ir];
                if (k.valid) {
                    lastGood = (int)ir;
                } else if (lastGood >= 0) {
                    k = tab->at(c, (int)iu, lastGood);
                }
            }
        }
    }
    // Kernel extent over the thin-lens radius, where the blur is substantial (near focus the ratio
    // can be large but the kernel itself is tiny).
    for (int c = 0; c < 3; ++c) {
        for (size_t iu = 0; iu < nU; ++iu) {
            if (std::fabs(tab->_u[iu]) < 0.5) {
                continue;
            }
            for (size_t ir = 0; ir < nR; ++ir) {
                const TracedKernel& k = tab->at(c, (int)iu, (int)ir);
                const double ext = std::max(std::sqrt(k.a[0] * k.a[0] + k.a[2] * k.a[2]), std::sqrt(k.a[1] * k.a[1] + k.a[3] * k.a[3]));
                tab->_maxScale = std::max(tab->_maxScale, std::min(4., ext + std::fabs(k.shift)));
            }
        }
    }
    return tab;
}

void
LensTable::kernel(double u, double rho, int channel, TracedKernel* out) const
{
    channel = std::max(0, std::min(2, channel));
    auto locate = [](const std::vector<double>& ax, double v, int* i0, double* f) {
        if (v <= ax.front()) {
            *i0 = 0; *f = 0.;
            return;
        }
        if (v >= ax.back()) {
            *i0 = (int)ax.size() - 2; *f = 1.;
            return;
        }
        int i = 0;
        while (i + 2 < (int)ax.size() && v > ax[i + 1]) {
            ++i;
        }
        *i0 = i;
        *f = (v - ax[i]) / (ax[i + 1] - ax[i]);
    };
    int iu, ir;
    double fu, fr;
    locate(_u, u, &iu, &fu);
    // No interpolation across focus: the near and far sides are different kernels.
    if (_u[iu] < 0. && _u[iu + 1] > 0.) {
        fu = u < 0. ? 0. : 1.;
    }
    locate(_rho, std::max(0., rho), &ir, &fr);
    const TracedKernel* k[4] = {&at(channel, iu, ir), &at(channel, iu + 1, ir), &at(channel, iu, ir + 1), &at(channel, iu + 1, ir + 1)};
    const double w[4] = {(1 - fu) * (1 - fr), fu * (1 - fr), (1 - fu) * fr, fu * fr};
    TracedKernel r;
    for (int i = 0; i < 4; ++i) {
        r.a[i] = 0.;
    }
    r.shift = 0.;
    r.energy = 0.;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            r.a[i] += w[j] * k[j]->a[i];
        }
        r.shift += w[j] * k[j]->shift;
        r.energy += w[j] * k[j]->energy;
    }
    // Clip discs and profile: from the nearest entry (discs differ in count between entries).
    int best = 0;
    for (int j = 1; j < 4; ++j) {
        if (w[j] > w[best]) {
            best = j;
        }
    }
    r.nClip = k[best]->nClip;
    for (int i = 0; i < r.nClip; ++i) {
        r.clipX[i] = k[best]->clipX[i];
        r.clipR[i] = k[best]->clipR[i];
    }
    r.ringScale = k[best]->ringScale;
    r.ringWeight = k[best]->ringWeight;
    *out = r;
}

} // namespace defocus

namespace defocus {

namespace {

// Clip discs blended rim by rim (centre and radius averaged over the entries that have it). Where
// an entry has no cut from that rim, the disc grows towards one that contains the whole stop, so a
// cut fades in across the table instead of appearing at once.
void blendClips(const TracedKernel* const k[4], const double w[4], KernelLookup* out)
{
    struct Rim { int id; double wHave, cx, R; };
    Rim rims[16];
    int nR = 0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < k[j]->nClip; ++i) {
            const int id = k[j]->clipRim[i];
            int r = 0;
            while (r < nR && rims[r].id != id) {
                ++r;
            }
            if (r == nR) {
                if (nR == 16) {
                    continue;
                }
                rims[nR++] = Rim{id, 0., 0., 0.};
            }
            rims[r].wHave += w[j];
            rims[r].cx += w[j] * k[j]->clipX[i];
            rims[r].R += w[j] * k[j]->clipR[i];
        }
    }
    out->nClip = 0;
    while (out->nClip < 4) {
        int bi = -1;
        double bs = 1e-9;
        for (int r = 0; r < nR; ++r) {
            if (rims[r].id != -2 && rims[r].wHave > bs) {
                bs = rims[r].wHave;
                bi = r;
            }
        }
        if (bi < 0) {
            break;
        }
        Rim& rm = rims[bi];
        const double cx = rm.cx / rm.wHave, R = rm.R / rm.wHave;
        const double rOpen = std::fabs(cx) + 1.02;   // same centre, cuts nothing
        out->clipX[out->nClip] = cx;
        out->clipR[out->nClip] = rm.wHave * R + (1. - rm.wHave) * std::max(R, rOpen);
        ++out->nClip;
        rm.id = -2;
    }
}

} // namespace

void
LensTable::lookup(double u, double rho, int channel, KernelLookup* out) const
{
    channel = std::max(0, std::min(2, channel));
    // Axes are short: a linear scan is as fast as a search.
    int iu = 0;
    while (iu + 2 < (int)_u.size() && u > _u[iu + 1]) {
        ++iu;
    }
    double fu = (u - _u[iu]) / (_u[iu + 1] - _u[iu]);
    fu = std::max(0., std::min(1., fu));
    rho = std::max(0., rho);
    int ir = 0;
    while (ir + 2 < (int)_rho.size() && rho > _rho[ir + 1]) {
        ++ir;
    }
    const double fr = std::max(0., std::min(1., (rho - _rho[ir]) / (_rho[ir + 1] - _rho[ir])));
    const TracedKernel* k[4] = {&at(channel, iu, ir), &at(channel, iu + 1, ir), &at(channel, iu, ir + 1), &at(channel, iu + 1, ir + 1)};
    const double w[4] = {(1 - fu) * (1 - fr), fu * (1 - fr), (1 - fu) * fr, fu * fr};
    // The map and the offset are relative to the thin-lens radius, which goes through zero at
    // focus while the kernel itself doesn't (the lens's own blur at best focus). So the absolute
    // kernel (relative x u) is what's interpolated, also across focus, and then made relative to
    // this u again; the kernel stays continuous through the focus plane.
    // Beyond the ends of the table the relative kernel is held (it keeps growing with the blur).
    const double uq = std::fabs(u) > 1e-6 ? u : (u < 0. ? -1e-6 : 1e-6);
    const bool inTable = u >= _u.front() && u <= _u.back();
    const double uu[4] = {inTable ? _u[iu] : uq, inTable ? _u[iu + 1] : uq, inTable ? _u[iu] : uq, inTable ? _u[iu + 1] : uq};
    for (int i = 0; i < 4; ++i) {
        double b = 0.;
        for (int j = 0; j < 4; ++j) {
            b += w[j] * k[j]->a[i] * uu[j];
        }
        out->a[i] = b / uq;
    }
    {
        double b = 0.;
        for (int j = 0; j < 4; ++j) {
            b += w[j] * k[j]->shift * uu[j];
        }
        out->shift = b / uq;
    }
    out->energy = w[0] * k[0]->energy + w[1] * k[1]->energy + w[2] * k[2]->energy + w[3] * k[3]->energy;
    out->centroid = w[0] * k[0]->centroid + w[1] * k[1]->centroid + w[2] * k[2]->centroid + w[3] * k[3]->centroid;
    // Profile: the densities blend (they stay positive), then become nested-disc weights.
    const int nb = TracedKernel::kRings;
    double d[TracedKernel::kRings + 1];
    for (int i = 0; i < nb; ++i) {
        d[i] = w[0] * k[0]->density[i] + w[1] * k[1]->density[i] + w[2] * k[2]->density[i] + w[3] * k[3]->density[i];
    }
    d[nb] = 0.;
    for (int i = 0; i < nb; ++i) {
        out->ringS[i] = (i + 1.) / nb;
        out->ringW[i] = d[i] - d[i + 1];
    }
    out->nRings = nb;
    out->ringScale = out->ringS;
    out->ringWeight = out->ringW;
    blendClips(k, w, out);
}

} // namespace defocus

namespace defocus {

bool
LensTable::tracePoint(double u, double rho, int channel, int grid, std::vector<double>* hx, std::vector<double>* hy) const
{
    if (!_sampler) {
        return false;
    }
    PointRays pr;
    if (_sampler(std::max(0, std::min(2, channel)), u, std::max(0., rho) * _p.fieldRadiusMm, grid, &pr) != 0) {
        return false;
    }
    hx->clear();
    hy->clear();
    for (size_t i = 0; i < pr.val.size(); ++i) {
        if (pr.val[i] && !pr.blk[i] && pr.sx[i] * pr.sx[i] + pr.sy[i] * pr.sy[i] <= 1.) {
            hx->push_back(pr.hx[i]);
            hy->push_back(pr.hy[i]);
        }
    }
    return true;
}

} // namespace defocus
