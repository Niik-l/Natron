// ZDefocusZ: the bokeh kernel library — the maths behind bokeh.h (DECISIONS.md D-36).
//
// Two things happen here:
//
//   1. An aperture is defined as a *transmission field* T over the plane (0 blocked, 1
//      open, innerBrightness brighter) as a function of unit-aperture polar coordinates.
//      Both the sprite builder and the reference quadrature read that one definition, so
//      checking a sprite's mass against apertureArea() is a check that the discretisation
//      is faithful — not a check that two implementations agree with each other.
//
//   2. That field becomes a stencil by integrating it over each cell of the plane the
//      renderer will gather from. A cell wholly inside or wholly outside the aperture is
//      decided analytically (exact, and cheap); only a cell the boundary crosses is
//      measured on a fixed subgrid. That is what makes a 3000 px kernel affordable, and it
//      is why the taps of a hard-edged aperture sum to the aperture's area.
#include "bokeh.h"

#include <algorithm>
#include <cmath>
#include <list>

namespace zd {

namespace {

const double kPi = 3.14159265358979323846;

// Quadrature of the aperture in unit coordinates. The boundary r(theta) is smooth and
// periodic — cos(pi/blades)/cos(a) with |a| < pi/blades <= pi/3, so the denominator never
// drops below 1/2 — hence midpoint sampling in theta converges fast (measured 1e-6 against
// the analytic polygon area). The radial integral is done in closed form instead: the
// transmission profile is piecewise constant with one smoothstep band, and sampling it on a
// uniform radial grid instead would leave an O(1/radii) error at *every hard edge* — a
// catadioptric hole at 0.95 lost 1.5% of the mass that way, which is larger than the
// discretisation error the reference is meant to measure.
const int kQuadAngles = 2048;

// A cell the aperture boundary crosses is measured on a subgrid. What decides the sample
// count is *which kind of field* the cell holds, because the two kinds converge at
// different rates and are not equally numerous:
//
//   - a cell carrying a discontinuity is O(perimeter / cell) of them and a midpoint average
//     converges as 1/n — the error is a fixed fraction of the subcell width at the edge;
//   - a cell inside the feather band or under the Gaussian is O(area / cell²) of them and
//     converges as 1/n² — its error is already tiny for a small grid.
//
// Using one grid generous enough for the edges is the mistake the split avoids: a uniform
// n = 32 made a 1024 px bladed sprite sample its feather band (10% of the aperture *area*)
// 20 times over for no gain in edge accuracy (26 s for a kernel that costs 4 s). The extra
// per-unit term follows the cell's own footprint in unit-aperture fractions, so a coarse
// cell — which can hold a whole blade corner, or an aperture squeezed several cell-widths
// wide — is refined while a fine cell keeps its nearly-straight edge cheap.
const int kSubSmooth = 8;  // continuous field inside the cell
const int kSubJump = 24;   // the cell straddles an edge of the field
const double kSubPerUnit = 48.0; // added when a cell spans a large share of the aperture

bool isFin(double v) { return std::isfinite(v); }

double clampTo(double v, double lo, double hi)
{
  if (!isFin(v))
    return lo; // a NaN knob becomes the low end of its range (zd.cpp's rule, D-34)
  return v < lo ? lo : (v > hi ? hi : v);
}

double clamp01(double v) { return clampTo(v, 0.0, 1.0); }

double clampAspect(double v)
{
  if (!isFin(v) || v <= 0.0)
    return 1.0; // 0 or negative is not an aspect ratio; a NaN is the round aperture
  return v < kAspectMin ? kAspectMin : (v > kAspectMax ? kAspectMax : v);
}

double smoothstep01(double t)
{
  double x = clamp01(t);
  return x * x * (3.0 - 2.0 * x);
}

// ∫₀^u smoothstep(t) dt and ∫₀^u t·smoothstep(t) dt, u in [0,1]: the two antiderivatives
// the feather band needs (see Aperture::radialMass).
double intSmoothstep(double u)
{
  return u * u * u * (1.0 - 0.5 * u);
}

double intTSmoothstep(double u)
{
  return u * u * u * u * (0.75 - 0.4 * u);
}

/** @brief The aperture and its transmission field, for one @c radius.
 *
 * Everything is expressed in *unit-aperture fractions*: a point of the source plane at
 * pixel offset (px, py) from the destination maps to unit coordinates through the inverse
 * of the aperture's own transform (rotate by -rotation, then x /= aspect and y *= aspect),
 * scaled by 1/radius. The aperture's boundary is then r(theta) <= 1, so the same field
 * serves a 3 px kernel and a 3000 px one. The transform's determinant is @c radius *
 * radius: `aspect` squeezes one axis and stretches the other, which is why area (and
 * therefore a normalised kernel) does not depend on it, and why a kernel's window is
 * measured by the extent the aperture reaches (see @ref kernelSpan), not by @c radius. */
struct Aperture {
  int blades;
  bool bladed; // only a bladed aperture has a polygon boundary; a disc is r(theta) = 1
  double roundness, discShape, innerSize, innerFeather, innerBrightness;
  double hole; // 0 when there is no central obstruction
  double aspect, cosr, sinr, radius;
  double sq; // max(aspect, 1/aspect): how far the unit mapping stretches the coarser axis
  bool hasInner, hasHole;
  bool innerStep;       // an inner edge with no ramp at all, i.e. feather exactly 0.5
  double bmin;          // inscribed radius: min over theta of the boundary
  double gaussNorm;     // makes the truncated Gaussian part unit-mass over the aperture
  double innerLoMin, innerHiMax; // conservative bounds of the feather band, all radii

