// TimeWarp Motion method: implementation of motion.h.
//
// Same shape as flow.cpp (D-31): row-by-row passes are cut into BandPlan bands and
// handed to a RowScheduler, which the plugin backs with the host's OFX multi-thread
// suite. Still no OFX here, and still one thread when no scheduler is installed.
//
// The reductions in solve() (residual sums, mean |flow|, the occlusion counts) are
// gathered per band and totalled in band order for the same reason the solver's are:
// the pair's diagnostics decide the occlusion indicators, and those decide what every
// tile of the render gathers from, so they must not depend on how many cores the host
// happened to have.
//
// The whole file is one idea: an intermediate frame is not the average of its two
// neighbours, it is what each neighbour can see of that moment. Each side is
// gathered with its own flow (so the detail of both frames survives), and the
// amount each side contributes is decided by whether the other side can confirm
// the content it is about to paint with - which is the only way to tell a real
// motion from a hole.
//
// What it costs, measured (accuracy suite, battery B): in a region where the content
// does not move at all, a cross-fade is *exact*, whereas this code warps with a flow
// that is only approximately zero there - the smoothness term's bleed from the object
// borders. That costs a few thousandths of image units (background MAE 0.0024 against
// 0.0000), and it is the whole cost of the method. What it buys in the same scenes is
// a factor of 40-400 wherever content actually moves (object interior 0.00025 against
// 0.09578, moving background 0.0023 against 0.0085).

#include "motion.h"

#include <algorithm>
#include <cmath>

namespace tw {

namespace {

const int kMaxComponents = 8; // interleaved floats per pixel handled in one gather

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** @brief Tuning knobs as this file needs them: in range, and with the occlusion
 * ramp's far end strictly past its near end so the division below is never by zero. */
struct SafeOptions {
  double scale; // start of the trust ramp, as a multiple of the mean residual
  double span;  // end of the ramp
  double minWeight;
  int smooth;

  explicit SafeOptions(const MotionOptions &o)
  {
    scale = clampd(o.residualScale, 0.0, 1e4);
    span = std::max(scale + 1e-3, clampd(o.residualSpan, 0.0, 1e4));
    minWeight = clampd(o.minWeight, 0.0, 0.5);
    smooth = clampi(o.weightSmooth, 0, 8);
  }
};

/** @brief Plane over a raw row-major buffer, so the estimator's luma frames are
 * sampled by the same code that samples an OFX image. */
struct PtrPlane : public Plane {
  const float *data;

