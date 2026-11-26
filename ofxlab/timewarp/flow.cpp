// TimeWarp Motion method: dense optical flow. Implementation of flow.h.
//
// Single-threaded code with one hook (D-31): every pass that works row by row is split
// into BandPlan bands and handed to a RowScheduler, which the plugin backs with the
// host's OFX multi-thread suite. This file still knows nothing about OFX and still runs
// on one thread when nobody installs a scheduler, which is what keeps it testable
// outside a host.

#include "flow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace tw {

const RowScheduler &serialRowScheduler()
{
  static const SerialRowScheduler kSerial;
  return kSerial;
}

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A pixel whose forward neighbour does not exist has a zero forward difference
// ("free" boundary conditions, the standard choice for flow solvers).
inline bool hasRight(int x, int w) { return x + 1 < w; }
inline bool hasDown(int y, int h) { return y + 1 < h; }

/** @brief Separable Gaussian blur with reflecting borders.
 *
 * Two band passes, not one (D-31): the horizontal pass only writes rows of its own band,
 * while the vertical pass reads the blurred rows the neighbouring bands wrote - so it can
 * only begin once every band's horizontal pass is done. Splitting a single fused pass at a
 * band boundary is the one thing this solver must not do, and this is the only place in the
 * estimator where a pass has that property.
 */
void blurGray(const float *src, std::vector<float> &dst, int w, int h, double sigma,
              const RowScheduler &rows)
{
  dst.assign((size_t)w * h, 0.0f);
  const BandPlan plan(h);

  if (sigma <= 0.0) {
    rows.run(plan, [&](int b) {
      const int y0 = plan.y0(b), y1 = plan.y1(b);
      std::copy(src + (size_t)y0 * w, src + (size_t)y1 * w, dst.begin() + (size_t)y0 * w);
    });
    return;
  }

  const int r = (int)std::ceil(3.0 * sigma);
  std::vector<double> k((size_t)2 * r + 1);
  double sum = 0.0;
  for (int i = -r; i <= r; ++i) {
    const double g = std::exp(-(double)i * (double)i / (2.0 * sigma * sigma));
    k[(size_t)i + r] = g;
    sum += g;
  }
  for (size_t i = 0; i < k.size(); ++i)
    k[i] /= sum;

  std::vector<float> tmp((size_t)w * h, 0.0f);

  // horizontal pass, reflecting at x = 0 and x = w-1
  rows.run(plan, [&](int b) {
    for (int y = plan.y0(b); y < plan.y1(b); ++y) {
      const float *row = src + (size_t)y * w;
      for (int x = 0; x < w; ++x) {
        double acc = 0.0;
        for (int i = -r; i <= r; ++i) {
          int sx = x + i;
          if (sx < 0)
            sx = -sx;
          else if (sx >= w)
            sx = 2 * (w - 1) - sx;
          acc += k[(size_t)i + r] * row[clampi(sx, 0, w - 1)];
        }
        tmp[(size_t)y * w + x] = (float)acc;
      }
    }
  });

  // vertical pass
  rows.run(plan, [&](int b) {
    for (int y = plan.y0(b); y < plan.y1(b); ++y) {
      for (int x = 0; x < w; ++x) {
        double acc = 0.0;
        for (int i = -r; i <= r; ++i) {
          int sy = y + i;
          if (sy < 0)
            sy = -sy;
          else if (sy >= h)
            sy = 2 * (h - 1) - sy;
          acc += k[(size_t)i + r] * tmp[(size_t)clampi(sy, 0, h - 1) * w + x];
        }
        dst[(size_t)y * w + x] = (float)acc;
      }
    }
  });
}

/** @brief Half-size copy of a blurred image (box average of 2x2 blocks). */
void down2(const std::vector<float> &src, int w, int h, std::vector<float> &dst, int &dw, int &dh,
           const RowScheduler &rows)
{
  dw = std::max(1, w / 2);
  dh = std::max(1, h / 2);
  dst.assign((size_t)dw * dh, 0.0f);
  const BandPlan plan(dh);
  rows.run(plan, [&](int b) {
    for (int y = plan.y0(b); y < plan.y1(b); ++y) {
      for (int x = 0; x < dw; ++x) {
        const int x0 = 2 * x, y0 = 2 * y;
        const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
        const double a = src[(size_t)y0 * w + x0];
        const double b = src[(size_t)y0 * w + x1];
        const double c = src[(size_t)y1 * w + x0];
        const double d = src[(size_t)y1 * w + x1];
        dst[(size_t)y * dw + x] = (float)(0.25 * (a + b + c + d));
      }
    }
  });
}

/** @brief One level of the coarse-to-fine pyramid. */
struct Level {
  std::vector<float> a, b; // the two frames resampled to this level
  int w, h;
  int iters;     // Gauss-Seidel sweeps budgeted for this level
  FlowField f;   // flow known so far, in pixels of THIS level
  std::vector<float> du, dv; // increment being solved for, same units
};

} // namespace

////////////////////////////////////////////////////////////////////////////////
// public helpers

/** @brief Catmull-Rom weights for the taps at t-1, t, t+1, t+2 (t in [0,1)).
 *
 * Interpolating and C1, which is what a data term wants: it reproduces a linear
 * ramp exactly, unlike bilinear, whose interpolation error is proportional to the
 * local curvature. That error is not neutral here: warping with it attenuates the
 * high frequencies in proportion to the sub-pixel offset, and the attenuated
 * gradient then reads as *less* motion than the two frames actually contain -
 * a systematic sub-pixel bias on exactly the slow, smooth motion a retimer runs on.
 */
void cubicWeights(double t, double w[4])
{
  const double t2 = t * t, t3 = t2 * t;
  w[0] = -0.5 * t3 + t2 - 0.5 * t;
  w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
  w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
  w[3] = 0.5 * t3 - 0.5 * t2;
}

bool FlowField::finite() const
{
  for (size_t i = 0; i < u.size(); ++i) {
    if (!std::isfinite(u[i]) || !std::isfinite(v[i]))
      return false;
  }
  return true;
}