  Aperture(const BokehOptions &o, double radius_, double gaussNorm_)
    : blades(o.blades), bladed(o.shape == eShapeBladed), roundness(o.roundness), discShape(o.discShape),
      innerSize(o.innerSize), innerFeather(o.innerFeather),
      innerBrightness(o.innerBrightness), hole(o.catadioptric ? o.catadioptricSize : 0.0),
      aspect(o.aspect), radius(radius_), sq(1.0)
  {
    if (aspect > 1.0)
      sq = aspect;
    else
      sq = 1.0 / aspect; // sanitise() guarantees aspect > 0
    double phi = o.rotation * kPi / 180.0;
    cosr = std::cos(phi);
    sinr = std::sin(phi);
    // Every shape owns its own knobs, exactly as Nuke gates them (references/zdefocus/:
    // inner_*, catadioptric* are "available for: bladed"; shape is "available for: disc").
    // Gated here rather than in sanitise so BokehOptions keeps what the user actually set
    // (and a knob that does not apply to the shape cannot change a kernel).
    hasInner = bladed && innerSize > 0.0;
    hasHole = bladed && hole > 0.0;
    innerStep = hasInner && innerFeather == 0.5; // innerBand() has zero width: a step, not a ramp
    if (!bladed) {
      innerSize = 0.0;
      innerBrightness = 1.0;
      roundness = 1.0;
    }
    if (o.shape != eShapeDisc)
      discShape = 1.0; // only a disc dissolves toward a Gaussian; everything else is hard
    bmin = bladed ? (1.0 - roundness) * std::cos(kPi / (double)blades) + roundness : 1.0;
    gaussNorm = gaussNorm_;

    double riMin = innerSize * bmin, riMax = innerSize;
    if (!hasInner) {
      innerLoMin = 0.0;
      innerHiMax = 0.0;
    } else if (innerFeather > 0.5) {
      innerLoMin = riMin;
      innerHiMax = riMax + (2.0 * innerFeather - 1.0) * (1.0 - riMax);
    } else {
      innerLoMin = 2.0 * innerFeather * riMin;
      innerHiMax = riMax;
    }
  }

  void toUnit(double px, double py, double *ux, double *uy) const
  {
    double rx = px * cosr + py * sinr;  // rotate by -rotation
    double ry = -px * sinr + py * cosr;
    *ux = rx / (aspect * radius); // x reaches aspect * radius, y reaches radius / aspect
    *uy = ry * aspect / radius;   // (aspect < 1 squeezes x, > 1 squeezes y: Nuke's `aspect`)
  }

  double boundaryAtAngle(double theta) const
  {
    if (!bladed || blades < 3)
      return 1.0;
    double seg = 2.0 * kPi / (double)blades;
    double a = std::fmod(theta, seg);
    if (a < 0.0)
      a += seg;
    a -= seg * 0.5; // angle from the normal of the side this wedge opens onto
    double poly = std::cos(kPi / (double)blades) / std::cos(a);
    return (1.0 - roundness) * poly + roundness;
  }

  double boundaryAt(double ux, double uy) const
  {
    return boundaryAtAngle(std::atan2(uy, ux));
  }

  /** @brief The feather band's radial extent at boundary radius @p b: [innerLo(b),
   * innerHi(b)]. Kept as one place so @ref hardTransmission and @ref radialMass cannot
   * disagree about where the ramp sits. */
  void innerBand(double b, double *lo, double *hi) const
  {
    double ri = innerSize * b;
    if (innerFeather > 0.5) {
      *lo = ri; // band: [ri, ri + w], reaching the outer boundary exactly at feather 1
      *hi = ri + (2.0 * innerFeather - 1.0) * (1.0 - ri);
    } else {
      *hi = ri;
      *lo = ri - (1.0 - 2.0 * innerFeather) * ri; // band: [ri - w, ri]
    }
    if (*hi < *lo)
      *hi = *lo;
  }