  PtrPlane(const float *d, int w, int h)
  {
    data = d;
    x1 = y1 = 0;
    x2 = w;
    y2 = h;
    comp = 1;
  }
  virtual const float *row(int y) const
  {
    if (!data || y < y1 || y >= y2)
      return 0;
    return data + (size_t)(y - y1) * (size_t)(x2 - x1);
  }
};

/** @brief Tap positions and weights of a cubic sample of the pixel range [lo,hi].
 * The coordinate is clamped first and the taps then clamped again, which is what
 * sampleCubic does: outside the range the edge tap is repeated, so the sample
 * approaches the edge value instead of wrapping or extrapolating. */
void cubicTaps(double p, int lo, int hi, int idx[4], double w[4])
{
  p = clampd(p, (double)lo, (double)hi);
  const int i0 = (int)std::floor(p);
  for (int k = 0; k < 4; ++k)
    idx[k] = clampi(i0 + k - 1, lo, hi);
  cubicWeights(p - (double)i0, w);
}

/** @brief Bilinear sample of a quantised indicator map, grid coordinates, clamped.
 * Returns [0,1]. Bilinear and not cubic: the map is a weight, and the overshoot a
 * cubic spline takes on a step in it would produce a negative weight. */
double sampleIndicator(const std::vector<unsigned char> &m, int w, int h, double x, double y)
{
  x = clampd(x, 0.0, (double)(w - 1));
  y = clampd(y, 0.0, (double)(h - 1));

  const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
  const double fx = x - x0, fy = y - y0;

  const double a = m[(size_t)y0 * w + x0];
  const double b = m[(size_t)y0 * w + x1];
  const double c = m[(size_t)y1 * w + x0];
  const double d = m[(size_t)y1 * w + x1];
  return ((a * (1.0 - fx) + b * fx) * (1.0 - fy) + (c * (1.0 - fx) + d * fx) * fy) / 255.0;
}

/** @brief Box average of an indicator map, separable, free boundary (a tap outside
 * the grid is absent rather than repeated, so the average stays unbiased at the
 * border). @p scratch and @p tmp are reused row buffers, sized w*h, and @p dst must already hold w*h bytes.
 *
 * Three band passes (D-31), for the same reason blurGray needs two: the horizontal pass
 * only reads rows of its own band, but the vertical pass reads the rows the bands either
 * side just wrote, so it has to be dispatched after every band's horizontal pass has
 * finished. Bands write their own rows of @p dst throughout, which is why no pass here
 * needs a lock. */
void smoothIndicator(const std::vector<unsigned char> &src, std::vector<unsigned char> &dst,
                     int w, int h, int radius, std::vector<float> &scratch, std::vector<float> &tmp,
                     const RowScheduler &rows)
{
  const size_t stride = (size_t)w;
  const BandPlan plan(h);

  if (radius <= 0) {
    rows.run(plan, [&](int band) {
      const size_t i0 = (size_t)plan.y0(band) * stride, i1 = (size_t)plan.y1(band) * stride;
      for (size_t i = i0; i < i1; ++i)
        dst[i] = src[i];
    });
    return;
  }

  rows.run(plan, [&](int band) {
    const size_t i0 = (size_t)plan.y0(band) * stride, i1 = (size_t)plan.y1(band) * stride;
    for (size_t i = i0; i < i1; ++i)
      scratch[i] = (float)src[i];
  });

  rows.run(plan, [&](int band) {
    for (int y = plan.y0(band); y < plan.y1(band); ++y) {
      const size_t row = (size_t)y * stride;
      for (int x = 0; x < w; ++x) {
        double acc = 0.0;
        int cnt = 0;
        for (int dx = -radius; dx <= radius; ++dx) {
          const int sx = x + dx;
          if (sx < 0 || sx >= w)
            continue; // absent, not repeated: the border average stays unbiased
          acc += scratch[row + (size_t)sx];
          ++cnt;
        }
        tmp[row + (size_t)x] = (float)(acc / (double)cnt);
      }
    }
  });

  rows.run(plan, [&](int band) {
    for (int y = plan.y0(band); y < plan.y1(band); ++y) {
      for (int x = 0; x < w; ++x) {
        double acc = 0.0;
        int cnt = 0;
        for (int dy = -radius; dy <= radius; ++dy) {
          if (y + dy < 0 || y + dy >= h)
            continue;
          acc += tmp[(size_t)(y + dy) * stride + (size_t)x];
          ++cnt;
        }
        const double v = acc / (double)cnt;
        dst[(size_t)y * stride + (size_t)x] = (unsigned char)clampi((int)(v + 0.5), 0, 255);
      }
    }
  });
}

/** @brief Where the two candidates gather from and how much each is trusted.
 * Implementation of motionCandidates(), taking the already-sanitised options so the
 * per-pixel call from motionBlendPlane does not re-derive them. */
bool candidatesImpl(const MotionField &f, int x, int y, double beta, const SafeOptions &so,
                    double *ax, double *ay, double *bx, double *by, double *wA, double *wB)
{
  if (f.empty() || !f.contains(x, y))
    return false;

  const size_t i = (size_t)(y - f.y1) * (size_t)f.w + (size_t)(x - f.x1);
  const double b = clampd(beta, 0.0, 1.0);
  const double px = (double)x, py = (double)y;

  // C_A gathers frame A, C_B gathers frame B. Each side is pulled back towards the
  // end it is *not* the frame of, by the fraction of the interval that end is away.
  *ax = px + b * (double)f.bwdU[i];
  *ay = py + b * (double)f.bwdV[i];
  *bx = px + (1.0 - b) * (double)f.fwdU[i];
  *by = py + (1.0 - b) * (double)f.fwdV[i];

  // The trust question is asked of the content being gathered, at the place it is
  // being gathered from: does the other frame contain it too? Content the other
  // frame does not contain is content that appears or disappears somewhere inside
  // the interval, and the closer the target is to the far end the less of it is
  // still there - hence beta on one side and 1 - beta on the other.
  const double occA = sampleIndicator(f.occA, f.w, f.h, px - (double)f.x1 + b * (double)f.bwdU[i],
                                      py - (double)f.y1 + b * (double)f.bwdV[i]);
  const double occB = sampleIndicator(f.occB, f.w, f.h,
                                      px - (double)f.x1 + (1.0 - b) * (double)f.fwdU[i],
                                      py - (double)f.y1 + (1.0 - b) * (double)f.fwdV[i]);

  const double trustA = clampd(1.0 - b * occA, 0.0, 1.0);
  const double trustB = clampd(1.0 - (1.0 - b) * occB, 0.0, 1.0);

  // Floor at so.minWeight rather than at 0: a side that the consistency test rejects
  // is faded, not cut, so crossing an occlusion border moves the picture continuously
  // instead of switching from one frame to the other in one beta step.
  double wa = so.minWeight + (1.0 - so.minWeight) * trustA;
  double wb = so.minWeight + (1.0 - so.minWeight) * trustB;
  const double s = wa + wb; // > 0 as long as minWeight > 0
  if (!(s > 0.0)) {
    *wA = *wB = 0.5;
    return true;
  }
  *wA = wa / s;
  *wB = wb / s;
  return true;
}

/** @brief Gather one 4x4 cubic patch of @p p into @p acc, one accumulator per
 * component starting at @p base. All components of a tap are read together, which is
 * why the geometry is computed once for the whole pixel rather than once per
 * component. Scanlines the plane did not deliver contribute 0 - the same rule the
 * rest of the plugin uses for pixels it cannot read. */
void gatherCubic(const Plane &p, double x, double y, int base, int ncomp, double *acc)
{
  int xs[4], ys[4];
  double wx[4], wy[4];
  cubicTaps(x, p.x1, p.x2 - 1, xs, wx);
  cubicTaps(y, p.y1, p.y2 - 1, ys, wy);

  for (int c = 0; c < ncomp; ++c)
    acc[c] = 0.0;

  const size_t comp = (size_t)p.comp;
  const size_t off = (size_t)base;
  for (int j = 0; j < 4; ++j) {
    const float *r = p.row(ys[j]);
    if (!r)
      continue;
    const double wj = wy[j];
    for (int k = 0; k < 4; ++k) {
      const double wk = wx[k] * wj;
      if (wk == 0.0)
        continue;
      const float *px = r + (size_t)(xs[k] - p.x1) * comp + off;
      for (int c = 0; c < ncomp; ++c)
        acc[c] += wk * (double)px[c];
    }
  }
}

/** @brief Clip a loop rect to what @p dst can actually hold. */
void clipToDst(const OutputPlane &dst, int *lx1, int *ly1, int *lx2, int *ly2)
{
  *lx1 = std::max(*lx1, dst.x1);
  *ly1 = std::max(*ly1, dst.y1);
  *lx2 = std::min(*lx2, dst.x2);
  *ly2 = std::min(*ly2, dst.y2);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

bool samplePlaneCubic(const Plane &p, double x, double y, int c, double *out)
{
  if (p.empty() || c < 0 || c >= p.comp)
    return false;

  double acc = 0.0;
  gatherCubic(p, x, y, c, 1, &acc);
  if (out)
    *out = acc;
  return true;
}

void samplePlaneCubicAll(const Plane &p, double x, double y, double *out)
{
  if (p.empty() || !out || p.comp < 1 || p.comp > kMaxComponents)
    return;

  const double bt[4] = { 0.0, 0.0, 0.0, 0.0 };
  for (int c = 0; c < p.comp; ++c)
    out[c] = bt[c];
  gatherCubic(p, x, y, 0, p.comp, out);
}

size_t motionFieldBytes(int w, int h)
{
  if (w <= 0 || h <= 0)
    return 0;
  const size_t n = (size_t)w * (size_t)h;
  return 4u * n * sizeof(float) + 2u * n * sizeof(unsigned char) + sizeof(MotionField);
}

size_t MotionField::bytes() const
{
  return motionFieldBytes(w, h);
}

bool MotionField::finite() const
{
  for (size_t i = 0; i < fwdU.size(); ++i) {
    if (!std::isfinite(fwdU[i]) || !std::isfinite(fwdV[i]) || !std::isfinite(bwdU[i]) ||
        !std::isfinite(bwdV[i]))
      return false;
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// solving one bracketing pair

void MotionSolver::solve(const float *a, const float *b, int w, int h, MotionField &out,
                         MotionDiagnostics *diag) const
{
  MotionDiagnostics d;
  out.clear();

  if (!a || !b || w <= 0 || h <= 0) {
    d.usable = false;
    if (diag)
      *diag = d;
    return;
  }

  // Both directions, each estimated on its own. Inverting one field out of the other
  // is the tempting shortcut and the wrong one: an inverted field carries the other
  // field's errors, and in the occluded patches - the only patches the weighting is
  // there to handle - the field is precisely where the errors are.
  FlowField fa, fb;
  FlowDiagnostics da, db;
  _est.estimate(a, b, w, h, fa, &da); // A -> B
  _est.estimate(b, a, w, h, fb, &db); // B -> A

  d.levelsUsed = std::max(da.levelsUsed, db.levelsUsed);
  d.sweeps = da.iterations + db.iterations;

  const size_t n = (size_t)w * h;
  if (fa.size() != n || fb.size() != n || !fa.finite() || !fb.finite()) {
    // A non-finite field is left as no field at all, which makes the caller fall back
    // to the cross-fade: wrong motion data in a render is worse than none.
    d.usable = false;
    if (diag)
      *diag = d;
    return;
  }

  out.reset(w, h);
  out.fwdU.swap(fa.u);
  out.fwdV.swap(fa.v);
  out.bwdU.swap(fb.u);
  out.bwdV.swap(fb.v);

  const SafeOptions so(_mo);
  const PtrPlane pa(a, w, h), pb(b, w, h);
  const RowScheduler &rows = _rows ? *_rows : serialRowScheduler();
  const BandPlan plan(h);

  // Per-band reduction slots, totalled in band order (D-31). These four numbers set
  // lo/inv below, and therefore every occlusion indicator, and therefore what each tile
  // of the render gathers from: summed by whichever thread finished first they would
  // depend on the host's core count.
  std::vector<double> slotA((size_t)plan.bands, 0.0), slotB((size_t)plan.bands, 0.0),
      slotMag((size_t)plan.bands, 0.0), slotMax((size_t)plan.bands, 0.0);
  std::vector<size_t> slotHalfA((size_t)plan.bands, 0), slotHalfB((size_t)plan.bands, 0);

  // Consistency residuals: how far each frame's own brightness is from what the other
  // frame has at the place its flow claims it moved to. A pixel whose flow is right
  // and whose content exists on both sides has a residual of the noise level; a
  // pixel covering an occlusion has one of the size of the content that appeared or
  // disappeared, which is orders of magnitude larger on real footage.
  //
  // Reads the two frames and both flow fields, writes only resA/resB of its own rows:
  // the cheapest part of this function to cut into bands, and it is two cubic gathers
  // per pixel, so it is not a cheap part of the cost either.
  std::vector<float> resA(n), resB(n);
  rows.run(plan, [&](int band) {
    double sumA = 0.0, sumB = 0.0, mag = 0.0, maxFlow = 0.0;
    for (int y = plan.y0(band); y < plan.y1(band); ++y) {
      const size_t row = (size_t)y * (size_t)w;
      for (int x = 0; x < w; ++x) {
        const size_t i = row + (size_t)x;

        double s = 0.0;
        samplePlaneCubic(pb, (double)x + (double)out.fwdU[i], (double)y + (double)out.fwdV[i], 0,
                         &s);
        resA[i] = (float)std::fabs((double)a[i] - s);

        s = 0.0;
        samplePlaneCubic(pa, (double)x + (double)out.bwdU[i], (double)y + (double)out.bwdV[i], 0,
                         &s);
        resB[i] = (float)std::fabs((double)b[i] - s);

        sumA += resA[i];
        sumB += resB[i];

        const double fu = (double)out.fwdU[i], fv = (double)out.fwdV[i];
        mag += std::sqrt(fu * fu + fv * fv);
        maxFlow = std::max(maxFlow, std::fabs(fu));
        maxFlow = std::max(maxFlow, std::fabs(fv));
        maxFlow = std::max(maxFlow, (double)std::fabs(out.bwdU[i]));
        maxFlow = std::max(maxFlow, (double)std::fabs(out.bwdV[i]));
      }
    }
    slotA[(size_t)band] = sumA;
    slotB[(size_t)band] = sumB;
    slotMag[(size_t)band] = mag;
    slotMax[(size_t)band] = maxFlow;
  });

  double sumA = 0.0, sumB = 0.0, meanMag = 0.0, maxFlow = 0.0;
  for (int band = 0; band < plan.bands; ++band) {
    sumA += slotA[(size_t)band];
    sumB += slotB[(size_t)band];
    meanMag += slotMag[(size_t)band];
    maxFlow = std::max(maxFlow, slotMax[(size_t)band]); // a max: order does not matter
  }

  d.residualA = sumA / (double)n;
  d.residualB = sumB / (double)n;
  d.meanResidual = 0.5 * (d.residualA + d.residualB);
  d.meanFlow = meanMag / (double)n;
  d.maxFlow = maxFlow;

  // The indicators are relative to the pair's own mean residual, so one set of
  // defaults behaves the same on a clean plate, on grain, on a dark scene and in HDR.
  // It also means a pair with no inconsistency at all is trusted everywhere, which is
  // what a static or uniformly changing pair should be.
  std::vector<unsigned char> rawA(n), rawB(n);
  std::vector<float> scratch(n), tmp(n);
  if (d.meanResidual > 1e-12) {
    const double lo = so.scale * d.meanResidual;
    const double inv = 1.0 / ((so.span - so.scale) * d.meanResidual);
    rows.run(plan, [&](int band) {
      for (size_t i = (size_t)plan.y0(band) * w, e = (size_t)plan.y1(band) * w; i < e; ++i) {
        const double va = clampd(((double)resA[i] - lo) * inv, 0.0, 1.0);
        const double vb = clampd(((double)resB[i] - lo) * inv, 0.0, 1.0);
        rawA[i] = (unsigned char)clampi((int)(va * 255.0 + 0.5), 0, 255);
        rawB[i] = (unsigned char)clampi((int)(vb * 255.0 + 0.5), 0, 255);
      }
    });
  }

  // Averaged before use: an occlusion border follows the flow field, and the field is
  // ragged there, so an unsmoothed indicator speckles the blend along it.
  smoothIndicator(rawA, out.occA, w, h, so.smooth, scratch, tmp, rows);
  smoothIndicator(rawB, out.occB, w, h, so.smooth, scratch, tmp, rows);

  rows.run(plan, [&](int band) {
    size_t ca = 0, cb = 0;
    for (size_t i = (size_t)plan.y0(band) * w, e = (size_t)plan.y1(band) * w; i < e; ++i) {
      if (out.occA[i] >= 128)
        ++ca;
      if (out.occB[i] >= 128)
        ++cb;
    }
    slotHalfA[(size_t)band] = ca;
    slotHalfB[(size_t)band] = cb;
  });
  size_t halfA = 0, halfB = 0;
  for (int band = 0; band < plan.bands; ++band) {
    halfA += slotHalfA[(size_t)band];
    halfB += slotHalfB[(size_t)band];
  }
  d.occFractionA = (double)halfA / (double)n;
  d.occFractionB = (double)halfB / (double)n;
  d.usable = true;

  if (diag)
    *diag = d;
}

////////////////////////////////////////////////////////////////////////////////
// reduced-resolution estimation guard

int flowGridStepsForQuality(int quality)
{
  // The preview rung only (D-33). The other rungs spend sweeps, which is the dial the flow
  // suite measures as a *quality* dial; this one is the dial it measures as a *cost* dial.
  return quality == eQualityDraft ? 1 : 0;
}

int flowReduceSteps(int w, int h, int *rw, int *rh, int extraSteps)
{
  long cw = std::max(1, w), ch = std::max(1, h);
  int steps = 0;
  while (cw * ch > (long)kMaxFlowPixels && (cw > 1 || ch > 1)) {
    cw = (cw + 1) / 2; // rounded up, so the reduced grid covers the whole frame
    ch = (ch + 1) / 2;
    ++steps;
  }

  int extra = std::max(0, extraSteps);
  if (extra > kMaxExtraGridSteps)
    extra = kMaxExtraGridSteps;
  for (int s = 0; s < extra; ++s) {
    if (cw <= 1 && ch <= 1)
      break; // nothing left to halve: a 1x1 grid would be an estimate of nothing
    cw = (cw + 1) / 2;
    ch = (ch + 1) / 2;
    ++steps;
  }

  if (rw)
    *rw = (int)cw;
  if (rh)
    *rh = (int)ch;
  return steps;
}

void reduceRectTo(const Plane &src, int x1, int y1, int x2, int y2, int gw, int gh,
                  std::vector<float> &dst)
{
  // Left serial on purpose (D-31). Its cells are independent, so it would band-split
  // easily, but it reads scanlines the host owns and it is a filter over the frame
  // rather than an iteration: under a tenth of a millisecond per frame next to the
  // estimate it feeds. prepareMotion runs it once per frame and reuses the result
  // across pairs, which is where its cost actually went.
  dst.clear();
  if (src.comp < 1 || src.empty() || gw <= 0 || gh <= 0 || x2 <= x1 || y2 <= y1)
    return;

  const int w = x2 - x1, h = y2 - y1;
  const int ncomp = src.comp;
  dst.assign((size_t)gw * gh * (size_t)ncomp, 0.0f);

  // Column partitions of the requested rect, computed once: they are the same for both
  // frames of a pair, which is what makes the two coarse grids comparable.
  std::vector<int> cx((size_t)gw + 1);
  for (int i = 0; i <= gw; ++i)
    cx[i] = x1 + (int)((long)i * w / gw);

  std::vector<long> cnt(gw, 0);
  for (int j = 0; j < gh; ++j) {
    const int ry0 = y1 + (int)((long)j * h / gh);
    const int ry1 = y1 + (int)((long)(j + 1) * h / gh);
    float *cell = dst.data() + (size_t)j * (size_t)gw * (size_t)ncomp;
    for (int i = 0; i < gw; ++i)
      cnt[i] = 0;

    for (int y = ry0; y < ry1; ++y) {
      const float *r = src.row(y);
      if (!r)
        continue; // a scanline the host did not deliver is absent, not guessed at

      for (int i = 0; i < gw; ++i) {
        const int c0 = cx[i], c1 = cx[i + 1];
        if (c1 <= c0)
          continue; // a cell with no columns of its own (grid wider than the rect)

        float *c = cell + (size_t)i * (size_t)ncomp;
        const int a = std::max(c0, src.x1), b = std::min(c1, src.x2);
        if (a < b) {
          const float *px = r + (size_t)(a - src.x1) * (size_t)ncomp;
          for (int x = a; x < b; ++x, px += ncomp)
            for (int k = 0; k < ncomp; ++k)
              c[k] += px[k];
          cnt[i] += b - a;
        } else {
          // The cell reaches past this plane's columns: take its nearest delivered
          // column, so the shared grid has a value here instead of a hole.
          const int ex = clampi(c0 < src.x1 ? src.x1 : src.x2 - 1, src.x1, src.x2 - 1);
          const float *px = r + (size_t)(ex - src.x1) * (size_t)ncomp;
          for (int k = 0; k < ncomp; ++k)
            c[k] += px[k];
          ++cnt[i];
        }
      }
    }

    for (int i = 0; i < gw; ++i) {
      if (cnt[i] <= 0)
        continue; // nothing delivered in this cell: leave it empty rather than guess
      float *c = cell + (size_t)i * (size_t)ncomp;
      const float inv = (float)(1.0 / (double)cnt[i]);
      for (int k = 0; k < ncomp; ++k)
        c[k] *= inv;
    }
  }
}

void reducePlaneTo(const Plane &src, int gw, int gh, std::vector<float> &dst)
{
  reduceRectTo(src, src.x1, src.y1, src.x2, src.y2, gw, gh, dst);
}

void resampleMotionField(const MotionField &src, MotionField &dst, int w, int h, double scale,
                         const RowScheduler *rows)
{
  dst.clear();
  if (src.empty() || w <= 0 || h <= 0)
    return;

  dst.reset(w, h, src.x1, src.y1);

  // One band pass: every value read belongs to @p src, which no band writes. The
  // reduced grid of D-25 means this runs over the *full* resolution rect on a 4K clip,
  // so it is worth the dispatch.
  const RowScheduler &sched = rows ? *rows : serialRowScheduler();
  const BandPlan plan(h);
  const double sx = (double)src.w / (double)w, sy = (double)src.h / (double)h;

  sched.run(plan, [&](int band) {
    for (int y = plan.y0(band); y < plan.y1(band); ++y) {
      const double fy = ((double)y + 0.5) * sy - 0.5;
      const int y0 = (int)std::floor(fy);
      const int cy0 = clampi(y0, 0, src.h - 1), cy1 = clampi(y0 + 1, 0, src.h - 1);
      const double ay = fy - (double)y0;

      for (int x = 0; x < w; ++x) {
        const double fx = ((double)x + 0.5) * sx - 0.5;
        const int x0 = (int)std::floor(fx);
        const int cx0 = clampi(x0, 0, src.w - 1), cx1 = clampi(x0 + 1, 0, src.w - 1);
        const double ax = fx - (double)x0;

        const size_t i00 = (size_t)cy0 * src.w + cx0, i01 = (size_t)cy0 * src.w + cx1;
        const size_t i10 = (size_t)cy1 * src.w + cx0, i11 = (size_t)cy1 * src.w + cx1;
        const size_t i = (size_t)y * w + x;

        const double w00 = (1.0 - ax) * (1.0 - ay), w01 = ax * (1.0 - ay);
        const double w10 = (1.0 - ax) * ay, w11 = ax * ay;

        dst.fwdU[i] = (float)(scale * (src.fwdU[i00] * w00 + src.fwdU[i01] * w01 +
                                       src.fwdU[i10] * w10 + src.fwdU[i11] * w11));
        dst.fwdV[i] = (float)(scale * (src.fwdV[i00] * w00 + src.fwdV[i01] * w01 +
                                       src.fwdV[i10] * w10 + src.fwdV[i11] * w11));
        dst.bwdU[i] = (float)(scale * (src.bwdU[i00] * w00 + src.bwdU[i01] * w01 +
                                       src.bwdU[i10] * w10 + src.bwdU[i11] * w11));
        dst.bwdV[i] = (float)(scale * (src.bwdV[i00] * w00 + src.bwdV[i01] * w01 +
                                       src.bwdV[i10] * w10 + src.bwdV[i11] * w11));

        const double oa = ((double)src.occA[i00] * w00 + (double)src.occA[i01] * w01 +
                           (double)src.occA[i10] * w10 + (double)src.occA[i11] * w11) / 255.0;
        const double ob = ((double)src.occB[i00] * w00 + (double)src.occB[i01] * w01 +
                           (double)src.occB[i10] * w10 + (double)src.occB[i11] * w11) / 255.0;
        dst.occA[i] = (unsigned char)clampi((int)(oa * 255.0 + 0.5), 0, 255);
        dst.occB[i] = (unsigned char)clampi((int)(ob * 255.0 + 0.5), 0, 255);
      }
    }
  });
}

////////////////////////////////////////////////////////////////////////////////
// synthesising the target frame

bool motionCandidates(const MotionField &f, int x, int y, double beta, const MotionOptions &opt,
                      double *ax, double *ay, double *bx, double *by, double *wA, double *wB)
{
  return candidatesImpl(f, x, y, beta, SafeOptions(opt), ax, ay, bx, by, wA, wB);
}

void crossFadePlane(const Plane &a, const Plane &b, double beta, OutputPlane &dst, int lx1,
                    int ly1, int lx2, int ly2)
{
  if (dst.empty() || a.comp != b.comp || a.comp != dst.comp || a.comp < 1 ||
      a.comp > kMaxComponents)
    return;

  const double bt = clampd(beta, 0.0, 1.0);
  clipToDst(dst, &lx1, &ly1, &lx2, &ly2);

  const int ncomp = a.comp;
  for (int y = ly1; y < ly2; ++y) {
    float *rd = dst.row(y);
    if (!rd)
      continue;
    const float *ra = a.row(y), *rb = b.row(y);
    for (int x = lx1; x < lx2; ++x) {
      const bool hasA = (ra && x >= a.x1 && x < a.x2);
      const bool hasB = (rb && x >= b.x1 && x < b.x2);
      const float *pa = hasA ? ra + (size_t)(x - a.x1) * (size_t)a.comp : 0;
      const float *pb = hasB ? rb + (size_t)(x - b.x1) * (size_t)b.comp : 0;
      float *pd = rd + (size_t)(x - dst.x1) * (size_t)dst.comp;
      for (int c = 0; c < ncomp; ++c) {
        const double va = pa ? (double)pa[c] : 0.0;
        const double vb = pb ? (double)pb[c] : 0.0;
        pd[c] = (float)((1.0 - bt) * va + bt * vb);
      }
    }
  }
}

void motionBlendPlane(const MotionField &f, double beta, const Plane &a, const Plane &b,
                      OutputPlane &dst, int lx1, int ly1, int lx2, int ly2,
                      const MotionOptions &opt)
{
  if (dst.empty())
    return;

  const int ncomp = a.comp;
  if (ncomp < 1 || ncomp > kMaxComponents || b.comp != ncomp || dst.comp != ncomp)
    return; // the caller's contract: same components everywhere, alpha included

  clipToDst(dst, &lx1, &ly1, &lx2, &ly2);
  if (lx2 <= lx1 || ly2 <= ly1)
    return;

  if (f.empty()) {
    // No motion data for this pair: the Blend method's dissolve, which is also what
    // the plugin does when an estimate was unusable.
    crossFadePlane(a, b, beta, dst, lx1, ly1, lx2, ly2);
    return;
  }

  const SafeOptions so(opt);
  const double bt = clampd(beta, 0.0, 1.0);
  double accA[kMaxComponents], accB[kMaxComponents];

  for (int y = ly1; y < ly2; ++y) {
    float *rd = dst.row(y);
    if (!rd)
      continue;

    for (int x = lx1; x < lx2; ++x) {
      float *pd = rd + (size_t)(x - dst.x1) * (size_t)dst.comp;

      double ax, ay, bx, by, wA, wB;
      if (!candidatesImpl(f, x, y, bt, so, &ax, &ay, &bx, &by, &wA, &wB)) {
        // Outside the field's grid (a tile that reaches past the frame the flow was
        // estimated on). Nothing to gather with, so fade the two frames here.
        const float *ra = a.row(y), *rb = b.row(y);
        for (int c = 0; c < ncomp; ++c) {
          const double va = (ra && x >= a.x1 && x < a.x2)
                              ? (double)ra[(size_t)(x - a.x1) * (size_t)a.comp + c]
                              : 0.0;
          const double vb = (rb && x >= b.x1 && x < b.x2)
                              ? (double)rb[(size_t)(x - b.x1) * (size_t)b.comp + c]
                              : 0.0;
          pd[c] = (float)((1.0 - bt) * va + bt * vb);
        }
        continue;
      }

      gatherCubic(a, ax, ay, 0, ncomp, accA);
      gatherCubic(b, bx, by, 0, ncomp, accB);
      for (int c = 0; c < ncomp; ++c)
        pd[c] = (float)(wA * accA[c] + wB * accB[c]);
    }
  }
}

} // namespace tw
