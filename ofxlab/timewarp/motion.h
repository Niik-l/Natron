// TimeWarp Motion method: bidirectional, occlusion-weighted synthesis of the
// frame at a fractional source time (DECISIONS.md D-25).
//
// OFX-free for the same reason as flow.h: it is compiled into the plugin *and*
// measured offline against an analytic ground truth, where the synthesized frame
// can be compared with the scene sampled at the target time. Lives in
// plugins/timewarp/ because only this plugin uses it (D-06).
//
// Frames: A at time t0, B at time t1 > t0, target t = t0 + beta (t1 - t0), so
// beta = 0 is A and beta = 1 is B. The flow convention is flow.h's - a field is
// defined on the frame it was estimated from and points into the other one -
// applied in both directions, each direction estimated on its own rather than
// inverted out of the other. Each field has an x and a y component:
//
//     fwd:  B(x + fwd(x)) ~= A(x)   estimated on A; A's content at x is at x+fwd in B
//     bwd:  A(y + bwd(y)) ~= B(y)   estimated on B; B's content at y is at y+bwd in A
//
// The target frame is gathered from both sides, both as backward warps (MotionField
// component names, and (x,y) abbreviated to x):
//
//     C_A(x) = A(x + beta     * bwd(x))   ==  A(x) itself at beta = 0
//     C_B(x) = B(x + (1-beta) * fwd(x))   ==  B(x) itself at beta = 1
//
// which is what the two directions are for: at either end one candidate *is* the
// frame, and in between each side contributes the content it can see. Estimating
// both directions is the point - a single direction has no way to tell which side
// it is wrong on, and it is the wrong side that shows up as a ghost.
#ifndef OFXLAB_TIMEWARP_MOTION_H
#define OFXLAB_TIMEWARP_MOTION_H

#include "flow.h"

#include <cstddef>
#include <vector>

namespace tw {

/** @brief Read-only float plane addressed by absolute scanline number, so an OFX
 * image (addressed per scanline) and a test vector are read through the same code.
 * Pixel bounds are the half-open rect [x1,x2) x [y1,y2). */
struct Plane {
  int x1, y1, x2, y2; // delivered pixel bounds; empty when x2 <= x1 or y2 <= y1
  int comp;           // interleaved floats per pixel (1 for a single-channel plane)

  Plane() : x1(0), y1(0), x2(0), y2(0), comp(1) {}
  virtual ~Plane() {}

  /** @brief Pointer to pixel (x1, @p y) - the start of that scanline - or 0 when the
   * scanline is not delivered. */
  virtual const float *row(int y) const = 0;

  bool empty() const { return x2 <= x1 || y2 <= y1; }
  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
};

/** @brief Plane over a contiguous buffer, used by the offline suites. */
struct VectorPlane : public Plane {
  const std::vector<float> *data;

  VectorPlane() : data(0) {}
  void set(const std::vector<float> &d, int w, int h, int comp_)
  {
    data = &d;
    x1 = y1 = 0;
    x2 = w;
    y2 = h;
    comp = comp_;
  }
  virtual const float *row(int y) const
  {
    if (!data || y < y1 || y >= y2)
      return 0;
    return data->data() + (size_t)(y - y1) * (size_t)width() * (size_t)comp;
  }
};

/** @brief Write counterpart of Plane, same addressing. */
struct OutputPlane {
  int x1, y1, x2, y2;
  int comp;

  OutputPlane() : x1(0), y1(0), x2(0), y2(0), comp(1) {}
  virtual ~OutputPlane() {}

  /** @brief Pointer to pixel (x1, @p y), or 0 when the scanline is not delivered. */
  virtual float *row(int y) = 0;

  bool empty() const { return x2 <= x1 || y2 <= y1; }
  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
};

/** @brief OutputPlane over a contiguous buffer. */
struct VectorOutPlane : public OutputPlane {
  std::vector<float> *data;