float sampleGray(const std::vector<float> &img, int w, int h, double x, double y)
{
  x = clampd(x, 0.0, (double)(w - 1));
  y = clampd(y, 0.0, (double)(h - 1));

  const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
  const double fx = x - x0, fy = y - y0;

  const double a = img[(size_t)y0 * w + x0];
  const double b = img[(size_t)y0 * w + x1];
  const double c = img[(size_t)y1 * w + x0];
  const double d = img[(size_t)y1 * w + x1];
  return (float)((a * (1.0 - fx) + b * fx) * (1.0 - fy) + (c * (1.0 - fx) + d * fx) * fy);
}

float sampleCubic(const std::vector<float> &img, int w, int h, double x, double y)
{
  x = clampd(x, 0.0, (double)(w - 1));
  y = clampd(y, 0.0, (double)(h - 1));

  const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  double wx[4], wy[4];
  cubicWeights(x - (double)x0, wx);
  cubicWeights(y - (double)y0, wy);

  // Taps clamped to the image: repeating the edge sample is the boundary condition
  // that neither wraps (aliasing at the frame edge) nor extrapolates (a ring that
  // the solver would read as false motion).
  int xs[4], ys[4];
  for (int k = 0; k < 4; ++k) {
    xs[k] = clampi(x0 + k - 1, 0, w - 1);
    ys[k] = clampi(y0 + k - 1, 0, h - 1);
  }

  double acc = 0.0;
  for (int j = 0; j < 4; ++j) {
    const float *row = img.data() + (size_t)ys[j] * w;
    const double r = wx[0] * row[xs[0]] + wx[1] * row[xs[1]] + wx[2] * row[xs[2]] + wx[3] * row[xs[3]];
    acc += wy[j] * r;
  }
  return (float)acc;
}

void medianFilterFlow(FlowField &f, int radius, const RowScheduler *rows)
{
  if (f.empty() || radius < 1)
    return;
  const int w = f.w, h = f.h;
  const int n = (2 * radius + 1) * (2 * radius + 1);
  FlowField src = f;
  const RowScheduler &sched = rows ? *rows : serialRowScheduler();
  const BandPlan plan(h);

  sched.run(plan, [&](int b) {
    std::vector<float> uu((size_t)n), vv((size_t)n); // per band: bands run concurrently
    for (int y = plan.y0(b); y < plan.y1(b); ++y) {
      for (int x = 0; x < w; ++x) {
        int c = 0;
        for (int dy = -radius; dy <= radius; ++dy) {
          for (int dx = -radius; dx <= radius; ++dx) {
            const int sx = clampi(x + dx, 0, w - 1), sy = clampi(y + dy, 0, h - 1);
            const size_t i = (size_t)sy * w + sx;
            uu[(size_t)c] = src.u[i];
            vv[(size_t)c] = src.v[i];
            ++c;
          }
        }
        std::nth_element(uu.begin(), uu.begin() + n / 2, uu.begin() + n);
        std::nth_element(vv.begin(), vv.begin() + n / 2, vv.begin() + n);
        const float mu = uu[(size_t)n / 2], mv = vv[(size_t)n / 2];
        // Only replace where the neighbourhood is actually varied: a median over a
        // uniform patch is that patch, and doing it everywhere would round the
        // sub-pixel flow the solver worked hard to find.
        const size_t i = (size_t)y * w + x;
        const float tol = 0.25f;
        if (std::fabs(src.u[i] - mu) > tol || std::fabs(src.v[i] - mv) > tol) {
          f.u[i] = mu;
          f.v[i] = mv;
        }
      }
    }
  });
}

void resampleFlow(const FlowField &src, FlowField &dst, int w, int h, double scale,
                  const RowScheduler *rows)
{
  dst.reset(w, h);
  if (src.empty() || w <= 0 || h <= 0)
    return;

  const double sx = (double)src.w / (double)w;
  const double sy = (double)src.h / (double)h;
  const RowScheduler &sched = rows ? *rows : serialRowScheduler();
  const BandPlan plan(h);

  sched.run(plan, [&](int b) {
    for (int y = plan.y0(b); y < plan.y1(b); ++y) {
      for (int x = 0; x < w; ++x) {
        const double fx = ((double)x + 0.5) * sx - 0.5;
        const double fy = ((double)y + 0.5) * sy - 0.5;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const int x1 = std::min(src.w - 1, x0 + 1), y1 = std::min(src.h - 1, y0 + 1);
        const double ax = fx - x0, ay = fy - y0;
        const int cx0 = clampi(x0, 0, src.w - 1), cy0 = clampi(y0, 0, src.h - 1);
        const int cx1 = clampi(x1, 0, src.w - 1), cy1 = clampi(y1, 0, src.h - 1);

        const size_t i00 = (size_t)cy0 * src.w + cx0, i01 = (size_t)cy0 * src.w + cx1;
        const size_t i10 = (size_t)cy1 * src.w + cx0, i11 = (size_t)cy1 * src.w + cx1;

        const double ux = (src.u[i00] * (1.0 - ax) + src.u[i01] * ax) * (1.0 - ay)
                        + (src.u[i10] * (1.0 - ax) + src.u[i11] * ax) * ay;
        const double vy = (src.v[i00] * (1.0 - ax) + src.v[i01] * ax) * (1.0 - ay)
                        + (src.v[i10] * (1.0 - ax) + src.v[i11] * ax) * ay;

        dst.u[(size_t)y * w + x] = (float)(ux * scale);
        dst.v[(size_t)y * w + x] = (float)(vy * scale);
      }
    }
  });
}

double meanEPE(const FlowField &got, const FlowField &ref, const unsigned char *mask)
{
  if (got.w != ref.w || got.h != ref.h)
    return -1.0;
  double sum = 0.0;
  long n = 0;
  for (size_t i = 0; i < got.u.size(); ++i) {
    if (mask && !mask[i])
      continue;
    const double du = got.u[i] - ref.u[i], dv = got.v[i] - ref.v[i];
    sum += std::sqrt(du * du + dv * dv);
    ++n;
  }
  return n ? sum / (double)n : 0.0;
}

