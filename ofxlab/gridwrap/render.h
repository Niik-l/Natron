// GridWrap: the render core (DECISIONS.md D-54, D-55, D-57, D-58, D-59, D-60).
//
// OFX-free, like grid.h and resample.h: the whole composition of the plugin lives here and every
// claim in it is checked headlessly (tests/gw_render_test.cpp) before the OFX layer in gridwrap.cpp
// asks a host for a pixel. What this file decides is everything *after* the grids: which input a
// query reads, where a query's sample position comes from, what happens where the grid does not
// reach, how the mask dissolves, and which pixels can be copied instead of gathered.
//
// The pipeline, per output pixel X (D-58 - the order is stated once, here, and every later step
// reads the result of the one before it):
//
//   1. (u,v) = Inverter(operative grid at `effectiveWarp()`).find(X).  Coverage *is* "a root was
//      found" (grid.h): where the grid does not reach, the pixel gets `background` instead.
//   2. The warped / morph sample. With S the source grid, D the destination grid and
//      G(t) = (1-t)S + tD (grid.h's blendGrids), the value is
//            out = (1-mix) * src[ S(u,v) ]  +  mix * dst[ D(u,v) ]
//      - the *parameter* comes from the blended grid, the *positions* from the two input grids,
//      which is what makes the four `output` modes the corners of (warp, mix) and makes two of
//      them exact copies (D-54, references/.../nuke_gridwrap_knobs.md *Output and morph*).
//   3. Composite with the background:  out = out where covered, background(X) where not.
//   4. `background_mix` dissolves that towards the same background:  out += b * (bg(X) - out).
//   5. The mask dissolve, against the **Source input's own pixel at that output pixel**:
//            out = src[X] + t * (out - src[X]),   t = m  or  4*m*(1-m) with `fringe`
//      m being the mask value at X (`1 - m` when `maskInvert`) and an unconnected mask fully on.
//      That is common/ofxlab_maskmix.h's rule, the one every plugin in this repo uses, and it is
//      what makes `maskInvert` with nothing connected deliver the Source straight through
//      (AGENTS.md, D-20 / D-58). The base is *Source itself*, not "the render with the warp
//      switched off": a dissolve against a warp-free render would turn `output = destination` into
//      a render of the *source* whenever a mask is connected, which is not what Nuke's `mix`
//      dissolves do.
//   6. `channels`: a channel that is not selected keeps the Source pixel at that output pixel.
//   7. `inject` writes the raw (not `maskInvert`ed) mask value into alpha - and only an RGBA
//      render has an alpha for it to write, because an alpha-only render's alpha *is* the render
//      (D-58).
//
// Steps 2-4 are the effect; step 5's base and step 6's merge-back are the input. A row is built as
// maximal runs of pixels sharing (coverage, cell) so that the gather can reuse one cell's plan entry
// across the run — the cell is what fixes that input's read window and gather pad — while the dilation
// each pixel gathers with is its own, clamped to that cell (D-87), and so that a run whose sample
// positions *are* the output pixels (D-59) can be read as nearest pixels instead of filtered.
//
// Coordinates (D-60): everything here is in **canonical corner space** - the space OFX regions, the
// grid basis and the grid's own positions are in. An image's rect is half-open: pixel (i,j) covers
// [i,i+1) x [j,j+1) and its centre is (i+0.5, j+0.5). Output pixel (x,y) is queried at
// (x+0.5, y+0.5). Sample positions handed to resample.h's gatherRun are `canonical - 0.5`, because
// that header states "the centre of window pixel X is X" (index space); the conversion happens in
// one place (samplePos()). Nearest reads (the background, step 5's base, step 6's merge-back) index
// the pixel whose cell *contains* the canonical position and read 0 outside the image, following
// common/ofxlab_maskmix.h's MaskReader; gathered reads use resample.h's documented edge extension.

#ifndef OFXLAB_GRIDWRAP_RENDER_H
#define OFXLAB_GRIDWRAP_RENDER_H

#include <cstddef>
#include <vector>

#include "grid.h"
#include "resample.h"

