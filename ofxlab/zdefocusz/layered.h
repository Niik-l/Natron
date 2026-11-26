// ZDefocusZ: the layered renderer - convolving the layers and compositing them back to
// front (DECISIONS.md D-35's composite, implemented here as D-38).
//
// OFX-free like zd.h and bokeh.h (D-34): the whole render is checked headlessly
// (tests/zd_layered_test.cpp) against a brute-force full-resolution convolution and
// against analytic multi-plane depth scenes before a plugin ever asks a host for a pixel.
//
// ---------------------------------------------------------------------------
// Why layers at all
// ---------------------------------------------------------------------------
// A single per-pixel-radius convolution is not a defocus: it has no *order*. Where a
// near object meets a far one, a per-pixel blur smears the far object's colour across the
// silhouette, because the far colour was what the camera recorded *behind* the near one
// only for the pixels the near object never covered - and every other pixel holds the
// near colour, which a per-pixel blur happily spreads into the background.
//
// So the render is Nuke ZDefocus's structure: assign every pixel to a **layer** with one
// blur size each, convolve each layer with that layer's kernel, composite the layers
// **back to front**. Ordering survives because a layer's convolved colour exists only
// where that layer had coverage - the far layer's copy of a pixel covered by a near
// object has coverage 0 there and contributes nothing, blurred or not.
//
// ---------------------------------------------------------------------------
// The composite is premultiplied, and that is the whole occlusion model
// ---------------------------------------------------------------------------
// Each layer is carried as a premultiplied pair: a colour field (image × weight × alpha)
// and a coverage field (weight × alpha), both convolved with the *same* layer's kernel -
// the colour's stencil and the coverage's stencil are one aperture, which is what a lens
// does to light and to a silhouette at the same time. Over-compositing **back to front** -
// each incoming layer sits in front of everything accumulated so far - is the source-over rule:

//     acc = layerCol + acc · (1 − layerAlpha)          (layerCol is premultiplied)
//     A   = 1 − Π (1 − layerAlpha), starting at 0