namespace {

/** @brief Linearised data term and sweeps for one pyramid level.
 *
 * Energy minimised per level (in that level's pixel units, w the flow):
 *
 *   E(w) = S(w) + lambda * D(w)
 *
 *   S = sum |grad u| + |grad v|              (TV)
 *     | 0.5 * (|grad u|^2 + |grad v|^2)      (Tikhonov / Horn-Schunck)
 *   D = sum |B(x+w) - A(x)|                  (L1, occlusion tolerant)
 *     | 0.5 * sum |B(x+w) - A(x)|^2          (L2)
 *
 * The brightness constraint is linearised about the flow that is already known.
 * Write w = f + d, with f the current estimate (inherited from the coarser level,
 * then the running sum of accepted increments) and d the increment being solved
 * for, and let bw(x) = B(x + f) be the warped second frame. Then
 *
 *   r(d) = grad bw . d + (bw - A)
 *
 * so the constant term is the *current residual*. Using B - A instead, as the
 * textbook Horn-Schunck linearisation does, would count the inherited flow twice
 * and the iteration would chase its own tail.
 *
 * Each sweep is a red black Gauss-Seidel sweep for the increment:
 *  - the data term enters implicitly through its own diagonal: it is local and
 *    stiff, so an implicit step is both stable and fast;
 *  - the smoothness term is an explicit step of its gradient flow, with the TV
 *    weights p = 1/|grad w| lagged from the previous sweep.
 *
 * Smoothness has to stay explicit because its coefficient blows up where the flow
 * becomes locally uniform - that is exactly how TV enforces piecewise-constant
 * flow - and a uniform field has zero smoothness gradient, so an implicit
 * discretisation puts a huge diagonal on a mode that the neighbours do not
 * resist: the data term is then divided by that diagonal and convergence drops to
 * about (data weight)/(1/eps) per sweep, which is how this solver stalls at
 * 1e-5 px per sweep. Explicitly, the uniform mode is untouched by the smoothness
 * term (the differences cancel) and only the data term moves it.
 *
 * The cap pMax = 0.9 / (4 tauTV) keeps that explicit step contractive: four links
 * per pixel, so 4 tauTV pMax <= 0.9 < 1.
 *
 * After relinSweeps sweeps the increment is folded into f and the second frame is
 * re-warped: a Gauss-Newton iteration. Red black ordering means no pixel reads a
 * value the current sweep has already written, which makes the result independent
 * of the sweep order within a colour class and therefore reproducible - a retime
 * must not flicker from tile to tile. It is also what makes the row bands of D-31 exact
 * rather than approximate: the four neighbours of a pixel of colour c all have colour
 * 1-c, so a colour pass may be cut at any row boundary and everything it reads across
 * that boundary is a value no band is writing.
 */
struct LevelSolver {
  const FlowOptions &opt;
  const RowScheduler &rows;
  std::vector<float> gx, gy, resid, coef; // linearised data term
  std::vector<float> pU, pV;              // lagged, capped smoothness weights
  std::vector<float> bw;                  // second frame warped by the current flow
  std::vector<float> sw;                  // per-pixel weight of the data term, see dataTerm()
  int w, h;
  double levelScale; // fine pixels per level pixel
  double residScale; // normalisation applied by dataTerm(): 1 / contrast reference
  BandPlan plan;                        // this level's row bands: a function of h alone
  std::vector<double> slotA, slotB, slotC; // per-band slots of a pass that reduces

  explicit LevelSolver(const FlowOptions &o, const RowScheduler *rows_)
    : opt(o)
    , rows(rows_ ? *rows_ : serialRowScheduler())
    , w(0)
    , h(0)
    , levelScale(1.0)
    , residScale(1.0)
  {
  }

  void resize(int w_, int h_, double scale)
  {
    w = w_;
    h = h_;
    levelScale = scale;
    const size_t n = (size_t)w * h;
    gx.resize(n);
    gy.resize(n);
    resid.resize(n);
    coef.resize(n);
    pU.resize(n);
    pV.resize(n);
    bw.resize(n);
    sw.resize(n);
    plan = BandPlan(h);
    slotA.assign((size_t)plan.bands, 0.0);
    slotB.assign((size_t)plan.bands, 0.0);
    slotC.assign((size_t)plan.bands, 0.0);
  }

  static double totalU(const Level &lv, size_t i) { return (double)lv.f.u[i] + lv.du[i]; }
  static double totalV(const Level &lv, size_t i) { return (double)lv.f.v[i] + lv.dv[i]; }

  /** @brief Index range of @p band, for the passes that need no row structure. */
  size_t begin(int band) const { return (size_t)plan.y0(band) * w; }
  size_t end(int band) const { return (size_t)plan.y1(band) * w; }

  /** @brief Total of a per-band slot, summed in band index order.
   *
   * The order is the point (D-31). Floating-point addition is not associative, so a
   * reduction gathered by whichever thread finished first would depend on the thread
   * count; filling one slot per band and totalling the slots in band order makes the
   * same pass give the same bits whether it ran on one thread or on thirty-two.
   */
  double total(const std::vector<double> &slot) const
  {
    double sum = 0.0;
    for (int b = 0; b < plan.bands; ++b)
      sum += slot[(size_t)b];
    return sum;
  }