namespace gw {

// ---------------------------------------------------------------------------
// The `output` choice (D-54)
// ---------------------------------------------------------------------------
// Nuke's five names, in Nuke's menu order. Each is a pair (warp, mix), which is why `warp` and
// `mix` are not part of this enum: they are greyed out in the four corner modes and live only in
// `morph`.
enum OutputMode {
    kOutputSource = 0,        // warp 0, mix 0
    kOutputSourceWarped,      // warp 1, mix 0  - Nuke's default, and the pair of knob defaults
    kOutputDestination,       // warp 1, mix 1
    kOutputDestinationWarped, // warp 0, mix 1
    kOutputMorph,             // both knobs live
    kOutputModeCount
};

extern const char* const kOutputModeNames[kOutputModeCount];

bool outputModeFromName(const char* name, OutputMode& out);
OutputMode outputModeOrDefault(const char* name, OutputMode fallback);

// The (warp, mix) a corner mode pins its knobs to. `morph` is not answered here: Options asks the
// knobs (D-54).
bool outputModeCorner(OutputMode m, double& warp, double& mix);

// ---------------------------------------------------------------------------
// The `background` choice
// ---------------------------------------------------------------------------
// Nuke's four, in its menu order. `on src` / `on dst` name which *input* fills the area the grid
// does not reach, which is why they pair with `output`: `output = source` plus `background = on src`
// leaves an uncovered pixel holding the source pixel either way. The four are Nuke's; the *default* is
// not: this plugin defaults to `on black` (D-79), so a fresh node composites onto transparent black -
// the Source pixel wherever its grid reaches, and nothing at all outside that grid's basis.
enum BackgroundMode {
    kBackgroundBlack = 0,  // "on black" - *our* default (D-79); Nuke's is "on src"
    kBackgroundOnSrc,      // "on src"
    kBackgroundOnDst,      // "on dst"
    kBackgroundOnBg,       // "on bg", i.e. the Background input
    kBackgroundModeCount
};

extern const char* const kBackgroundModeNames[kBackgroundModeCount];

bool backgroundModeFromName(const char* name, BackgroundMode& out);
BackgroundMode backgroundModeOrDefault(const char* name, BackgroundMode fallback);

// ---------------------------------------------------------------------------
// The render's knob state
// ---------------------------------------------------------------------------
// Options re-clamps what it is handed: the core's ranges must not depend on the OFX layer having
// remembered to clamp a double a host was free to set to anything (D-53).
struct Options {
    Options()
        : renderRes(kDefaultRenderRes), filter(kDefaultFilter), output(kOutputSourceWarped),
          warp(double(kWarpDefaultNum)), mix(double(kMixDefaultNum)), background(kBackgroundBlack),
          backgroundMix(0.0), fringe(false), inject(false), maskInvert(false), maxStretch(1.0)
    {
        sel[0] = sel[1] = sel[2] = sel[3] = true;   // `channels` defaults to `all`
    }

    int renderRes;
    Filter filter;
    OutputMode output;
    double warp;               // live in `morph` only
    double mix;                // live in `morph` only
    BackgroundMode background;
    double backgroundMix;
    bool sel[4];               // `channels`, in the order r, g, b, a (common/ofxlab_channels.h)
    bool fringe;
    bool inject;
    bool maskInvert;

    // The largest cell stretch of the operative grid (grid.h's cellStretch / planCells), reported
    // for cost reporting and for the cost-ladder test. The render itself uses the per-cell gains
    // from the plan, which are never larger than this, and each pixel's own dilation, which is
    // never larger than its cell's gain (grid.h's pixelGain, D-87).
    double maxStretch;

    void clamp();

    // The (warp, mix) actually used: pinned by `output` except in `morph`.
    double effectiveWarp() const { return output == kOutputMorph ? warp : outputModeWarpOf(output); }
    double effectiveMix() const { return output == kOutputMorph ? mix : outputModeMixOf(output); }

    // Which input fills an uncovered pixel: 0 = nothing (black), 1 = Source, 2 = Dest,
    // 3 = Background. Which input *has* pixels is the windows' business: an absent input reads 0,
    // which is the same value black would give.
    int backgroundInput() const;

private:
    static double outputModeWarpOf(OutputMode m);
    static double outputModeMixOf(OutputMode m);
};

// ---------------------------------------------------------------------------
// A mask the core can read without a host
// ---------------------------------------------------------------------------
class MaskSampler {
public:
    virtual ~MaskSampler() {}

