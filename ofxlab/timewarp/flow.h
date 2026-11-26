// TimeWarp Motion method: dense optical flow (DECISIONS.md D-24).
//
// Deliberately free of OFX headers: the estimator is pure numerical code, so it
// is compiled into the plugin *and* exercised offline against analytic ground
// truth (see the EPE suite in the test harness). Lives in plugins/timewarp/
// rather than common/ because only one plugin uses it (D-06).
//
// Convention used everywhere in TimeWarp (D-25): a flow field is defined on the
// frame it was estimated from and points INTO the other one,
//
//     B(x + u(x,y), y + v(x,y))  ~=  A(x,y)      (field (u,v) estimated on A)
//
// so A's content at x is *found at* x + u in B. Synthesising the frame at fraction
// beta from A towards B (beta = 0 is A, beta = 1 is B) is therefore a backward warp
// of B by the *remaining* fraction: sample B at (x + (1-beta)*u, y + (1-beta)*v),
// which is B itself at beta = 1 and equals A(x) at beta = 0 by the definition above.
// Occlusions are handled by estimating both directions and weighting them against
// each other; see motion.h, which is where this convention is consumed.

#ifndef OFXLAB_TIMEWARP_FLOW_H
#define OFXLAB_TIMEWARP_FLOW_H

#include <cstddef>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace tw {

/** @brief Dense displacement field, one (u,v) pair per pixel of frame A. */
struct FlowField {
  int w, h;
  std::vector<float> u, v; // row-major, w*h each

  FlowField() : w(0), h(0) {}
  FlowField(int w_, int h_) : w(w_), h(h_), u((size_t)w_ * h_, 0.0f), v((size_t)w_ * h_, 0.0f) {}

  void reset(int w_, int h_)
  {
    w = w_;
    h = h_;
    u.assign((size_t)w * h, 0.0f);
    v.assign((size_t)w * h, 0.0f);
  }
  void clear()
  {
    w = h = 0;
    u.clear();
    v.clear();
  }
  bool empty() const { return w <= 0 || h <= 0 || u.empty(); }
  std::size_t size() const { return u.size(); }
  bool finite() const; // debug aid: no NaN/inf leaked into a render
};

/** @brief Row bands the solver splits its passes into, and @p bands is a function of
 * @p h @b only (DECISIONS.md D-31).
 *
 * That restriction is the whole point. A pass split into bands writes the pixels of its
 * own band and nothing else, and a band's part of any reduction goes into its own slot,
 * which is then totalled in band order. So the numbers a parallel pass produces do not
 * depend on how many threads ran it: 1 thread and 32 threads give bit-identical flow.
 * Had the split followed the host's CPU count instead, the same clip would render
 * slightly differently on two machines - which for a retimer means a flow field cached
 * under one thread count would not survive the host changing it.
 *
 * Red black Gauss-Seidel is what makes this exact rather than approximate: the four
 * neighbours of a pixel of colour c all have colour 1-c, so no update within a colour
 * pass can feed another one (see LevelSolver::sweep), and a colour pass may therefore be
 * cut at any row boundary without changing a single value.
 */
struct BandPlan {
  static const int kMinRows = 16; // bands smaller than this do not pay for a dispatch
  static const int kMaxBands = 64; // bounds the per-band reduction slots a pass keeps

  int h;
  int bands;

  /** @brief Empty plan: one band, no rows. */
  BandPlan() : h(0), bands(1) {}

  explicit BandPlan(int h_, int maxBands = kMaxBands)
    : h(h_ < 0 ? 0 : h_)
    , bands(h < kMinRows ? 1 : std::min(h / kMinRows, maxBands < 1 ? 1 : maxBands))
  {
  }

  /** @brief First row of @p band, bands partition [0,h) exactly (no gap, no overlap). */
  int y0(int band) const { return (int)((long long)h * band / bands); }
  int y1(int band) const { return (int)((long long)h * (band + 1) / bands); }
};

/** @brief Where the estimator runs its row bands.
 *
 * The estimator itself is single-threaded code with a hook: it hands each band to
 * @ref run and expects it to be finished when @ref run returns, which is why no pass
 * here needs a lock - bands are disjoint in what they write, and every value a band
 * shares with the others is a per-band slot summed afterwards in band order. The plugin
 * installs one backed by the host's OFX multi-thread suite (a plugin may not spawn its
 * own threads, D-03); the offline suites use the serial one below.
 */
class RowScheduler {
public:
  virtual ~RowScheduler() {}