  /** @brief Warp the second frame by f and linearise the data term about it.
   *
   * Two band phases rather than one (D-31): the warp writes the rows of its own band,
   * but the gradient of a pixel reads the warped values of its four neighbours, so the
   * gradient phase can only start once every band has finished warping. That makes this
   * the one place in a level's work where a fused pass would have to be split - the
   * same shape as blurGray's two dispatches.
   */
  void dataTerm(const Level &lv)
  {
    if (opt.warpDataTerm) {
      // Two loops rather than a per-pixel branch: this warp runs once per
      // re-linearisation (every relinSweeps sweeps), so it is not the hot loop -
      // the sweeps are - which makes the ~4x cost of the cubic sample irrelevant
      // next to the accuracy it buys on sub-pixel motion.
      if (opt.cubicWarp) {
        rows.run(plan, [&](int b) {
          for (int y = plan.y0(b); y < plan.y1(b); ++y) {
            for (int x = 0; x < w; ++x) {
              const size_t i = (size_t)y * w + x;
              bw[i] = sampleCubic(lv.b, lv.w, lv.h, (double)x + lv.f.u[i], (double)y + lv.f.v[i]);
            }
          }
        });
      } else {
        rows.run(plan, [&](int b) {
          for (int y = plan.y0(b); y < plan.y1(b); ++y) {
            for (int x = 0; x < w; ++x) {
              const size_t i = (size_t)y * w + x;
              bw[i] = sampleGray(lv.b, lv.w, lv.h, (double)x + lv.f.u[i], (double)y + lv.f.v[i]);
            }
          }
        });
      }
    } else {
      rows.run(plan, [&](int b) {
        std::copy(lv.b.begin() + begin(b), lv.b.begin() + end(b), bw.begin() + begin(b));
      });
    }

    // Gradients of the warped frame, central where possible (one-sided only at the
    // border): forward differences carry a first-order bias proportional to the
    // local curvature, which shows up as a systematic sub-pixel error on slow
    // motion - exactly the case a retimer lives on.
    rows.run(plan, [&](int b) {
      double acc = 0.0;
      for (int y = plan.y0(b); y < plan.y1(b); ++y) {
        for (int x = 0; x < w; ++x) {
          const size_t i = (size_t)y * w + x;
          const float l = x > 0 ? bw[i - 1] : bw[i];
          const float r = hasRight(x, w) ? bw[i + 1] : bw[i];
          const float u = y > 0 ? bw[i - w] : bw[i];
          const float d = hasDown(y, h) ? bw[i + w] : bw[i];
          gx[i] = 0.5f * (r - l);
          gy[i] = 0.5f * (d - u);
          resid[i] = bw[i] - lv.a[i];
          acc += std::fabs(gx[i]) + std::fabs(gy[i]);
        }
      }
      slotA[(size_t)b] = acc;
    });
    const double n = (double)((size_t)w * h);
    const double meanGrad = total(slotA) / n;

    // Divide everything the data term sees by a contrast scale. An L1 data term
    // scales with image contrast, the smoothness term does not, so without this
    // step the same lambda would mean one thing for a 0..1 clip and another for
    // HDR footage - or between a textured shot and a low-contrast one. After the
    // division lambda is dimensionless: data and smoothing are compared in units of
    // "one mean gradient", which is what makes the defaults portable between hosts
    // and clips.
    //
    // Each level uses *its own* mean gradient, which is what lets a coarse level
    // work at all: after blur and downsampling its gradient is small in absolute
    // terms, yet the displacement it is allowed to describe is large in the same
    // units, so the residual and the gradient shrink together and the ratio - which
    // is what the solver actually reads - does not. Using the finest level's
    // contrast here instead (measured: the variant that normalises coarse levels by
    // contrast x levelScale) divides a coarse residual by a scale 2^l times too
    // big, the coarsest level stops moving at all, and large motion is lost from
    // the top down: EPE on a 46 px translation went from 3.3 px to 27 px.
    const double scale = meanGrad;
    residScale = scale > 1e-9 ? 1.0 / scale : 1.0;

    // Structure weight (FlowOptions::dataGradMin / dataGradBlur): how much of the data
    // term each pixel gets, normalised by the level's own mean of |grad bw|^2 so that
    // it redistributes lambda instead of changing its average - which is what makes it
    // comparable with the unweighted solver in the tuning harness, and what makes it a
    // trade-off knob rather than a strength knob.
    //
    // This is the Goldstein-style |grad|^2 weighting that an earlier round of tuning
    // rejected, and that rejection was an artifact of the scene it was measured on: a
    // periodic block texture, noise free. On an aperiodic scene with flat regions, and
    // with grain added, the weighting clearly wins - it is the coarse-motion and the
    // clean-region fix rather than a general accuracy gain. At lambda 0.05, on a
    // 640x360 aperiodic scene with grain 0.03: mean flow jitter over the flat 6% of
    // the frame 0.032 px -> 0.017 px, EPE on a 0.2 px translation 0.183 -> 0.185 px
    // (a wash), EPE on a 46 px translation 0.39 -> 0.48 px (slightly worse); on the
    // same scene at 320x180, where a 60 px translation is close to the pyramid's
    // detection limit, it is the difference between 2.3 px and 46 px. In other words:
    // it costs a little accuracy where the data is good, and it is what keeps large
    // motion and flat regions sane where the data is not.
    //
    // Reading the gradient off a blurred copy of bw (dataGradBlur) is the half that
    // does the work on grainy footage: at fixed lambda and pre-blur, blur 0 / 0.5 / 1 /
    // 1.5 / 2 px gives flat-region jitter 0.029 / 0.024 / 0.017 / 0.017 / 0.017 px and
    // sub-pixel EPE 0.203 / 0.197 / 0.188 / 0.186 / 0.185 px, while plate interiors of
    // a piecewise scene stay flat at 2.33 px EPE until 2 px of blur starts to cost
    // them. Hence 1 px.
    if (opt.dataGradMin > 0.0) {
      const double sigma = opt.dataGradBlur / levelScale; // in pixels of this level
      const float *gs = bw.data();
      std::vector<float> sm;
      if (sigma >= 0.25) {
        blurGray(bw.data(), sm, w, h, sigma, rows);
        gs = sm.data();
      }
      rows.run(plan, [&](int b) {
        double acc = 0.0;
        for (int y = plan.y0(b); y < plan.y1(b); ++y) {
          for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            const float l = x > 0 ? gs[i - 1] : gs[i];
            const float r = hasRight(x, w) ? gs[i + 1] : gs[i];
            const float u = y > 0 ? gs[i - w] : gs[i];
            const float d = hasDown(y, h) ? gs[i + w] : gs[i];
            const double a = 0.5 * ((double)r - (double)l);
            const double b2 = 0.5 * ((double)d - (double)u);
            sw[i] = (float)(a * a + b2 * b2);
            acc += (double)sw[i];
          }
        }
        slotB[(size_t)b] = acc;
      });
      const double meanG2 = total(slotB) / (double)((size_t)w * h);
      rows.run(plan, [&](int b) {
        for (size_t i = begin(b), e = end(b); i < e; ++i)
          sw[i] = (float)std::max(opt.dataGradMin,
                                  meanG2 > 1e-12 ? (double)sw[i] / meanG2 : 1.0);
      });
    } else {
      std::fill(sw.begin(), sw.end(), 1.0f);
    }

