/*
  TimeWarp — OFX Lab.

  A Kronos-class retimer: it re-samples the Source clip at source times chosen
  by a speed curve, with Frame (nearest), Blend (box/shutter cross-fade) and
  Motion (bidirectional optical flow, motion.h) interpolation.
  There is no reference gizmo to port, so the semantics below ARE the spec —
  pinned in DECISIONS.md D-21..D-25.

  Timing (D-21). Let s(t) be the Speed knob. `step` = source frames advanced per
  output frame, integrated over each unit output-frame interval, anchored at the
  Source clip's first frame:

    OutputSpeed  s keyed on OUTPUT time:  dsrc/dout = s(t_out)
                 step over [t, t+1] = s(t + 0.5)
    InputSpeed   s keyed on SOURCE time:  dsrc/dout = s(t_src)
                 step over [t, t+1] = midpoint rule (RK2) of src' = s(src)
    Frame        no integration: src(t) = frame(t), the Frame knob read at t

  Both speed modes describe the retime as a ratio of durations (Kronos: "double
  speed halves the duration of the clip") and differ only in which timeline the
  keyframes sit on; a constant speed behaves identically in both. Negative
  speeds reverse. Output frame intervals are never subdivided: `speed` is read
  at the midpoint of each *output* frame, so the output sequence is always one
  frame per host frame (D-23).

  Anchor + hold (D-23). Output frame anchorOut (the Source clip's first frame)
  maps to the first frame of the source range (the last frame when reversing).
  Walking forward the source time advances until it leaves the source range,
  where it stays clamped: every output time outside the mapped range holds the
  nearest available source frame. No extrapolation, no wrap-around, so the
  plugin never asks the host for a frame it does not have.

  Output range (D-22). In the General context we report it via getTimeDomain:
  output frames run from the first to the last whose mapped source time still
  lies inside the source range, i.e. duration = source_duration / |speed|
  (Kronos: intervals/speed + 1). A frozen (speed 0) mapping yields the source
  duration. Frame timing keeps the input range: the Frame knob is a per-output-frame
  source time, and a plugin has no way to report a range for a knob it does not
  integrate. In the Filter context the host owns the frame range, so it follows the
  input.

  Exposure. Output frame t covers the source-time interval
  [src(t-0.5), src(t+0.5)]; Shutter narrows that to a fraction centred on
  src(t). Samples are the integer source frames overlapping the interval,
  weighted by the overlap and renormalised (box filter):
    - shutter 1, speed 2  -> averages the 2 source frames the frame spans
    - shutter 1, speed .5 -> the two bracketing frames, linear cross-fade
    - shutter 0           -> a single point sample
  Frame timing reads the same interval through the Frame knob, at t±0.5: an animated
  knob motion-blurs exactly as fast as it is scrubbed, and a static one collapses the
  interval to a point - Frame, Blend and Motion then all give that one frame. So Blend
  degenerates to Frame as shutter -> 0, and speed 1 / shutter anything reduces to
  exactly one weight-1 sample.

  Sampling is temporal only in M1/M2: pixels are read at their own position from
  each contributing source frame; out-of-range pixels contribute 0 (a retimer
  must not smear or clamp the picture, unlike SharpenZ's edge-clamped gather).
  Out-of-range *times* hold the nearest available source frame.
*/

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsParam.h"
#include "ofxsProcessing.H"

#include "ofxlab_maskmix.h"
#include "motion.h"

namespace tw {

enum Method { eMethodFrame = 0, eMethodBlend = 1, eMethodMotion = 2 };
enum Timing { eTimingOutputSpeed = 0, eTimingInputSpeed = 1, eTimingFrame = 2 };

// Hard caps: a pathological speed curve must not build an unbounded table or
// average ten thousand frames per output frame.
const int kMaxRetimeFrames = 100000;
const int kMaxBoxSamples = 512;
const double kSpeedLimit = 1e4;

////////////////////////////////////////////////////////////////////////////////
// pure core (host-free, unit-testable)

struct SpeedFn {
  virtual ~SpeedFn() {}
  virtual double operator()(double t) const = 0;
};

/** @brief The Frame timing knob: the absolute source frame to show at output time t. */
struct FrameFn {
  virtual ~FrameFn() {}
  virtual double operator()(double t) const = 0;
};

// NaN/inf-safe speed, magnitude capped (a 10000x retime is already absurd).
inline double sanitiseSpeed(double s)
{
  if (!(s == s))
    return 1.0;
  if (s > kSpeedLimit)
    return kSpeedLimit;
  if (s < -kSpeedLimit)
    return -kSpeedLimit;
  return s;
}

// Advance the source time from output frame `out` by h output frames (h = ±1).
// OutputSpeed samples s at the midpoint of the traversed output interval;
// InputSpeed integrates src' = s(src) with the midpoint (RK2) rule.
inline double advance(Timing timing, const SpeedFn &s, double out, double src, double h)
{
  if (timing == eTimingInputSpeed) {
    double k = s(src);
    return src + h * s(src + 0.5 * h * k);
  }
  return src + h * s(out + 0.5 * h);
}

struct TimeMap {
  bool valid;
  double outFirst, outLast;  // output frames whose mapping lies in the source range
  double anchorOut;          // output frame that maps to anchorSrc
  double anchorSrc;
  double srcFirst, srcLast;  // normalised source range (srcFirst <= srcLast)
  std::vector<double> src;   // src[k] = source time at output frame anchorOut + k
  bool truncated;

  TimeMap()
    : valid(false), outFirst(0), outLast(0), anchorOut(0), anchorSrc(0)
    , srcFirst(0), srcLast(0), truncated(false) {}

  double srcAt(double out) const;  // linear between integer frames, held outside
};

// Build the mapping for the whole source range. srcFirst/srcLast are the frames
// the host can actually deliver (D-23: always hold, never extrapolate).
//
// The table walks the mapping one step BEYOND each end of the source range
// (kPad samples): the exposure interval of an end frame needs the true
// continuous mapping just outside the range, so that (for example) the last
// output frame of a 2x retime still averages the final two source frames
// instead of holding the second-to-last one. Values are clamped to the source
// range on query (TimeMap::srcAt), which is what implements the hold.
inline TimeMap buildForward(Timing timing, const SpeedFn &speed, double srcFirst, double srcLast)
{
  const double kPad = 2.0;  // table samples kept past each boundary

  TimeMap m;
  if (!(srcFirst <= srcLast))
    return m;

  m.srcFirst = srcFirst;
  m.srcLast = srcLast;
  m.anchorOut = srcFirst;  // output frame numbering follows the host's frames

  const double s0 = speed(srcFirst);
  m.anchorSrc = (s0 < 0.0) ? srcLast : srcFirst;
  m.src.push_back(m.anchorSrc);  // table[0] belongs to output frame anchorOut

  // forward walk
  double outLast = m.anchorOut;
  bool crossedFwd = false, cappedFwd = false;
  {
    double out = m.anchorOut, v = m.anchorSrc;
    double pad = kPad;
    for (int i = 0; i < kMaxRetimeFrames; ++i) {
      const double nv = advance(timing, speed, out, v, 1.0);
      const bool outside = (nv < srcFirst || nv > srcLast);
      if (outside && !crossedFwd) {
        crossedFwd = true;
        const double bound = (nv > srcLast) ? srcLast : srcFirst;
        const double d = nv - v;
        double frac = (d == 0.0) ? 0.0 : (bound - v) / d;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        outLast = std::floor(out + frac + 1e-9);
      }
      out += 1.0;
      v = nv;
      m.src.push_back(v);
      if (crossedFwd && --pad <= 0.0)
        break;
      if (i == kMaxRetimeFrames - 1)
        cappedFwd = true;
    }
  }

  // backward walk
  double outFirst = m.anchorOut;
  bool crossedBack = false, cappedBack = false;
  {
    double out = m.anchorOut, v = m.anchorSrc;
    double pad = kPad;
    std::vector<double> back;
    for (int i = 0; i < kMaxRetimeFrames; ++i) {
      const double nv = advance(timing, speed, out, v, -1.0);
      const bool outside = (nv < srcFirst || nv > srcLast);
      if (outside && !crossedBack) {
        crossedBack = true;
        const double bound = (nv > srcLast) ? srcLast : srcFirst;
        const double d = nv - v;
        double frac = (d == 0.0) ? 0.0 : (bound - v) / d;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        outFirst = std::ceil(out - frac - 1e-9);
      }
      out -= 1.0;
      v = nv;
      back.push_back(v);
      if (crossedBack && --pad <= 0.0)
        break;
      if (i == kMaxRetimeFrames - 1)
        cappedBack = true;
    }
    // `back` was collected newest-first; the table is chronological
    std::reverse(back.begin(), back.end());
    m.src.insert(m.src.begin(), back.begin(), back.end());
    m.anchorOut -= (double)back.size();
  }

  // A walk that hit the cap without crossing a boundary means the mapping is
  // (near) frozen: the output then spans the source duration, one output frame
  // per source frame (D-23).
  if (cappedFwd && !crossedFwd)
    outLast = srcLast;
  if (cappedBack && !crossedBack)
    outFirst = srcFirst;
  m.truncated = cappedFwd || cappedBack;

  // never claim frames the table cannot back
  const double tableFirst = m.anchorOut;
  const double tableLast = m.anchorOut + (double)m.src.size() - 1.0;
  if (outFirst < tableFirst) outFirst = tableFirst;
  if (outLast > tableLast)   outLast = tableLast;
  if (outLast < outFirst)    outLast = outFirst;

  m.outFirst = outFirst;
  m.outLast = outLast;
  m.valid = true;
  return m;
}

inline double TimeMap::srcAt(double out) const
{
  if (src.empty())
    return out;
  const double nLast = (double)src.size() - 1.0;
  double f = out - anchorOut;
  if (f <= 0.0)
    return src[0] < srcFirst ? srcFirst : (src[0] > srcLast ? srcLast : src[0]);
  if (f >= nLast) {
    double v = src[(size_t)nLast];
    return v < srcFirst ? srcFirst : (v > srcLast ? srcLast : v);
  }
  const int i = (int)f;
  const double frac = f - (double)i;
  double v = src[(size_t)i] + frac * (src[(size_t)(i + 1)] - src[(size_t)i]);
  if (v < srcFirst) v = srcFirst;
  if (v > srcLast)  v = srcLast;
  return v;
}

// fold a frame index into the frame range the host can deliver
inline long clampToRange(long k, double rangeFirst, double rangeLast)
{
  if ((double)k < rangeFirst) return (long)std::ceil(rangeFirst);
  if ((double)k > rangeLast)  return (long)std::floor(rangeLast);
  return k;
}

// nearest source frame (ties away from -inf)
inline long nearestFrame(double x){
  return (long)std::floor(x + 0.5);
}

// Box samples over the source-time interval [a,b]: every integer source frame
// whose [k-0.5, k+0.5] cell overlaps [a,b], weighted by the overlap and
// renormalised so the weights always sum to 1. Frames the host cannot deliver
// are folded onto the nearest available one (the "always hold" rule, D-23).
// A span wider than kMaxBoxSamples is evenly subsampled.
inline void boxSamples(double a, double b, double rangeFirst, double rangeLast,
                       std::map<long, double> &out)
{
  out.clear();
  if (b < a)
    std::swap(a, b);

  if (!(b - a > 0.0)) {  // zero-width exposure: point sample
    long k = nearestFrame(a);
    if (k < rangeFirst) k = (long)std::ceil(rangeFirst);
    if (k > rangeLast)  k = (long)std::floor(rangeLast);
    out[k] = 1.0;
    return;
  }

  const long kMin = (long)std::ceil(a - 0.5);
  const long kMax = (long)std::floor(b + 0.5);
  const long n = kMax - kMin + 1;

  if (n <= 0) {
    out[nearestFrame(0.5 * (a + b))] = 1.0;
    return;
  }

  if (n <= kMaxBoxSamples) {
    double total = 0.0;
    for (long k = kMin; k <= kMax; ++k) {
      const double w = std::min(b, (double)k + 0.5) - std::max(a, (double)k - 0.5);
      if (w <= 0.0)
        continue;
      long kk = k;
      if ((double)kk < rangeFirst) kk = (long)std::ceil(rangeFirst);
      if ((double)kk > rangeLast)  kk = (long)std::floor(rangeLast);
      out[kk] += w;
      total += w;
    }
    if (total <= 0.0) {
      out[nearestFrame(0.5 * (a + b))] = 1.0;
      return;
    }
    for (std::map<long, double>::iterator it = out.begin(); it != out.end(); ++it)
      it->second /= total;
    return;
  }

  // subsampled box: kMaxBoxSamples evenly spaced points across the interval
  const int N = kMaxBoxSamples;
  double total = 0.0;
  for (int j = 0; j < N; ++j) {
    const double t = a + (b - a) * ((double)j + 0.5) / (double)N;
    long k = nearestFrame(t);
    if ((double)k < rangeFirst) k = (long)std::ceil(rangeFirst);
    if ((double)k > rangeLast)  k = (long)std::floor(rangeLast);
    out[k] += 1.0;
    total += 1.0;
  }
  for (std::map<long, double>::iterator it = out.begin(); it != out.end(); ++it)
    it->second /= total;
}

////////////////////////////////////////////////////////////////////////////////
// Motion sampling schedule (M4)
//
// Motion cannot average integer frames: it synthesises each sub-exposure from the
// two integer frames that bracket it. So the schedule is a set of *fractional*
// source times across the output frame's exposure window, equally spaced (midpoint
// rule) and equally weighted (sum 1), with the window clamped to the frames the
// host can deliver before sampling - a slow-motion tail holds the last frame
// instead of inventing samples past it.
//
// What makes that affordable: the number of flow estimates is bounded by the
// window's span in source frames, not by the number of samples, because adjacent
// samples reuse the same bracketing pair. A window W source frames wide touches at
// most ceil(W) + 1 unit brackets, so widening the shutter buys samples, not
// estimates - and samples are the cheap half.
//
// The other bound is memory, and it is the host's to state: distinctMotionPairs
// counts the estimates a schedule needs, maxPairs caps them, and the plugin caps
// that by the bytes a motion field costs at the frame's pixel count. Coarser
// sampling is the cheaper picture, never the bigger allocation - which is why a
// capped schedule degrades towards whole-frame samples, and a whole-frame sample
// needs no flow at all (DECISIONS.md D-25).

const double kMotionSubSample = 0.5; // source frames per sub-exposure sample
const int kMaxMotionSamples = 6;     // ceiling on samples per output frame
const double kMotionTimeSnap = 1e-6; // a sample this close to a frame IS that frame

/** @brief One sub-exposure of the Motion method: the frame at fractional source
 * time @p f0 + beta * (f1 - f0), contributing @p weight to the output frame.
 * @p f0 == @p f1 (beta 0) is a whole frame, unwarped: either the sample landed on
 * that frame (the snap, which is what keeps Motion exact at speed 1) or the window
 * was clamped against the source range and the frame is held there. */
struct MotionSample {
  long f0, f1;
  double beta, weight;

