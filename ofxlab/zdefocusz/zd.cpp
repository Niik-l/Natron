// The host-free depth -> CoC model. See zd.h for the model itself and
// tests/zd_core_test.cpp for the numbers it is checked against.
#include "zd.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zd {

namespace {

const double kInf = std::numeric_limits<double>::infinity();

bool finite(double v) { return !std::isnan(v) && !std::isinf(v); }

double clampd(double v, double lo, double hi)
{
  if (std::isnan(v))
    return lo; // a NaN knob becomes the safe end of its own range
  if (v < lo)
    return lo;
  return v > hi ? hi : v;
}

// The CoC clamp, in one place: [minBlur, maxBlur] (sanitise() guarantees the order).
double clampCoC(double c, const FocusOptions &o)
{
  if (std::isnan(c))
    return o.minBlur;
  if (c < o.minBlur)
    return o.minBlur;
  return c > o.maxBlur ? o.maxBlur : c;
}

// Inside the in-focus slice? Slice thickness is measured in the same distance unit as
// focusValue, centred on it, so focusRange 0 is a zero-thickness slice (Nuke's note
// that "true theoretical DOF would set this to zero"). Blur mode measures it in the
// channel's own units instead, because there is no distance to measure in.
bool insideFocusSlice(double distance, double raw, const FocusOptions &o)
{
  if (!(o.focusRange > 0.0))
    return false;
  double half = 0.5 * o.focusRange;
  if (o.depthMode == eDepthBlur) {
    double v = o.depthInvert ? -raw : raw;
    return finite(v) && std::fabs(v - o.focusValue) <= half;
  }
  return finite(distance) && std::fabs(distance - o.focusValue) <= half;
}

} // namespace

void sanitise(FocusOptions &o)
{
  if (o.depthMode < 0 || o.depthMode >= kNumDepthModes)
    o.depthMode = eDepthDistance;
  if (o.blurFrom < 0 || o.blurFrom >= kNumBlurSources)
    o.blurFrom = eBlurFromSize;

  // `focusValue` is a distance in the depth channel's own unit, so its floor is the smallest
  // distance the model can mean: 0 is "no geometry" in the three distance modes. In Blur mode the
  // channel is a CoC map, and 0 *is* the focus plane; clamping it upward there gives every
  // in-focus pixel a 1e-9 px CoC, which is invisible in a picture but is not 0, so it would put
  // every pixel on the near side of the focal plane and defeat the identity fast path (a fully
  // in-focus depth map has to reproduce its input exactly). D-39.
  o.focusValue = clampd(o.focusValue, o.depthMode == eDepthBlur ? 0.0 : kDepthMin, kDepthMax);
  o.focusRange = clampd(o.focusRange, 0.0, kDepthMax);
  o.depthNear = clampd(o.depthNear, kDepthMin, kDepthMax);
  o.depthFar = clampd(o.depthFar, kDepthMin, kDepthMax);
  o.cocScale = clampd(o.cocScale, kCocScaleMin, kCocScaleMax);

  o.blurSize = clampd(o.blurSize, kCoCMin, kCoCMax);
  o.maxBlur = clampd(o.maxBlur, kCoCMin, kCoCMax);
  o.minBlur = clampd(o.minBlur, kCoCMin, kCoCMax);
  // minBlur above maxBlur would make the clamp unordered; the cap wins, because the cap
  // is also a kernel-size limit and the floor is only a taste.
  if (o.minBlur > o.maxBlur)
    o.minBlur = o.maxBlur;

  o.aperture = clampd(o.aperture, kApertureMin, kApertureMax);
  o.focalLength = clampd(o.focalLength, kFocalMin, kFocalMax);
  o.sensorWidth = clampd(o.sensorWidth, kSensorMin, kSensorMax);
  o.depthScale = clampd(o.depthScale, kDepthScaleMin, kDepthScaleMax);
}