    // lambda is the one knob that trades the image against the smoothness term, and
    // the measured trade-off is not symmetric. Going from 0.05 to 0.13 (320x180,
    // grain 0.03, structure weighting on) improves the interior of a moving plate on a
    // piecewise scene from 2.33 to 1.29 px EPE, but costs sub-pixel EPE on uniform
    // motion 0.188 -> 0.258 px, flat-region flow jitter 0.018 -> 0.030 px, and a 46 px
    // translation 0.72 -> 1.94 px. What lambda does *not* change is the part that would
    // justify raising it: the measured plate EPE is mostly flow jitter inside a region
    // that is already moving correctly, and in the image domain - warping the second
    // frame by the estimated flow against warping it by the true one - the same change
    // is worth 0.0172 -> 0.0159 at a motion boundary, an 8% gain against a 40% loss
    // everywhere else. So 0.05, which is also where large motion stops being fragile.
    //
    // Under grain the same sweep shows what lambda actually controls: the mean flow it
    // settles on in flat regions for a true 0.2 px translation barely moves with lambda
    // (0.252 at 0.02, 0.250 at 0.05, 0.246 at 0.15), but the scatter around it scales
    // with it (0.123 -> 0.310 px). lambda amplifies noise; it does not create bias.
    const double eps = std::max(1e-6, opt.eps); // L1 relaxation, normalised units
    rows.run(plan, [&](int band) {
      for (size_t i = begin(band), e = end(band); i < e; ++i) {
        const double gxi = gx[i] * residScale, gyi = gy[i] * residScale;
        const double ri = resid[i] * residScale;
        gx[i] = (float)gxi;
        gy[i] = (float)gyi;
        resid[i] = (float)ri;

        double c = opt.lambda * (double)sw[i];
        if (opt.useL1Data)
          c /= std::fabs(ri) + eps; // IRLS weight of the L1 data term
        coef[i] = (float)c;
      }
    });

    // The *form* of this weight - not just its scale - was measured end to end over
    // the block scene, its piecewise variant and the aperiodic scene (320x180, grain
    // 0.03, 28 configurations). Huber and truncated-quadratic weights are clearly the
    // better treatment of noise: Huber with threshold 0.15 of the normalised residual
    // at lambda 0.05 improves sub-pixel EPE 0.188 -> 0.091 px, flat-region speckle
    // 0.0177 -> 0.0057 px and a 46 px translation 0.712 -> 0.254 px, which is a large
    // enough win to be worth taking if it were free. It is not. A residual threshold is
    // precisely what a motion boundary produces, so those weights switch the data term
    // off along every boundary, and the same configuration takes the interior of a plate
    // sliding over a still background from 2.44 to 9.03 px while dragging the background
    // from 1.50 to 7.48 px. The thresholds are also brittle in both directions: 0.05
    // fails a 46 px translation (31.4 px) and 0.50 fails a 60 px one (31.5 px). Plain L2
    // without IRLS is worse everywhere except plate interiors (0.87-1.07 px, and only at
    // lambda 1.5-4, i.e. by smoothing the discontinuity rather than by finding motion)
    // and costs sub-pixel accuracy 0.188 -> 0.24-0.47 px. A weight sharper than L1
    // (IRLS pow 1.5) is the best thing measured on the block scene's sub-pixel bias
    // (0.321 -> 0.295 px) and collapses large motion (39.1 px at 46 px). So the 1/|r|
    // weight of an L1 data term is the only form that does not fail one of the three,
    // and the noise it loses to Huber is what the structure weighting above is there to
    // absorb rather than the data term.
    //
    // Capping the structure weight at 1 / 2 / 4 / 8 was measured in the same battery:
    // it moves the sub-pixel numbers by well under 1%, and a cap of 1 costs a 46 px
    // translation 0.712 -> 15.7 px, because the cap removes exactly the high-contrast
    // pixels the weighting exists to trust. Uncapped.
  }

  /** @brief Step of the explicit smoothness flow, and the weight cap that keeps
   * it contractive over the four links of a pixel. */
  double tauTV() const { return clampd(opt.tauTV, 0.02, 0.22); }
  double pCap() const { return 0.9 / (4.0 * tauTV()); }

  /** @brief Lagged TV weights p = 1/|grad w| of the total flow f + d, capped;
   * the constant 1 for Tikhonov. */
  void smoothWeights(const Level &lv)
  {
    if (!opt.useTV) {
      std::fill(pU.begin(), pU.end(), 1.0f);
      std::fill(pV.begin(), pV.end(), 1.0f);
      return;
    }

    const double eps = std::max(1e-4, opt.eps);
    const double cap = pCap();
    const int stride = w;

    rows.run(plan, [&](int b) {
      for (int y = plan.y0(b); y < plan.y1(b); ++y) {
        for (int x = 0; x < w; ++x) {
          const size_t i = (size_t)y * stride + x;
          const double uT = totalU(lv, i), vT = totalV(lv, i);
          const double ux = (hasRight(x, w) ? totalU(lv, i + 1) : uT) - uT;
          const double uy = (hasDown(y, h) ? totalU(lv, i + stride) : uT) - uT;
          const double vx = (hasRight(x, w) ? totalV(lv, i + 1) : vT) - vT;
          const double vy = (hasDown(y, h) ? totalV(lv, i + stride) : vT) - vT;
          pU[i] = (float)std::min(1.0 / std::sqrt(ux * ux + uy * uy + eps * eps), cap);
          pV[i] = (float)std::min(1.0 / std::sqrt(vx * vx + vy * vy + eps * eps), cap);
        }
      }
    });
  }