// `A` is the accumulated alpha: the fraction of that pixel the layers composited so far cover,
// and `1 − layerAlpha` is the light still coming through. The accumulator stores **A**, not the
// transmission: the straightening at the end divides by A, and a penumbra is exactly where A is
// tiny, so computing it as `1 − T` from a float32 `T` near 1 would throw away every relative
// digit of the answer. That is not a hypothetical — it was measured at 0.77 % on an opaque
// plate's outer fringe, where A is about 1e-6 and float32 `T` is 1 − 1e-6 (D-49). Accumulating A
// adds the same complement per layer and keeps its relative precision at any size.
//
// `1 − layerAlpha` is *not* what dims the accumulated colour - a layer dims what is behind it by
// its own opacity, whatever was already accumulated. The rule does everything else for free:
//
//   - Weights sum to 1 per pixel (zd.h's soft seam, D-35), so the *unconvolved* composite
//     is exactly the input picture: a fully in-focus scene reproduces its input.
//   - At a defocused silhouette both layers' coverage falls to a penumbra, and the
//     composite blends exactly those two colours in exactly the ratio the two apertures
//     cover that pixel with. That is the defocus edge, not an approximation of it.
//   - A near layer occludes the deeper layers' copies of its own pixels automatically,
//     because occlusion *is* coverage. There is no occlusion knob: the weights are it.
//
// Each grid cell therefore carries five numbers, and every one of them is convolved with
// the *same* layer kernel: premultiplied R, G, B, the **coverage** (alpha × weight, which
// is what occludes) and the **weight** (weight alone, which is what that coverage would
// have been had the source been opaque). The weight plane exists for one reason, and it is
// the reason `fill_foreground` is not enough: the ordered colour blend below is exact, but
// over-compositing two *partial* coverages gives an alpha below 1 wherever two layers'
// penumbrae overlap (0.5 over 0.5 is 0.75), so a defocus of a fully opaque picture would
// come out with a translucent halo along every defocused silhouette. The output alpha is
// therefore the coverage sum divided by the weight sum - "the input's alpha, defocused" -
// which is exactly 1 wherever the source is opaque, 0 where it is empty, and cannot exceed
// 1 because coverage is never above weight, cell by cell and tap by tap.
//
// Each pixel is assigned to a **side** of the focal plane as well as a level, because the
// two sides are two separate ladders (Nuke: "up to 256 on each side of the focal plane").
// The composite order is then: far ladder from the largest CoC down, the in-focus level,
// near ladder from the smallest CoC up - which is depth order, given that on each side CoC
// grows monotonically toward the camera and toward infinity. CoC *magnitude* alone cannot
// order a near pixel against a far one; the side can, and it comes from the sign of
// (mapped distance − focusValue).
//
// ---------------------------------------------------------------------------
// Cost: the level's kernel is gathered from a cell-reduced field
// ---------------------------------------------------------------------------
// A layer is gathered from its pixels **already reduced**: the extraction stage sums the
// pixels inside each `cell` px cell (the same `cell` the layer's kernel was built for, D-36)
// and convolves that lattice, which is then read back bilinearly at full resolution. There is
// no full-resolution copy of a layer anywhere in the render. The tap count is therefore
// ~constant whatever the radius is (bokeh.h's coarsestCell is what picks `cell` so the shape
// survives the reduction), and the reduction, the kernel and the read-back all live on a
// lattice anchored to **absolute** pixel coordinates - never to the render window - so a
// per-pixel result is identical under any partition of it (D-31's guarantee, which the suite
// re-asserts here rather than trusting).
//
// Stages run level-major: extract, fill, convolve, composite, in that order per level, and every one of
// them except the fill takes a row range the host may parallelise. A row range names **output** rows,
// and each stage reaches back from them by its own halo (a convolved cell needs the cells its kernel
// reaches; a cell average needs the pixels inside it; the fill dilates the cells extraction just wrote),
// so any partition of the output rows leaves the same cells holding the same values. Compositing is
// the one stage that must run exactly once per level - it reads the accumulator - which the
// level-major loop already guarantees.
//
// ---------------------------------------------------------------------------
// fillForeground: the disocclusion band
// ---------------------------------------------------------------------------
// Defocusing an opaque near object thins its coverage at the silhouette and lets deeper layers show
// through the penumbra - correct for a real lens, and exactly the thing Nuke's `fill_foreground` exists
// to reduce ("it can add things that are not there"). Here it is a **dilation of the layer's own
// coverage** by the layer's own blur radius: the coverage a layer could have supplied had its content
// not thinned out. It never lowers a coverage, it changes nothing where coverage was already full -
// which is what makes it assertable without a picture - and the layer's weight follows that coverage
// only as far as it needs, so the output alpha stays where the layer's own aperture put it.
//
// What it dilates is the coverage *before* the layer's kernel has run, and that order is the difference
// between a fill that reads as a defocus and one that reads as a stencil: the level's own aperture is
// then what spreads the lifted coverage, so the band it adds falls off like the rest of the defocus
// instead of keeping the hard edge a max-filter over a blurred field leaves. A lifted cell takes the
// coverage-weighted colour of the window it was lifted from - an average of colours that each still
// describe real pixels of the layer's own content - so no cell of a filled defocus can carry a colour
// the scene it is defocusing does not contain.
//
// It may lift a cell only if the **delivered image** covers that cell whole (D-48). The distinction is
// between reading and writing: a cell the plate's edge cuts holds the coverage of the part of it that
// was delivered, divided by the cell's whole area (@ref extractRows), so lifting *it* hands out coverage
// for the part nobody delivered, and a cell just outside the picture is precisely why the last cell
// inside it deserves the lift. Before the bound the fill wrote the whole window it was allowed to read,
// and a host saw the difference as a fringe on the padding it had allocated. Measured headlessly
// (zd_layered_test section E-1): 1403 lifted cells sit outside the plates it prices, 0 survive the
// bound, and every one of the 1403 was outside the delivered box - on the disc plate at CoC 40 (the
// picture _bin/clips/bokeh_disc.exr is, at the same knobs the probe renders it with) the fringe outside
// the plate goes from 97896 values reaching 57 px to 76816 reaching 42 px, which is still more than the
// 10212 values at 17 px the same render makes with the knob off because the fill's job is real on a
// plate whose alpha disc is smaller than its depth disc, and still inside the 50 px a level of that
// radius gathers either way. On a plate whose depth is one value everywhere the bounded fill is inert:
// E-1's flat grey plate moves 0 of 94864 values with the knob on and off, and its own halo, 28212
// values at 44 px, is the whole fringe either way. Arriving *fully covered* is not that condition: the
// probe's 1-channel Z ramp is opaque by the host's own convention and the knob still moves it, because
// a ramp assigns its pixels to different levels and each level's coverage grid is full of holes where
// the levels meet (offscreen probe section 9, D-48).
#ifndef OFXLAB_ZDEFOCUSZ_LAYERED_H
#define OFXLAB_ZDEFOCUSZ_LAYERED_H

#include <vector>

#include "bokeh.h"
#include "zd.h"

namespace zd {

// ---------------------------------------------------------------------------
// Knob ranges, declared once (the D-33/D-34 precedent: descriptor range == model clamp).
// Bloom follows Nuke's record (references/zdefocus/nuke_zdefocus_knobs.md): a threshold in
// the *encoded* luma domain - which is why it stays in 0..1 whatever the working space is -
// and a gain. `bloomGamma` is a bool in the record, so its value is a constant here.
// ---------------------------------------------------------------------------
const double kBloomThresholdMin = 0.0, kBloomThresholdMax = 1.0;
const double kBloomGainMin = 1.0, kBloomGainMax = 100.0;
const double kBloomGamma = 2.2; // Nuke's bloom_gamma runs "a 2.2 gamma" through the blur

// The dither knob is a percentage in the descriptor (like TimeWarp's mix knobs) and half a
// *local* level step at 100 %: zd.h's ditheredCoC already scales by the spacing of the seam
// a pixel sits in, so this is only the dial that says "how much of that".
const double kDitherMax = 100.0;

// The sprite radius cap: the largest radius a **kernel is built for** (D-45). It is a cost knob and
// not a look knob, and its range is deliberately the whole CoC range so that "uncapped" is a value
// inside the range rather than a special case. What it bounds is everything derived from a level's
// kernel — its cell, its ring, the gather the render asks a host for, and therefore the padded CoC
// map — all of which grow with the widest radius the kernels are built for: levelMargin is a fill
// radius plus a kernel half-span plus two, in cells of a lattice whose cell is ~radius/7, which makes
// the padding a little over twice the radius. Measured (zd_layered_test section I): a 4096 px level
// pads 5850 px a side with fill off and 9945 px with fill on (cell 585, a 17 cell ring); a 512 px cap
// pads 730 / 1241 px — 0.12 of it. Small radii pad proportionally more, because the ring is a whole
// number of cells: 4 px of padding for a 1 px kernel (cell 1, span 3).
const double kSpriteRadiusCapMin = 0.0, kSpriteRadiusCapMax = kCoCMax;

/** @brief The composite half of the knob table: everything that is not depth (zd.h) and
 * not shape (bokeh.h). `layers`/`layerSpacing`/`layerCurve` are read by the plugin when it
 * builds the @ref LevelSet it hands to @ref LayeredRender; they live here so one struct
 * carries every knob the renderer itself consults. */
struct LayerOptions {
  int layers = 0;             // 0 = autoLayerCount(maxBlur, layerSpacing)
  double layerSpacing = 4.0;  // px between levels when the count is derived
  double layerCurve = 1.0;    // 1 = even; > 1 packs levels near the focus plane (D-35)