  MotionSample() : f0(0), f1(0), beta(0.0), weight(0.0) {}
  MotionSample(long a, long b, double be, double w)
    : f0(a), f1(b), beta(be), weight(w) {}
  bool wholeFrame() const { return f0 == f1; }
};

/** @brief The distinct frames a set of sub-exposures reads, ascending - what the
 * host has to deliver for one output frame. */
inline void distinctBracketFrames(const std::vector<MotionSample> &ms, std::vector<long> &out)
{
  out.clear();
  for (size_t i = 0; i < ms.size(); ++i) {
    if (std::find(out.begin(), out.end(), ms[i].f0) == out.end())
      out.push_back(ms[i].f0);
    if (ms[i].f1 != ms[i].f0 && std::find(out.begin(), out.end(), ms[i].f1) == out.end())
      out.push_back(ms[i].f1);
  }
  std::sort(out.begin(), out.end());
}

// The frames one output frame needs, plus their weights (sum 1).
struct SampleSet {
  std::vector<long> frames;  // distinct source frames to fetch, ascending
  std::vector<double> weights; // per-frame weights, sum 1; empty for Motion
  std::vector<MotionSample> motion; // Motion only: the sub-exposures, weights sum 1
  bool single;   // exactly one weight-1 sample -> straight copy (bit-exact)

  SampleSet() : single(false) {}
  long first() const { return frames.empty() ? 0 : frames.front(); }
  long last() const { return frames.empty() ? 0 : frames.back(); }
};

/** @brief Source-time lookup for the three timing modes.
 *
 * Speed modes read the mapped table; Frame timing reads the Frame knob, whose value
 * @b is the source frame (no `+ t`, no integration). Everything downstream - the
 * exposure interval, the box weights, the hold rule - goes through srcAt, so an
 * animated knob is a mapping like any other and the out-of-range clamp is the same
 * "hold the nearest available frame" rule (D-21). */
struct Lookup {
  const TimeMap *map;      // speed modes
  const FrameFn *frame;    // eTimingFrame
  bool frameTiming;

  Lookup() : map(0), frame(0), frameTiming(false) {}

  double srcAt(double out) const
  {
    if (frameTiming)
      return frame ? (*frame)(out) : out; // a host with no such knob follows the input
    return map ? map->srcAt(out) : out;
  }
};

/** @brief How many flow estimates a schedule needs: its distinct *fractional*
 * brackets. A sample that landed on a whole frame needs no estimate at all, which is
 * why the snap is not only an exactness device: it is also why a memory-capped
 * schedule costs nothing at speed 1, where every sample lands on a frame. */
/** @brief The distinct brackets a set of sub-exposures has to be estimated for, canonical
 * (lower frame first) and ascending - the order in which one frame's luma can be reused
 * for the next pair. A sub-exposure that landed on a whole frame needs no estimate. */
inline void distinctBracketPairs(const std::vector<MotionSample> &ms,
                                 std::vector<std::pair<long, long> > &out)
{
  out.clear();
  for (size_t i = 0; i < ms.size(); ++i) {
    if (ms[i].wholeFrame())
      continue;
    long f0 = ms[i].f0, f1 = ms[i].f1;
    if (f1 < f0) std::swap(f0, f1);
    if (std::find(out.begin(), out.end(), std::make_pair(f0, f1)) == out.end())
      out.push_back(std::make_pair(f0, f1));
  }
  std::sort(out.begin(), out.end());
}

inline int distinctMotionPairs(const std::vector<MotionSample> &ms)
{
  std::vector<std::pair<long, long> > pairs;
  distinctBracketPairs(ms, pairs);
  return (int)pairs.size();
}

/** @brief The @p n midpoints of the exposure window [a,b], turned into sub-exposures.
 * Equal weights summing to 1; @p single when they collapse onto one whole frame. */
inline void motionSamplesForCount(double a, double b, int n, double rangeFirst, double rangeLast,
                                  std::vector<MotionSample> &out, bool &single)
{
  out.clear();
  single = false;

  double total = 0.0;
  for (int j = 0; j < n; ++j) {
    const double s = a + (b - a) * ((double)j + 0.5) / (double)n;

    // Snap first: a fractional time that is a frame within rounding must stay that
    // frame exactly, or the speed-1 passthrough would come out of a flow estimate.
    const long near = nearestFrame(s);
    if (std::fabs(s - (double)near) <= kMotionTimeSnap) {
      const long k = clampToRange(near, rangeFirst, rangeLast);
      out.push_back(MotionSample(k, k, 0.0, 1.0));
      total += 1.0;
      continue;
    }

    long f0 = clampToRange((long)std::floor(s), rangeFirst, rangeLast);
    long f1 = clampToRange((long)std::ceil(s), rangeFirst, rangeLast);
    if (f1 < f0) std::swap(f0, f1);

    double beta = 0.0;
    if (f1 > f0) {
      beta = (s - (double)f0) / (double)(f1 - f0);
      if (beta < 0.0) beta = 0.0;
      if (beta > 1.0) beta = 1.0;
    }
    out.push_back(MotionSample(f0, f1, beta, 1.0));
    total += 1.0;
  }

  for (size_t i = 0; i < out.size(); ++i)
    out[i].weight /= total;

  single = (out.size() == 1 && out[0].f0 == out[0].f1 && out[0].weight == 1.0);
}

/** @brief The sub-exposures of output frame @p t, weights summing to 1.
 *
 * @p single is true when the whole schedule collapses onto one integer frame with
 * weight 1 - shutter 0, or a shutter-1 frame of a speed-1 retime - which is the copy
 * fast path and the identity condition.
 *
 * @p maxPairs caps the number of flow estimates the schedule may need (see
 * distinctMotionPairs). 0 means no cap. Fewer sub-exposures is a coarser picture of
 * the same exposure, so the cap degrades sampling rather than memory: a window wide
 * enough that its midpoints land on whole frames costs nothing at all, because a
 * whole-frame sample needs no flow.
 */
inline void buildMotionSamples(const Lookup &lk, double shutter, double t, double rangeFirst,
                               double rangeLast, std::vector<MotionSample> &out, bool &single,
                               int maxPairs = 0)
{
  out.clear();
  single = false;

  const double c = lk.srcAt(t);
  double a = c, b = c;
  if (shutter > 0.0) {
    const double half = 0.5 * shutter * std::fabs(lk.srcAt(t + 0.5) - lk.srcAt(t - 0.5));
    if (half > 0.0) {
      a = c - half;
      b = c + half;
    }
  }

  // Clamp the window to the deliverable frames, by the frames' own cells: the
  // outermost sample can never land outside [firstDeliverable, lastDeliverable].
  const double lo = (double)clampToRange((long)std::floor(a), rangeFirst, rangeLast);
  const double hi = (double)clampToRange((long)std::ceil(b), rangeFirst, rangeLast);
  if (a < lo) a = lo;
  if (b > hi) b = hi;
  if (b < a) std::swap(a, b);

  if (!(b - a > 0.0)) { // zero-width exposure, or a window clamped to one frame
    const long k = clampToRange(nearestFrame(a), rangeFirst, rangeLast);
    out.push_back(MotionSample(k, k, 0.0, 1.0));
    single = true;
    return;
  }

  int n = (int)std::ceil((b - a) / kMotionSubSample);
  if (n < 1) n = 1;
  if (n > kMaxMotionSamples) n = kMaxMotionSamples;

  for (; n > 1; --n) {
    motionSamplesForCount(a, b, n, rangeFirst, rangeLast, out, single);
    if (maxPairs <= 0 || distinctMotionPairs(out) <= maxPairs)
      return;
  }

  motionSamplesForCount(a, b, 1, rangeFirst, rangeLast, out, single);
}

// Samples contributing to output frame t.
//   Frame  -> the single nearest source frame
//   Blend  -> box average over the shutter-scaled source interval the frame covers
//   Motion -> fractional sub-exposures over that same interval (motion.h does the
//             synthesising); frames holds the distinct brackets they read
// shutter 0 collapses all three to the same single sample.
inline void buildSamples(const Lookup &lk, Method method, double shutter, double t,
                         double rangeFirst, double rangeLast, SampleSet &out, int maxPairs = 0)
{
  out.frames.clear();
  out.weights.clear();
  out.motion.clear();
  out.single = false;

  if (method == eMethodMotion) {
    buildMotionSamples(lk, shutter, t, rangeFirst, rangeLast, out.motion, out.single, maxPairs);
    if (out.single) { // the copy path reads the frames/weights pair like every other
      out.frames.push_back(out.motion[0].f0);
      out.weights.push_back(1.0);
    } else {
      distinctBracketFrames(out.motion, out.frames);
    }
    return;
  }

  const double c = lk.srcAt(t);

  if (method == eMethodFrame || !(shutter > 0.0)) {
    const long k = clampToRange(nearestFrame(c), rangeFirst, rangeLast);
    out.frames.push_back(k);
    out.weights.push_back(1.0);
    out.single = true;
    return;
  }

  const double half = 0.5 * shutter * std::fabs(lk.srcAt(t + 0.5) - lk.srcAt(t - 0.5));
  if (!(half > 0.0)) {
    const long k = clampToRange(nearestFrame(c), rangeFirst, rangeLast);
    out.frames.push_back(k);
    out.weights.push_back(1.0);
    out.single = true;
    return;
  }

  std::map<long, double> w;
  boxSamples(c - half, c + half, rangeFirst, rangeLast, w);
  for (std::map<long, double>::const_iterator it = w.begin(); it != w.end(); ++it) {
    if (it->second <= 0.0)
      continue;
    out.frames.push_back(it->first);
    out.weights.push_back(it->second);
  }
  if (out.frames.empty()) {
    const long k = clampToRange(nearestFrame(c), rangeFirst, rangeLast);
    out.frames.push_back(k);
    out.weights.push_back(1.0);
  }
  out.single = (out.frames.size() == 1 && out.weights[0] == 1.0);
  return;
}

} // namespace tw

////////////////////////////////////////////////////////////////////////////////
// knobs -> core
//
// Every knob may be keyframed. The time mapping is a *functional* property, so
// the speed/offset knobs are read through the param suite's time-varying getter
// (paramGetValueAtTime) at every time the mapping is evaluated. A host without
// keyframe interpolation makes the getter throw; we notice once per instance
// (sticky flag) and fall back to the static value, so the plugin works
// everywhere and simply treats the knobs as constant.

struct Params {
  int method;     // tw::Method
  int timing;     // tw::Timing
  double shutter; // [0,1]
  double mix;     // [0,1]
  bool maskInvert;

  Params()
    : method(tw::eMethodBlend), timing(tw::eTimingOutputSpeed)
    , shutter(1.0), mix(1.0), maskInvert(false) {}
};

class KnobSpeedFn : public tw::SpeedFn {
  OFX::DoubleParam *_p;
  bool *_keyframed; // instance-wide sticky flag

public:
  KnobSpeedFn(OFX::DoubleParam *p, bool *keyframed) : _p(p), _keyframed(keyframed) {}

  virtual double operator()(double t) const
  {
    if (*_keyframed) {
      try {
        return tw::sanitiseSpeed(_p->getValueAtTime(t));
      } catch (const std::exception &) {
        *_keyframed = false; // host cannot keyframe-interpolate
      }
    }
    return tw::sanitiseSpeed(_p->getValue());
  }
};

/** @brief The Frame knob, read as an absolute source frame. Keyframed like `speed`:
 * the value is taken at the output time, which is what makes a scrubbing knob a
 * mapping rather than a constant. */
class KnobFrameFn : public tw::FrameFn {
  OFX::DoubleParam *_p;
  bool *_keyframed;

public:
  KnobFrameFn(OFX::DoubleParam *p, bool *keyframed) : _p(p), _keyframed(keyframed) {}