  VectorOutPlane() : data(0) {}
  void set(std::vector<float> &d, int w, int h, int comp_)
  {
    data = &d;
    x1 = y1 = 0;
    x2 = w;
    y2 = h;
    comp = comp_;
  }
  virtual float *row(int y)
  {
    if (!data || y < y1 || y >= y2)
      return 0;
    return data->data() + (size_t)(y - y1) * (size_t)width() * (size_t)comp;
  }
};

/** @brief Catmull-Rom sample of a plane at absolute coordinates. Taps are clamped to
 * the plane's bounds, which is the same boundary rule sampleCubic uses (neither
 * wrapping nor extrapolating), and scanlines the plane did not deliver contribute 0.
 * Returns false only when the plane holds no pixels at all. */
bool samplePlaneCubic(const Plane &p, double x, double y, int c, double *out);

/** @brief Same gather for every component at once, written to @p out[@p c]. The tap
 * geometry is computed once per pixel instead of once per component, which is what a
 * render loop wants when it warps all four channels of a pixel. */
void samplePlaneCubicAll(const Plane &p, double x, double y, double *out);

/** @brief Motion data for one bracketing pair of frames: both flow directions plus
 * how much each frame's own content can be trusted when gathering the other side.
 *
 * The grid is the grid of the pair's frames, which is also the grid of the
 * synthesized frame; @p x1 / @p y1 say where that grid sits in absolute pixel
 * coordinates, so a field estimated over a whole frame can be used to fill a tile.
 *
 * occA / occB are quantised occlusion indicators of each frame's *own* content: 0
 * means the content is found on the other side, 255 means it is not present there at
 * all - so gathering that side for a target time near the other end is exactly what
 * produces a ghost. See MotionOptions.
 */
struct MotionField {
  int x1, y1; // absolute position of grid pixel (0,0)
  int w, h;
  std::vector<float> fwdU, fwdV; // estimated on A: B(x+fwd) ~= A(x)
  std::vector<float> bwdU, bwdV; // estimated on B: A(y+bwd) ~= B(y)
  std::vector<unsigned char> occA, occB;

  MotionField() : x1(0), y1(0), w(0), h(0) {}

  void reset(int w_, int h_, int ox_ = 0, int oy_ = 0)
  {
    x1 = ox_;
    y1 = oy_;
    w = w_;
    h = h_;
    const size_t n = (size_t)w * h;
    fwdU.assign(n, 0.0f);
    fwdV.assign(n, 0.0f);
    bwdU.assign(n, 0.0f);
    bwdV.assign(n, 0.0f);
    occA.assign(n, 0);
    occB.assign(n, 0);
  }
  void clear()
  {
    x1 = y1 = w = h = 0;
    fwdU.clear();
    fwdV.clear();
    bwdU.clear();
    bwdV.clear();
    occA.clear();
    occB.clear();
  }
  bool empty() const { return w <= 0 || h <= 0 || fwdU.empty(); }
  size_t size() const { return fwdU.size(); }

  /** @brief Bytes held by one field, used to bound the plugin's flow cache. */
  size_t bytes() const;

  bool contains(int x, int y) const
  {
    return x >= x1 && y >= y1 && x < x1 + w && y < y1 + h;
  }

