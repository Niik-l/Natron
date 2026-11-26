// ZDefocusZ: the blur kernel - what a point of light is recorded *as* (DECISIONS.md D-36).
//
// OFX-free like zd.h (D-34), and checked the same way (tests/zd_bokeh_test.cpp): a bokeh
// is geometry, so it is checked against geometry - areas, supports, mass, per-channel
// shape - rather than against a picture.
//
// A defocus needs, for each blur size, a **kernel**: the pattern a single point of light
// becomes. Everything the renderer does is convolve a layer with that pattern, so the
// kernel library owns two things - the shape, and the discretisation that turns a shape
// into a fixed number of taps whatever the radius is.
//
// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------
// Three, mirroring Nuke ZDefocus's `filter_type` (its knob record is
// references/zdefocus/nuke_zdefocus_knobs.md):
//
//   Disc     a circular aperture, with `discShape` dissolving the profile from a Gaussian
//            (0) to a hard disc (1). Nuke's `shape` knob, same intent, same ends.
//   Bladed   an iris polygon: `blades` sides, `roundness` blending the polygon toward its
//            circumscribed circle (0 = straight sides, 1 = circle: the support radius is
//            interpolated between the polygon's radial boundary and 1, which makes the
//            *area* interpolate between the polygon's area and pi), `rotation`, and an
//            inner polygon with its own brightness and feather (`innerSize`,
//            `innerFeather`, `innerBrightness`) for the obstruction/secondary-spectrum
//            look. `catadioptric` punches a central hole (a mirror lens' donut bokeh).
//   Image    a picture *is* the kernel - a photograph of a real aperture, or anything a
//            user paints. Resampled from the Bokeh clip's own mip chain.
//
// `aspect` squeezes the aperture along one axis at a time (`< 1` squeezes x, `> 1` squeezes
// y, exactly Nuke ZDefocus's `aspect`), which also makes it **area preserving**: it moves
// the aperture's extent, never its energy. `rotation` turns it; both apply to every shape,
// image included.
//
// ---------------------------------------------------------------------------
// Discretisation: cell area, not point samples
// ---------------------------------------------------------------------------
// The renderer convolves a destination pixel with a kernel whose radius in *pixels* is
// the layer's CoC, but it gathers that kernel from a plane reduced to cells of `cell`
// pixels apiece, so that a tap's worth of work is the same whether the radius is 3 px or
// 3000 (D-35's cost argument). A tap therefore has to stand for the aperture's coverage
// over a whole cell, not for its value at one point: each tap is the **area fraction**
// of its cell that the aperture covers, measured by supersampling the cell (exact for a
// cell wholly inside or wholly outside the aperture; a boundary cell uses a subgrid anchored
// to the cell and sized by the cell's own footprint, hence coordinate-independent). Two
// consequences the suite checks:
//
//   - Mass: the taps sum to the aperture's *area* in cell units, so a kernel normalised
//     to 1 is a true averaging kernel - a flat field stays flat, which is what a defocus
//     of a uniform HDR field requires.
//   - Support: span = 2*ceil(reach / cell) + 1, where `reach` is the extent the aperture
//     reaches along the axis `aspect` lengthens — never larger, so the renderer's RoI
//     expansion and its tap loop are both bounded by the same two numbers.
//
// A hard edge is softened by the discretisation alone (a boundary cell is a fraction),
// which is why no shape needs an "edge feather" constant of its own.
//
// ---------------------------------------------------------------------------
// Per-channel kernels, because a bokeh picture has colours
// ---------------------------------------------------------------------------
// A photographed aperture is not monochrome: a lens with chromatic aberration, a coloured
// filter in front of it, or a painted bokeh map has different shapes in R, G and B. So an
// image kernel carries **one kernel per channel**, each normalised to unit mass on its
// own - a red ring defocuses red light as a red ring, and green light however its own
// channel says - plus one **coverage** kernel, the mean of the channel kernels, which is
// what the alpha/occlusion side of the composite sees (D-37). `perChannel` off collapses
// the four into one kernel (the alpha if the bokeh has one, otherwise the mean of the
// channels), which is the shape-only behaviour Nuke's `use_input_channels` off gives.
//
// Every kernel is a translation-invariant stencil built from its own parameters: no
// coordinates, no render window, no order. That is what keeps the D-31 guarantee intact
// through the kernel library - two bands asking for the same radius and cell get the same
// taps, bit for bit.
#ifndef OFXLAB_ZDEFOCUSZ_BOKEH_H
#define OFXLAB_ZDEFOCUSZ_BOKEH_H