  /** @brief One red black sweep of the increment. Returns the largest single
   * increment change, which is what the convergence test watches.
   *
   * One colour pass, cut at the band boundaries. Exact, not approximate: a pixel of
   * colour @p pass reads only the four neighbours of the other colour, and no band
   * writes a pixel of that colour during this dispatch - not one in its own rows, and
   * not one in the band next door. So the values a band reads across its boundary are
   * the values it would have read serially, and the sweep is bit-identical to the same
   * sweep run on one thread (D-31). The per-band maxima are combined afterwards, in
   * band order.
   */
  double sweep(Level &lv, int pass)
  {
    const double maxMotion = opt.maxMotion / levelScale; // bound on the total flow
    const double tau = tauTV();

    rows.run(plan, [&](int b) {
      double best = 0.0;
      for (int y = plan.y0(b); y < plan.y1(b); ++y) {
        for (int x = ((y & 1) == pass) ? 0 : 1; x < w; x += 2) {
          const size_t i = (size_t)y * w + x;
          const double gxv = gx[i], gyv = gy[i], r0 = resid[i], c = coef[i];
          double dU = lv.du[i], dV = lv.dv[i];
          const double uT = (double)lv.f.u[i] + dU, vT = (double)lv.f.v[i] + dV;

          // ---- u --------------------------------------------------------------
          double divU = 0.0; // div(p grad u) at this pixel; free boundary
          if (hasRight(x, w))
            divU += pU[i] * (totalU(lv, i + 1) - uT);
          if (x > 0)
            divU += pU[i - 1] * (totalU(lv, i - 1) - uT);
          if (hasDown(y, h))
            divU += pU[i] * (totalU(lv, i + w) - uT);
          if (y > 0)
            divU += pU[i - w] * (totalU(lv, i - w) - uT);

          dU = (dU - c * gxv * (gyv * dV + r0) + tau * divU) / (1.0 + c * gxv * gxv);

          // ---- v, symmetric, using the fresh dU: the 2x2 block is Gauss-Seidel
          //      within the pixel too
          double divV = 0.0;
          if (hasRight(x, w))
            divV += pV[i] * (totalV(lv, i + 1) - vT);
          if (x > 0)
            divV += pV[i - 1] * (totalV(lv, i - 1) - vT);
          if (hasDown(y, h))
            divV += pV[i] * (totalV(lv, i + w) - vT);
          if (y > 0)
            divV += pV[i - w] * (totalV(lv, i - w) - vT);

          dV = (dV - c * gyv * (gxv * dU + r0) + tau * divV) / (1.0 + c * gyv * gyv);

          if (!std::isfinite(dU) || !std::isfinite(dV)) {
            dU = 0.0;
            dV = 0.0;
          }
          // The clamp bounds the total flow, not the increment.
          const double uNew = clampd((double)lv.f.u[i] + dU, -maxMotion, maxMotion);
          const double vNew = clampd((double)lv.f.v[i] + dV, -maxMotion, maxMotion);
          best = std::max(best, std::fabs(dU - lv.du[i]));
          best = std::max(best, std::fabs(dV - lv.dv[i]));
          lv.du[i] = (float)(uNew - lv.f.u[i]);
          lv.dv[i] = (float)(vNew - lv.f.v[i]);
        }
      }
      slotA[(size_t)b] = best;
    });

    double maxUpd = 0.0;
    for (int b = 0; b < plan.bands; ++b)
      maxUpd = std::max(maxUpd, slotA[(size_t)b]);
    return maxUpd;
  }

  /** @brief Accept the increment into the flow and clear it for the next
   * re-linearisation. */
  void fold(Level &lv)
  {
    const double maxMotion = opt.maxMotion / levelScale;
    rows.run(plan, [&](int b) {
      for (size_t i = begin(b), e = end(b); i < e; ++i) {
        lv.f.u[i] = (float)clampd((double)lv.f.u[i] + lv.du[i], -maxMotion, maxMotion);
        lv.f.v[i] = (float)clampd((double)lv.f.v[i] + lv.dv[i], -maxMotion, maxMotion);
        lv.du[i] = 0.0f;
        lv.dv[i] = 0.0f;
      }
    });
  }

  /** @brief Energy at the flow f that dataTerm() linearised about, scored with the
   * true residual B(x+f) - A(x) rather than with its linearisation. The data part
   * is measured in units of the level's mean gradient (dataTerm normalises by it),
   * the smoothness part in flow units; a constant factor on the data part leaves
   * the minimiser untouched. Call it when the increment is empty, i.e. right after
   * dataTerm() or after fold(). */
  double energy(const Level &lv, double *residualOut)
  {
    rows.run(plan, [&](int b) {
      double eSmooth = 0.0, eData = 0.0, resSum = 0.0;
      const double wData = opt.lambda;

      for (int y = plan.y0(b); y < plan.y1(b); ++y) {
        for (int x = 0; x < w; ++x) {
          const size_t i = (size_t)y * w + x;
          const double uT = lv.f.u[i], vT = lv.f.v[i];
          const double ux = (hasRight(x, w) ? lv.f.u[i + 1] : uT) - uT;
          const double uy = (hasDown(y, h) ? lv.f.u[i + w] : uT) - uT;
          const double vx = (hasRight(x, w) ? lv.f.v[i + 1] : vT) - vT;
          const double vy = (hasDown(y, h) ? lv.f.v[i + w] : vT) - vT;

          if (opt.useTV)
            eSmooth += std::sqrt(ux * ux + uy * uy) + std::sqrt(vx * vx + vy * vy);
          else
            eSmooth += 0.5 * (ux * ux + uy * uy + vx * vx + vy * vy);

          eData += opt.useL1Data ? wData * (double)sw[i] * std::fabs(resid[i])
                                 : 0.5 * wData * (double)sw[i] * resid[i] * resid[i];
          resSum += std::fabs(resid[i]);
        }
      }
      slotA[(size_t)b] = eSmooth;
      slotB[(size_t)b] = eData;
      slotC[(size_t)b] = resSum;
    });

    if (residualOut)
      // back to image units: resid was divided by the mean gradient (dataTerm)
      *residualOut = total(slotC) / residScale / (double)((size_t)w * h);
    return total(slotA) + total(slotB);
  }
};

/** @brief Gauss-Newton iteration on one level: red black Gauss-Seidel sweeps for
 * the increment, re-warped and re-linearised every relinSweeps sweeps.
 *
 * Sweeps between re-linearisations are cheap and smooth out local detail;
 * re-linearising re-warps the frame, which is what moves the estimate across a
 * large displacement. Both are needed, and their ratio (relinSweeps) is the main
 * tuning knob of the solver.
 */