  /** @brief The hard-edged profile: the inner polygon's brightness with its feather band,
   * and the catadioptric hole punched through whatever it covers. */
  double hardTransmission(double rr, double b) const
  {
    if (hasHole && rr <= hole)
      return 0.0;
    if (!hasInner)
      return 1.0;
    double lo, hi;
    innerBand(b, &lo, &hi);
    double w = hi - lo;
    if (!(w > 0.0))
      return rr <= lo ? innerBrightness : 1.0;
    return innerBrightness + smoothstep01((rr - lo) / w) * (1.0 - innerBrightness);
  }

  /** @brief ∫₀^b T(r)·r dr in closed form for the hard-edged profile: constant pieces plus
   * one smoothstep band, all clipped to [0, b] and to the punched-out hole. Analytic on
   * purpose (see the kQuadAngles note): a sampled radial integral leaves an error at each
   * hard edge, and this is the reference the sprite masses are judged against. */
  double radialMass(double b) const
  {
    double c = hasHole ? (hole < b ? hole : b) : 0.0; // the obstruction, clipped
    if (!(b > c))
      return 0.0; // the hole is the whole aperture
    if (!hasInner)
      return 0.5 * (b * b - c * c);
    double lo, hi;
    innerBand(b, &lo, &hi);
    double m = 0.0;
    double q = lo < b ? lo : b; // [c, lo): the inner polygon's own brightness
    if (q > c)
      m += innerBrightness * 0.5 * (q * q - c * c);
    if (hi > lo) { // the feather band, clipped to [c, b]
      double p = lo > c ? lo : c, r = hi < b ? hi : b;
      if (r > p) {
        double w = hi - lo;
        double u1 = (p - lo) / w, u2 = (r - lo) / w;
        m += innerBrightness * 0.5 * (r * r - p * p) +
             (1.0 - innerBrightness) * (w * lo * (intSmoothstep(u2) - intSmoothstep(u1)) +
                                        w * w * (intTSmoothstep(u2) - intTSmoothstep(u1)));
      }
    }
    double p = hi > c ? hi : c; // (hi, b]: fully open
    if (b > p)
      m += 0.5 * (b * b - p * p);
    return m;
  }

  /** @brief The field at one unit coordinate. */
  double transmission(double ux, double uy) const
  {
    double rr = std::sqrt(ux * ux + uy * uy);
    double b = boundaryAt(ux, uy);
    if (rr > b)
      return 0.0;
    double hard = hardTransmission(rr, b);
    if (discShape >= 1.0)
      return hard;
    double g = gaussNorm * std::exp(-4.0 * rr * rr);
    if (discShape <= 0.0)
      return g;
    return (1.0 - discShape) * g + discShape * hard;
  }