#include <cstddef>
#include <list>
#include <vector>

namespace zd {

/** @brief The three kernel shapes. Kept separate from the plugin's `filterType` choice
 * only in name: eFilterImage is this library's eShapeImage fed from the Bokeh clip. */
enum BokehShape {
  eShapeDisc = 0,
  eShapeBladed,
  eShapeImage,
  kNumShapes
};

/** @brief Which channel of an image bokeh defines the *shape* when `perChannel` is off:
 * the bokeh's alpha if it has one, else the mean of its colour channels. */
enum BokehSource {
  eBokehPerChannel = 0, // one kernel per channel (the default: each channel keeps its own shape)
  eBokehAlpha,          // one kernel from the bokeh's alpha, applied to every channel
  eBokehMean,           // one kernel from the mean of the bokeh's channels
  kNumBokehSources
};

// ---------------------------------------------------------------------------
// Kernel knob ranges, declared once (the D-33/D-34 precedent: descriptor range ==
// model clamp). The shape parameters are all *fractions of the aperture radius*, which
// is why they need no pixel units and no relation to maxBlur.
// ---------------------------------------------------------------------------
const double kShapeMin = 0.0, kShapeMax = 1.0;   // 0 Gaussian profile .. 1 hard disc
const double kAspectMin = 0.125, kAspectMax = 8.0; // 1 round, >1 squeezes y, <1 squeezes x
const double kRotationMin = -360.0, kRotationMax = 360.0; // degrees, counter-clockwise
const int kBladesMin = 3, kBladesMax = 32;       // 3 triangle, 4 square, 6 hexagon ...
const double kRoundMin = 0.0, kRoundMax = 1.0;   // 0 straight sides .. 1 circumscribed circle
const double kInnerSizeMin = 0.0, kInnerSizeMax = 1.0;   // inner polygon radius, of the outer
const double kInnerFeatherMin = 0.0, kInnerFeatherMax = 1.0; // 0.5 = a hard inner edge
const double kInnerBrightnessMin = 0.0, kInnerBrightnessMax = 2.0; // 0 black .. 1 same .. 2 brighter
const double kHoleMin = 0.0, kHoleMax = 0.95;    // catadioptric central obstruction
// A kernel's own discretisation floor: the coarsest cell a tap grid may use, and the
// radius below which the library stops refining the cell (a 1 px kernel is a 3-tap
// stencil, and supersampling cannot make a 0.2 px aperture finer than it is).
const double kCellMin = 1.0;

/** @brief Everything the shape library reads. `image` is not here: an image kernel is
 * built from a plane the caller supplies (see @ref SpriteLibrary::setSourceImage),
 * because the core may not know what an OFX clip is. */
struct BokehOptions {
  int shape = eShapeDisc;
  double discShape = 1.0;    // Nuke's `shape`: Gaussian -> hard disc
  double aspect = 1.0;       // <1 squashes x, >1 squashes y
  double rotation = 0.0;     // degrees

  int blades = 5;
  double roundness = 0.2;
  double innerSize = 0.8;
  double innerFeather = 1.0; // Nuke's `inner_feather`: >0.5 outward, <0.5 inward, 0.5 none
  double innerBrightness = 0.8;
  bool catadioptric = false;
  double catadioptricSize = 0.3;

  int source = eBokehPerChannel; // image shape only
  bool operator==(const BokehOptions &o) const;
};

/** @brief Clamp a BokehOptions in place to the ranges above and resolve the two
 * relations the shape maths needs (a blade count of 2 is not a polygon; an inner
 * polygon of size 0 has nothing to feather or brighten). */
void sanitise(BokehOptions &o);

/** @brief One kernel: a square stencil of taps, spaced @ref cell source pixels apart,
 * spanning a support of @ref span taps per side.
 *
 * Planes are ordered: one per channel the kernel covers, then the **coverage** plane
 * last. For a colour kernel that is R, G, B, coverage; for a single-channel kernel just
 * coverage. Each plane is normalised to unit sum when the aperture has any area at all;
 * an aperture of zero area (radius 0) is a single tap of 1, i.e. the identity, because a
 * layer at CoC 0 must pass its plane through unchanged.
 *
 * Taps are stored row-major, and a tap at (dx, dy) tap-offsets - span/2 .. span/2 -
 * stands for the source pixel at (x + dx*cell, y + dy*cell). Everything about a kernel
 * is a function of its own radius/cell/options and nothing else. */
struct Kernel {
  int span = 1;
  int planes = 1;
  double radius = 0.0; // px, the CoC this kernel was built for
  double cell = 1.0;   // px per tap of the plane it convolves