    // Mask value at output pixel (x,y): 0 outside the mask, 1 when no mask is connected.
    virtual double valueAt(int x, int y) const = 0;
};

// No Mask input connected: fully on, everywhere (AGENTS.md: an unconnected mask is fully on).
class AlwaysOnMask : public MaskSampler {
public:
    virtual ~AlwaysOnMask() {}
    virtual double valueAt(int, int) const { return 1.0; }
};

// A flat plane, for the headless tests: `value` inside a rect, 0 outside it.
class PlaneMask : public MaskSampler {
public:
    PlaneMask(const Rect& b, double value) : _b(b), _v(value) {}
    virtual ~PlaneMask() {}
    virtual double valueAt(int x, int y) const
    {
        if (!_b.contains(double(x) + 0.5, double(y) + 0.5)) return 0.0;
        return _v;
    }

private:
    Rect _b;
    double _v;
};

// ---------------------------------------------------------------------------
// The three gatherable inputs, as resample.h's windows
// ---------------------------------------------------------------------------
// A clip the host did not (or could not) supply is an empty SourceWindow, which the core reads as
// zero - the same thing the background path supplies - so "no input connected" and "input with no
// pixels there" mean the same thing to a render.
struct RenderInputs {
    RenderInputs() {}

    SourceWindow src;  // the Source clip
    SourceWindow dst;  // the Dest clip
    SourceWindow bg;   // the Background clip
};

// ---------------------------------------------------------------------------
// The render
// ---------------------------------------------------------------------------
// One WarpRender per render thread (D-55): grid.h's Inverter::find uses scratch candidate storage,
// so an inverter cannot be shared across threads, and the plan and the row buffers are per-thread
// scratch too. The grids and the knob state are read once, at construction.
class WarpRender {
public:
    // How a covered pixel's sample position relates to the output pixel (D-59). Which of these a
    // render *is* follows from (warp, mix) alone: the root of a query in the operative grid lands
    // on the query, so whenever the operative grid *is* the grid an input is sampled through, that
    // input's sample position is the output pixel itself and the read is the nearest pixel - not a
    // gather, which with a non-interpolating filter would blur a pixel that is supposed to arrive
    // unchanged.
    enum CopyKind { kCopyNone = 0, kCopySource, kCopyDest, kCopyBlend };

    WarpRender(const Grid& srcGrid, const Grid& dstGrid, const Options& opt);

    // The grid queries are resolved against: G(warp) when morphing, the destination grid in
    // `destination` / `source warped`, the source grid in `source` / `destination warped`.
    const Grid& operativeGrid() const { return _grid; }
    const Options& options() const { return _opt; }

    // The operative grid's surface bounds unioned with its basis: what the plugin puts on its own
    // RoD. The basis is included because the copy modes write input pixels at their own positions,
    // so a render reaches as far as the rect the grid was resolved against even when the grid is
    // warped entirely inside it (D-60).
    Rect footprint() const;

    // The widest gather pad any input of this render can need, in canonical px: a report, for a cost
    // claim and for a test. What a host has to allocate is the two requests below, which carry this pad
    // *and* the whole-pixel rounding. 0 when nothing is gathered.
    double maxGatherPad() const;

    // The canonical rect the gather of a render window reads for the Source / Dest inputs: the
    // cells that can write inside the window, and each one's own input-side hull inflated by the
    // pad it owes plus one pixel. Cells are selected by their own output box because a cell whose
    // output box is nowhere near the window cannot contribute to it, and a request built from all
    // cells would ask the host for pixels nothing writes. The extra pixel is canonical-rect-to-whole-
    // pixel rounding: a plugin floors/ceils this rect and gets every tap the render reads, which is
    // what tests/gw_render_test.cpp F6 checks pixel by pixel. Equal to the window on a copy path.
    Rect sourceRequest(const Rect& window) const;
    Rect destRequest(const Rect& window) const;

    // What the warped sample of this render is, for the copy paths and for the plugin's isIdentity.
    CopyKind copyKind() const;

    // True when the whole render - covered or not, whatever a mask says - is "the Source image,
    // unchanged, at the same pixels", which is what the plugin reports to a host as an identity
    // render so it can be skipped. `inject` is excluded: it writes something the source does not
    // have. `background_mix` is excluded too, because dissolving a value towards an equal value is
    // not the same *float* as the value: this predicate promises bit-for-bit identity, and that is
    // what a host relies on when it hands back the input image untouched (D-59).
    bool isSourceCopy() const;