double solveLevel(Level &lv, LevelSolver &sol, const FlowOptions &opt, int *sweepsRun,
                  double *energyFirst, double *energyLast, double *residualLast)
{
  lv.du.assign((size_t)lv.w * lv.h, 0.0f);
  lv.dv.assign((size_t)lv.w * lv.h, 0.0f);

  const int inner = std::max(1, opt.relinSweeps);
  double maxUpd = 1e30, energyPrev = 1e30;
  int sweeps = 0;

  for (int outer = 0; sweeps < lv.iters; ++outer) {
    sol.dataTerm(lv);

    double res = 0.0;
    const double e = sol.energy(lv, &res);
    if (outer == 0 && energyFirst)
      *energyFirst = e;
    if (energyLast)
      *energyLast = e;
    if (residualLast)
      *residualLast = res;

    // Divergence guard, not a convergence test: a re-linearisation that made the
    // energy clearly worse means the increment overshot, so keep the flow
    // accepted so far and let the finer level refine it.
    if (outer > 1 && e > energyPrev * 1.05)
      break;
    energyPrev = e;

    for (int in = 0; in < inner && sweeps < lv.iters; ++in) {
      sol.smoothWeights(lv);
      // Sequenced on purpose. Written as std::max(sol.sweep(lv, 0), sol.sweep(lv, 1)) the
      // two calls are unsequenced relative to each other (C++11: the order of evaluation
      // of function arguments is unspecified), and both mutate lv.du / lv.dv - undefined
      // behaviour that happened to produce the same numbers while everything ran on one
      // thread. It is also the one place where the colour passes genuinely do depend on
      // each other: pass 1 reads what pass 0 wrote. Pass 0, then pass 1, always (D-31).
      const double updRed = sol.sweep(lv, 0);
      const double updBlack = sol.sweep(lv, 1);
      maxUpd = std::max(updRed, updBlack);
      ++sweeps;
    }
    sol.fold(lv);

    if (maxUpd < 1e-5 && outer > 0)
      break; // the increment has stopped moving: converged
  }

  if (sweepsRun)
    *sweepsRun = sweeps;
  return maxUpd;
}

} // namespace

void TVL1Estimator::estimate(const float *a, const float *b, int w, int h, FlowField &out,
                             FlowDiagnostics *diag) const
{
  FlowDiagnostics local;
  FlowDiagnostics *dg = diag ? diag : &local;

  if (w <= 0 || h <= 0 || !a || !b) {
    out.clear();
    return;
  }

  const FlowOptions &opt = _opt;

  // ---- build the pyramid (finest first) ------------------------------------
  //
  // Each level is the previous one blurred by gaussSigma and then halved, so the
  // finest level is the input untouched and the blur accumulates downwards in units of
  // finest-frame pixels: it costs sub-pixel accuracy on the level that actually has to
  // be accurate (measured: 0.5 -> 0.75 px moves nothing at the finest level, which is
  // why) and buys the coarse ones, where a displacement too large for the finest level
  // has to be found at all. That is what 0.75 rather than 0.5 is for: on a 320x180
  // scene a 60 px translation is at the pyramid's detection limit and sits at 2.3 px
  // EPE with 0.75 against 36 px with 0.5, while the same scene at 640x360 - where the
  // motion is no longer at the limit - differs by 4% in either direction. Beyond 1.0
  // the coarse levels start to lose the structure they were being found with.
  std::vector<Level> py((size_t)std::max(1, opt.levels));
  py[0].w = w;
  py[0].h = h;
  py[0].a.assign(a, a + (size_t)w * h);
  py[0].b.assign(b, b + (size_t)w * h);

  int nLevels = 1;
  for (int l = 1; l < (int)py.size(); ++l) {
    const Level &prev = py[(size_t)l - 1];
    if (prev.w < opt.minDim || prev.h < opt.minDim)
      break;
    std::vector<float> ba, bb;
    blurGray(prev.a.data(), ba, prev.w, prev.h, opt.gaussSigma, rows());
    blurGray(prev.b.data(), bb, prev.w, prev.h, opt.gaussSigma, rows());
    Level &lv = py[(size_t)l];
    down2(ba, prev.w, prev.h, lv.a, lv.w, lv.h, rows());
    down2(bb, prev.w, prev.h, lv.b, lv.w, lv.h, rows()); // recomputes the same dw/dh
    ++nLevels;
  }
  py.resize((size_t)nLevels);

  const int coarsest = nLevels - 1;
  py[(size_t)coarsest].f.reset(py[(size_t)coarsest].w, py[(size_t)coarsest].h);

  dg->levelsUsed = nLevels;

  LevelSolver sol(opt, &rows());
  bool first = true;

  // ---- coarse to fine ------------------------------------------------------
  for (int l = coarsest; l >= 0; --l) {
    Level &lv = py[(size_t)l];
    const double scale = (double)w / (double)lv.w; // fine pixels per level pixel
    lv.iters = (l >= coarsest - 1) ? opt.coarseIters : opt.iters;

    if (!first) {
      const Level &coarse = py[(size_t)l + 1];
      // flow is in pixels of its own level, so carrying it down multiplies by 2
      resampleFlow(coarse.f, lv.f, lv.w, lv.h, (double)lv.w / (double)coarse.w, &rows());
    } else {
      lv.f.reset(lv.w, lv.h);
    }

    sol.resize(lv.w, lv.h, scale);

    int sweeps = 0;
    double eFirst = 0.0, eLast = 0.0, resLast = 0.0;
    const double upd = solveLevel(lv, sol, opt, &sweeps, &eFirst, &eLast, &resLast);
    dg->iterations += sweeps;
    dg->maxUpdate = upd;

    if (l == 0) {
      // The diagnostics describe the finest level, because that is the flow the
      // plugin actually warps with. Energies across levels are not comparable:
      // each is measured in that level's own pixel units.
      dg->energyStart = eFirst;
      dg->energyEnd = eLast;
      dg->residualMean = resLast;
    }
    first = false;
  }

  // ---- result --------------------------------------------------------------
  out = py[0].f;
  if (opt.medianPost)
    medianFilterFlow(out, 1, &rows());

  if (!out.finite()) {
    // A runaway field must never reach a render: fall back to no motion, which
    // the processor turns into the Blend behaviour.
    out.reset(w, h);
  }
}