  virtual double operator()(double t) const
  {
    if (*_keyframed) {
      try {
        return _p->getValueAtTime(t);
      } catch (const std::exception &) {
        *_keyframed = false;
      }
    }
    return _p->getValue();
  }
};

////////////////////////////////////////////////////////////////////////////////
// Motion host plumbing (M4)
//
// motion.h is OFX-free on purpose: it reads float planes addressed by absolute
// scanline number, which is exactly how an OFX image is addressed, so an image is
// wrapped here and never copied. Two wrappers:
//
//   ImagePlane   - the image's own interleaved floats at its own delivered bounds.
//                  getRowBytes is deliberately ignored: bounds are the only thing to
//                  trust, and a host that pads its rows must not be misread.
//   LumaRowPlane - Rec.709 luma of one scanline at a time, in a scratch row.
//
// Luma is what the flow is estimated on (flow.h measures a single channel); colour is
// what the render gathers. One scratch row is enough because reduceRectTo consumes each
// scanline before it asks for the next - which is why a 4K frame costs one row of floats
// here instead of a second frame's worth of them. Luma is measured on unpremultiplied
// colour, so a fully transparent pixel has no colour to measure and reads 0, and a matte
// moving over constant colour is not motion the estimator can see (D-25).

// Occlusion weighting has no knobs in v1: the values are the ones the accuracy suite's
// knob sweep pinned (motion.h). One place holds them for both halves - the solver that
// builds a field and the render that reads its weights.
const tw::MotionOptions &motionKnobs()
{
  static const tw::MotionOptions mo; // tw::MotionOptions' measured defaults
  return mo;
}

/** @brief A delivered OFX image seen as a tw::Plane: no copy, its own bounds. */
class ImagePlane : public tw::Plane {
  OFX::Image *_img; // non-const: getPixelAddress is a non-const call in the Support lib

public:
  ImagePlane() : _img(0) {}

  void set(OFX::Image *img, int ncomp)
  {
    _img = img;
    comp = ncomp;
    if (!img) {
      x1 = y1 = x2 = y2 = 0;
      return;
    }
    const OfxRectI b = img->getBounds();
    x1 = b.x1;
    y1 = b.y1;
    x2 = b.x2;
    y2 = b.y2;
  }

  virtual const float *row(int y) const
  {
    if (!_img || y < y1 || y >= y2)
      return 0; // a scanline outside the delivered bounds is absent, not guessed at
    return (const float *)_img->getPixelAddress(x1, y);
  }
};

/** @brief Luma of an OFX image, one scanline at a time (see the note above). */
class LumaRowPlane : public tw::Plane {
  OFX::Image *_img;
  int _ncomp;
  bool _premult;
  mutable std::vector<float> _buf; // the last row computed; valid until the next row()
  mutable int _bufY;

public:
  LumaRowPlane() : _img(0), _ncomp(0), _premult(false), _bufY(std::numeric_limits<int>::min()) {}

  void set(OFX::Image *img)
  {
    _img = img;
    _ncomp = 0;
    _premult = false;
    comp = 1;
    if (!img) {
      x1 = y1 = x2 = y2 = 0;
      _buf.clear();
      return;
    }
    const OfxRectI b = img->getBounds();
    x1 = b.x1;
    y1 = b.y1;
    x2 = b.x2;
    y2 = b.y2;
    _ncomp = (img->getPixelComponents() == OFX::ePixelComponentRGBA) ? 4 : 1;
    _premult = (img->getPreMultiplication() == OFX::eImagePreMultiplied);
    _buf.assign((size_t)std::max(0, x2 - x1), 0.0f);
    _bufY = std::numeric_limits<int>::min(); // no row cached yet
  }

  virtual const float *row(int y) const
  {
    if (!_img || _ncomp == 0 || y < y1 || y >= y2)
      return 0;
    if (_bufY == y)
      return _buf.data();

    const float *px = (const float *)_img->getPixelAddress(x1, y);
    if (!px) {
      _bufY = std::numeric_limits<int>::min();
      return 0;
    }

    const int w = x2 - x1;
    for (int x = 0; x < w; ++x) {
      const float *p = px + (size_t)x * (size_t)_ncomp;
      if (_ncomp == 1) {
        _buf[x] = p[0]; // an alpha-only source has no colour: its density is the signal
        continue;
      }
      const float a = p[3];
      if (!(a > 0.0f)) {
        _buf[x] = 0.0f; // nothing there: unpremultiplied colour under a=0 is undefined
        continue;
      }
      float r = p[0], g = p[1], b = p[2];
      if (_premult) {
        r /= a;
        g /= a;
        b /= a;
      }
      _buf[x] = 0.2126f * r + 0.7152f * g + 0.0722f * b; // Rec.709
    }
    _bufY = y;
    return _buf.data();
  }
};

/** @brief How many source frames a render may hold flow fields for.
 *
 * A field costs about 18 bytes per grid pixel (motionFieldBytes) and it is retained at
 * full resolution, because the render warps at full resolution: a 4K rect is ~150 MB per
 * pair, a 2 MP rect ~37 MB. This budget is therefore what decides how many pairs one
 * output frame can keep at once - 320 MB holds 2 pairs of 4K fields, or the sampling
 * ceiling of 6 at 2 MP - and the schedule is capped to that many pairs before it is
 * built (maxPairs / distinctMotionPairs). It is deliberately well under what a desktop
 * host can spare, and a TV-L1 estimate's own transient at the guard grid (~110 MB for a
 * 4K pair) is the peak this plugin adds on top of the host's frames.
 */
const size_t kFlowMemoryBudget = 320ull * 1024ull * 1024ull;

/** @brief The estimator's threading state, one per plugin instance (D-31). Held next to
 * the field cache it threads through, because the plugin class below needs it as a member
 * and OfxRowScheduler, which uses it, is defined with the estimate.
 *
 * Deliberately only a failure latch: how many cores the host has is @b not cached. Measured
 * on Natron 2.6, multiThreadNumCPUs() answers a different number from one call to the next
 * (26-31 on a 32-core box, and 1 when the host's own pool is momentarily full) because it
 * reports what the host is willing to give this plugin right now, not a core count. Caching
 * that answer latches a "1" onto whatever estimate happened while the pool was busy, and that
 * estimate then runs its remaining few thousand passes serially. Measured, per 152-estimate
 * run: cached per instance (the obvious thing to do) - 26 estimates ran fully serial at
 * 1008-1475 ms while the other 126 took 409 ms, 88.7 s total against 62.3 s uncached; cached
 * per estimate (the other obvious thing) - 16 estimates at 1016-1461 ms, 83.8 s. Both caching
 * variants are worse, and neither is correct: the count is only valid for the moment it is
 * asked (DECISIONS.md D-31). */
struct FlowThreads {
  bool broken; // a dispatch failed: this instance's estimator stays on its own thread
  FlowThreads() : broken(false) {}
};

/** @brief How many band-dispatches one process may have inside the host at once (D-31).
 *
 * A dispatch blocks the thread that asked for it until every band has run, and the bands run
 * on the host's threads. Natron renders on threads taken from its own bounded pool, and its
 * OFX suite submits the band jobs to that same pool and waits (RB-2.6 Engine/OfxHost.cpp:
 * multiThread() maps nThreads over QtConcurrent's global pool and calls waitForFinished()).
 * So a render thread that is itself a pool thread can end up waiting for jobs that only other
 * pool threads could run - and if every pool thread is waiting for jobs of its own, none is
 * left to run anybody's. Nothing in the plugin can detect that: OFX has no way to ask whether
 * the calling thread belongs to the host's pool. What it can do is cap how many of its own
 * threads it leaves blocked at once, which is what this tally is for.
 *
 * The cap is a ceiling on the number of blocked waiters, not on the work: an estimate that
 * cannot get a slot runs the pass on its own thread, which is never blocked by another
 * estimate. The per-pass cost is one atomic compare-and-swap. */
inline std::atomic<int> &dispatchClaims()
{
  static std::atomic<int> claims(0); // per process; C++11 initialises a local static once
  return claims;
}
const int kMaxDispatchClaims = 4; // dispatching is worth its overhead in threads beyond this

/** @brief How many threads one band-dispatch may be spread over (D-31).
 *
 * Not the number of bands - @ref OfxRowScheduler::run takes min(this, bands), so a participant
 * never runs with nothing to do - and not a guess at the machine's core count either. Measured
 * inside Natron 2.6 on this project's 16-core / 32-thread box: 152 flow estimates of a 640x360
 * grid per run (a 360-row grid = 22 bands at kMinRows 16, 64 at kMinRows 4), each run started in
 * a quiet window. Median ms per estimate:
 *
 *   threads cap:        4      8                16     none (host answers 26-31)
 *   22 bands:           590    454 456 462      410    404 407 419 422
 *   64 bands:            -      -               402 403   405 408
 *
 * So throughput is flat from 16 threads up to whatever the host offers, and a cap of 8 costs
 * ~11%. The cap is therefore about the @b tail, not the median: at 64 bands the p90 estimate is
 * 405 ms capped vs 613 uncapped and the worst 420 vs 741 (at 22 bands: 413 vs 407-557, worst 431
 * vs 423-751), and the whole 152-estimate run takes 61 s capped vs 73-77 s uncapped. A wide
 * dispatch captures the host's whole thread pool for the duration of every pass - a few thousand
 * passes per estimate - and the host's own answer then drops to 1 for the other instances sharing
 * the render, which serialises them. Past the physical cores, threads share one core's L1/L2 and
 * execution units, and each extra concurrent band adds another stream of a level's working set
 * (several MB at 640x360) that does not fit in cache.
 *
 * 16 is this box's physical core count, kept as a constant rather than probed (host-genericity,
 * rule 4: reading /proc/cpuinfo for SMT siblings is exactly the kind of host sniffing that stays
 * out of a plugin). Below 16 the cap never binds - the host's answer and BandPlan::kMaxBands
 * already bound it - so the only machines it touches are ones big enough for per-band memory
 * traffic to be the limit, which is where the measurement says fewer streams is safer. The same
 * measurements say the band @b granularity barely matters (402-403 ms at 64 bands vs 410 at 22,
 * both capped at 16), which is why kMinRows stays 16: fewer bands, fewer reduction slots, less
 * per-pass dispatch overhead, for no measurable loss.
 *
 * An earlier version of this constant said 8, from an offline bench (std::thread pool, one
 * estimate at a time): 640x360, 22 bands, serial 495 ms then 270 / 159 / 112 / 130 / 152 / 181 /
 * 241 ms at 2 / 4 / 8 / 12 / 16 / 22 / 32 threads. That knee did @b not reproduce inside a host,
 * and the reason is the bench's own pool: it wakes every participant on one condition variable per
 * pass and waits for every one to check in, participants with no band included, so its per-pass
 * overhead grows with the thread count while the OFX suite's does not. It is kept here as an
 * example of what a bench can mislead with, not as evidence. See D-31 for both tables.
 *
 * Clamping the thread count cannot change the numbers: the band split - not the thread count -
 * is the unit of the computation, and D-31's exactness argument holds for any partition, so
 * using 16 of the available 31 threads costs nothing in accuracy. */
const int kMaxDispatchThreads = 16;

/** @brief Flow fields kept between renders, keyed by the pair they were estimated from
 * and by the Motion tuning that produced them.
 *
 * A field depends on the pixels of its two frames, on the Motion tuning that estimated it,
 * and on nothing else - not on speed, not on shutter, not on the render window. So the
 * time-domain knobs cannot make a field stale: a different one of those is a different *set*
 * of pairs, not different pairs. The Motion knobs are different - a field is a function of
 * the estimator configuration as well as of the pixels - and they are handled as part of an
 * entry's identity rather than as invalidation: an entry held under another tuning is a
 * miss, its estimate is redone and replaces it, and the rest age out through the LRU and
 * endRender() the way unused pairs always have (D-33). That is why only changedClip and
 * purgeCaches clear this cache outright.
 *
 * Reuse is by rect *containment*: a field estimated over a rect is valid anywhere inside
 * it, because the estimate only had more context. Estimating over the union of the frames
 * a render needs - rather than over the render window - is what lets one field serve
 * every tile of a tiled host instead of re-estimating per tile.
 *
 * Fields are held by shared_ptr, so a render gathering through one cannot have it pulled
 * out from under it. The cache is byte-budgeted: the least recently used entry goes
 * first, preferring entries this render is not using, and a field bigger than the whole
 * budget is used for this render and not kept. endRender() drops the pairs this render
 * never looked at, which is what stops a slow-motion scrub from accumulating a field for
 * every frame it has ever visited.
 */
class FlowCache {
public:
  FlowCache() : _bytes(0), _clock(0) {}

  /** @brief The field for this pair if one is held that covers @p rect and was estimated
   * under the same tuning. A tuning mismatch is a miss, not a match: the entry stays where
   * it is and ages out like any other unused pair. */
  std::shared_ptr<const tw::MotionField> lookup(long f0, long f1, const OfxRectI &rect,
                                                const tw::MotionTuning &tuning)
  {
    for (size_t i = 0; i < _e.size(); ++i) {
      Entry &en = _e[i];
      if (en.f0 != f0 || en.f1 != f1)
        continue;
      if (!en.tuning.sameAs(tuning))
        continue; // a field of another tuning is not this render's field
      if (en.rect.x1 > rect.x1 || en.rect.y1 > rect.y1 || en.rect.x2 < rect.x2 ||
          en.rect.y2 < rect.y2)
        continue; // it does not cover the rect this render needs
      en.kept = true;
      en.stamp = ++_clock;
      return en.field;
    }
    return std::shared_ptr<const tw::MotionField>();
  }