  double dither = 100.0;      // percent of half a local level step (zd.h's ditheredCoC)
  bool fillForeground = true; // Nuke's fill_foreground, on by default like there

  double spriteRadiusCap = 0.0; // 0 = uncapped: the largest radius a kernel is built for (D-45)

  bool bloom = false;         // boost pixels over bloomThreshold by bloomGain, pre-blur
  double bloomThreshold = 0.8;
  double bloomGain = 2.0;
  bool bloomGamma = false;    // compare the threshold in a 2.2-encoded space

  bool operator==(const LayerOptions &o) const;
};

/** @brief The radius a level's kernel is actually built for: the level's own CoC, or
 * @ref LayerOptions::spriteRadiusCap when that is finite and smaller. Exported because the
 * plugin's gather RoI and its memory estimate must use the same number the renderer uses. */
double cappedRadius(double radius, double cap);

/** @brief Clamp a LayerOptions in place to the ranges above. */
void sanitise(LayerOptions &o);

/** @brief An absolute render window, half-open like an OFX region: pixels
 * [x1,x2) × [y1,y2). */
struct RenderRect {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;

  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
  bool empty() const { return x2 <= x1 || y2 <= y1; }
};

/** @brief The plane pair the renderer reads and writes, addressed by absolute pixel
 * coordinates exactly like TimeWarp's tw::Plane/tw::OutputPlane (motion.h): a scanline a
 * plane does not deliver is absent, not guessed at, and contributes zero. A `Plane` may
 * hold 1, 3 or 4 interleaved components (a 3-component plane has no alpha, and is read with
 * alpha 1; a 1-component plane is grey with full coverage). The output plane takes the same
 * three shapes: RGBA writes colour plus the render's alpha, a 3-component plane takes the
 * colour alone (there is no channel to put the alpha in), a 1-component plane takes the Rec.709
 * luma of the composited colour - which for a 1-component input is that pixel's own value, so a
 * grey render stays grey. Both write paths read component counts the same way, which is what
 * makes the identity path and the composite path the same answer on the same plane.
 *
 * Colour and alpha are **straight** (unpremultiplied) on both sides of a render: the
 * renderer premultiplies internally, because that is the only form in which a coverage
 * weight and a colour weight can be convolved by the same aperture, and un-premultiplies on
 * the way out (so a plugin whose host insists on premultiplied pixels does the last multiply
 * itself, the way SharpenZ and TimeWarp do with their `unpremult` knobs). A 1-component plane
 * carries its values as grey with full coverage, which is what the flat-field tests read. */
struct Plane {
  int x1, y1, x2, y2; // delivered pixel bounds; empty when x2 <= x1 or y2 <= y1
  int comp;           // interleaved floats per pixel (1, 3 or 4)

  Plane() : x1(0), y1(0), x2(0), y2(0), comp(4) {}
  virtual ~Plane() {}

  /** @brief Pointer to pixel (x1, @p y) - the start of that scanline - or 0 when the
   * scanline is not delivered. */
  virtual const float *row(int y) const = 0;

  bool empty() const { return x2 <= x1 || y2 <= y1; }
  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
};

/** @brief Write counterpart of Plane, same addressing. */
struct OutputPlane {
  int x1, y1, x2, y2;
  int comp;

  OutputPlane() : x1(0), y1(0), x2(0), y2(0), comp(4) {}
  virtual ~OutputPlane() {}

  /** @brief Pointer to pixel (x1, @p y), or 0 when the scanline is not delivered. */
  virtual float *row(int y) = 0;