  std::vector<float> taps; // planes * span * span
  std::vector<double> mass; // per plane, the sum *before* normalisation: the aperture's
                            // area in cell units. Kept because it is the quantity the
                            // suite can check against @ref apertureArea, and because a
                            // normalised kernel whose raw mass is wrong is a kernel whose
                            // shape is wrong in a way normalisation hides.

  int half() const { return span / 2; }
  const float *plane(int p) const { return &taps[(size_t)p * (size_t)span * (size_t)span]; }
  float *plane(int p) { return &taps[(size_t)p * (size_t)span * (size_t)span]; }
  const float *coverage() const { return plane(planes - 1); }
  int coveragePlane() const { return planes - 1; }

  /** @brief Sum of a normalised plane (1 whenever the aperture had any area at all). */
  double sum(int p) const;
  /** @bytes of tap storage, for the renderer's memory cap (spriteRadiusCap). */
  size_t bytes() const { return taps.size() * sizeof(float); }

  bool operator==(const Kernel &k) const;
};

/** @brief A plane of samples, the OFX-free stand-in for a clip's pixels: an RGB/RGBA
 * (or single-channel) image, row-major, with the row pitch kept so a reduced plane can
 * be fed straight back in. The renderer's own plane type is this shape, so an *image*
 * kernel needs no OFX knowledge to be built (D-34). */
struct PlaneImage {
  int width = 0, height = 0;
  int channels = 4;   // 1 (coverage only) or 4 (RGBA)
  int rowStride = 0;  // floats per row (>= width * channels)
  std::vector<float> px;

  const float *at(int x, int y) const { return &px[(size_t)y * (size_t)rowStride + (size_t)x * (size_t)channels]; }
};

/** @brief Box-reduce a plane by an integer factor, with a clamp at the edges: the
 * reduced value is the mean of the source pixels that fall in the cell, and a cell
 * hanging off the plane averages only what is there. Deterministic and independent of
 * any render partition, because a cell's contents are fixed by absolute coordinates. */
PlaneImage reduceBox(const PlaneImage &src, int factor);

/** @brief Does a bokeh picture make the library build one kernel plane per colour channel, plus a
 * coverage plane, or a single shared kernel? The library's own rule (@c eShapeImage + @c
 * eBokehPerChannel + a 4-channel picture that actually has pixels in it — an image shape with
 * nothing on the clip falls back to the analytic shape, which is one plane), exported because a
 * plugin that has to *estimate* the bytes its render will cost must count the planes the library
 * is going to build, and a duplicated rule is two rules that can drift. */
bool usesPerChannelKernels(const BokehOptions &o, const PlaneImage &img);

/** @brief Builds the kernels this plugin needs and keeps them: a kernel is a pure
 * function of (options, radius, cell), so requesting the same one twice must return the
 * same taps *and* the same object - the renderer holds a reference per layer and would
 * otherwise rebuild a 3000-tap kernel for every tile it renders.
 *
 * An image kernel additionally needs a source plane: @ref setSourceImage gives the
 * library one (with the mip chain built lazily), and its taps are resampled from that
 * chain, so a kernel at any radius is the bokeh picture *scaled* to that radius rather
 * than a nearest-neighbour copy of it.
 *
 * Lifetime of a `Kernel &`: stable across @ref kernel calls (the slots are a std::list, so
 * inserting a new kernel never moves an existing one), but @ref setSourceImage and @ref clear
 * destroy every slot. A renderer prepares its kernels, renders with them, and only then reacts
 * to a changed bokeh input - which is the order the render path actually uses anyway (D-36). */
class SpriteLibrary {
public:
  explicit SpriteLibrary(const BokehOptions &options);

  /** @brief The kernel for a CoC radius (px) gathered from a plane reduced to `cell`
   * px cells. `cell` 1 means gathering from the full-resolution plane. Returns the same
   * object for identical requests. */
  const Kernel &kernel(double radius, double cell);

