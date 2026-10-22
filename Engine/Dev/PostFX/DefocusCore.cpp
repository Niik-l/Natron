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

#include "DefocusCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace defocus {

namespace {

const double kPi = 3.14159265358979323846;
const int kNumCh = 5;          // r, g, b, a, guess
const int kStripRows = 32;     // full-res output rows per scatter work item
const double kSliceR0 = 4.;    // px; slice spacing is relative above this radius

typedef std::chrono::steady_clock Clock;

double secondsSince(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------------------------
// Threading. Only the calling thread calls aborted(): Natron's abort check reads thread-local
// render state, so worker threads just watch the shared stop flag.

int threadCount(int requested)
{
    if (requested > 0) {
        return requested;
    }
    unsigned h = std::thread::hardware_concurrency();
    return h ? (int)h : 4;
}

bool parallelFor(int n, int grain, int threads,
                 const std::function<bool()>& aborted,
                 const std::function<void(int, int)>& fn)
{
    if (n <= 0) {
        return true;
    }
    grain = std::max(1, grain);
    const int chunks = (n + grain - 1) / grain;
    threads = std::max(1, std::min(threads, chunks));
    std::atomic<int> next(0);
    std::atomic<bool> stop(false);
    auto worker = [&](bool isCaller) {
        for (;;) {
            if (stop.load(std::memory_order_relaxed)) {
                return;
            }
            if (isCaller && aborted && aborted()) {
                stop = true;
                return;
            }
            const int c = next.fetch_add(1);
            if (c >= chunks) {
                return;
            }
            const int b = c * grain;
            fn(b, std::min(n, b + grain));
        }
    };
    if (threads == 1) {
        worker(true);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads - 1);
        for (int i = 0; i < threads - 1; ++i) {
            pool.emplace_back(worker, false);
        }
        worker(true);
        for (size_t i = 0; i < pool.size(); ++i) {
            pool[i].join();
        }
    }
    return !stop.load();
}

// ---------------------------------------------------------------------------------------------
// CoC model. r(1/d) = rInf * (1 - S/d): linear in inverse depth, negative in front of focus.

struct CocModel
{
    double rInf = 0.;       // render-scale px radius at infinity
    double focus = 1.;      // scene units
    double maxR = 1.;       // render-scale px
    double pxPerMm = 1.;    // render-scale px per mm on the film back

    explicit CocModel(const Settings& s)
    {
        focus = std::max(1e-9, s.focusDistance);
        pxPerMm = s.formatWidthPx * s.renderScale / std::max(1e-9, s.filmbackWidthMm);
        if (s.cocMode == eCocSizeAtInfinity) {
            rInf = s.sizeAtInfinityPx * s.renderScale;
        } else {
            const double f = std::max(1e-6, s.focalLengthMm);
            const double N = std::max(1e-3, s.fNumber);
            const double S = std::max(focus * s.worldToMm, f * 1.0001);
            const double cInfMm = f * f / (N * (S - f));   // diameter on the film back
            rInf = 0.5 * cInfMm * pxPerMm;
        }
        maxR = std::max(0.5, s.maxRadiusPx * s.renderScale);
    }

    double radius(double invDepth) const { return rInf * (1. - focus * invDepth); }
};

// ---------------------------------------------------------------------------------------------
// Aperture shapes. The unit aperture is a convex region inside the unit disc, described by
// constraints (half-planes for straight blades, discs for curved blades or a round iris).
// A sample's kernel maps unit coordinates to pixel offsets with a 2x2 matrix.

struct Constraint
{
    bool disc;
    double nx, ny, h;   // half-plane n.u <= h
    double cx, cy, R;   // disc |u - c| <= R
};

struct KernelShape
{
    double m[4];        // pixel = [m0 m1; m2 m3] * unit
    double inv[4];
    double yExtent;     // max |pixel.y| over the unit disc
    bool clip;
    double clipX, clipY;// cat's-eye clip disc (unit coords, radius 1)
    int nRings;
    const double* ringScale;
    const double* ringWeight;
    double energy;      // optical vignetting: light lost to the clip
    bool point;
};

class ProceduralLens
{
public:

    explicit ProceduralLens(const LensSettings& l)
        : _l(l)
    {
        if (l.blades >= 3) {
            const int n = std::min(16, l.blades);
            const double half = kPi / n;
            const double curv = std::max(0., std::min(1., l.curvature));
            const double rot = l.rotationDeg * kPi / 180.;
            for (int k = 0; k < n; ++k) {
                const double a = rot + (k + 0.5) * 2. * half;
                Constraint c;
                c.nx = std::cos(a);
                c.ny = std::sin(a);
                if (curv < 1e-3) {
                    c.disc = false;
                    c.h = std::cos(half);
                    c.cx = c.cy = c.R = 0.;
                } else {
                    // Arc through the two polygon vertices, bulging outwards; curvature 1 = unit circle.
                    const double R = 1. / curv;
                    const double sh = std::sin(half);
                    const double d = std::cos(half) - std::sqrt(std::max(0., R * R - sh * sh));
                    c.disc = true;
                    c.cx = d * c.nx;
                    c.cy = d * c.ny;
                    c.R = R;
                    c.h = 0.;
                }
                _constraints.push_back(c);
            }
        } else {
            Constraint c;
            c.disc = true;
            c.cx = c.cy = 0.;
            c.R = 1.;
            c.nx = c.ny = c.h = 0.;
            _constraints.push_back(c);
        }

        const double q = std::max(0.05, l.squeeze);
        _sqX = 1. / std::sqrt(q);
        _sqY = std::sqrt(q);

        buildRings(+1, _farScale, _farWeight);
        buildRings(-1, _nearScale, _nearWeight);
    }

    const std::vector<Constraint>& constraints() const { return _constraints; }

    // r: signed radius in grid px; (fx, fy): field position, 1 = frame corner.
    void shape(double r, double fx, double fy, KernelShape& out) const
    {
        const double rad = std::fabs(r);
        double m0 = rad * _sqX, m1 = 0., m2 = 0., m3 = rad * _sqY;
        const double rho = std::sqrt(fx * fx + fy * fy);
        const double c = rho > 1e-9 ? fx / rho : 1.;
        const double s = rho > 1e-9 ? fy / rho : 0.;
        const double sgn = r < 0. ? -1. : 1.;

        if (_l.swirl != 0. && rho > 1e-6) {
            // Stretch along the tangential direction: R(phi) diag(1, t) R(-phi).
            const double t = std::max(0.25, std::min(4., 1. + _l.swirl * rho * rho * sgn));
            const double a00 = c * c + t * s * s, a01 = (1. - t) * c * s, a11 = s * s + t * c * c;
            const double n0 = a00 * m0 + a01 * m2, n1 = a00 * m1 + a01 * m3;
            const double n2 = a01 * m0 + a11 * m2, n3 = a01 * m1 + a11 * m3;
            m0 = n0; m1 = n1; m2 = n2; m3 = n3;
        }
        if (r < 0.) {
            // Near-field defocus is the aperture point-reflected relative to far-field defocus.
            m0 = -m0; m1 = -m1; m2 = -m2; m3 = -m3;
        }
        out.m[0] = m0; out.m[1] = m1; out.m[2] = m2; out.m[3] = m3;
        const double det = m0 * m3 - m1 * m2;
        // Below ~0.1 px the disc stays inside its own pixel anyway. (A larger cut-off makes a visible
        // step where a depth ramp crosses it: discs spill into neighbours, points don't.)
        out.point = std::sqrt(std::fabs(det)) < 0.1;
        if (!out.point) {
            out.inv[0] = m3 / det; out.inv[1] = -m1 / det;
            out.inv[2] = -m2 / det; out.inv[3] = m0 / det;
        }
        out.yExtent = std::sqrt(m2 * m2 + m3 * m3);

        out.clip = false;
        out.energy = 1.;
        if (_l.catEye > 0. && rho > _l.catEyeStart) {
            const double o = std::min(1.9, _l.catEye * (rho - _l.catEyeStart) / std::max(1e-6, 1. - _l.catEyeStart));
            if (o > 1e-4) {
                out.clip = true;
                out.clipX = o * c;
                out.clipY = o * s;
                // Two unit discs offset by o: the light kept, which darkens the frame edges.
                out.energy = (2. * std::acos(o / 2.) - (o / 2.) * std::sqrt(4. - o * o)) / kPi;
            }
        }

        if (r < 0.) {
            out.nRings = (int)_nearScale.size();
            out.ringScale = _nearScale.data();
            out.ringWeight = _nearWeight.data();
        } else {
            out.nRings = (int)_farScale.size();
            out.ringScale = _farScale.data();
            out.ringWeight = _farWeight.data();
        }
    }

private:

