// GridWrap: the resampling kernels and the separable gather (DECISIONS.md D-57).
//
// The `(B, C)` pairs are transcribed from references/gridwrap/nuke_gridwrap_knobs.md *Filters*,
// where they are the values fitted by measurement in Nuke 17 and not a choice of ours; the
// formulas behind them are the published BC-spline and the sinc windowing. Tests:
// tests/gw_resample_test.cpp.
#include "resample.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace gw {

namespace {

const double kPi = 3.14159265358979323846;

// The BC-spline members: `hasBC` false means this filter is not in that family (Impulse and the
// three sinc-family names), and the table *is* the mapping recorded in the reference file.
struct Spec
{
    bool bc;       // expressed by (B, C)
    bool sinc;     // Lanczos / sinc family
    double B, C;   // when bc
    int a;          // sinc window: 0 = truncated sinc, 2 or 3 = sinc(x)*sinc(x/a)
};

Spec specOf(Filter f)
{
    Spec s = { false, false, 0.0, 0.0, 0 };
    switch (f) {
        case kFilterImpulse:  break;                              // a box: nearest pixel
        case kFilterCubic:    s.bc = true; s.B = 0.0;  s.C = 0.0;  break;
        case kFilterKeys:     s.bc = true; s.B = 0.0;  s.C = 0.5;  break;
        case kFilterSimon:    s.bc = true; s.B = 0.0;  s.C = 0.75; break;
        case kFilterRifman:   s.bc = true; s.B = 0.0;  s.C = 1.0;  break;
        case kFilterMitchell: s.bc = true; s.B = 1.0 / 3.0; s.C = 1.0 / 3.0; break;
        case kFilterParzen:   s.bc = true; s.B = 1.0;  s.C = 0.0;  break;
        case kFilterNotch:    s.bc = true; s.B = 1.5;  s.C = -0.25; break;
        case kFilterLanczos4: s.sinc = true; s.a = 2; break;
        case kFilterLanczos6: s.sinc = true; s.a = 3; break;
        case kFilterSinc4:    s.sinc = true; s.a = 0; break;
        default: break;
    }
    return s;
}

double sincPi(double t)
{
    if (t == 0.0) return 1.0;
    const double p = kPi * t;
    return std::sin(p) / p;
}

// Integer support, from the same table: the point at which every kernel here is exactly 0. It is
// the radius the reference file's table declares, and the number `filterRadius` publishes.
int supportOf(Filter f)
{
    switch (f) {
        case kFilterImpulse:  return 0;                       // a box: one tap
        case kFilterCubic:    return 1;                       // B = C = 0: the outer branch of the
                                                              // BC-spline is identically zero
        case kFilterLanczos6: return 3;                       // sinc(x) * sinc(x / 3)
        case kFilterLanczos4:                                 // sinc(x) * sinc(x / 2)
        case kFilterSinc4:    return 2;                       // sinc(x), 4 taps
        default:              return 2;                       // every other BC-spline: 4 taps
    }
}

int lowerName(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

bool sameName(const char* a, const char* b)
{
    for (int i = 0;; ++i) {
        const int ca = lowerName(a[i]), cb = lowerName(b[i]);
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
}

} // namespace

const char* const kFilterNames[kFilterCount] = {
    "impulse", "cubic", "keys", "simon", "rifman", "mitchell",
    "parzen",  "notch", "lanczos4", "lanczos6", "sinc4"
};

bool filterFromName(const char* name, Filter& out)
{
    if (!name) return false;
    for (int i = 0; i < kFilterCount; ++i) {
        if (sameName(name, kFilterNames[i])) {
            out = Filter(i);
            return true;
        }
    }
    return false;
}

Filter filterOrDefault(const char* name, Filter fallback)
{
    Filter f = fallback;
    return filterFromName(name, f) ? f : fallback;
}

const char* filterName(Filter f)
{
    if (f < 0 || f >= kFilterCount) return kFilterNames[kFilterCubic];
    return kFilterNames[f];
}

double bcSpline(double t, double B, double C)
{
    if (t < 0.0) t = -t;
    const double t2 = t * t, t3 = t2 * t;
    if (t < 1.0)
        return ((12.0 - 9.0 * B - 6.0 * C) * t3 + (-18.0 + 12.0 * B + 6.0 * C) * t2 + (6.0 - 2.0 * B)) / 6.0;
    if (t < 2.0)
        return ((-B - 6.0 * C) * t3 + (6.0 * B + 30.0 * C) * t2 + (-12.0 * B - 48.0 * C) * t + (8.0 * B + 24.0 * C)) / 6.0;
    return 0.0;
}

double kernelValue(Filter f, double t)
{
    if (t < 0.0) t = -t;
    const Spec s = specOf(f);
    if (f == kFilterImpulse) return 1.0;                 // one tap, all the mass
    if (s.bc) return bcSpline(t, s.B, s.C);
    if (s.a == 0) return t < 2.0 ? sincPi(t) : 0.0;      // truncated sinc: 4 taps
    return t < double(s.a) ? sincPi(t) * sincPi(t / double(s.a)) : 0.0;
}

double filterRadius(Filter f)
{
    return double(supportOf(f));
}

double filterPeak(Filter f)
{
    return kernelValue(f, 0.0);
}

bool filterInterpolates(Filter f)
{
    if (f == kFilterImpulse) return true;
    const Spec s = specOf(f);
    if (s.bc) return s.B == 0.0;      // k(0) = (6 - 2B)/6, so B > 0 approximates
    return s.sinc;                    // every sinc window here is 1 at 0 and 0 at the integers
}

double gatherDilation(Filter f, double gain)
{
    const double base = filterRadius(f);
    if (base <= 0.0) return 1.0;                    // Impulse: one tap whatever the gain
    if (!(gain > 1.0)) return 1.0;                  // 1, 0, negative, NaN: nothing to dilate by
    const double cap = kMaxGatherRadius / base;     // the hardest dilation this filter may dilate by
    return gain > cap ? cap : gain;
}

double gatherRadius(Filter f, double gain)
{
    const double base = filterRadius(f);
    if (base <= 0.0) return 0.0;                    // Impulse never dilates
    return base * gatherDilation(f, gain);          // never past kMaxGatherRadius, by construction
}

int gatherTaps(Filter f, double gain)
{
    // ceil, not floor: a dilated kernel has its last non-zero tap *between* whole pixels when the
    // dilation is not a whole number, and a window rounded down would drop it.
    return 2 * int(std::ceil(gatherRadius(f, gain))) + 1;
}

int gatherPad(Filter f, double gain)
{
    return (gatherTaps(f, gain) - 1) / 2;
}

void axisWeights(Filter f, double pos, double gain, Axis& a)
{
    a.k0 = 0;
    a.taps = 1;
    a.sum = 0.0;
    a.w[0] = 0.0;
    if (!std::isfinite(pos)) return;              // nothing to read: weights stay inert

    const double g = gatherDilation(f, gain);     // the dilation the window is sized from, too
    const int half = int(std::ceil(gatherRadius(f, gain)));
    int taps = 2 * half + 1;
    if (taps > kMaxKernelTaps) taps = kMaxKernelTaps;    // guard, cannot happen past the cap

    // Centred on the *nearest* pixel, so the tap count does not depend on where between two pixels
    // the position sits.
    const long centre = std::lrint(pos);
    a.k0 = int(centre - half);
    a.taps = taps;

    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const double v = kernelValue(f, (pos - double(a.k0 + i)) / g);
        a.w[i] = v;
        sum += v;
    }

    a.sum = sum;                                  // what the kernel itself weighed: 1 at gain 1,
                                                  // ~g when dilated - the 1/g the average owes
    if (sum != 0.0 && sum != 1.0) {
        for (int i = 0; i < taps; ++i) a.w[i] = a.w[i] / sum;
    }
}

namespace {

// The dilation one run gathers with along each axis: a per-pixel array when the render knows how much
// *this* output pixel minifies (D-87), or one number for the whole run when it does not - which is what
// a cell's plan gain is, and what every caller of `gatherRun` hands over. A null array means "the
// constant", so both entry points share one body and neither pays for a fill.
struct RunGains
{
    const double* px;
    const double* py;
    double cx;
    double cy;