  /** @brief Keep a freshly estimated field, evicting the cheapest to lose.
   *
   * One field per pair: an entry of the same pair but another tuning is *replaced*, not kept
   * alongside, because a host renders one tuning at a time and holding both would double the
   * cache to serve a render that will never ask for the older one. */
  void retain(long f0, long f1, const OfxRectI &rect,
              const std::shared_ptr<const tw::MotionField> &field, size_t budget,
              const tw::MotionTuning &tuning)
  {
    if (!field)
      return;
    const size_t nb = field->bytes();
    if (nb > budget)
      return; // bigger than the entire budget: this render uses it, nothing is kept

    for (size_t i = 0; i < _e.size(); ++i) {
      if (_e[i].f0 == f0 && _e[i].f1 == f1) {
        _bytes -= _e[i].field->bytes();
        _e[i].rect = rect;
        _e[i].field = field;
        _e[i].tuning = tuning;
        _e[i].kept = true;
        _e[i].stamp = ++_clock;
        _bytes += nb;
        evictOverBudget(budget);
        return;
      }
    }

    Entry en;
    en.f0 = f0;
    en.f1 = f1;
    en.rect = rect;
    en.field = field;
    en.tuning = tuning;
    en.kept = true;
    en.stamp = ++_clock;
    _e.push_back(en);
    _bytes += nb;
    evictOverBudget(budget);
  }

  /** @brief Forget the pairs this render did not use, and who used what. */
  void endRender()
  {
    std::vector<Entry> kept;
    kept.reserve(_e.size());
    for (size_t i = 0; i < _e.size(); ++i) {
      if (!_e[i].kept)
        continue;
      _e[i].kept = false;
      kept.push_back(_e[i]);
    }
    _e.swap(kept);
    _bytes = 0;
    for (size_t i = 0; i < _e.size(); ++i)
      _bytes += _e[i].field->bytes();
  }

  void clear()
  {
    _e.clear();
    _bytes = 0;
  }
  size_t bytes() const { return _bytes; }
  size_t count() const { return _e.size(); }

private:
  struct Entry {
    long f0, f1; // f0 < f1
    OfxRectI rect;
    std::shared_ptr<const tw::MotionField> field;
    tw::MotionTuning tuning; // the knobs that produced this field (D-33)
    bool kept;
    unsigned long stamp;
  };

  void evictOverBudget(size_t budget)
  {
    while (_bytes > budget && !_e.empty()) {
      size_t victim = 0;
      bool found = false;
      unsigned long oldest = 0;
      for (int pass = 0; pass < 2 && !found; ++pass) {
        for (size_t i = 0; i < _e.size(); ++i) {
          if (pass == 0 && _e[i].kept)
            continue; // this render may be using it
          if (!found || _e[i].stamp < oldest) {
            found = true;
            victim = i;
            oldest = _e[i].stamp;
          }
        }
      }
      if (!found)
        return;
      _bytes -= _e[victim].field->bytes();
      _e[victim] = _e.back();
      _e.pop_back();
    }
  }

  std::vector<Entry> _e;
  size_t _bytes;
  unsigned long _clock;
};

/** @brief The rect every fetched frame lands in: the union of their delivered bounds.
 *
 * It is the rect a pair's flow is estimated over, and it is the field's grid, so a
 * render window inside it is answered by containment with no resampling. No padding:
 * the warp clamps to the frame edge anyway (motion.h), and a padded rect would buy a
 * border of field that can only ever gather clamped pixels. */
OfxRectI unionBounds(const std::map<long, std::unique_ptr<OFX::Image> > &imgs)
{
  OfxRectI r = {0, 0, 0, 0};
  bool first = true;
  for (std::map<long, std::unique_ptr<OFX::Image> >::const_iterator it = imgs.begin();
       it != imgs.end(); ++it) {
    if (!it->second)
      continue;
    const OfxRectI b = it->second->getBounds();
    if (b.x2 <= b.x1 || b.y2 <= b.y1)
      continue;
    if (first) {
      r = b;
      first = false;
    } else {
      r.x1 = std::min(r.x1, b.x1);
      r.y1 = std::min(r.y1, b.y1);
      r.x2 = std::max(r.x2, b.x2);
      r.y2 = std::max(r.y2, b.y2);
    }
  }
  return r;
}

/** @brief Index of a source frame in the render's ordered sample list, or -1. */
int indexOfFrame(const std::vector<long> &frames, long f)
{
  for (size_t i = 0; i < frames.size(); ++i)
    if (frames[i] == f)
      return (int)i;
  return -1;
}

/** @brief Reduced luma of one frame over @p rect onto the @p gw x @p gh estimation grid.
 * False when the frame has no pixels or the grid did not fill. */
bool reducedLuma(OFX::Image *img, const OfxRectI &rect, int gw, int gh, std::vector<float> &out)
{
  LumaRowPlane pl;
  pl.set(img);
  if (pl.empty())
    return false;
  tw::reduceRectTo(pl, rect.x1, rect.y1, rect.x2, rect.y2, gw, gh, out);
  return out.size() == (size_t)gw * (size_t)gh;
}

////////////////////////////////////////////////////////////////////////////////
// processor: temporal gather
//
// Each output pixel is the weighted sum of the SAME pixel position in every
// contributing source frame. Pixels outside a source frame's bounds contribute
// 0: a retimer must never smear or duplicate edge pixels (deliberate contrast
// with SharpenZ's edge-clamped gather, DECISIONS.md D-25). The mask/mix
// dissolve (common/ofxlab_maskmix.h, D-20) then blends that against the input
// at the render time.

class TimeWarpBase : public OFX::ImageProcessor {
public:
  /** @brief One Motion sub-exposure, resolved for the gather: the two bracketing
   * samples (@p ia is the earlier frame, the field's A) and the pair's flow field,
   * which is absent when no estimate was available or the sample landed on a whole
   * frame - in both cases the two frames are cross-faded at the pixel's own position,
   * and at beta 0 that is frame A itself with no rounding. */
  struct MotionGather {
    std::shared_ptr<const tw::MotionField> field;
    int ia, ib;
    double beta, weight;

    MotionGather() : ia(-1), ib(-1), beta(0.0), weight(0.0) {}
  };

protected:

  std::vector<OFX::Image *> _samples; // one image per contributing source frame
  std::vector<OfxRectI> _sampleBounds;
  std::vector<double> _weights; // same order as _samples
  OFX::Image *_srcImg;          // input at the render time: dissolve base
  OfxRectI _srcBounds;
  OFX::Image *_maskImg;
  int _maskComp;
  bool _copyOnly; // single weight-1 sample, mix 1, no mask -> bit-exact copy
  Params _p;
  std::vector<MotionGather> _motion; // Motion only: the sub-exposures to gather
  tw::MotionOptions _mo;             // the occlusion weights the gather reads

public:
  TimeWarpBase(OFX::ImageEffect &instance, const Params &p)
    : OFX::ImageProcessor(instance), _srcImg(0), _maskImg(0), _maskComp(0)
    , _copyOnly(false), _p(p), _mo(motionKnobs())
  {
    _srcBounds.x1 = _srcBounds.y1 = _srcBounds.x2 = _srcBounds.y2 = 0;
  }

  void addSample(OFX::Image *img, double w)
  {
    _samples.push_back(img);
    _sampleBounds.push_back(img ? img->getBounds() : OfxRectI{0, 0, 0, 0});
    _weights.push_back(w);
  }

  void setSrcImg(OFX::Image *v)
  {
    _srcImg = v;
    _srcBounds = v ? v->getBounds() : OfxRectI{0, 0, 0, 0};
  }

  void setMaskImg(OFX::Image *v, int comp)
  {
    _maskImg = v;
    _maskComp = comp;
  }

  void setCopyOnly(bool v) { _copyOnly = v; }

  void addMotionSample(const MotionGather &m) { _motion.push_back(m); }
};

template <class PIX, int nComponents>
class TimeWarpProcessor : public TimeWarpBase {
  static_assert(nComponents >= 1 && nComponents <= 4, "TimeWarp renders RGBA or Alpha only");
  static_assert(std::is_same<PIX, float>::value,
                "Motion gathers float planes: float32 only in v1 (D-07)");

public:
  TimeWarpProcessor(OFX::ImageEffect &instance, const Params &p) : TimeWarpBase(instance, p) {}

  void multiThreadProcessImages(OfxRectI procWindow) override
  {
    const int x1 = procWindow.x1, x2 = procWindow.x2;
    const int y1 = procWindow.y1, y2 = procWindow.y2;

    OfxLab::MaskReader maskReader;
    maskReader.set(_maskImg, _maskComp);
    const bool haveMask = maskReader.active();
    const double mix = _p.mix;

    // straight copy of the one contributing frame (bit-identical passthrough)
    if (_copyOnly) {
      const OfxRectI b = _sampleBounds.empty() ? OfxRectI{0, 0, 0, 0} : _sampleBounds[0];
      for (int y = y1; y < y2; ++y) {
        if (_effect.abort())
          break;
        PIX *dstPix = (PIX *)_dstImg->getPixelAddress(x1, y);
        const int lo = std::max(x1, b.x1), hi = std::min(x2, b.x2);
        if (y < b.y1 || y >= b.y2 || hi <= lo) {
          std::memset(dstPix, 0, (size_t)(x2 - x1) * nComponents * sizeof(PIX));
          continue;
        }
        for (int x = x1; x < lo; ++x)
          for (int c = 0; c < nComponents; ++c)
            dstPix[(x - x1) * nComponents + c] = (PIX)0;
        for (int x = hi; x < x2; ++x)
          for (int c = 0; c < nComponents; ++c)
            dstPix[(x - x1) * nComponents + c] = (PIX)0;
        const PIX *row = (const PIX *)_samples[0]->getPixelAddress(lo, y);
        std::memcpy(dstPix + (size_t)(lo - x1) * nComponents, row,
                    (size_t)(hi - lo) * nComponents * sizeof(PIX));
      }
      return;
    }

    const size_t nSamples = _samples.size();
    std::vector<const PIX *> rows(nSamples, 0);

    // without a mask the blend weight is constant across the row
    const double tRow = haveMask ? 0.0 : OfxLab::maskMix(mix, 1.0, _p.maskInvert);
    const bool pureEffect = !haveMask && tRow >= 1.0; // == the plain retimed output
    const bool needSrc = !pureEffect;

    // Frame/Blend read each sample at the pixel's own position, so a scanline pointer
    // is enough. Motion reads the two bracketing frames at positions the flow picked,
    // which is what tw::Plane and the cubic gather are for. Both methods end in the
    // same emit stage at the bottom of the loop.
    const bool motion = !_motion.empty();
    std::vector<ImagePlane> planes;
    std::vector<const float *> rowA(_motion.size(), 0), rowB(_motion.size(), 0);
    if (motion) {
      planes.resize(nSamples);
      for (size_t k = 0; k < nSamples; ++k)
        planes[k].set(_samples[k], nComponents);
    }

    for (int y = y1; y < y2; ++y) {
      if (_effect.abort())
        break;
      PIX *dstPix = (PIX *)_dstImg->getPixelAddress(x1, y);

      for (size_t k = 0; k < nSamples; ++k) {
        const OfxRectI &b = _sampleBounds[k];
        rows[k] = (y >= b.y1 && y < b.y2 && b.x2 > b.x1)
                    ? (const PIX *)_samples[k]->getPixelAddress(b.x1, y)
                    : 0;
      }
      if (motion) {
        for (size_t k = 0; k < _motion.size(); ++k) {
          rowA[k] = planes[_motion[k].ia].row(y);
          rowB[k] = planes[_motion[k].ib].row(y);
        }
      }
      const PIX *srcRow = 0;
      if (needSrc && y >= _srcBounds.y1 && y < _srcBounds.y2 && _srcBounds.x2 > _srcBounds.x1)
        srcRow = (const PIX *)_srcImg->getPixelAddress(_srcBounds.x1, y);

      for (int x = x1; x < x2; ++x, dstPix += nComponents) {
        double acc[4]; // nComponents <= 4 (RGBA or Alpha)
        for (int c = 0; c < nComponents; ++c)
          acc[c] = 0.0;

        if (!motion) {
          for (size_t k = 0; k < nSamples; ++k) {
            const OfxRectI &b = _sampleBounds[k];
            if (!rows[k] || x < b.x1 || x >= b.x2)
              continue; // out-of-bounds contributes 0
            const PIX *px = rows[k] + (size_t)(x - b.x1) * nComponents;
            const double wk = _weights[k];
            for (int c = 0; c < nComponents; ++c)
              acc[c] += wk * (double)px[c];
          }
        } else {
          for (size_t k = 0; k < _motion.size(); ++k) {
            const MotionGather &m = _motion[k];
            const ImagePlane &pa = planes[m.ia], &pb = planes[m.ib];

            double ax, ay, bx, by, wA, wB;
            if (m.field &&
                tw::motionCandidates(*m.field, x, y, m.beta, _mo, &ax, &ay, &bx, &by, &wA, &wB)) {
              // Both sides as backward warps, gathered at full resolution with the
              // frame-edge clamps motion.h uses (the deliberate contrast with the
              // out-of-bounds rule above, D-25).
              double gA[4], gB[4];
              tw::samplePlaneCubicAll(pa, ax, ay, gA);
              tw::samplePlaneCubicAll(pb, bx, by, gB);
              for (int c = 0; c < nComponents; ++c)
                acc[c] += m.weight * (wA * gA[c] + wB * gB[c]);
              continue;
            }

            // No field for this pixel - outside the grid the flow was estimated on, or an
            // estimate that was unusable. Then this sub-exposure is the Blend method's
            // dissolve, exactly as motionBlendPlane falls back outside a field's grid.
            // A whole-frame sample is here too, where beta is 0 and the result is frame A
            // itself with no rounding at all.
            const float *ra = rowA[k], *rb = rowB[k];
            for (int c = 0; c < nComponents; ++c) {
              const double va = (ra && x >= pa.x1 && x < pa.x2)
                                  ? (double)ra[(size_t)(x - pa.x1) * nComponents + c]
                                  : 0.0;
              const double vb = (rb && x >= pb.x1 && x < pb.x2)
                                  ? (double)rb[(size_t)(x - pb.x1) * nComponents + c]
                                  : 0.0;
              acc[c] += m.weight * ((1.0 - m.beta) * va + m.beta * vb);
            }
          }
        }

        if (pureEffect) {
          for (int c = 0; c < nComponents; ++c)
            dstPix[c] = (PIX)acc[c];
          continue;
        }

        const double t = haveMask ? OfxLab::maskMix(mix, maskReader.valueAt(x, y), _p.maskInvert)
                                  : tRow;
        const PIX *px = 0;
        if (srcRow && x >= _srcBounds.x1 && x < _srcBounds.x2)
          px = srcRow + (size_t)(x - _srcBounds.x1) * nComponents;

        for (int c = 0; c < nComponents; ++c) {
          const float s = px ? (float)px[c] : 0.0f;
          dstPix[c] = (PIX)OfxLab::blendOut(s, (float)acc[c], t);
        }
      }
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
// plugin instance

class TimeWarpPlugin : public OFX::ImageEffect {
protected:
  // do not need to delete these, the ImageEffect is managing them for us
  OFX::Clip *dstClip_;
  OFX::Clip *srcClip_;
  OFX::Clip *maskClip_; // only exists in the General context

  OFX::ChoiceParam *method_, *timing_;
  OFX::DoubleParam *speed_, *frame_, *shutter_, *mix_;
  OFX::BooleanParam *maskInvert_;

  // Motion quality (D-33): the rung, and the three advanced dials only a Custom rung reads.
  // All of them are read *statically*, never keyframe-interpolated: they decide what an
  // estimate *is*, and a keyframed one would make every field this instance holds a function
  // of the time it was estimated at, which no host can show and no cache can honour.
  OFX::ChoiceParam *quality_;
  OFX::IntParam *flowIters_, *flowGrid_;
  OFX::DoubleParam *flowLambda_;

  // The time mapping is a property of the whole speed curve plus the Source
  // clip's frame range, not of a single frame, so it is built once and reused
  // by every action that needs it (D-22). Guarded because a host may call
  // several actions on one instance; invalidated whenever an input does.
  OFX::MultiThread::Mutex _mapMutex;
  bool _mapValid;
  int _mapTiming;
  OfxRangeD _mapSrcRange;
  tw::TimeMap _map;
  mutable bool _keyframed; // sticky: false once the host refuses timed getters

  // Motion (D-25). The fields this instance has estimated, keyed by the pair they came
  // from: they depend on those frames' pixels and on no knob, so only a changed Source
  // clip or a purge invalidates them. Guarded because a host may run several actions on
  // one instance, and held by shared_ptr so a render gathering through a field cannot
  // have it evicted mid-gather.
  OFX::MultiThread::Mutex _flowMutex;
  FlowCache _flow;
  mutable FlowThreads _flowThreads; // how the estimates spread over the host's cores (D-31)

public:
  TimeWarpPlugin(OfxImageEffectHandle handle);

  virtual void render(const OFX::RenderArguments &args) override;
  virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &) override
  {
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
    return false; // full scale: the host's default region
  }
  virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip *&identityClip,
                          double &identityTime) override;
  virtual void getFramesNeeded(const OFX::FramesNeededArguments &args,
                               OFX::FramesNeededSetter &frames) override;
  virtual bool getTimeDomain(OfxRangeD &range) override;

  virtual void purgeCaches() override;
  virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) override;
  virtual void changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName) override;

  void setupAndProcess(TimeWarpBase &processor, const OFX::RenderArguments &args,
                       const Params &p, const tw::SampleSet &samples);