    double profile(double rho, double sgn) const
    {
        double p = 1.;
        if (_l.sphericalAberration != 0.) {
            p += _l.sphericalAberration * sgn * (rho * rho - 0.5) * 2.;
        }
        if (_l.onionAmount != 0.) {
            p *= 1. + _l.onionAmount * std::sin(2. * kPi * _l.onionFrequency * rho);
        }
        return std::max(0., p);
    }

    // Staircase approximation of the radial profile as nested scaled copies of the aperture.
    void buildRings(double sgn, std::vector<double>& scale, std::vector<double>& weight) const
    {
        int n = 1;
        if (_l.sphericalAberration != 0. || _l.onionAmount != 0.) {
            n = std::max(2, _l.profileSteps);
            if (_l.onionAmount != 0.) {
                n = std::max(n, (int)std::ceil(4. * _l.onionFrequency));
            }
            n = std::min(n, 64);
        }
        std::vector<double> p(n + 1, 0.);
        for (int j = 0; j < n; ++j) {
            p[j] = n == 1 ? 1. : profile((j + 0.5) / n, sgn);
        }
        for (int j = 0; j < n; ++j) {
            const double w = p[j] - p[j + 1];
            if (std::fabs(w) > 1e-9) {
                scale.push_back((j + 1.) / n);
                weight.push_back(w);
            }
        }
        if (scale.empty()) {
            scale.push_back(1.);
            weight.push_back(1.);
        }
    }

    LensSettings _l;
    std::vector<Constraint> _constraints;
    double _sqX = 1., _sqY = 1.;
    std::vector<double> _nearScale, _nearWeight, _farScale, _farWeight;
};

// Clip the parametric line u(t) = a t + b against the aperture scaled by sc, and the optional
// clip disc. Returns false if empty.
inline bool clipLine(const std::vector<Constraint>& cons, double sc,
                     const KernelShape& k,
                     double ax, double ay, double bx, double by,
                     double& t0, double& t1)
{
    t0 = -std::numeric_limits<double>::infinity();
    t1 = std::numeric_limits<double>::infinity();
    const double aa = ax * ax + ay * ay;
    for (size_t i = 0; i < cons.size(); ++i) {
        const Constraint& c = cons[i];
        if (c.disc) {
            const double ox = bx - sc * c.cx, oy = by - sc * c.cy;
            const double B = 2. * (ax * ox + ay * oy);
            const double C = ox * ox + oy * oy - sc * sc * c.R * c.R;
            const double disc = B * B - 4. * aa * C;
            if (disc <= 0.) {
                return false;
            }
            const double sq = std::sqrt(disc);
            t0 = std::max(t0, (-B - sq) / (2. * aa));
            t1 = std::min(t1, (-B + sq) / (2. * aa));
        } else {
            const double kk = c.nx * ax + c.ny * ay;
            const double mm = sc * c.h - (c.nx * bx + c.ny * by);
            if (std::fabs(kk) < 1e-12) {
                if (mm < 0.) {
                    return false;
                }
            } else if (kk > 0.) {
                t1 = std::min(t1, mm / kk);
            } else {
                t0 = std::max(t0, mm / kk);
            }
        }
        if (t0 >= t1) {
            return false;
        }
    }
    if (k.clip) {
        const double ox = bx - k.clipX, oy = by - k.clipY;
        const double B = 2. * (ax * ox + ay * oy);
        const double C = ox * ox + oy * oy - 1.;
        const double disc = B * B - 4. * aa * C;
        if (disc <= 0.) {
            return false;
        }
        const double sq = std::sqrt(disc);
        t0 = std::max(t0, (-B - sq) / (2. * aa));
        t1 = std::min(t1, (-B + sq) / (2. * aa));
    }
    return t0 < t1;
}

// ---------------------------------------------------------------------------------------------
// Row-difference scatter buffer. Each row stores (w + 1) cells of kNumCh floats; adding v to cells
// [i0, i1] costs two writes, and a prefix sum along the row resolves it.

struct SpanBuffer
{
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<float> d;
    int touchLo = 0, touchHi = -1;   // touched rows

    void setup(int ax0, int ay0, int aw, int ah)
    {
        x0 = ax0; y0 = ay0; w = aw; h = ah;
        const size_t need = (size_t)(w + 1) * h * kNumCh;
        if (d.size() < need) {
            d.assign(need, 0.f);
        }
        touchLo = h; touchHi = -1;
    }

    float* row(int y) { return d.data() + (size_t)(y - y0) * (w + 1) * kNumCh; }

    void clearTouched()
    {
        if (touchHi >= touchLo) {
            std::memset(d.data() + (size_t)touchLo * (w + 1) * kNumCh, 0,
                        sizeof(float) * (size_t)(touchHi - touchLo + 1) * (w + 1) * kNumCh);
        }
        touchLo = h; touchHi = -1;
    }

    void touch(int ry)
    {
        touchLo = std::min(touchLo, ry);
        touchHi = std::max(touchHi, ry);
    }

    static inline void addRange(float* r, int i0, int i1, const float* v, double s)
    {
        float* a = r + (size_t)i0 * kNumCh;
        float* b = r + (size_t)(i1 + 1) * kNumCh;
        for (int c = 0; c < kNumCh; ++c) {
            const float x = (float)(v[c] * s);
            a[c] += x;
            b[c] -= x;
        }
    }

    // Adds v * coverage to the cells covered by [X0, X1] (buffer-relative x, cell i = [i-.5, i+.5]).
    void span(int ry, double X0, double X1, const float* v, double s)
    {
        if (X1 <= -0.5 || X0 >= w - 0.5 || X1 <= X0) {
            return;
        }
        float* r = d.data() + (size_t)ry * (w + 1) * kNumCh;
        const int c0 = (int)std::floor(X0 + 0.5);
        const int c1 = (int)std::floor(X1 + 0.5);
        if (c0 == c1) {
            if (c0 >= 0 && c0 < w) {
                addRange(r, c0, c0, v, s * (X1 - X0));
            }
        } else {
            if (c0 >= 0) {
                addRange(r, c0, c0, v, s * (c0 + 0.5 - X0));
            }
            if (c1 < w) {
                addRange(r, c1, c1, v, s * (X1 - (c1 - 0.5)));
            }
            const int m0 = std::max(0, c0 + 1), m1 = std::min(w - 1, c1 - 1);
            if (m0 <= m1) {
                addRange(r, m0, m1, v, s);
            }
        }
        touch(ry);
    }

    void point(int ry, int cx, const float* v, double s)
    {
        if (cx < 0 || cx >= w) {
            return;
        }
        addRange(d.data() + (size_t)ry * (w + 1) * kNumCh, cx, cx, v, s);
        touch(ry);
    }