  /** @brief Call @p band once for every band index of @p plan, all of them done on return. */
  virtual void run(const BandPlan &plan, const std::function<void(int)> &band) const = 0;
};

/** @brief The default scheduler: bands in index order, on the calling thread. */
class SerialRowScheduler : public RowScheduler {
public:
  virtual void run(const BandPlan &plan, const std::function<void(int)> &band) const
  {
    for (int b = 0; b < plan.bands; ++b)
      band(b);
  }
};

/** @brief The one shared serial scheduler, used when none was installed. */
const RowScheduler &serialRowScheduler();

/** @brief Tuning knobs of the estimator.
 *
 * The defaults are what the tuning harness settled on (notes in DECISIONS.md
 * D-24). A deep pyramid costs a small fraction of the finest level and is what
 * carries large motion, so @p levels is set generously and @p minDim is the knob
 * that really stops the recursion: depth adapts to resolution instead of being
 * fixed per clip.
 */
struct FlowOptions {
  int levels;         // pyramid levels; 1 disables the pyramid. Raise with motion size
  int coarseIters;    // sweeps budgeted to the coarsest levels
  int iters;          // sweeps budgeted to every finer level
  int relinSweeps;    // sweeps between re-warps (Gauss-Newton re-linearisations)
  double lambda;      // data weight relative to smoothing, in units of "one mean
                      // image gradient" (see LevelSolver::dataTerm): larger follows
                      // the image closer. Dimensionless, so it transfers between
                      // 0..1 clips and HDR
  double eps;         // relaxation of the L1 terms (data and TV), same units as lambda
  double maxMotion;   // clamp of |u|,|v| in pixels: guards against runaway flow
  double gaussSigma;  // pyramid pre-blur sigma in pixels of the level being built
  double tauTV;       // explicit step of the smoothness flow (capped weights keep it contractive)
  bool useTV;         // true: TV (L1 gradient) penalty, false: Tikhonov (Horn-Schunck)
  bool useL1Data;     // true: L1 data term (occlusion tolerant), false: L2
  bool warpDataTerm;  // true: gradients of the *warped* frame (Gauss-Newton scheme)
  bool cubicWarp;     // true: Catmull-Rom warps, which keep the sub-pixel detail
                      // bilinear attenuates (see cubicWeights)
  bool medianPost;    // 3x3 median filter of the result: kills speckle at edges
  int minDim;         // stop refining below this size (per-axis pixel count)

  /** @brief How much of the data term each pixel gets, see LevelSolver::dataTerm.
   *
   * 0 leaves the data term uniform. > 0 weights it by @c |grad bw|^2 averaged over
   * the level, so pixels whose brightness carries no information about where they
   * came from - sky, out-of-focus backgrounds, film grain - are left to the
   * smoothness term instead of being given a data term that only ever asks them to
   * follow the noise. The weight is mean-normalised, so it redistributes lambda
   * rather than scaling it. @p dataGradMin is a floor on it, which matters only as a
   * guard: measured, any floor from 0.02 to 0.10 gives bit-comparable results, and
   * 0.3 (half the frame pinned at the floor) starts to cost accuracy.
   *
   * @p dataGradBlur reads that gradient from a Gaussian-blurred copy of the warped
   * frame, at a fixed scale in pixels of the *finest* frame, so grain is not counted
   * as structure. It is the part that matters: on a grainy clip it cuts the flow
   * jitter over flat regions by 40% and improves EPE, and it costs nothing on a clean
   * one. Too much of it (2 px and up) starts blurring real detail away and costs
   * large motion and motion boundaries.
   */
  double dataGradMin;
  double dataGradBlur;

  FlowOptions()
    : levels(8), coarseIters(1000), iters(120), relinSweeps(64), lambda(0.05), eps(0.01)
    , maxMotion(64.0), gaussSigma(0.75), tauTV(0.15), useTV(true), useL1Data(true)
    , warpDataTerm(true), cubicWarp(true), medianPost(true), minDim(6)
    , dataGradMin(0.05), dataGradBlur(1.0) {}
};

/** @brief The Motion quality presets (DECISIONS.md D-33).
 *
 * A preset is a point on the cost/accuracy curve the flow suite measures, not a vague
 * label: the table behind @ref flowOptionsForQuality is printed by
 * `./tests/run.sh sweep`, and the accuracy suite asserts that each step up spends a
 * larger sweep budget and buys equal-or-better end-point error on the content that is
 * decidable from the two frames. @ref eQualityStandard *is* @ref FlowOptions' default, so
 * a plugin that never mentions quality behaves exactly as before.
 *
 * @ref eQualityCustom is the one preset that decides nothing: it exists so a host's UI
 * can offer the per-solver knobs without the plugin ever writing into them.
 */