    // Which of the four inputs this render can read a *pixel* from, and so which ones the plugin has to
    // ask a host for (D-64). A host allocates an image for every input it is asked for, so asking for an
    // input this render never reads is not free: it is a buffer and a format conversion for pixels that
    // are then thrown away, and in a host that answers two clip requests out of one cached image it is
    // also how a render ends up looking at the wrong input. These four answers are what lets the plugin
    // answer "nothing" for such a clip instead.
    //
    // "Reads" means *some delivered pixel can depend on that input*. A step whose result provably cannot
    // reach the output is not a read: with the mask weight exactly 0 every pixel is dissolved back to the
    // Source base, so the warped sample beneath it - and the background fill an uncovered pixel got - are
    // both thrown away, and B11 measures that replacing those inputs changes not one float. The direction
    // that must never be wrong is the other one: an input a delivered pixel does read has to be asked for,
    // because a core reading a clip the host never allocated reads 0 where a real pixel belonged.
    //
    // All four take `maskConnected`, because that is what makes the mask weight knowable at all. An
    // unconnected mask is fully on (D-20), so its weight is exactly computable - 1, or 0 under
    // `maskInvert`, and 0 under `fringe` too - and that single number answers renderRow's `skipBase`
    // question and every "can this value survive the dissolve" question below. A connected mask can carry
    // any value in [0,1], so only the bounds are knowable, and the predicates take the conservative bound:
    // the Source base is asked for, and the mask itself is read.
    bool readsSource(bool maskConnected) const;
    bool readsDest(bool maskConnected) const;
    bool readsBackground(bool maskConnected) const;
    bool readsMask(bool maskConnected) const;

    // The rect each input must deliver for a render window: `sourceRequest` / `destRequest` when that
    // input is read, the window itself for the two nearest-read inputs, and an empty rect at the
    // window's own corner when this render reads nothing from that input. Empty and non-empty are the
    // only two answers a plugin may give a host for an input, and tests/gw_render_test.cpp B11 checks
    // the empty ones against renders that really do not read that input.
    Rect sourceRoI(const Rect& window, bool maskConnected) const;
    Rect destRoI(const Rect& window, bool maskConnected) const;
    Rect backgroundRoI(const Rect& window, bool maskConnected) const;
    Rect maskRoI(const Rect& window, bool maskConnected) const;

    // Renders output pixels [x1,x2) x [y1,y2) (canonical, half-open) into `out`, which holds
    // (x2-x1) rows of `rowStride` floats, `nchans` interleaved channels per pixel. `nchans` is the
    // *render's* component count (4 or 1); what the inputs have is the windows' business.
    void render(int x1, int y1, int x2, int y2, const RenderInputs& inputs,
                const MaskSampler& mask, float* out, int nchans, int rowStride);

    // One row, the shape an OFX render pass writes from: `out` is the base of that row, `nchans`
    // interleaved channels per pixel. Same contract as render().
    void renderRow(int x1, int x2, int y, const RenderInputs& inputs, const MaskSampler& mask,
                   float* out, int nchans);

    // One cell of the operative grid, with the two input-side nets and the gains their stretches
    // imply. Public because a test reads it to check the run splitting and the RoI requests agree
    // with what the render actually gathers.
    struct CellPlanEntry {
        int r, c;           // the lattice cell this entry is, so a root's (r,c) is a plan index
        Cell srcCell, dstCell;
        Rect outBox;      // where this cell lands in output space (operative grid's hull)
        Rect srcBox;      // where sampling the Source input reaches (the source cell's hull)
        Rect dstBox;      // ... and the Dest input's
        double srcGain, dstGain;   // max(1, stretch): what the filter's radius is multiplied by
        int padSrc, padDst;        // gatherPad(filter, gain): the whole-pixel reach those gains owe
        bool used;        // false when one of the three nets is missing, so it is never selected
    };

    const std::vector<CellPlanEntry>& plan() const { return _plan; }

    // The union of the plan's cell output boxes: where the gather can write. This is grid.h's
    // gridFootprint(operative grid) measured cell by cell, and it is what the two RoI requests
    // select cells against; a test checks the two bounds agree.
    Rect planBounds() const { return _planBounds; }

