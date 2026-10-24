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
#include "DefocusGpu.h"
#include "LensTrace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

// DEFOCUS_TRACE (testbed builds only): DEFOCUS_TRACE_X / DEFOCUS_TRACE_Y env vars name one pixel
// whose per-slice coverage is printed to stderr while compositing.
#ifdef DEFOCUS_TRACE
#include <cstdio>
#include <cstdlib>
#endif

namespace defocus {

namespace {

const double kPi = 3.14159265358979323846;
// Scatter buffers: r, g, b, a, guess, then the coverage of the red and the blue kernels. With
// axial colour (or a traced lens's per-channel kernels) red and blue are blurred by different
// kernels than green, so each composites over the slices behind with its own coverage; sharing
// green's alpha leaves coloured lines wherever a surface spans several depth slices.
const int kNumCh = 7;
const int kSampleCh = 5;       // per sample: r, g, b, a, guess
const int kAlphaR = 5, kAlphaB = 6;
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

// Inside a parallel job: the job's stop flag (every thread) and the abort check (calling thread
// only). Long work items poll stopRequested() so an abort (node deleted, knob changed) doesn't
// wait for every item in flight to finish.
thread_local std::atomic<bool>* tlsStop = nullptr;
thread_local const std::function<bool()>* tlsAbort = nullptr;

bool stopRequested()
{
    if (tlsAbort && *tlsAbort && (*tlsAbort)()) {
        if (tlsStop) {
            tlsStop->store(true);
        }
        return true;
    }
    return tlsStop && tlsStop->load(std::memory_order_relaxed);
}

// Sets the thread's stop flag / abort check for a scope.
struct StopScope
{
    std::atomic<bool>* prevStop;
    const std::function<bool()>* prevAbort;
    StopScope(std::atomic<bool>* stop, const std::function<bool()>* abort)
        : prevStop(tlsStop), prevAbort(tlsAbort)
    {
        tlsStop = stop;
        tlsAbort = abort;
    }
    ~StopScope()
    {
        tlsStop = prevStop;
        tlsAbort = prevAbort;
    }
};

int threadCount(int requested)
{
    if (requested > 0) {
        return requested;
    }
    unsigned h = std::thread::hardware_concurrency();
    return h ? (int)h : 4;
}

// Workers kept alive for one render() call. The fill runs several parallel loops per depth slice,
// and with hundreds of slices starting threads for each loop costs more than the work.
class WorkerPool
{
public:
    explicit WorkerPool(int threads)
    {
        for (int i = 0; i < threads - 1; ++i) {
            _threads.emplace_back([this]() { workerLoop(); });
        }
    }

    ~WorkerPool()
    {
        {
            std::lock_guard<std::mutex> l(_m);
            _quit = true;
        }
        _wake.notify_all();
        for (size_t i = 0; i < _threads.size(); ++i) {
            _threads[i].join();
        }
    }

    int size() const { return (int)_threads.size() + 1; }

    // Runs fn(chunk) for every chunk on the workers and the calling thread. Returns false if
    // aborted() (called on the calling thread only) stopped it.
    bool run(int chunks, const std::function<void(int)>& fn, const std::function<bool()>& aborted)
    {
        {
            std::lock_guard<std::mutex> l(_m);
            _job = &fn;
            _chunks = chunks;
            _next = 0;
            _stop = false;
            _busy = (int)_threads.size();
            ++_generation;
        }
        _wake.notify_all();
        {
            StopScope scope(&_stop, &aborted);
            for (;;) {
                if (_stop.load() || (aborted && aborted())) {
                    _stop = true;
                    break;
                }
                const int c = _next.fetch_add(1);
                if (c >= chunks) {
                    break;
                }
                fn(c);
            }
        }
        std::unique_lock<std::mutex> l(_m);
        _idle.wait(l, [this]() { return _busy == 0; });
        _job = nullptr;
        return !_stop.load();
    }

private:
    void workerLoop()
    {
        StopScope scope(&_stop, nullptr);
        unsigned seen = 0;
        for (;;) {
            const std::function<void(int)>* job;
            int chunks;
            {
                std::unique_lock<std::mutex> l(_m);
                _wake.wait(l, [&]() { return _quit || _generation != seen; });
                if (_quit) {
                    return;
                }
                seen = _generation;
                job = _job;
                chunks = _chunks;
            }
            while (!_stop.load(std::memory_order_relaxed)) {
                const int c = _next.fetch_add(1);
                if (c >= chunks) {
                    break;
                }
                (*job)(c);
            }
            {
                std::lock_guard<std::mutex> l(_m);
                --_busy;
            }
            _idle.notify_all();
        }
    }