  bool empty() const { return x2 <= x1 || y2 <= y1; }
  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
};

/** @brief Vector-backed Plane/OutputPlane, the stand-in the offline suites render with. */
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

enum LayerSide {
  eSideFar = 0,  // behind the focal plane: composited first, largest CoC first
  eSideNear,     // in front of it: composited last, smallest CoC first
  kNumSides,
  eSideAny = -1  // not a side: the in-focus level's visit, which takes both (see @ref LevelVisit)
};

/** @brief The signed CoC of one raw depth value: the same magnitude @ref cocRadius
 * returns, plus the side of the focal plane it sits on.
 *
 * The side is the sign of (mapped distance − focusValue), which is the only ordering
 * information a depth channel carries: on each side the CoC grows monotonically (toward
 * infinity behind the focal plane, toward the focal point in front of it), so ordering by
 * CoC *within* a side is ordering by depth. Blur mode has no distance to compare, and its
 * channel's own sign is the only ordering a blur map implies: value above `focusValue` is
 * taken as the far side, below it as the near side. A pixel with no geometry (not a
 * positive distance) is far, which is the same conservative reading zd.h's defocusFactor
 * makes when it gives it the CoC at infinity. */
struct SignedCoC {
  double coc;  // px, in [minBlur, maxBlur] exactly as cocRadius()
  int side;    // LayerSide
};
SignedCoC signedCoC(double raw, const FocusOptions &o, double blurSizePx);

/** @brief A CoC map over an absolute area: the magnitude every level assignment reads,
 * and the side every pixel composites on. The map is computed once per render over the
 * *work* area (the render rect padded by the plan's largest kernel reach), because the
 * pixels outside the render window are what a level's blur gathers *from*. */
struct CoCMap {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0; // absolute, half-open
  std::vector<float> coc;
  std::vector<unsigned char> side;

  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
  bool empty() const { return x2 <= x1 || y2 <= y1; }

