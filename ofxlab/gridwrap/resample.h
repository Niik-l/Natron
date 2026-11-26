// GridWrap: the resampling kernels and the separable gather (DECISIONS.md D-57).
//
// OFX-free like grid.h, and checked the same way (tests/gw_resample_test.cpp): a filter is a
// formula, so it is checked against formulas - support, mass, symmetry, what it reproduces
// exactly, what it costs - before a pixel is ever gathered.
//
// The eleven names and their shapes come from references/gridwrap/nuke_gridwrap_knobs.md
// *Filters*: seven of them are one published formula (the Mitchell-Netravali BC-spline) with a
// `(B, C)` fitted by measurement in Nuke 17, and the other four (`Impulse`, `Lanczos4`,
// `Lanczos6`, `Sinc4`) are the sinc-family members that page names. So this file transcribes
// that table rather than choosing kernels, and the order Foundry's own descriptions give -
// Cubic smoothest, Rifman sharpest, Sinc4 sharpest of all - falls out of the fitted numbers.
//
// ---------------------------------------------------------------------------
// What a gather is here
// ---------------------------------------------------------------------------
// GridWrap is an inverse warp: every output pixel knows the grid coordinate that lands on it
// (grid.h's Inverter), and therefore a position in the Source input and one in the Dest input.
// Resampling a position means convolving it with a kernel - and a warp minifies *and* magnifies
// in the same frame, in different places, so a filter has two obligations:
//
//   - **Support follows the kernel**, not a fixed tap count: a `Cubic` reach is 1 px, `Keys` /
//     `Mitchell` / `Parzen` / `Notch` / `Lanczos4` / `Sinc4` 2 px, `Lanczos6` 3 px.
//   - **The kernel is dilated where the grid minifies the input**: with `gain = max(1, |d input| /
//     |d operative|)` from `planCells()`, the weights are `k(d / gain)` and the window reaches
//     `ceil(base * gain)` input pixels (`gatherDilation` / `gatherRadius` / `gatherTaps`). Dilating -
//     not merely reading further out - is what makes that a *filter* rather than a wider window of
//     the same zeros: the kernel's own shape is stretched to cover the footprint an output pixel has
//     in the input, which is where the input's detail is. Without it a shrunken grid aliases exactly
//     the way Geremia's zone plate does when a Transform minifies with a narrow filter, and GridWrap
//     has no separate `blur` knob to fix it with. `Impulse` is the exception by definition: one tap,
//     whatever the gain, which is precisely why it aliases.
//
// The gather is **separable in the two screen axes** - horizontal weights applied to each source
// row inside the vertical span, then combined by the vertical weights - which is what makes the
// tap count `tapsX * tapsY` rather than a 2-D kernel, and why the horizontal weights are computed
// once per output pixel and reused across every row it reads. The alternative (filtering along the
// grid's own local axes) would be truer to a rotated grid but has no axis-aligned intermediate
// buffer to hold the first pass, so it is not what a CPU gather can do in one pass; the grid's
// rotation is handled instead by the RoI and the widened radius, and the residue is measured
// (tests/gw_resample_test.cpp section E).
//
// ---------------------------------------------------------------------------
// Two things every filter is held to here
// ---------------------------------------------------------------------------
//   - **Mass**: the taps of one axis are divided by their own sum, so a gather is an average and a
//     flat field stays flat whatever the radius ended up as, including past the cap below. For the
//     seven BC-splines that division is a no-op to within the last rounding digit (they have
//     partition of unity on their own lattice); for `Lanczos4`, `Lanczos6` and `Sinc4` it is **not**
//     - a truncated sinc does not sum to 1 on the integer lattice, and the division is part of what
//     those filters are here. A dilated kernel read on the *input's* lattice weighs `~ gain` for
//     every filter, and there the same division is the `1 / gain` the dilation owes the average.
//   - **Phase**: the tap indices are symmetric about the *nearest* pixel, so the tap count does not
//     depend on the fractional part of the position and no half-pixel bias creeps into the indexing.
//     Whether the weights' first moment `sum w_i (pos - index_i)` vanishes - i.e. whether a gather
//     reproduces a linear ramp exactly at every fractional offset - is a property of the kernel, not
//     of the gather: it is exactly 0 for `Keys` / `Mitchell` / `Parzen` / `Notch` / `Sinc4`, and
//     0.09622504 (= sqrt(3)/18) for `Cubic` and `Rifman`, 0.04811252 for `Simon`, 0.0317
//     `Lanczos4`, 0.0200 `Lanczos6`, 0.5 `Impulse`. The suite measures that ladder instead of
//     claiming a symmetry the kernels do not have.
//
// Radius is capped (`kMaxGatherRadius`) so the number of taps, the gather window and therefore the
// RoI a host is asked for are all bounded numbers whatever a user does to the grid. Past the cap the
// *dilation* stops (`gatherDilation`), which keeps the window the dilated kernel's whole support: a
// cell minified harder than the cap gets a narrower filter, not a truncated one, and an average
// either way. `clamp` is not a GridWarp knob, so a sharpening filter may ring, and this file does not
// hide it (that is Nuke's behaviour too).
#ifndef OFXLAB_GW_RESAMPLE_H
#define OFXLAB_GW_RESAMPLE_H