  bool finite() const; // debug aid: no NaN/inf leaked into a render
};

/** @brief Tuning of the occlusion weighting, separate from the estimator's own knobs.
 *
 * The trust test is the pair's own consistency residual: how far a pixel's brightness
 * is from what the other frame has at the place the flow claims it moved to. Scale
 * relative to the pair's mean residual, so the same defaults behave the same on a
 * clean plate, on grain, and in HDR. A residual below @p residualScale times the mean
 * is trusted completely, past @p residualSpan times it is treated as fully occluded,
 * and in between the weight ramps - which matters more than the endpoints, because a
 * hard threshold shows up as a hard seam in the render.
 *
 * @p weightSmooth averages the indicator over a box of that radius in pixels before it
 * is used. Occlusion borders follow the flow field, which is ragged there, and the
 * average is what stops the blend from speckling along them.
 *
 * @p minWeight is a floor on each candidate's weight *before* normalisation: a side is
 * never discarded entirely, which keeps the render continuous as beta crosses a
 * region's occlusion threshold instead of snapping from one side to the other.
 *
 * The defaults are the sweep in the accuracy suite, and the sweep's first result is that
 * these four knobs are flat - to four decimal places of MAE - in the *decidable* regions
 * of a two-layer composite: where both candidates are accurate, the weights cannot change
 * the answer. What they do change is (a) how much of the grid the indicator claims is
 * occluded and (b) the ambiguous band. scale 2.0 / span 10.0 detects 0.018 / 0.013 of the
 * grid, which matches the geometric occlusion area of that composite (~1.4%); span 4.0
 * over-detects sevenfold (0.109 / 0.090) and span 20.0 detects nothing (0.000), and
 * scale 6.0 under-detects (0.005). weightSmooth 2 is where the indicator stops speckling
 * without erasing the detection (fraction 0.034 -> 0.018 from 0 -> 2, band MAE 0.01401 ->
 * 0.01393); 4 is marginally cleaner in the band but halves the detection again. minWeight
 * is not an accuracy knob: 0.0, 0.05 and 0.5 differ by under 0.00004 MAE, and beta is
 * continuous with the floor at 0 too, so it stays as the guard its name describes.
 *
 * What the weighting is worth, measured along beta (battery D of the same suite): in the
 * decidable regions it is neutral - object interior ratio exactly 1.000, background 1.007
 * at half way - and its entire effect is near the ends, where it improves background MAE
 * by 10-13% and band MAE by 11-17%, improving 10 of 14 sampled points with none worse than
 * 1%. Picking the correct side perfectly would do better (band MAE 0.0037-0.0045 near the
 * ends against 0.0085-0.0093 here), which is the honest limit of a trust measure derived
 * from a consistency residual: that residual saturates only in the minority of pixels that
 * are genuinely occluded, and the dominant error in the band is the estimator's own flow
 * error at an occlusion border, which no weighting can undo.
 */
struct MotionOptions {
  double residualScale;
  double residualSpan;
  int weightSmooth;
  double minWeight;

  MotionOptions()
    : residualScale(2.0), residualSpan(10.0), weightSmooth(2), minWeight(0.05) {}

  /** @brief Field-by-field identity, the same rule as flow.h's @ref sameFlowOptions: a
   * weighting change changes the indicators stored in a field, so a field estimated under
   * one weighting is not a field estimated under another. */
  bool sameAs(const MotionOptions &o) const
  {
    return residualScale == o.residualScale && residualSpan == o.residualSpan
        && weightSmooth == o.weightSmooth && minWeight == o.minWeight;
  }
};

/** @brief Everything that decides what one Motion estimate is: the estimator's options,
 * the weighting of its residuals, and how much the estimation grid is reduced.
 *
 * The plugin resolves its Motion knobs into one of these per render, and the flow cache
 * compares them field by field - a cached field is a function of its two frames' pixels
 * *and* of the knobs that produced it, so a knob change makes every cached field a miss
 * rather than something to invalidate (D-33). */
struct MotionTuning {
  FlowOptions flow;
  MotionOptions motion;
  int gridSteps; // extra halvings on top of kMaxFlowPixels' own (flowGridStepsForQuality)

  MotionTuning() : gridSteps(0) {}

  bool sameAs(const MotionTuning &o) const
  {
    return gridSteps == o.gridSteps && motion.sameAs(o.motion) && sameFlowOptions(flow, o.flow);
  }
};

/** @brief What solve() did, for the accuracy suite and a host's readout. */
struct MotionDiagnostics {
  double residualA, residualB;   // mean consistency residual of each direction
  double meanResidual;           // the scale the indicators are relative to
  double occFractionA, occFractionB; // share of the grid past half occlusion
  double maxFlow;                // largest single displacement component, both fields
  double meanFlow;               // mean |fwd| over the grid: 0 on a static pair
  int levelsUsed;                // deeper of the two estimates' pyramids
  int sweeps;                    // Gauss-Seidel sweeps over both estimates
  bool usable;                   // false when an estimate went non-finite: cross-fade

  MotionDiagnostics()
    : residualA(0), residualB(0), meanResidual(0), occFractionA(0), occFractionB(0)
    , maxFlow(0), meanFlow(0), levelsUsed(0), sweeps(0), usable(true) {}
};

/** @brief Estimates a MotionField for one bracketing pair of frames.
 *
 * Single-channel frames (luma), row-major, w*h each. The plugin builds them from the
 * source images; see TimeWarpPlugin::buildGray.
 */
class MotionSolver {
public:
  MotionSolver(const MotionOptions &mo = MotionOptions(), const FlowOptions &fo = FlowOptions())
    : _mo(mo), _est(fo), _rows(0) {}