  float cocAt(int x, int y) const
  {
    if (x < x1 || x >= x2 || y < y1 || y >= y2)
      return 0.0f; // outside the prepared area: in focus, i.e. nothing to gather
    return coc[(size_t)(y - y1) * (size_t)width() + (size_t)(x - x1)];
  }
  int sideAt(int x, int y) const
  {
    if (x < x1 || x >= x2 || y < y1 || y >= y2)
      return eSideFar;
    return (int)side[(size_t)(y - y1) * (size_t)width() + (size_t)(x - x1)];
  }
};

/** @brief Build a CoC map over @p area from a depth plane (1, 3 or 4 components; a 4-component
 * plane is read at component 3, because alpha is not a distance, and the plugin's own channel
 * selector unpacks the channel it picked into a 1-component plane anyway - this model has no idea
 * what a clip is). Rows the plane does not deliver hold no geometry: the CoC at
 * infinity, on the far side. */
CoCMap prepareCoC(const Plane &depth, const RenderRect &area, const FocusOptions &o, double blurSizePx);

/** @brief The pair of levels one pixel contributes to, and the share each one owns: the two
 * bracketing levels of @p levels and the weights zd.h's soft seam gives them (they sum to 1),
 * after @ref LayerOptions::dither has moved the CoC the bracket is taken on.
 *
 * Exported because the plugin's <c>preview = Layers</c> output has to show the assignment the
 * renderer actually performs, not a re-derivation of it: this is the same call @ref LayeredRender
 * makes when it extracts a level's cells, so what a user sees in the preview is what the blur does. */
struct LevelPair {
  int i0, i1;
  double w0, w1;
};

/** @brief @ref LevelPair for one pixel of one map. */
LevelPair pixelPair(double coc, int x, int y, const LevelSet &levels, const LayerOptions &opt);

/** @brief One **level visit**: a level of the level set, and the side of the focal plane whose
 * pixels that visit owns.
 *
 * A level is visited once per side it has content on, because the near and the far pixels that
 * bracket into the same CoC band are *not* at the same distance: compositing them in one pass would
 * blend them, and blending is not ordering. Visiting the far ladder first, then the in-focus level,
 * then the near ladder puts every pixel of the map in depth order, which is what "up to 256 levels
 * on each side of the focal plane" means.
 *
 * The in-focus level is the one visit that takes both sides (<c>eSideAny</c>): its radius is 0, its
 * kernel is a single tap of 1, and at cell 1 the bilinear weights come out 1 and 0 exactly, so an
 * in-focus pixel reads exactly its own cell and one visit cannot move content across a pixel or mix
 * the two ladders. */
struct LevelVisit {
  int level = 0;      // index into the level set
  int side = eSideAny; // LayerSide, or eSideAny for the in-focus level
};

/** @brief One level visit's assignment geometry, known after prepare() and before any buffer is
 * allocated: the absolute pixel box that holds the pixels this visit owns, and the bytes the visit
 * will need (levelGeom() derives both, and beginLevel() calls the same function, so the reported
 * peak is the allocation). */
struct LevelRect {
  int level = 0;
  int side = eSideAny;
  int xLo = 0, yLo = 0, xHi = -1, yHi = -1; // empty when xHi < xLo
};

/** @brief What a render will do, derived once: the per-side level count, the largest CoC
 * the map can reach, the reach the work area must be padded by, whether the whole render is
 * provably a copy of the input (every CoC 0 - the identity fast path, which is exact rather
 * than a convolution with an identity kernel), and the per-level boxes the levels will
 * actually touch. */
struct RenderPlan {
  int levels = 0;        // levels above the in-focus level, per side (levelSet.count()-1)
  double maxCoC = 0.0;   // the largest CoC in the prepared map
  double maxReach = 0.0; // px *outside* the render rect that some level gathers from: the widest
                         // level's ring, in px (levelMargin * cellForRadius). Pads the CoC map, and
                         // is the number the plugin hands its host as the RoI, so the two cannot
                         // disagree
  bool allSharp = false;
  std::vector<LevelRect> levelRects; // one per level *visit*, in levelOrder() order: a visit with no
                                     // pixel anywhere is not in the list at all, so its size is the
                                     // number of beginLevel() calls the render makes
  size_t peakBytes = 0;
};

/** @brief The cell (integer px per tap) a layer of @p radius gathers at: the coarsest
 * cell bokeh.h measures as still resolving the shape, floored to an integer because the
 * reduction uses an integer factor, and never below 1 (a radius-0 level gathers at cell 1
 * so the in-focus layer stays exactly itself). The cell is a property of the **kernel**, so @p cap is applied
 * first: a level whose radius the cap clips is reduced on the lattice of the kernel that was built,
 * not of the CoC that was measured (D-45). @p cap 0 is uncapped. */
int cellForRadius(double radius, double aspect, double cap = 0.0);

/** @brief The ring width, in cells, that one level of @p radius needs around its convolved box:
 * @ref LayerOptions::fillForeground's dilation radius + the kernel's half span + the one cell bilinear
 * read-back samples + one for safety. Exported because a plugin must ask its host for at least as much
 * as the renderer will then read (see @ref maxKernelReach), and because @ref LevelGeom takes its ring
 * from here, so the padding advertised is the padding allocated. The ring is counted in the cells of @p cap's kernel, so it is the ring the level
 * really walks, and a capped level shrinks it. @p cap 0 is uncapped. */
int levelMargin(double radius, double aspect, bool fill, double cap = 0.0);

/** @brief The widest reach of a level set, in px **outside** the render rect: the largest
 * @ref levelMargin over the set, in cells, times that level's cell. @ref prepare pads its CoC map by
 * exactly this, and a plugin hands its host the same number for the RoI, the RoD and the memory
 * estimate, so no host is ever asked for less than the render reads. Measured against the level set's
 * radii - the sizes the kernels are built for - and not against maxBlur, and those radii are capped by @p cap first because the gather is a property
 * of the kernel that exists. @p cap 0 is uncapped. */
double maxKernelReach(const LevelSet &levels, double aspect, bool fill, double cap = 0.0);

/** @brief The layered renderer.
 *
 * The plugin's render path drives it stage by stage so the host can parallelise *inside*
 * each stage (the TimeWarp precedent: a serial prepare, then row-parallel work):
 *
 *     prepare()                         build the CoC map + plan
 *     if (plan().allSharp) copyInputThrough()   the whole render, verbatim
 *     else for each visit in levelOrder():
 *         beginLevel(visit)
 *         extractRows()                               (row-ranged: image -> cells)
 *         fillLevel()                                 (whole level: a max-filter needs a whole
 *                                                      row of a row, by its nature - and it lifts
 *                                                      cells, not a blur, so it sits before one)
 *         blurRows()                                  (row-ranged: cells -> convolved)
 *         compositeRows()                             (row-ranged, once per visit)
 *         finishLevel()
 *     writeOutput()                       once, after the last visit: the accumulator holds
 *                                         the whole composite, and the identity path above
 *                                         replaces it
 *
 * with every row-ranged stage taking **output** rows and reaching back by its own halo, so
 * any partition of the row range leaves the same cells holding the same values - which the
 * suite re-asserts here rather than trusting (D-31). Within a level the stages must run in
 * that order; the level visits run in levelOrder() (far ladder largest-CoC first, the in-focus
 * level, near ladder smallest-CoC first), one visit per side a level has content on. fillLevel() is
 * the one stage that runs over a whole level, because dilation is a max-filter and a max-filter over
 * a row needs that whole row to exist - and it runs *before* blurRows(), because what it lifts is the
 * coverage the level extracted, so the level's own aperture is what turns a lifted step into a penumbra
 * (@ref fillLevel).
 *
 * `aspect` is read from the SpriteLibrary's own options, because the kernel library owns
 * the shape and the renderer must pad by exactly the reach that shape has (D-36: a
 * radius-based window silently truncates an aspect-squeezed aperture's area). */

/** @brief What the current visit's geometry looks like from *outside* the renderer: the same numbers
 * @ref LayeredRender::Level holds, read out. It exists because the GPU path (gpu.h) and
 * tests/gpu_parity.cpp launch a stage with the renderer's own lattices, boxes and reach rather than
 * recomputing them — which is the whole point of that route: the plan a host measures stays
 * LayeredRender's (DECISIONS.md D-51). A view is valid only between a @ref LayeredRender::beginLevel
 * that returned true and the next call that changes the current level; before the first it reports
 * `valid == false` with every number at its default. */
struct LevelView {
  bool valid = false;
  int level = 0;            // index into the level set
  int side = eSideAny;      // which side of the focal plane the visit owns (eSideAny: both)
  double radius = 0.0;      // the CoC this level blurs with, after spriteRadiusCap
  int cell = 1;             // px per cell
  int half = 0;             // built kernel's half span, in cells
  int dilate = 0;           // fillForeground's radius, in cells
  int pad = 1;              // how far the convolved box reaches past what the composite reads
  int margin = 0;           // the grid's zero ring, in cells