    RunGains(const double* px_, const double* py_, double cx_, double cy_)
        : px(px_), py(py_), cx(cx_), cy(cy_) {}

    double xAt(int i) const { return px ? px[i] : cx; }
    double yAt(int i) const { return py ? py[i] : cy; }
};

void gatherRunImpl(const SourceWindow& src, Filter f, const double* sx, const double* sy, int n,
                   float* out, int nchans, const RunGains& gains)
{
    if (n <= 0 || !out || nchans <= 0) return;

    // How many channels there is to *read* is the window's business; how many there are to *write* is
    // the caller's. A host that hands the render nothing on the clip still expects an image of the
    // render's own component count, and a window with fewer channels than the render has is read for
    // what it has and zeroed for the rest - which is what "no input" means to the composite.
    const int nc = src.empty() ? 0 : std::min(nchans, src.channels());

    // Accumulated in double so a 65-tap Lanczos6 sums the same way whatever order the taps
    // arrive in, and the float the host is handed is one rounding, not sixty.
    double stack[16];
    std::vector<double> heap;
    double* acc = stack;
    if (nc > 16) { heap.resize(std::size_t(nc)); acc = heap.data(); }

    if (nc == 0) {
        // No pixels to read: the caller decides coverage and composites the background, so this
        // delivers nothing rather than inventing an edge - but it delivers it *as the render's own
        // channel count*, so the output pixels are written and not left at whatever they held.
        for (int i = 0; i < n; ++i) {
            float* o = out + std::size_t(i) * nchans;
            for (int ch = 0; ch < nchans; ++ch) o[ch] = 0.0f;
        }
        return;
    }

    Axis ax, ay;

    for (int i = 0; i < n; ++i) {
        axisWeights(f, sx[i], gains.xAt(i), ax);
        axisWeights(f, sy[i], gains.yAt(i), ay);

        for (int ch = 0; ch < nc; ++ch) acc[ch] = 0.0;

        for (int j = 0; j < ay.taps; ++j) {
            const double wv = ay.w[j];
            if (wv == 0.0) continue;                 // past the kernel's support: free
            const float* row = src.row(ay.k0 + j);
            if (!row) continue;
            for (int k = 0; k < ax.taps; ++k) {
                const double w = wv * ax.w[k];
                if (w == 0.0) continue;
                const float* p = src.pixel(row, ax.k0 + k);
                if (!p) continue;
                for (int ch = 0; ch < nc; ++ch) acc[ch] += w * double(p[ch]);
            }
        }

        float* o = out + std::size_t(i) * nchans;
        for (int ch = 0; ch < nchans; ++ch) o[ch] = (ch < nc) ? float(acc[ch]) : 0.0f;
    }
}

} // namespace

void gatherRun(const SourceWindow& src, Filter f, const double* sx, const double* sy, int n,
               float* out, int nchans, double gainX, double gainY)
{
    gatherRunImpl(src, f, sx, sy, n, out, nchans, RunGains(0, 0, gainX, gainY));
}

void gatherRunPerPixel(const SourceWindow& src, Filter f, const double* sx, const double* sy, int n,
                       float* out, int nchans, const double* gainX, const double* gainY)
{
    // A null array asks for no dilation along that axis, not for the cell's: this entry point is the
    // one that knows the difference between "this pixel minifies" and "this run has no gain to report".
    gatherRunImpl(src, f, sx, sy, n, out, nchans, RunGains(gainX, gainY, 1.0, 1.0));
}

} // namespace gw