  /** @brief The same solver from the plugin's resolved knob set (D-33). */
  explicit MotionSolver(const MotionTuning &tuning)
    : _mo(tuning.motion), _est(tuning.flow), _rows(0) {}

  /** @brief Both flow directions plus the occlusion indicators of the pair. */
  void solve(const float *a, const float *b, int w, int h, MotionField &out,
             MotionDiagnostics *diag = 0) const;

  /** @brief Row band scheduler of D-31, installed on this solver's own passes and on
   * the flow estimator it wraps. 0 means "on the calling thread", which is what the
   * offline suites use. */
  void setRowScheduler(const RowScheduler *rows)
  {
    _rows = rows;
    _est.setRowScheduler(rows);
  }
  const RowScheduler *rowScheduler() const { return _rows; }

  const MotionOptions &options() const { return _mo; }
  MotionOptions &options() { return _mo; }
  const FlowOptions &flowOptions() const { return _est.options(); }
  FlowOptions &flowOptions() { return _est.options(); }

private:
  MotionOptions _mo;
  TVL1Estimator _est;
  const RowScheduler *_rows;
};

////////////////////////////////////////////////////////////////////////////////
// reduced-resolution estimation guard
//
// A TV-L1 estimate holds a pyramid of several float arrays per level, so both the
// transient memory and the time of one estimate grow with the frame's pixel count:
// a 4K pair costs hundreds of MB and tens of seconds, which is a host-crashing
// amount of memory for a plugin that has no say over the host's budget. Capping the
// grid bounds both. The cap is 2.5 MP, which halves a 4K frame exactly once
// (3840x2160 -> 1920x1080, 4096x2160 -> 2048x1080), the reduction Kronos-class hosts
// apply to their flow grid, and only the *flow field* is measured coarser: the warping
// still gathers from the full-resolution frames with the full-resolution cubic spline.
// Measured, accuracy suite battery E and D-25: one step is 4x faster and costs under
// 2x the object-interior MAE, with the ambiguous band almost untouched; what it really
// costs is occlusion detection, which box-reducing the frames blunts.

const int kMaxFlowPixels = 2500 * 1000;

/** @brief Halvings an extra request may add, whatever a knob says: past four, a grid is
 * under 1/256th of the frame's area and there is nothing left to estimate. It is also the
 * upper bound the plugin's grid knob offers, so the knob cannot ask for something the guard
 * would then have to refuse. */
const int kMaxExtraGridSteps = 4;

/** @brief Extra halvings a quality preset wants on top of the ones @ref kMaxFlowPixels
 * already asks for.
 *
 * Only the preview rung pays for them, and only on a frame big enough for a halving to mean
 * something: a preset is supposed to be *cheaper*, and the flow suite (D-33) measured that
 * `iters` is the only knob inside flow.h that moves the clock - every other knob buys
 * accuracy at flat time. The estimation grid is the other dial there is.
 *
 * What a halving is allowed to cost is measured twice, in battery E2 and E3 of the Motion
 * suite. E2 (grid alone, default sweeps): 4.0x faster, background MAE under 0.010, and the
 * object interior within 1.7x - so the grid on its own is nearly free. E3 (the preview rung
 * as the plugin runs it: the coarser grid *and* the rung's fewer sweeps): 9.5x faster than
 * Standard, 3.8x of it from the grid, and the worst object-interior MAE 0.0091 of full luma
 * range against Standard's 0.0010 on the same counter-moving composite. That ratio is large
 * and the absolute number is not: 0.9% of luma range inside a moving object is what a preview
 * is for. Every rung meant to be *accurate* keeps the cap's own single step and spends sweeps. */
int flowGridStepsForQuality(int quality);

/** @brief Bytes a field over a @p w x @p h grid holds: the two flow directions (two
 * float components each) and the two occlusion indicators, plus the struct's own
 * bookkeeping. @p w and @p h are the *estimation* grid, so a capped field is cheaper
 * than a full-resolution one - which is why the plugin's memory cap turns bytes into
 * a number of pairs. */
size_t motionFieldBytes(int w, int h);

/** @brief How many halvings of a w*h grid the estimate should run on, with @p rw / @p rh
 * reporting that grid. Halved dims are rounded up, so a reduced grid always covers the whole
 * frame.
 *
 * The count is the cap's own halvings - 0 when the frame is already under @ref
 * kMaxFlowPixels - plus @p extraSteps, which is how a quality preset (see @ref
 * flowGridStepsForQuality) or a host's advanced knob asks for a coarser grid; it is clamped
 * to @ref kMaxExtraGridSteps and negative requests count as zero. It terminates on any
 * aspect: a 2x4000000 frame halves its long axis until it is 1x2000000, and stops there
 * rather than collapsing the short axis to nothing. */
int flowReduceSteps(int w, int h, int *rw = 0, int *rh = 0, int extraSteps = 0);

/** @brief Box reduction of @p src onto a @p gw x @p gh grid laid over the *explicit*
 * rect [x1,x2) x [y1,y2), @p src.comp interleaved floats per cell.
 *
 * The rect is the point of this function: a pair's two frames are only comparable if
 * their coarse grids land on the same pieces of the image, and two images fetched from
 * a host need not have the same bounds. Cell (i,j) therefore partitions the requested
 * rect at @c x1 + floor(i*(x2-x1)/gw), and each plane is read at those absolute
 * coordinates: columns the rect reaches past the plane's own bounds fall back on its
 * nearest delivered column, so a coarse frame never grows a black band the full
 * resolution frame does not have - the estimator would read that band as motion.
 * Scanlines the plane did not deliver are skipped rather than guessed at, and a cell's
 * mean is taken over the pixels it actually saw; a cell that saw nothing stays 0.
 *
 * Reducing is a filter and not a decimation: aliasing in the luma frames is aliasing in
 * the flow measured from them. */
void reduceRectTo(const Plane &src, int x1, int y1, int x2, int y2, int gw, int gh,
                  std::vector<float> &dst);

/** @brief reduceRectTo over @p src's own delivered rect. */
void reducePlaneTo(const Plane &src, int gw, int gh, std::vector<float> &dst);

/** @brief Resample a MotionField onto a @p w x @p h grid, multiplying every
 * displacement by @p scale. Follows resampleFlow's cell-centre convention (which it
 * uses for the two flow directions), so @p scale is @c w/src.w == h/src.h, and the
 * destination keeps the source's absolute origin. The occlusion indicators are
 * resampled bilinearly and unscaled: they are weights, and the overshoot a cubic
 * spline takes on a step in a weight would produce a negative weight.
 * @p rows is the band scheduler of D-31, 0 for "on the calling thread". */
void resampleMotionField(const MotionField &src, MotionField &dst, int w, int h, double scale,
                         const RowScheduler *rows = 0);

/** @brief Where the two candidates gather from and how much each is trusted, for the
 * target pixel at absolute grid position (@p x, @p y).
 *
 * Returns false when the position is outside the field's grid, in which case the
 * caller should cross-fade the two frames directly instead. Gather positions are
 * absolute and may fall outside the grid, which is the caller's business:
 * samplePlaneCubic clamps. wA + wB always equals 1.
 */
bool motionCandidates(const MotionField &f, int x, int y, double beta, const MotionOptions &opt,
                      double *ax, double *ay, double *bx, double *by, double *wA, double *wB);

/** @brief Synthesize the frame at fraction @p beta over the rect
 * [lx1,lx2) x [ly1,ly2). The planes and @p dst carry the same number of interleaved
 * components, which are all warped (alpha included); the flow was estimated on luma.
 * Loop rect outside the field's grid is filled with a cross-fade, and a field that is
 * empty behaves as the Blend method's dissolve. */
void motionBlendPlane(const MotionField &f, double beta, const Plane &a, const Plane &b,
                      OutputPlane &dst, int lx1, int ly1, int lx2, int ly2,
                      const MotionOptions &opt);

/** @brief Plain temporal cross-fade over the same rect: what the Blend method does
 * between two frames. The baseline Motion has to beat, and the fallback when an
 * estimate is unusable. */
void crossFadePlane(const Plane &a, const Plane &b, double beta, OutputPlane &dst, int lx1,
                    int ly1, int lx2, int ly2);

} // namespace tw

#endif // OFXLAB_TIMEWARP_MOTION_H