// The quality presets (D-33). Every row below is a measured point of the battery
// `./tests/run.sh sweep` prints (320x180, five analytic scenes, EPE over the *reachable*
// content only - the sweep's own columns, which exclude what a frame cannot contain).
// Two numbers behind the shape of the ladder, both from that table:
//
//   - `iters` is the only cost dial. iters 30/60/120/240/480 measure 192/334/629/1006/1803 ms
//     over the battery; coarseIters, relinSweeps, minDim and levels all move accuracy while
//     staying within a few percent of the same time, so a preset that wanted to be *cheaper*
//     had to move `iters` (or the grid the estimate runs on, which is motion.h's business).
//   - `lambda` is nearly free in time (629-639 ms) but it is not a free accuracy dial: it
//     trades the counter-motion case against large motion. plate 17.87/13.33/5.84/3.51/2.87 px
//     at lambda 0.02/0.05/0.10/0.20/0.40, while the 46 px aperiodic translation goes
//     21.27/0.023/0.102/0.439/0.425 px. No lambda is better on both, so lambda stays at its
//     measured 0.05 on every rung except the top one, where paying 0.42 px of large-motion
//     error is the documented point of the rung.
//
// Measured battery rows, EPE in px (trans 7 / trans 23 / aper 46 / sub-pixel / background /
// plate), total estimate ms, and the sweep count (which is deterministic, unlike the time):
//
//   Draft    it 40  rel 64  lam 0.05   0.053 0.156 0.068 0.010 0.037 14.05   241 ms  2656
//   Standard it 120 rel 64  lam 0.05   0.030 0.106 0.023 0.008 0.022 13.33   632 ms  4256
//   High     it 240 rel 64  lam 0.05   0.018 0.077 0.023 0.008 0.018 13.32  1013 ms  5104
//   Extreme  it 480 rel 128 lam 0.20   0.017 0.082 0.041 0.009 0.024  2.86  2207 ms 12648
//
// So High buys its 1.6x purely in sub-pixel accuracy (every column equal or better), and
// Extreme is the rung that fixes the one case the estimator structurally loses (D-24): the
// plate's 13.3 px becomes 2.9 px, and what it pays on decidable content is 0.018 px on the
// large translation - an amount no retime can show - plus 3.5x the time. Extreme's
// relinSweeps 128 is what keeps lambda 0.20 from losing the large motion as the budget grows:
// at the same iters and lambda, relin 256 measures 0.150 px on the 46 px translation against
// 0.041 (and relin 128 at the default lambda measures 0.021 against 0.023, so the change is
// specific to the stronger data term, which overshoots a re-warp more easily).
//
// No rung changes `minDim` or `levels`: minDim 16 or levels 4 stop the pyramid above the
// scale a 46 px translation has to be found at, and the battery says what that costs -
// 45.3 and 50.0 px. Draft's cheapness is `iters` alone here; on a big frame it also gets a
// coarser estimation grid from the plugin (motion.h's pixel budget), which is where a
// preview rung's speed actually comes from.
FlowOptions flowOptionsForQuality(int quality)
{
  FlowOptions o; // eQualityCustom, and anything out of range, is the measured default

  switch (quality) {
  case eQualityDraft:
    o.iters = 40; // 38% of Standard's time; worst reachable column 3x Standard's (aper 46 px)
    break;
  case eQualityStandard:
    break; // the defaults, which is what this rung *is* (checked in the accuracy suite)
  case eQualityHigh:
    o.iters = 240;
    break;
  case eQualityExtreme:
    o.iters = 480;
    o.relinSweeps = 128;
    o.lambda = 0.20;
    break;
  default:
    break;
  }
  return o;
}

bool sameFlowOptions(const FlowOptions &a, const FlowOptions &b)
{
  return a.levels == b.levels && a.coarseIters == b.coarseIters && a.iters == b.iters
      && a.relinSweeps == b.relinSweeps && a.lambda == b.lambda && a.eps == b.eps
      && a.maxMotion == b.maxMotion && a.gaussSigma == b.gaussSigma && a.tauTV == b.tauTV
      && a.useTV == b.useTV && a.useL1Data == b.useL1Data && a.warpDataTerm == b.warpDataTerm
      && a.cubicWarp == b.cubicWarp && a.medianPost == b.medianPost && a.minDim == b.minDim
      && a.dataGradMin == b.dataGradMin && a.dataGradBlur == b.dataGradBlur;
}

HornSchunckEstimator::HornSchunckEstimator()
{
  FlowOptions o;
  o.levels = 3;
  o.coarseIters = 120;
  o.iters = 60;
  o.lambda = 1.0;
  o.useTV = false;    // Tikhonov smoothness
  o.useL1Data = false; // quadratic data term
  o.medianPost = false;
  _opt = o;
}

std::string describeFlow(const FlowField &f)
{
  if (f.empty())
    return "empty flow field";
  double maxMag = 0.0, sum = 0.0;
  double minU = 1e30, maxU = -1e30, minV = 1e30, maxV = -1e30;
  for (size_t i = 0; i < f.u.size(); ++i) {
    const double m = std::sqrt((double)f.u[i] * f.u[i] + (double)f.v[i] * f.v[i]);
    maxMag = std::max(maxMag, m);
    sum += m;
    minU = std::min(minU, (double)f.u[i]);
    maxU = std::max(maxU, (double)f.u[i]);
    minV = std::min(minV, (double)f.v[i]);
    maxV = std::max(maxV, (double)f.v[i]);
  }
  char buf[256];
  std::snprintf(buf, sizeof buf, "%dx%d  mean |f| %.4f  max |f| %.4f  u [%.3f,%.3f]  v [%.3f,%.3f]",
                f.w, f.h, sum / (double)f.u.size(), maxMag, minU, maxU, minV, maxV);
  return std::string(buf);
}

} // namespace tw