    // Prefix-sum the touched rows in place: cell i then holds its value.
    void resolve()
    {
        for (int y = touchLo; y <= touchHi; ++y) {
            float* r = d.data() + (size_t)y * (w + 1) * kNumCh;
            float acc[kNumCh] = {0.f, 0.f, 0.f, 0.f, 0.f};
            for (int i = 0; i < w; ++i) {
                for (int c = 0; c < kNumCh; ++c) {
                    acc[c] += r[(size_t)i * kNumCh + c];
                    r[(size_t)i * kNumCh + c] = acc[c];
                }
            }
            for (int c = 0; c < kNumCh; ++c) {
                r[(size_t)w * kNumCh + c] = 0.f;
            }
        }
    }
};

struct Interval
{
    int dy;
    double t0, t1, w;
};

// Scatter one sample centred at buffer coordinates (bx, by) (buffer-relative, in buffer pixels).
// The kernel is normalised to unit energy (times the shape's vignetting energy).
void scatterSample(SpanBuffer& buf, std::vector<Interval>& scratch,
                   const ProceduralLens& lens, const KernelShape& k,
                   double bx, double by, const float* v)
{
    if (k.point) {
        const int cx = (int)std::floor(bx + 0.5);
        const int cy = (int)std::floor(by + 0.5);
        if (cy >= 0 && cy < buf.h) {
            buf.point(cy, cx, v, k.energy);
        }
        return;
    }
    const double ext = k.yExtent;
    // Sub-rows per pixel row: small kernels need many, or neighbours with slightly different radii
    // (a depth ramp) distribute energy unevenly between rows and the sum ripples by a few percent.
    // Kept constant over wide radius bands: a count that changes with every radius is itself a
    // source of row-to-row ripple.
    const int ns = ext < 2. ? 16 : ext < 8. ? 8 : ext < 32. ? 4 : 2;
    // Rows whose cells intersect [by - ext, by + ext].
    const int yLo = (int)std::floor(by - ext + 0.5);
    const int yHi = (int)std::floor(by + ext + 0.5);
    if (yHi < 0 || yLo >= buf.h) {
        // Still need the area for normalisation only if something is written; nothing is.
        return;
    }
    const std::vector<Constraint>& cons = lens.constraints();
    const double ax = k.inv[0], ay = k.inv[2];
    scratch.clear();
    double area = 0.;
    for (int y = yLo; y <= yHi; ++y) {
        for (int j = 0; j < ns; ++j) {
            const double yy = (y - 0.5 + (j + 0.5) / ns) - by;
            if (yy < -ext || yy > ext) {
                continue;
            }
            const double bxu = yy * k.inv[1], byu = yy * k.inv[3];
            for (int ring = 0; ring < k.nRings; ++ring) {
                double t0, t1;
                if (!clipLine(cons, k.ringScale[ring], k, ax, ay, bxu, byu, t0, t1)) {
                    continue;
                }
                const double w = k.ringWeight[ring] / ns;
                area += w * (t1 - t0);
                if (y >= 0 && y < buf.h) {
                    Interval iv;
                    iv.dy = y;
                    iv.t0 = t0;
                    iv.t1 = t1;
                    iv.w = w;
                    scratch.push_back(iv);
                }
            }
        }
    }
    if (area <= 1e-9) {
        const int cx = (int)std::floor(bx + 0.5);
        const int cy = (int)std::floor(by + 0.5);
        if (cy >= 0 && cy < buf.h) {
            buf.point(cy, cx, v, k.energy);
        }
        return;
    }
    const double norm = k.energy / area;
    for (size_t i = 0; i < scratch.size(); ++i) {
        const Interval& iv = scratch[i];
        buf.span(iv.dy, bx + iv.t0, bx + iv.t1, v, iv.w * norm);
    }
}

// ---------------------------------------------------------------------------------------------
// Samples

struct PixSample
{
    float r;        // signed CoC radius, render-scale px (green channel)
    float w;        // presence: fraction of the pixel this sample covers
    float c[4];     // premultiplied RGBA (already scaled by presence)
    bool hi;        // scatter at full resolution (highlight)
};

struct Sample
{
    int x, y;       // grid cell (level grid) or full-res pixel
    float r;        // render-scale px
    float c[kNumCh];
};

struct Slice
{
    int index = 0;
    int level = 0;
    float maxAbsR = 0.f;
    std::vector<Sample> cells;  // level-grid samples
    std::vector<Sample> hi;     // full-res highlight samples
};

inline int sliceIndexOf(double r, double step)
{
    const double s = std::log1p(std::fabs(r) / kSliceR0) / std::log1p(step);
    const int i = (int)std::floor(s + 0.5);
    return r < 0. ? -i : i;
}

inline float luminance(const float* c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

bool lessYX(const Sample& a, const Sample& b)
{
    return a.y < b.y || (a.y == b.y && a.x < b.x);
}

} // anonymous namespace

// ---------------------------------------------------------------------------------------------

double
signedCocRadiusAtDepth(const Settings& s, double depthUnits)
{
    Settings t = s;
    t.renderScale = 1.;
    CocModel m(t);
    if (!(depthUnits > 0.) || !std::isfinite(depthUnits)) {
        return m.rInf;
    }
    return m.radius(1. / depthUnits);
}

double
decodeDepthUnits(const Settings& s, double v)
{
    const double inf = std::numeric_limits<double>::infinity();
    if (!std::isfinite(v)) {
        return inf;
    }
    switch (s.depthConvention) {
    case eDepthDirectCoC:
        return std::numeric_limits<double>::quiet_NaN();
    case eDepthInverse:
        return v != 0. ? 1. / std::fabs(v) : inf;
    case eDepthNormalized: {
        double zn = s.normalizedInvert ? 1. - v : v;
        zn = std::max(0., std::min(1., zn));
        return s.normalizedNear + zn * (s.normalizedFar - s.normalizedNear);
    }
    case eDepthRadial:
    case eDepthPlanarZ:
    default: {
        const double z = std::fabs(v);
        if ((s.zeroIsInfinity && z <= 0.) || (s.farThreshold > 0. && z >= s.farThreshold)) {
            return inf;
        }
        return z;
    }
    }
}

double
maxPaddingPx(const Settings& s)
{
    const double q = std::max(0.05, s.lens.squeeze);
    const double sq = std::sqrt(std::max(q, 1. / q));
    const double sw = 1. + std::max(0., std::fabs(s.lens.swirl));
    return s.maxRadiusPx * s.renderScale * sq * std::min(4., sw)
           + std::fabs(s.lens.axialCA) * s.renderScale + 2.;
}

bool
render(const Inputs& in,
       const Settings& s,
       const ImageView& output,
       const ImageView* errorMap,
       Stats* statsOut,
       const std::function<bool()>& aborted)
{
    Stats stats;
    const int nThreads = threadCount(s.threads);
    const Clock::time_point tDecode = Clock::now();

    const CocModel coc(s);
    const ProceduralLens lens(s.lens);
    const double rs = s.renderScale;
    const double caPx = s.lens.axialCA * rs;

    const ConstImageView& src = in.source;
    if (!src.valid() || !output.valid()) {
        return true;
    }
    const int srcCh = src.channels;


    auto srcAt = [&](int x, int y, float* c) {
        if (!src.bounds.contains(x, y)) {
            if (s.border != eBorderRepeat) {
                c[0] = c[1] = c[2] = c[3] = 0.f;
                return;
            }
            x = std::max(src.bounds.x1, std::min(src.bounds.x2 - 1, x));
            y = std::max(src.bounds.y1, std::min(src.bounds.y2 - 1, y));
        }
        const float* p = src.pixel(x, y);
        c[0] = p[0];
        c[1] = srcCh > 1 ? p[1] : p[0];
        c[2] = srcCh > 2 ? p[2] : p[0];
        c[3] = srcCh > 3 ? p[3] : 1.f;
    };

    // ---- 1. depth -> signed radius per source pixel
    const Rect& SB = src.bounds;
    const int Sw = SB.width(), Sh = SB.height();
    std::vector<float> rSrc((size_t)Sw * Sh, 0.f);
    std::vector<unsigned char> clampedSrc((size_t)Sw * Sh, 0);
    const ConstImageView& dep = in.depth;
    const double fMm = std::max(1e-6, s.focalLengthMm);

    parallelFor(Sh, 16, nThreads, aborted, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            const int y = SB.y1 + j;
            for (int i = 0; i < Sw; ++i) {
                const int x = SB.x1 + i;
                const int dx = x, dy = y;
                double v = std::numeric_limits<double>::quiet_NaN();
                if (dep.valid() && dep.bounds.contains(dx, dy)) {
                    v = dep.pixel(dx, dy)[in.depthChannel];
                    if (s.depthPremultiplied) {
                        float c[4];
                        srcAt(dx, dy, c);
                        if (c[3] > 1e-6f) {
                            v /= c[3];
                        }
                    }
                }
                double r;
                if (s.depthConvention == eDepthDirectCoC) {
                    r = std::isfinite(v) ? v * rs : 0.;
                } else {
                    double inv = 0.;    // 1/depth, 0 = infinity
                    if (std::isfinite(v)) {
                        switch (s.depthConvention) {
                        case eDepthInverse:
                            inv = std::fabs(v);
                            break;
                        case eDepthNormalized: {
                            double zn = s.normalizedInvert ? 1. - v : v;
                            zn = std::max(0., std::min(1., zn));
                            const double d = s.normalizedNear + zn * (s.normalizedFar - s.normalizedNear);
                            inv = d > 0. ? 1. / d : 0.;
                            break;
                        }
                        case eDepthRadial:
                        case eDepthPlanarZ:
                        default: {
                            double z = std::fabs(v);
                            const bool inf = (s.zeroIsInfinity && z <= 0.) || (s.farThreshold > 0. && z >= s.farThreshold);
                            if (!inf && z > 0.) {
                                if (s.depthConvention == eDepthRadial) {
                                    const double xm = (x - s.opticalCentreX) / coc.pxPerMm;
                                    const double ym = (y - s.opticalCentreY) / coc.pxPerMm;
                                    z *= fMm / std::sqrt(fMm * fMm + xm * xm + ym * ym);
                                }
                                inv = 1. / z;
                            }
                            break;
                        }
                        }
                    }
                    r = coc.radius(inv);
                }
                r *= r < 0. ? s.nearGain : s.farGain;
                const size_t p = (size_t)j * Sw + i;
                if (std::fabs(r) > coc.maxR) {
                    r = r < 0. ? -coc.maxR : coc.maxR;
                    clampedSrc[p] = 1;
                }
                rSrc[p] = (float)r;
            }
        }
    });
    if (aborted && aborted()) {
        return false;
    }
    float maxAbsR = 0.f;
    for (size_t p = 0; p < rSrc.size(); ++p) {
        maxAbsR = std::max(maxAbsR, std::fabs(rSrc[p]));
    }

    // Domain: the source window, padded for Repeat so border pixels can scatter inwards. The pad
    // follows the largest radius actually present, not Max Radius.
    Rect D = SB;
    if (s.border == eBorderRepeat) {
        Settings t = s;
        t.renderScale = 1.;
        t.maxRadiusPx = maxAbsR;
        t.lens.axialCA = s.lens.axialCA * rs;
        const int pad = (int)std::ceil(maxPaddingPx(t));
        D = Rect(D.x1 - pad, D.y1 - pad, D.x2 + pad, D.y2 + pad);
    }
    const int Dw = D.width(), Dh = D.height();
    const size_t N = (size_t)Dw * Dh;
    std::vector<float> rPix(N, 0.f);
    std::vector<unsigned char> clampedPix(N, 0);
    parallelFor(Dh, 64, nThreads, aborted, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            const int sy = std::max(0, std::min(Sh - 1, j + D.y1 - SB.y1));
            for (int i = 0; i < Dw; ++i) {
                const int sx = std::max(0, std::min(Sw - 1, i + D.x1 - SB.x1));
                rPix[(size_t)j * Dw + i] = rSrc[(size_t)sy * Sw + sx];
                clampedPix[(size_t)j * Dw + i] = clampedSrc[(size_t)sy * Sw + sx];
            }
        }
    });
    if (aborted && aborted()) {
        return false;
    }

    // ---- 2. samples, with depth-edge reconstruction
    // Edge pixels: the CoC varies by more than the threshold across their 3x3.
    const float edgeThr = (float)(s.edgeThresholdPx * rs);
    std::vector<unsigned char> isEdge(N, 0);
    if (s.edgeMode != eEdgeOff) {
        parallelFor(Dh, 16, nThreads, aborted, [&](int b, int e) {
            for (int j = b; j < e; ++j) {
                for (int i = 0; i < Dw; ++i) {
                    float lo = rPix[(size_t)j * Dw + i], hi = lo;
                    for (int jj = std::max(0, j - 1); jj <= std::min(Dh - 1, j + 1); ++jj) {
                        for (int ii = std::max(0, i - 1); ii <= std::min(Dw - 1, i + 1); ++ii) {
                            const float rq = rPix[(size_t)jj * Dw + ii];
                            lo = std::min(lo, rq);
                            hi = std::max(hi, rq);
                        }
                    }
                    isEdge[(size_t)j * Dw + i] = (hi - lo) > edgeThr ? 1 : 0;
                }
            }
        });
    }
    // Compact samples: most pixels are one whole sample (colour = the source pixel, radius in rS);
    // pixels split at a depth edge keep both parts in `splits`. Sample ids: p is a pixel's first
    // sample, N + k the second part of split k.
    struct SplitRec
    {
        size_t p;
        PixSample s[2];
    };
    std::vector<float> rS(N, 0.f);
    std::vector<unsigned char> nSamp(N, 0);
    std::vector<int> splitIdx(N, -1);
    std::vector<std::vector<SplitRec> > splitChunks((Dh + 15) / 16);
    std::vector<float> edgeA(N, -1.f);

    parallelFor(Dh, 16, nThreads, aborted, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            const int y = D.y1 + j;
            for (int i = 0; i < Dw; ++i) {
                const int x = D.x1 + i;
                const size_t p = (size_t)j * Dw + i;
                float C[4];
                srcAt(x, y, C);
                if (C[0] == 0.f && C[1] == 0.f && C[2] == 0.f && C[3] == 0.f) {
                    continue;
                }
                const float rOwn = rPix[p];
                PixSample out[2];
                float rFront = rOwn, rBack = rOwn;
                if (s.edgeMode != eEdgeOff) {
                    for (int dj = -1; dj <= 1; ++dj) {
                        const int jj = j + dj;
                        if (jj < 0 || jj >= Dh) {
                            continue;
                        }
                        for (int di = -1; di <= 1; ++di) {
                            const int ii = i + di;
                            if (ii < 0 || ii >= Dw) {
                                continue;
                            }
                            const float rq = rPix[(size_t)jj * Dw + ii];
                            rFront = std::min(rFront, rq);
                            rBack = std::max(rBack, rq);
                        }
                    }
                }
                const float delta = rBack - rFront;
                if (s.edgeMode == eEdgeOff || delta <= s.edgeThresholdPx * rs) {
                    out[0].r = rOwn;
                    out[0].w = 1.f;
                    std::memcpy(out[0].c, C, sizeof(C));
                    out[0].hi = false;
                    rS[p] = out[0].r;
                    nSamp[p] = 1;
                    continue;
                }
                if (s.edgeMode == eEdgeMinMax) {
                    // Snap the pixel to the nearer extreme: kills depths floating between objects.
                    out[0].r = (rOwn - rFront) <= (rBack - rOwn) ? rFront : rBack;
                    out[0].w = 1.f;
                    std::memcpy(out[0].c, C, sizeof(C));
                    out[0].hi = false;
                    rS[p] = out[0].r;
                    nSamp[p] = 1;
                    continue;
                }
                // Unmix: estimate the front coverage from the colours on either side of the edge,
                // taken from clean (non-edge) pixels in the 5x5, so anti-aliased neighbours that are
                // themselves mixtures don't pollute the estimate. Falls back to any 3x3 neighbour.
                double Cf[4] = {0, 0, 0, 0}, Cb[4] = {0, 0, 0, 0};
                int nf = 0, nb = 0;
                const float tol = 0.25f * delta;
                for (int pass = 0; pass < 2 && (nf == 0 || nb == 0); ++pass) {
                    const int rad = pass == 0 ? 2 : 1;
                    for (int c = 0; c < 4; ++c) {
                        Cf[c] = Cb[c] = 0.;
                    }
                    nf = nb = 0;
                    for (int dj = -rad; dj <= rad; ++dj) {
                        const int jj = j + dj;
                        if (jj < 0 || jj >= Dh) {
                            continue;
                        }
                        for (int di = -rad; di <= rad; ++di) {
                            const int ii = i + di;
                            if ((di == 0 && dj == 0) || ii < 0 || ii >= Dw) {
                                continue;
                            }
                            const size_t q = (size_t)jj * Dw + ii;
                            if (pass == 0 && isEdge[q]) {
                                continue;
                            }
                            const float rq = rPix[q];
                            float Q[4];
                            srcAt(D.x1 + ii, D.y1 + jj, Q);
                            if (std::fabs(rq - rFront) <= tol) {
                                for (int c = 0; c < 4; ++c) Cf[c] += Q[c];
                                ++nf;
                            } else if (std::fabs(rBack - rq) <= tol) {
                                for (int c = 0; c < 4; ++c) Cb[c] += Q[c];
                                ++nb;
                            }
                        }
                    }
                }
                double a = -1.;
                if (nb > 0) {
                    double bMag = 0.;
                    for (int c = 0; c < 4; ++c) {
                        bMag = std::max(bMag, std::fabs(Cb[c] / nb));
                    }
                    if (bMag < 1e-5) {
                        // Empty behind (CG element over nothing): the edge pixel belongs to the front.
                        out[0].r = rFront;
                        out[0].w = 1.f;
                        std::memcpy(out[0].c, C, sizeof(C));
                        out[0].hi = false;
                        rS[p] = out[0].r;
                    nSamp[p] = 1;
                        continue;
                    }
                }
                if (nf > 0 && nb > 0) {
                    double dd = 0., num = 0., mag = 1e-12;
                    for (int c = 0; c < 4; ++c) {
                        Cf[c] /= nf;
                        Cb[c] /= nb;
                        const double d = Cf[c] - Cb[c];
                        dd += d * d;
                        num += (C[c] - Cb[c]) * d;
                        mag += Cf[c] * Cf[c] + Cb[c] * Cb[c];
                    }
                    if (dd > 1e-4 * mag) {
                        a = std::max(0., std::min(1., num / dd));
                    }
                }
                if (a < 0.) {
                    // Colours do not separate: keep the pixel whole at its own depth.
                    out[0].r = rOwn;
                    out[0].w = 1.f;
                    std::memcpy(out[0].c, C, sizeof(C));
                    out[0].hi = false;
                    rS[p] = out[0].r;
                    nSamp[p] = 1;
                    continue;
                }
                edgeA[p] = (float)a;
                if (a > 0.98 || a < 0.02) {
                    out[0].r = a > 0.5 ? rFront : rBack;
                    out[0].w = 1.f;
                    std::memcpy(out[0].c, C, sizeof(C));
                    out[0].hi = false;
                    rS[p] = out[0].r;
                    nSamp[p] = 1;
                    continue;
                }
                // Front part takes the front colour; fall back to a proportional split if the
                // remainder would go negative.
                float F[4], B[4];
                bool ok = true;
                for (int c = 0; c < 4; ++c) {
                    F[c] = (float)(a * Cf[c]);
                    B[c] = C[c] - F[c];
                    if (B[c] < -1e-4f * (std::fabs(C[c]) + 1e-3f)) {
                        ok = false;
                    }
                }
                if (!ok) {
                    for (int c = 0; c < 4; ++c) {
                        F[c] = (float)(a * C[c]);
                        B[c] = C[c] - F[c];
                    }
                }
                float bMax = 0.f, cMax = 0.f;
                for (int c = 0; c < 4; ++c) {
                    bMax = std::max(bMax, std::fabs(B[c]));
                    cMax = std::max(cMax, std::fabs(C[c]));
                }
                if (bMax <= 1e-4f * cMax) {
                    // Nothing behind (e.g. a CG element over an empty background): one front sample.
                    out[0].r = rFront;
                    out[0].w = (float)a;
                    std::memcpy(out[0].c, C, sizeof(C));
                    out[0].hi = false;
                    rS[p] = out[0].r;
                    nSamp[p] = 1;
                    continue;
                }
                out[0].r = rFront;
                out[0].w = (float)a;
                std::memcpy(out[0].c, F, sizeof(F));
                out[0].hi = false;
                out[1].r = rBack;
                out[1].w = (float)(1. - a);
                std::memcpy(out[1].c, B, sizeof(B));
                out[1].hi = false;
                nSamp[p] = 2;
                SplitRec sr;
                sr.p = p;
                sr.s[0] = out[0];
                sr.s[1] = out[1];
                splitChunks[b / 16].push_back(sr);
            }
        }
    });
    if (aborted && aborted()) {
        return false;
    }
    std::vector<SplitRec> splits;
    for (size_t c = 0; c < splitChunks.size(); ++c) {
        for (size_t k = 0; k < splitChunks[c].size(); ++k) {
            splitIdx[splitChunks[c][k].p] = (int)splits.size();
            splits.push_back(splitChunks[c][k]);
        }
    }
    std::vector<std::vector<SplitRec> >().swap(splitChunks);
    const size_t nIds = N + splits.size();
    std::vector<unsigned char> hiFlag(nIds, 0);
    auto sampleOf = [&](size_t p, int k) -> size_t { return k == 0 ? p : N + (size_t)splitIdx[p]; };
    auto pixelOf = [&](size_t id) -> size_t { return id < N ? id : splits[id - N].p; };
    auto sampleR = [&](size_t id) -> float {
        if (id >= N) {
            return splits[id - N].s[1].r;
        }
        return splitIdx[id] >= 0 ? splits[splitIdx[id]].s[0].r : rS[id];
    };
    auto sampleW = [&](size_t id) -> float {
        if (id >= N) {
            return splits[id - N].s[1].w;
        }
        return splitIdx[id] >= 0 ? splits[splitIdx[id]].s[0].w : 1.f;
    };
    auto sampleColour = [&](size_t id, float* c) {
        if (id >= N) {
            std::memcpy(c, splits[id - N].s[1].c, sizeof(float) * 4);
        } else if (splitIdx[id] >= 0) {
            std::memcpy(c, splits[splitIdx[id]].s[0].c, sizeof(float) * 4);
        } else {
            srcAt(D.x1 + (int)(id % Dw), D.y1 + (int)(id / Dw), c);
        }
    };

    // ---- debug outputs that need no scatter
    if (s.output != eOutputResult) {
        const Rect& O = output.bounds;
        for (int y = O.y1; y < O.y2; ++y) {
            for (int x = O.x1; x < O.x2; ++x) {
                float* o = output.pixel(x, y);
                o[0] = o[1] = o[2] = 0.f;
                if (output.channels > 3) {
                    o[3] = 1.f;
                }
                if (!D.contains(x, y)) {
                    continue;
                }
                const size_t p = (size_t)(y - D.y1) * Dw + (x - D.x1);
                const float r = rPix[p];
                switch (s.output) {
                case eOutputFocus:
                    o[0] = r < 0.f ? std::min(1.f, -r / 8.f) : 0.f;
                    o[1] = std::max(0.f, 1.f - std::fabs(r));
                    o[2] = r > 0.f ? std::min(1.f, r / 8.f) : 0.f;
                    break;
                case eOutputSignedCoC:
                    o[0] = (float)(r / rs);
                    o[1] = (float)(std::fabs(r) / rs);
                    o[2] = clampedPix[p] ? 1.f : 0.f;
                    break;
                case eOutputSlices: {
                    const int k = sliceIndexOf(r, s.sliceStep);
                    const unsigned h = (unsigned)(k * 2654435761u);
                    o[0] = ((h >> 0) & 255) / 255.f;
                    o[1] = ((h >> 8) & 255) / 255.f;
                    o[2] = ((h >> 16) & 255) / 255.f;
                    if (k == 0) {
                        o[0] = o[1] = o[2] = 0.5f;
                    }
                    break;
                }
                case eOutputEdge:
                    o[0] = o[1] = o[2] = edgeA[p] < 0.f ? 0.f : edgeA[p];
                    if (edgeA[p] < 0.f) {
                        o[2] = 0.f;
                    }
                    break;
                default:
                    break;
                }
            }
        }
        if (errorMap && errorMap->valid()) {
            for (int y = errorMap->bounds.y1; y < errorMap->bounds.y2; ++y) {
                std::memset(errorMap->pixel(errorMap->bounds.x1, y), 0,
                            sizeof(float) * errorMap->bounds.width() * errorMap->channels);
            }
        }
        stats.secondsDecode = secondsSince(tDecode);
        if (statsOut) {
            *statsOut = stats;
        }
        return true;
    }

    // ---- 3. highlights (capped to the brightest maxHighlights)
    const double fullR = std::max(1., s.fullResRadius);
    if (s.highlightThreshold > 0.) {
        std::vector<std::pair<float, size_t> > cand;
        for (size_t p = 0; p < N; ++p) {
            for (int k = 0; k < nSamp[p]; ++k) {
                const size_t id = sampleOf(p, k);
                if (std::fabs(sampleR(id)) <= fullR) {
                    continue;
                }
                float c[4];
                sampleColour(id, c);
                const float l = luminance(c);
                if (l > s.highlightThreshold) {
                    cand.push_back(std::make_pair(l, id));
                }
            }
        }
        if ((int)cand.size() > s.maxHighlights) {
            std::nth_element(cand.begin(), cand.begin() + s.maxHighlights, cand.end(),
                             [](const std::pair<float, size_t>& a, const std::pair<float, size_t>& b) { return a.first > b.first; });
            cand.resize(s.maxHighlights);
        }
        for (size_t i = 0; i < cand.size(); ++i) {
            hiFlag[cand[i].second] = 1;
        }
        stats.highlights = (std::int64_t)cand.size();
    }

    // ---- 4. bucket samples into slices (slice index computed once per sample)
    std::vector<short> sIdx(nIds, 0);
    parallelFor(Dh, 64, nThreads, aborted, [&](int b, int e) {
        for (size_t p = (size_t)b * Dw; p < (size_t)e * Dw; ++p) {
            for (int k = 0; k < nSamp[p]; ++k) {
                const size_t id = sampleOf(p, k);
                sIdx[id] = (short)sliceIndexOf(sampleR(id), s.sliceStep);
            }
        }
    });
    int kMin = 0, kMax = 0;
    bool any = false;
    for (size_t p = 0; p < N; ++p) {
        for (int k = 0; k < nSamp[p]; ++k) {
            const int si = sIdx[sampleOf(p, k)];
            if (!any) {
                kMin = kMax = si;
                any = true;
            } else {
                kMin = std::min(kMin, si);
                kMax = std::max(kMax, si);
            }
            ++stats.samples;
            if (k == 1) {
                ++stats.edgeSplits;
            }
        }
    }
    for (size_t p = 0; p < clampedSrc.size(); ++p) {
        stats.clamped += clampedSrc[p];
    }
    const int nSlices = any ? kMax - kMin + 1 : 0;
    std::vector<int> counts(nSlices + 1, 0);
    for (size_t p = 0; p < N; ++p) {
        for (int k = 0; k < nSamp[p]; ++k) {
            ++counts[sIdx[sampleOf(p, k)] - kMin + 1];
        }
    }
    for (int i = 0; i < nSlices; ++i) {
        counts[i + 1] += counts[i];
    }
    std::vector<size_t> order(counts.empty() ? 0 : counts[nSlices]);
    {
        std::vector<int> fillPos(counts.begin(), counts.end());
        for (size_t p = 0; p < N; ++p) {
            for (int k = 0; k < nSamp[p]; ++k) {
                const int si = sIdx[sampleOf(p, k)] - kMin;
                order[fillPos[si]++] = sampleOf(p, k);   // scan order within a slice
            }
        }
    }
    stats.secondsDecode = secondsSince(tDecode);
    if (aborted && aborted()) {
        return false;
    }

    // ---- 5. per slice (nearest first): level choice, background fill, aggregation
    const Clock::time_point tFill = Clock::now();
    std::vector<Slice> slices;
    slices.reserve(nSlices);
    std::vector<float> H(N, 0.f), Rocc(N, 0.f), Rfront(N, std::numeric_limits<float>::quiet_NaN());
    float maxRocc = 0.f;
    Rect holeBox;   // full-res domain-relative bbox of H > 0
    bool haveHole = false;
    const ConstImageView& clean = in.cleanBackground;

    for (int si = 0; si < nSlices; ++si) {
        if (aborted && aborted()) {
            return false;
        }
        const int b = counts[si], e = counts[si + 1];
        if (b == e) {
            continue;
        }
        Slice sl;
        sl.index = si + kMin;
        for (int q = b; q < e; ++q) {
            sl.maxAbsR = std::max(sl.maxAbsR, std::fabs(sampleR(order[q])));
        }
        int L = 0;
        while (L < 4 && sl.maxAbsR / (double)(1 << L) > fullR) {
            ++L;
        }
        sl.level = L;
        const int sc = 1 << L;
        const int gw = (Dw + sc - 1) / sc, gh = (Dh + sc - 1) / sc;

        // Present cells bbox
        int px1 = gw, py1 = gh, px2 = -1, py2 = -1;
        for (int q = b; q < e; ++q) {
            const size_t p = pixelOf(order[q]);
            const int ci = (int)(p % Dw) / sc, cj = (int)(p / Dw) / sc;
            px1 = std::min(px1, ci); px2 = std::max(px2, ci);
            py1 = std::min(py1, cj); py2 = std::max(py2, cj);
        }
        const int band = (int)std::ceil((maxRocc + sl.maxAbsR) / sc) + 2;
        Rect region(std::max(0, px1 - band), std::max(0, py1 - band),
                    std::min(gw, px2 + 1 + band), std::min(gh, py2 + 1 + band));
        const int rw = region.width(), rh = region.height();
        const size_t RN = (size_t)rw * rh;

        // Aggregate this slice's samples into region cells (all levels: L0 cells are pixels).
        std::vector<float> W(RN, 0.f), P(RN * 4, 0.f), Rs(RN, 0.f);
        const double inv = 1. / ((double)sc * sc);
        for (int q = b; q < e; ++q) {
            PixSample ps;
            ps.r = sampleR(order[q]);
            ps.w = sampleW(order[q]);
            sampleColour(order[q], ps.c);
            ps.hi = hiFlag[order[q]] != 0;
            const size_t p = pixelOf(order[q]);
            const int pi = (int)(p % Dw), pj = (int)(p / Dw);
            if (ps.hi) {
                Sample hs;
                hs.x = D.x1 + pi;
                hs.y = D.y1 + pj;
                hs.r = ps.r;
                std::memcpy(hs.c, ps.c, sizeof(ps.c));
                hs.c[4] = 0.f;
                sl.hi.push_back(hs);
                continue;
            }
            const size_t c = (size_t)(pj / sc - region.y1) * rw + (pi / sc - region.x1);
            W[c] += (float)(ps.w * inv);
            for (int k = 0; k < 4; ++k) {
                P[c * 4 + k] += (float)(ps.c[k] * inv);
            }
            Rs[c] += ps.w * ps.r;
        }

        // Background fill behind nearer slices.
        std::vector<Sample> fills;
        const bool regionHits = haveHole &&
                                !(holeBox.x2 / sc + 1 < region.x1 || holeBox.x1 / sc > region.x2 ||
                                  holeBox.y2 / sc + 1 < region.y1 || holeBox.y1 / sc > region.y2);
        if (s.fillBehind && regionHits) {
            // Occluders and clean plate per cell.
            std::vector<float> Hc(RN, 0.f), RoccC(RN, 0.f), RfrontC(RN, std::numeric_limits<float>::quiet_NaN());
            std::vector<float> cleanC(clean.valid() ? RN * 4 : 0, 0.f);
            std::vector<unsigned char> cleanOk(clean.valid() ? RN : 0, 0);
            for (int j = 0; j < rh; ++j) {
                for (int i = 0; i < rw; ++i) {
                    const size_t c = (size_t)j * rw + i;
                    const int gi = region.x1 + i, gj = region.y1 + j;
                    double hSum = 0., rFrontSum = 0., cb[4] = {0, 0, 0, 0};
                    int nPix = 0, nFront = 0, nClean = 0;
                    float rOcc = 0.f;
                    for (int yy = gj * sc; yy < std::min(Dh, gj * sc + sc); ++yy) {
                        for (int xx = gi * sc; xx < std::min(Dw, gi * sc + sc); ++xx) {
                            const size_t p = (size_t)yy * Dw + xx;
                            ++nPix;
                            hSum += H[p];
                            rOcc = std::max(rOcc, Rocc[p]);
                            if (!std::isnan(Rfront[p])) {
                                rFrontSum += Rfront[p];
                                ++nFront;
                            }
                            if (clean.valid() && clean.bounds.contains(D.x1 + xx, D.y1 + yy)) {
                                const float* cp = clean.pixel(D.x1 + xx, D.y1 + yy);
                                for (int k = 0; k < 4; ++k) {
                                    cb[k] += (k < clean.channels) ? cp[k] : (k == 3 ? 1.f : 0.f);
                                }
                                ++nClean;
                            }
                        }
                    }
                    if (nPix) {
                        Hc[c] = (float)(hSum / nPix);
                    }
                    RoccC[c] = rOcc;
                    if (nFront) {
                        RfrontC[c] = (float)(rFrontSum / nFront);
                    }
                    if (nClean) {
                        for (int k = 0; k < 4; ++k) {
                            cleanC[c * 4 + k] = (float)(cb[k] / nClean);
                        }
                        cleanOk[c] = 1;
                    }
                }
            }

            // Chamfer distance + nearest seed cell. Seeds are cells of this slice at least one cell
            // away from any nearer content: with an aliased depth pass the pixels right at the edge
            // carry the foreground's anti-aliased colour, and extending them streaks it inwards.
            std::vector<float> dist(RN, std::numeric_limits<float>::infinity());
            std::vector<int> srcIdx(RN, -1);
            int nSeeds = 0;
            for (int pass = 0; pass < 2 && nSeeds == 0; ++pass) {
                for (int j = 0; j < rh; ++j) {
                    for (int i = 0; i < rw; ++i) {
                        const size_t c = (size_t)j * rw + i;
                        if (W[c] <= 0.f) {
                            continue;
                        }
                        bool seed = true;
                        if (pass == 0) {
                            seed = W[c] > 0.5f;
                            for (int dj = -1; dj <= 1 && seed; ++dj) {
                                for (int di = -1; di <= 1 && seed; ++di) {
                                    const int ii = i + di, jj = j + dj;
                                    if (ii >= 0 && jj >= 0 && ii < rw && jj < rh && Hc[(size_t)jj * rw + ii] > 1e-3f) {
                                        seed = false;
                                    }
                                }
                            }
                        }
                        if (seed) {
                            dist[c] = 0.f;
                            srcIdx[c] = (int)c;
                            ++nSeeds;
                        }
                    }
                }
            }
            const float d1 = 1.f, d2 = 1.41421356f;
            auto relax = [&](size_t c, int ni, int nj, float d) {
                if (ni < 0 || nj < 0 || ni >= rw || nj >= rh) {
                    return;
                }
                const size_t n = (size_t)nj * rw + ni;
                if (dist[n] + d < dist[c]) {
                    dist[c] = dist[n] + d;
                    srcIdx[c] = srcIdx[n];
                }
            };
            for (int j = 0; j < rh; ++j) {
                for (int i = 0; i < rw; ++i) {
                    const size_t c = (size_t)j * rw + i;
                    relax(c, i - 1, j, d1); relax(c, i, j - 1, d1);
                    relax(c, i - 1, j - 1, d2); relax(c, i + 1, j - 1, d2);
                }
            }
            for (int j = rh - 1; j >= 0; --j) {
                for (int i = rw - 1; i >= 0; --i) {
                    const size_t c = (size_t)j * rw + i;
                    relax(c, i + 1, j, d1); relax(c, i, j + 1, d1);
                    relax(c, i + 1, j + 1, d2); relax(c, i - 1, j + 1, d2);
                }
            }
            for (int j = 0; j < rh; ++j) {
                for (int i = 0; i < rw; ++i) {
                    const size_t c = (size_t)j * rw + i;
                    if (srcIdx[c] < 0) {
                        continue;
                    }
                    if (std::isnan(RfrontC[c])) {
                        continue;
                    }
                    const double h = std::min((double)Hc[c], 1. - (double)W[c]);
                    if (h < 1e-3) {
                        continue;
                    }
                    const int sidx = srcIdx[c];
                    const double rSrc = Rs[sidx] / std::max(1e-12f, W[sidx] / (float)inv);
                    if (dist[c] > (RoccC[c] + std::fabs(rSrc)) / sc + 2.) {
                        continue;
                    }
                    Sample f;
                    f.x = region.x1 + i;
                    f.y = region.y1 + j;
                    f.r = (float)rSrc;
                    if (!cleanOk.empty() && cleanOk[c]) {
                        for (int k = 0; k < 4; ++k) {
                            f.c[k] = (float)(cleanC[c * 4 + k] * h);
                        }
                        f.c[4] = 0.f;
                    } else {
                        const double wSrc = W[sidx];
                        for (int k = 0; k < 4; ++k) {
                            f.c[k] = (float)(P[(size_t)sidx * 4 + k] / wSrc * h);
                        }
                        const double rF = RfrontC[c];
                        const double gap = std::fabs(rF - rSrc);
                        const bool disc = gap > std::max(1.5 * rs, 0.2 * std::max(std::fabs(rF), std::fabs(rSrc)));
                        f.c[4] = disc ? (float)h : 0.f;
                        if (!disc) {
                            // Same continuous surface split across slices: the fill stands in for
                            // the occluder itself, so give it the occluder's radius. A radius
                            // mismatch at the junction leaves coverage slightly off 1 (darkening).
                            f.r = (float)rF;
                        }
                    }
                    fills.push_back(f);
                }
            }
        }
        stats.filled += (std::int64_t)fills.size();

        // Original cells.
        for (int j = 0; j < rh; ++j) {
            for (int i = 0; i < rw; ++i) {
                const size_t c = (size_t)j * rw + i;
                if (W[c] <= 0.f && P[c * 4] == 0.f && P[c * 4 + 1] == 0.f && P[c * 4 + 2] == 0.f) {
                    continue;
                }
                Sample o;
                o.x = region.x1 + i;
                o.y = region.y1 + j;
                o.r = W[c] > 0.f ? (float)(Rs[c] / (W[c] / inv)) : sl.maxAbsR;
                for (int k = 0; k < 4; ++k) {
                    o.c[k] = P[c * 4 + k];
                }
                o.c[4] = 0.f;
                sl.cells.push_back(o);
            }
        }
        sl.cells.insert(sl.cells.end(), fills.begin(), fills.end());
        std::sort(sl.cells.begin(), sl.cells.end(), lessYX);
        std::sort(sl.hi.begin(), sl.hi.end(), lessYX);

        // This slice now occludes everything farther.
        for (int q = b; q < e; ++q) {
            PixSample ps;
            ps.r = sampleR(order[q]);
            ps.w = sampleW(order[q]);
            const size_t p = pixelOf(order[q]);
            if (std::isnan(Rfront[p])) {
                Rfront[p] = ps.r;
            }
            H[p] += ps.w;
            Rocc[p] = std::max(Rocc[p], std::fabs(ps.r));
            maxRocc = std::max(maxRocc, std::fabs(ps.r));
            const int pi = (int)(p % Dw), pj = (int)(p / Dw);
            if (!haveHole) {
                holeBox = Rect(pi, pj, pi + 1, pj + 1);
                haveHole = true;
            } else {
                holeBox.x1 = std::min(holeBox.x1, pi); holeBox.x2 = std::max(holeBox.x2, pi + 1);
                holeBox.y1 = std::min(holeBox.y1, pj); holeBox.y2 = std::max(holeBox.y2, pj + 1);
            }
        }
        stats.maxAbsRadius = std::max(stats.maxAbsRadius, (double)sl.maxAbsR);
        slices.push_back(std::move(sl));
    }
    stats.slices = (int)slices.size();
    stats.secondsFill = secondsSince(tFill);

    // ---- 6. scatter per output strip, back to front
    const Clock::time_point tScatter = Clock::now();
    const Rect O = output.bounds;
    const int Ow = O.width(), Oh = O.height();
    const int nStrips = (Oh + kStripRows - 1) / kStripRows;
    const double cx0 = s.opticalCentreX, cy0 = s.opticalCentreY;
    const double invField = 1. / std::max(1e-6, s.fieldRadiusPx);

    // Scatter one sample (grid coords) with optional per-channel CoC (axial CA).
    auto scatterWithCA = [&](SpanBuffer& buf, std::vector<Interval>& scratch, const Sample& smp,
                             double gx, double gy, double fullX, double fullY, double gridScale) {
        const double fx = (fullX - cx0) * invField, fy = (fullY - cy0) * invField;
        KernelShape k;
        if (caPx == 0.) {
            lens.shape(smp.r / gridScale, fx, fy, k);
            scatterSample(buf, scratch, lens, k, gx, gy, smp.c);
            return;
        }
        float v[kNumCh];
        const double rr[3] = {smp.r + caPx, smp.r, smp.r - caPx};
        for (int ch = 0; ch < 3; ++ch) {
            for (int c = 0; c < kNumCh; ++c) {
                v[c] = 0.f;
            }
            v[ch] = smp.c[ch];
            if (ch == 1) {
                v[3] = smp.c[3];
                v[4] = smp.c[4];
            }
            lens.shape(rr[ch] / gridScale, fx, fy, k);
            scatterSample(buf, scratch, lens, k, gx, gy, v);
        }
    };

    auto lowerY = [](const std::vector<Sample>& v, int y) {
        return std::lower_bound(v.begin(), v.end(), y, [](const Sample& a, int yy) { return a.y < yy; });
    };

    const bool okScatter = parallelFor(nStrips, 1, nThreads, aborted, [&](int b, int e) {
        SpanBuffer full;
        SpanBuffer lvl[5];
        std::vector<Interval> scratch;
        std::vector<float> acc, sl;
        for (int st = b; st < e; ++st) {
            const int y0 = O.y1 + st * kStripRows;
            const int y1 = std::min(O.y2, y0 + kStripRows);
            const int sh = y1 - y0;
            acc.assign((size_t)Ow * sh * kNumCh, 0.f);
            sl.assign((size_t)Ow * sh * kNumCh, 0.f);
            full.setup(O.x1, y0, Ow, sh);

            for (int k = (int)slices.size() - 1; k >= 0; --k) {
                const Slice& S = slices[k];
                const int L = S.level;
                const int sc = 1 << L;
                const double maxR = S.maxAbsR * 4. + std::fabs(caPx) + 2.;  // generous: swirl / squeeze
                bool touched = false;

                // Highlights and level-0 cells: full-res buffer.
                auto scatterFull = [&](const std::vector<Sample>& v, bool gridIsLevel) {
                    const int yLo = (int)std::floor(y0 - maxR) - 1, yHi = (int)std::ceil(y1 + maxR) + 1;
                    for (auto it = lowerY(v, gridIsLevel ? yLo - D.y1 : yLo); it != v.end(); ++it) {
                        const int sy = gridIsLevel ? it->y + D.y1 : it->y;
                        if (sy > yHi) {
                            break;
                        }
                        const int sx = gridIsLevel ? it->x + D.x1 : it->x;
                        scatterWithCA(full, scratch, *it, sx - O.x1, sy - y0, sx, sy, 1.);
                        touched = true;
                    }
                };
                if (!S.hi.empty()) {
                    scatterFull(S.hi, false);
                }
                if (L == 0) {
                    scatterFull(S.cells, true);
                } else if (!S.cells.empty()) {
                    // Level buffer covering the strip for bilinear upsampling.
                    auto lu = [&](double v, int origin) { return (v - origin + 0.5) / sc - 0.5; };
                    const int ilo = (int)std::floor(lu(O.x1, D.x1)), ihi = (int)std::floor(lu(O.x2 - 1, D.x1)) + 1;
                    const int jlo = (int)std::floor(lu(y0, D.y1)), jhi = (int)std::floor(lu(y1 - 1, D.y1)) + 1;
                    SpanBuffer& lb = lvl[L];
                    lb.setup(ilo, jlo, ihi - ilo + 1, jhi - jlo + 1);
                    const double maxRl = maxR / sc + 1.;
                    const int yLo = (int)std::floor(jlo - maxRl), yHi = (int)std::ceil(jhi + maxRl);
                    bool lt = false;
                    for (auto it = lowerY(S.cells, yLo); it != S.cells.end(); ++it) {
                        if (it->y > yHi) {
                            break;
                        }
                        const double fullX = D.x1 + (it->x + 0.5) * sc - 0.5;
                        const double fullY = D.y1 + (it->y + 0.5) * sc - 0.5;
                        scatterWithCA(lb, scratch, *it, it->x - ilo, it->y - jlo, fullX, fullY, sc);
                        lt = true;
                    }
                    if (lt) {
                        lb.resolve();
                        // Bilinear upsample into the slice strip.
                        for (int y = y0; y < y1; ++y) {
                            const double v = lu(y, D.y1) - jlo;
                            int j0 = (int)std::floor(v);
                            const float fy = (float)(v - j0);
                            j0 = std::max(0, std::min(lb.h - 2, j0));
                            const float* r0 = lb.row(lb.y0 + j0);
                            const float* r1 = lb.row(lb.y0 + j0 + 1);
                            float* dst = sl.data() + (size_t)(y - y0) * Ow * kNumCh;
                            for (int x = O.x1; x < O.x2; ++x) {
                                const double u = lu(x, D.x1) - ilo;
                                int i0 = (int)std::floor(u);
                                const float fx = (float)(u - i0);
                                i0 = std::max(0, std::min(lb.w - 2, i0));
                                const float* a0 = r0 + (size_t)i0 * kNumCh;
                                const float* a1 = r1 + (size_t)i0 * kNumCh;
                                float* o = dst + (size_t)(x - O.x1) * kNumCh;
                                for (int c = 0; c < kNumCh; ++c) {
                                    const float top = a0[c] + (a0[c + kNumCh] - a0[c]) * fx;
                                    const float bot = a1[c] + (a1[c + kNumCh] - a1[c]) * fx;
                                    o[c] += top + (bot - top) * fy;
                                }
                            }
                        }
                        lb.touchLo = 0;
                        lb.touchHi = lb.h - 1;
                        lb.clearTouched();
                        touched = true;
                    }
                }
                if (!touched) {
                    continue;
                }
                if (full.touchHi >= full.touchLo) {
                    full.resolve();
                    for (int ry = full.touchLo; ry <= full.touchHi; ++ry) {
                        const float* r = full.row(y0 + ry);
                        float* dst = sl.data() + (size_t)ry * Ow * kNumCh;
                        for (size_t q = 0; q < (size_t)Ow * kNumCh; ++q) {
                            dst[q] += r[q];
                        }
                    }
                    full.clearTouched();
                }
                // Composite this slice over everything behind it. Coverage above 1 inside one slice
                // means the background fill overlapped the slice's own pixels (same surface counted
                // twice at a slice boundary), so normalise it back to full coverage.
                for (size_t q = 0; q < (size_t)Ow * sh; ++q) {
                    float* o = acc.data() + q * kNumCh;
                    float* v = sl.data() + q * kNumCh;
                    if (v[3] > 1.f) {
                        const float n = 1.f / v[3];
                        for (int c = 0; c < kNumCh; ++c) {
                            v[c] *= n;
                        }
                    }
                    const float t = 1.f - std::max(0.f, v[3]);
                    for (int c = 0; c < kNumCh; ++c) {
                        o[c] = v[c] + o[c] * t;
                        v[c] = 0.f;
                    }
                }
            }

            // Write: mix / mask against the source, error map.
            for (int y = y0; y < y1; ++y) {
                for (int x = O.x1; x < O.x2; ++x) {
                    const float* a = acc.data() + ((size_t)(y - y0) * Ow + (x - O.x1)) * kNumCh;
                    float res[4] = {a[0], a[1], a[2], a[3]};
                    double amount = s.mix;
                    if (in.mask.valid()) {
                        amount *= in.mask.bounds.contains(x, y)
                                  ? std::max(0.f, std::min(1.f, in.mask.pixel(x, y)[in.maskChannel])) : 0.f;
                    }
                    if (amount < 1.) {
                        float c[4] = {0.f, 0.f, 0.f, 0.f};
                        if (src.bounds.contains(x, y)) {
                            srcAt(x, y, c);
                        }
                        for (int k = 0; k < 4; ++k) {
                            res[k] = (float)(c[k] + (res[k] - c[k]) * amount);
                        }
                    }
                    float* o = output.pixel(x, y);
                    const int oc = std::min(4, output.channels);
                    for (int k = 0; k < oc; ++k) {
                        o[k] = res[k];
                    }
                    if (errorMap && errorMap->valid() && errorMap->bounds.contains(x, y)) {
                        float* em = errorMap->pixel(x, y);
                        em[0] = std::max(0.f, std::min(1.f, a[4]));
                        em[1] = (D.contains(x, y) && clampedPix[(size_t)(y - D.y1) * Dw + (x - D.x1)]) ? 1.f : 0.f;
                        if (errorMap->channels > 2) {
                            em[2] = 0.f;
                        }
                        if (errorMap->channels > 3) {
                            em[3] = 1.f;
                        }
                    }
                }
            }
        }
    });
    stats.secondsScatter = secondsSince(tScatter);
    if (statsOut) {
        *statsOut = stats;
    }
    return okScatter;
}

} // namespace defocus
