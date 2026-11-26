// Implementation of the GridWrap render core. See render.h for the pipeline, the copy paths and
// the coordinate convention; the numbers behind the filters are resample.h's and behind the grids
// are grid.h's.

#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace gw {

namespace {

// A window that is not there: every read from it is zero, which is what "no input connected" means
// to the composite.
const SourceWindow kNoWindow;

int lowerName(char c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

// Option strings are compared as words: case is irrelevant, and `_` / `-` / repeated spaces are
// spelling, not meaning - `output` is a choice knob whose value a script may write any of these ways.
bool sameOption(const char* a, const char* b)
{
    int i = 0, j = 0;
    for (;;) {
        // Collapse runs of space-ish characters on both sides before comparing one character.
        while (a[i] == ' ' || a[i] == '\t' || a[i] == '_' || a[i] == '-' || a[i] == '.') ++i;
        while (b[j] == ' ' || b[j] == '\t' || b[j] == '_' || b[j] == '-' || b[j] == '.') ++j;

        const int ca = lowerName(a[i]), cb = lowerName(b[j]);
        if (ca != cb) return false;
        if (ca == 0) return true;
        ++i;
        ++j;
    }
}

// The dissolve every step here uses, in the same form as common/ofxlab_maskmix.h's blendOut: the
// end points are returned untouched, so `t == 0` gives the base and `t == 1` gives the effect
// bit-for-bit, and a value dissolved towards *itself* comes back exactly equal. That last property
// is what makes an uncovered pixel's `background_mix` a no-op instead of a rounding step.
inline double dissolveTo(double base, double t, double effect)
{
    if (t >= 1.0) return effect;
    if (t <= 0.0) return base;
    return base + t * (effect - base);
}

// The mask weight, which is common/ofxlab_maskmix.h's maskMix without the master `mix` (GridWrap's
// `mix` *is* the morph axis, D-54) plus Nuke's `fringe` variant. Clamping first is what makes a
// mask channel that carries -0.3 or 1.4 behave like a clamped one.
inline double maskWeight(double m, bool invert, bool fringe)
{
    if (!(m > 0.0)) m = 0.0;
    else if (m > 1.0) m = 1.0;
    if (invert) m = 1.0 - m;
    if (fringe) m = 4.0 * m * (1.0 - m);   // JOS's edge weight: 0 at both ends, 1 at m = 0.5
    return m;
}

// Offset of output pixel `i` in a row laid out with `nchans` interleaved channels.
inline std::size_t fnOf(int i, int nchans)
{
    return std::size_t(i) * std::size_t(nchans);
}

} // namespace

// ---------------------------------------------------------------------------
// The `output` and `background` choices
// ---------------------------------------------------------------------------

const char* const kOutputModeNames[kOutputModeCount] = {
    "source", "source warped", "destination", "destination warped", "morph"
};

const char* const kBackgroundModeNames[kBackgroundModeCount] = {
    "on black", "on src", "on dst", "on bg"
};

bool outputModeFromName(const char* name, OutputMode& out)
{
    if (!name) return false;
    for (int i = 0; i < kOutputModeCount; ++i) {
        if (sameOption(name, kOutputModeNames[i])) {
            out = OutputMode(i);
            return true;
        }
    }
    return false;
}

OutputMode outputModeOrDefault(const char* name, OutputMode fallback)
{
    OutputMode m;
    return outputModeFromName(name, m) ? m : fallback;
}

bool outputModeCorner(OutputMode m, double& warp, double& mix)
{
    switch (m) {
        case kOutputSource:           warp = 0.0; mix = 0.0; return true;
        case kOutputSourceWarped:     warp = 1.0; mix = 0.0; return true;
        case kOutputDestination:      warp = 1.0; mix = 1.0; return true;
        case kOutputDestinationWarped: warp = 0.0; mix = 1.0; return true;
        default: break;               // `morph`: the knobs decide (D-54)
    }
    return false;
}

bool backgroundModeFromName(const char* name, BackgroundMode& out)
{
    if (!name) return false;
    for (int i = 0; i < kBackgroundModeCount; ++i) {
        if (sameOption(name, kBackgroundModeNames[i])) {
            out = BackgroundMode(i);
            return true;
        }
    }
    return false;
}

BackgroundMode backgroundModeOrDefault(const char* name, BackgroundMode fallback)
{
    BackgroundMode m;
    return backgroundModeFromName(name, m) ? m : fallback;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

double Options::outputModeWarpOf(OutputMode m)
{
    double w = 0.0, x = 0.0;
    outputModeCorner(m, w, x);
    return w;
}

double Options::outputModeMixOf(OutputMode m)
{
    double w = 0.0, x = 0.0;
    outputModeCorner(m, w, x);
    return x;
}

void Options::clamp()
{
    renderRes = clampRenderRes(renderRes);
    if (filter < kFilterImpulse || filter >= kFilterCount) filter = kDefaultFilter;
    if (output < kOutputSource || output >= kOutputModeCount) output = kOutputSourceWarped;
    if (background < kBackgroundBlack || background >= kBackgroundModeCount) background = kBackgroundBlack;
    warp = clampUnit(warp);
    mix = clampUnit(mix);
    backgroundMix = clampUnit(backgroundMix);
    if (!(maxStretch >= 1.0)) maxStretch = 1.0;
}

int Options::backgroundInput() const
{
    switch (background) {
        case kBackgroundOnSrc: return 1;
        case kBackgroundOnDst: return 2;
        case kBackgroundOnBg:  return 3;
        default: break;
    }
    return 0;   // "on black"
}

// ---------------------------------------------------------------------------
// The render
// ---------------------------------------------------------------------------

WarpRender::WarpRender(const Grid& srcGrid, const Grid& dstGrid, const Options& opt)
    : _opt(opt), _sameGrids(false), _padSrc(0.0), _padDst(0.0), _cellCols(0)
{
    _opt.clamp();

    _srcGrid = srcGrid;
    _dstGrid = dstGrid;

    // One render needs one lattice: the two grids are addressed by the same (r,c). A host is
    // expected to keep `rows`/`cols` one pair for both grids (D-53); if it does not, grid.h's
    // rebuild rule (R9) carries the destination grid onto the source grid's topology rather than
    // inventing a second rule here, which is the same resample the plugin does when a user changes
    // the topology - not a clamp and not a truncation.
    if (_srcGrid.rows() != _dstGrid.rows() || _srcGrid.cols() != _dstGrid.cols()) {
        _dstGrid = resampleGrid(_dstGrid, _srcGrid.rows(), _srcGrid.cols(), _srcGrid.basis());
    }

    _sameGrids = gridsEqual(_srcGrid, _dstGrid);

    // The operative grid. At the corners of (warp, mix) that is literally the input grid the render
    // samples through, which is what makes the copy paths exact (D-59) - and it is also the cheap
    // answer: blending two grids to read it back at t = 0 or t = 1 would only re-round it.
    const double w = _opt.effectiveWarp();
    if (w <= 0.0) {
        _grid = _srcGrid;
    } else if (w >= 1.0) {
        _grid = _dstGrid;
    } else if (!blendGrids(_srcGrid, _dstGrid, w, _grid)) {
        _grid = _srcGrid;   // unreachable after the rebuild above, but a render needs *a* grid
    }

    _srcCopy = (w <= 0.0) || _sameGrids;
    _dstCopy = (w >= 1.0) || _sameGrids;

    _allSel = true;
    _anySel = false;
    for (int k = 0; k < 4; ++k) {
        if (!_opt.sel[k]) _allSel = false;
        else _anySel = true;
    }

    _cellCols = _grid.cellCols() > 0 ? _grid.cellCols() : 0;
    _inv.build(_grid, _opt.renderRes);
    buildPlan();

    _identity = copyKind() == kCopySource && _opt.background == kBackgroundOnSrc &&
                _opt.backgroundMix == 0.0 && !_opt.inject;
}

void WarpRender::buildPlan()
{
    _plan.clear();
    _planBounds = Rect();
    _padSrc = 0.0;
    _padDst = 0.0;

    const std::vector<CellPlan> plans = planCells(_srcGrid, _dstGrid, _grid, _opt.renderRes);
    _plan.reserve(plans.size());

    for (std::size_t i = 0; i < plans.size(); ++i) {
        const CellPlan& p = plans[i];

        CellPlanEntry e;
        e.r = p.r;
        e.c = p.c;
        e.outBox = p.outBox;
        e.srcBox = p.srcBox;
        e.dstBox = p.dstBox;
        e.srcGain = p.srcGain;
        e.dstGain = p.dstGain;
        e.padSrc = gatherPad(_opt.filter, p.srcGain);
        e.padDst = gatherPad(_opt.filter, p.dstGain);
        e.used = makeCell(_srcGrid, p.r, p.c, e.srcCell) && makeCell(_dstGrid, p.r, p.c, e.dstCell);
        if (!e.used) {
            // No net to sample through: the cell still occupies its index (so a root's (r,c) stays
            // a plan index), it is just never selected, and a pixel whose best root was in it is
            // treated as uncovered - which is the conservative answer, the background.
            e.srcGain = 1.0;
            e.dstGain = 1.0;
            e.padSrc = 0;
            e.padDst = 0;
        }

        // Only the inputs this render actually gathers contribute to the pad it has to ask for.
        if (e.used) {
            const bool needSrc = _opt.effectiveMix() < 1.0 && !_srcCopy;
            const bool needDst = _opt.effectiveMix() > 0.0 && !_dstCopy;
            if (needSrc && double(e.padSrc) > _padSrc) _padSrc = double(e.padSrc);
            if (needDst && double(e.padDst) > _padDst) _padDst = double(e.padDst);
            _planBounds = Rect::unite(_planBounds, p.outBox);
        }

        _plan.push_back(e);
    }
}

Rect WarpRender::footprint() const
{
    return Rect::unite(gridFootprint(_grid), _grid.basis());
}

double WarpRender::maxGatherPad() const
{
    return std::max(_padSrc, _padDst);
}

Rect WarpRender::sourceRequest(const Rect& window) const
{
    // Nothing to write means nothing to read. An empty *or reversed* window answers as a rect with no
    // extent at the window's own corner, rather than as the neighbourhood of every cell the plan happens
    // to have - and rather than as the swapped pair, which a host that floors and ceils the answer would
    // read as a real rectangle on the far side of the frame (tests/gw_render_test.cpp H4b/H4c).
    if (window.empty()) return Rect(window.x0, window.y0, window.x0, window.y0);

    // The Source pixel at each output pixel is read nearest (step 5's base and step 6's merge-back),
    // so a request always has to cover the window itself.
    Rect acc = window;
    if (_srcCopy) return acc;   // nothing is gathered out of it

    const double slack = std::max(_inv.tolerance(), 2.0 * _inv.bucketSize());
    const Rect probe = window.inflated(slack);
    for (std::size_t i = 0; i < _plan.size(); ++i) {
        const CellPlanEntry& e = _plan[i];
        if (!e.used) continue;
        if (_opt.effectiveMix() >= 1.0) continue;              // the source is not sampled at all
        if (_srcCopy) continue;
        if (Rect::intersect(e.outBox, probe).empty()) continue;
        // The pad the cell's own gain owes, plus one pixel: a request is a rect in canonical space and
        // a host delivers *whole pixels*, and the tap farthest from a sample position sits half a pixel
        // outside the position's own pixel cell (tests/gw_render_test.cpp F7 measures which taps the
        // render actually reads). Without that pixel the gather would reach past what the host allocated
        // and read edge-extended values instead of the real ones.
        acc = Rect::unite(acc, e.srcBox.inflated(double(e.padSrc) + 1.0));
    }
    return acc;
}

Rect WarpRender::destRequest(const Rect& window) const
{
    if (window.empty()) return Rect(window.x0, window.y0, window.x0, window.y0);

    Rect acc = window;
    if (_dstCopy) return acc;

    const double slack = std::max(_inv.tolerance(), 2.0 * _inv.bucketSize());
    const Rect probe = window.inflated(slack);
    for (std::size_t i = 0; i < _plan.size(); ++i) {
        const CellPlanEntry& e = _plan[i];
        if (!e.used) continue;
        if (_opt.effectiveMix() <= 0.0) continue;
        if (_dstCopy) continue;
        if (Rect::intersect(e.outBox, probe).empty()) continue;
        acc = Rect::unite(acc, e.dstBox.inflated(double(e.padDst) + 1.0));
    }
    return acc;
}

WarpRender::CopyKind WarpRender::copyKind() const
{
    const double mix = _opt.effectiveMix();
    if (mix == 0.0) return _srcCopy ? kCopySource : kCopyNone;
    if (mix == 1.0) return _dstCopy ? kCopyDest : kCopyNone;
    return (_srcCopy && _dstCopy) ? kCopyBlend : kCopyNone;
}

bool WarpRender::isSourceCopy() const
{
    return _identity;
}

Rect WarpRender::emptyAt(const Rect& w)
{
    return Rect(w.x0, w.y0, w.x0, w.y0);
}

// The interval renderRow's mask weight can take for this knob state. An unconnected mask is fully on
// (D-20), so the weight is exactly computable: 1, or 0 once `maskInvert` flips it, and 0 again under
// `fringe` because 4·m·(1−m) is 0 at m = 1 (and `maskInvert` first makes that m a 0, which the fringe
// formula also takes to 0). A connected mask carries any value in [0,1], so only the bounds are knowable
// - which is the conservative direction for a plugin answering a host.
WarpRender::WeightRange WarpRender::maskWeightBounds(bool maskConnected) const
{
    if (maskConnected) return WeightRange(0.0, 1.0);
    double m = 1.0;
    if (_opt.maskInvert) m = 0.0;
    if (_opt.fringe) m = 4.0 * m * (1.0 - m);
    return WeightRange(m, m);
}

// Step 5's `skipBase` is the only way a pixel's Source base is never read, and it needs every channel
// selected (`channels`) *and* that pixel's mask weight equal to 1. So the base is provably unread only
// when the weight is 1 at *every* pixel: with a mask connected the interval is [0,1] and any pixel can
// carry 0.4, which is a real read, so the lower bound is what this question needs. With no mask
// connected renderRow's weight is computable exactly (D-20), and only `maskInvert` or `fringe` can move
// it off 1.
bool WarpRender::canSkipBase(bool maskConnected) const
{
    return _allSel && maskWeightBounds(maskConnected).lo >= 1.0;
}

// Whether step 2's warped sample (and the step 3/4 background composite built on top of it) can be what
// a delivered pixel holds. It is thrown away wherever step 5 dissolves it back to the Source base or step
// 6 merges an unselected channel back from that base - which is what makes an unconnected `maskInvert` or
// `fringe` render read nothing but the Source. The question is existential, so it takes the *upper* bound:
// some pixel can hold the effect as soon as the weight can come out above 0 anywhere.
bool WarpRender::canWriteEffect(bool maskConnected) const
{
    return anyChannelDelivered() && maskWeightBounds(maskConnected).hi > 0.0;
}

// Whether what step 3 fills an uncovered pixel with - or step 4's `background_mix` dissolve - survives to
// a delivered pixel: same answer as the effect's, because the value it names travels the same path.
bool WarpRender::canReachBackground(bool maskConnected) const
{
    return canWriteEffect(maskConnected);
}

// Whether `channels` selects at least one of r, g, b, a. The predicates cannot know how many channels the
// host is rendering (an RGBA clip may be rendered as RGB), so this answers for the widest case; the
// narrow one is applied where the selection actually happens, in renderRow's `logicalChannel(k, nchans)`.
// With nothing selected, every delivered channel is merged back from the Source base and the warped
// sample is thrown away everywhere - which is also one of isIdentity's three claims (D-59).
bool WarpRender::anyChannelDelivered() const
{
    return _anySel;
}

bool WarpRender::readsSource(bool maskConnected) const
{
    // A Source-passthrough render returns from renderRow before any of the steps below are reached: it
    // reads the Source and nothing else, the mask included (D-59).
    if (_identity) return true;

    // Step 2 samples the Source whenever the morph weight leaves any of the picture's own input in it and
    // that warped sample can still reach a delivered pixel.
    if (_opt.effectiveMix() < 1.0 && canWriteEffect(maskConnected)) return true;

    // Steps 3-4 fill an uncovered pixel from it, and `background_mix` dissolves a covered pixel towards
    // it, when the `background` choice names it and the value it names survives steps 5-6.
    if (_opt.backgroundInput() == 1 && canReachBackground(maskConnected)) return true;

    // Step 6 merges every channel `channels` left unselected back from the Source base, and step 5's base
    // is read wherever its weight comes out under 1 - renderRow's `skipBase` inverted. With no mask
    // connected that weight is computable exactly, so `maskInvert` - which makes it 0 - is read here as
    // what it is: D-20's "an unconnected mask with `maskInvert` passes the source straight through",
    // delivered *by the Source clip*. With a mask connected any pixel can carry a weight under 1, so the
    // base has to be askable-for there too.
    if (!_allSel || !canSkipBase(maskConnected)) return true;

    return false;
}

bool WarpRender::readsDest(bool maskConnected) const
{
    if (_identity) return false;   // a passthrough render reads the Source only

    // Step 2 samples Dest whenever its morph weight is not 0 - nearest when the operative grid *is* the
    // destination grid (D-59), gathered otherwise - and the warped sample can still reach a delivered
    // pixel. A mask whose weight can only ever be 0 dissolves that sample away entirely, which B11
    // measures as "replacing the Dest input changes not one float".
    if (_opt.effectiveMix() > 0.0 && canWriteEffect(maskConnected)) return true;

    // ... and steps 3-4 / `background_mix` read it when the `background` choice names it.
    return _opt.backgroundInput() == 2 && canReachBackground(maskConnected);
}

bool WarpRender::readsBackground(bool maskConnected) const
{
    // The Background input is only ever the background: no step samples it as the warped picture. A
    // Source-passthrough render never reaches step 3 at all, and a mask weight that can only be 0 replaces
    // what step 3 filled in with the Source base, so the Background arrives and is then thrown away.
    if (_identity) return false;
    return _opt.backgroundInput() == 3 && canReachBackground(maskConnected);
}

bool WarpRender::readsMask(bool maskConnected) const
{
    // The Source-passthrough render returns from renderRow before a mask is consulted, so a mask
    // connected to such a node is read by nothing. Otherwise an unconnected mask is the constant 1 (D-20)
    // - which is what `inject` then writes into alpha, with no input read for it - and a connected one
    // can carry any value the dissolve needs.
    if (_identity) return false;
    return maskConnected;
}

Rect WarpRender::sourceRoI(const Rect& window, bool maskConnected) const
{
    if (!readsSource(maskConnected)) return emptyAt(window);
    return sourceRequest(window);
}

Rect WarpRender::destRoI(const Rect& window, bool maskConnected) const
{
    if (!readsDest(maskConnected)) return emptyAt(window);
    return destRequest(window);
}

Rect WarpRender::backgroundRoI(const Rect& window, bool maskConnected) const
{
    if (!readsBackground(maskConnected)) return emptyAt(window);
    return window;   // an uncovered pixel takes the background pixel *at that output pixel*: no pad owed
}

Rect WarpRender::maskRoI(const Rect& window, bool maskConnected) const
{
    // Same nearest read as the background: renderRow samples the mask at the output pixel it is composing,
    // so the window is the whole of what it can ask for.
    if (!readsMask(maskConnected)) return emptyAt(window);
    return window;
}

int WarpRender::logicalChannel(int k, int nchans)
{
    if (nchans == 1) return 3;      // an alpha-only render's single channel *is* the alpha channel
    if (k >= 0 && k < 4) return k;
    return 3;
}

int WarpRender::alphaIndex(int nchans)
{
    return nchans == 4 ? 3 : -1;    // only an RGBA render has a channel `inject` may overwrite
}

void WarpRender::ensureFloats(std::vector<float>& v, std::size_t n)
{
    if (v.size() < n) v.assign(n, 0.0f);
}

void WarpRender::ensureDoubles(std::vector<double>& v, std::size_t n)
{
    if (v.size() < n) v.assign(n, 0.0);
}

void WarpRender::ensureInts(std::vector<int>& v, std::size_t n)
{
    if (v.size() < n) v.assign(n, 0);
}

void WarpRender::ensureVecs(std::vector<Vec>& v, std::size_t n)
{
    if (v.size() < n) v.assign(n, Vec());
}

void WarpRender::nearestRead(const SourceWindow& in, int x, int y, float* out, int nchans)
{
    if (!out || nchans <= 0) return;

    const float* p = 0;
    if (!in.empty()) {
        const int ix0 = in.x0(), iy0 = in.y0();
        if (x >= ix0 && x < ix0 + in.width() && y >= iy0 && y < iy0 + in.height()) {
            p = in.pixels() + std::size_t(y - iy0) * std::size_t(in.rowStride())
              + std::size_t(x - ix0) * std::size_t(in.channels());
        }
    }

    if (!p) {
        for (int k = 0; k < nchans; ++k) out[k] = 0.0f;
        return;
    }
    const int n = std::min(nchans, in.channels());
    for (int k = 0; k < n; ++k) out[k] = p[k];
    for (int k = n; k < nchans; ++k) out[k] = 0.0f;
}

void WarpRender::renderRow(int x1, int x2, int y, const RenderInputs& inputs,
                           const MaskSampler& mask, float* out, int nchans)
{
    if (!out || x2 <= x1 || nchans <= 0) return;

    const int n = x2 - x1;
    const std::size_t fn = std::size_t(n) * std::size_t(nchans);

    ensureInts(_cell, std::size_t(n));
    ensureDoubles(_sx, std::size_t(n));
    ensureDoubles(_sy, std::size_t(n));
    ensureDoubles(_hu, std::size_t(n));
    ensureDoubles(_hv, std::size_t(n));
    ensureDoubles(_gx, std::size_t(n));
    ensureDoubles(_gy, std::size_t(n));
    ensureVecs(_odu, std::size_t(n));
    ensureVecs(_odv, std::size_t(n));
    ensureFloats(_a, fn);
    ensureFloats(_b, fn);
    ensureFloats(_eff, fn);

    // The whole render is the Source image at the same pixels: covered or not, whatever a mask
    // says, every step either returns the source pixel or dissolves it towards itself (D-59).
    if (_identity) {
        for (int i = 0; i < n; ++i) {
            nearestRead(inputs.src, x1 + i, y, out + std::size_t(i) * std::size_t(nchans), nchans);
        }
        return;
    }

    const SourceWindow* bgWin = &kNoWindow;
    switch (_opt.backgroundInput()) {
        case 1: bgWin = &inputs.src; break;
        case 2: bgWin = &inputs.dst; break;
        case 3: bgWin = &inputs.bg; break;
        default: break;
    }

    // ---- step 1: coverage, and the cell each covered pixel belongs to ----------------------
    // The cell index is the run key, because a run shares one cell's plan entry: which input nets to
    // evaluate, that input's gather pad, and the ceiling every pixel's own dilation is clamped to (D-87).
    const bool dilate = filterRadius(_opt.filter) > 0.0;   // Impulse dilates nothing, gain or no gain
    for (int i = 0; i < n; ++i) {
        _cell[i] = -1;
        if (!_inv.built()) continue;

        const double qx = double(x1 + i) + 0.5;   // canonical centre of output pixel x1+i (D-60)
        const double qy = double(y) + 0.5;
        if (!_inv.find(qx, qy, _hit)) continue;

        const int idx = _hit.r * _cellCols + _hit.c;
        if (idx < 0 || std::size_t(idx) >= _plan.size()) continue;
        const CellPlanEntry& entry = _plan[size_t(idx)];
        if (!entry.used) continue;

        _cell[i] = idx;
        _hu[i] = _hit.u;
        _hv[i] = _hit.v;

        // The operative net's partials at the root, kept for the pixel's own gain (D-87). Only where a
        // gather can dilate at all: a cell whose plan gain is 1 blurs by the filter's base radius
        // whatever the local slope is, and that is the answer the plan already reports.
        if (dilate && (entry.srcGain > 1.0 || entry.dstGain > 1.0)) {
            _odu[i] = _hit.du;
            _odv[i] = _hit.dv;
        }
    }

    const double mix = _opt.effectiveMix();
    const bool needSrc = mix < 1.0;
    const bool needDst = mix > 0.0;

    // ---- steps 2-4: the warped / morph sample, then the background composite --------------
    int i = 0;
    while (i < n) {
        int j = i + 1;
        while (j < n && _cell[j] == _cell[i]) ++j;
        const int len = j - i;
        const int idx = _cell[i];

        if (idx < 0) {
            // Uncovered: step 3 gives the pixel to the background, and step 4 then dissolves it
            // towards that same background, which dissolveTo answers exactly (D-58).
            for (int t = 0; t < len; ++t) {
                nearestRead(*bgWin, x1 + i + t, y, _eff.data() + fnOf(i + t, nchans), nchans);
            }
            i = j;
            continue;
        }

        const CellPlanEntry& e = _plan[size_t(idx)];

        if (needSrc) {
            if (_srcCopy) {
                for (int t = 0; t < len; ++t) {
                    nearestRead(inputs.src, x1 + i + t, y, _a.data() + fnOf(i + t, nchans), nchans);
                }
            } else {
                // What each pixel of this run dilates the kernel by (D-87). The plan's gain is the
                // *worst* pixel of the cell, so gathering the whole run at that one number blurs the
                // pixels of the cell that do not minify at all. The input net's partials come out of
                // the evaluation that already produces the sample position, so this costs no pass.
                const bool localSrc = dilate && e.srcGain > 1.0;
                Vec idu, idv;
                Vec* pdu = 0;
                Vec* pdv = 0;
                if (localSrc) { pdu = &idu; pdv = &idv; }
                for (int t = 0; t < len; ++t) {
                    Vec pos;
                    evalCell(e.srcCell, _hu[i + t], _hv[i + t], &pos, pdu, pdv);
                    _sx[i + t] = samplePos(pos.x);
                    _sy[i + t] = samplePos(pos.y);
                    if (localSrc)
                        pixelGain(idu, idv, _odu[i + t], _odv[i + t], e.srcGain, _gx[i + t], _gy[i + t]);
                }
                if (localSrc) {
                    gatherRunPerPixel(inputs.src, _opt.filter, _sx.data() + i, _sy.data() + i, len,
                                      _a.data() + fnOf(i, nchans), nchans, _gx.data() + i, _gy.data() + i);
                } else {
                    gatherRun(inputs.src, _opt.filter, _sx.data() + i, _sy.data() + i, len,
                              _a.data() + fnOf(i, nchans), nchans, e.srcGain, e.srcGain);
                }
            }
        }

        if (needDst) {
            if (_dstCopy) {
                for (int t = 0; t < len; ++t) {
                    nearestRead(inputs.dst, x1 + i + t, y, _b.data() + fnOf(i + t, nchans), nchans);
                }
            } else {
                // Same law on the destination input, against its own plan gain (D-87). `_gx` / `_gy` are
                // run scratch, like `_sx` / `_sy`: each input fills them again before it gathers.
                const bool localDst = dilate && e.dstGain > 1.0;
                Vec jdu, jdv;
                Vec* pdu = 0;
                Vec* pdv = 0;
                if (localDst) { pdu = &jdu; pdv = &jdv; }
                for (int t = 0; t < len; ++t) {
                    Vec pos;
                    evalCell(e.dstCell, _hu[i + t], _hv[i + t], &pos, pdu, pdv);
                    _sx[i + t] = samplePos(pos.x);
                    _sy[i + t] = samplePos(pos.y);
                    if (localDst)
                        pixelGain(jdu, jdv, _odu[i + t], _odv[i + t], e.dstGain, _gx[i + t], _gy[i + t]);
                }
                if (localDst) {
                    gatherRunPerPixel(inputs.dst, _opt.filter, _sx.data() + i, _sy.data() + i, len,
                                      _b.data() + fnOf(i, nchans), nchans, _gx.data() + i, _gy.data() + i);
                } else {
                    gatherRun(inputs.dst, _opt.filter, _sx.data() + i, _sy.data() + i, len,
                              _b.data() + fnOf(i, nchans), nchans, e.dstGain, e.dstGain);
                }
            }
        }

        float* effRow = _eff.data() + fnOf(i, nchans);
        if (needSrc && !needDst) {
            std::memcpy(effRow, _a.data() + fnOf(i, nchans), std::size_t(len) * std::size_t(nchans) * sizeof(float));
        } else if (needDst && !needSrc) {
            std::memcpy(effRow, _b.data() + fnOf(i, nchans), std::size_t(len) * std::size_t(nchans) * sizeof(float));
        } else {
            const float* pa = _a.data() + fnOf(i, nchans);
            const float* pb = _b.data() + fnOf(i, nchans);
            const double wa = 1.0 - mix, wb = mix;
            const std::size_t cnt = std::size_t(len) * std::size_t(nchans);
            for (std::size_t f = 0; f < cnt; ++f) effRow[f] = float(wa * double(pa[f]) + wb * double(pb[f]));
        }

        // `background_mix` on covered pixels only: an uncovered pixel already holds the background,
        // and dissolving a value towards itself is exact.
        if (_opt.backgroundMix > 0.0) {
            const double b = _opt.backgroundMix;
            ensureFloats(_bgPix, std::size_t(nchans));
            for (int t = 0; t < len; ++t) {
                nearestRead(*bgWin, x1 + i + t, y, _bgPix.data(), nchans);
                float* dst = _eff.data() + fnOf(i + t, nchans);
                for (int k = 0; k < nchans; ++k) {
                    dst[k] = float(dissolveTo(double(dst[k]), b, double(_bgPix[k])));
                }
            }
        }

        i = j;
    }

    // ---- steps 5-7: mask dissolve, `channels` merge-back, `inject` -------------------------
    ensureFloats(_basePix, std::size_t(nchans));
    for (int t = 0; t < n; ++t) {
        const int x = x1 + t;
        float* dst = out + std::size_t(t) * std::size_t(nchans);
        const float* eff = _eff.data() + fnOf(t, nchans);

        const double mraw = mask.valueAt(x, y);
        const double m = maskWeight(mraw, _opt.maskInvert, _opt.fringe);

        // Step 5's base is the Source pixel at this output pixel. It cannot matter when the mask
        // weight is 1 and every channel is selected: then both step 5 and step 6 return the effect
        // untouched, and skipping the read cannot change a single float.
        const bool skipBase = _allSel && m >= 1.0;
        if (!skipBase) nearestRead(inputs.src, x, y, _basePix.data(), nchans);

        for (int k = 0; k < nchans; ++k) {
            double v;
            if (skipBase) {
                v = double(eff[k]);
            } else {
                v = dissolveTo(double(_basePix[k]), m, double(eff[k]));
                if (!_opt.sel[logicalChannel(k, nchans)]) v = double(_basePix[k]);
            }
            dst[k] = float(v);
        }

        if (_opt.inject) {
            const int ai = alphaIndex(nchans);
            if (ai >= 0) dst[ai] = float(mraw);   // the raw mask value, not the `maskInvert`ed one
        }
    }
}

void WarpRender::render(int x1, int y1, int x2, int y2, const RenderInputs& inputs,
                        const MaskSampler& mask, float* out, int nchans, int rowStride)
{
    if (!out || x2 <= x1 || y2 <= y1 || nchans <= 0) return;
    if (rowStride < nchans) rowStride = nchans;

    for (int y = y1; y < y2; ++y) {
        renderRow(x1, x2, y, inputs, mask, out + std::size_t(y - y1) * std::size_t(rowStride), nchans);
    }
}

} // namespace gw