private:
  Params readParams(double t) const;
  OfxRangeD srcRange() const;
  bool mappedRange(int timing, double &outFirst, double &outLast);
  bool samplesAt(double t, const Params &p, tw::SampleSet &out, int maxPairs = 0);
  bool dissolveActive(const Params &p) const;
  bool inputOnly(const Params &p) const; // the dissolve collapses to the input frame

  void ensureMap(int timing, const OfxRangeD &r); // requires _mapMutex held
  void invalidateMap();

  // Motion half (motion.h): fetch, cap, estimate. See the Motion section below.
  static int motionPairCap(const OfxRectI &rect);
  tw::MotionTuning motionTuning() const; // the Motion knobs resolved once per render (D-33)
  void fetchFrames(const tw::SampleSet &want, OFX::BitDepthEnum depth,
                   OFX::PixelComponentEnum comps,
                   std::map<long, std::unique_ptr<OFX::Image> > &pool) const;
  void prepareMotion(const tw::SampleSet &samples, const std::vector<OFX::Image *> &imgs,
                     const std::vector<long> &frames, const OfxRectI &rect,
                     TimeWarpBase &processor, const tw::MotionTuning &tuning);
  std::shared_ptr<tw::MotionField> estimateFlow(const std::vector<float> &ga,
                                                const std::vector<float> &gb, int gw, int gh,
                                                const OfxRectI &rect,
                                                const tw::MotionTuning &tuning) const;
};

TimeWarpPlugin::TimeWarpPlugin(OfxImageEffectHandle handle)
  : OFX::ImageEffect(handle), dstClip_(0), srcClip_(0), maskClip_(0)
  , _mapValid(false), _mapTiming(-1), _keyframed(true)
{
  dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
  srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
  if (getContext() == OFX::eContextGeneral)
    maskClip_ = fetchClip("Mask");

  method_      = fetchChoiceParam("method");
  timing_      = fetchChoiceParam("timing");
  speed_       = fetchDoubleParam("speed");
  frame_       = fetchDoubleParam("frame");
  shutter_     = fetchDoubleParam("shutter");
  mix_         = fetchDoubleParam("mix");
  maskInvert_  = fetchBooleanParam("maskInvert");

  quality_     = fetchChoiceParam("quality");
  flowIters_   = fetchIntParam("flowIterations");
  flowLambda_  = fetchDoubleParam("flowDataWeight");
  flowGrid_    = fetchIntParam("flowGridHalvings");

  _mapSrcRange.min = _mapSrcRange.max = 0.0;
}

Params TimeWarpPlugin::readParams(double t) const
{
  Params p;
  int method = 1, timing = 0;
  double shutter = 1.0, mix = 1.0;
  bool maskInvert = false;

  if (_keyframed) {
    try {
      method_->getValueAtTime(t, method);
      timing_->getValueAtTime(t, timing);
      shutter_->getValueAtTime(t, shutter);
      mix_->getValueAtTime(t, mix);
      maskInvert_->getValueAtTime(t, maskInvert);
    } catch (const std::exception &) {
      _keyframed = false; // host has no timed param getters
    }
  }
  if (!_keyframed) {
    method_->getValue(method);
    timing_->getValue(timing);
    shutter_->getValue(shutter);
    mix_->getValue(mix);
    maskInvert_->getValue(maskInvert);
  }

  p.method = (method >= 0 && method <= tw::eMethodMotion) ? method : tw::eMethodBlend;
  p.timing = (timing >= 0 && timing <= tw::eTimingFrame) ? timing : tw::eTimingOutputSpeed;
  p.shutter = std::min(1.0, std::max(0.0, shutter));
  p.mix = std::min(1.0, std::max(0.0, mix));
  p.maskInvert = maskInvert;
  return p;
}

// The frames the Source clip can actually deliver (D-23: the mapping holds at
// these bounds and the plugin never asks for anything outside them).
OfxRangeD TimeWarpPlugin::srcRange() const
{
  OfxRangeD r = srcClip_->getFrameRange();
  const double kHuge = 1e12;
  if (!(r.max >= r.min) || r.min < -kHuge || r.max > kHuge) {
    // unbounded/uninitialised input range: a one-frame range makes the mapping
    // degenerate into the honest "always hold" behaviour
    r.min = 0.0;
    r.max = 0.0;
  }
  return r;
}

// rebuild the cached mapping if it is stale; requires _mapMutex held
void TimeWarpPlugin::ensureMap(int timing, const OfxRangeD &r)
{
  if (_mapValid && _mapTiming == timing && _mapSrcRange.min == r.min && _mapSrcRange.max == r.max)
    return;

  KnobSpeedFn fn(speed_, &_keyframed);
  _map = tw::buildForward((tw::Timing)timing, fn, r.min, r.max);
  _mapValid = _map.valid;
  _mapTiming = timing;
  _mapSrcRange = r;
}

bool TimeWarpPlugin::mappedRange(int timing, double &outFirst, double &outLast)
{
  const OfxRangeD r = srcRange();
  OFX::MultiThread::AutoMutex guard(_mapMutex);
  ensureMap(timing, r);
  if (!_map.valid)
    return false;
  outFirst = _map.outFirst;
  outLast = _map.outLast;
  return true;
}

// Source frames contributing to output frame `t`, with weights summing to 1.
bool TimeWarpPlugin::samplesAt(double t, const Params &p, tw::SampleSet &out, int maxPairs)
{
  const OfxRangeD r = srcRange();

  if (p.timing == tw::eTimingFrame) {
    // no integration and no table: src(t) = frame(t), the knob read at the output time
    KnobFrameFn fn(frame_, &_keyframed);
    tw::Lookup lk;
    lk.frameTiming = true;
    lk.frame = &fn;
    tw::buildSamples(lk, (tw::Method)p.method, p.shutter, t, r.min, r.max, out, maxPairs);
    return true;
  }

  tw::Lookup lk;
  {
    OFX::MultiThread::AutoMutex guard(_mapMutex);
    ensureMap(p.timing, r);
    if (!_map.valid) {
      out.frames.clear();
      out.weights.clear();
      out.single = false;
      return false;
    }
    lk.map = &_map;
    tw::buildSamples(lk, (tw::Method)p.method, p.shutter, t, r.min, r.max, out, maxPairs);
  }
  return true;
}

// Does the output get dissolved against the input at the render time (mask
// and/or mix < 1)? Then that frame is needed as well (D-20). maskInvert counts
// too even without a connected mask: the unconnected mask is fully on, so the
// inverted weight is 0 and the input is what survives (D-29).
bool TimeWarpPlugin::dissolveActive(const Params &p) const
{
  return p.mix < 1.0 || p.maskInvert || (maskClip_ && maskClip_->isConnected());
}

// maskInvert with no Mask clip connected: the implicit mask is fully on (D-20),
// the inverted dissolve weight is 0 everywhere, and the output *is* the input
// frame at the render time - the retiming is dissolved away entirely.
bool TimeWarpPlugin::inputOnly(const Params &p) const
{
  return p.maskInvert && !(maskClip_ && maskClip_->isConnected());
}

void TimeWarpPlugin::invalidateMap()
{
  OFX::MultiThread::AutoMutex guard(_mapMutex);
  _mapValid = false;
  _map = tw::TimeMap();
  _mapTiming = -1;
}

void TimeWarpPlugin::purgeCaches()
{
  invalidateMap();
  OFX::MultiThread::AutoMutex guard(_flowMutex);
  _flow.clear();
}

void TimeWarpPlugin::changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName)
{
  if (paramName == "speed" || paramName == "timing" || paramName == "method" ||
      paramName == "frame" || paramName == "shutter")
    invalidateMap();
  (void)args;
}

void TimeWarpPlugin::changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName)
{
  if (clipName == kOfxImageEffectSimpleSourceClipName || clipName == srcClip_->name()) {
    invalidateMap();
    // A flow field is an estimate of the source pixels themselves, so new pixels mean
    // new estimates. Knob changes deliberately do not touch the flow cache: a different
    // knob is a different *set* of pairs, not different pairs (D-25).
    OFX::MultiThread::AutoMutex guard(_flowMutex);
    _flow.clear();
  }
  (void)args;
}

// How many flow estimates one output frame may hold at this frame size: the cache
// budget divided by what one retained field costs over the rect the render needs,
// bounded below by 1 (a schedule needs at least one pair to be a Motion schedule at all)
// and above by the sampling ceiling, past which extra pairs cost memory the schedule
// cannot use (buildMotionSamples caps the sample count first).
int TimeWarpPlugin::motionPairCap(const OfxRectI &rect)
{
  const size_t fieldBytes = tw::motionFieldBytes(rect.x2 - rect.x1, rect.y2 - rect.y1);
  if (fieldBytes == 0)
    return 1;
  int pairs = (int)(kFlowMemoryBudget / fieldBytes);
  if (pairs < 1)
    pairs = 1;
  if (pairs > tw::kMaxMotionSamples)
    pairs = tw::kMaxMotionSamples;
  return pairs;
}

