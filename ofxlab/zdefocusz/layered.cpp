// ZDefocusZ: the layered renderer - the maths behind layered.h (DECISIONS.md D-38).
//
// Four things happen here, in this order, and each one is small enough to read:
//
//   1. prepare(): every pixel gets a CoC (zd.signedCoC), and the *plan* falls out of that map -
//      which levels have any pixels at all, how far a level's reach extends outside the render
//      window, and whether the whole render is provably a copy of the input.
//
//   2. Extraction: a level's cells are summed straight out of the image, weighted by how much of
//      each pixel that level owns (zd.h's soft seam). Pixels the level does not own contribute
//      zero to the cell average - which is what makes occlusion fall out of the composite instead
//      of needing a rule of its own.
//
//   3. Convolve: the cells are convolved with the level's kernel, on a lattice anchored to
//      **absolute** coordinates, so a tap's work is the same at CoC 3 px and CoC 3000 px and no
//      partition of the render window can move a pixel.
//
//   4. Composite: premultiplied over, back to front, reading the convolved cells bilinearly at
//      their centres. Colour where coverage is zero is zero, so ordering *is* coverage.
//
// Every box below is derived from the level's **radius**, not from the CoC of the pixels that
// level owns, and the reason is worth stating once: a level's pixels can carry a CoC pinned at
// `maxBlur` by `minBlur` (or legitimately that large) while the kernel the level convolves with
// is built for the level-set radius, which is the only number that says how far the level's
// content can spread. A box derived from CoCs could be much larger than the kernel it feeds.
// Bounding each level's field by its own radius bounds the level's *cost*, which is what makes
// the per-level peak in the plan honest whatever the knob set is.
#include "layered.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace zd {

namespace {

bool finite(double v) { return !std::isnan(v) && !std::isinf(v); }

double clampd(double v, double lo, double hi)
{
  if (std::isnan(v))
    return lo; // a NaN knob becomes the safe end of its own range
  if (v < lo)
    return lo;
  return v > hi ? hi : v;
}

// floor() of a quotient, in integers, rounding toward -infinity rather than toward zero: a render
// region may sit at negative coordinates, and a lattice that mirrored itself across zero would be
// two lattices.
long long floorDiv(long long a, long long b) // b > 0
{
  long long q = a / b;
  if (q * b > a)
    --q;
  return q;
}

// ceil() of the same quotient: the first cell whose *start* is at or after <p x</p>, which is how the
// fill's write box finds the first cell the delivered plane covers whole (D-48).
long long ceilDiv(long long a, long long b) // b > 0
{
  long long q = a / b;
  if (q * b < a)
    ++q;
  return q;
}

// ---------------------------------------------------------------------------
// The lattice. Cell c covers absolute pixels [c*cell, (c+1)*cell) and its centre is the point
// (c*cell + cell/2), which is the point the composite samples. Every index below is absolute.
// ---------------------------------------------------------------------------

int cellOf(int px, int cell) { return (int)floorDiv(px, cell); }
int cellStart(int c, int cell) { return c * cell; }

// The cells bilinearly interpolated *between* pixels [x0, x1) in a lattice of `cell`: the cells
// whose centres a pixel in that range can read, which is one wider on each side than the cells the
// pixels live in (a pixel whose centre sits below a cell centre still interpolates from it).
int readCellLo(int x0, int cell) { return cellOf(x0, cell) - 1; }
int readCellHi(int x1, int cell) { return cellOf(x1 - 1, cell) + 1; }

// ---------------------------------------------------------------------------
// Which levels a pixel belongs to, and with what weight (zd.h's soft seam, D-35)
// ---------------------------------------------------------------------------

// zd.h's ditheredCoC swings a CoC by half a *local* level step at full amplitude; the knob scales
// that swing around the undithered value, so 0 is exactly the undithered assignment and 100 is
// zd.h's own answer.
double dampedCoC(double coc, int x, int y, const LevelSet &levels, const LayerOptions &opt)
{
  if (!(opt.dither > 0.0))
    return coc;
  const double d = ditheredCoC(coc, x, y, levels, true);
  return coc + (d - coc) * (opt.dither / kDitherMax);
}

// The levels a pixel could join *either* way. Dither moves a pixel between levels, and a level's
// working box is fixed by prepare() before any pixel is extracted, so the boxes are sized from the
// union: a level never has to reach outside its own box to find a pixel dithered into it.
int pixelLevels(double coc, int x, int y, const LevelSet &levels, const LayerOptions &opt, int out[4])
{
  int n = 0;
  double t = 0.0;
  int i0 = 0, i1 = 0;
  levels.bracket(coc, &i0, &i1, &t);
  out[n++] = i0;
  if (i1 != i0)
    out[n++] = i1;
  if (opt.dither > 0.0) {
    int j0 = 0, j1 = 0;
    levels.bracket(dampedCoC(coc, x, y, levels, opt), &j0, &j1, &t);
    const int cand[2] = {j0, j1};
    for (int k = 0; k < 2; ++k) {
      bool seen = false;
      for (int m = 0; m < n; ++m)
        if (out[m] == cand[k])
          seen = true;
      if (!seen && n < 4)
        out[n++] = cand[k];
    }
  }
  return n;
}

void includePixel(LevelRect &lr, int index, int x, int y)
{
  if (lr.xHi < lr.xLo) { // LevelRect's default (xHi < xLo) *is* its empty state
    lr.level = index;
    lr.xLo = lr.xHi = x;
    lr.yLo = lr.yHi = y;
    return;
  }
  if (x < lr.xLo)
    lr.xLo = x;
  if (x > lr.xHi)
    lr.xHi = x;
  if (y < lr.yLo)
    lr.yLo = y;
  if (y > lr.yHi)
    lr.yHi = y;
}

// Rec.709 weights on *straight* colour. A deviation from Nuke, recorded as one: their bloom
// threshold names no luma formula, and a weighted luma is what the rest of this repository (and
// every colour pipeline) means by "over threshold". Taking max(RGB) instead would boost a
// saturated magenta spec that a luma pass leaves alone - a taste, not a bug.
double straightLuma(double r, double g, double b)
{
  const double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  return l > 0.0 ? l : 0.0;
}

} // namespace

// ---------------------------------------------------------------------------
// The exported level assignment (layered.h)
// ---------------------------------------------------------------------------