    // The forward-mapped lattice's cell count, for the cost claim in ARCHITECTURE.md's filter note
    // and for a test that wants to know how many cells the plan is answering.
    std::size_t inverterCellCount() const { return _inv.cellCount(); }

    // The inversion lattice itself, so a test (and a host's RoI report) can read the two numbers that
    // decide coverage - the residual a root has to fall under, and the lattice's bucket size - from the
    // *same* Inverter the render queries, instead of rebuilding one and hoping it agrees.
    const Inverter& inverter() const { return _inv; }

    // Sample positions in resample.h's index space, from canonical ones (D-60).
    static double samplePos(double canonical) { return canonical - 0.5; }

    // The logical r/g/b/a index of output channel `k` of an `nchans`-channel render: an RGBA render
    // has r,g,b,a in order; an alpha-only render's single channel *is* the alpha channel.
    static int logicalChannel(int k, int nchans);

    // The output channel `inject` writes into, or -1 when the render has no such channel.
    static int alphaIndex(int nchans);

private:
    void buildPlan();

    // The interval renderRow's mask weight can take for this knob state (see the note on `readsSource`).
    struct WeightRange {
        double lo, hi;
        WeightRange(double lo_, double hi_) : lo(lo_), hi(hi_) {}
    };
    WeightRange maskWeightBounds(bool maskConnected) const;

    // The three reachability questions the input predicates are built from: can a pixel skip reading its
    // Source base (renderRow's `skipBase`), can the warped sample reach a delivered channel, and can what
    // the `background` choice fills in reach one too. See the note above `readsSource`.
    bool canSkipBase(bool maskConnected) const;
    bool canWriteEffect(bool maskConnected) const;
    bool canReachBackground(bool maskConnected) const;
    bool anyChannelDelivered() const;

    // A rect with no extent at `w`'s own corner: the answer for an input nothing reads (the same shape
    // `sourceRequest` gives an empty window, and for the same reason - see the note there about a host
    // that floors and ceils a swapped pair into a real rectangle on the far side of the frame).
    static Rect emptyAt(const Rect& w);

    // Reads the input pixel whose cell contains the canonical position (x + 0.5, y + 0.5), where
    // (x,y) is an *output* pixel index: 0 outside the window's bounds.
    static void nearestRead(const SourceWindow& in, int x, int y, float* out, int nchans);

    static void ensureFloats(std::vector<float>& v, std::size_t n);
    static void ensureDoubles(std::vector<double>& v, std::size_t n);
    static void ensureInts(std::vector<int>& v, std::size_t n);
    static void ensureVecs(std::vector<Vec>& v, std::size_t n);

    Grid _srcGrid, _dstGrid, _grid;
    Options _opt;
    Inverter _inv;
    bool _sameGrids;
    bool _srcCopy, _dstCopy;      // this input's sample position *is* the output pixel (D-59)
    bool _allSel;                 // every `channels` flag is on, so step 6 never fires
    bool _anySel;                 // at least one is on, so some channel can hold the warped sample
    bool _identity;               // cached isSourceCopy()

    std::vector<CellPlanEntry> _plan;
    Rect _planBounds;                 // union of the cells' output boxes
    double _padSrc, _padDst;          // 0 when that input is never gathered
    int _cellCols;

    // Row scratch, reused across rows and runs.
    std::vector<double> _sx, _sy;     // gather sample positions, index space
    std::vector<double> _hu, _hv;     // the root's parameters, per output pixel
    std::vector<double> _gx, _gy;     // per-pixel gather dilation, clamped to the cell's plan gain (D-87)
    std::vector<Vec> _odu, _odv;      // the operative net's partials at that root, which is what the
                                      // per-pixel gain divides out; the *input* net's partials are a
                                      // gather loop's locals, because that loop already evaluates them
    std::vector<float> _a, _b, _eff;  // the two sampled inputs and the composited effect
    std::vector<float> _bgPix, _basePix;   // one pixel of the background, and of the Source base
    std::vector<int> _cell;           // plan index per output pixel, -1 where uncovered
    Hit _hit;                         // one query's answer
};

} // namespace gw

#endif // OFXLAB_GRIDWRAP_RENDER_H