    std::vector<std::thread> _threads;
    std::mutex _m;
    std::condition_variable _wake, _idle;
    const std::function<void(int)>* _job = nullptr;
    int _chunks = 0;
    int _busy = 0;
    unsigned _generation = 0;
    bool _quit = false;
    std::atomic<int> _next{0};
    std::atomic<bool> _stop{false};
};

// The pool of the render() running on this thread, if any (Natron renders frames concurrently:
// each render() call has its own).
thread_local WorkerPool* tlsPool = nullptr;

bool parallelFor(int n, int grain, int threads,
                 const std::function<bool()>& aborted,
                 const std::function<void(int, int)>& fn)
{
    if (n <= 0) {
        return true;
    }
    grain = std::max(1, grain);
    const int chunks = (n + grain - 1) / grain;
    if (tlsPool && tlsPool->size() > 1 && chunks > 1) {
        return tlsPool->run(chunks, [&](int c) {
            const int b = c * grain;
            fn(b, std::min(n, b + grain));
        }, aborted);
    }
    threads = std::max(1, std::min(threads, chunks));
    std::atomic<int> next(0);
    std::atomic<bool> stop(false);
    auto worker = [&](bool isCaller) {
        StopScope scope(&stop, isCaller ? &aborted : nullptr);
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
    bool sizeAtInfinity = false;
    double fMm = 50., nStop = 2.8, worldToMm = 1000.;

    explicit CocModel(const Settings& s)
    {
        focus = std::max(1e-9, s.focusDistance);
        pxPerMm = s.formatWidthPx * s.renderScale / std::max(1e-9, s.filmbackWidthMm);
        sizeAtInfinity = s.cocMode == eCocSizeAtInfinity;
        fMm = std::max(1e-6, s.focalLengthMm);
        nStop = std::max(1e-3, s.fNumber);
        worldToMm = s.worldToMm;
        maxR = std::max(0.5, s.maxRadiusPx * s.renderScale);
        rInf = sizeAtInfinity ? s.sizeAtInfinityPx * s.renderScale : rInfAt(focus);
    }

    // Radius at infinity when focused at S (scene units).
    double rInfAt(double S) const
    {
        if (sizeAtInfinity) {
            return rInf;
        }
        const double Smm = std::max(S * worldToMm, fMm * 1.0001);
        const double cInfMm = fMm * fMm / (nStop * (Smm - fMm));   // diameter on the film back
        return 0.5 * cInfMm * pxPerMm;
    }

    double radius(double invDepth) const { return rInf * (1. - focus * invDepth); }
    double radiusAt(double invDepth, double S) const { return rInfAt(S) * (1. - S * invDepth); }
};

// Pinhole projection for pixel rays: the camera's when there is one, else the lens knobs (focal
// length on the film back) about the optical centre.
struct Projection
{
    double fpx = 1., cx = 0., cy = 0.;

    explicit Projection(const Settings& s)
    {
        if (s.camera.valid) {
            fpx = s.camera.focalPx;
            cx = s.camera.centreX;
            cy = s.camera.centreY;
        } else {
            fpx = std::max(1e-6, s.focalLengthMm) * s.formatWidthPx * s.renderScale / std::max(1e-9, s.filmbackWidthMm);
            cx = s.opticalCentreX;
            cy = s.opticalCentreY;
        }
        fpx = std::max(1e-6, fpx);
    }
    // Camera-space ray (u, v, -1) through the centre of pixel (x, y).
    double u(double x) const { return (x + 0.5 - cx) / fpx; }
    double v(double y) const { return (y + 0.5 - cy) / fpx; }
};

// Focus distance per pixel: constant, or where the pixel's ray meets the tilted focus plane. In
// camera space the plane is depth = S + tan(tilt) * (Y - Yq) + tan(swing) * (X - Xq), through the
// pivot pixel's ray at depth S; along the ray (u Z, v Z, Z) that solves to num / den below.
struct FocusField
{
    bool tilted = false;
    double S = 1., tt = 0., ts = 0., num = 1.;
    Projection proj;

    explicit FocusField(const Settings& s)
        : proj(s)
    {
        S = std::max(1e-9, s.focusDistance);
        tt = std::tan(std::max(-89.9, std::min(89.9, s.focusTiltDeg)) * kPi / 180.);
        ts = std::tan(std::max(-89.9, std::min(89.9, s.focusSwingDeg)) * kPi / 180.);
        tilted = tt != 0. || ts != 0.;
        const double uq = proj.u(s.focusPivotX - 0.5), vq = proj.v(s.focusPivotY - 0.5);
        num = S * (1. - tt * vq - ts * uq);
    }

    double at(double x, double y) const
    {
        if (!tilted) {
            return S;
        }
        const double den = 1. - tt * proj.v(y) - ts * proj.u(x);
        if (den <= 1e-9 || num <= 0.) {
            return 1e12;    // the ray never meets the plane in front: focused past infinity
        }
        return std::max(1e-9, std::min(1e12, num / den));
    }
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
    int nClip;          // clip discs (unit coords): cat's eye, or the rims of a traced lens
    double clipX[4], clipY[4], clipR[4];
    int nRings;
    const double* ringScale;
    const double* ringWeight;
    double ringS[17], ringW[17];   // storage when the rings are built per sample (traced lens)
    double energy;      // optical vignetting: light lost to the clip
    double offX, offY;  // kernel centre offset in pixels (coma of a traced lens)
    bool point;
};

class ProceduralLens
{
public:

    // rInf: thin-lens radius at infinity (render-scale px), to place a sample in a traced lens's
    // table.
    explicit ProceduralLens(const LensSettings& l, double rInf = 0.)
        : _l(l)
        , _rInf(rInf)
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

    // Separate kernels per colour channel (a traced chromatic lens).
    bool perChannel() const { return _l.table && _l.chromatic && _l.character > 0.; }

    // How far a traced kernel reaches, over the thin-lens radius, at defocus u and field radius rho
    // (1 for the procedural lens). Near focus it can be many times the thin-lens disc (field
    // curvature, astigmatism), which the fill behind nearer slices has to cover.
    double extentScale(double u, double rho) const
    {
        if (!_l.table) {
            return 1.;
        }
        const double ch = std::max(0., std::min(1., _l.character));
        double e = 1.;
        for (int c = 0; c < 3; c += perChannel() ? 1 : 3) {
            KernelLookup L;
            _l.table->lookup(u, rho, perChannel() ? c : 1, &L);
            const double A[4] = {ch * L.a[0] - (1. - ch), ch * L.a[1], ch * L.a[2], ch * L.a[3] - (1. - ch)};
            const double E = 0.5 * (A[0] + A[3]), F = 0.5 * (A[0] - A[3]), G = 0.5 * (A[2] + A[1]), H = 0.5 * (A[2] - A[1]);
            const double sMax = std::sqrt(E * E + H * H) + std::sqrt(F * F + G * G);
            e = std::max(e, sMax);
        }
        return std::min(e, 64.);
    }

    // r: signed radius in grid px; (fx, fy): field position, 1 = frame corner; u: the sample's
    // radius over the radius at infinity (traced lens); channel: 0 R, 1 G, 2 B.
    void shape(double r, double fx, double fy, KernelShape& out, double u = 0., int channel = 1) const
    {
        out.offX = out.offY = 0.;
        if (_l.table) {
            tracedShape(r, fx, fy, u, channel, out);
            return;
        }
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

        out.nClip = 0;
        out.energy = 1.;
        if (_l.catEye > 0. && rho > _l.catEyeStart) {
            const double o = std::min(1.9, _l.catEye * (rho - _l.catEyeStart) / std::max(1e-6, 1. - _l.catEyeStart));
            if (o > 1e-4) {
                out.nClip = 1;
                out.clipX[0] = o * c;
                out.clipY[0] = o * s;
                out.clipR[0] = 1.;
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

    // Traced lens: the table's kernel for this defocus, field position and channel, turned from the
    // field frame (x towards the frame edge) into the image, blended with an ideal thin lens by
    // Character (ideal = the traced convention: the stop image is inverted, a = -1).
    void tracedShape(double r, double fx, double fy, double u, int channel, KernelShape& out) const
    {
        // The kernel is r times the table's map, which is relative to u; at focus both go to zero
        // while the lens's own blur there doesn't. Keep the pair consistent (the lookup holds u
        // away from zero the same way), or in-focus pixels become points next to 2 px kernels.
        if (std::fabs(u) < 1e-6) {
            u = u < 0. ? -1e-6 : 1e-6;
            r = u * _rInf;
        }
        KernelLookup L;
        const double rho = std::sqrt(fx * fx + fy * fy);
        _l.table->lookup(u, rho, channel, &L);
        const double ch = std::max(0., std::min(1., _l.character));
        const double A[4] = {ch * L.a[0] - (1. - ch), ch * L.a[1], ch * L.a[2], ch * L.a[3] - (1. - ch)};
        const double c = rho > 1e-9 ? fx / rho : 1., s = rho > 1e-9 ? fy / rho : 0.;
        // M = R A R^T, R = [c -s; s c]; then the squeeze and the radius.
        const double t0 = c * A[0] - s * A[2], t1 = c * A[1] - s * A[3];
        const double t2 = s * A[0] + c * A[2], t3 = s * A[1] + c * A[3];
        const double M[4] = {t0 * c - t1 * s, t0 * s + t1 * c, t2 * c - t3 * s, t2 * s + t3 * c};
        double m0 = r * M[0] * _sqX, m1 = r * M[1] * _sqY, m2 = r * M[2] * _sqX, m3 = r * M[3] * _sqY;
        // Near a line focus (astigmatism) the kernel is a long thin line. Treating it as a point
        // because its area is small, while a neighbour a little further from the line focus is
        // drawn at full length, leaves holes and doubled pixels in an opaque surface: keep the thin
        // axis at least a third of a pixel (m = U S V^T, S clamped) and use the long axis for the
        // point test.
        const double E = 0.5 * (m0 + m3), F = 0.5 * (m0 - m3), G = 0.5 * (m2 + m1), Hh = 0.5 * (m2 - m1);
        const double Q = std::sqrt(E * E + Hh * Hh), Rr = std::sqrt(F * F + G * G);
        const double sMax = Q + Rr, sMin = Q - Rr;   // sMin < 0: the map flips (det < 0)
        if (std::fabs(sMin) < 0.33 && sMax >= 0.5) {
            const double a1 = std::atan2(G, F), a2 = std::atan2(Hh, E);
            const double th = 0.5 * (a2 - a1), ph = 0.5 * (a2 + a1);   // m = R(ph) diag(sMax, sMin) R(th)
            const double s2 = sMin < 0. ? -0.33 : 0.33;
            const double cp = std::cos(ph), sp = std::sin(ph), ct = std::cos(th), st = std::sin(th);
            m0 = cp * sMax * ct - sp * s2 * st;
            m1 = -cp * sMax * st - sp * s2 * ct;
            m2 = sp * sMax * ct + cp * s2 * st;
            m3 = -sp * sMax * st + cp * s2 * ct;
        }
        out.m[0] = m0; out.m[1] = m1; out.m[2] = m2; out.m[3] = m3;
        const double det = m0 * m3 - m1 * m2;
        out.point = sMax < 0.5;
        if (!out.point) {
            out.inv[0] = m3 / det; out.inv[1] = -m1 / det;
            out.inv[2] = -m2 / det; out.inv[3] = m0 / det;
        }
        out.yExtent = std::sqrt(m2 * m2 + m3 * m3);
        // Centred on the pixel: the kernel's own centre (the clip discs make it one-sided) goes back
        // to the sample, and the lens's centre offset (coma) is not applied. Both move with depth
        // and field position, and a moving centre leaves seams in surfaces; the image doesn't shift.
        {
            const double cu = ch * L.centroid;
            out.offX = -(m0 * cu * c + m1 * cu * s);
            out.offY = -(m2 * cu * c + m3 * cu * s);
        }
        // Rim clip discs: grow away as Character goes to 0 (a disc scaled by 1/ch keeps its side).
        out.nClip = 0;
        if (ch > 1e-3) {
            for (int i = 0; i < L.nClip && i < 4; ++i) {
                out.clipX[out.nClip] = L.clipX[i] / ch * c;
                out.clipY[out.nClip] = L.clipX[i] / ch * s;
                out.clipR[out.nClip] = L.clipR[i] / ch;
                ++out.nClip;
            }
        }
        // Unit energy: the light a lens's rims cut away depends on the field and on the entry the
        // lookup lands on, so applying it here darkens alpha and leaves bands where neighbouring
        // samples use different entries. Vignetting is a separate (colour-only) stage.
        out.energy = 1.;
        // Profile blended with uniform (a single full-size ring).
        int n = 0;
        bool hasOne = false;
        for (int i = 0; i < L.nRings && n < 16; ++i) {
            out.ringS[n] = L.ringScale[i];
            out.ringW[n] = ch * L.ringWeight[i];
            if (std::fabs(L.ringScale[i] - 1.) < 1e-9) {
                out.ringW[n] += 1. - ch;
                hasOne = true;
            }
            ++n;
        }
        if (!hasOne) {
            out.ringS[n] = 1.;
            out.ringW[n] = 1. - ch;
            ++n;
        }
        out.nRings = n;
        out.ringScale = out.ringS;
        out.ringWeight = out.ringW;
    }

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
    double _rInf = 0.;
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
    for (int i = 0; i < k.nClip; ++i) {
        const double ox = bx - k.clipX[i], oy = by - k.clipY[i];
        const double B = 2. * (ax * ox + ay * oy);
        const double C = ox * ox + oy * oy - k.clipR[i] * k.clipR[i];
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
            float acc[kNumCh] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
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

// A sample too small to draw: all of it into its pixel. A traced lens's centre offset (coma) moves
// it by a fraction of a pixel; rounding that would move whole pixels where the rounding flips
// (holes and doubled pixels in a flat surface), so it is split bilinearly instead.
inline void pointSample(SpanBuffer& buf, const KernelShape& k, double bx, double by, const float* v)
{
    if (k.offX == 0. && k.offY == 0.) {
        const int cx = (int)std::floor(bx + 0.5);
        const int cy = (int)std::floor(by + 0.5);
        if (cy >= 0 && cy < buf.h) {
            buf.point(cy, cx, v, k.energy);
        }
        return;
    }
    const int x0 = (int)std::floor(bx), y0 = (int)std::floor(by);
    const double fx = bx - x0, fy = by - y0;
    for (int j = 0; j < 2; ++j) {
        const int cy = y0 + j;
        if (cy < 0 || cy >= buf.h) {
            continue;
        }
        const double wy = j ? fy : 1. - fy;
        buf.point(cy, x0, v, k.energy * wy * (1. - fx));
        buf.point(cy, x0 + 1, v, k.energy * wy * fx);
    }
}

// Scatter one sample centred at buffer coordinates (bx, by) (buffer-relative, in buffer pixels).
// The kernel is normalised to unit energy (times the shape's vignetting energy).
void scatterSample(SpanBuffer& buf, std::vector<Interval>& scratch,
                   const ProceduralLens& lens, const KernelShape& k,
                   double bx, double by, const float* v)
{
    bx += k.offX;
    by += k.offY;
    if (k.point) {
        pointSample(buf, k, bx, by, v);
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
        pointSample(buf, k, bx, by, v);
        return;
    }
    const double norm = k.energy / area;
    for (size_t i = 0; i < scratch.size(); ++i) {
        const Interval& iv = scratch[i];
#ifdef DEFOCUS_TRACE
        if (!(std::fabs(iv.t0) < 4. * ext + 8.) || !(std::fabs(iv.t1) < 4. * ext + 8.) || !std::isfinite(norm)) {
            static int nBad = 0;
            if (nBad++ < 5) {
                std::printf("bad span: bx %.2f by %.2f ext %.3f t %g..%g norm %g area %g inv %g %g %g %g v %g\n", bx, by, ext,
                            iv.t0, iv.t1, norm, area, k.inv[0], k.inv[1], k.inv[2], k.inv[3], v[0]);
            }
        }
#endif
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
    float c[kSampleCh];
};

struct Slice
{
    int index = 0;
    int level = 0;
    float maxAbsR = 0.f;
    float reach = 0.f;          // largest kernel half-size (render px): a traced kernel can be many
                                // times the slice's thin-lens radius near focus
    std::vector<Sample> cells;  // level-grid samples
    std::vector<Sample> fills;  // level-grid background filled in behind nearer slices
    std::vector<Sample> hi;     // full-res highlight samples
};

inline int sliceIndexOf(double r, double step)
{
    const double s = std::log1p(std::fabs(r) / kSliceR0) / std::log1p(step);
    const int i = (int)std::floor(s + 0.5);
    return r < 0. ? -i : i;
}

// Exact Euclidean distance transform of a w x h grid (Felzenszwalb & Huttenlocher), with the index
// of the nearest seed cell; columns, then rows, each in parallel. dist = +inf and nearest = -1
// where there is no seed at all.
void distanceTransform(int w, int h, const std::vector<unsigned char>& seed, std::vector<float>& dist,
                       std::vector<int>& nearest, std::vector<int>& g, std::vector<int>& gRow,
                       int nThreads, const std::function<bool()>& aborted)
{
    const size_t n = (size_t)w * h;
    const int kNone = std::numeric_limits<int>::max() / 4;
    dist.assign(n, std::numeric_limits<float>::infinity());
    nearest.assign(n, -1);
    // 1. Per column: distance (in rows) to the nearest seed in that column, and its row.
    g.assign(n, kNone);
    gRow.assign(n, -1);
    parallelFor(w, 16, nThreads, aborted, [&](int b, int e) {
        for (int x = b; x < e; ++x) {
            int last = -1;
            for (int y = 0; y < h; ++y) {
                const size_t c = (size_t)y * w + x;
                if (seed[c]) {
                    last = y;
                }
                if (last >= 0) {
                    g[c] = y - last;
                    gRow[c] = last;
                }
            }
            last = -1;
            for (int y = h - 1; y >= 0; --y) {
                const size_t c = (size_t)y * w + x;
                if (seed[c]) {
                    last = y;
                }
                if (last >= 0 && last - y < g[c]) {
                    g[c] = last - y;
                    gRow[c] = last;
                }
            }
        }
    });
    // 2. Per row: lower envelope of the parabolas (x - q)^2 + g(q)^2.
    parallelFor(h, 8, nThreads, aborted, [&](int b, int e) {
        std::vector<int> v(w);
        std::vector<double> z(w + 1);
        for (int y = b; y < e; ++y) {
            const size_t row = (size_t)y * w;
            auto f = [&](int q) { const double gq = g[row + q]; return gq * gq; };
            int k = -1;
            for (int q = 0; q < w; ++q) {
                if (g[row + q] >= kNone) {
                    continue;
                }
                const double fq = f(q);
                double s = 0.;
                while (k >= 0) {
                    s = ((fq + (double)q * q) - (f(v[k]) + (double)v[k] * v[k])) / (2. * (q - v[k]));
                    if (s <= z[k]) {
                        --k;
                    } else {
                        break;
                    }
                }
                ++k;
                v[k] = q;
                z[k] = k == 0 ? -std::numeric_limits<double>::infinity() : s;
                z[k + 1] = std::numeric_limits<double>::infinity();
            }
            if (k < 0) {
                continue;   // no seed reaches this row
            }
            int j = 0;
            for (int x = 0; x < w; ++x) {
                while (z[j + 1] < x) {
                    ++j;
                }
                const int q = v[j];
                const double d2 = (double)(x - q) * (x - q) + f(q);
                dist[row + x] = (float)std::sqrt(d2);
                nearest[row + x] = gRow[row + q] * w + q;
            }
        }
    });
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
focusDistanceAt(const Settings& s, double x, double y)
{
    return FocusField(s).at(x - 0.5, y - 0.5);
}

double
signedCocRadiusAt(const Settings& s, double depthUnits, double x, double y)
{
    // Full-res radius: evaluate at render scale, then divide it back out.
    const CocModel m(s);
    const double S = FocusField(s).at(x - 0.5, y - 0.5);
    const double r = (!(depthUnits > 0.) || !std::isfinite(depthUnits)) ? m.rInfAt(S) : m.radiusAt(1. / depthUnits, S);
    return r / std::max(1e-9, s.renderScale);
}

namespace {

bool
invert4(const double m[16], double out[16])
{
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
            a[r][c + 4] = r == c ? 1. : 0.;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) {
                piv = r;
            }
        }
        if (std::fabs(a[piv][c]) < 1e-12) {
            return false;
        }
        if (piv != c) {
            for (int k = 0; k < 8; ++k) {
                std::swap(a[c][k], a[piv][k]);
            }
        }
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) {
            a[c][k] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != c && a[r][c] != 0.) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r][c + 4];
        }
    }
    return true;
}

} // anonymous namespace

bool
cameraFromExrMatrices(const double w2c[16], const double w2ndc[16], double width, double height,
                      double renderScale, CameraModel* out)
{
    // Row-vector convention: camera = [world 1] * w2c, ndc = [world 1] * w2ndc.
    double c2w[16];
    if (width <= 0. || height <= 0. || !invert4(w2c, c2w)) {
        return false;
    }
    // Projection from camera space: ndc = [camera 1] * proj, proj = c2w * w2ndc.
    double proj[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double v = 0.;
            for (int k = 0; k < 4; ++k) {
                v += c2w[r * 4 + k] * w2ndc[k * 4 + c];
            }
            proj[r * 4 + c] = v;
        }
    }
    // Perspective divide w = proj[2][3] * z: the camera looks along +z (Karma, RenderMan) or -z.
    const double p23 = proj[2 * 4 + 3];
    if (std::fabs(p23) < 1e-9) {
        return false;   // orthographic, or not a camera
    }
    const double fwd = p23 > 0. ? 1. : -1.;
    // ndc_x = ax * x / depth + bx, depth = p23 * z (likewise y).
    const double ax = proj[0 * 4 + 0], ay = proj[1 * 4 + 1];
    const double bx = proj[2 * 4 + 0] / p23, by = proj[2 * 4 + 1] / p23;
    if (std::fabs(ax) < 1e-12 || std::fabs(ay) < 1e-12) {
        return false;
    }
    const double sx = ax > 0. ? 1. : -1., sy = ay > 0. ? 1. : -1.;
    CameraModel m;
    m.valid = true;
    for (int k = 0; k < 3; ++k) {
        m.origin[k] = c2w[3 * 4 + k];
        // Our camera axes in world space: X = sx * row 0, Y = sy * row 1, Z = -fwd * row 2.
        m.rot[k * 3 + 0] = sx * c2w[0 * 4 + k];
        m.rot[k * 3 + 1] = sy * c2w[1 * 4 + k];
        m.rot[k * 3 + 2] = -fwd * c2w[2 * 4 + k];
    }
    // NDC -1..1 spans the display window on both axes.
    m.focalPx = 0.25 * (std::fabs(ax) * width + std::fabs(ay) * height) * renderScale;
    m.centreX = (bx * 0.5 + 0.5) * width * renderScale;
    m.centreY = (by * 0.5 + 0.5) * height * renderScale;
    *out = m;
    return true;
}

bool
focusTiltFromNormal(const double n[3], double* tiltDeg, double* swingDeg)
{
    // Plane n.X = c with X = (X, Y, -depth): depth = (n0 X + n1 Y - c) / n2, so
    // tan(tilt) = n1 / n2 and tan(swing) = n0 / n2.
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 0.) || std::fabs(n[2]) < 1e-3 * len) {
        return false;
    }
    *tiltDeg = std::atan(n[1] / n[2]) * 180. / kPi;
    *swingDeg = std::atan(n[0] / n[2]) * 180. / kPi;
    return true;
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
    double sw = std::min(4., 1. + std::max(0., std::fabs(s.lens.swirl)));
    if (s.lens.table) {
        sw = std::max(sw, s.lens.table->maxScale());   // a traced kernel can outgrow the thin-lens disc
    }
    return s.maxRadiusPx * s.renderScale * sq * sw
           + std::fabs(s.lens.axialCA) * s.renderScale + 2.;
}

namespace {

bool renderDefocus(const Inputs& in, const Settings& s, const ImageView& output, const ImageView* errorMap,
                   Stats* stats, const std::function<bool()>& aborted);

// A radial map at normalised field radius rho (linear, held past the ends).
inline float mapAt(const std::vector<float>& m, double rhoMax, double rho)
{
    if (m.empty()) {
        return 1.f;
    }
    const double t = std::max(0., rho) / std::max(1e-9, rhoMax) * (m.size() - 1);
    if (t >= m.size() - 1) {
        return m.back();
    }
    const int i = (int)t;
    const float f = (float)(t - i);
    return m[i] + (m[i + 1] - m[i]) * f;
}

// Catmull-Rom sample of channel c of src at (x, y) (pixel centres at integer + 0.5); 0 outside.
inline float sampleCubic(const ImageView& src, int c, double x, double y)
{
    const double fx = x - 0.5, fy = y - 0.5;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const double tx = fx - x0, ty = fy - y0;
    auto w = [](double t, double* k) {
        const double t2 = t * t, t3 = t2 * t;
        k[0] = 0.5 * (-t3 + 2. * t2 - t);
        k[1] = 0.5 * (3. * t3 - 5. * t2 + 2.);
        k[2] = 0.5 * (-3. * t3 + 4. * t2 + t);
        k[3] = 0.5 * (t3 - t2);
    };
    double kx[4], ky[4];
    w(tx, kx);
    w(ty, ky);
    double acc = 0.;
    for (int j = 0; j < 4; ++j) {
        const int yy = y0 - 1 + j;
        if (yy < src.bounds.y1 || yy >= src.bounds.y2) {
            continue;
        }
        double row = 0.;
        for (int i = 0; i < 4; ++i) {
            const int xx = x0 - 1 + i;
            if (xx < src.bounds.x1 || xx >= src.bounds.x2) {
                continue;
            }
            row += kx[i] * src.pixel(xx, yy)[c];
        }
        acc += ky[j] * row;
    }
    return (float)acc;
}

// The output pixel (x, y) -> where it comes from, for map m.
inline void mapPoint(const Settings& s, const std::vector<float>& m, double x, double y, double* sx, double* sy)
{
    const double dx = x - s.opticalCentreX, dy = y - s.opticalCentreY;
    const double rho = std::sqrt(dx * dx + dy * dy) / std::max(1e-6, s.fieldRadiusPx);
    const double k = mapAt(m, s.stages.rhoMax, rho);
    *sx = s.opticalCentreX + dx * k;
    *sy = s.opticalCentreY + dy * k;
}

// Resample src into dst through the stages' radial maps (per channel; channels past 2 follow green).
void warpThrough(const Settings& s, const ImageView& src, const ImageView& dst, int nThreads,
                 const std::function<bool()>& aborted)
{
    const LensStages& st = s.stages;
    parallelFor(dst.bounds.height(), 16, nThreads, aborted, [&](int b, int e) {
        for (int y = dst.bounds.y1 + b; y < dst.bounds.y1 + e; ++y) {
            for (int x = dst.bounds.x1; x < dst.bounds.x2; ++x) {
                float* o = dst.pixel(x, y);
                double gx = 0., gy = 0.;
                mapPoint(s, st.scale[1], x + 0.5, y + 0.5, &gx, &gy);
                for (int c = 0; c < dst.channels; ++c) {
                    double px = gx, py = gy;
                    if (c == 0 || c == 2) {
                        mapPoint(s, st.scale[c], x + 0.5, y + 0.5, &px, &py);
                    }
                    o[c] = c < src.channels ? sampleCubic(src, c, px, py) : 0.f;
                }
            }
        }
    });
}

} // namespace

bool
render(const Inputs& in,
       const Settings& s,
       const ImageView& output,
       const ImageView* errorMap,
       Stats* stats,
       const std::function<bool()>& aborted)
{
    if (!s.stages.active() || s.output != eOutputResult || !output.valid()) {
        return renderDefocus(in, s, output, errorMap, stats, aborted);
    }
    const int nThreads = threadCount(s.threads);
    if (!s.stages.geometric()) {
        if (!renderDefocus(in, s, output, errorMap, stats, aborted)) {
            return false;
        }
    } else {
        // The result over every point the warp reads (the output's outline mapped back, plus the
        // centre), then resampled into the output.
        double x1 = output.bounds.x1, y1 = output.bounds.y1, x2 = output.bounds.x2, y2 = output.bounds.y2;
        auto take = [&](double x, double y) {
            for (int c = 0; c < 3; ++c) {
                double sx, sy;
                mapPoint(s, s.stages.scale[c], x, y, &sx, &sy);
                x1 = std::min(x1, sx); x2 = std::max(x2, sx);
                y1 = std::min(y1, sy); y2 = std::max(y2, sy);
            }
        };
        const Rect& ob = output.bounds;
        for (int i = 0; i <= 64; ++i) {
            const double fx = ob.x1 + (ob.x2 - ob.x1) * i / 64., fy = ob.y1 + (ob.y2 - ob.y1) * i / 64.;
            take(fx, ob.y1); take(fx, ob.y2); take(ob.x1, fy); take(ob.x2, fy);
        }
        take(s.opticalCentreX, s.opticalCentreY);
        Rect tb((int)std::floor(x1) - 3, (int)std::floor(y1) - 3, (int)std::ceil(x2) + 3, (int)std::ceil(y2) + 3);
        // no more than the output plus a frame's worth around it (a lens that can't image the
        // corners shouldn't ask for an unbounded window)
        const int mw = ob.width(), mh = ob.height();
        tb = Rect(std::max(tb.x1, ob.x1 - mw), std::max(tb.y1, ob.y1 - mh), std::min(tb.x2, ob.x2 + mw), std::min(tb.y2, ob.y2 + mh));
        std::vector<float> tmp((size_t)tb.width() * tb.height() * 4, 0.f), tmpE;
        ImageView tv;
        tv.data = tmp.data();
        tv.bounds = tb;
        tv.channels = 4;
        tv.rowStride = (std::ptrdiff_t)tb.width() * 4;
        ImageView ev;
        if (errorMap && errorMap->valid()) {
            tmpE.assign((size_t)tb.width() * tb.height() * errorMap->channels, 0.f);
            ev.data = tmpE.data();
            ev.bounds = tb;
            ev.channels = errorMap->channels;
            ev.rowStride = (std::ptrdiff_t)tb.width() * errorMap->channels;
        }
        if (!renderDefocus(in, s, tv, ev.valid() ? &ev : nullptr, stats, aborted)) {
            return false;
        }
        warpThrough(s, tv, output, nThreads, aborted);
        if (ev.valid()) {
            Settings sg = s;   // the error map follows green
            sg.stages.scale[0] = sg.stages.scale[2] = sg.stages.scale[1];
            warpThrough(sg, ev, *errorMap, nThreads, aborted);
        }
    }
    if (!s.stages.illum.empty()) {
        parallelFor(output.bounds.height(), 16, nThreads, aborted, [&](int b, int e) {
            for (int y = output.bounds.y1 + b; y < output.bounds.y1 + e; ++y) {
                for (int x = output.bounds.x1; x < output.bounds.x2; ++x) {
                    const double dx = x + 0.5 - s.opticalCentreX, dy = y + 0.5 - s.opticalCentreY;
                    const float v = mapAt(s.stages.illum, s.stages.rhoMax, std::sqrt(dx * dx + dy * dy) / std::max(1e-6, s.fieldRadiusPx));
                    float* o = output.pixel(x, y);
                    for (int c = 0; c < 3 && c < output.channels; ++c) {
                        o[c] *= v;
                    }
                }
            }
        });
    }
    return !(aborted && aborted());
}

namespace {

bool
renderDefocus(const Inputs& in,
              const Settings& s,
              const ImageView& output,
              const ImageView* errorMap,
              Stats* statsOut,
              const std::function<bool()>& aborted)
{
    Stats stats;
    const int nThreads = threadCount(s.threads);
    // One pool for every parallel loop of this call (see WorkerPool).
    WorkerPool pool(nThreads);
    struct PoolScope
    {
        WorkerPool* prev;
        explicit PoolScope(WorkerPool* p) : prev(tlsPool) { tlsPool = p; }
        ~PoolScope() { tlsPool = prev; }
    } poolScope(&pool);
    const Clock::time_point tDecode = Clock::now();

    const CocModel coc(s);
    const ProceduralLens lens(s.lens, coc.rInf);
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

    // ---- 0. surface geometry (optional Position / Normal planes)
    const FocusField focusField(s);
    const Projection proj(s);
    const ConstImageView& posV = in.position;
    const ConstImageView& nrmV = in.normal;
    const bool havePos = posV.valid() && posV.channels >= in.positionChannel + 3;
    const bool haveNrm = nrmV.valid() && nrmV.channels >= in.normalChannel + 3;
    // A camera-space P may look down +z or -z depending on the renderer: take the sign most
    // pixels have, so that in front of the camera is -z.
    double posZSign = 1.;
    if (havePos && in.positionSpace == eSpaceCamera) {
        long nPos = 0, nNeg = 0;
        const Rect& b = posV.bounds;
        const int step = std::max(1, std::max(b.width(), b.height()) / 128);
        for (int y = b.y1; y < b.y2; y += step) {
            for (int x = b.x1; x < b.x2; x += step) {
                const float z = posV.pixel(x, y)[in.positionChannel + 2];
                nPos += z > 0.f;
                nNeg += z < 0.f;
            }
        }
        posZSign = nPos > nNeg ? -1. : 1.;
    }
    // Geometry is compared in camera space when there is one (camera-space P, or world P plus a
    // camera), else in world space (edges only: the fill plane needs pixel rays).
    const bool geomCamera = havePos && (in.positionSpace == eSpaceCamera || s.camera.valid);
    // Position at a source pixel in the geometry space. Exactly 0 = nothing there (background).
    auto geomPos = [&](int x, int y, double c[3]) -> bool {
        if (!havePos || !posV.bounds.contains(x, y)) {
            return false;
        }
        const float* q = posV.pixel(x, y) + in.positionChannel;
        if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2]) ||
            (q[0] == 0.f && q[1] == 0.f && q[2] == 0.f)) {
            return false;
        }
        if (in.positionSpace == eSpaceCamera) {
            c[0] = q[0]; c[1] = q[1]; c[2] = q[2] * posZSign;
        } else if (s.camera.valid) {
            const double w[3] = {q[0], q[1], q[2]};
            s.camera.toCamera(w, c);
        } else {
            c[0] = q[0]; c[1] = q[1]; c[2] = q[2];
        }
        return true;
    };
    auto geomNormal = [&](int x, int y, double n[3]) -> bool {
        if (!haveNrm || !nrmV.bounds.contains(x, y)) {
            return false;
        }
        const float* q = nrmV.pixel(x, y) + in.normalChannel;
        const double w[3] = {q[0], q[1], q[2]};
        if (geomCamera) {
            if (in.normalSpace == eSpaceCamera) {
                n[0] = w[0]; n[1] = w[1]; n[2] = w[2] * (in.positionSpace == eSpaceCamera ? posZSign : 1.);
            } else if (s.camera.valid) {
                s.camera.dirToCamera(w, n);
            } else {
                return false;   // world normals with camera-space P and no camera
            }
        } else {
            if (in.normalSpace != eSpaceWorld) {
                return false;
            }
            n[0] = w[0]; n[1] = w[1]; n[2] = w[2];
        }
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(len > 0.5) || !std::isfinite(len)) {
            return false;   // no normal (empty pixel, volume)
        }
        n[0] /= len; n[1] /= len; n[2] /= len;
        return true;
    };
    const bool useGeom = havePos && haveNrm;

    // ---- 1. depth -> signed radius per source pixel
    const Rect& SB = src.bounds;
    const int Sw = SB.width(), Sh = SB.height();
    std::vector<float> rSrc((size_t)Sw * Sh, 0.f);
    std::vector<float> invSrc((size_t)Sw * Sh, 0.f);   // 1 / planar depth, 0 = infinity or unknown
    std::vector<unsigned char> clampedSrc((size_t)Sw * Sh, 0);
    // Position/Normal can disagree with the depth pass: volumes and transparent objects (Karma
    // writes the surface behind a VDB into P and N, but the volume into depth). Such pixels don't
    // use the geometry.
    std::vector<unsigned char> geomTrust(useGeom ? (size_t)Sw * Sh : 0, 1);
    const ConstImageView& dep = in.depth;

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
                } else if (s.depthConvention == eDepthPosition) {
                    // Planar depth from the camera-space position; no position = infinity.
                    double c[3];
                    double inv = 0.;
                    if (geomCamera && geomPos(dx, dy, c) && -c[2] > 0.) {
                        inv = 1. / -c[2];
                    }
                    invSrc[(size_t)j * Sw + i] = (float)inv;
                    r = focusField.tilted ? coc.radiusAt(inv, focusField.at(x, y)) : coc.radius(inv);
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
                                    const double u = proj.u(x), w = proj.v(y);
                                    z /= std::sqrt(1. + u * u + w * w);
                                }
                                inv = 1. / z;
                            }
                            break;
                        }
                        }
                    }
                    invSrc[(size_t)j * Sw + i] = (float)inv;
                    r = focusField.tilted ? coc.radiusAt(inv, focusField.at(x, y)) : coc.radius(inv);
                    if (useGeom && geomCamera) {
                        double c[3];
                        if (inv > 0. && geomPos(dx, dy, c) && std::fabs(-c[2] * inv - 1.) > 0.02) {
                            geomTrust[(size_t)j * Sw + i] = 0;
                        }
                    }
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

    // ---- 1b. denoise the radius map within surfaces. Depth passes carry render noise, and at
    // sub-pixel radii a +-0.05 px jitter between neighbours swings how much light a pixel keeps by
    // several percent, which shows as speckled cracks. Average over the 5x5 neighbours on the same
    // surface (radius within tolerance, and only pixels with content: empty pixels carry no
    // surface) so depth edges stay sharp; a linear ramp is unchanged.
    std::vector<unsigned char> occupied(N, 0);
    parallelFor(Dh, 16, nThreads, aborted, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            for (int i = 0; i < Dw; ++i) {
                float c[4];
                srcAt(D.x1 + i, D.y1 + j, c);
                occupied[(size_t)j * Dw + i] = (c[0] != 0.f || c[1] != 0.f || c[2] != 0.f || c[3] != 0.f) ? 1 : 0;
            }
        }
    });
    std::vector<float> rSm(N, 0.f);
    parallelFor(Dh, 16, nThreads, aborted, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            for (int i = 0; i < Dw; ++i) {
                const float r0 = rPix[(size_t)j * Dw + i];
                const float tol = 0.25f * (float)rs + 0.03f * std::fabs(r0);
                double sum = 0.;
                int n = 0;
                for (int jj = std::max(0, j - 2); jj <= std::min(Dh - 1, j + 2); ++jj) {
                    for (int ii = std::max(0, i - 2); ii <= std::min(Dw - 1, i + 2); ++ii) {
                        const size_t q = (size_t)jj * Dw + ii;
                        const float rq = rPix[q];
                        if (std::fabs(rq - r0) <= tol && (occupied[q] || (jj == j && ii == i))) {
                            sum += rq;
                            ++n;
                        }
                    }
                }
                rSm[(size_t)j * Dw + i] = (float)(sum / n);
            }
        }
    });

    // How two neighbouring domain pixels relate, from Position/Normal:
    //  0 = the same surface: each lies on the other's tangent plane (a floor at a grazing angle, a
    //      curved surface). Both planes must agree: past an object, a grazing floor's plane can
    //      contain the step along the view, but the object's plane does not.
    //  1 = an occlusion edge: far apart along the view compared with a pixel's width (camera space).
    // -1 = unknown (no geometry, a crease, ambiguous): the radius-based tests decide.
    auto geomRelation = [&](int i, int j, int ii, int jj) -> int {
        if (!useGeom) {
            return -1;
        }
        const int xa = std::max(SB.x1, std::min(SB.x2 - 1, D.x1 + i)), ya = std::max(SB.y1, std::min(SB.y2 - 1, D.y1 + j));
        const int xb = std::max(SB.x1, std::min(SB.x2 - 1, D.x1 + ii)), yb = std::max(SB.y1, std::min(SB.y2 - 1, D.y1 + jj));
        if (!geomTrust[(size_t)(ya - SB.y1) * Sw + (xa - SB.x1)] || !geomTrust[(size_t)(yb - SB.y1) * Sw + (xb - SB.x1)]) {
            return -1;
        }
        double pa[3], na[3], pb[3], nb[3];
        if (!geomPos(xa, ya, pa) || !geomNormal(xa, ya, na) || !geomPos(xb, yb, pb) || !geomNormal(xb, yb, nb)) {
            return -1;
        }
        const double d[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dl > 1e-12)) {
            return 0;
        }
        const double e1 = std::fabs(na[0] * d[0] + na[1] * d[1] + na[2] * d[2]);
        const double e2 = std::fabs(nb[0] * d[0] + nb[1] * d[1] + nb[2] * d[2]);
        if (std::max(e1, e2) <= 0.25 * dl) {
            return 0;
        }
        if (geomCamera) {
            const double za = -pa[2], zb = -pb[2];
            const double lateral = std::max(za, zb) / proj.fpx;   // scene units across one pixel
            if (za > 0. && zb > 0. && std::fabs(za - zb) > 4. * lateral) {
                return 1;
            }
        }
        return -1;
    };

    // The occlusion-edge question from depth alone (no Position/Normal needed): an outline is a
    // depth step much larger than a pixel's width at that distance that doesn't continue the
    // surface's own depth gradient (a floor at a grazing angle steps by as much on either side).
    // Unlike a threshold on the blur difference this doesn't depend on focus, so an outline gets the
    // same treatment on every frame of a focus pull; with a tilted focus plane the blur difference
    // across an outline hovers around any threshold, the decision flips from pixel to pixel along
    // it, and the flips crawl as focus moves (beaded, wavy outlines).
    // Returns 1 for an occlusion edge, else -1 (let the other tests decide).
    auto depthRelation = [&](int i, int j, int ii, int jj) -> int {
        if (s.depthConvention == eDepthDirectCoC) {
            return -1;
        }
        auto zAt = [&](int a, int b) -> double {   // planar depth of a domain pixel; 0 = none / infinity
            if (a < 0 || b < 0 || a >= Dw || b >= Dh) {
                return 0.;
            }
            const int x = std::max(SB.x1, std::min(SB.x2 - 1, D.x1 + a));
            const int y = std::max(SB.y1, std::min(SB.y2 - 1, D.y1 + b));
            const float iv = invSrc[(size_t)(y - SB.y1) * Sw + (x - SB.x1)];
            return iv > 0.f ? 1. / iv : 0.;
        };
        const double za = zAt(i, j), zb = zAt(ii, jj);
        if (!(za > 0.) || !(zb > 0.)) {
            return -1;   // against infinity (sky): the radius test sees the jump
        }
        const double lateral = std::max(za, zb) / proj.fpx;   // scene units across one pixel
        const double dz = zb - za;
        if (std::fabs(dz) <= 4. * lateral) {
            return -1;
        }
        // Surface gradient measured 2-3 pixels out on each side, past any anti-aliased blend
        // pixels at the outline (those make an edge look like a short ramp to its neighbours).
        // Done on 1/depth, which is exactly linear across the image on any plane (depth itself
        // curves, a lot at grazing angles).
        const int di = ii - i, dj = jj - j;
        const double a1 = zAt(i - 2 * di, j - 2 * dj), a0 = zAt(i - 3 * di, j - 3 * dj);
        const double b1 = zAt(ii + 2 * di, jj + 2 * dj), b0 = zAt(ii + 3 * di, jj + 3 * dj);
        if (a1 > 0. && a0 > 0. && b1 > 0. && b0 > 0.) {
            // A real outline has a smooth surface on either side (steep is fine: a sphere near its
            // silhouette). Noisy depth (a volume, fur, render noise) jumps about as much everywhere:
            // not an outline, or every noisy pixel would force slices and cut the rest of the frame
            // into thin bands. Smoothness = how much 1/depth's slope changes from pixel to pixel.
            const double am = zAt(i - 4 * di, j - 4 * dj), bm = zAt(ii + 4 * di, jj + 4 * dj);
            if (am > 0. && bm > 0.) {
                const double curvL = std::fabs((1. / a1 - 1. / a0) - (1. / a0 - 1. / am));
                const double curvR = std::fabs((1. / b0 - 1. / b1) - (1. / bm - 1. / b0));
                if (std::max(curvL, curvR) > 0.25 * std::fabs(1. / zb - 1. / za)) {
                    return -1;
                }
            }
            const double gap = 1. / b1 - 1. / a1;   // over 5 pixel steps
            const double err = std::min(std::fabs(gap - 5. * (1. / a1 - 1. / a0)), std::fabs(gap - 5. * (1. / b0 - 1. / b1)));
            return err > 4. * lateral / (za * zb) ? 1 : -1;   // the depth tolerance, in 1/depth
        }
        return 1;
    };

    // Whether two neighbouring (occupied) domain pixels must go into different slices:
    //  0 = no; 1 = yes, an occlusion edge (Position/Normal); 2 = yes, by the blur-radius test;
    //  3 = no, because Position/Normal say one surface where the radius test would split (only
    //      told apart from 0 with `explain`, which costs the radius test).
    auto pairSplit = [&](int i, int j, int ii, int jj, bool explain) -> int {
        const size_t p = (size_t)j * Dw + i, q = (size_t)jj * Dw + ii;
        const float lo = std::min(rSm[p], rSm[q]), hi = std::max(rSm[p], rSm[q]);
        // With Position/Normal the geometry decides: one surface never splits, and a real
        // occlusion edge splits even when the radii differ by little (those leave the faint seams
        // the radius test lets through).
        // A clear depth step is an outline whatever P/N say (filtered P/N blur contacts and
        // outlines into "one surface"); otherwise P/N may still find one surface or an outline.
        int rel = depthRelation(i, j, ii, jj);
        if (rel == -1) {
            rel = geomRelation(i, j, ii, jj);
        }
        if (rel == 1 && !s.splitOcclusionEdges) {
            rel = -1;   // Fast: occlusion edges go by the radius test
        }
        if (rel == 1) {
            return hi - lo > 0.02f * (float)rs ? 1 : 0;
        }
        if (rel == 0 && !explain) {
            return 0;
        }
        const float sliceThr = 0.1f * (float)rs + 0.015f * std::max(std::fabs(lo), std::fabs(hi));
        bool radiusSplit = hi - lo > sliceThr;
        // A steep but smooth surface (a floor running towards the lens) steps by about as much on
        // either side: that is a ramp, not an edge, and splitting it puts a slice boundary on every
        // row (visible as horizontal lines). Only a step that differs from the surface's own
        // gradient on both sides is a depth edge.
        if (radiusSplit) {
            const float d = rSm[q] - rSm[p];
            const int di = ii - i, dj = jj - j;
            const int pi = i - di, pj = j - dj, ni = ii + di, nj = jj + dj;
            float excess = std::fabs(d);
            if (pi >= 0 && pj >= 0 && pi < Dw && pj < Dh && occupied[(size_t)pj * Dw + pi]) {
                excess = std::min(excess, std::fabs(d - (rSm[p] - rSm[(size_t)pj * Dw + pi])));
            }
            if (ni >= 0 && nj >= 0 && ni < Dw && nj < Dh && occupied[(size_t)nj * Dw + ni]) {
                excess = std::min(excess, std::fabs(d - (rSm[(size_t)nj * Dw + ni] - rSm[q])));
            }
            radiusSplit = excess > sliceThr;
        }
        if (rel == 0) {
            return radiusSplit ? 3 : 0;
        }
        return radiusSplit ? 2 : 0;
    };

    // ---- 2. samples, with depth-edge reconstruction
    // Edge pixels: the CoC varies by more than the threshold across their 3x3 (and, with surface
    // geometry, not every neighbour is on the same surface).
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
                    bool edge = (hi - lo) > edgeThr;
                    if (edge && useGeom) {
                        bool allSame = true;
                        for (int jj = std::max(0, j - 1); jj <= std::min(Dh - 1, j + 1) && allSame; ++jj) {
                            for (int ii = std::max(0, i - 1); ii <= std::min(Dw - 1, i + 1) && allSame; ++ii) {
                                if ((ii != i || jj != j) && geomRelation(i, j, ii, jj) != 0) {
                                    allSame = false;
                                }
                            }
                        }
                        edge = !allSame;
                    }
                    isEdge[(size_t)j * Dw + i] = edge ? 1 : 0;
                }
            }
        });
    }
    // Compact samples: most pixels are one whole sample (colour = the source pixel, radius in rS).
    // Pixels with more (both parts of a depth-edge split, or a deep pixel's samples) list all of
    // them in `extra`, contiguous; sample ids: p is a pixel's first sample, N + k its others.
    // `layer`: deep samples, which composite front to back within a slice; the two parts of a split
    // sit side by side in the pixel and add up.
    struct ExtraRec
    {
        size_t p;
        PixSample s;
        bool layer;
    };
    std::vector<float> rS(N, 0.f);
    std::vector<unsigned short> nSamp(N, 0);
    std::vector<int> firstExtra(N, -1);
    std::vector<std::vector<ExtraRec> > extraChunks((Dh + 15) / 16);
    std::vector<float> edgeA(N, -1.f);

    // Deep: the blur radius of a planar depth at a pixel, as step 1 decodes the depth pass.
    const bool deepOn = in.deep.valid();
    auto radiusOfZ = [&](int x, int y, double z) -> float {
        double inv = 0.;
        const bool inf = !(z > 0.) || !std::isfinite(z) || (s.farThreshold > 0. && z >= s.farThreshold);
        if (!inf) {
            if (s.depthConvention == eDepthRadial) {
                const double u = proj.u(x), w = proj.v(y);
                z /= std::sqrt(1. + u * u + w * w);
            }
            inv = 1. / z;
        }
        double r = focusField.tilted ? coc.radiusAt(inv, focusField.at(x, y)) : coc.radius(inv);
        r *= r < 0. ? s.nearGain : s.farGain;
        return (float)std::max(-coc.maxR, std::min(coc.maxR, r));
    };

    parallelFor(Dh, 16, nThreads, aborted, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            const int y = D.y1 + j;
            for (int i = 0; i < Dw; ++i) {
                const int x = D.x1 + i;
                const size_t p = (size_t)j * Dw + i;
                if (deepOn) {
                    // Every deep sample at its own radius, front to back; a volume segment
                    // (ZBack > Z) in pieces along its radius range (OpenEXR's split: each piece
                    // alpha 1 - (1 - a)^(1/m), colour in proportion), so fog blurs with depth.
                    int cnt = 0;
                    const float* d = in.deep.at(x, y, &cnt);
                    if (!d || cnt <= 0) {
                        continue;
                    }
                    std::vector<std::pair<float, int> > zs;
                    zs.reserve(cnt);
                    for (int k = 0; k < cnt; ++k) {
                        const float* q = d + (size_t)k * in.deep.stride;
                        if (q[in.deep.iA] <= 0.f && q[in.deep.iR] == 0.f && q[in.deep.iG] == 0.f && q[in.deep.iB] == 0.f) {
                            continue;
                        }
                        zs.push_back(std::make_pair(q[in.deep.iZ], k));
                    }
                    std::stable_sort(zs.begin(), zs.end());
                    std::vector<ExtraRec>& out = extraChunks[b / 16];
                    const size_t first = out.size();
                    for (size_t k = 0; k < zs.size() && out.size() - first < 60000; ++k) {
                        const float* q = d + (size_t)zs[k].second * in.deep.stride;
                        const float c[4] = {q[in.deep.iR], q[in.deep.iG], q[in.deep.iB], q[in.deep.iA]};
                        const double z0 = q[in.deep.iZ];
                        const double z1 = in.deep.iZBack >= 0 ? (double)q[in.deep.iZBack] : z0;
                        const float r0 = radiusOfZ(x, y, z0);
                        int m = 1;
                        float r1 = r0;
                        if (z1 > z0 * 1.0001) {
                            r1 = radiusOfZ(x, y, z1);
                            m = std::max(1, std::min(16, (int)std::ceil(std::fabs(r1 - r0) / std::max(0.5, rs))));
                        }
                        const double a = std::max(0.f, std::min(1.f, c[3]));
                        const double am = m == 1 ? a : 1. - std::pow(1. - a, 1. / m);
                        const double scale = m == 1 ? 1. : (a > 1e-9 ? am / a : 1. / m);
                        for (int piece = 0; piece < m; ++piece) {
                            ExtraRec er;
                            er.p = p;
                            er.layer = true;
                            er.s.r = m == 1 ? r0 : r0 + (r1 - r0) * (piece + 0.5f) / m;
                            er.s.w = 1.f;
                            for (int ch = 0; ch < 4; ++ch) {
                                er.s.c[ch] = (float)(c[ch] * scale);
                            }
                            er.s.hi = false;
                            out.push_back(er);
                        }
                    }
                    if (out.size() > first) {
                        rS[p] = out[first].s.r;
                        nSamp[p] = (unsigned short)(out.size() - first);
                    }
                    continue;
                }
                float C[4];
                srcAt(x, y, C);
                if (C[0] == 0.f && C[1] == 0.f && C[2] == 0.f && C[3] == 0.f) {
                    continue;
                }
                const float rOwn = rSm[p];
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
                            const float rq = rSm[(size_t)jj * Dw + ii];
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
                            const float rq = rSm[q];
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
                for (int k = 0; k < 2; ++k) {
                    ExtraRec er;
                    er.p = p;
                    er.s = out[k];
                    er.layer = false;
                    extraChunks[b / 16].push_back(er);
                }
            }
        }
    });
    if (aborted && aborted()) {
        return false;
    }
    std::vector<ExtraRec> extra;
    {
        size_t total = 0;
        for (size_t c = 0; c < extraChunks.size(); ++c) {
            total += extraChunks[c].size();
        }
        extra.reserve(total);
        for (size_t c = 0; c < extraChunks.size(); ++c) {
            for (size_t k = 0; k < extraChunks[c].size(); ++k) {
                const ExtraRec& er = extraChunks[c][k];
                if (firstExtra[er.p] < 0) {
                    firstExtra[er.p] = (int)extra.size();
                }
                extra.push_back(er);
            }
        }
    }
    std::vector<std::vector<ExtraRec> >().swap(extraChunks);
    const size_t nIds = N + extra.size();
    std::vector<unsigned char> hiFlag(nIds, 0);
    // A pixel's first sample is its own id; the k-th of a listed pixel is N + its record.
    auto sampleOf = [&](size_t p, int k) -> size_t { return k == 0 ? p : N + (size_t)firstExtra[p] + (size_t)k; };
    auto pixelOf = [&](size_t id) -> size_t { return id < N ? id : extra[id - N].p; };
    auto recOf = [&](size_t id) -> const ExtraRec* {
        if (id >= N) {
            return &extra[id - N];
        }
        return firstExtra[id] >= 0 ? &extra[firstExtra[id]] : nullptr;
    };
    auto sampleR = [&](size_t id) -> float {
        const ExtraRec* er = recOf(id);
        return er ? er->s.r : rS[id];
    };
    auto sampleW = [&](size_t id) -> float {
        const ExtraRec* er = recOf(id);
        return er ? er->s.w : 1.f;
    };
    auto sampleLayer = [&](size_t id) -> bool {
        const ExtraRec* er = recOf(id);
        return er && er->layer;
    };
    auto sampleColour = [&](size_t id, float* c) {
        const ExtraRec* er = recOf(id);
        if (er) {
            std::memcpy(c, er->s.c, sizeof(float) * 4);
        } else {
            srcAt(D.x1 + (int)(id % Dw), D.y1 + (int)(id / Dw), c);
        }
    };

    // ---- debug outputs that need no scatter
    if (s.output != eOutputResult) {
        // Surface Edges: per pixel, the strongest slicing decision with its 4 neighbours (see
        // pairSplit).
        std::vector<unsigned char> surfaceMark(s.output == eOutputSurfaceEdges ? N : 0, 0);
        if (!surfaceMark.empty()) {
            parallelFor(Dh, 16, nThreads, aborted, [&](int b, int e) {
                for (int j = b; j < e; ++j) {
                    for (int i = 0; i < Dw; ++i) {
                        if (!occupied[(size_t)j * Dw + i]) {
                            continue;
                        }
                        int kind = 0;
                        const int nb[4][2] = {{i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1}};
                        for (int k = 0; k < 4 && kind != 1; ++k) {
                            const int ii = nb[k][0], jj = nb[k][1];
                            if (ii < 0 || jj < 0 || ii >= Dw || jj >= Dh || !occupied[(size_t)jj * Dw + ii]) {
                                continue;
                            }
                            const int d = pairSplit(i, j, ii, jj, true);
                            if (d == 1 || (d == 2 && kind != 1) || (d == 3 && kind == 0)) {
                                kind = d;
                            }
                        }
                        surfaceMark[(size_t)j * Dw + i] = (unsigned char)kind;
                    }
                }
            });
        }
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
                case eOutputSurfaceEdges: {
                    // Strongest mark in the 3x3 (thickened so single-pixel outlines are visible).
                    const int i = x - D.x1, j = y - D.y1;
                    int kind = 0;
                    for (int jj = std::max(0, j - 1); jj <= std::min(Dh - 1, j + 1); ++jj) {
                        for (int ii = std::max(0, i - 1); ii <= std::min(Dw - 1, i + 1); ++ii) {
                            const int d = surfaceMark[(size_t)jj * Dw + ii];
                            if (d == 1 || (d == 2 && kind != 1) || (d == 3 && kind == 0)) {
                                kind = d;
                            }
                        }
                    }
                    const int sx = std::max(SB.x1, std::min(SB.x2 - 1, x)), sy = std::max(SB.y1, std::min(SB.y2 - 1, y));
                    const bool distrusted = useGeom && !geomTrust[(size_t)(sy - SB.y1) * Sw + (sx - SB.x1)];
                    float c[4];
                    srcAt(x, y, c);
                    const float dim = 0.25f * std::min(1.f, luminance(c));
                    o[0] = o[1] = o[2] = dim;
                    if (kind == 1) {
                        o[0] = 1.f; o[1] = 0.1f; o[2] = 0.1f;
                    } else if (kind == 2) {
                        o[0] = 1.f; o[1] = 0.85f; o[2] = 0.1f;
                    } else if (kind == 3) {
                        o[0] = 0.1f; o[1] = 0.9f; o[2] = 0.2f;
                    } else if (distrusted) {
                        o[0] = dim; o[1] = dim; o[2] = 0.35f + dim;
                    }
                    break;
                }
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

    // ---- 4. bucket samples into slices.
    // Slices are spaced by Slice Step, but a depth edge (blur radius jumping by more than ~0.25 px
    // between neighbours) must never fall inside one slice: within a slice samples add
    // up without occlusion, and two surfaces with different radii leave a coverage seam where they
    // meet (an object over the floor behind it). So samples are first binned at a fraction of the
    // step, and extra slice boundaries are placed (greedy interval stabbing) so that the front and
    // back of every depth edge land in different slices. Smooth surfaces and near-equal depths
    // still share slices and add up. (Fine bins: 1/16 of the step.)
    // Forced boundaries fall on a grid of 1/8 of the step: finer cuts every outline more exactly but
    // slices smooth surfaces elsewhere into thin bands, each band boundary a faint ripple.
    const double fineStep = s.sliceStep / 8.;
    std::vector<short> sIdx(nIds, 0);
    parallelFor(Dh, 64, nThreads, aborted, [&](int b, int e) {
        for (size_t p = (size_t)b * Dw; p < (size_t)e * Dw; ++p) {
            for (int k = 0; k < nSamp[p]; ++k) {
                const size_t id = sampleOf(p, k);
                sIdx[id] = (short)sliceIndexOf(sampleR(id), fineStep);
            }
        }
    });
    {
        int fMin = 0, fMax = 0;
        bool anyF = false;
        for (size_t p = 0; p < N; ++p) {
            for (int k = 0; k < nSamp[p]; ++k) {
                const int fi = sIdx[sampleOf(p, k)];
                fMin = anyF ? std::min(fMin, fi) : fi;
                fMax = anyF ? std::max(fMax, fi) : fi;
                anyF = true;
            }
        }
        if (anyF) {
            const int nb = fMax - fMin + 1;
            const int kInf = std::numeric_limits<int>::max();
            // Depth-edge intervals [front bin, back bin]: per row chunk, merged afterwards.
            const int grain = 32;
            std::vector<std::vector<int> > chunkMinEnd((Dh + grain - 1) / grain);
            parallelFor(Dh, grain, nThreads, aborted, [&](int b, int e) {
                std::vector<int>& minEnd = chunkMinEnd[b / grain];
                minEnd.assign(nb, kInf);
                for (int j = b; j < e; ++j) {
                    for (int i = 0; i < Dw; ++i) {
                        const size_t p = (size_t)j * Dw + i;
                        if (!occupied[p]) {
                            continue;
                        }
                        // Jumps between directly adjacent pixels (right and up, so each pair once).
                        // Two surfaces whose radii differ by d leave a seam of roughly d / r where
                        // they meet in one slice, so even sub-pixel jumps count; a smooth ramp's
                        // per-pixel step and (denoised) depth noise stay below the threshold.
                        const int nbr[2][2] = {{i + 1, j}, {i, j + 1}};
                        for (int k = 0; k < 2; ++k) {
                            const int ii = nbr[k][0], jj = nbr[k][1];
                            if (ii >= Dw || jj >= Dh) {
                                continue;
                            }
                            const size_t q = (size_t)jj * Dw + ii;
                            if (!occupied[q]) {
                                continue;
                            }
                            if (pairSplit(i, j, ii, jj, false) == 0) {
                                continue;
                            }
                            const float lo = std::min(rSm[p], rSm[q]), hi = std::max(rSm[p], rSm[q]);
                            const int fa = std::max(0, std::min(nb - 1, sliceIndexOf(lo, fineStep) - fMin));
                            const int fb = std::max(0, std::min(nb - 1, sliceIndexOf(hi, fineStep) - fMin));
                            if (fb > fa) {
                                minEnd[fa] = std::min(minEnd[fa], fb);
                            }
                        }
                    }
                }
            });
            std::vector<int> minEnd(nb, kInf);
            for (size_t c = 0; c < chunkMinEnd.size(); ++c) {
                for (int k = 0; k < (int)chunkMinEnd[c].size(); ++k) {
                    minEnd[k] = std::min(minEnd[k], chunkMinEnd[c][k]);
                }
            }
            auto fineCentre = [&](int fi) {
                const double v = kSliceR0 * (std::pow(1. + fineStep, std::fabs((double)fi)) - 1.);
                return fi < 0 ? -v : v;
            };
            std::vector<short> sliceOf(nb, 0);
            int slice = 0, need = kInf;
            for (int k = 0; k < nb; ++k) {
                bool boundary = false;
                if (k > 0 && sliceIndexOf(fineCentre(fMin + k), s.sliceStep) != sliceIndexOf(fineCentre(fMin + k - 1), s.sliceStep)) {
                    boundary = true;   // regular Slice Step spacing
                }
                if (k == need) {
                    boundary = true;   // a depth edge spans the current slice
                }
                if (boundary) {
                    ++slice;
                    need = kInf;
                }
                need = std::min(need, minEnd[k]);
                sliceOf[k] = (short)slice;
            }
            for (size_t id = 0; id < nIds; ++id) {
                const size_t p = pixelOf(id);
                // (a listed pixel's first record is reached through the pixel id, not its own)
                if (nSamp[p] == 0 || (id >= N && (size_t)firstExtra[p] == id - N)) {
                    continue;
                }
                sIdx[id] = sliceOf[std::max(0, std::min(nb - 1, sIdx[id] - fMin))];
            }
        }
    }
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
    // Per-slice scratch, reused across slices.
    std::vector<float> W, P, Rs, Hc, RoccC, RfrontC, cleanC, dist, distP;
    std::vector<unsigned char> cleanOk, seedMask, covered;
    std::vector<int> srcIdx, srcP, edtG, edtRow;
    slices.reserve(nSlices);
    std::vector<float> H(N, 0.f), Rocc(N, 0.f), Rfront(N, std::numeric_limits<float>::quiet_NaN());
    float maxRocc = 0.f;
    float maxReachOcc = 0.f;   // the same as kernel reach (a traced lens: can be far larger)
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
        double rLo = 1e30, rHi = -1e30;
        for (int q = b; q < e; ++q) {
            const double r = sampleR(order[q]);
            sl.maxAbsR = std::max(sl.maxAbsR, (float)std::fabs(r));
            rLo = std::min(rLo, r);
            rHi = std::max(rHi, r);
        }
        sl.reach = sl.maxAbsR;
        if (s.lens.table && coc.rInf > 0.) {
            // the traced kernel's size over the slice's radii and the whole field (a Petzval's
            // field curvature makes kernels near focus tens of times the thin-lens disc)
            for (int iu = 0; iu <= 4; ++iu) {
                const double r = rLo + (rHi - rLo) * iu / 4.;
                if (std::fabs(r) < 1e-6) {
                    continue;
                }
                for (int ir = 0; ir <= 12; ++ir) {
                    sl.reach = std::max(sl.reach, (float)(std::fabs(r) * lens.extentScale(r / coc.rInf, 0.1 * ir)));
                }
            }
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
        const int band = (int)std::ceil((std::max(maxRocc, maxReachOcc) + sl.reach) / sc) + 2;
        Rect region(std::max(0, px1 - band), std::max(0, py1 - band),
                    std::min(gw, px2 + 1 + band), std::min(gh, py2 + 1 + band));
        const int rw = region.width(), rh = region.height();
        const size_t RN = (size_t)rw * rh;

        // Aggregate this slice's samples into region cells (all levels: L0 cells are pixels).
        W.assign(RN, 0.f);
        P.assign(RN * 4, 0.f);
        Rs.assign(RN, 0.f);
        const double inv = 1. / ((double)sc * sc);
        // A deep pixel's layers in this slice (adjacent in `order`, front first) composite over
        // each other before they join the cell; the radius is weighted by what each layer shows.
        size_t layP = std::numeric_limits<size_t>::max(), layC = 0;
        double layCol[4] = {0., 0., 0., 0.}, layRW = 0., layVis = 0., layR0 = 0.;
        auto flushLayers = [&]() {
            if (layP == std::numeric_limits<size_t>::max()) {
                return;
            }
            W[layC] += (float)inv;
            for (int k = 0; k < 4; ++k) {
                P[layC * 4 + k] += (float)(layCol[k] * inv);
            }
            Rs[layC] += (float)(layVis > 0. ? layRW / layVis : layR0);
            layP = std::numeric_limits<size_t>::max();
        };
        for (int q = b; q < e; ++q) {
            PixSample ps;
            ps.r = sampleR(order[q]);
            ps.w = sampleW(order[q]);
            sampleColour(order[q], ps.c);
            ps.hi = hiFlag[order[q]] != 0;
            const size_t p = pixelOf(order[q]);
            const int pi = (int)(p % Dw), pj = (int)(p / Dw);
            if (!ps.hi && sampleLayer(order[q])) {
                const size_t c = (size_t)(pj / sc - region.y1) * rw + (pi / sc - region.x1);
                if (p != layP) {
                    flushLayers();
                    layP = p;
                    layC = c;
                    layCol[0] = layCol[1] = layCol[2] = layCol[3] = 0.;
                    layRW = layVis = 0.;
                    layR0 = ps.r;
                }
                const double t = std::max(0., 1. - layCol[3]);
                const double vis = t * std::max(1e-6f, ps.c[3]);
                for (int k = 0; k < 4; ++k) {
                    layCol[k] += t * ps.c[k];
                }
                layRW += vis * ps.r;
                layVis += vis;
                continue;
            }
            flushLayers();
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
        flushLayers();

        // Background fill behind nearer slices.
        std::vector<Sample> fills;
        const bool regionHits = haveHole &&
                                !(holeBox.x2 / sc + 1 < region.x1 || holeBox.x1 / sc > region.x2 ||
                                  holeBox.y2 / sc + 1 < region.y1 || holeBox.y1 / sc > region.y2);
        if (s.fillBehind && regionHits) {
            // Occluders and clean plate per cell.
            Hc.assign(RN, 0.f);
            RoccC.assign(RN, 0.f);
            RfrontC.assign(RN, std::numeric_limits<float>::quiet_NaN());
            cleanC.assign(clean.valid() ? RN * 4 : 0, 0.f);
            cleanOk.assign(clean.valid() ? RN : 0, 0);
            parallelFor(rh, 8, nThreads, aborted, [&](int jb, int je) {
            for (int j = jb; j < je; ++j) {
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
            });

            // Distance + nearest seed cell. Seeds are cells of this slice at least one cell away
            // from any nearer content: with an aliased depth pass the pixels right at the edge carry
            // the foreground's anti-aliased colour, and extending them streaks it inwards.
            seedMask.assign(RN, 0);
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
                            seedMask[c] = 1;
                            ++nSeeds;
                        }
                    }
                }
            }
            distanceTransform(rw, rh, seedMask, dist, srcIdx, edtG, edtRow, nThreads, aborted);
            // Distance to the nearest cell this slice actually covers decides how far the fill must
            // reach; the seeds above sit back from the edge and would cut the fill short.
            // The nearest covered cell also gives the fill its blur radius: on a depth ramp that is
            // the adjacent pixel, so the radius stays continuous; behind an object it is the
            // background's own radius (not the object's), so their junction doesn't leave a seam.
            covered.assign(RN, 0);
            for (size_t c = 0; c < RN; ++c) {
                covered[c] = W[c] > 0.f ? 1 : 0;
            }
            distanceTransform(rw, rh, covered, distP, srcP, edtG, edtRow, nThreads, aborted);
            const int fillGrain = 8;
            std::vector<std::vector<Sample> > fillChunks((rh + fillGrain - 1) / fillGrain);
            parallelFor(rh, fillGrain, nThreads, aborted, [&](int jb, int je) {
            std::vector<Sample>& chunk = fillChunks[jb / fillGrain];
            for (int j = jb; j < je; ++j) {
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
                    int sidx = srcIdx[c];
                    const int pidx = srcP[c] >= 0 ? srcP[c] : sidx;
                    // The eroded seeds normally sit a cell or two behind the covered edge. When the
                    // nearest one is much farther than that (a thin strip of this slice, e.g. a few
                    // floor rows, erodes away entirely), it belongs to some other surface, and its
                    // colour would streak along the strip (a cone's colour drawn as a line across
                    // the floor). Use the nearest covered cell instead.
                    if (dist[c] > distP[c] + 3.f) {
                        sidx = pidx;
                    }
                    // Continue the surface's radius gradient from that cell (exact on a depth ramp).
                    auto rAt = [&](int q) { return Rs[q] / std::max(1e-12f, W[q] / (float)inv); };
                    const int pi = pidx % rw, pj = pidx / rw;
                    double gx = 0., gy = 0.;
                    {
                        const int l = pi > 0 && W[pidx - 1] > 0.f ? pidx - 1 : pidx;
                        const int r = pi + 1 < rw && W[pidx + 1] > 0.f ? pidx + 1 : pidx;
                        const int d = pj > 0 && W[pidx - rw] > 0.f ? pidx - rw : pidx;
                        const int u = pj + 1 < rh && W[pidx + rw] > 0.f ? pidx + rw : pidx;
                        if (r != l) {
                            gx = (rAt(r) - rAt(l)) / ((r - l));
                        }
                        if (u != d) {
                            gy = (rAt(u) - rAt(d)) / ((u - d) / rw);
                        }
                    }
                    const double rNear = rAt(pidx);
                    double rSrc = rNear + gx * (i - pi) + gy * (j - pj);
                    // Never extrapolate past zero or by more than the local blur itself.
                    if ((rSrc > 0.) != (rNear > 0.) || std::fabs(rSrc - rNear) > std::fabs(rNear)) {
                        rSrc = rNear;
                    }
                    double reachOcc = RoccC[c], reachSrc = std::fabs(rSrc);
                    if (s.lens.table && coc.rInf != 0.) {
                        // A traced kernel can be larger than the thin-lens disc, and offset.
                        const double fxF = (D.x1 + (i + region.x1 + 0.5) * sc - s.opticalCentreX);
                        const double fyF = (D.y1 + (j + region.y1 + 0.5) * sc - s.opticalCentreY);
                        const double rho = std::sqrt(fxF * fxF + fyF * fyF) / std::max(1e-6, s.fieldRadiusPx);
                        const double uO = RoccC[c] / coc.rInf;
                        reachOcc *= std::max(lens.extentScale(uO, rho), lens.extentScale(-uO, rho));
                        reachSrc *= lens.extentScale(rSrc / coc.rInf, rho);
                    }
                    // Axial colour: red and blue blur up to |axialCA| further than green.
                    const double caReach = 2. * std::fabs(s.lens.axialCA) * s.renderScale;
                    if (distP[c] > (reachOcc + reachSrc + caReach) / sc + 2.) {
                        continue;
                    }
                    // Same surface continuing under a nearer slice (its blur matches the occluder's),
                    // as opposed to background hidden behind a different object.
                    const double rF = RfrontC[c];
                    const bool disc = std::fabs(rF - rSrc) > std::max(1.5 * rs, 0.2 * std::max(std::fabs(rF), std::fabs(rSrc)));
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
                        f.c[4] = disc ? (float)h : 0.f;
                    }
                    chunk.push_back(f);
                }
            }
            });
            for (size_t k = 0; k < fillChunks.size(); ++k) {
                fills.insert(fills.end(), fillChunks[k].begin(), fillChunks[k].end());
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
        std::sort(sl.cells.begin(), sl.cells.end(), lessYX);
        sl.fills.swap(fills);
        std::sort(sl.fills.begin(), sl.fills.end(), lessYX);
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
            H[p] = std::min(1.f, H[p] + ps.w);   // (deep layers: several per pixel)
            Rocc[p] = std::max(Rocc[p], std::fabs(ps.r));
            maxRocc = std::max(maxRocc, std::fabs(ps.r));
            maxReachOcc = std::max(maxReachOcc, sl.reach);
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
        // Where the sample sits in a traced lens's table: its radius over the radius at infinity.
        const double u = coc.rInf != 0. ? smp.r / coc.rInf : 0.;
        float v[kNumCh];
        if (caPx == 0. && !lens.perChannel()) {
            for (int c = 0; c < kSampleCh; ++c) {
                v[c] = smp.c[c];
            }
            v[kAlphaR] = v[kAlphaB] = smp.c[3];
            lens.shape(smp.r / gridScale, fx, fy, k, u, 1);
            scatterSample(buf, scratch, lens, k, gx, gy, v);
            return;
        }
        const double rr[3] = {smp.r + caPx, smp.r, smp.r - caPx};
        for (int ch = 0; ch < 3; ++ch) {
            for (int c = 0; c < kNumCh; ++c) {
                v[c] = 0.f;
            }
            v[ch] = smp.c[ch];
            if (ch == 0) {
                v[kAlphaR] = smp.c[3];
            } else if (ch == 1) {
                v[3] = smp.c[3];
                v[4] = smp.c[4];
            } else {
                v[kAlphaB] = smp.c[3];
            }
            lens.shape(rr[ch] / gridScale, fx, fy, k, u, ch);
            scatterSample(buf, scratch, lens, k, gx, gy, v);
        }
    };

    auto lowerY = [](const std::vector<Sample>& v, int y) {
        return std::lower_bound(v.begin(), v.end(), y, [](const Sample& a, int yy) { return a.y < yy; });
    };

    // Write: mix / mask against the source, error map. a: the accumulator's kNumCh channels.
    auto writePixel = [&](int x, int y, const float* a) {
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
    };

    // GPU: the same shapes, drawn and composited by DefocusGpu (plan/03_gpu_defocus.md). The
    // CPU scatter below runs instead when it can't (no GPU, too large, an error).
    if (s.useGpu) {
        gpu::Job job;
        job.ow = Ow;
        job.oh = Oh;
        job.oxD = O.x1 - D.x1;
        job.oyD = O.y1 - D.y1;
        job.inside.resize((size_t)Ow * Oh);
        for (int y = O.y1; y < O.y2; ++y) {
            for (int x = O.x1; x < O.x2; ++x) {
                float c[4];
                srcAt(x, y, c);
                job.inside[(size_t)(y - O.y1) * Ow + (x - O.x1)] = std::max(0.f, std::min(1.f, c[3]));
            }
        }
        for (const Constraint& c : lens.constraints()) {
            const float e[8] = {(float)c.nx, (float)c.ny, (float)c.h, c.disc ? 1.f : 0.f,
                                (float)c.cx, (float)c.cy, (float)c.R, 0.f};
            job.constraints.insert(job.constraints.end(), e, e + 8);
        }
        // Rings shared by many shapes (the procedural lens's far / near profiles) go up once; a
        // traced lens's per-sample rings go into the chunk's own list (flagged, offset at the merge).
        const std::uint32_t kLocalRing = 0x80000000u;
        std::vector<std::pair<const double*, std::uint32_t> > sharedRings;
        // The procedural lens's shapes are built on the GPU from 32-byte samples; a traced lens's
        // (table lookups) here.
        const bool genOnGpu = !s.lens.table;
        job.lens.onGpu = genOnGpu;
        job.lens.nCh = (caPx == 0. && !lens.perChannel()) ? 1 : 3;
        {
            const double q = std::max(0.05, s.lens.squeeze);
            job.lens.sqX = (float)(1. / std::sqrt(q));
            job.lens.sqY = (float)std::sqrt(q);
            job.lens.swirl = (float)s.lens.swirl;
            job.lens.catEye = (float)s.lens.catEye;
            job.lens.catEyeStart = (float)s.lens.catEyeStart;
            job.lens.cx0 = (float)cx0;
            job.lens.cy0 = (float)cy0;
            job.lens.invField = (float)invField;
            job.lens.caPx = (float)caPx;
        }
        if (!s.lens.table) {
            for (double r : {1., -1.}) {
                KernelShape k;
                lens.shape(r * 4., 0., 0., k, 0., 1);
                if (k.ringScale != k.ringS) {
                    const std::uint32_t off = (std::uint32_t)(job.rings.size() / 2);
                    sharedRings.push_back(std::make_pair(k.ringScale, off));
                    for (int i = 0; i < k.nRings; ++i) {
                        job.rings.push_back((float)k.ringScale[i]);
                        job.rings.push_back((float)k.ringWeight[i]);
                    }
                    if (r > 0.) {
                        job.lens.farOff = off;
                        job.lens.farN = (std::uint32_t)k.nRings;
                    } else {
                        job.lens.nearOff = off;
                        job.lens.nearN = (std::uint32_t)k.nRings;
                    }
                }
            }
        }
        // How far a procedural kernel can reach, over its radius in cells: the squeeze, and the swirl
        // stretch at the frame corner furthest from the optical centre.
        double stretch = std::max(job.lens.sqX, job.lens.sqY);
        if (s.lens.swirl != 0.) {
            double rhoMax = 0.;
            for (int cx : {O.x1, O.x2}) {
                for (int cy : {O.y1, O.y2}) {
                    rhoMax = std::max(rhoMax, std::hypot(cx - cx0, cy - cy0) * invField);
                }
            }
            stretch *= std::max(1., std::min(4., 1. + std::fabs(s.lens.swirl) * rhoMax * rhoMax));
        }

        struct Keyed { int ky, kx; gpu::Shape sh; };
        struct Building { std::vector<Keyed> v; std::vector<float> rings; double reachX = 1., reachY = 1.; float maxR = 0.f; };
        // One sample -> its shape(s) (three with axial colour): scatterWithCA's channel split.
        auto addSample = [&](Building& T, const Sample& smp, double gx, double gy, double fullX, double fullY,
                             double gridScale, int kx, int ky) {
            const double fx = (fullX - cx0) * invField, fy = (fullY - cy0) * invField;
            const double u = coc.rInf != 0. ? smp.r / coc.rInf : 0.;
            auto emit = [&](const KernelShape& k, const float* v) {
                Keyed e;
                e.ky = ky;
                e.kx = kx;
                gpu::Shape& sh = e.sh;
                std::memset(&sh, 0, sizeof(sh));
                sh.bx = (float)(gx + k.offX);
                sh.by = (float)(gy + k.offY);
                sh.ext = (float)k.yExtent;
                for (int i = 0; i < 4; ++i) {
                    sh.inv[i] = k.point ? 0.f : (float)k.inv[i];
                }
                for (int c = 0; c < kNumCh; ++c) {
                    sh.v[c] = v[c];
                }
                sh.kx = kx;
                for (int i = 0; i < k.nClip && i < 4; ++i) {
                    sh.clip[3 * i] = (float)k.clipX[i];
                    sh.clip[3 * i + 1] = (float)k.clipY[i];
                    sh.clip[3 * i + 2] = (float)k.clipR[i];
                }
                sh.energy = (float)k.energy;
                sh.flags = (k.point ? gpu::eShapePoint : 0u) | ((std::uint32_t)std::min(4, k.nClip) << 1) |
                           ((k.offX != 0. || k.offY != 0.) ? gpu::eShapeOffset : 0u);
                if (!k.point) {
                    bool found = false;
                    for (const auto& sr : sharedRings) {
                        if (sr.first == k.ringScale) {
                            sh.ringOff = sr.second;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        sh.ringOff = kLocalRing | (std::uint32_t)(T.rings.size() / 2);
                        for (int i = 0; i < k.nRings; ++i) {
                            T.rings.push_back((float)k.ringScale[i]);
                            T.rings.push_back((float)k.ringWeight[i]);
                        }
                    }
                    sh.nRings = (std::uint32_t)k.nRings;
                }
                const double xExt = k.point ? 1. : std::sqrt(k.m[0] * k.m[0] + k.m[1] * k.m[1]);
                const double yExt = k.point ? 1. : k.yExtent;
                T.reachX = std::max(T.reachX, std::fabs(sh.bx - kx) + xExt);
                T.reachY = std::max(T.reachY, std::fabs(sh.by - ky) + yExt);
                T.v.push_back(e);
            };
            KernelShape k;
            float v[kNumCh];
            if (caPx == 0. && !lens.perChannel()) {
                for (int c = 0; c < kSampleCh; ++c) {
                    v[c] = smp.c[c];
                }
                v[kAlphaR] = v[kAlphaB] = smp.c[3];
                lens.shape(smp.r / gridScale, fx, fy, k, u, 1);
                emit(k, v);
                return;
            }
            const double rr[3] = {smp.r + caPx, smp.r, smp.r - caPx};
            for (int ch = 0; ch < 3; ++ch) {
                for (int c = 0; c < kNumCh; ++c) {
                    v[c] = 0.f;
                }
                v[ch] = smp.c[ch];
                if (ch == 0) {
                    v[kAlphaR] = smp.c[3];
                } else if (ch == 1) {
                    v[3] = smp.c[3];
                    v[4] = smp.c[4];
                } else {
                    v[kAlphaB] = smp.c[3];
                }
                lens.shape(rr[ch] / gridScale, fx, fy, k, u, ch);
                emit(k, v);
            }
        };

        // Per slice, five targets each fed by one sample set (already in (row, column) order):
        // highlights, level-0 cells (same buffer), level-0 fills, level cells, level fills. The sets
        // are cut into chunks built in parallel.
        struct Chunk { int slice, target, set, b, e; };   // set: 0 hi, 1 cells, 2 fills
        const int kChunk = 16384;
        std::vector<Chunk> chunks;
        job.slices.resize(slices.size());
        std::vector<std::vector<float> > levelInside(8);
        for (size_t si = 0; si < slices.size(); ++si) {
            const Slice& S = slices[si];
            gpu::Slice& G = job.slices[si];
            const int L = S.level;
            G.level = L;
            const int sc = 1 << L;
            auto lu = [&](double val, int origin) { return (val - origin + 0.5) / sc - 0.5; };
            if (L > 0) {
                G.ilo = (int)std::floor(lu(O.x1, D.x1));
                G.jlo = (int)std::floor(lu(O.y1, D.y1));
            }
            const int lw = L > 0 ? (int)std::floor(lu(O.x2 - 1, D.x1)) + 1 - G.ilo + 1 : 0;
            const int lh = L > 0 ? (int)std::floor(lu(O.y2 - 1, D.y1)) + 1 - G.jlo + 1 : 0;
            const std::vector<Sample>* sets[3] = {&S.hi, &S.cells, &S.fills};
            for (int set = 0; set < 3; ++set) {
                const int target = set == 0 ? gpu::eTargetHighlights
                                 : set == 1 ? (L == 0 ? gpu::eTargetCells : gpu::eTargetLevel)
                                            : (L == 0 ? gpu::eTargetFills : gpu::eTargetLevelFills);
                gpu::Target& T = G.t[target];
                const bool lvlT = target == gpu::eTargetLevel || target == gpu::eTargetLevelFills;
                T.w = lvlT ? lw : Ow;
                T.h = lvlT ? lh : Oh;
                T.fx0 = lvlT ? (float)(D.x1 + (G.ilo + 0.5) * sc - 0.5) : (float)O.x1;
                T.fy0 = lvlT ? (float)(D.y1 + (G.jlo + 0.5) * sc - 0.5) : (float)O.y1;
                T.fstep = lvlT ? (float)sc : 1.f;
                T.gridScale = T.fstep;
                const int n = (int)sets[set]->size();
                if (genOnGpu) {
                    T.samples.resize(n);
                }
                for (int b = 0; b < n; b += kChunk) {
                    Chunk c = {(int)si, target, set, b, std::min(n, b + kChunk)};
                    chunks.push_back(c);
                }
            }
        }
        std::vector<Building> built(chunks.size());
        parallelFor((int)chunks.size(), 1, nThreads, aborted, [&](int b, int e) {
            for (int ci = b; ci < e; ++ci) {
                const Chunk& C = chunks[ci];
                const Slice& S = slices[C.slice];
                gpu::Slice& G = job.slices[C.slice];
                const int sc = 1 << S.level;
                const std::vector<Sample>& v = C.set == 0 ? S.hi : C.set == 1 ? S.cells : S.fills;
                Building& T = built[ci];
                for (int i = C.b; i < C.e; ++i) {
                    const Sample& smp = v[i];
                    // the sample's cell in its target, and its full-res position
                    int kx, ky;
                    double fullX, fullY, gs = 1.;
                    if (C.set == 0) {   // highlights: full-res pixels
                        kx = smp.x - O.x1;
                        ky = smp.y - O.y1;
                        fullX = smp.x;
                        fullY = smp.y;
                    } else if (S.level == 0) {
                        kx = smp.x + D.x1 - O.x1;
                        ky = smp.y + D.y1 - O.y1;
                        fullX = smp.x + D.x1;
                        fullY = smp.y + D.y1;
                    } else {
                        kx = smp.x - G.ilo;
                        ky = smp.y - G.jlo;
                        fullX = D.x1 + (smp.x + 0.5) * sc - 0.5;
                        fullY = D.y1 + (smp.y + 0.5) * sc - 0.5;
                        gs = sc;
                    }
                    if (genOnGpu) {
                        gpu::Sample& g = G.t[C.target].samples[i];
                        g.kx = kx;
                        g.ky = ky;
                        g.r = smp.r;
                        for (int c = 0; c < kSampleCh; ++c) {
                            g.c[c] = smp.c[c];
                        }
                        T.maxR = std::max(T.maxR, std::fabs(smp.r));
                    } else {
                        addSample(T, smp, kx, ky, fullX, fullY, gs, kx, ky);
                    }
                }
            }
        });
        // Finish each target: row starts (shapes: merged chunks, local ring offsets fixed) and reach.
        auto finish = [&](size_t si, int target) {
            gpu::Target& t = job.slices[si].t[target];
            if (genOnGpu) {
                float maxR = 0.f;
                for (size_t ci = 0; ci < chunks.size(); ++ci) {
                    if (chunks[ci].slice == (int)si && chunks[ci].target == target) {
                        maxR = std::max(maxR, built[ci].maxR);
                    }
                }
                if (t.samples.empty()) {
                    return;
                }
                const bool sorted = std::is_sorted(t.samples.begin(), t.samples.end(), [](const gpu::Sample& a, const gpu::Sample& c) {
                    return a.ky != c.ky ? a.ky < c.ky : a.kx < c.kx;
                });
                if (!sorted) {
                    std::stable_sort(t.samples.begin(), t.samples.end(), [](const gpu::Sample& a, const gpu::Sample& c) {
                        return a.ky != c.ky ? a.ky < c.ky : a.kx < c.kx;
                    });
                }
                t.keyY0 = t.samples.front().ky;
                const int nRows = t.samples.back().ky - t.keyY0 + 1;
                t.rowStart.assign((size_t)nRows + 1, 0);
                for (const gpu::Sample& g : t.samples) {
                    ++t.rowStart[(size_t)(g.ky - t.keyY0) + 1];
                }
                for (int r = 0; r < nRows; ++r) {
                    t.rowStart[(size_t)r + 1] += t.rowStart[(size_t)r];
                }
                // the kernel's half size in cells, squeezed / swirled, plus a cell for points
                t.reachX = t.reachY = (int)std::ceil((maxR + std::fabs(caPx)) / t.gridScale * stretch) + 2;
                return;
            }
            std::vector<Keyed> all;
            double reachX = 1., reachY = 1.;
            for (size_t ci = 0; ci < chunks.size(); ++ci) {
                if (chunks[ci].slice != (int)si || chunks[ci].target != target) {
                    continue;
                }
                Building& T = built[ci];
                const std::uint32_t base = (std::uint32_t)(job.rings.size() / 2);
                job.rings.insert(job.rings.end(), T.rings.begin(), T.rings.end());
                for (Keyed& k : T.v) {
                    if (k.sh.ringOff & kLocalRing) {
                        k.sh.ringOff = base + (k.sh.ringOff & ~kLocalRing);
                    }
                }
                all.insert(all.end(), T.v.begin(), T.v.end());
                reachX = std::max(reachX, T.reachX);
                reachY = std::max(reachY, T.reachY);
                std::vector<Keyed>().swap(T.v);
            }
            if (all.empty()) {
                return;
            }
            // One set per target, already in (row, column) order; order indices, not the 136-byte
            // entries, if it ever isn't.
            auto before = [&all](std::uint32_t a, std::uint32_t c) {
                return all[a].ky != all[c].ky ? all[a].ky < all[c].ky : all[a].kx < all[c].kx;
            };
            std::vector<std::uint32_t> order(all.size());
            for (size_t i = 0; i < all.size(); ++i) {
                order[i] = (std::uint32_t)i;
            }
            if (!std::is_sorted(order.begin(), order.end(), before)) {
                std::stable_sort(order.begin(), order.end(), before);
            }
            t.keyY0 = all[order.front()].ky;
            const int nRows = all[order.back()].ky - t.keyY0 + 1;
            t.rowStart.assign((size_t)nRows + 1, 0);
            t.shapes.resize(all.size());
            for (size_t i = 0; i < all.size(); ++i) {
                const Keyed& k = all[order[i]];
                t.shapes[i] = k.sh;
                ++t.rowStart[(size_t)(k.ky - t.keyY0) + 1];
            }
            for (int r = 0; r < nRows; ++r) {
                t.rowStart[(size_t)r + 1] += t.rowStart[(size_t)r];
            }
            t.reachX = (int)std::ceil(reachX) + 1;
            t.reachY = (int)std::ceil(reachY) + 1;
        };
        for (size_t si = 0; si < slices.size(); ++si) {
            for (int k = 0; k < gpu::eTargetCount; ++k) {
                finish(si, k);
            }
            gpu::Slice& G = job.slices[si];
            const gpu::Target& lf = G.t[gpu::eTargetLevelFills];
            const int L = G.level;
            if (L > 0 && !lf.empty() && L < (int)levelInside.size()) {
                // Level-grid silhouette: mean source alpha per cell (the CPU's mL).
                const int sc = 1 << L;
                const int lw = lf.w, lh = lf.h;
                std::vector<float>& mL = levelInside[L];
                if (mL.empty()) {
                    mL.assign((size_t)lw * lh, 0.f);
                    for (int j = 0; j < lh; ++j) {
                        for (int i = 0; i < lw; ++i) {
                            double sum = 0.;
                            int n = 0;
                            for (int yy = D.y1 + (G.jlo + j) * sc; yy < D.y1 + (G.jlo + j + 1) * sc; ++yy) {
                                for (int xx = D.x1 + (G.ilo + i) * sc; xx < D.x1 + (G.ilo + i + 1) * sc; ++xx) {
                                    float c[4];
                                    srcAt(xx, yy, c);
                                    sum += std::max(0.f, std::min(1.f, c[3]));
                                    ++n;
                                }
                            }
                            mL[(size_t)j * lw + i] = (float)(sum / std::max(1, n));
                        }
                    }
                }
                G.lvlInside = mL;
            }
        }
        std::vector<float> accFull;
        std::string why;
        const double buildSeconds = secondsSince(tScatter);
        const bool gpuOk = gpu::scatter(job, accFull, why, aborted);
        if (std::getenv("DEFOCUS_GPU_TIMES")) {
            size_t n = 0;
            for (const gpu::Slice& g : job.slices) {
                for (const gpu::Target& t : g.t) {
                    n += t.shapes.size() + t.samples.size();
                }
            }
            std::fprintf(stderr, "[DefocusGpu] build %.3f s (%zu shapes or samples, %zu slices), gpu %.3f s\n", buildSeconds, n,
                         job.slices.size(), secondsSince(tScatter) - buildSeconds);
        }
        if (gpuOk) {
            for (int y = O.y1; y < O.y2; ++y) {
                for (int x = O.x1; x < O.x2; ++x) {
                    writePixel(x, y, accFull.data() + ((size_t)(y - O.y1) * Ow + (x - O.x1)) * kNumCh);
                }
            }
            stats.gpu = true;
            stats.secondsScatter = secondsSince(tScatter);
            if (statsOut) {
                *statsOut = stats;
            }
            return true;
        }
        if (aborted && aborted()) {
            return false;
        }
        stats.gpuMessage = why;
    }

    // A traced lens's kernels near a line focus can reach much further than Slice::reach (sampled at
    // a few radii) says; a strip only draws the samples within its margin, so a longer kernel was
    // cut at strip boundaries (alpha bands across a Petzval's floor, found by the GPU path, which
    // measures every shape). The margin now comes from the samples' actual shapes (full-res px).
    std::vector<double> shapeReach(slices.size(), 0.);
    if (s.lens.table) {
        parallelFor((int)slices.size(), 1, nThreads, aborted, [&](int b, int e) {
            for (int si = b; si < e; ++si) {
                const Slice& S = slices[si];
                const double sc = (double)(1 << S.level);
                double reach = 0.;
                auto measure = [&](const std::vector<Sample>& v, bool fullRes) {
                    for (const Sample& smp : v) {
                        const double fullX = fullRes ? smp.x : (S.level == 0 ? smp.x + D.x1 : D.x1 + (smp.x + 0.5) * sc - 0.5);
                        const double fullY = fullRes ? smp.y : (S.level == 0 ? smp.y + D.y1 : D.y1 + (smp.y + 0.5) * sc - 0.5);
                        const double g = (fullRes || S.level == 0) ? 1. : sc;
                        const double fx = (fullX - cx0) * invField, fy = (fullY - cy0) * invField;
                        const double u = coc.rInf != 0. ? smp.r / coc.rInf : 0.;
                        for (int ch = 0; ch < 3; ch += lens.perChannel() ? 1 : 3) {
                            KernelShape k;
                            lens.shape(smp.r / g, fx, fy, k, u, lens.perChannel() ? ch : 1);
                            const double ext = k.point ? 1. : std::max(k.yExtent, std::sqrt(k.m[0] * k.m[0] + k.m[1] * k.m[1]));
                            reach = std::max(reach, (ext + std::max(std::fabs(k.offX), std::fabs(k.offY))) * g);
                        }
                    }
                };
                measure(S.hi, true);
                measure(S.cells, false);
                measure(S.fills, false);
                shapeReach[si] = reach;
            }
        });
    }

    const bool okScatter = parallelFor(nStrips, 1, nThreads, aborted, [&](int b, int e) {
        SpanBuffer full, fullF;
        SpanBuffer lvl[5], lvlF[5];
        std::vector<Interval> scratch;
        std::vector<float> acc, sl, inside, insideL[5];
        int insideLAt[5][2];
        for (int st = b; st < e; ++st) {
            const int y0 = O.y1 + st * kStripRows;
            const int y1 = std::min(O.y2, y0 + kStripRows);
            const int sh = y1 - y0;
            acc.assign((size_t)Ow * sh * kNumCh, 0.f);
            sl.assign((size_t)Ow * sh * kNumCh, 0.f);
            full.setup(O.x1, y0, Ow, sh);
            fullF.setup(O.x1, y0, Ow, sh);
            // Where the image has content (source alpha), full-res and per level. Filled-in
            // background stands for surface hidden behind nearer slices, which only exists inside
            // the image's own silhouette: its blur must not spill into empty space, where it would
            // draw a second copy of an outline the nearer slice already blurs (a wavy edge against
            // the sky at every slice boundary that meets it).
            inside.resize((size_t)Ow * sh);
            for (int y = y0; y < y1; ++y) {
                for (int x = O.x1; x < O.x2; ++x) {
                    float c[4];
                    srcAt(x, y, c);
                    inside[(size_t)(y - y0) * Ow + (x - O.x1)] = std::max(0.f, std::min(1.f, c[3]));
                }
            }
            for (int L = 0; L < 5; ++L) {
                insideL[L].clear();
            }

            for (int k = (int)slices.size() - 1; k >= 0; --k) {
                if (stopRequested()) {
                    return;
                }
                const Slice& S = slices[k];
                const int L = S.level;
                const int sc = 1 << L;
                // generous: swirl / squeeze; a traced kernel's own reach
                const double maxR = std::max(std::max(S.maxAbsR * 4., S.reach * 1.25 + 1.), shapeReach[k] + 1.) +
                                    std::fabs(caPx) + 2.;
                auto lu = [&](double val, int origin) { return (val - origin + 0.5) / sc - 0.5; };
                const int ilo = (int)std::floor(lu(O.x1, D.x1)), ihi = (int)std::floor(lu(O.x2 - 1, D.x1)) + 1;
                const int jlo = (int)std::floor(lu(y0, D.y1)), jhi = (int)std::floor(lu(y1 - 1, D.y1)) + 1;

                // Scatters samples (highlights: full-res pixels; else level-grid cells) into the
                // full-res buffer (level 0) or the level buffer. Returns whether anything was written.
                auto scatterSet = [&](const std::vector<Sample>& v, bool gridIsLevel, SpanBuffer& fullBuf,
                                      SpanBuffer& lb) -> bool {
                    bool touched = false;
                    if (v.empty()) {
                        return false;
                    }
                    if (!gridIsLevel || L == 0) {
                        const int yLo = (int)std::floor(y0 - maxR) - 1, yHi = (int)std::ceil(y1 + maxR) + 1;
                        for (auto it = lowerY(v, gridIsLevel ? yLo - D.y1 : yLo); it != v.end(); ++it) {
                            const int sy = gridIsLevel ? it->y + D.y1 : it->y;
                            if (sy > yHi) {
                                break;
                            }
                            const int sx = gridIsLevel ? it->x + D.x1 : it->x;
                            scatterWithCA(fullBuf, scratch, *it, sx - O.x1, sy - y0, sx, sy, 1.);
                            touched = true;
                        }
                        return touched;
                    }
                    const double maxRl = maxR / sc + 1.;
                    const int yLo = (int)std::floor(jlo - maxRl), yHi = (int)std::ceil(jhi + maxRl);
                    for (auto it = lowerY(v, yLo); it != v.end(); ++it) {
                        if (it->y > yHi) {
                            break;
                        }
                        const double fullX = D.x1 + (it->x + 0.5) * sc - 0.5;
                        const double fullY = D.y1 + (it->y + 0.5) * sc - 0.5;
                        scatterWithCA(lb, scratch, *it, it->x - ilo, it->y - jlo, fullX, fullY, sc);
                        touched = true;
                    }
                    return touched;
                };

                if (L > 0) {
                    lvl[L].setup(ilo, jlo, ihi - ilo + 1, jhi - jlo + 1);
                    lvlF[L].setup(ilo, jlo, ihi - ilo + 1, jhi - jlo + 1);
                }
                bool touched = scatterSet(S.hi, false, full, lvl[L]);
                bool touchedL = scatterSet(S.cells, true, full, lvl[L]);
                const bool filled = scatterSet(S.fills, true, fullF, lvlF[L]);
                if (L == 0) {
                    touched = touched || touchedL;
                }

                // Fills join the slice weighted by the silhouette: level 0 at full res (touched rows
                // only), other levels on the level grid before the one upsample.
                if (filled && L == 0 && fullF.touchHi >= fullF.touchLo) {
                    fullF.resolve();
                    full.resolve();
                    for (int ry = fullF.touchLo; ry <= fullF.touchHi; ++ry) {
                        float* r = full.row(y0 + ry);
                        const float* f = fullF.row(y0 + ry);
                        const float* m = inside.data() + (size_t)ry * Ow;
                        for (int x = 0; x < Ow; ++x) {
                            for (int c = 0; c < kNumCh; ++c) {
                                r[(size_t)x * kNumCh + c] += f[(size_t)x * kNumCh + c] * m[x];
                            }
                        }
                    }
                    full.touch(fullF.touchLo);
                    full.touch(fullF.touchHi);
                    fullF.clearTouched();
                    // full is resolved now: copy it out directly below
                    for (int ry = full.touchLo; ry <= full.touchHi; ++ry) {
                        const float* r = full.row(y0 + ry);
                        float* dd = sl.data() + (size_t)ry * Ow * kNumCh;
                        for (size_t q = 0; q < (size_t)Ow * kNumCh; ++q) {
                            dd[q] += r[q];
                        }
                    }
                    full.clearTouched();
                    touched = true;
                } else if (full.touchHi >= full.touchLo) {
                    full.resolve();
                    for (int ry = full.touchLo; ry <= full.touchHi; ++ry) {
                        const float* r = full.row(y0 + ry);
                        float* dd = sl.data() + (size_t)ry * Ow * kNumCh;
                        for (size_t q = 0; q < (size_t)Ow * kNumCh; ++q) {
                            dd[q] += r[q];
                        }
                    }
                    full.clearTouched();
                }
                if (L > 0 && (touchedL || filled)) {
                    SpanBuffer& lb = lvl[L];
                    lb.resolve();
                    if (filled) {
                        SpanBuffer& lf = lvlF[L];
                        lf.resolve();
                        // Level-grid silhouette (mean source alpha per cell), once per strip and level.
                        std::vector<float>& mL = insideL[L];
                        if (mL.empty() || insideLAt[L][0] != ilo || insideLAt[L][1] != jlo) {
                            mL.assign((size_t)lb.w * lb.h, 0.f);
                            insideLAt[L][0] = ilo;
                            insideLAt[L][1] = jlo;
                            for (int j = 0; j < lb.h; ++j) {
                                for (int i = 0; i < lb.w; ++i) {
                                    double sum = 0.;
                                    int n = 0;
                                    for (int yy = D.y1 + (jlo + j) * sc; yy < D.y1 + (jlo + j + 1) * sc; ++yy) {
                                        for (int xx = D.x1 + (ilo + i) * sc; xx < D.x1 + (ilo + i + 1) * sc; ++xx) {
                                            float c[4];
                                            srcAt(xx, yy, c);
                                            sum += std::max(0.f, std::min(1.f, c[3]));
                                            ++n;
                                        }
                                    }
                                    mL[(size_t)j * lb.w + i] = (float)(sum / std::max(1, n));
                                }
                            }
                        }
                        for (int j = 0; j < lb.h; ++j) {
                            float* r = lb.row(lb.y0 + j);
                            const float* f = lf.row(lf.y0 + j);
                            for (int i = 0; i < lb.w; ++i) {
                                const float m = mL[(size_t)j * lb.w + i];
                                for (int c = 0; c < kNumCh; ++c) {
                                    r[(size_t)i * kNumCh + c] += f[(size_t)i * kNumCh + c] * m;
                                }
                            }
                        }
                        lf.touchLo = 0;
                        lf.touchHi = lf.h - 1;
                        lf.clearTouched();
                    }
                    // Bilinear upsample into the slice strip.
                    for (int y = y0; y < y1; ++y) {
                        const double vv = lu(y, D.y1) - jlo;
                        int j0 = (int)std::floor(vv);
                        const float fy = (float)(vv - j0);
                        j0 = std::max(0, std::min(lb.h - 2, j0));
                        const float* r0 = lb.row(lb.y0 + j0);
                        const float* r1 = lb.row(lb.y0 + j0 + 1);
                        float* dd = sl.data() + (size_t)(y - y0) * Ow * kNumCh;
                        for (int x = O.x1; x < O.x2; ++x) {
                            const double u = lu(x, D.x1) - ilo;
                            int i0 = (int)std::floor(u);
                            const float fx = (float)(u - i0);
                            i0 = std::max(0, std::min(lb.w - 2, i0));
                            const float* a0 = r0 + (size_t)i0 * kNumCh;
                            const float* a1 = r1 + (size_t)i0 * kNumCh;
                            float* o = dd + (size_t)(x - O.x1) * kNumCh;
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
                if (!touched) {
                    continue;
                }
                // Composite this slice over everything behind it. Coverage above 1 inside one slice
                // means the background fill overlapped the slice's own pixels (same surface counted
                // twice at a slice boundary), so normalise it back to full coverage.
                for (size_t q = 0; q < (size_t)Ow * sh; ++q) {
                    float* o = acc.data() + q * kNumCh;
                    float* v = sl.data() + q * kNumCh;
#ifdef DEFOCUS_TRACE
                    static const int traceX = std::getenv("DEFOCUS_TRACE_X") ? std::atoi(std::getenv("DEFOCUS_TRACE_X")) : -1000000000;
                    static const int traceY = std::getenv("DEFOCUS_TRACE_Y") ? std::atoi(std::getenv("DEFOCUS_TRACE_Y")) : -1000000000;
                    if (O.x1 + (int)(q % Ow) == traceX && y0 + (int)(q / Ow) == traceY) {
                        std::fprintf(stderr, "trace slice %d level %d maxR %.2f cells %zu hi %zu: a %.4f g %.4f | behind a %.4f\n",
                                     S.index, S.level, S.maxAbsR, S.cells.size(), S.hi.size(), v[3], v[1], o[3]);
                    }
#endif
                    // Per channel group: red with its coverage, blue with its, the rest with alpha.
                    static const int groups[3][3] = {{0, kAlphaR, -1}, {2, kAlphaB, -1}, {1, 3, 4}};
                    static const int cov[3] = {kAlphaR, kAlphaB, 3};
                    for (int g = 0; g < 3; ++g) {
                        float a = v[cov[g]];
                        if (a > 1.f) {
                            const float n = 1.f / a;
                            for (int i = 0; i < 3 && groups[g][i] >= 0; ++i) {
                                v[groups[g][i]] *= n;
                            }
                            a = 1.f;
                        }
                        const float t = 1.f - std::max(0.f, a);
                        for (int i = 0; i < 3 && groups[g][i] >= 0; ++i) {
                            const int c = groups[g][i];
                            o[c] = v[c] + o[c] * t;
                            v[c] = 0.f;
                        }
                    }
                }
            }

            // Write: mix / mask against the source, error map.
            for (int y = y0; y < y1; ++y) {
                for (int x = O.x1; x < O.x2; ++x) {
                    writePixel(x, y, acc.data() + ((size_t)(y - y0) * Ow + (x - O.x1)) * kNumCh);
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

} // namespace

} // namespace defocus