// Fetch every frame `want` asks for that the pool does not already hold. A frame the
// host cannot deliver stays absent: the schedule is then rendered from what exists.
void TimeWarpPlugin::fetchFrames(const tw::SampleSet &want, OFX::BitDepthEnum depth,
                                 OFX::PixelComponentEnum comps,
                                 std::map<long, std::unique_ptr<OFX::Image> > &pool) const
{
  for (size_t i = 0; i < want.frames.size(); ++i) {
    const long f = want.frames[i];
    if (pool.count(f))
      continue;
    std::unique_ptr<OFX::Image> img(srcClip_->fetchImage((double)f));
    if (!img)
      continue;
    if (img->getPixelDepth() != depth || img->getPixelComponents() != comps)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported); // no depth conversion in v1
    pool[f] = std::move(img);
  }
}

////////////////////////////////////////////////////////////////////////////////
// Motion: the quality rung and the advanced dials, resolved once per render (D-33)

/** @brief The Motion knobs turned into one @ref tw::MotionTuning.
 *
 * @c quality is the ladder the flow suite measured (flow.h carries the table): Draft,
 * Standard, High and Extreme are points of it, and Custom is the rung where the three advanced
 * dials decide instead. They are read **only** under Custom so that every named rung stays the
 * measured point its help text describes; what they may be asked for is measured too, and the
 * bounds are in their descriptors.
 *
 * The one dial a rung pulls by itself is the estimation grid: Draft asks for an extra halving,
 * because the flow suite found no knob *inside* the estimator that makes it cheaper - only
 * costlier - and battery E3 of the Motion suite is what that halving is allowed to cost. The
 * other rungs spend sweeps.
 *
 * Every dial is clamped to its described range and an unreadable knob leaves the rung's own
 * value: a knob a host cannot deliver must not turn into an estimate of nothing. And none of
 * them are read through the timed getter, unlike the time knobs: they decide what an estimate
 * *is*, so keyframing one would make a cached field depend on when it was estimated (see the
 * FlowCache's identity rule).
 */
tw::MotionTuning TimeWarpPlugin::motionTuning() const
{
  int quality = tw::eQualityStandard;
  if (quality_)
    quality_->getValue(quality);
  if (quality < 0 || quality >= tw::kNumQualities)
    quality = tw::eQualityStandard; // a knob out of range is the default rung, not an error

  tw::MotionTuning t;
  // The residual weighting is the same set the render reads a field's indicators with (see
  // TimeWarpBase's _mo). No knob writes into MotionOptions yet, so on every rung this is
  // motion.h's measured defaults; when an occlusion knob is added it belongs here, not there.
  t.motion = motionKnobs();
  t.flow = tw::flowOptionsForQuality(quality);
  t.gridSteps = tw::flowGridStepsForQuality(quality);

  if (quality != tw::eQualityCustom)
    return t;

  int v = 0;
  if (flowIters_) {
    flowIters_->getValue(v);
    if (v >= tw::kFlowItersMin && v <= tw::kFlowItersMax)
      t.flow.iters = v;
  }
  double d = 0.0;
  if (flowLambda_) {
    flowLambda_->getValue(d);
    if (d >= tw::kFlowLambdaMin && d <= tw::kFlowLambdaMax)
      t.flow.lambda = d;
  }
  if (flowGrid_) {
    flowGrid_->getValue(v);
    if (v >= 0 && v <= tw::kMaxExtraGridSteps)
      t.gridSteps = v;
  }
  return t;
}

////////////////////////////////////////////////////////////////////////////////
// Motion: estimate the pairs this render reads
//
// The pairs stay serial (D-24): they ascend, so consecutive estimates reuse the reduced
// luma of the frame they share - walking (0,1),(1,2),(2,3) reduces each frame once
// instead of twice - and one estimate is already a parallel pass, so overlapping pairs
// would only make the estimator compete with itself for the same cores. Inside one
// estimate, the row bands of D-31 go to the host's threads through OfxRowScheduler below.
//
// The rect is the union of the frames the render delivered, so one estimate answers the
// whole render (and, through the cache's containment rule, every tile of it).

/** @brief tw::RowScheduler backed by the host's OFX multi-thread suite (D-31).
 *
 * A plugin may not spawn its own threads (D-03), so the only door to another core is the
 * suite the host handed us. Each pass is dispatched through its own Processor, which is
 * what keeps @ref run const: the band counter belongs to the dispatch, not to the
 * scheduler, and one pass cannot interfere with another.
 *
 * Why no lock and no barrier are needed: the bands come from BandPlan, they partition the
 * rows, a band body writes only rows of its own band, and a band's share of any reduction
 * goes into its own slot, summed afterwards in band order. The suite's call returns when
 * every spawned thread has finished, which is the whole synchronisation a pass needs. A
 * barrier *inside* a dispatched group was the obvious alternative for the passes that read
 * neighbouring rows, and it is deliberately not used: a group larger than the host's
 * thread pool would deadlock waiting for threads that never start. Those passes are
 * dispatched one phase at a time instead (blurGray, dataTerm, smoothIndicator).
 *
 * Three guards keep a host safe from us:
 *   - @c isSpawnedThread(): never dispatch from inside a dispatch. prepareMotion runs
 *     before processor.process(), so the render thread is not one of the host's OFX
 *     workers; if some host calls it differently, the estimate degrades to serial rather
 *     than deadlocking a render that is already holding the pool.
 *   - @c Claim below: never let more of our own threads wait inside the host's pool than the
 *     host has threads to spare, so there is always a thread left to run the bands we queued.
 *   - @c abort() is asked once per pass, on the render thread and never from a spawned
 *     one, which makes a pass the abort granularity of an estimate (D-24 had it at one
 *     pair, which is coarser than a pass by two orders of magnitude).
 *
 * And one rule about exceptions (D-14): an exception escaping a host-spawned thread ends
 * the process, so every band body is wrapped, and what it threw is rethrown on the thread
 * that asked for the pass, where estimateFlow turns it into "no field". After any failure
 * the estimator stays serial for the rest of this instance's renders instead of failing
 * every pass all over again.
 */
class OfxRowScheduler : public tw::RowScheduler {
public:
  /** @brief The host asked this render to stop. An exception only because "no field" is
   * how estimateFlow reports anything that stopped an estimate. */
  struct Aborted : public std::exception {
    virtual const char *what() const throw() { return "TimeWarp: render aborted mid-estimate"; }
  };

  OfxRowScheduler(const std::function<bool()> &abortFn, FlowThreads &state)
    : _abort(abortFn), _st(state)
  {
  }

  /** @brief Threads this pass may be spread over, 1 meaning "this thread only".
   *
   * Asked of the host @b per pass, because the answer is not a constant: measured on a
   * 32-core box under Natron 2.6 it ranged over 26-31 and dropped to 1 while the host's own
   * pool was busy (see FlowThreads). Caching it would have made a whole node's Motion
   * renders serial because of one unlucky first estimate - and asking per pass is also the
   * only way to respect a host that is deliberately handing us fewer cores right now. The
   * cost is two host calls (microseconds) against a pass that costs milliseconds, and the
   * freedom to change the count mid-estimate is exactly what D-31's exactness argument
   * grants: band splits do not affect the numbers at all.
   */
  int threads() const
  {
    if (_st.broken)
      return 1; // a previous dispatch failed: stay on the thread the host gave us
    if (OFX::MultiThread::isSpawnedThread())
      return 1; // never nest a dispatch inside a dispatch (guards above)
    unsigned n = OFX::MultiThread::getNumCPUs(); // 1 when the host has no multi-thread suite
    if (n < 1)
      n = 1;
    if (n > (unsigned int)tw::BandPlan::kMaxBands)
      n = (unsigned int)tw::BandPlan::kMaxBands; // no more threads than there can be bands
    if (n > (unsigned int)kMaxDispatchThreads)
      n = (unsigned int)kMaxDispatchThreads; // measured knee: more concurrent bands thrash memory
    return (int)n;
  }

  virtual void run(const tw::BandPlan &plan, const std::function<void(int)> &band) const
  {
    if (_abort())
      throw Aborted();

    // min(cpus, bands). Both limits are measured and they are different things: 11 bands on a
    // 320x180 grid is a hard ceiling on how many threads can have something to do - the suite's
    // call waits for every participant it was told to use, so a participant with no band is not
    // free - and kMaxDispatchThreads is the point past which extra concurrent band streams stop
    // paying for themselves.
    const int t = threads();
    const int n = std::min(t, plan.bands);
    if (n > 1) {
      // A dispatch blocks this thread until every band has run, so take a slot first:
      // at most kMaxDispatchClaims of our threads may be waiting inside the host's pool at
      // once, and an estimate that cannot take one runs the pass on its own thread.
      Claim claim(std::min(kMaxDispatchClaims, t - 1));
      if (claim.held()) {
        Dispatch d(plan, band);
        try {
          d.multiThread((unsigned int)n);
        } catch (const std::exception &) {
          fail("the host refused the dispatch"); // rethrown below, as the same kind of failure
        }
        if (d.failed())
          fail("a band body threw");
        return;
      }
    }

    unsigned failed = 0;
    for (int b = 0; b < plan.bands; ++b) {
      try {
        band(b);
      } catch (const std::exception &) {
        ++failed;
      } catch (...) {
        ++failed;
      }
    }
    if (failed)
      fail("band body threw");
  }

private:
  /** @brief A dispatch slot, released when the pass is over. Never waits: if all the slots
   * are taken this pass runs serially, which is the point (see kMaxDispatchClaims). */
  class Claim {
  public:
    explicit Claim(int limit)
    {
      std::atomic<int> &claims = dispatchClaims();
      int have = claims.load();
      while (have < limit) {
        if (claims.compare_exchange_weak(have, have + 1)) {
          _held = true;
          return;
        }
      }
      _held = false;
    }
    ~Claim()
    {
      if (_held)
        dispatchClaims().fetch_sub(1);
    }
    bool held() const { return _held; }

  private:
    bool _held;
    Claim(const Claim &);            // a slot is not a thing two passes may share
    Claim &operator=(const Claim &);
  };

  /** @brief One pass: its own band counter, so a pass is independent of every other one. */
  class Dispatch : public OFX::MultiThread::Processor {
  public:
    Dispatch(const tw::BandPlan &plan, const std::function<void(int)> &band)
      : _plan(plan), _band(band), _next(0), _failed(0)
    {
    }

    virtual void multiThreadFunction(unsigned int /*threadIndex*/, unsigned int /*threadMax*/)
    {
      for (;;) {
        const int b = _next.fetch_add(1); // which band is next is not part of the result
        if (b >= _plan.bands)
          return;
        try {
          _band(b);
        } catch (const std::exception &) {
          ++_failed; // never escapes a thread the host owns
        } catch (...) {
          ++_failed;
        }
      }
    }

    unsigned int failed() const { return _failed; }

  private:
    const tw::BandPlan &_plan;
    const std::function<void(int)> &_band;
    std::atomic<int> _next;
    std::atomic<unsigned int> _failed;
  };

  /** @brief Remember the failure, then report it the way an estimate reports everything
   * it could not finish: as an exception estimateFlow turns into a null field. */
  void fail(const char *why) const
  {
    _st.broken = true;
    throw std::runtime_error(std::string("TimeWarp: flow estimation failed, ") + why);
  }

  std::function<bool()> _abort;
  FlowThreads &_st;
};