  int gX0 = 0, gY0 = 0, gCols = 0, gRows = 0;             // the reduced grid, zero ring included
  int convC0 = 0, convR0 = 0, convC1 = 0, convR1 = 0;     // cells the convolution computes
  int fillC0 = 0, fillR0 = 0, fillC1 = 0, fillR1 = 0;     // cells fillLevel may write
  int pxX0 = 0, pxY0 = 0, pxX1 = 0, pxY1 = 0;             // the pixels that range covers
  int compX0 = 0, compY0 = 0, compX1 = 0, compY1 = 0;     // rect pixels this level composites

  int kernelSpan = 0, kernelPlanes = 0, kernelTaps = 0;   // the built kernel, as the tap loop sees it
  const Kernel *kernel = 0;                                // owned by the SpriteLibrary
};

class LayeredRender {
public:
  LayeredRender(const LayerOptions &layers, SpriteLibrary &sprites, const LevelSet &levels);

  /** @brief Everything the render needs from the depth half of the knobs, resolved once:
   * the CoC map over the work area, and the plan. `blurSizePx` is
   * zd::resolveBlurSize(options, imageWidthPx) - the renderer cannot know the image width.
   * Returns false when the rect is empty (nothing to render, nothing allocated). */
  bool prepare(const Plane &depth, const Plane &image, const RenderRect &rect, const FocusOptions &focus, double blurSizePx);

  const RenderPlan &plan() const { return m_plan; }
  const CoCMap &cocMap() const { return m_coc; }
  const RenderRect &rect() const { return m_rect; }

  /** @brief The composite order, as level visits: the far ladder in descending radius, then the
   * in-focus level (one visit, both sides), then the near ladder in ascending radius - a radius that
   * is bigger on the near side is *nearer* to the camera, so it composites later. Only the visits
   * with content anywhere in the work area are listed, in the same order as
   * @ref RenderPlan::levelRects. Empty when no pixel of the work area joined any level at all; an
   * all-sharp scene still lists the in-focus level, and @ref RenderPlan::allSharp is what tells the
   * caller to use @ref copyInputThrough instead of walking the order. */
  const std::vector<LevelVisit> &levelOrder() const { return m_order; }

  /** @brief The identity fast path: write @ref image out verbatim (straight, as it arrived).
   * Every CoC in the plan is 0, which means every rect pixel belongs to the in-focus level
   * with weight 1 and coverage 1, which means the composite would reproduce the input exactly -
   * so it is copied instead, which is both bit-exact by construction and the whole point of
   * the fast path. Row-ranged like every other stage. */
  void copyInputThrough(OutputPlane &out, int rowStart, int rowEnd) const;

  // -- one level visit: begin, three row-ranged stages, one whole-level pass ----
  /** @brief Start one level visit: allocates its buffers and reads the pixels assigned to it.
   * Takes a visit from @ref levelOrder() (its level index and side), and returns false when the
   * visit has no pixel anywhere the level can reach - which is the same test the plan used to leave
   * it out of the order, so a caller that walks levelOrder() never sees false. */
  bool beginLevel(const LevelVisit &visit);
  /** @brief Build this level's reduced field straight out of @ref image: for every cell of
   * its lattice, the box average of the premultiplied colour and coverage that this level
   * owns of the pixels inside that cell (zd.h's soft seam, D-35), counting only the pixels on
   * this visit's side of the focal plane, with bloom applied *before*
   * the convolution (Nuke's highlights: a boost the blur then spreads). The reduction is
   * folded into the extraction, so the renderer never holds a full-resolution copy of a level.
   * Takes **output** rows: the cells a later stage will need are reached back by this stage's
   * own halo, so any partition of the output rows fills the same cells with the same values. */
  void extractRows(int rowStart, int rowEnd);
  /** @brief Convolve the reduced field with this level's kernel: one kernel plane per colour
   * channel when the kernel has per-channel planes (D-37), and the kernel's own coverage
   * plane for the coverage *and* the weight - one aperture, five planes. What it convolves is the
   * field @ref fillLevel left, when that knob is on: the fill lifts cells of this lattice, and this
   * stage is what turns a lifted step into a defocused edge. */
  void blurRows(int rowStart, int rowEnd);
  /** @brief fillForeground: dilate this level's own **extracted** coverage by the window its blur
   * radius spans, give every cell that dilation lifts the coverage-weighted colour of the same
   * window, and raise its weight only as far as that coverage needs (the header note, and the
   * pass's own note in layered.cpp, give why each half is there). Runs between @ref extractRows and
   * @ref blurRows, so the level's own aperture is what spreads the lifted coverage. A no-op when
   * fillForeground is off or the level is the in-focus one.
   *
   * It may only lift a cell the **delivered image** covers whole. The cells the pass *reads* are the
   * same window whatever that bound does — a cell just outside the picture is exactly why the last
   * cell inside it is worth filling — but writing outside the delivered box invents coverage for
   * pixels nobody delivered, and a host measures that as a fringe on the padding it allocated. The
   * bound is @ref LevelGeom's fill box, and it costs nothing the knob was asked to do: measured
   * headlessly (zd_layered_test section E-1), it removes 1403 lifted cells across the three knob sets
   * it prices, every lift it removes sits outside the plate, and a plate whose depth is one value
   * everywhere loses none at all. A plate that merely arrives *covered* is not such a plate: a Z ramp
   * is opaque and every one of its levels has holes to lift. DECISIONS.md D-48. */
  void fillLevel();
  /** @brief Over-composite this level into the accumulator (premultiplied, back to front):
   * `acc = col + acc·(1 − a)` for the colour, with this level's coverage and weight sums
   * added alongside (the header note explains why the output alpha needs both). */
  void compositeRows(int rowStart, int rowEnd);
  /** @brief Release this level's buffers. */
  void finishLevel();