double mappedDistance(double raw, const FocusOptions &o)
{
  double v = o.depthInvert ? -raw : raw;
  switch (o.depthMode) {
    case eDepthInverseDistance:
      // 1/0 is +inf, which is the right answer for a 1/z buffer with no geometry there.
      return 1.0 / v;
    case eDepthNormalized:
      // Linear through the two knobs, so either ordering of them gives either direction
      // of the channel; no clamping, because a value outside 0..1 is a distance outside
      // the range the user named, not necessarily an error.
      return o.depthNear + v * (o.depthFar - o.depthNear);
    case eDepthBlur:
    case eDepthDistance:
    default:
      return v;
  }
}

double defocusFactor(double distance, const FocusOptions &o)
{
  // "No geometry here": not a positive distance. Read as *far away*, which is the
  // conservative defocus (the far end of a depth map is what gets blurred), and it is
  // the same reading for a 0 sentinel and for a NaN.
  if (!(distance > 0.0))
    return 1.0;
  if (distance == kInf)
    return 1.0;
  return std::fabs(1.0 - o.focusValue / distance);
}

double resolveBlurSize(const FocusOptions &o, double imageWidthPx)
{
  if (o.blurFrom != eBlurFromLens)
    return o.blurSize > 0.0 ? o.blurSize : 0.0;

  // c(infinity) = f^2 / (N (u_f - f)) millimetres, from c(u) = A|v'-v|/v' with A = f/N,
  // 1/v' = 1/f - 1/u, 1/v = 1/f - 1/u_f:
  //   |v'-v| = v' v |1/u_f - 1/u|  =>  c(u) = A v |1/u_f - 1/u| = A v |u-u_f|/(u u_f)
  //   A v / u_f = (f/N)(u_f f/(u_f-f))/u_f = f^2 / (N (u_f - f))
  // so the coefficient of F is the CoC at infinite depth, and it is only a function of
  // where the lens is *focused*.
  double focusMm = o.focusValue * o.depthScale;
  if (!(focusMm > o.focalLength) || !finite(focusMm))
    return 0.0; // focused at or inside the lens: no finite CoC at infinity
  double cocMm = (o.focalLength * o.focalLength) / (o.aperture * (focusMm - o.focalLength));
  if (!(cocMm > 0.0) || !finite(cocMm))
    return 0.0;

  // mm -> px. OFX 1.4 has no kOfxImageEffectPropFormat anywhere (the vendored headers
  // define no image-format property), a region of interest is a *tile*, so no RoI width
  // is the image's width; and the pixel aspect ratio is only on clip/image instances and
  // the project, each of which a host may leave unset (Support's own
  // Clip::getPixelAspectRatio falls back to 1.0 for that reason). The one width that is
  // both the image's and independent of tiling is the Source clip's region of definition
  // (the Output clip's when nothing is connected), in canonical units: the plugin passes
  // that over the clip's reported aspect ratio, which is the frame's width in *device*
  // pixels, and a host reporting no aspect ratio means square pixels (D-34). Measured by
  // the offscreen probe on a 2.0-aspect plate, whose canonical RoD a host reports as
  // twice its stored width (see the `zpar2` group and tests/offscreen/analyse_zdefocus.py).
  if (!(imageWidthPx > 0.0) || !finite(imageWidthPx))
    return 0.0;
  double pxPerMm = imageWidthPx / o.sensorWidth;
  double px = cocMm * pxPerMm;
  if (!finite(px))
    return kCoCMax;
  return px > kCoCMax ? kCoCMax : px;
}

double cocRadius(double raw, const FocusOptions &o, double blurSizePx)
{
  if (o.depthMode == eDepthBlur) {
    // The channel already *is* a blur map: scale it, clamp it, no optics.
    double v = o.depthInvert ? -raw : raw;
    if (!finite(v))
      return clampCoC(o.minBlur, o);
    double dv = std::fabs(v - o.focusValue);
    if (insideFocusSlice(0.0, raw, o))
      return clampCoC(o.minBlur, o);
    return clampCoC(o.cocScale * dv, o);
  }

  double d = mappedDistance(raw, o);
  if (insideFocusSlice(d, raw, o))
    return clampCoC(o.minBlur, o); // Nuke's blur_dof: the slice is flat at minBlur

  double c = blurSizePx * defocusFactor(d, o);
  if (!finite(c))
    return o.maxBlur; // F ran off to infinity (an object at the focal point) or overflowed
  return clampCoC(c, o);
}