void TimeWarpPlugin::prepareMotion(const tw::SampleSet &samples,
                                   const std::vector<OFX::Image *> &imgs,
                                   const std::vector<long> &frames, const OfxRectI &rect,
                                   TimeWarpBase &processor, const tw::MotionTuning &tuning)
{
  const int rw = rect.x2 - rect.x1, rh = rect.y2 - rect.y1;
  if (rw <= 0 || rh <= 0)
    return; // no pixels to estimate over: every sub-exposure cross-fades instead

  // The estimation grid: the cap's own halvings plus what this rung asks for (D-25, D-33).
  int gw = rw, gh = rh;
  tw::flowReduceSteps(rw, rh, &gw, &gh, tuning.gridSteps);

  // The distinct pairs the sub-exposures read: ascending, lower frame first, which is
  // also the order that lets one pair's far luma be the next pair's near frame. A sample
  // that landed on a whole frame needs no estimate at all.
  std::vector<std::pair<long, long> > pairs;
  tw::distinctBracketPairs(samples.motion, pairs);

  std::map<std::pair<long, long>, std::shared_ptr<const tw::MotionField> > fieldOf;
  std::vector<float> ga, gb;
  long grayFrame = -1; // the source frame ga currently holds, reduced onto this grid

  for (size_t k = 0; k < pairs.size(); ++k) {
    if (abort())
      break; // the host asked this render to stop (passes check it too, see OfxRowScheduler)

    const long f0 = pairs[k].first, f1 = pairs[k].second;
    {
      OFX::MultiThread::AutoMutex guard(_flowMutex);
      fieldOf[pairs[k]] = _flow.lookup(f0, f1, rect, tuning);
    }
    if (fieldOf[pairs[k]]) {
      grayFrame = -1; // a cached pair breaks the chain: the next estimate starts fresh
      continue;
    }

    const int i0 = indexOfFrame(frames, f0), i1 = indexOfFrame(frames, f1);
    if (i0 < 0 || i1 < 0) {
      grayFrame = -1; // the host did not deliver one of these frames
      continue;
    }
    if (grayFrame != f0 && !reducedLuma(imgs[i0], rect, gw, gh, ga)) {
      grayFrame = -1;
      continue;
    }
    if (!reducedLuma(imgs[i1], rect, gw, gh, gb)) {
      grayFrame = -1;
      continue;
    }

    // ga is the pair's lower frame, gb the higher one: that is the order the sub-exposures
    // address them in (beta counts from floor to ceil) and the order the field's A/B halves
    // mean. Estimating the other way round would mirror every flow vector.
    std::shared_ptr<tw::MotionField> field = estimateFlow(ga, gb, gw, gh, rect, tuning);

    grayFrame = f1;
    std::swap(ga, gb); // this pair's far frame is the next pair's near one

    if (field) {
      OFX::MultiThread::AutoMutex guard(_flowMutex);
      _flow.retain(f0, f1, rect, field, kFlowMemoryBudget, tuning);
    }
    fieldOf[pairs[k]] = field; // null means "no field": that sub-exposure cross-fades
  }

  {
    // The pairs this render never looked at are not worth carrying into the next frame.
    OFX::MultiThread::AutoMutex guard(_flowMutex);
    _flow.endRender();
  }

  // Resolve the sub-exposures. A sample whose frames the host did not deliver is dropped
  // and the survivors renormalised - the same rule the box samples use, so a partial
  // render stays honest instead of quietly darkening.
  std::vector<TimeWarpBase::MotionGather> gathers;
  gathers.reserve(samples.motion.size());
  double total = 0.0;
  for (size_t i = 0; i < samples.motion.size(); ++i) {
    const tw::MotionSample &m = samples.motion[i];
    const int ia = indexOfFrame(frames, m.f0);
    const int ib = m.wholeFrame() ? ia : indexOfFrame(frames, m.f1);
    if (ia < 0 || ib < 0)
      continue;

    std::shared_ptr<const tw::MotionField> field; // absent for a whole-frame sample
    if (!m.wholeFrame()) {
      long f0 = m.f0, f1 = m.f1;
      if (f1 < f0)
        std::swap(f0, f1);
      std::map<std::pair<long, long>, std::shared_ptr<const tw::MotionField> >::const_iterator it =
          fieldOf.find(std::make_pair(f0, f1));
      if (it != fieldOf.end())
        field = it->second;
    }

    TimeWarpBase::MotionGather g;
    g.field = field;
    g.ia = ia; // beta counts from m.f0, which is the field's frame A
    g.ib = ib;
    g.beta = m.beta;
    g.weight = m.weight;
    gathers.push_back(g);
    total += m.weight;
  }

  if (gathers.size() > 1 && total > 0.0 && !(total == 1.0)) {
    for (size_t i = 0; i < gathers.size(); ++i)
      gathers[i].weight /= total;
  }
  for (size_t i = 0; i < gathers.size(); ++i)
    processor.addMotionSample(gathers[i]);
}

// One pair, both directions, on the guard's grid; then resampled to the rect's full
// resolution, because the render warps at full resolution (D-25). A field that went
// non-finite, or an estimate the solver refused, means "no field": the caller cross-fades
// that sub-exposure rather than failing the render, and a plugin exception must never
// reach the host anyway (D-14).
//
// The scheduler lives here, next to the solver it feeds, so it is dead before this
// function returns and no host thread can outlive the pass it was handed.
std::shared_ptr<tw::MotionField> TimeWarpPlugin::estimateFlow(const std::vector<float> &ga,
                                                              const std::vector<float> &gb, int gw,
                                                              int gh, const OfxRectI &rect,
                                                              const tw::MotionTuning &tuning) const
{
  const int rw = rect.x2 - rect.x1, rh = rect.y2 - rect.y1;
  std::shared_ptr<tw::MotionField> out;
  if (ga.size() != (size_t)gw * (size_t)gh || gb.size() != (size_t)gw * (size_t)gh)
    return out;

  try {
    const OfxRowScheduler rows([this] { return abort(); }, _flowThreads);

    std::shared_ptr<tw::MotionField> coarse(new tw::MotionField());
    tw::MotionSolver solver(tuning); // the estimator *and* the residual weighting of this rung
    solver.setRowScheduler(&rows);
    tw::MotionDiagnostics diag;
    solver.solve(ga.data(), gb.data(), gw, gh, *coarse, &diag);
    coarse->x1 = rect.x1; // solve() works in grid coordinates; the grid's place in the
    coarse->y1 = rect.y1; // image is what the render addresses its gathers by
    if (!diag.usable || !coarse->finite())
      return std::shared_ptr<tw::MotionField>();

    if (gw != rw || gh != rh) {
      // The guard rounds a reduced grid *up*, so the two axes' scales can differ by a
      // fraction of a percent; the field is resampled with one isotropic scale, which is
      // far below the flow error the estimator itself makes.
      std::shared_ptr<tw::MotionField> full(new tw::MotionField());
      tw::resampleMotionField(*coarse, *full, rw, rh, (double)rw / (double)gw, &rows);
      full->x1 = rect.x1;
      full->y1 = rect.y1;
      if (!full->finite())
        return std::shared_ptr<tw::MotionField>();
      out = full;
    } else {
      out = coarse;
    }
  } catch (const std::exception &) {
    return std::shared_ptr<tw::MotionField>(); // std::bad_alloc included: fall back, never throw
  } catch (...) {
    return std::shared_ptr<tw::MotionField>();
  }
  return out;
}

////////////////////////////////////////////////////////////////////////////////
// render plumbing

void TimeWarpPlugin::setupAndProcess(TimeWarpBase &processor, const OFX::RenderArguments &args,
                                     const Params &p, const tw::SampleSet &schedule)
{
  std::unique_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
  if (!dst)
    OFX::throwSuiteStatusException(kOfxStatFailed);

  const OFX::BitDepthEnum dstBitDepth = dst->getPixelDepth();
  const OFX::PixelComponentEnum dstComponents = dst->getPixelComponents();

  std::map<long, std::unique_ptr<OFX::Image> > pool;
  fetchFrames(schedule, dstBitDepth, dstComponents, pool);
  if (pool.empty())
    OFX::throwSuiteStatusException(kOfxStatFailed);

  // Motion caps the number of flow estimates a frame may need to what the cache can
  // afford at this frame size. The cap is applied after the fetch, because what a field
  // costs is only known once the frames are in hand - and the uncapped schedule was
  // fetched first, because its frames are a superset of the capped one's (fewer
  // sub-exposures sample the same window), so the capped schedule is a rebuild of the
  // same exposure from frames the render already has.
  tw::SampleSet sel = schedule;
  if (p.method == tw::eMethodMotion && !sel.single && !sel.motion.empty()) {
    const int maxPairs = motionPairCap(unionBounds(pool));
    if (tw::distinctMotionPairs(sel.motion) > maxPairs) {
      tw::SampleSet capped;
      if (samplesAt(args.time, p, capped, maxPairs) && !capped.frames.empty()) {
        fetchFrames(capped, dstBitDepth, dstComponents, pool);
        sel = capped;
      }
    }
  }

  // The frames the render actually has, ascending, with their weights. Motion keeps its
  // weights on the sub-exposures instead, so its frames carry none (1.0) and nothing is
  // renormalised here. A frame the host cannot deliver is dropped and the surviving
  // weights renormalised, so the exposure stays honest instead of quietly darkening.
  std::vector<long> frames;
  std::vector<OFX::Image *> ordered;
  std::vector<double> weights;
  const bool boxed = !sel.weights.empty();
  double total = 0.0;
  for (size_t i = 0; i < sel.frames.size(); ++i) {
    const long f = sel.frames[i];
    std::map<long, std::unique_ptr<OFX::Image> >::iterator it = pool.find(f);
    if (it == pool.end())
      continue;
    frames.push_back(f);
    ordered.push_back(it->second.get());
    const double w = boxed ? sel.weights[i] : 1.0;
    weights.push_back(w);
    total += w;
  }
  if (ordered.empty())
    OFX::throwSuiteStatusException(kOfxStatFailed);
  if (boxed && ordered.size() > 1 && total > 0.0 && !(total == 1.0)) {
    for (size_t i = 0; i < weights.size(); ++i)
      weights[i] /= total;
  }
  for (size_t i = 0; i < ordered.size(); ++i)
    processor.addSample(ordered[i], weights[i]);

  const bool copyOnly =
    sel.single && !dissolveActive(p) && ordered.size() == 1 && weights[0] == 1.0;

  // The dissolve base is the input at the render time (D-20). Hosts that ask
  // for a fractional time fall back to the nearest frame the clip has.
  std::unique_ptr<OFX::Image> src;
  if (!copyOnly) {
    src.reset(srcClip_->fetchImage(args.time));
    if (!src) {
      const OfxRangeD r = srcRange();
      src.reset(srcClip_->fetchImage((double)tw::clampToRange(tw::nearestFrame(args.time), r.min, r.max)));
    }
  }
  processor.setSrcImg(src.get());

  // optional Mask clip (General context only); hosts deliver alpha (Natron
  // extracts the user-selected mask channel for us); RGBA is accepted too.
  std::unique_ptr<OFX::Image> mask;
  if (maskClip_ && maskClip_->isConnected()) {
    mask.reset(maskClip_->fetchImage(args.time));
    if (mask) {
      if (mask->getPixelComponents() == OFX::ePixelComponentRGBA)
        processor.setMaskImg(mask.get(), 3);
      else if (mask->getPixelComponents() == OFX::ePixelComponentAlpha)
        processor.setMaskImg(mask.get(), 0);
      else
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  }

  // Motion estimates before the gather starts, and only when a gather is actually going
  // to happen: a single weight-1 sample needs no flow at any method. The Motion knobs are
  // resolved once, here, so every estimate of this render - and every cache entry it reads
  // or writes - is the same configuration (D-33).
  if (p.method == tw::eMethodMotion && !copyOnly && !sel.motion.empty()) {
    const tw::MotionTuning tuning = motionTuning();
    prepareMotion(sel, ordered, frames, unionBounds(pool), processor, tuning);
  }

  processor.setDstImg(dst.get());
  processor.setRenderWindow(args.renderWindow);
  processor.setCopyOnly(copyOnly);
  processor.process();
}

void TimeWarpPlugin::render(const OFX::RenderArguments &args)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  const OFX::BitDepthEnum dstBitDepth = dstClip_->getPixelDepth();
  const OFX::PixelComponentEnum dstComponents = dstClip_->getPixelComponents();

  // v1 scope: float32 only (DECISIONS.md D-07)
  if (dstBitDepth != OFX::eBitDepthFloat)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

  const Params p = readParams(args.time);

  tw::SampleSet samples;
  if (inputOnly(p)) {
    // The dissolve weight is 0 everywhere, so no retimed sample reaches the
    // output: ask for the render-time frame alone - the same single frame
    // getFramesNeeded declares - and let the dissolve write it out (D-29).
    const OfxRangeD r = srcRange();
    samples.frames.push_back(tw::clampToRange(tw::nearestFrame(args.time), r.min, r.max));
    samples.single = true;
  } else if (!samplesAt(args.time, p, samples) || samples.frames.empty()) {
    OFX::throwSuiteStatusException(kOfxStatFailed);
  }

  if (dstComponents == OFX::ePixelComponentRGBA) {
    TimeWarpProcessor<float, 4> fred(*this, p);
    setupAndProcess(fred, args, p, samples);
  } else if (dstComponents == OFX::ePixelComponentAlpha) {
    TimeWarpProcessor<float, 1> fred(*this, p);
    setupAndProcess(fred, args, p, samples);
  } else {
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
  }
}

////////////////////////////////////////////////////////////////////////////////
// factory

mDeclarePluginFactory(TimeWarpPluginFactory, {}, {});

using namespace OFX;

/** @brief Put a knob inside a group, if this host has groups at all.
 *
 * @c kOfxParamPropParent is a property a host may not implement, and the Support library
 * throws when a host refuses one - which, in a describe action, means the plugin does not
 * load in that host at all. The nesting is worth offering (Natron and Resolve both show it)
 * and worth nothing else: a knob whose group was refused still works, at the root of the
 * hierarchy. So the failure is caught here rather than allowed to reach the Support library's
 * describe translation (D-14), and the caller describes the knob exactly the same way either
 * way. Same reasoning for the group itself, see describeInContext. */
static void reparent(GroupParamDescriptor *group, ParamDescriptor *knob)
{
  if (!group)
    return;
  try {
    knob->setParent(*group);
  } catch (const std::exception &) {
    // no parent property in this host: the knob stays where it was defined
  }
}

void TimeWarpPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  desc.setLabels("TimeWarp", "TimeWarp", "A Kronos-class retimer: re-samples the source at times chosen by a speed curve, with Frame (nearest), Blend (shutter box average) and Motion (optical flow) interpolation. Speed 2 makes the clip twice as fast and half as long, 0.5 makes it half as fast and twice as long, -1 reverses it; the output frame rate never changes.");
  desc.setPluginGrouping("OFX Lab");

  // General carries the Mask input and the time-domain action (the retimed
  // output range), Filter is kept for hosts that only chain effects (D-22).
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedContext(eContextFilter);

  // v1 scope: float32 only; UByte/UShort come later (DECISIONS.md D-07)
  desc.addSupportedBitDepth(eBitDepthFloat);

  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(true);
  desc.setTemporalClipAccess(true);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void TimeWarpPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
  OFX::ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setTemporalClipAccess(true); // the whole point of the plugin
  srcClip->setSupportsTiles(true);
  srcClip->setIsMask(false);

  // Mask input: General context only (filter contexts forbid masks). Optional
  // and alpha-only; Natron then adds its standard "enable mask" + mask channel
  // selector knobs and delivers the chosen channel as alpha.
  if (context == eContextGeneral) { // describeInContext runs under `using namespace OFX` scope below
    OFX::ClipDescriptor *maskClip = desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setOptional(true);
    maskClip->setTemporalClipAccess(false);
    maskClip->setSupportsTiles(true);
    maskClip->setIsMask(true);
  }

  // Output, in every supported context: a host builds its clip wiring straight from
  // the descriptor, and Natron dereferences getClip("Output") (guarded only by an
  // assert(), so not in a release build) while it computes a node's accepted
  // components — an undescribed Output clip takes the host down at node creation (D-28).
  OFX::ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(ePixelComponentRGBA);
  dstClip->addSupportedComponent(ePixelComponentAlpha);
  dstClip->setSupportsTiles(true);

  OFX::ChoiceParamDescriptor *method = desc.defineChoiceParam("method");
  method->setLabels("Method", "Method", "How the source is sampled between its frames. Frame takes the single nearest source frame (fastest, judder-free but stroby in slow motion). Blend box-averages every source frame the output frame's exposure covers, which is a linear cross-fade in slow motion. Motion synthesises each sub-exposure of the exposure by warping the two frames it sits between with optical flow estimated in both directions, weighted by how much of each frame that flow can actually find in the other: the highest-quality option, and the slowest.");
  method->appendOption("Frame");
  method->appendOption("Blend");
  method->appendOption("Motion");
  method->setDefault(tw::eMethodBlend);

  // The Motion quality rung (D-33). Every number in this help text is a measured row of
  // `./tests/run.sh sweep` (320x180, five analytic scenes, error over the content a frame can
  // actually contain) or of battery E3 of the Motion suite; the rungs are points on that
  // table, not adjectives.
  OFX::ChoiceParamDescriptor *quality = desc.defineChoiceParam("quality");
  quality->setLabels("Quality", "Quality", "Motion only: how much estimator work goes into each flow field. Draft is 0.4x Standard's estimate time and 9x cheaper still with the coarser estimation grid this rung asks for, at up to 3x Standard's error where motion is decidable. Standard is the measured default. High spends 1.6x the time for equal-or-better error on every measured case. Extreme spends 3.5x and strengthens the data term: it is the rung that resolves regions moving against each other (a counter-moving plate goes from 13.3 px of error to 3.5) and it gives back a little accuracy on large, uniform motion. Custom decides nothing and reads the Advanced Motion knobs below instead. Read as a constant, not animated: it changes what an estimate *is*, so a value that varied over time would make every field this node caches depend on when it was estimated.");
  quality->appendOption("Draft");
  quality->appendOption("Standard");
  quality->appendOption("High");
  quality->appendOption("Extreme");
  quality->appendOption("Custom");
  quality->setDefault(tw::eQualityStandard);

  OFX::ChoiceParamDescriptor *timing = desc.defineChoiceParam("timing");
  timing->setLabels("Timing", "Timing", "Which mapping the time knobs drive. Output Speed reads the Speed knob at the output frame, Input Speed integrates it over the source (the two are identical for a constant speed). Frame skips the speed curve entirely: the Frame knob's own value at each output frame IS the source frame to show, which is how you scrub, freeze or step through the clip. Speed is ignored by Frame timing, and Frame is ignored by the two Speed modes.");
  timing->appendOption("Output Speed");
  timing->appendOption("Input Speed");
  timing->appendOption("Frame");
  timing->setDefault(tw::eTimingOutputSpeed);

  OFX::DoubleParamDescriptor *speed = desc.defineDoubleParam("speed");
  speed->setLabels("Speed", "Speed", "Source frames consumed per output frame. 2 plays twice as fast over half as many frames, 0.5 plays in slow motion over twice as many, -1 reverses the clip, 0 freezes on the first frame. A negative speed reverses, and the output frame rate is never changed. Ignored by Frame timing.");
  speed->setRange(-tw::kSpeedLimit, tw::kSpeedLimit);
  speed->setDisplayRange(-4, 4);
  speed->setDefault(1.0);

  OFX::DoubleParamDescriptor *frameParam = desc.defineDoubleParam("frame");
  frameParam->setLabels("Frame", "Frame", "Frame timing only: the source frame this output frame shows, read at the output frame's time. Keyframe it to scrub, freeze or step through the clip - a linear ramp is a retime (10 to 20 across ten output frames plays that section at 1x), a constant is a freeze, a descending ramp reverses. The host interpolates the knob, and that interpolation IS the motion of the retime: Blend and Motion then box-average or flow-warp across exactly the frames the ramp sweeps. Outside the source range the nearest available frame is held. Ignored by the two Speed modes. (Renamed from frameOffset, which was relative to the output frame.)");
  frameParam->setRange(-100000, 100000);
  frameParam->setDisplayRange(-100, 100);
  frameParam->setDefault(0.0);

  OFX::DoubleParamDescriptor *shutter = desc.defineDoubleParam("shutter");
  shutter->setLabels("Shutter", "Shutter", "Exposure as a fraction of the source interval each output frame covers. 1 averages every source frame the exposure spans (a 2x retime blends 3 source frames, slow motion cross-fades the 2 bracketing frames), 0 is a point sample and makes Blend behave exactly like Frame. Under Frame timing the interval is what the Frame knob sweeps across this frame, so a static knob is a point sample at any shutter.");
  shutter->setRange(0, 1);
  shutter->setDisplayRange(0, 1);
  shutter->setDefault(1.0);

  OFX::DoubleParamDescriptor *mix = desc.defineDoubleParam("mix");
  mix->setLabels("Mix", "Mix", "Dissolve between the input (0) and the retimed result (1).");
  mix->setRange(0, 1);
  mix->setDisplayRange(0, 1);
  mix->setDefault(1.0);

  OFX::BooleanParamDescriptor *maskInvert = desc.defineBooleanParam("maskInvert");
  maskInvert->setLabels("Mask Invert", "Mask Invert", "Invert the mask: black areas get the retiming, white areas pass the input through.");
  maskInvert->setDefault(false);

  // The dials the quality rungs stand in for (D-33), grouped.
  //
  // A group is a UI container and not every host implements one: the Support library throws
  // when a host refuses the group type or the parent property, and a plugin that cannot
  // describe itself is a plugin that does not load. So the nesting is *attempted* - which is
  // what Natron and Resolve show - and if a host will not have it the three dials are simply
  // described at the root level, where they behave identically. The group is decoration; the
  // knobs are the interface.
  OFX::GroupParamDescriptor *advanced = 0;
  try {
    advanced = desc.defineGroupParam("advancedMotion");
    advanced->setLabels("Advanced Motion", "Advanced Motion", "The estimator dials the Quality presets stand in for, read only when Quality is Custom so that every named preset stays the measured point its help text describes. Each one is a measured row of `./tests/run.sh sweep` or of the Motion suite's grid battery. Read as constants, not animated.");
    advanced->setOpen(false);
  } catch (const std::exception &) {
    advanced = 0; // no groups in this host: the dials stay at the root level
  }

  // The descriptor's range and the plugin's clamp are the same constants (flow.h), so a knob
  // cannot offer a value the reader would then have to refuse. The defaults are the measured
  // defaults, which is what makes Custom with untouched dials exactly Standard.
  const tw::FlowOptions def;

  OFX::IntParamDescriptor *flowIters = desc.defineIntParam("flowIterations");
  flowIters->setLabels("Flow Iterations", "Flow Iterations", "Custom only: sweeps the estimator budgets to every pyramid level above its coarsest. It is the only dial inside the estimator that moves the clock: measured over five analytic scenes at 320x180, 30/60/120/240/480 sweeps take 192/334/629/1006/1803 ms, and the largest error on content a frame can actually contain falls along the same ladder from 0.173 px to 0.058 px. Diminishing returns all the way up, and 10 sweeps is not enough to resolve anything but uniform motion.");
  flowIters->setRange(tw::kFlowItersMin, tw::kFlowItersMax);
  flowIters->setDefault(def.iters);
  if (advanced)
    reparent(advanced, flowIters);

  OFX::DoubleParamDescriptor *flowLambda = desc.defineDoubleParam("flowDataWeight");
  flowLambda->setLabels("Flow Data Weight", "Flow Data Weight", "Custom only: how closely the flow follows the image relative to how much it is smoothed, in units of one mean image gradient, so the same number means the same thing on a 0..1 clip and on HDR. It costs no time (629-637 ms at any value in the measured table) but it is not free in accuracy - it trades the cases against each other. At 0.02/0.05/0.10/0.20/0.40 a counter-moving plate measures 17.9/13.3/5.8/3.5/2.9 px of error while a 46 px aperiodic translation measures 21.3/0.023/0.10/0.44/0.43: no value wins both, which is why the default is 0.05 and only the top rung pays 0.20.");
  flowLambda->setRange(tw::kFlowLambdaMin, tw::kFlowLambdaMax);
  flowLambda->setDefault(def.lambda);
  flowLambda->setDisplayRange(0.01, 0.4);
  if (advanced)
    reparent(advanced, flowLambda);

  OFX::IntParamDescriptor *flowGrid = desc.defineIntParam("flowGridHalvings");
  flowGrid->setLabels("Flow Grid Halvings", "Flow Grid Halvings", "Custom only: extra times the estimation grid is halved on each axis, on top of the halvings the pixel cap already applies. The estimate runs on the reduced frames; the warping still gathers the full-resolution ones, so this costs resolution of the *flow field's estimation*, never of the output. Measured on two-layer composites at 320x180: one halving is 3.9x faster (256 ms to 65 ms) and costs a background error under 0.004 of full luma range and 1.2x-1.7x the object-interior error; two halvings are 14x faster (18 ms) at 4.5x-6.8x that error. Four is the most a grid can be halved before there is nothing left to estimate.");
  flowGrid->setRange(0, tw::kMaxExtraGridSteps);
  flowGrid->setDefault(0);
  if (advanced)
    reparent(advanced, flowGrid);
}

OFX::ImageEffect *TimeWarpPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
  return new TimeWarpPlugin(handle);
}

namespace OFX
{
namespace Plugin
{
void getPluginIDs(OFX::PluginFactoryArray &ids)
{
  static TimeWarpPluginFactory p("net.ofxlab.timewarp", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX

////////////////////////////////////////////////////////////////////////////////
// time domain (General context, D-22)

bool TimeWarpPlugin::getTimeDomain(OfxRangeD &range)
{
  int timing = 0;
  timing_->getValue(timing);
  if (timing < 0 || timing > tw::eTimingFrame)
    timing = 0;

  // Frame timing has no curve to integrate: the Frame knob is a per-output-frame
  // source time, so the output range follows the input (a plugin cannot report a
  // range for a knob it does not integrate).
  if (timing == tw::eTimingFrame)
    return false;

  double outFirst = 0.0, outLast = 0.0;
  if (!mappedRange(timing, outFirst, outLast))
    return false;

  range.min = outFirst;
  range.max = outLast;
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// frames needed

void TimeWarpPlugin::getFramesNeeded(const OFX::FramesNeededArguments &args,
                                     OFX::FramesNeededSetter &frames)
{
  const Params p = readParams(args.time);

  // The dissolve collapsed to the input (maskInvert, no mask connected): the
  // output *is* the render-time frame, and nothing else is needed (D-29).
  if (inputOnly(p)) {
    const OfxRangeD s = srcRange();
    const double k = (double)tw::clampToRange(tw::nearestFrame(args.time), s.min, s.max);
    OfxRangeD n;
    n.min = k;
    n.max = k;
    frames.setFramesNeeded(*srcClip_, n);
    return;
  }

  // Uncapped on purpose: this action must ask for a superset of the frames the render
  // may end up using, and the memory cap (motionPairCap) only applies to the estimates,
  // not to what the host is told to be ready for.
  tw::SampleSet samples;
  if (!samplesAt(args.time, p, samples) || samples.frames.empty())
    return; // leave it to the host's default

  double lo = (double)samples.first();
  double hi = (double)samples.last();

  // the mask/mix dissolve blends against the input at the render time
  if (dissolveActive(p)) {
    lo = std::min(lo, args.time);
    hi = std::max(hi, args.time);
  }

  OfxRangeD r;
  r.min = lo;
  r.max = hi;
  frames.setFramesNeeded(*srcClip_, r);

  if (maskClip_ && maskClip_->isConnected()) {
    OfxRangeD m;
    m.min = args.time;
    m.max = args.time;
    frames.setFramesNeeded(*maskClip_, m);
  }
}

////////////////////////////////////////////////////////////////////////////////
// identity: a single weight-1 sample with a full dissolve is a copy of one
// source frame. Reporting it (with the mapped time) lets the host skip the
// render entirely; speed 1 / shutter 0 / mix 1 is then bit-identical to the
// input.

bool TimeWarpPlugin::isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip *&identityClip,
                                double &identityTime)
{
  const Params p = readParams(args.time);

  // maskInvert with no mask connected dissolves the effect away entirely: the
  // output is the input frame, whatever the speed curve says (D-29).
  if (inputOnly(p)) {
    const OfxRangeD s = srcRange();
    identityClip = srcClip_;
    identityTime = (double)tw::clampToRange(tw::nearestFrame(args.time), s.min, s.max);
    return true;
  }

  if (dissolveActive(p))
    return false;

  tw::SampleSet samples;
  if (!samplesAt(args.time, p, samples) || !samples.single || samples.frames.empty())
    return false;

  identityClip = srcClip_;
  identityTime = (double)samples.frames[0];
  return true;
}