  /** @brief Run every remaining stage of the current level over the whole level. A
   * convenience for hosts that do not drive the row ranges (and for the suites). */
  void runLevel();

  /** @brief Write the composited result to @p out over [@p rowStart, @p rowEnd): straight
   * colour out of the accumulator, alpha out of the coverage and weight sums (see the header
   * note). The identity fast path ends up in the same code, which is why a fully in-focus
   * render reproduces its input exactly rather than approximately. */
  void writeOutput(OutputPlane &out, int rowStart, int rowEnd) const;

  /** @brief Bytes held by the render right now: the CoC map plus the current level's buffers.
   * OFX 1.4 has no memory *accounting* suite to report this to (ofxsMemory.h offers only an
   * allocator), so a plugin uses the number the other way round: it compares @ref peakBytes with
   * its own cap and splits the render rect when the render would not fit (DECISIONS.md D-40). */
  size_t bytes() const;
  /** @brief Bytes the plan will hold at its peak, known after prepare() and before any
   * level is allocated. */
  size_t peakBytes() const;

  /** @brief The convolved cell rows this level must have ready for output rows
   * [@p rowStart, @p rowEnd): the rows the cells those pixels read live on, widened by
   * @ref Level::pad and clamped to the rows this level convolves, then widened by @p halo -
   * how far *this* stage reaches outside them (a convolution reads `half` cells of extracted
   * field; extraction fills the cells a convolution reads). Empty when @p lo > @p hi, so a
   * caller's loop runs zero times.
   *
   * Public because the GPU path launches a stage over *exactly* the cells the CPU stage of the
   * same name would have written (gpu.h): a device that computed a different set of cells would be
   * a device with a different render, and this is the one place that set is decided. */
  void cellRowsFor(int rowStart, int rowEnd, int halo, int *lo, int *hi) const;

  // -- the current visit, seen from outside (gpu.h and tests/gpu_parity.cpp) --------------
  /** @brief The current level's geometry exactly as @ref beginLevel computed it. Cheap: a copy of
   * plain integers plus the kernel pointer the SpriteLibrary owns. */
  LevelView levelView() const;

  /** @brief The layer knobs this renderer was constructed with, and the level set it draws its
   * ladders from, and the aspect ratio its kernels were built for. The GPU path needs all three to
   * transcribe the level assignment (the dither, the seams) into a kernel. */
  const LayerOptions &layerOptions() const { return m_opt; }
  const LevelSet &levelSet() const { return m_levels; }
  double aspect() const { return m_aspect; }

  /** @brief The picture @ref prepare was given, and its component count (1, 3 or 4): the pixels
   * *this renderer* would have read, which is what the GPU uploads. The plugin owns the plane's
   * lifetime for the whole render. */
  const Plane &image() const { return *m_image; }
  int imageComponentCount() const { return m_imgComp; }

  /** @brief The current level's two grids, and the render's accumulator: handed out by reference so
   * the GPU path can round-trip them (@ref fillLevel stays on the CPU, which is why the extracted grid
   * goes device -> CPU -> device) and so the parity suite can compare one stage at a time.
   *
   * The grids are 5 floats per cell (premultiplied R, G, B, coverage, weight) at @ref LevelView's
   * @c gCols by @c gRows; the accumulator is 4 floats per rect pixel in @ref accum and 2 in
   * @ref accumWeight, addressed from the rect's first pixel. */
  std::vector<float> &gridA() { return m_lv.gA; }
  std::vector<float> &gridB() { return m_lv.gB; }
  std::vector<float> &accum() { return m_acc; }
  std::vector<float> &accumWeight() { return m_accW; }
  bool haveLevel() const { return m_haveLevel; }

private:
  struct Level {
    int index = 0;   // index into the level set
    int side = eSideAny; // which side of the focal plane this visit owns (eSideAny = both)
    double radius = 0.0;
    int cell = 1;    // px per cell this level gathers at (cellForRadius)
    int half = 0;    // kernel half span, in cells
    int dilate = 0;  // fillForeground's radius, in cells (0 = no dilation for this level)
    int pad = 1;     // how far the convolved cell box reaches past the cells the composite reads:
                     // always 1, the bilinear read-back, which samples the cell on the far side of the
                     // cells it writes. The fill does not widen this box: it dilates the *extracted*
                     // cells, and the convolution is what spreads them (see the note above)
    const Kernel *kernel = 0;