enum Quality {
  eQualityDraft = 0,    // short sweep budget (plus a coarser estimation grid in the plugin)
  eQualityStandard = 1, // the measured defaults (D-24)
  eQualityHigh = 2,     // twice the sweeps, same grid and same data weight
  eQualityExtreme = 3,  // more sweeps and a stronger data term: the rung that converges
                        // counter-moving regions, at the cost of large-motion accuracy (D-24)
  eQualityCustom = 4,   // the advanced knobs decide, the preset decides nothing
  kNumQualities = 5
};

/** @brief The ranges a host may be offered for the two estimator dials @ref eQualityCustom
 * exposes, taken from the sweep table: `iters` 10..960 runs from below the smallest budget
 * measured (30) to twice the largest (480), and `lambda` 0.01..1 brackets the 0.02..0.40 the
 * table covers - where each of them pays is on the knob's own help text.
 *
 * These are the descriptor's range *and* the plugin's clamp, in one place, so a knob cannot
 * describe one range and then accept another. */
const int kFlowItersMin = 10, kFlowItersMax = 960;
const double kFlowLambdaMin = 0.01, kFlowLambdaMax = 1.0;

/** @brief The estimator options a preset selects. Out-of-range presets give Standard. */
FlowOptions flowOptionsForQuality(int quality);

/** @brief Field-by-field identity of two solver configurations.
 *
 * Two uses, both deliberate: the accuracy suite asserts what a preset is *allowed* to
 * change (see the preset checks), and the plugin's flow cache refuses to hand a field to a
 * render configured differently - a cached field is a function of the pixels *and* of the
 * knobs that produced it. No hashing and no rounding: any difference in any field is a
 * different field. */
bool sameFlowOptions(const FlowOptions &a, const FlowOptions &b);

/** @brief Solver statistics: what the accuracy suite asserts and what a host
 * could show in a progress readout. */
struct FlowDiagnostics {
  int levelsUsed;     // pyramid levels actually built
  int iterations;     // Gauss-Seidel sweeps actually run, summed over the levels
  double energyStart; // energy of the finest level at its first re-linearisation
  double energyEnd;   // ... and at its last one (they bound the refinement step)
  double maxUpdate;   // biggest single-pixel increment change in the last sweep pair
  double residualMean; // mean |B(x+w) - A(x)| of the finest level's final flow
  double timeMs;      // filled by the caller that times the estimate

  FlowDiagnostics()
    : levelsUsed(0), iterations(0), energyStart(0.0), energyEnd(0.0)
    , maxUpdate(0.0), residualMean(0.0), timeMs(0.0) {}
};

/** @brief Interface the processor talks to, so the estimator can be replaced. */
class MotionEstimator {
public:
  explicit MotionEstimator(const FlowOptions &opt) : _opt(opt), _rows(0) {}
  virtual ~MotionEstimator() {}

  /** @brief Estimate the displacement mapping frame @p a into frame @p b.
   *
   * Both frames are single-channel, row-major, w*h floats (the estimator works
   * on luma; the processor warps the actual RGBA planes with the result).
   */
  virtual void estimate(const float *a, const float *b, int w, int h, FlowField &out) const = 0;

  virtual const char *name() const = 0;

  const FlowOptions &options() const { return _opt; }
  FlowOptions &options() { return _opt; }

  /** @brief Install the row-band scheduler every estimate will use (D-31).
   *
   * Not owned, and it must outlive the estimates: the plugin's scheduler lives in the
   * processor that owns this estimator. 0 means "the calling thread", which is what the
   * offline suites get.
   */
  void setRowScheduler(const RowScheduler *rows) { _rows = rows; }
  const RowScheduler *rowScheduler() const { return _rows; }

protected:
  /** @brief The scheduler in force, or the serial one. Never null. */
  const RowScheduler &rows() const { return _rows ? *_rows : serialRowScheduler(); }

  FlowOptions _opt;
  const RowScheduler *_rows;
};