#include <cmath>
#include <cstddef>

namespace gw {

// ---------------------------------------------------------------------------
// The filters, named as Nuke's `filter` menu names them
// ---------------------------------------------------------------------------
enum Filter {
    kFilterImpulse = 0,  // nearest pixel: a box
    kFilterCubic,        // BC-spline B = 0, C = 0  - Nuke's default: no negative lobes at all
    kFilterKeys,         // B = 0, C = 1/2 (Catmull-Rom, Keys' family with a = -1/2)
    kFilterSimon,        // B = 0, C = 3/4
    kFilterRifman,       // B = 0, C = 1
    kFilterMitchell,     // B = C = 1/3
    kFilterParzen,       // B = 1, C = 0 (the cubic B-spline, the "Parzen window")
    kFilterNotch,        // B = 3/2, C = -1/4 (flattest of the menu: kills fine patterns)
    kFilterLanczos4,     // sinc(x) * sinc(x / 2): 4 pixels across
    kFilterLanczos6,     // sinc(x) * sinc(x / 3): 6 pixels across
    kFilterSinc4,        // sinc(x) truncated to its first 4 taps: the deepest lobes
    kFilterCount
};

// The `filter` knob's default, which is Nuke's: `Cubic`, the smoothest of the menu. The other
// statement of this number is ARCHITECTURE.md's GridWrap knob table and the descriptor in
// gridwrap.cpp (repo convention: a knob default is written once per plugin, and this is this
// knob's home because the filter *is* this file's table).
const Filter kDefaultFilter = kFilterCubic;

// The names as the descriptor declares them and as a script writes them.
extern const char* const kFilterNames[kFilterCount];

// Case-insensitive lookup (a host hands back the descriptor's own string; a hand-written script
// may capitalise it). Unknown or null gives `fallback`.
bool filterFromName(const char* name, Filter& out);
Filter filterOrDefault(const char* name, Filter fallback);
const char* filterName(Filter f);

// ---------------------------------------------------------------------------
// The kernels
// ---------------------------------------------------------------------------

// The kernel value at a distance: even in `t`, and 0 at or beyond `filterRadius`. `t` is in the
// kernel's own units, which at `gain == 1` is a distance in source pixels between a sample position
// and a source pixel centre; a dilated gather hands it that distance divided by the gain
// (`axisWeights`).
double kernelValue(Filter f, double t);

// The untouched support radius, in source pixels: 0 for Impulse, 1 for Cubic, 3 for Lanczos6,
// 2 for the rest. Half-integers never occur: every kernel here has an integer support.
double filterRadius(Filter f);

// k(0), the value at the centre: 1 for every interpolating member, 8/9 for Mitchell, 2/3 for
// Parzen, 1/2 for Notch. It is the number that says whether a filter softens a still image.
double filterPeak(Filter f);

// True when the kernel is 1 at 0 and 0 at every other whole pixel, i.e. an unmoved image comes
// back bit-identical (Impulse, Cubic, Keys, Simon, Rifman, Lanczos4, Lanczos6, Sinc4).
bool filterInterpolates(Filter f);

// The Mitchell-Netravali BC-spline, exposed because it *is* seven of the eleven names and a test
// checks the transcribed `(B, C)` pairs against the closed forms they are supposed to be.
double bcSpline(double t, double B, double C);

// ---------------------------------------------------------------------------
// Gather geometry: how wide a filter gets, and which taps it lands on
// ---------------------------------------------------------------------------

// Cap on the dilated radius, in source pixels. Bounds the taps, the gather window and the RoI
// request whatever the grid does (see the header note).
const double kMaxGatherRadius = 32.0;

// Largest tap count one axis can therefore have: 2 * cap + 1.
const int kMaxKernelTaps = 65;

// `gain` is `max(1, stretch)` from the cell plan. The dilated support is `base * gain`, capped.
double gatherRadius(Filter f, double gain);

// The dilation `axisWeights` actually uses: `gain`, shrunk when `gain * base` would pass the cap, so
// the tap window and the dilated kernel always agree (a cell minified harder than the cap gets an
// average over the cap, not a kernel with taps cut off mid-support). 1 for Impulse, whatever the gain.
double gatherDilation(Filter f, double gain);

// Taps one axis uses at that gain: odd, symmetric about the nearest pixel, and exactly
// `2 * ceil(gatherRadius) + 1` - the smallest window that holds every non-zero tap of the dilated
// kernel (see `axisWeights` and `gatherDilation`). 1 for Impulse, whatever the gain.
int gatherTaps(Filter f, double gain);

// One axis of one gather: `pos` is a sample position in window pixel coordinates (the centre of
// window pixel X is X), the weights are `kernelValue(f, (pos - index) / gatherDilation(f, gain))`
// normalised to sum to 1, and `k0` is the window index of the first tap. `taps` never exceeds
// kMaxKernelTaps. On a window this file cannot read (see SourceWindow) the weights are still what the
// kernel says: only the indices matter to the gather, which clamps them.
struct Axis {
    int k0;
    int taps;
    double sum;                  // the kernel's mass before renormalisation: 1 at gain 1, ~gain when dilated
    double w[kMaxKernelTaps];
};
void axisWeights(Filter f, double pos, double gain, Axis& a);

// ---------------------------------------------------------------------------
// The window a gather reads
// ---------------------------------------------------------------------------
// A rectangle of interleaved float channels, addressed in the *image's* coordinates, so a render
// pass can hand the core a window the host already clamped and the core still speaks in the same
// numbers the grid does. Reading outside the rectangle returns the edge pixel it ran into (edge
// extension), which is what makes a gather whose kernel crosses the input's border defined
// without asking the host for anything more. An empty window (no input connected, or a host that
// delivered nothing) gathers 0.
class SourceWindow {
public:
    SourceWindow()
        : _px(0), _stride(0), _x0(0), _y0(0), _w(0), _h(0), _n(0) {}