  /** @brief Area-average of the field over the source cell centred at (cx, cy) px: a
   * transmission in [0, innerBrightnessMax]. A kernel's taps are exactly these averages,
   * so the sum of the taps is the aperture's area in cell units. */
  double cellAverage(double cx, double cy, double cell) const
  {
    double h = 0.5 * cell;
    double px4[4] = {cx - h, cx + h, cx + h, cx - h};
    double py4[4] = {cy - h, cy - h, cy + h, cy + h};
    double ux[4], uy[4];
    double maxrr = 0.0, minrr = 1e300;
    double bLo = 1e300, bHi = 0.0; // the boundary radius at this cell's own corner angles
    for (int i = 0; i < 4; ++i) {
      toUnit(px4[i], py4[i], &ux[i], &uy[i]);
      double d = std::sqrt(ux[i] * ux[i] + uy[i] * uy[i]);
      if (d > maxrr)
        maxrr = d;
      double b = boundaryAt(ux[i], uy[i]);
      if (b < bLo)
        bLo = b;
      if (b > bHi)
        bHi = b;
    }
    for (int i = 0; i < 4; ++i) { // closest approach of the mapped cell to the origin:
      int j = (i + 1) & 3;        // it can be on an edge, not only at a corner
      double ex = ux[j] - ux[i], ey = uy[j] - uy[i];
      double len2 = ex * ex + ey * ey;
      double t = len2 > 0.0 ? -(ux[i] * ex + uy[i] * ey) / len2 : 0.0;
      t = clamp01(t);
      double qx = ux[i] + t * ex, qy = uy[i] + t * ey;
      double d = std::sqrt(qx * qx + qy * qy);
      if (d < minrr)
        minrr = d;
    }
    // A cell that *contains* the aperture's centre has no closest approach: the edge
    // distances above describe the origin leaving the cell, which it does not, and taking
    // them would report the cell holding the whole aperture as "outside" (r=1 px gathered
    // at cell=2 collapsed to a single tap because of exactly this).
    if (std::fabs(cx) <= h && std::fabs(cy) <= h)
      minrr = 0.0;

    if (minrr >= 1.0)
      return 0.0; // wholly outside: no boundary reaches past 1

    if (maxrr <= bmin) { // wholly inside the aperture: decided, not sampled
      bool constant = false;
      double v = 0.0;
      if (hasHole && maxrr <= hole) {
        constant = true;
        v = 0.0;
      } else if (!hasHole || minrr >= hole) {
        if (!hasInner) {
          constant = true;
          v = 1.0;
        } else if (maxrr <= innerLoMin) {
          constant = true;
          v = innerBrightness;
        } else if (minrr >= innerHiMax) {
          constant = true;
          v = 1.0;
        }
      }
      if (constant && (discShape >= 1.0 || cell * sq <= radius / 32.0)) {
        if (discShape >= 1.0)
          return v;
        // A Gaussian varies across the cell, but a mapped cell this small next to the
        // profile's own scale makes the midpoint exact to ~1e-5 of the mass (measured at
        // cell 1), and a subgrid average of a fine cell is what a *coarse* cell needs, not
        // a fine one. A coarse cell falls through to the subgrid instead, which is what
        // holds a Gaussian kernel's mass error at cell = radius / 4 to ~1e-3.
        double mx, my;
        toUnit(cx, cy, &mx, &my);
        return (1.0 - discShape) * gaussNorm * std::exp(-4.0 * (mx * mx + my * my)) + discShape * v;
      }
    }

    // The subgrid is anchored to the cell (its own centre and pitch), never to the tap's
    // position in the kernel, so two bands asking for the same kernel get the same taps.
    double scale = cell * sq / radius; // this cell's width in unit-aperture fractions
    // Is the field discontinuous *inside this cell*? The aperture's edge crosses it when the
    // corner radii straddle the boundary radius at those same corners — using the global
    // inscribed radius `bmin` instead would flag the whole shell between the apothem and the
    // vertices as edge cells (46% of a pentagon's area, and 26 s for a kernel that costs 4).
    bool jump = maxrr > bLo && minrr < bHi;
    if (!jump && hasHole)
      jump = minrr < hole && maxrr > hole; // the punched-out centre
    if (!jump && hasInner && minrr < innerHiMax && maxrr > innerLoMin &&
        (innerStep || innerHiMax - innerLoMin <= scale))
      jump = true; // a feather band no wider than the cell is a step, not a ramp
    int n = (jump ? kSubJump : kSubSmooth) + (int)std::ceil(kSubPerUnit * scale);
    double sum = 0.0;
    const double step = cell / (double)n;
    for (int j = 0; j < n; ++j) {
      double py = cy - h + (j + 0.5) * step;
      for (int i = 0; i < n; ++i) {
        double qx, qy;
        toUnit(cx - h + (i + 0.5) * step, py, &qx, &qy);
        sum += transmission(qx, qy);
      }
    }
    return sum / ((double)n * (double)n);
  }
};

/** @brief The aperture's mass and the Gaussian's normalising constant, in unit-aperture
 * area fractions, by quadrature over the same boundary the sprites sample. The Gaussian
 * is truncated at the aperture boundary on purpose: a kernel has a finite support, so a
 * profile with infinite support would leak mass past the span and the mass identity
 * (sum of taps == aperture area in cell units) would not hold. */
void apertureQuadrature(const BokehOptions &o, double *mass, double *gaussNorm)
{
  *mass = 0.0;
  *gaussNorm = 0.0;
  if (o.shape == eShapeImage)
    return; // the picture defines the shape; there is no analytic aperture to integrate

  // radius/aspect/rotation do not enter a unit-frame integral, and hardTransmission and
  // boundaryAtAngle do not read them, so a unit aperture is enough to integrate over.
  Aperture proto(o, 1.0, 1.0);
  double dtheta = 2.0 * kPi / (double)kQuadAngles;
  double gint = 0.0;
  for (int a = 0; a < kQuadAngles; ++a) {
    double theta = (a + 0.5) * dtheta;
    double b = proto.boundaryAtAngle(theta);
    gint += (1.0 - std::exp(-4.0 * b * b)) * 0.125 * dtheta; // analytic in radius
  }
  if (gint > 0.0)
    *gaussNorm = 1.0 / gint;

  double hard = 0.0;
  for (int a = 0; a < kQuadAngles; ++a) {
    double theta = (a + 0.5) * dtheta;
    hard += proto.radialMass(proto.boundaryAtAngle(theta)) * dtheta; // analytic in radius
  }
  *mass = (1.0 - proto.discShape) * 1.0 + proto.discShape * hard; // Gaussian half is unit-mass by construction
}

/** @brief Bilinear sample of a plane at fractional pixel coordinates, clamped at the
 * picture's own border: a kernel whose aperture is wider than the bokeh picture keeps the
 * border value out there instead of developing a dark rim of its own. */
double samplePlane(const PlaneImage &p, double x, double y, int ch)
{
  if (p.width <= 0 || p.height <= 0 || ch < 0 || ch >= p.channels)
    return 0.0;
  if (!(x > 0.0))
    x = 0.0;
  if (x > (double)(p.width - 1))
    x = (double)(p.width - 1);
  if (!(y > 0.0))
    y = 0.0;
  if (y > (double)(p.height - 1))
    y = (double)(p.height - 1);
  int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  int x1 = x0 + 1 < p.width ? x0 + 1 : x0;
  int y1 = y0 + 1 < p.height ? y0 + 1 : y0;
  double fx = x - (double)x0, fy = y - (double)y0;
  const float *r0 = p.at(x0, y0), *r1 = p.at(x1, y0), *s0 = p.at(x0, y1), *s1 = p.at(x1, y1);
  double a = r0[ch] + fx * (r1[ch] - r0[ch]);
  double b = s0[ch] + fx * (s1[ch] - s0[ch]);
  return a + fy * (b - a);
}

} // namespace

// ---------------------------------------------------------------------------
// Options, sanitisation, ranges
// ---------------------------------------------------------------------------

bool BokehOptions::operator==(const BokehOptions &o) const
{
  return shape == o.shape && source == o.source && catadioptric == o.catadioptric &&
         blades == o.blades && discShape == o.discShape && aspect == o.aspect &&
         rotation == o.rotation && roundness == o.roundness && innerSize == o.innerSize &&
         innerFeather == o.innerFeather && innerBrightness == o.innerBrightness &&
         catadioptricSize == o.catadioptricSize;
}

void sanitise(BokehOptions &o)
{
  if (o.shape < 0 || o.shape >= kNumShapes)
    o.shape = 0;
  if (o.source < 0 || o.source >= kNumBokehSources)
    o.source = 0;
  o.discShape = clampTo(o.discShape, kShapeMin, kShapeMax);
  o.aspect = clampAspect(o.aspect);
  o.rotation = clampTo(o.rotation, kRotationMin, kRotationMax);
  if (!(o.blades >= kBladesMin))
    o.blades = kBladesMin; // 2 blades is not a polygon
  if (o.blades > kBladesMax)
    o.blades = kBladesMax;
  o.roundness = clampTo(o.roundness, kRoundMin, kRoundMax);
  o.innerSize = clampTo(o.innerSize, kInnerSizeMin, kInnerSizeMax);
  o.innerFeather = clampTo(o.innerFeather, kInnerFeatherMin, kInnerFeatherMax);
  o.innerBrightness = clampTo(o.innerBrightness, kInnerBrightnessMin, kInnerBrightnessMax);
  o.catadioptricSize = clampTo(o.catadioptricSize, kHoleMin, kHoleMax);
  // innerSize 0 means there is no inner polygon, so innerFeather and innerBrightness are
  // simply never read (Aperture::hasInner) rather than having to be reconciled here.
}

// ---------------------------------------------------------------------------
// Kernel
// ---------------------------------------------------------------------------

double Kernel::sum(int p) const
{
  if (p < 0 || p >= planes)
    return 0.0;
  double s = 0.0;
  size_t n = (size_t)span * (size_t)span;
  const float *t = plane(p);
  for (size_t i = 0; i < n; ++i)
    s += (double)t[i];
  return s;
}

bool Kernel::operator==(const Kernel &k) const
{
  return span == k.span && planes == k.planes && radius == k.radius && cell == k.cell &&
         taps == k.taps && mass == k.mass;
}

// ---------------------------------------------------------------------------
// Shape maths
// ---------------------------------------------------------------------------

double apertureBoundary(int blades, double roundness, double theta)
{
  int n = blades < kBladesMin ? kBladesMin : blades;
  double seg = 2.0 * kPi / (double)n;
  double a = std::fmod(theta, seg);
  if (a < 0.0)
    a += seg;
  a -= seg * 0.5;
  double poly = std::cos(kPi / (double)n) / std::cos(a);
  double k = clamp01(roundness);
  return (1.0 - k) * poly + k;
}

double apertureArea(const BokehOptions &o)
{
  BokehOptions c = o;
  sanitise(c);
  double mass = 0.0, gnorm = 0.0;
  apertureQuadrature(c, &mass, &gnorm);
  return mass; // in units of radius^2: `aspect` redistributes the extent, not the area
}

int kernelSpan(double radius, double cell, double aspect)
{
  if (!isFin(radius) || !(radius > 0.0))
    return 1;
  double c = (!isFin(cell) || cell < kCellMin) ? kCellMin : cell;
  double a = clampAspect(aspect);
  // The window has to reach the aperture's own extent, which aspect splits between the two
  // axes: x reaches aspect * radius, y reaches radius / aspect. Getting this wrong does not
  // crash anything, it silently truncates the kernel and its mass (an aspect-2 aperture
  // measured with a span based on radius alone lost 39% of its area).
  double reach = radius * (a > 1.0 ? a : 1.0 / a);
  long long half = (long long)std::ceil(reach / c);
  // The cap below is unreachable for a caller that honours @ref coarsestCell, whose cell is a fixed
  // number of taps across the aperture (14 the long axis, 4 the short): the half-span is then bounded
  // by those taps plus the flooring of a cell to a whole pixel - the coarsest-cell sweep's worst span
  // is 257 (the flattest aspect) and aspect 1 plateaus at 17 whatever the radius. It is kept so that a
  // caller with a hand-made cell cannot make this allocate gigabytes.
  if (half > 4096)
    half = 4096;
  return (int)(2 * half + 1);
}

// ---------------------------------------------------------------------------
// Discretisation limits
// ---------------------------------------------------------------------------

double coarsestCell(double radius, double aspect)
{
  // Measured, not tasted (D-36). The renderer gathers a layer's kernel from a plane reduced
  // to `cell` px cells, so the kernel it *applies* is the aperture convolved with that cell
  // quantisation; the measure that matters is therefore how much wider the result is than
  // the exact convolution, i.e. the kernel's second moment against the same kernel built at
  // cell 1. Walking cells upward until that ratio leaves a chosen tolerance gives a
  // *taps across the aperture's extent* plateau, and it is a plateau: the blade count between
  // 3 and 32 moves the crossing by less than the tolerance itself, and so does the radius
  // (the crossing scales with radius exactly, which is why this returns taps, not pixels).
  // What does move it is how flat the aperture is along each axis, because a squeezed axis
  // contributes proportionally less of the second moment: `aspect` 2 tolerates ~2.5 taps
  // across the squeezed axis and ~10 across the stretched one.
  //
  // The tolerance behind kTapsLongAxis is the measured ratio, not an aspiration: over 108
  // cases (9 shapes x aspect 1 / 2 / 0.25 / 8 x radius 8 / 32 / 128, each at its own coarsest
  // cell) the worst second-moment ratio is 1.0232 and the worst tap span is 257 (0.25 MB), and
  // the same bound holds for the reciprocal aspect because the two axes trade. Pushing the
  // taps higher is not free accuracy either: what is left at this point is the deliberate cell
  // quantisation — the ratio is 1.0286 with 8 sub-samples per cell and 1.0290 with 64, so more
  // sub-sampling buys a slower sprite, not a blur closer to the exact convolution.
  const double kTapsLongAxis = 14.0; // ~14 taps across the axis `aspect` lengthens: worst ratio 1.0232
  const double kTapsShortAxis = 4.0; // measured crossings 2.5-3.5; 4 keeps the margin
  double a = clampAspect(aspect);
  double stretch = radius * (a > 1.0 ? a : 1.0 / a); // the axis aspect lengthens
  double narrow = radius / (a > 1.0 ? a : 1.0 / a);  // the axis aspect squeezes
  double c = 2.0 * stretch / kTapsLongAxis;
  double shortCap = 2.0 * narrow / kTapsShortAxis;
  if (shortCap < c)
    c = shortCap;
  if (!isFin(c) || c < kCellMin)
    return kCellMin; // a kernel finer than a pixel is one tap, whatever the shape claims
  return c;
}

// ---------------------------------------------------------------------------
// Planes
// ---------------------------------------------------------------------------

PlaneImage reduceBox(const PlaneImage &src, int factor)
{
  PlaneImage dst;
  if (factor < 1)
    factor = 1;
  dst.channels = src.channels;
  if (src.width <= 0 || src.height <= 0)
    return dst;
  dst.width = (src.width + factor - 1) / factor;
  dst.height = (src.height + factor - 1) / factor;
  dst.rowStride = dst.width * src.channels;
  dst.px.assign((size_t)dst.rowStride * (size_t)dst.height, 0.0f);
  for (int y = 0; y < dst.height; ++y) {
    int y0 = y * factor, y1 = y0 + factor;
    if (y1 > src.height)
      y1 = src.height;
    for (int x = 0; x < dst.width; ++x) {
      int x0 = x * factor, x1 = x0 + factor;
      if (x1 > src.width)
        x1 = src.width;
      int n = (x1 - x0) * (y1 - y0);
      for (int c = 0; c < dst.channels; ++c) {
        double s = 0.0;
        for (int yy = y0; yy < y1; ++yy) {
          const float *row = src.at(x0, yy);
          for (int xx = x0; xx < x1; ++xx)
            s += (double)row[(size_t)(xx - x0) * (size_t)dst.channels + (size_t)c];
        }
        dst.px[(size_t)y * (size_t)dst.rowStride + (size_t)x * (size_t)dst.channels + (size_t)c] = (float)(s / (double)n);
      }
    }
  }
  return dst;
}

// ---------------------------------------------------------------------------
// Sprite library
// ---------------------------------------------------------------------------

/** @brief Does this bokeh picture make the library build one kernel plane per colour channel
 * (plus the coverage plane), or a single shared one? Exported from the library's own file because
 * a plugin sizing its working buffers has to count the planes the library is going to build, and
 * asking the library the same question it asks itself is the only way those two numbers cannot
 * drift apart. */
bool usesPerChannelKernels(const BokehOptions &o, const PlaneImage &img)
{
  return o.shape == eShapeImage && o.source == eBokehPerChannel && img.channels == 4 &&
         img.width > 0 && img.height > 0; // no picture on the clip: SpriteLibrary::build falls
}                                          // back to the analytic shape, which is one plane, and
                                           // this function has to describe what it builds

namespace {

/** @brief Which mip level a gather at @p cell should read: the deepest level whose pixel
 * pitch is still no coarser than the cell, so a tap averages about one mip pixel and the
 * bokeh picture is *rescaled* rather than nearest-neighbour copied at large radii. */
int pickMipLevel(double cell, int levels)
{
  int level = 0;
  while (level + 1 < levels && (double)(1u << (unsigned)(level + 1)) <= cell)
    ++level;
  return level;
}

} // namespace

SpriteLibrary::SpriteLibrary(const BokehOptions &options)
  : m_opt(options), m_gaussNorm(1.0)
{
  // The Gaussian's normalising constant is a property of the options alone, never of the
  // radius, so it is computed once here; the quadrature's mass is discarded because a
  // kernel's own mass is measured from its taps (that is what makes the comparison in
  // tests/zd_bokeh_test.cpp a test of the discretisation rather than a tautology).
  sanitise(m_opt);
  double mass = 0.0;
  apertureQuadrature(m_opt, &mass, &m_gaussNorm);
}

const Kernel &SpriteLibrary::kernel(double radius, double cell)
{
  double r = isFin(radius) && radius > 0.0 ? radius : 0.0;
  double c = (!isFin(cell) || cell < kCellMin) ? kCellMin : cell;
  long long rk = (long long)std::llround(r * 1024.0); // 1/1024 px: two equal radii, two equal keys
  long long ck = (long long)std::llround(c * 1024.0);
  for (std::list<Slot>::const_iterator it = m_slots.begin(); it != m_slots.end(); ++it) {
    if (it->radiusKey == rk && it->cellKey == ck) {
      ++m_reused;
      return it->kernel;
    }
  }
  m_slots.push_back(Slot());
  Slot &s = m_slots.back(); // std::list: references survive insertion, which is the whole
  s.radiusKey = rk;         // point — the renderer holds a Kernel& per layer and must not
  s.cellKey = ck;           // have it move when the next layer's kernel is built.
  build(s.kernel, r, c);
  ++m_built;
  return s.kernel;
}

void SpriteLibrary::setSourceImage(const PlaneImage &image)
{
  m_image = image;
  m_mipsDirty = true;
  clear(); // an image kernel is an answer about *this* picture; a cache built from another
}          // one would be the wrong answer to the same question

void SpriteLibrary::clear()
{
  m_slots.clear();
  m_built = 0;
  m_reused = 0;
}

size_t SpriteLibrary::bytes() const
{
  size_t b = 0;
  for (std::list<Slot>::const_iterator it = m_slots.begin(); it != m_slots.end(); ++it)
    b += it->kernel.bytes() + it->kernel.mass.size() * sizeof(double);
  return b;
}

void SpriteLibrary::build(Kernel &k, double radius, double cell) const
{
  if (m_opt.shape == eShapeImage)
    buildMips();

  if (!(radius > 0.0)) {
    // CoC 0: the identity, and deliberately the identity with the *same plane count* the
    // shape would have used — a renderer's channel logic must not change with the blur.
    int planes = usesPerChannelKernels(m_opt, m_image) ? 4 : 1;
    k.span = 1;
    k.planes = planes;
    k.radius = 0.0;
    k.cell = cell;
    k.taps.assign((size_t)planes, 1.0f);
    k.mass.assign((size_t)planes, 1.0);
    return;
  }

  if (m_opt.shape == eShapeImage && m_image.width > 0 && m_image.height > 0)
    buildImage(k, radius, cell);
  else
    buildAnalytic(k, radius, cell); // an image shape with no Bokeh clip connected has no
}                                   // picture to sample, so it gets the hard disc instead

void SpriteLibrary::buildAnalytic(Kernel &k, double radius, double cell) const
{
  Aperture ap(m_opt, radius, m_gaussNorm);
  int half = kernelSpan(radius, cell, m_opt.aspect) / 2;
  k.span = 2 * half + 1;
  k.planes = 1;
  k.radius = radius;
  k.cell = cell;
  k.taps.assign((size_t)k.span * (size_t)k.span, 0.0f);
  k.mass.assign(1, 0.0);

  double sum = 0.0;
  for (int dy = -half; dy <= half; ++dy) {
    for (int dx = -half; dx <= half; ++dx) {
      double v = ap.cellAverage((double)dx * cell, (double)dy * cell, cell);
      k.taps[(size_t)(dy + half) * (size_t)k.span + (size_t)(dx + half)] = (float)v;
      sum += v;
    }
  }
  k.mass[0] = sum;
  if (sum > 0.0) {
    double inv = 1.0 / sum;
    for (size_t i = 0; i < k.taps.size(); ++i)
      k.taps[i] = (float)((double)k.taps[i] * inv);
  } else {
    k.taps[(size_t)half * (size_t)k.span + (size_t)half] = 1.0f;
    k.mass[0] = 1.0;
  }
}

void SpriteLibrary::buildImage(Kernel &k, double radius, double cell) const
{
  const bool perCh = usesPerChannelKernels(m_opt, m_image);
  const int srcCh = m_image.channels;
  int half = kernelSpan(radius, cell, m_opt.aspect) / 2;
  k.span = 2 * half + 1;
  k.planes = perCh ? 4 : 1;
  k.radius = radius;
  k.cell = cell;
  const size_t n = (size_t)k.span * (size_t)k.span;
  std::vector<double> raw((size_t)k.planes * n, 0.0);

  int level = pickMipLevel(cell, (int)m_mips.size());
  const PlaneImage &src = m_mips[(size_t)level];
  Aperture ap(m_opt, radius, m_gaussNorm); // only its transform is used here

  for (int dy = -half; dy <= half; ++dy) {
    for (int dx = -half; dx <= half; ++dx) {
      double ux = 0.0, uy = 0.0;
      ap.toUnit((double)dx * cell, (double)dy * cell, &ux, &uy);
      size_t idx = (size_t)(dy + half) * (size_t)k.span + (size_t)(dx + half);
      double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
      // The picture *is* the aperture: its full extent stands for the aperture's bounding
      // box, so a tap at unit offset (ux, uy) reads the picture at that fraction of it —
      // which is what rescales the bokeh to the layer's radius — and a tap outside that box
      // is outside the aperture, hence 0.
      if (std::fabs(ux) <= 1.0 && std::fabs(uy) <= 1.0) {
        double sx = (ux + 1.0) * 0.5 * (double)(src.width - 1);
        double sy = (uy + 1.0) * 0.5 * (double)(src.height - 1);
        if (srcCh == 1) {
          r = g = b = a = samplePlane(src, sx, sy, 0);
        } else {
          r = samplePlane(src, sx, sy, 0);
          g = samplePlane(src, sx, sy, 1);
          b = samplePlane(src, sx, sy, 2);
          a = samplePlane(src, sx, sy, 3);
        }
      }
      if (perCh) {
        raw[idx] = r;
        raw[n + idx] = g;
        raw[2 * n + idx] = b;
        raw[3 * n + idx] = (r + g + b) / 3.0; // the coverage plane: the mean of the channels
      } else {
        double v;
        if (srcCh == 1)
          v = r;
        else if (m_opt.source == eBokehAlpha)
          v = a;
        else
          v = (r + g + b) / 3.0;
        raw[idx] = v;
      }
    }
  }

  k.taps.assign(n * (size_t)k.planes, 0.0f);
  k.mass.assign((size_t)k.planes, 0.0);
  const size_t center = (size_t)half * (size_t)k.span + (size_t)half;
  for (int p = 0; p < k.planes; ++p) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i)
      s += raw[(size_t)p * n + i];
    k.mass[p] = s;
    if (s > 0.0) {
      double inv = 1.0 / s;
      for (size_t i = 0; i < n; ++i)
        k.taps[(size_t)p * n + i] = (float)(raw[(size_t)p * n + i] * inv);
      continue;
    }
    // A channel whose bokeh picture is black has no shape to impose. A zero kernel would
    // delete that channel from the render, which is not what "each channel keeps its own
    // shape" means, so an empty channel becomes the identity and mass says so. Note the
    // plane offset: taps is one flat array, so an identity written at `center` alone would
    // land in plane 0 and silently overwrite *its* centre tap.
    k.taps[(size_t)p * n + center] = 1.0f;
    k.mass[p] = 1.0;
  }
}

void SpriteLibrary::buildMips() const
{
  if (!m_mipsDirty)
    return;
  m_mips.clear();
  if (m_image.width > 0 && m_image.height > 0) {
    m_mips.push_back(m_image);
    while (m_mips.back().width > 1 || m_mips.back().height > 1)
      m_mips.push_back(reduceBox(m_mips.back(), 2));
  }
  m_mipsDirty = false;
}

} // namespace zd