/** @brief Coarse-to-fine TV-L1 flow (Weickert-style, with a red-black
 * Gauss-Seidel solver and a Gauss-Newton/warped data term).
 *
 * The L1 data term is what makes this a retime-grade estimator rather than a
 * textbook one: bright regions, occlusions and motion discontinuities survive
 * instead of being averaged away, which is exactly where Kronos-class retimers
 * show halos and smearing. Measured on a textured, frame-centred occlusion object
 * (320x180): the object's own motion converges to 0.04 px where the Horn-Schunck
 * baseline reaches 3.1 px, and 0.07 px against 24.5 px when the object overtakes a
 * moving background.
 *
 * Documented limitation: two regions moving in *opposite* directions across one
 * boundary (background +6 px, object -14 px) is the case the coarse-to-fine
 * initialisation loses - the coarse levels see only the dominant motion and hand a
 * wrong field down, and the TV term holds it there (+25.6 px instead of -14 px).
 * It is not a budget problem (400 iterations leaves 26.9 px); it needs lambda 0.20
 * *and* 400 iterations (2.7 px). The background - which is what a retime mostly
 * renders - stays at 0.17 px throughout. M4's bidirectional, occlusion-weighted
 * formulation is the fix; see DECISIONS.md D-24.
 */
class TVL1Estimator : public MotionEstimator {
public:
  explicit TVL1Estimator(const FlowOptions &opt = FlowOptions()) : MotionEstimator(opt) {}

  virtual void estimate(const float *a, const float *b, int w, int h, FlowField &out) const
  {
    estimate(a, b, w, h, out, 0);
  }
  /** @brief Same, with the solver statistics the accuracy suite checks. */
  void estimate(const float *a, const float *b, int w, int h, FlowField &out,
                FlowDiagnostics *diag) const;

  virtual const char *name() const { return "pyramidal TV-L1"; }
};

/** @brief Horn-Schunck baseline (quadratic data term, Tikhonov smoothness).
 *
 * Kept because it is the reference the accuracy suite measures against: it is
 * fast but blurs motion boundaries, which is what a retimer must not do.
 */
class HornSchunckEstimator : public TVL1Estimator {
public:
  HornSchunckEstimator();
  virtual const char *name() const { return "Horn-Schunck (baseline)"; }
};

/** @brief 3x3..(2r+1)x(2r+1) median of the (u,v) pair, free boundary.
 * Post-processing for occluded / low-confidence patches. Not part of the
 * production Motion path, which weights the two estimated directions against each
 * other instead (motion.h); kept because it is the cheapest fix for a speckled
 * field and the accuracy suite measures it. @p rows is the band scheduler of D-31,
 * 0 for "on the calling thread". */
void medianFilterFlow(FlowField &f, int radius, const RowScheduler *rows = 0);

/** @brief Bilinear resample of a flow field to another size, rescaled by @p scale
 * (u,v are pixel displacements of their own resolution). For reusing a field at a
 * different resolution; the plugin estimates and samples at the delivered
 * resolution, so it does not currently use this. @p rows as above. */
void resampleFlow(const FlowField &src, FlowField &dst, int w, int h, double scale,
                  const RowScheduler *rows = 0);

/** @brief Catmull-Rom weights for the taps at t-1, t, t+1, t+2 (t in [0,1)), the
 * same spline sampleGray's cubic counterpart uses; exposed so the Motion sampler
 * (motion.h) does not carry a second copy of the spline. */
void cubicWeights(double t, double w[4]);

/** @brief Bilinear sample of a single-channel image, coordinates clamped.
 * Monotone across a step edge (measured: exact at offset 0.25 of a 0/1 step). */
float sampleGray(const std::vector<float> &img, int w, int h, double x, double y);

/** @brief Bicubic (Catmull-Rom) sample of a single-channel image, coordinates
 * clamped, out-of-image taps repeated. Keeps the detail bilinear removes, so
 * sub-pixel warps do not attenuate the gradients the flow is measured from: on smooth
 * non-linear content the cubic's mean error is 0.0010 against bilinear's 0.00796, and
 * using bilinear for the warp instead costs sub-pixel EPE 0.008 -> 0.033 px on a clean
 * scene and 0.057 -> 0.113 px at 7 px of motion. The cubic is interpolating but not
 * monotone - it overshoots its own taps by 0.074 of a step's height - and clamping the
 * sample to the taps removes that at no measurable cost either way (sub-pixel bias
 * +0.236 -> +0.228 px, 7 px motion 0.057 -> 0.083 px), so the plain spline is kept. */
float sampleCubic(const std::vector<float> &img, int w, int h, double x, double y);

/** @brief Mean end-point error against a reference field, over an optional mask
 * (mask != 0 selects the pixel). Returns -1 when the sizes do not match. */
double meanEPE(const FlowField &got, const FlowField &ref, const unsigned char *mask);

/** @brief Human-readable summary of a field (max |flow|, mean |flow|). */
std::string describeFlow(const FlowField &f);

} // namespace tw

#endif // OFXLAB_TIMEWARP_FLOW_H