  /** @brief Hand the library the bokeh picture (image shape). Replacing it invalidates
   * every cached kernel: a cached kernel built from a different picture is a different
   * answer to the same question. */
  void setSourceImage(const PlaneImage &image);

  /** @brief The options these kernels are built from. A renderer that has to size its own
   * working window needs the same `aspect` the kernels were built with, and asking the
   * library that built them is the only way to be certain the two agree. */
  const BokehOptions &options() const { return m_opt; }

  /** @brief Tally of cache behaviour: how many kernels were built and how many requests
   * were answered by an existing one. The offscreen probe and the sweep report it; a
   * renderer that rebuilds a kernel per tile is the bug this counts. */
  int built() const { return m_built; }
  int reused() const { return m_reused; }

  /** @brief Drop every built kernel (not the source image): the plugin calls this at
   * purgeCaches(), when the Bokeh clip changes, and when a different level set asks for
   * different radii. Kernels are *not* dropped at endRender(): a kernel is a pure function
   * of (options, radius, cell), so the next frame of the same shot wants the same one. */
  void clear();

  /** @brief Total tap bytes held. Kept out of the cache decision and into the plugin's:
   * a kernel's taps grow as (radius/cell)^2 * planes, so the *cap* that matters is on
   * `cell` (the renderer picks the coarsest cell that keeps the shape), not here. */
  size_t bytes() const;

private:
  struct Slot {
    long long radiusKey; // radius quantised to 1/1024 px, so two equal radii are equal keys
    long long cellKey;
    Kernel kernel;
  };

  void build(Kernel &k, double radius, double cell) const;
  void buildAnalytic(Kernel &k, double radius, double cell) const;
  void buildImage(Kernel &k, double radius, double cell) const;
  void buildMips() const;

  BokehOptions m_opt;
  double m_gaussNorm; // makes the truncated Gaussian profile unit-mass: a property of the
                      // options alone (see apertureQuadrature), never of the radius
  std::list<Slot> m_slots; // list, not vector: kernel() returns a reference the renderer
                           // keeps per layer, and it must survive the next insertion
  PlaneImage m_image;
  mutable std::vector<PlaneImage> m_mips; // mips[0] == m_image, each next reduced by 2
  mutable bool m_mipsDirty = true;
  int m_built = 0;
  int m_reused = 0;
};

/** @brief The aperture's boundary radius, in unit-aperture fractions, at angle @p theta
 * (radians): 1 is the circumscribed circle, cos(pi/blades) is the apothem of a polygon
 * with a side normal at that angle. Exposed for the tests, which check the sprite's mass
 * against the analytic area of exactly this boundary. */
double apertureBoundary(int blades, double roundness, double theta);

/** @brief The aperture's *mass* in unit-aperture area fractions: pi for a hard round
 * aperture of uniform brightness, the polygon area for a bladed one, and adjusted for
 * @c discShape (a Gaussian profile is mass-normalised to 1), @c innerBrightness and the
 * catadioptric hole. Numerical (an angular quadrature of the same boundary the sprites
 * use, with a radial quadrature of the same transmission), and the reference the sprite
 * masses are checked against. A kernel of @c radius px therefore has raw mass
 * `apertureArea(o) * radius * radius / cell^2` — and because `aspect` only moves the
 * extent between the two axes, that prediction does not depend on it. */
double apertureArea(const BokehOptions &o);

/** @brief The tap span a kernel of @p radius gathered at @p cell occupies:
 * 2*ceil(reach/cell) + 1, where `reach` is the extent the aperture actually reaches along
 * the axis @p aspect lengthens (radius * aspect, or radius / aspect). The renderer's RoI
 * expansion is derived from the same quantity, so the two cannot disagree (that is how
 * SharpenZ keeps its RoI honest). */
int kernelSpan(double radius, double cell, double aspect = 1.0);

/** @brief The coarsest cell (in px) at which a kernel of @p radius still resolves its
 * shape, i.e. the largest tap spacing that keeps the blur this kernel produces within a
 * few percent of the exact convolution (measured against the same kernel at cell 1).
 * Expressed as taps across the aperture's extents: @ref kernelSpan's tap count and this
 * bound are the two ends of the same window, and a renderer that gathers no coarser than
 * this cannot lose the shape to its mip chain. */
double coarsestCell(double radius, double aspect = 1.0);

} // namespace zd

#endif