// The pair of levels a pixel contributes to, and how much of it each one owns. This is the *only*
// place weights are decided, and it does not depend on which level is asking, so every level sees
// the same split and the weights sum to 1 per pixel by construction (which is what makes a
// fully in-focus render its input exactly). @ref LayeredRender::extractRows calls it for every pixel
// it owns, and the plugin's `preview = Layers` calls it to show that assignment, so the preview and
// the blur cannot drift apart.
LevelPair pixelPair(double coc, int x, int y, const LevelSet &levels, const LayerOptions &opt)
{
  LevelPair p;
  double t = 0.0;
  levels.bracket(dampedCoC(coc, x, y, levels, opt), &p.i0, &p.i1, &t);
  p.w0 = 1.0 - t;
  p.w1 = (p.i1 == p.i0) ? 0.0 : t;
  return p;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

bool LayerOptions::operator==(const LayerOptions &o) const
{
  return layers == o.layers && layerSpacing == o.layerSpacing && layerCurve == o.layerCurve &&
         dither == o.dither && fillForeground == o.fillForeground &&
         spriteRadiusCap == o.spriteRadiusCap && bloom == o.bloom &&
         bloomThreshold == o.bloomThreshold && bloomGain == o.bloomGain && bloomGamma == o.bloomGamma;
}

void sanitise(LayerOptions &o)
{
  if (o.layers < 0)
    o.layers = 0; // 0 = derive it (zd.h's autoLayerCount); only the plugin knows maxBlur
  if (o.layers > kLayerMax)
    o.layers = kLayerMax;
  o.layerSpacing = clampd(o.layerSpacing, kLayerSpacingMin, kLayerSpacingMax);
  o.layerCurve = clampd(o.layerCurve, kLayerCurveMin, kLayerCurveMax);
  o.dither = clampd(o.dither, 0.0, kDitherMax);
  o.spriteRadiusCap = clampd(o.spriteRadiusCap, kSpriteRadiusCapMin, kSpriteRadiusCapMax);
  o.bloomThreshold = clampd(o.bloomThreshold, kBloomThresholdMin, kBloomThresholdMax);
  o.bloomGain = clampd(o.bloomGain, kBloomGainMin, kBloomGainMax);
}

// The radius a level's kernel is built for. The CoC a pixel measures is one thing; the aperture the
// renderer then owns, samples and gathers through is another, and every cost in this renderer follows
// the second (cell, ring, gather, sprite). A cap therefore clips the radius *at the kernel*, and
// nothing else: the CoC map, the level set and the assignment a pixel gets all stay exactly what the
// depth says, so a capped render defocuses everything at most `cap` px instead of blurring less.
// 0 (and any NaN, which clampd sends to the low end) means uncapped. (D-45)
double cappedRadius(double radius, double cap)
{
  if (!(cap > 0.0))
    return radius;
  return radius > cap ? cap : radius;
}

// ---------------------------------------------------------------------------
// Depth -> CoC, with the side of the focal plane it sits on
// ---------------------------------------------------------------------------

SignedCoC signedCoC(double raw, const FocusOptions &o, double blurSizePx)
{
  SignedCoC s;
  s.coc = cocRadius(raw, o, blurSizePx);
  s.side = eSideFar;

  // The side is the sign of (mapped distance - focusValue): the only ordering information a depth
  // channel carries, because on each side the CoC grows monotonically (toward infinity behind the
  // focal plane, toward the focal point in front of it), so ordering by CoC *within* a side *is*
  // ordering by depth. Blur mode has no distance to compare, so the channel's own relation to
  // focusValue is the only ordering a blur map implies - and its negative half is the near side,
  // because a negative blur map reads as "in front of" (D-35).
  double v = 0.0;
  if (o.depthMode == eDepthBlur) {
    v = o.depthInvert ? -raw : raw;
  } else {
    v = mappedDistance(raw, o);
    // A mapped distance that is not a positive, finite number is *no geometry*, and the renderer
    // reads it the way zd.h's defocusFactor does: far away, at the CoC at infinity. A distance
    // mode must not put such a pixel on the near side just because 0 is numerically below
    // focusValue: the near side is geometry between the lens and the focal plane, and no geometry
    // is neither. It is the far side, which is also where it composites - behind everything.
    if (!finite(v) || !(v > 0.0))
      return s; // eSideFar, which is what s.side holds
  }
  s.side = (finite(v) && v < o.focusValue) ? eSideNear : eSideFar;
  return s;
}

CoCMap prepareCoC(const Plane &depth, const RenderRect &area, const FocusOptions &o, double blurSizePx)
{
  CoCMap m;
  if (area.empty())
    return m;
  m.x1 = area.x1;
  m.y1 = area.y1;
  m.x2 = area.x2;
  m.y2 = area.y2;
  const int w = area.width(), h = area.height();
  m.coc.assign((size_t)w * (size_t)h, 0.0f);
  m.side.assign((size_t)w * (size_t)h, (unsigned char)eSideFar);

  const int dcomp = depth.comp == 4 ? 3 : 0; // RGBA: a 4-component plane holds Z in the 4th
                                             // component, because alpha is not a distance (and the
                                             // plugin's own selector unpacks the channel it picked
                                             // into a 1-component plane anyway)
  for (int y = area.y1; y < area.y2; ++y) {
    // A scanline the depth plane does not deliver holds no geometry, which is the same reading as
    // a sentinel value inside it: far away, blurred at maxBlur.
    const float *drow = depth.empty() ? 0 : depth.row(y);
    float *coc = &m.coc[(size_t)(y - area.y1) * (size_t)w];
    unsigned char *side = &m.side[(size_t)(y - area.y1) * (size_t)w];
    for (int x = area.x1; x < area.x2; ++x) {
      double raw = 0.0;
      if (drow && x >= depth.x1 && x < depth.x2)
        raw = (double)drow[(size_t)(x - depth.x1) * (size_t)depth.comp + (size_t)dcomp];
      const SignedCoC s = signedCoC(raw, o, blurSizePx);
      coc[(size_t)(x - area.x1)] = (float)s.coc;
      side[(size_t)(x - area.x1)] = (unsigned char)s.side;
    }
  }
  return m;
}

// ---------------------------------------------------------------------------
// The window a level gathers on
// ---------------------------------------------------------------------------

int cellForRadius(double radius, double aspect, double cap)
{
  const double r = cappedRadius(radius, cap); // the cell belongs to the kernel that gets built (D-45)
  if (!(r > 0.0))
    return 1; // the in-focus level gathers at cell 1, so it stays exactly itself
  const double c = coarsestCell(r, aspect);
  if (!(c > 1.0))
    return 1; // kCellMin: a kernel finer than a pixel is one tap whatever the shape claims
  return (int)std::floor(c); // a box reduction needs an integer factor
}

namespace {

// Everything one level of a given radius needs, in cells of its own lattice. The ring width comes
// from bokeh.h's kernelSpan, which the bokeh suite pins against the span a *built* kernel actually
// gets (`kernelSpan(r, cell, aspect) == built.span` for every shape and radius it walks), so the
// window prepared here is the window the tap loop needs and prepare() can report the peak without
// building a single kernel. beginLevel() still takes the built kernel's own half for the tap loop and
// widens the ring if that ever disagrees, because a truncated tap loop is a silently truncated blur.
struct LevelGeom {
  int cell, half, dilate, pad;
  int compX0, compY0, compX1, compY1; // the rect pixels this level composites (empty when inverted)
  int convC0, convC1, convR0, convR1; // cells the convolution computes (inclusive)
  int fillC0, fillR0, fillC1, fillR1; // cells `fill_foreground` may write (empty when inverted)
  int gX0, gY0, gCols, gRows;         // the grid those cells live in, zero ring included
  int margin;                         // ring width in cells: dilate + half + 2
  int pxX0, pxY0, pxX1, pxY1;         // the pixels that range covers, cell-aligned
  bool valid;
  size_t bytes; // this level's own buffers: gA + gB (5 planes each) + the fill's two line scratch
};

// Everything a level of this radius needs. @p lr is the level's assignment box from the CoC map
// (absolute, inclusive, possibly reaching outside @p rect); @p rect is the render window the level
// composites into; @p img is the delivered image plane, whose bounds bound the fill's writes (see
// fillLevel, D-48). prepare() and beginLevel() both call this with the same three inputs, so the
// reported peak *is* the allocation. @p cap is the same @ref LayerOptions::spriteRadiusCap both callers
// read, so the geometry is derived from one radius twice rather than from two radii (D-45).
LevelGeom levelGeom(double radius, double aspect, bool fill, double cap, const LevelRect &lr, const RenderRect &rect, const Plane &img)
{
  LevelGeom g;
  g.valid = false;
  g.cell = 1;
  g.half = 0;
  g.dilate = 0;
  g.pad = 1;
  g.compX0 = g.compY0 = g.compX1 = g.compY1 = 0;
  g.convC0 = g.convC1 = g.convR0 = g.convR1 = 0;
  g.gX0 = g.gY0 = g.gCols = g.gRows = g.margin = 0;
  g.pxX0 = g.pxY0 = g.pxX1 = g.pxY1 = 0;
  g.bytes = 0;
  if (lr.xHi < lr.xLo || lr.yHi < lr.yLo || rect.empty())
    return g; // no pixel joined this level anywhere: nothing to extract, convolve or composite

  // Everything below describes the kernel this level *builds*, so it is keyed on the capped radius:
  // the lattice, the tap reach and the fill radius all follow the aperture that exists (D-45).
  const double rk = cappedRadius(radius, cap);
  g.cell = cellForRadius(rk, aspect);
  g.half = kernelSpan(rk, (double)g.cell, aspect) / 2;
  // fillForeground's radius, in cells of this level's own lattice: the coverage a disc of this
  // diameter can be moved by when it is max-filtered over its own blur radius.
  g.dilate = fill && rk > 0.0 ? (int)std::floor(rk / (double)g.cell + 0.5) : 0;

  // The cells the level's *content* sits on (every pixel of the work area assigned to it,
  // bilinear-inclusive), widened by everything that content can spread to: `half` cells of kernel
  // and `dilate` cells of fill. A rect pixel could receive coverage from any of them.
  const int aC0 = readCellLo(lr.xLo, g.cell) - (g.half + g.dilate);
  const int aC1 = readCellHi(lr.xHi + 1, g.cell) + (g.half + g.dilate);
  const int aR0 = readCellLo(lr.yLo, g.cell) - (g.half + g.dilate);
  const int aR1 = readCellHi(lr.yHi + 1, g.cell) + (g.half + g.dilate);

  // Clipped to the window. This clip is what keeps a level's work proportional to the window rather
  // than to the CoC: the grid below is cell-reduced, so a 4000 px kernel is a few dozen cells.
  g.compX0 = std::max(rect.x1, cellStart(aC0, g.cell));
  g.compX1 = std::min(rect.x2, cellStart(aC1 + 1, g.cell));
  g.compY0 = std::max(rect.y1, cellStart(aR0, g.cell));
  g.compY1 = std::min(rect.y2, cellStart(aR1 + 1, g.cell));
  if (g.compX1 <= g.compX0 || g.compY1 <= g.compY0)
    return g; // this level cannot reach a single pixel of the window

  // The convolved cell box: the cells the convolution computes, which are the cells the composite
  // bilinear-reads and nothing more. readCellLo/readCellHi already stand one cell outside the cells a
  // pixel reads, so <c>pad</c> is that one cell - and the box does not depend on the knob at all,
  // because the fill works *below* the convolution, on the extracted grid, so what it widens is what
  // extraction reads (below) rather than what the blur writes.
  //
  // The closure then runs outwards in one direction: a composited pixel reads a convolved cell, that
  // cell's taps read <c>half</c> cells around the convolved box, and each of those cells is the
  // <c>dilate</c>-window lift of the cell it sits on - so extraction must cover the convolved box
  // widened by <c>half + dilate</c>, which is exactly what it does (below), and a lifted cell is only
  // ever written where the convolution reads one (fillLevel). Every cell a composited pixel depends on
  // is therefore extracted, lifted and convolved inside this one render, whatever the render window is.
  // A cell outside the extracted box is the ring, which is exactly zero in every render; a cell inside
  // a narrower render's box but outside a wider one's would be the one way this renderer could make a
  // partition-dependent pixel (the claim tests/zd_layered_test.cpp section F pins).
  const int cC0 = readCellLo(g.compX0, g.cell);
  const int cC1 = readCellHi(g.compX1 - 1, g.cell);
  const int cR0 = readCellLo(g.compY0, g.cell);
  const int cR1 = readCellHi(g.compY1 - 1, g.cell);
  g.convC0 = cC0 - g.pad;
  g.convC1 = cC1 + g.pad;
  g.convR0 = cR0 - g.pad;
  g.convR1 = cR1 + g.pad;

  // The cells `fill_foreground` may write: the convolved box widened by the level's own aperture, cut
  // down to the cells the *delivered* image covers whole.
  //
  // The widening is what the fill has always had: a lifted cell is an input to the convolution, so it has
  // to sit within `half` cells of a cell the convolution computes, and the convolution reads `half` cells
  // past the box it computes - which is the box fillLevel() and dilateLines() already wrote to. The cut is
  // the new half. Extraction ran over the whole padded map, so coverage exists past the delivered plane:
  // those cells describe pixels of a plate the host trimmed, and lifting them is how a fill that is
  // switched on shows up as a fringe on the host's own padding (D-48). What the bound keeps is therefore a
  // cell whose *every* pixel the extraction could read: a cell straddling the plate's edge holds the
  // average of only the part of itself that was delivered, so lifting it to what its own window claims
  // invents the coverage of the part that was not. `ceil` for the first cell that starts at or after the
  // plane, `floor - 1` for the last cell that ends at or before it, and an inverted pair when the plane is
  // narrower than a cell, which means no writes at all.
  g.fillC0 = (int)ceilDiv(img.x1, g.cell);
  g.fillR0 = (int)ceilDiv(img.y1, g.cell);
  g.fillC1 = (int)floorDiv(img.x2, g.cell) - 1;
  g.fillR1 = (int)floorDiv(img.y2, g.cell) - 1;

  // The ring of exact zeros around it: `half` cells (the convolution reads that far outside the
  // convolved box, and the composite samples bilinearly one cell outside the cells it writes) and
  // `dilate` more (what the fill reads outside it), plus one for safety.
  g.margin = levelMargin(rk, aspect, fill); // the exported helper's three terms, so the ring this
  // grid reserves *is* the padding prepare() advertises and the plugin hands its host (maxKernelReach)
  g.gX0 = g.convC0 - g.margin;
  g.gY0 = g.convR0 - g.margin;
  g.gCols = (g.convC1 - g.convC0 + 1) + 2 * g.margin;
  g.gRows = (g.convR1 - g.convR0 + 1) + 2 * g.margin;
  if (g.gCols <= 0 || g.gRows <= 0)
    return g;

  // The pixels extraction reads, cell-aligned: the convolved box widened by the taps it convolves *and*
  // by the fill's radius, because a lift is read <c>dilate</c> cells past the cell it is written to and
  // the convolution reads the cell it is written to. The same pixels as before the fill moved below the
  // convolution - the convolved box used to carry <c>dilate</c> of this widening and extraction carries
  // it now - which is why the ring below, and the reach the plan advertises from it, is unchanged.
  g.pxX0 = cellStart(g.convC0 - g.half - g.dilate, g.cell);
  g.pxX1 = cellStart(g.convC1 + g.half + g.dilate + 1, g.cell);
  g.pxY0 = cellStart(g.convR0 - g.half - g.dilate, g.cell);
  g.pxY1 = cellStart(g.convR1 + g.half + g.dilate + 1, g.cell);

  // gA and gB hold 5 planes per cell each (premultiplied R, G, B, coverage, weight: the convolved
  // grid needs no sixth plane because a level's over-alpha *is* its coverage plane). The fill needs
  // no grid of its own - it reads gA's coverage plane and writes its dilated result into gB's weight
  // plane, which blurRows() overwrites from zero before anything reads it (see fillLevel) - and its
  // line scratch holds a row plus the zero pad it dilates through, which is why the pad is part of
  // the reported peak rather than an extra surprise.
  const size_t cells = (size_t)g.gCols * (size_t)g.gRows;
  const size_t line = (size_t)(g.gCols > g.gRows ? g.gCols : g.gRows) + 2u * (size_t)g.dilate + 1u;
  g.bytes = cells * (5u + 5u) * sizeof(float) + line * 2u * sizeof(float);
  g.valid = true;
  return g;
}

} // namespace

// The same numbers, without a level rect: what one level of this radius needs at the *worst*, which
// is what pads the CoC map and what the plugin hands its host as the RoI and the memory estimate
// (one formula, so the two cannot disagree - levelGeom() takes its ring width from here).
int levelMargin(double radius, double aspect, bool fill, double cap)
{
  const double r = cappedRadius(radius, cap); // the ring belongs to the kernel that gets built (D-45)
  const int cell = cellForRadius(r, aspect);
  const int half = kernelSpan(r, (double)cell, aspect) / 2;
  const int dilate = fill && r > 0.0 ? (int)std::floor(r / (double)cell + 0.5) : 0;
  return dilate + half + 2;
}

// The widest reach of a whole level set, in px *outside* the render window some level gathers from:
// each level's own margin, in cells of its own lattice, times that cell. prepare() pads its CoC map
// by exactly this, and a plugin hands its host the same number for the RoI, the RoD and the memory
// estimate, so a host can never be asked for less than the renderer then reads (the D-38 note in
// layered.h: the reach is measured against the level set's radii - what the kernels are built for -
// and not against maxBlur, so a level set finer than maxBlur does not pad for a blur that cannot
// exist). A @p cap is applied to each radius first, because the gather is a property of the kernel
// the renderer is willing to build and not of the CoC the depth measured (D-45): a capped ladder pads
// the map, the RoI and the memory estimate for the widest kernel it can actually make.
double maxKernelReach(const LevelSet &levels, double aspect, bool fill, double cap)
{
  double reach = 0.0;
  const int L = levels.count();
  for (int i = 0; i < L; ++i) {
    const double r = cappedRadius(levels.radius[(size_t)i], cap);
    const double need = (double)levelMargin(r, aspect, fill) * (double)cellForRadius(r, aspect);
    if (need > reach)
      reach = need;
  }
  return reach;
}

// ---------------------------------------------------------------------------
// LayeredRender
// ---------------------------------------------------------------------------

LayeredRender::LayeredRender(const LayerOptions &layers, SpriteLibrary &sprites, const LevelSet &levels)
    : m_opt(layers), m_sprites(sprites), m_bokeh(sprites.options()), m_levels(levels), m_aspect(1.0), m_image(0), m_imgComp(4)
{
  m_aspect = m_bokeh.aspect > 0.0 ? m_bokeh.aspect : 1.0;
}

bool LayeredRender::prepare(const Plane &depth, const Plane &image, const RenderRect &rect, const FocusOptions &focus, double blurSizePx)
{
  m_rect = rect;
  m_coc = CoCMap();
  m_plan = RenderPlan();
  m_order.clear();
  m_acc.clear();
  m_accW.clear();
  m_haveLevel = false;
  m_image = &image;
  m_imgComp = image.comp; // the stride the extraction stage reads with: 1, 3 or 4 (layered.h)

  if (rect.empty())
    return false; // nothing to render, nothing allocated

  sanitise(m_opt);

  const int L = m_levels.count();
  if (L <= 0) {
    m_plan.levels = 0;
    m_plan.allSharp = true; // no level set means nothing this node can blur
    return true;
  }
  m_plan.levels = L - 1;

  // The CoC map covers the render rect padded by as far outside it as any level gathers, because
  // the pixels *outside* the window are what a level's blur and fill gather *from*. "As far as any
  // level" is measured in cells of each level's own lattice - kernel half-span plus fill radius plus
  // the read-back cell - and against the level set's radii (what the kernels are built for), not
  // maxBlur, so a level set finer than maxBlur does not pad the map for a blur that cannot exist.
  // spriteRadiusCap is applied to those radii first, so a capped ladder pads for the widest kernel it
// can build and not for the CoC its deepest level measured (D-45).
// This is also the number the plugin hands its host as the RoI, so the two cannot disagree.
  m_plan.maxReach = maxKernelReach(m_levels, m_aspect, m_opt.fillForeground, m_opt.spriteRadiusCap);
  const int pad = (int)std::ceil(m_plan.maxReach);
  m_area.x1 = rect.x1 - pad;
  m_area.y1 = rect.y1 - pad;
  m_area.x2 = rect.x2 + pad;
  m_area.y2 = rect.y2 + pad;

  m_coc = prepareCoC(depth, m_area, focus, blurSizePx);

  // The composite order, and the only ordering rule there is (see layered.h): the far ladder from the
  // largest CoC down (the most defocused is the farthest behind the focal plane), the in-focus level,
  // then the near ladder from the smallest CoC up (on the near side a *bigger* CoC is nearer, so it
  // composites later). A level is visited once per side it has content on, because the near and the
  // far pixels that bracket into the same CoC band are not at the same distance: one pass over both
  // would blend them, and a blend is not an order.
  std::vector<LevelRect> sideRects((size_t)kNumSides * (size_t)L);
  for (int i = 0; i < L; ++i) {
    sideRects[(size_t)i].level = i;
    sideRects[(size_t)i].side = eSideFar;
    sideRects[(size_t)L + (size_t)i].level = i;
    sideRects[(size_t)L + (size_t)i].side = eSideNear;
  }
  sideRects[0].side = eSideAny; // the in-focus level is one visit that takes both sides (LevelVisit)

  // Per-visit boxes, straight out of the map: the pixels in the *work area* that can join the visit -
  // not just the pixels inside the window - because a level whose content lies entirely outside the
  // window still blurs *into* it. A visit with no pixel anywhere is left out of the order entirely; a
  // visit's boxes (beginLevel) are then widened by what its own content can spread to.
  double maxCoC = 0.0;
  for (int y = m_area.y1; y < m_area.y2; ++y) {
    for (int x = m_area.x1; x < m_area.x2; ++x) {
      const double coc = (double)m_coc.cocAt(x, y);
      if (coc > maxCoC)
        maxCoC = coc;
      const int side = m_coc.sideAt(x, y);
      int lv[4];
      const int n = pixelLevels(coc, x, y, m_levels, m_opt, lv);
      for (int k = 0; k < n; ++k) {
        // The in-focus level keeps one box for both sides: its kernel is a single tap of 1, its
        // lattice is cell 1, and the bilinear weights at cell 1 come out 1 and 0 exactly, so an
        // in-focus pixel reads exactly its own cell and one visit cannot move it anywhere.
        const int slot = lv[k] == 0 ? 0 : (side == eSideNear ? 1 : 0) * L + lv[k];
        includePixel(sideRects[(size_t)slot], lv[k], x, y);
      }
    }
  }
  m_plan.maxCoC = maxCoC;
  // Identity: every CoC in the *work area* is 0, so every pixel is in-focus, every level but the
  // first is empty, and the composite would be its own copy. Judged over the area and not just
  // the window because a defocused object outside the window blurs into it, and a near level
  // composites *over* an in-focus pixel rather than being occluded by it.
  m_plan.allSharp = !(maxCoC > 0.0) || m_plan.levels == 0;

  // The order and the boxes are the same list, in the same order: the visits the render runs. A
  // level whose pixels exist somewhere in the work area but cannot reach a single pixel of *this*
  // window is left out of both — the same test beginLevel() applies, so levelOrder() can never hand
  // back a visit beginLevel() would reject. (It matters whenever the window is smaller than the
  // CoC: a host that tiles its ROI gets a rect whose padding ring is full of pixels no level in it
  // can land on.)
  //
  // Peak, measured before anything is allocated: the map and the accumulator live for the whole
  // render, one level's buffers at a time. levelGeom() is the same call beginLevel() makes, and it
  // is called here for exactly the visits that get advertised, so the reported peak is the
  // allocation the render actually makes.
  const size_t maps = m_coc.coc.size() * sizeof(float) + m_coc.side.size() * sizeof(unsigned char) +
                      (size_t)m_rect.width() * (size_t)m_rect.height() * 6 * sizeof(float);
  size_t peak = 0;
  m_order.clear();
  m_plan.levelRects.clear();
  m_order.reserve((size_t)kNumSides * (size_t)L);
  m_plan.levelRects.reserve((size_t)kNumSides * (size_t)L);
  for (int side = 0; side < kNumSides; ++side) {
    const bool near = (side == eSideNear);
    for (int i = near ? 1 : L - 1; near ? i < L : i >= 1; near ? ++i : --i) {
      const LevelRect &lr = sideRects[(size_t)side * (size_t)L + (size_t)i];
      if (lr.xHi < lr.xLo)
        continue; // no pixel anywhere joined this level on this side
      const LevelGeom g = levelGeom(m_levels.radius[(size_t)i], m_aspect, m_opt.fillForeground, m_opt.spriteRadiusCap, lr, m_rect,
                                   *m_image);
      if (!g.valid)
        continue; // its pixels are all out of reach of the window: nothing to extract, convolve or composite
      if (g.bytes > peak)
        peak = g.bytes;
      m_plan.levelRects.push_back(lr);
      LevelVisit v;
      v.level = i;
      v.side = side;
      m_order.push_back(v);
    }
    if (!near) {
      const LevelRect &lr = sideRects[0]; // the in-focus level, once, between the two ladders
      const LevelGeom g = levelGeom(m_levels.radius[0], m_aspect, m_opt.fillForeground, m_opt.spriteRadiusCap, lr, m_rect,
                                    *m_image);
      if (lr.xHi >= lr.xLo && g.valid) {
        if (g.bytes > peak)
          peak = g.bytes;
        m_plan.levelRects.push_back(lr);
        LevelVisit v;
        v.level = 0;
        v.side = eSideAny;
        m_order.push_back(v);
      }
    }
  }
  m_plan.peakBytes = maps + peak;

  // The accumulator is only the composite's scratch: the identity path never touches it. All four
  // planes start at **0**, because the fourth holds the accumulated alpha `A` rather than the
  // transmission `1 - A` it is applied as: before any visit has run nothing is in front of the light,
  // and that is A = 0. compositeRows() grows it from there and writeOutput() divides the accumulated
  // colour by it to get straight colour, which is why it is stored as the alpha and not recovered from
  // `1 - T` at the end (the header note, D-49).
  if (!m_plan.allSharp) {
    const size_t pixels = (size_t)m_rect.width() * (size_t)m_rect.height();
    m_acc.assign(pixels * 4, 0.0f); // plane 3 starts at 0: it accumulates the alpha, not the
                                    // transmission (header note, D-49)
    m_accW.assign(pixels * 2, 0.0f);
  }
  return true;
}

// ---------------------------------------------------------------------------
// One level: geometry, buffers, then the stages
// ---------------------------------------------------------------------------

float *LayeredRender::cellAt(std::vector<float> &g, int col, int row, int p) const
{
  return &g[(((size_t)(row - m_lv.gY0) * (size_t)m_lv.gCols + (size_t)(col - m_lv.gX0)) * 5u) + (size_t)p];
}

bool LayeredRender::beginLevel(const LevelVisit &visit)
{
  if (!m_image || visit.level < 0 || visit.level >= m_levels.count() || m_rect.empty())
    return false;

  // The plan holds one box per visit, and a visit is identified by (level, side) - which is what
  // levelOrder() hands back, so a caller that walks the order always finds its own box.
  const LevelRect *lr = 0;
  for (size_t i = 0; i < m_plan.levelRects.size(); ++i) {
    const LevelRect &cand = m_plan.levelRects[i];
    if (cand.level == visit.level && cand.side == visit.side) {
      lr = &cand;
      break;
    }
  }
  if (!lr)
    return false; // prepare() left this visit out of the plan: no pixel joined it anywhere

  const LevelGeom g =
      levelGeom(m_levels.radius[(size_t)visit.level], m_aspect, m_opt.fillForeground, m_opt.spriteRadiusCap, *lr, m_rect,
                *m_image);
  if (!g.valid)
    return false; // the visit's pixels cannot reach the window this render is drawing

  m_lv = Level();
  m_lv.index = visit.level;
  m_lv.side = visit.side;
  // The radius this level actually blurs with: its own CoC, capped (D-45). It is what the kernel is
  // built for, and the sprite cache keys on it, so every level the cap clips asks for the same
  // (radius, cell) pair and shares one kernel.
  m_lv.radius = cappedRadius(m_levels.radius[(size_t)visit.level], m_opt.spriteRadiusCap);
  m_lv.cell = g.cell;
  m_lv.dilate = g.dilate;
  m_lv.pad = g.pad;
  m_lv.kernel = &m_sprites.kernel(m_lv.radius, (double)g.cell);
  m_lv.half = m_lv.kernel->half();
  m_lv.convC0 = g.convC0;
  m_lv.convC1 = g.convC1;
  m_lv.convR0 = g.convR0;
  m_lv.convR1 = g.convR1;
  m_lv.fillC0 = g.fillC0;
  m_lv.fillR0 = g.fillR0;
  m_lv.fillC1 = g.fillC1;
  m_lv.fillR1 = g.fillR1;
  m_lv.pxX0 = g.pxX0;
  m_lv.pxX1 = g.pxX1;
  m_lv.pxY0 = g.pxY0;
  m_lv.pxY1 = g.pxY1;
  m_lv.compX0 = g.compX0;
  m_lv.compX1 = g.compX1;
  m_lv.compY0 = g.compY0;
  m_lv.compY1 = g.compY1;

  // The ring is sized from the span the *plan* reserved (levelGeom), but the taps come from the built
  // kernel. The bokeh suite pins the two equal; if a kernel ever came back wider, the ring widens to
  // fit it rather than the tap loop silently truncating the blur.
  int margin = g.margin;
  if (m_lv.half > g.half)
    margin += m_lv.half - g.half;
  m_lv.margin = margin;
  m_lv.gCols = g.gCols + 2 * (margin - g.margin);
  m_lv.gRows = g.gRows + 2 * (margin - g.margin);
  m_lv.gX0 = g.gX0 - (margin - g.margin);
  m_lv.gY0 = g.gY0 - (margin - g.margin);

  m_lv.gA.assign((size_t)m_lv.gCols * (size_t)m_lv.gRows * 5u, 0.0f);
  m_lv.gB.assign((size_t)m_lv.gCols * (size_t)m_lv.gRows * 5u, 0.0f);

  m_haveLevel = true;
  return true;
}

// The cell rows a stage must have ready for output rows [rowStart, rowEnd). The rows it needs are
// the *convolved* rows those pixels read - their own rows, widened by `pad` (levelGeom's widening,
// which is what makes a cell bilinear read-back samples a convolved cell rather than a stale one) and
// then clamped to the rows this level actually convolves. @p halo is how far *this* stage reaches
// outside them: the convolution reads `half` cells of extracted field, extraction fills the cells the
// convolution reads. So a stage is always asked for its own range, never a smaller one.
void LayeredRender::cellRowsFor(int rowStart, int rowEnd, int halo, int *lo, int *hi) const
{
  const Level &lv = m_lv;
  const int yA = std::max(rowStart, lv.compY0), yB = std::min(rowEnd, lv.compY1);
  if (yB <= yA) {
    *lo = 1;
    *hi = 0; // empty: the caller's loop runs zero times
    return;
  }
  int cLo = readCellLo(yA, lv.cell) - lv.pad;
  int cHi = readCellHi(yB - 1, lv.cell) + lv.pad;
  if (cLo < lv.convR0)
    cLo = lv.convR0;
  if (cHi > lv.convR1)
    cHi = lv.convR1;
  if (cHi < cLo) {
    *lo = 1;
    *hi = 0;
    return;
  }
  *lo = cLo - halo;
  *hi = cHi + halo;
}

// ---------------------------------------------------------------------------
// Extraction: a level's cells, summed straight out of the image
// ---------------------------------------------------------------------------
//
// One pass per cell row, and one scanline of the image delivered at a time: a cell is the box average
// of the pixels inside it, so the stage holds a row of cell sums, never a copy of a level.
void LayeredRender::extractRows(int rowStart, int rowEnd)
{
  if (!m_haveLevel)
    return;
  const Level &lv = m_lv;
  const int cell = lv.cell;

  // The cells to fill are the cells the stages above this one will read: the convolved rows these output
  // rows need, widened by the kernel half-span *and* by the fill's radius - a lift is computed from a
  // window that reaches `dilate` cells past the cell the convolution reads, and it is that lift the
  // convolution then convolves.
  int cyLo = 0, cyHi = 0;
  cellRowsFor(rowStart, rowEnd, lv.half + lv.dilate, &cyLo, &cyHi);
  if (cyHi < cyLo)
    return;

  const int cxLo = lv.convC0 - lv.half - lv.dilate, cxHi = lv.convC1 + lv.half + lv.dilate;
  const int nCols = cxHi - cxLo + 1;
  const Plane &img = image();
  const int stride = imageComponentCount();
  const int side = lv.side; // eSideAny takes both (the in-focus level); every other visit takes one
  const int xLo = std::max(lv.pxX0, img.x1), xHi = std::min(lv.pxX1, img.x2);
  if (nCols <= 0 || xHi <= xLo)
    return;

  // Bloom, resolved once per stage. bloomGamma puts both the comparison and the gain in a
  // 2.2-encoded space (Nuke's bloom_gamma: "a 2.2 gamma through the blur and reversed after"), which
  // for a *power* gain is exactly scaling linear colour by gain^(1/2.2) - so the record's two
  // readings agree, and no pixel ever takes a pow. The boost is applied to straight colour *before*
  // the convolution, because a highlight is a highlight whatever layer it ends up in.
  const bool bloom = m_opt.bloom && m_opt.bloomGain != 1.0; // gain 1 is a no-op whatever the threshold
  const double linThreshold = m_opt.bloomGamma ? std::pow(m_opt.bloomThreshold, kBloomGamma) : m_opt.bloomThreshold;
  const double linGain = m_opt.bloomGamma ? std::pow(m_opt.bloomGain, 1.0 / kBloomGamma) : m_opt.bloomGain;

  std::vector<double> sum((size_t)nCols * 5u, 0.0);
  const double invArea = 1.0 / ((double)cell * (double)cell);

  for (int cy = cyLo; cy <= cyHi; ++cy) {
    std::fill(sum.begin(), sum.end(), 0.0);
    const int pyLo = cellStart(cy, cell), pyHi = pyLo + cell;
    for (int y = pyLo; y < pyHi; ++y) { // cell rows ascend, so one delivered scanline at a time
      const float *irow = img.empty() ? 0 : img.row(y);
      if (!irow)
        continue; // a scanline the image does not deliver holds neither light nor coverage
      for (int x = xLo; x < xHi; ++x) {
        if (side != eSideAny && m_coc.sideAt(x, y) != side)
          continue; // this pixel belongs to the other ladder: a level of this one holds none of it
        const LevelPair pr = pixelPair((double)m_coc.cocAt(x, y), x, y, m_levels, m_opt);
        const double w = (pr.i0 == lv.index) ? pr.w0 : ((pr.i1 == lv.index) ? pr.w1 : 0.0);
        if (!(w > 0.0))
          continue; // this level owns none of this pixel: zero in colour *and* in coverage

        const float *p = &irow[(size_t)(x - img.x1) * (size_t)stride];
        double r, g, b, a;
        if (stride >= 4) {
          r = p[0];
          g = p[1];
          b = p[2];
          a = p[3];
        } else if (stride == 3) {
          r = p[0];
          g = p[1];
          b = p[2];
          a = 1.0; // an RGB source has no alpha channel to be transparent with
        } else {
          r = g = b = (double)p[0]; // a single-channel source: its value *is* its colour and its density
          a = 1.0;
        }
        if (!(a > 0.0) || !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
          continue; // transparent or non-finite holds neither light nor occlusion
        if (a > 1.0)
          a = 1.0; // straight alpha, always inside its own range

        if (bloom && straightLuma(r, g, b) > linThreshold) {
          r *= linGain;
          g *= linGain;
          b *= linGain;
        }

        // Premultiplied colour, coverage and weight side by side, the last two carrying the level's
        // share of the pixel: the aperture a lens applies to a silhouette is the aperture it applies
        // to the light inside it, so all three must carry the same weight through the same convolution.
        const double pm = a * w;
        double *s = &sum[(size_t)(cellOf(x, cell) - cxLo) * 5u];
        s[0] += r * pm;
        s[1] += g * pm;
        s[2] += b * pm;
        s[3] += pm;
        s[4] += w;
      }
    }
    for (int cx = cxLo; cx <= cxHi; ++cx) {
      const double *s = &sum[(size_t)(cx - cxLo) * 5u];
      if (s[0] == 0.0 && s[1] == 0.0 && s[2] == 0.0 && s[3] == 0.0 && s[4] == 0.0)
        continue; // an all-zero cell is already zero: writing it would only cost a store
      *cellAt(m_lv.gA, cx, cy, 0) = (float)(s[0] * invArea);
      *cellAt(m_lv.gA, cx, cy, 1) = (float)(s[1] * invArea);
      *cellAt(m_lv.gA, cx, cy, 2) = (float)(s[2] * invArea);
      *cellAt(m_lv.gA, cx, cy, 3) = (float)(s[3] * invArea);
      *cellAt(m_lv.gA, cx, cy, 4) = (float)(s[4] * invArea);
    }
  }
}

// ---------------------------------------------------------------------------
// Convolution: one aperture, five planes
// ---------------------------------------------------------------------------

namespace {

// The kernel plane each grid plane convolves with: a kernel plane per colour channel when the kernel
// carries per-channel planes (D-37), and the kernel's own coverage plane for coverage *and* weight. A
// kernel with one plane - a shape-only bokeh, or a 1-component image bokeh - therefore drives all
// five, which is exactly what a monochrome aperture does to a colour picture.
int kernelPlane(const Kernel &k, int p) { return p < k.planes - 1 ? p : k.planes - 1; }

} // namespace

void LayeredRender::blurRows(int rowStart, int rowEnd)
{
  if (!m_haveLevel)
    return;
  Level &lv = m_lv;
  int rLo = 0, rHi = 0;
  cellRowsFor(rowStart, rowEnd, 0, &rLo, &rHi); // the convolved rows these output rows read
  if (rHi < rLo)
    return;

  const Kernel &k = *lv.kernel;
  const int span = k.span, half = lv.half;
  const int kp[5] = { kernelPlane(k, 0), kernelPlane(k, 1), kernelPlane(k, 2), kernelPlane(k, 3), kernelPlane(k, 4) };

  // A level at CoC 0 (or a bokeh channel with no area at all, which bokeh.h turns into an identity
  // tap, D-37) convolves with a single tap of 1: copying the row is the same answer and free, and it
  // is what makes the in-focus level *bit-identical* to the cells it came from.
  bool identity = (span == 1);
  if (identity) {
    for (int p = 0; p < k.planes; ++p) {
      if (k.plane(p)[0] != 1.0f) {
        identity = false;
        break;
      }
    }
  }

  // The taps never leave the grid: the ring is `dilate + half + 2` cells and this stage reads `half`
  // cells outside the convolved box, so the tap loop needs no bounds test at all (D-31's "never
  // special-case an edge": outside the grid means *no content*, not the nearest value).
  const int gCols = lv.gCols, gRows = lv.gRows;
  for (int r = rLo; r <= rHi; ++r) {
    if (r < lv.gY0 || r >= lv.gY0 + gRows)
      continue;
    const size_t baseB = ((size_t)(r - lv.gY0) * (size_t)gCols) * 5u;
    for (int c = lv.convC0; c <= lv.convC1; ++c) {
      float *d = &lv.gB[baseB + (size_t)(c - lv.gX0) * 5u];
      d[0] = d[1] = d[2] = d[3] = d[4] = 0.0f; // every convolved cell starts empty
    }
    if (identity) {
      for (int c = lv.convC0; c <= lv.convC1; ++c) {
        const float *s = cellAt(lv.gA, c, r, 0);
        float *d = &lv.gB[baseB + (size_t)(c - lv.gX0) * 5u];
        for (int p = 0; p < 5; ++p)
          d[p] = s[p];
      }
      continue;
    }
    for (int c = lv.convC0; c <= lv.convC1; ++c) {
      float *d = &lv.gB[baseB + (size_t)(c - lv.gX0) * 5u];
      // The taps are read *around this cell*: the leftmost tap column, as a grid column index. The
      // ring is wider than half (margin = dilate + half + 2), so this is never negative, and the
      // rightmost tap lands inside the grid for the same reason.
      const size_t cBase = (size_t)(c - lv.gX0) - (size_t)half;
      for (int dy = -half; dy <= half; ++dy) {
        const float *src = &lv.gA[(((size_t)(r + dy - lv.gY0) * (size_t)gCols) + cBase) * 5u];
        const float *kr0 = k.plane(kp[0]) + (size_t)(dy + half) * (size_t)span;
        const float *kr1 = k.plane(kp[1]) + (size_t)(dy + half) * (size_t)span;
        const float *kr2 = k.plane(kp[2]) + (size_t)(dy + half) * (size_t)span;
        const float *krc = k.plane(kp[3]) + (size_t)(dy + half) * (size_t)span;
        for (int dx = -half; dx <= half; ++dx) {
          const float tc = krc[dx + half];
          const float t0 = kr0[dx + half], t1 = kr1[dx + half], t2 = kr2[dx + half];
          if (t0 == 0.0f && t1 == 0.0f && t2 == 0.0f && tc == 0.0f)
            continue; // a bokeh's support is not a square: skip the taps outside it
          const float *s = src + (size_t)(dx + half) * 5u;
          d[0] += t0 * s[0];
          d[1] += t1 * s[1];
          d[2] += t2 * s[2];
          d[3] += tc * s[3];
          d[4] += tc * s[4];
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// fillForeground: lift the layer's own coverage and colour, *before* the blur
// ---------------------------------------------------------------------------

// Max over the window of @p span cells *ending* at each cell, amortised O(1) per cell (the
// decreasing-deque max filter). Never in place: an in-place scan compares values it has already
// filtered, which overestimates ([9,1,1,1] with span 3 would report 9 at every position instead of
// only the first). @p span <= 1 is a copy, because a window of one is the identity.
void LayeredRender::slidingMaxRow(const float *src, float *dst, int count, int span) const
{
  if (count <= 0)
    return;
  if (span <= 1) {
    for (int i = 0; i < count; ++i)
      dst[i] = src[i];
    return;
  }
  std::deque<int> dq; // indices, decreasing value: the front is the maximum of the window
  for (int i = 0; i < count; ++i) {
    while (!dq.empty() && src[dq.back()] <= src[i])
      dq.pop_back();
    dq.push_back(i);
    if (dq.front() <= i - span)
      dq.pop_front(); // one index can leave the window per step
    dst[i] = src[dq.front()];
  }
}

// Max-filter every line of the current level's grid in direction (@p dx, @p dy) with a *centred*
// window of 2*<c>dilate</c>+1 cells, taking zero for a cell outside the grid. A line is gathered into
// @p line, which is zero-padded by `dilate` on both ends, so the centred window is the trailing
// window of 2*dilate+1 read `dilate` places later - and the padding is what makes "outside the grid"
// mean zero rather than the nearest value, exactly as the convolution's ring does.
//
// @p src and @p dst are cell planes read with the strides @p srcStride and @p dstStride, so the pass
// can read a plane of the extracted grid and write a plane of the convolved one (fillLevel) without
// either grid being copied.
//
// @p dst is *accumulated*: every cell takes the max of what it already holds and this direction's
// window. That is what makes the four families fillLevel() runs (horizontal, vertical, both diagonals)
// one dilation rather than four sequential ones - every pass reads the same @p src, so the result is
// the union of four centred segments of length 2*<c>dilate</c>+1, whose reach is exactly
// <c>dilate</c> cells. Sequential passes would read what the previous one wrote, and the reach would
// be the *sum* of the four segments: wider than <c>dilate</c>, wider than this level's ring reserves,
// and therefore different in a narrow render rect than in a wide one. Like the convolution, it can
// under-fill a penumbra slightly and can never invent coverage a level's own content could not have
// supplied.
void LayeredRender::dilateLines(const float *src, size_t srcStride, float *dst, size_t dstStride, int dx, int dy, std::vector<float> &line, std::vector<float> &filt) const
{
  const int gX0 = m_lv.gX0, gY0 = m_lv.gY0, gCols = m_lv.gCols;
  const int d = m_lv.dilate, half = m_lv.half;
  const int span = 2 * d + 1;

  // The cells this pass may *read* are the extracted ones: the cells the convolution reads, widened by
  // the dilation radius, which is what extractRows() writes and lies `margin - d - half` = 2 cells
  // inside the grid. Outside them the grid is the ring of exact zeros, which is what the zero pad below
  // stands for.
  const int x0 = m_lv.convC0 - half - d, x1 = m_lv.convC1 + half + d;
  const int y0 = m_lv.convR0 - half - d, y1 = m_lv.convR1 + half + d;
  // The cells it may *write* are the ones the convolution reads, cut to the cells the fill is allowed to
  // lift at all (`fillC0..fillR1`: the delivered plane, D-48). A lift anywhere else is a value no stage
  // consumes, and a value whose own window reaches outside what a narrower render extracted - so writing
  // it is the one way this pass could make a partition-dependent pixel (section F) - and it is also the
  // fringe a host sees on its own padding when the fill is on.
  const int wx0 = std::max(m_lv.convC0 - half, m_lv.fillC0), wx1 = std::min(m_lv.convC1 + half, m_lv.fillC1);
  const int wy0 = std::max(m_lv.convR0 - half, m_lv.fillR0), wy1 = std::min(m_lv.convR1 + half, m_lv.fillR1);

  for (int r = y0; r <= y1; ++r) {
    for (int c = x0; c <= x1; ++c) {
      const int pc = c - dx, pr = r - dy;
      if (pc >= x0 && pc <= x1 && pr >= y0 && pr <= y1)
        continue; // this cell has a predecessor in the region, so its line starts earlier

      int len = 0;
      for (int cc = c, rr = r;; cc += dx, rr += dy) {
        line[(size_t)(d + len)] = src[(((size_t)(rr - gY0) * (size_t)gCols) + (size_t)(cc - gX0)) * srcStride];
        ++len;
        if (!(cc + dx >= x0 && cc + dx <= x1 && rr + dy >= y0 && rr + dy <= y1))
          break;
      }
      for (int i = 0; i < d; ++i) {
        line[(size_t)i] = 0.0f;
        line[(size_t)(d + len + i)] = 0.0f;
      }
      slidingMaxRow(&line[0], &filt[0], d + len + d, span);
      for (int t = 0, cc = c, rr = r; t < len; ++t, cc += dx, rr += dy) {
        if (cc < wx0 || cc > wx1 || rr < wy0 || rr > wy1)
          continue; // outside the cells the convolution reads: this lift is nobody's input
        float &dref = dst[(((size_t)(rr - gY0) * (size_t)gCols) + (size_t)(cc - gX0)) * dstStride];
        const float v = filt[(size_t)(d + t + d)];
        if (v > dref)
          dref = v; // max over the directions this stage has already contributed
      }
    }
  }
}

// The sums over the window dilateLines() maxes: the union of the four centred families, every cell
// counted once, because the only point the four families share is the centre. A cell outside the grid
// contributes nothing, exactly as the fill's zero pad contributes nothing to the max. @p sums takes
// premultiplied R, G, B and the coverage, in that order, and reads gA only.
void LayeredRender::fillWindowSums(int col, int row, float sums[4]) const
{
  const int gX0 = m_lv.gX0, gY0 = m_lv.gY0, gCols = m_lv.gCols, gRows = m_lv.gRows;
  const int d = m_lv.dilate;
  const int dirs[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };

  const size_t own = (((size_t)(row - gY0) * (size_t)gCols) + (size_t)(col - gX0)) * 5u;
  sums[0] = m_lv.gA[own];
  sums[1] = m_lv.gA[own + 1u];
  sums[2] = m_lv.gA[own + 2u];
  sums[3] = m_lv.gA[own + 3u]; // the centre, once
  for (int k = 0; k < 4; ++k) {
    for (int t = -d; t <= d; ++t) {
      if (t == 0)
        continue; // already summed
      const int cc = col + t * dirs[k][0], rr = row + t * dirs[k][1];
      if (cc < gX0 || cc >= gX0 + gCols || rr < gY0 || rr >= gY0 + gRows)
        continue; // outside the grid is nothing
      const size_t o = (((size_t)(rr - gY0) * (size_t)gCols) + (size_t)(cc - gX0)) * 5u;
      sums[0] += m_lv.gA[o];
      sums[1] += m_lv.gA[o + 1u];
      sums[2] += m_lv.gA[o + 2u];
      sums[3] += m_lv.gA[o + 3u];
    }
  }
}

// fill_foreground, implemented as the header describes it: the coverage this level *could* have
// supplied had its content not thinned out at a silhouette, max-filtered over the level's own blur
// radius - one dilation, of coverage alone - taken over the *extracted* cells, before the level's
// kernel has run. That order is the whole of it, and it is what makes the fill look like a defocus
// rather than a stencil.
//
// A dilation of a *convolved* coverage dilates what the blur already did to a silhouette. The extracted
// coverage of an opaque object is a step: 1 inside, 0 outside, one cell of partial in between. The blur
// turns that step into the penumbra, and a max-filter of radius <c>dilate</c> over the blurred result
// then flattens the whole penumbra back to the plateau it came from, with one cell of step left where
// the plateau ends. Coverage that changes by half its range in a single cell is what a *hard edge* is,
// and the colour that travelled with it is worse: scaling a cell's premultiplied colour by the same
// `dil / cov` its coverage is raised by preserves that cell's *straight* colour, and the straight
// colour of a cell whose coverage is a residue of the blur is a ratio of two numbers the kernel nearly
// cancelled. The fill then reproduces the residue at full opacity, in patches. Measured on the suite's
// silhouette scene: `dil / cov` up to 58x, adjacent-cell coverage jumps of 0.54, and a 145 px run of
// output with a single flat value. All three are the blotches.
//
// Dilating the extracted coverage lifts the step *before* anything has smoothed it, and the level's own
// kernel convolves the lifted step afterwards: the fill's plateau becomes a penumbra the same aperture
// makes everywhere else, the reach is the same <c>dilate</c> cells, and no coverage reaches the output
// as a raw max. Colour comes from the same window, as its coverage-weighted mean
// `sum(premultiplied) / sum(coverage)`: an average of colours that each still describe real pixels of
// this level's own content. Every extracted cell's premultiplied colour is its coverage times a colour
// inside the source's range, and so is the window's sum - so a filled cell carries the colour of the
// region the fill is filling, and no cell can hold a colour the scene does not contain, whatever the
// ratio. The window is the *same* one the coverage was maxed over (the union of the four families, each
// cell counted once) because the coverage this cell is given came out of it: averaging a wider window
// would describe colours that contributed none of it.
//
// The weight plane is then raised only as far as that coverage needs it, which is the half that keeps
// the output alpha honest. Raising coverage alone would raise `coverage / weight` too - a translucent
// layer made opaque where its own aperture had it thinner. Dilating the weight by the *same filter
// independently* is worse: it can lift a cell's weight above anything the window's coverage justifies
// (a cell next to a fully transparent one of the same level takes that cell's weight and its own alpha
// falls), and an alpha the source never had is exactly the kind of invention this renderer is built to
// avoid. Taking only what coverage needs keeps `coverage <= weight` cell by cell, so the output alpha
// can neither exceed 1 nor fall below where the layer's own aperture put it.
//
// For an opaque source a cell's coverage *is* its weight, so the two dilations would have come out
// identical and this pass is bit-identical to dilating both; a flat field is bit-identical whatever the
// alpha, because a max filter of a constant is that constant, and where nothing is lifted nothing is
// rewritten either. A cell the window cannot lift - because the most this level's own content could
// have put there is what is already there - is left exactly as it was. A cell the window *can* lift may
// have had no coverage at all, which is the disocclusion the knob exists for; what it cannot do is
// invent a footprint the level's own content does not have. The coverage a lift takes came from a cell
// this level extracted, the lift is applied only over the cells this level's convolution reads (its
// convolved box widened by <c>half</c>), and extraction is widened by <c>dilate</c> for exactly that
// reason: so the level's footprint grows by <c>dilate</c> cells and no further. Section E-0 measures it
// as stated - the last cell holding extracted coverage and the last cell holding convolved coverage each
// move by exactly <c>dilate</c> when the knob is turned on, and nothing changes past that.
void LayeredRender::fillLevel()
{
  if (!m_haveLevel || !m_opt.fillForeground || m_lv.dilate <= 0)
    return; // no dilation radius: nothing to do (the in-focus level is already itself)

  Level &lv = m_lv;
  const int gX0 = lv.gX0, gY0 = lv.gY0, gCols = lv.gCols;
  const int d = lv.dilate;
  std::vector<float> line((size_t)(lv.gCols > lv.gRows ? lv.gCols : lv.gRows) + 2u * (size_t)d + 1u, 0.0f);
  std::vector<float> filt(line.size(), 0.0f);
  const int dirs[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };

  // The cells this pass applies to: the ones the convolution reads, which are the convolved box widened
  // by its own half span, cut to the cells the delivered image plane covers whole.
  //
  // The widening is the half that has always been there: every window this pass consults reaches
  // `dilate` cells past the cell it applies to, and those are exactly the cells extractRows() wrote
  // (levelGeom), so the lift of every cell this pass writes is computed from cells this render extracted,
  // in this window and in any narrower one. The cut is D-48's half, and it is about *where a lift may be
  // written*, not about what may be read: extraction covers the padded map, so cells past the delivered
  // plane hold coverage of pixels the host trimmed away, and a lift written to one is a value the host
  // then sees - as a fringe on its own padding. The bound keeps only cells whose own pixels the
  // extraction all saw, because a cell straddling the plate's edge holds the average of only the part of
  // itself that was delivered and lifting it to what its window claims invents the rest. Reading stays
  // uncut: a lift *inside* the plate is justified by content that happens to sit just outside it, which is
  // what a defocus reaching past an edge is.
  const int wx0 = std::max(lv.convC0 - lv.half, lv.fillC0), wx1 = std::min(lv.convC1 + lv.half, lv.fillC1);
  const int wy0 = std::max(lv.convR0 - lv.half, lv.fillR0), wy1 = std::min(lv.convR1 + lv.half, lv.fillR1);
  if (wx1 < wx0 || wy1 < wy0)
    return; // the delivered plane is narrower than a cell of this lattice: there is no cell to lift

  // Three passes, and the one that reads a window reads only what the others leave alone:
  //
  //   A  the dilation of gA's coverage plane, accumulated into gB's weight plane;
  //   B  for every cell A lifts, that window's sums - read from gA, stashed in gB's colour planes;
  //   C  the lifted cells take what B stashed for them.
  //
  // gB is free to borrow: blurRows() is the stage that fills it, it has not run yet, and it zeroes every
  // cell it writes before accumulating into it. Borrowing it is also why neither grid needs copying, and
  // why no cell's fill can see another's: pass A writes gB alone, pass B reads gA alone, and gA is only
  // ever written by pass C, which runs after every window has been read. That last point is what makes
  // this a dilation rather than a propagation: a pass that read gA while writing it would let a lifted
  // cell raise the coverage of the cell that reads it, which then lifts the next one, and the reach of
  // the fill would be however far the scan order walked rather than <c>dilate</c> cells - which is one
  // way this filter could make a partition-dependent pixel (section F). So the sums are stashed here and
  // applied there, and pass C reads gB plus this cell's own gA values and writes this cell alone.
  for (int r = wy0; r <= wy1; ++r) {
    const size_t rowBase = (size_t)(r - gY0) * (size_t)gCols;
    for (int c = wx0; c <= wx1; ++c)
      lv.gB[(rowBase + (size_t)(c - gX0)) * 5u + 4u] = lv.gA[(rowBase + (size_t)(c - gX0)) * 5u + 3u]; // seeded with the cell's own coverage: a centred window includes it
  }
  for (int k = 0; k < 4; ++k)
    dilateLines(lv.gA.data() + 3u, 5u, lv.gB.data() + 4u, 5u, dirs[k][0], dirs[k][1], line, filt); // four families, one dilation

  for (int r = wy0; r <= wy1; ++r) {
    const size_t rowBase = (size_t)(r - gY0) * (size_t)gCols;
    for (int c = wx0; c <= wx1; ++c) {
      const size_t cell = rowBase + (size_t)(c - gX0);
      const float cov = lv.gA[cell * 5u + 3u], dil = lv.gB[cell * 5u + 4u];
      if (dil <= cov)
        continue; // nothing to raise: this cell already had the most its window could give it
      float sums[4];
      fillWindowSums(c, r, sums);
      // The window's best coverage is one of the cells summed, and every plane is non-negative, so
      // sums[3] >= dil > 0: the factor below is never above 1 and no lifted cell divides by a window
      // that is empty. `sums[k] * dil / sums[3]` is the window's straight colour times this cell's new
      // coverage, which is what a premultiplied cell of that colour and coverage *is*.
      const float k = dil / sums[3];
      float *stash = &lv.gB[cell * 5u];
      stash[0] = sums[0] * k;
      stash[1] = sums[1] * k;
      stash[2] = sums[2] * k;
      stash[3] = dil;
    }
  }

  for (int r = wy0; r <= wy1; ++r) {
    const size_t rowBase = (size_t)(r - gY0) * (size_t)gCols;
    for (int c = wx0; c <= wx1; ++c) {
      const size_t cell = rowBase + (size_t)(c - gX0);
      const float dil = lv.gB[cell * 5u + 4u];
      float *p = &lv.gA[cell * 5u];
      if (dil <= p[3])
        continue; // the cells pass B left alone are left exactly as extraction wrote them
      const float *stash = &lv.gB[cell * 5u];
      p[0] = stash[0];
      p[1] = stash[1];
      p[2] = stash[2];
      p[3] = stash[3];
      if (dil > p[4])
        p[4] = dil; // the weight follows coverage, never runs ahead of it (see the note above)
    }
  }
}

// ---------------------------------------------------------------------------
// Composite: premultiplied over, back to front
// ---------------------------------------------------------------------------

// Reading a convolved cell at full resolution is bilinear interpolation of the four cell centres
// nearest the pixel centre. The cell whose centre is at or below a pixel centre is
// floor((2*px + 1 - cell) / 2*cell) - floor() of (px + 0.5 - cell/2), kept integral so it is exact,
// and its range over a span of pixels is what readCellLo/readCellHi index (conservatively, one
// wider on each side). At cell 1 the weights come out 1 and 0 exactly, which is what makes the
// in-focus level read its own cell rather than an interpolation of it.
void LayeredRender::compositeRows(int rowStart, int rowEnd)
{
  if (!m_haveLevel || m_acc.empty())
    return;
  const Level &lv = m_lv;
  const int cell = lv.cell;
  const int yA = std::max(rowStart, lv.compY0), yB = std::min(rowEnd, lv.compY1);
  if (yB <= yA)
    return;

  const int gCols = lv.gCols;
  const size_t rowPitch = (size_t)gCols * 5u;
  const int rectW = m_rect.width();
  const double den = 2.0 * (double)cell;

  for (int y = yA; y < yB; ++y) {
    const long long ny = 2LL * y + 1 - cell;
    const long long r0 = floorDiv(ny, den); // the cell whose centre is at or below this pixel's
    if (r0 < lv.gY0 || r0 + 1 >= lv.gY0 + lv.gRows)
      continue; // the level's grid does not cover this row (it cannot: compY is inside the conv box)
    const double ty = (double)(ny - r0 * (long long)cell * 2) / den;
    const float *grow0 = &lv.gB[(size_t)(r0 - lv.gY0) * rowPitch];
    const float *grow1 = grow0 + rowPitch;

    // The accumulator is addressed by absolute pixel, so it starts at *this level's* first column -
    // not at the rect's. A level whose box begins inside the rect (every near level does) would
    // otherwise composite into the wrong pixels, and a rect-wide level (the top of the far ladder)
    // would look perfectly correct while doing it: the offset is zero exactly where the bug hides.
    const size_t xOff = (size_t)(lv.compX0 - m_rect.x1);
    float *acc = &m_acc[(((size_t)(y - m_rect.y1) * (size_t)rectW) + xOff) * 4u];
    float *accW = &m_accW[(((size_t)(y - m_rect.y1) * (size_t)rectW) + xOff) * 2u];

    for (int x = lv.compX0; x < lv.compX1; ++x, acc += 4, accW += 2) {
      const long long nx = 2LL * x + 1 - cell;
      const long long c0 = floorDiv(nx, den);
      const double tx = (double)(nx - c0 * (long long)cell * 2) / den;

      const double w00 = (1.0 - tx) * (1.0 - ty), w10 = tx * (1.0 - ty), w01 = (1.0 - tx) * ty, w11 = tx * ty;
      const float *p00 = grow0 + (size_t)(c0 - lv.gX0) * 5u;
      const float *p10 = p00 + 5u, *p01 = grow1 + (size_t)(c0 - lv.gX0) * 5u, *p11 = p01 + 5u;

      // This level at this pixel: its convolved premultiplied colour, its coverage (which *is* its
      // alpha in the over operator) and its weight (what that coverage would have been at full alpha).
      const double cr = w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];
      const double cg = w00 * p00[1] + w10 * p10[1] + w01 * p01[1] + w11 * p11[1];
      const double cb = w00 * p00[2] + w10 * p10[2] + w01 * p01[2] + w11 * p11[2];
      double ca = w00 * p00[3] + w10 * p10[3] + w01 * p01[3] + w11 * p11[3];
      const double cw = w00 * p00[4] + w10 * p10[4] + w01 * p01[4] + w11 * p11[4];
      if (ca < 0.0)
        ca = 0.0; // an ulp of drift in a convolved plane must not make a layer *subtract* coverage
      else if (ca > 1.0)
        ca = 1.0;

      // `acc[3]` is the accumulated alpha of the layers composited so far. The source-over rule
      // attenuates the *accumulated* colour by the incoming layer's own `1 - alpha` (header note), so the
      // incoming layer's opacity is what `tr` is - and the accumulated alpha gains what this layer covers
      // of the fraction that is still open: A += ca·(1 - A), which is the same product in the other
      // order, and `ca·tr` would be the wrong one. Colour and alpha are tracked apart on purpose - see the
      // header note on why the output alpha is a ratio, and on why A is stored instead of the transmission
      // it is complementary to (D-49).
      const double tr = 1.0 - ca;
      acc[0] = (float)(cr + (double)acc[0] * tr);
      acc[1] = (float)(cg + (double)acc[1] * tr);
      acc[2] = (float)(cb + (double)acc[2] * tr);
      acc[3] = (float)((double)acc[3] + ca * (1.0 - (double)acc[3]));
      accW[0] += (float)ca;
      accW[1] += (float)cw;
    }
  }
}

void LayeredRender::finishLevel()
{
  m_lv = Level();
  m_haveLevel = false; // the vectors go with the next beginLevel's assign()
}

// The current visit, copied out. Every number here is Level's, and the GPU path launches its kernels
// with these instead of deriving them a second time (gpu.h, D-51): the lattices, the boxes and the ring
// a host measures are decided once, in levelGeom()/beginLevel(), and a device that used its own would be
// a device running a different render.
LevelView LayeredRender::levelView() const
{
  LevelView v;
  if (!m_haveLevel)
    return v; // `valid == false`, every number at its default
  const Level &lv = m_lv;
  v.valid = true;
  v.level = lv.index;
  v.side = lv.side;
  v.radius = lv.radius;
  v.cell = lv.cell;
  v.half = lv.half;
  v.dilate = lv.dilate;
  v.pad = lv.pad;
  v.margin = lv.margin;
  v.gX0 = lv.gX0;
  v.gY0 = lv.gY0;
  v.gCols = lv.gCols;
  v.gRows = lv.gRows;
  v.convC0 = lv.convC0;
  v.convR0 = lv.convR0;
  v.convC1 = lv.convC1;
  v.convR1 = lv.convR1;
  v.fillC0 = lv.fillC0;
  v.fillR0 = lv.fillR0;
  v.fillC1 = lv.fillC1;
  v.fillR1 = lv.fillR1;
  v.pxX0 = lv.pxX0;
  v.pxY0 = lv.pxY0;
  v.pxX1 = lv.pxX1;
  v.pxY1 = lv.pxY1;
  v.compX0 = lv.compX0;
  v.compY0 = lv.compY0;
  v.compX1 = lv.compX1;
  v.compY1 = lv.compY1;
  if (lv.kernel) {
    v.kernel = lv.kernel;
    v.kernelSpan = lv.kernel->span;
    v.kernelPlanes = lv.kernel->planes;
    v.kernelTaps = (int)lv.kernel->taps.size();
  }
  return v;
}

// Every remaining stage of the current level, over the whole level. The convolution and the composite
// are row-ranged even here, because the whole level *is* their row range; the fill is the stage that
// cannot be partitioned - a max filter needs the whole line it filters - and it runs *between*
// extraction and the convolution, because what it lifts is the extracted coverage (fillLevel).
void LayeredRender::runLevel()
{
  if (!m_haveLevel)
    return;
  extractRows(m_lv.compY0, m_lv.compY1);
  fillLevel();
  blurRows(m_lv.compY0, m_lv.compY1);
  compositeRows(m_lv.compY0, m_lv.compY1);
}

// ---------------------------------------------------------------------------
// Out
// ---------------------------------------------------------------------------

// Straight colour out of the accumulator, alpha out of the coverage and weight sums. Three cases, and
// each one is the only answer that keeps the plane's own contract:
//
//   nothing composited here (accA == 0)  -> alpha 0, black: this window holds no content.
//   alpha to write (outA > 0)            -> divide by the accumulated over-alpha to get *straight*
//                                          colour, and write it with the alpha the render actually
//                                          has: `1 − Π(1−aᵢ)` is the blend's own transparency, and
//                                          Σcoverage / Σweight is the source's, defocused. The former is
//                                          what made the colour right, the latter is the one that is
//                                          honest about the pixel. Colour is written **straight**, not
//                                          premultiplied, because that is the plane contract (header
//                                          note) and the only convention under which the identity
//                                          path and this one are the same answer to the same picture:
//                                          a fully in-focus, half-transparent input comes back exactly
//                                          as it went in, and a host that wants premultiplied pixels
//                                          does the last multiply itself (the `unpremult` knob).
//   straight colour but no alpha         -> keep the premultiplied blend and write alpha 0, because a
//                                          colour with no density cannot be called anything else.
void LayeredRender::writeOutput(OutputPlane &out, int rowStart, int rowEnd) const
{
  if (out.empty() || out.comp <= 0 || m_acc.empty())
    return; // no accumulator means no composite ran: the identity path uses copyInputThrough instead
  const int stride = out.comp;
  const int rectW = m_rect.width();
  const int yA = std::max(rowStart, m_rect.y1), yB = std::min(rowEnd, m_rect.y2);
  if (yB <= yA)
    return;

  for (int y = yA; y < yB; ++y) {
    float *orow = out.row(y);
    if (!orow)
      continue; // this scanline is not delivered: there is nothing to write it into
    const float *acc = &m_acc[((size_t)(y - m_rect.y1) * (size_t)rectW) * 4u];
    const float *accW = &m_accW[((size_t)(y - m_rect.y1) * (size_t)rectW) * 2u];
    for (int x = m_rect.x1; x < m_rect.x2; ++x) {
      if (x < out.x1 || x >= out.x2)
        continue; // the output plane is not obliged to cover the whole window
      const int off = (x - m_rect.x1);
      const float *a = acc + (size_t)off * 4u;
      const float *w = accW + (size_t)off * 2u;
      float *o = &orow[(size_t)(x - out.x1) * (size_t)stride];

      const double accA = (double)a[3]; // what the layers composited here actually covers, accumulated
                                        // as an alpha rather than recovered as `1 - T`: a defocused
                                        // silhouette's outer penumbra is exactly where this number is
                                        // tiny, and float32 `1 - T` would carry none of its digits (D-49)
      double outA = (double)w[1] > 0.0 ? (double)w[0] / (double)w[1] : 0.0;
      if (outA < 0.0)
        outA = 0.0;
      else if (outA > 1.0)
        outA = 1.0; // coverage never exceeds weight, so this is only ever trimming an ulp

      double r, g, b;
      if (accA <= 0.0) {
        r = g = b = 0.0;
        outA = 0.0;
      } else if (outA > 0.0) {
        const double inv = 1.0 / accA; // straight, per the note above: the alpha that travels with
        r = (double)a[0] * inv; // the colour is the honest one (outA), and a host that wants
        g = (double)a[1] * inv; // premultiplied pixels does the last multiply with that alpha
        b = (double)a[2] * inv;
      } else {
        r = (double)a[0];
        g = (double)a[1];
        b = (double)a[2];
      }
      if (stride >= 4) {
        o[0] = (float)r;
        o[1] = (float)g;
        o[2] = (float)b;
        o[3] = (float)outA;
      } else if (stride == 3) {
        o[0] = (float)r; // a 3-component output plane has no alpha to carry, so it gets the colour
        o[1] = (float)g; // alone - which is what a 3-component *input* plane is read as too
        o[2] = (float)b;
      } else {
        o[0] = (float)straightLuma(r, g, b); // a 1-component output plane is grey, as a 1-component
                                             // input plane is grey (and Rec.709 on a grey triple is that
                                             // value exactly, so a grey render stays grey)
      }
    }
  }
}

// The identity fast path: the input, verbatim. Every CoC in the work area is 0, so the composite would
// reproduce the input through a round trip that is exact in the maths but not bit-for-bit in float32
// (extract multiplies, writeOutput divides); copying is both exact and the cheapest thing available.
void LayeredRender::copyInputThrough(OutputPlane &out, int rowStart, int rowEnd) const
{
  if (out.empty() || out.comp <= 0 || !m_image)
    return;
  const Plane &img = *m_image;
  const int stride = img.comp > 0 ? img.comp : 0;
  const int ostride = out.comp;
  const int yA = std::max(rowStart, m_rect.y1), yB = std::min(rowEnd, m_rect.y2);
  if (yB <= yA)
    return;

  for (int y = yA; y < yB; ++y) {
    float *orow = out.row(y);
    if (!orow)
      continue;
    const float *irow = img.empty() ? 0 : img.row(y);
    for (int x = m_rect.x1; x < m_rect.x2; ++x) {
      if (x < out.x1 || x >= out.x2)
        continue;
      float *o = &orow[(size_t)(x - out.x1) * (size_t)ostride];
      if (!irow || x < img.x1 || x >= img.x2 || stride <= 0) {
        for (int k = 0; k < ostride; ++k)
          o[k] = 0.0f; // the input does not deliver this pixel: it is not there
        continue;
      }
      const float *p = &irow[(size_t)(x - img.x1) * (size_t)stride];
      double r, g, b;
      if (stride >= 3) {
        r = p[0];
        g = p[1];
        b = p[2];
      } else {
        r = g = b = (double)p[0]; // a 1-component input plane is grey, as extraction reads it
      }
      if (ostride >= 4) {
        o[0] = (float)r;
        o[1] = (float)g;
        o[2] = (float)b;
        o[3] = stride >= 4 ? p[3] : 1.0f; // an input with no alpha channel is fully opaque
      } else if (ostride == 3) {
        o[0] = (float)r; // a 3-component output plane has no alpha to carry: the colour alone,
        o[1] = (float)g; // which is the same reading writeOutput makes
        o[2] = (float)b;
      } else {
        // grey out of a grey input is that value; out of a colour input it is the luma
        // writeOutput would write, so the two paths read a 1-component plane the same way
        o[0] = stride >= 3 ? (float)straightLuma(r, g, b) : p[0];
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

size_t LayeredRender::bytes() const
{
  size_t n = m_coc.coc.size() * sizeof(float) + m_coc.side.size() * sizeof(unsigned char) + m_acc.size() * sizeof(float) + m_accW.size() * sizeof(float);
  n += m_lv.gA.size() * sizeof(float) + m_lv.gB.size() * sizeof(float);
  return n;
}

size_t LayeredRender::peakBytes() const
{
  return m_plan.peakBytes;
}

} // namespace zd