    // `pixels` is the window's (`x0`, `y0`) pixel — this rect's *minimum* y row, which is not the
    // same claim as a picture-relative "top left": canonical y increases up the picture (D-64), so
    // saying "top-left" here would say the opposite of what the base pointer is. `rowStride` in
    // floats (>= width * channels), (`x0`, `y0`) the window's origin in the image's coordinate
    // space, `w` x `h` its extent and `nchans` the interleaved channels per pixel.
    SourceWindow(const float* pixels, int rowStride, int w, int h, int x0, int y0, int nchans)
        : _px(pixels), _stride(rowStride), _x0(x0), _y0(y0), _w(w), _h(h), _n(nchans)
    {
        if (_w < 0) _w = 0;
        if (_h < 0) _h = 0;
        if (_n < 0) _n = 0;
    }

    const float* pixels() const { return _px; }
    int rowStride() const { return _stride; }
    int width() const { return _w; }
    int height() const { return _h; }
    int x0() const { return _x0; }
    int y0() const { return _y0; }
    int channels() const { return _n; }
    bool empty() const { return !_px || _w <= 0 || _h <= 0 || _n <= 0; }

    // Base pointer of window row `y`, clamped to the window. Null on an empty window.
    const float* row(int y) const
    {
        if (empty()) return 0;
        int r = y;
        if (r < _y0) r = _y0;
        if (r > _y0 + _h - 1) r = _y0 + _h - 1;
        return _px + std::size_t(r - _y0) * std::size_t(_stride);
    }

    // A pixel inside a row base returned by `row()`, clamped along x. Never null when `row` was.
    const float* pixel(const float* rowBase, int x) const
    {
        if (!rowBase) return 0;
        int c = x;
        if (c < _x0) c = _x0;
        if (c > _x0 + _w - 1) c = _x0 + _w - 1;
        return rowBase + std::size_t(c - _x0) * std::size_t(_n);
    }

private:
    const float* _px;
    int _stride, _x0, _y0, _w, _h, _n;
};

// ---------------------------------------------------------------------------
// The gather
// ---------------------------------------------------------------------------
// `n` output pixels in a row, each with its own source position; `gainX` / `gainY` are the cell's
// stretch factors, constant across the run, exactly as `planCells()` reports them.
// `out` is `n * nchans` floats and every one of them is written: `nchans` is the render's channel
// count, which is the caller's to decide and not something this can read out of a window that may be
// empty. A window with fewer channels than that is read for what it has and zeroed for the rest; an
// empty window writes zeros. Positions outside the window are read through the clamp, so a caller
// that wants "nothing there" composites the background itself rather than asking this to decide
// coverage (grid.h's Inverter is what decides it).
void gatherRun(const SourceWindow& src, Filter f, const double* sx, const double* sy, int n,
               float* out, int nchans, double gainX, double gainY);

// The same gather with the dilation given per pixel, which is what a minifying cell actually needs: its
// gain is the worst pixel in it, and a run gathered at the worst pixel's dilation blurs pixels that do
// not minify at all (D-87). `gainX` / `gainY` are `n` doubles each - the caller keeps them inside the
// cell's own plan gain, because `gatherPad(filter, cellGain)` is the pad the caller's window was
// allocated for and a wider kernel would read outside it. A null array means no dilation on that axis.
void gatherRunPerPixel(const SourceWindow& src, Filter f, const double* sx, const double* sy, int n,
                       float* out, int nchans, const double* gainX, const double* gainY);

// The pad a gather of this filter at this gain can reach beyond the sample positions, in whole
// source pixels - half the tap count, which is what a RoI request owes the filter. 0 for Impulse.
int gatherPad(Filter f, double gain);

} // namespace gw

#endif // OFXLAB_GW_RESAMPLE_H