int autoLayerCount(double maxBlur, double layerSpacing)
{
  if (!(maxBlur > 0.0) || !(layerSpacing > 0.0))
    return kLayerMin; // nothing to blur, but a level set must still exist
  double spacing = clampd(layerSpacing, kLayerSpacingMin, kLayerSpacingMax);
  double n = std::ceil(maxBlur / spacing);
  if (!(n > 1.0))
    return kLayerMin;
  if (n > (double)kLayerMax)
    return kLayerMax;
  return (int)n;
}

LevelSet buildLevelSet(const FocusOptions &o, int layers, double layerSpacing, double layerCurve)
{
  FocusOptions s = o;
  sanitise(s);

  int n = layers;
  if (n <= 0)
    n = autoLayerCount(s.maxBlur, layerSpacing);
  if (n < 1)
    n = 1;
  if (n > kLayerMax)
    n = kLayerMax;

  double curve = clampd(layerCurve, kLayerCurveMin, kLayerCurveMax);

  LevelSet ls;
  ls.radius.push_back(0.0); // level 0: the in-focus pixels, convolved with nothing
  if (!(s.maxBlur > 0.0))
    return ls;

  ls.radius.reserve((size_t)n + 1);
  double prev = 0.0;
  for (int k = 1; k <= n; ++k) {
    double r = s.maxBlur * std::pow((double)k / (double)n, curve);
    // pow() is strictly increasing in k, but two levels can still land on the same float
    // when maxBlur/n is below the resolution of a double at that magnitude; a level set
    // with a repeated radius would have a zero-width seam, so nudge it upward.
    if (!(r > prev))
      r = prev + (s.maxBlur - prev) / (double)(n - k + 1);
    ls.radius.push_back(r);
    prev = r;
  }
  ls.radius[(size_t)n] = s.maxBlur; // the top level is the cap exactly
  return ls;
}

void LevelSet::bracket(double coc, int *i0, int *i1, double *t) const
{
  *i0 = *i1 = 0;
  *t = 0.0;
  int n = count();
  if (n == 0)
    return;
  if (!(coc > 0.0))
    return; // the sharp level, and nothing else

  // Largest i with radius[i] <= coc (radius is ascending, radius[0] == 0 <= coc).
  int lo = 0, hi = n - 1;
  while (hi - lo > 1) {
    int mid = lo + (hi - lo) / 2;
    if (radius[(size_t)mid] <= coc)
      lo = mid;
    else
      hi = mid;
  }
  if (coc >= radius[(size_t)hi]) { // coc at or above the top level
    *i0 = *i1 = hi;
    return;
  }
  if (coc == radius[(size_t)lo]) { // exactly on a level: that level only, no seam
    *i0 = *i1 = lo;
    return;
  }
  *i0 = lo;
  *i1 = hi;
  double span = radius[(size_t)hi] - radius[(size_t)lo];
  *t = span > 0.0 ? (coc - radius[(size_t)lo]) / span : 0.0;
}

double LevelSet::spacingAt(double coc) const
{
  int i0, i1;
  double t;
  bracket(coc, &i0, &i1, &t);
  if (i1 <= i0)
    return 0.0;
  return radius[(size_t)i1] - radius[(size_t)i0];
}

double pixelHash(int x, int y)
{
  // The 32-bit integer mix from lowbias32 (Chris Wellons): a few multiply/xorshift rounds
  // that spread every bit of the input over the output. Functions of (x, y) alone, so a
  // pixel gets the same value whatever band it is rendered in.
  unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (double)h * (1.0 / 4294967296.0);
}

double ditheredCoC(double coc, int x, int y, const LevelSet &levels, bool enabled)
{
  if (!enabled || !(coc > 0.0) || levels.empty())
    return coc; // an in-focus pixel is never dithered *into* a blur

  double amp = levels.spacingAt(coc); // the width of the seam it sits in
  if (!(amp > 0.0))
    return coc;
  double v = coc + (pixelHash(x, y) - 0.5) * amp; // +- half a step, locally
  if (v < 0.0)
    v = 0.0;
  double top = levels.radius[(size_t)levels.count() - 1];
  return v > top ? top : v;
}

} // namespace zd