    // Absolute cell lattice: cell c covers absolute pixels [c*cell, (c+1)*cell), and its
    // centre sits at absolute coordinate c*cell + cell/2 - which is what the composite reads.
    // Every number below derives from absolute coordinates, never from the render window, so
    // a per-pixel result cannot depend on how the window is partitioned (D-31).
    int convC0 = 0, convR0 = 0;   // first cell the convolution computes
    int convC1 = 0, convR1 = 0;   // last one, inclusive: widened by `pad` so the fill has extracted
                                  // cells to dilate *from* at the very edge *and* every cell bilinear
                                  // read-back samples was convolved (a cell outside this box holds the
                                  // fill's scratch, and no stage that composites reads it)
    int fillC0 = 0, fillR0 = 0;   // the cells `fillForeground` may *write*: the convolved box widened
    int fillC1 = 0, fillR1 = 0;   // by `half`, cut to the cells the delivered image plane covers whole
                                  // (inverted when the plane is narrower than a cell, which means the
                                  // pass does nothing at all - see fillLevel, D-48). It is narrower than
                                  // the convolved box whenever the host delivered less than this level's
                                  // window, and it says nothing about what the pass may *read*
    int gX0 = 0, gY0 = 0;         // first cell of the grid (convC0 - margin, convR0 - margin)
    int gCols = 0, gRows = 0;     // cells in it, zero ring included
    int margin = 0;               // ring width in cells: dilate + half + 2

    int pxX0 = 0, pxY0 = 0;       // the level's cell box, widened by the kernel half *and* by the fill
    int pxX1 = 0, pxY1 = 0;       // radius, expanded to pixels: what the extraction stage reads from
                                  // (cell-aligned), because a lift is computed from cells the
                                  // convolution itself does not read

    int compX0 = 0, compY0 = 0;   // the rect pixels this level composites: [compX0,compX1)
    int compX1 = 0, compY1 = 0;   // by [compY0,compY1)

    std::vector<float> gA, gB;    // reduced grids: 5 planes each (premultiplied R, G, B,
                                  // coverage, weight)
  };

  // A reduced grid: the convolved cell box plus a ring of exact zeros `margin` cells wide, so
  // the tap loop can reach half a span outside the box and the bilinear read-back one cell
  // outside it without special-casing an edge. Outside means "no content", never the nearest
  // value: a render field has no picture border to extend, unlike a bokeh picture (D-37).
  /** @brief Address one cell of a level's reduced grid: cell index (@p col, @p row) in the
   * level's own absolute lattice, plane 0..4 (premultiplied R, G, B, coverage, weight). */
  float *cellAt(std::vector<float> &g, int col, int row, int p) const;

  /** @brief Max-filter over every line of the *current* level's grid in direction (@p dx, @p dy),
   * centred window 2*<c>Level::dilate</c>+1 cells, zero outside the grid: reads the cell plane @p src
   * with the stride @p srcStride and accumulates the max into the plane @p dst with the stride
   * @p dstStride, over the whole grid. Never in place along a direction, which is what makes the passes
   * of fillLevel() order-independent. @p line and @p filt are scratch the caller owns (a line and its
   * filtered copy: a max-filter read in place would compare already-filtered values). */
  void dilateLines(const float *src, size_t srcStride, float *dst, size_t dstStride, int dx, int dy, std::vector<float> &line, std::vector<float> &filt) const;

  /** @brief Sums over the window @ref dilateLines maxes - the union of the four centred families, each
   * cell counted once - of the current level's extracted cell: premultiplied R, G, B and coverage. The
   * colour @ref fillLevel gives a lifted cell is these sums' ratio, so the colour and the coverage a
   * fill invents come out of the same window (@ref fillLevel's note says why they must). */
  void fillWindowSums(int col, int row, float sums[4]) const;

  /** @brief Max over the window of @p span cells ending at each cell, amortised O(1) per cell
   * (the decreasing-deque max-filter). @p span <= 1 copies. The *centred* window dilation
   * wants is this filter shifted by half a span, which is what dilateLines() arranges. */
  void slidingMaxRow(const float *src, float *dst, int count, int span) const;

  LayerOptions m_opt;
  SpriteLibrary &m_sprites; // non-const: kernel() builds and caches, so it is not const
  const BokehOptions &m_bokeh;
  LevelSet m_levels;
  double m_aspect;

  const Plane *m_image; // set by prepare(): the picture being defocused, never copied
  int m_imgComp;

  RenderRect m_rect;
  RenderRect m_area; // rect padded by plan().maxReach, so a level can gather from outside it
  CoCMap m_coc;
  RenderPlan m_plan;
  std::vector<LevelVisit> m_order;

  std::vector<float> m_acc;  // accumulator over the rect, 4 floats per pixel: premultiplied RGB and
                             // the accumulated alpha `A` of the visits that already ran: `1 − Π 1 − aᵢ`,
                             // starting at 0 (the note at the top of this header says why A is stored
                             // instead of the transmission it is complementary to). A is what the two
                             // sums below are divided by at the end
  std::vector<float> m_accW; // ... and 2 more per pixel: the coverage sum and the weight sum, whose
                             // ratio is the output alpha (see the note above). 6 per pixel in total,
                             // kept in two vectors because the four are touched by every layer and the
                             // two are read once, at the end
  Level m_lv;
  bool m_haveLevel = false;
};

} // namespace zd

#endif
