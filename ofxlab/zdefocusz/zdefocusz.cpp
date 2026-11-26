/*
  zdefocusz.cpp — ZDefocusZ: depth-driven defocus, for Natron and any other OFX host.

  The node reads a depth/Z channel, turns it into a circle-of-confocus map, splits that map into
  up to 256 discrete blur sizes and composites the levels back-to-front through a per-level bokeh
  kernel on a cell-reduced lattice. All of *that* is host-free and unit-tested
  (tests/zd_depth_test.cpp, zd_bokeh_test.cpp, zd_layered_test.cpp) and lives in `zd.{h,cpp}`,
  `bokeh.{h,cpp}` and `layered.{h,cpp}`. This file is the half that talks to a host: the knob
  table (plugins/zdefocusz/DESIGN.md §2), the input clips, the gather padding, the per-render memory cap
  and the emit stage. The `useGPU` checkbox hands the two heavy stages of every level visit to an OpenCL
  device (gpu.{h,cpp}, D-51); every other stage — and the whole visit when the checkbox is off or the
  device declines the band — is the CPU code below.

  Inputs: `Source` (the picture); an optional `Depth` clip — when nothing is connected there the
  depth channel is read out of `Source` itself, which is how a Blender Z pass or a Nuke ZDown
  arrives on a single input; an optional `Bokeh` picture, used as *the* aperture instead of the
  analytic one; and the standard optional alpha `Mask` (D-20, General context only).

  There is no layer/matte graph here on purpose: ZDefocusZ defocuses its input and a host
  composites it, which is what makes a depth-driven filter and a depth-driven *reconstructor* two
  different nodes (DECISIONS.md D-40).

  Knob ranges and defaults come from the core headers (zd.h, bokeh.h, layered.h) — the descriptor
  offers the same constants the model clamps to, so a knob cannot offer a value the maths would
  then have to refuse (the D-33 precedent).
*/

#include <ofxsImageEffect.h>
#include <ofxsMultiThread.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>  // the fallback sentence's file channel (see logGpuFallbackLine)
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ofxlab_channels.h"   // the "channels" knob parser (D-06, D-17)
#include "ofxlab_maskmix.h"    // the mask/mix dissolve (D-20)

#include "bokeh.h"
#include "gpu.h"
#include "layered.h"
#include "zd.h"

////////////////////////////////////////////////////////////////////////////////
// per-render limits (DECISIONS.md D-40)

namespace {

/** @brief Bytes one plugin instance is willing to hold for one render.
 *
 * OFX 1.4 has no memory *accounting* suite — `ofxsMemory.h` offers an allocator, not a budget —
 * so a plugin that does not want to be the process's OOM candidate has to keep its own number
 * (the precedent is TimeWarp's `kFlowMemoryBudget`, ARCHITECTURE.md §4; this plugin's half of that
 * model is DESIGN.md §5). Here the number bounds
 * the CoC map, the composite accumulator and one level's cell grids, and the way it is honoured
 * is to split the render rect into row bands (see measureMemory). */
const size_t kMemoryBudget = 512u * 1024u * 1024u;

/** @brief Largest Bokeh picture accepted on the Bokeh clip, per axis.
 *
 * An image kernel resamples the picture, so its cost is the mip chain built from it: the cache
 * holds the picture plus each halving of it, which is 4/3 of it, times 4 channels of float32.
 * 4096² RGBA is 67 MB of source and 89 MB of chain, which is already a *bokeh map* and not a
 * picture. A host that hands us more than this is handing us a plate, not an aperture. */
const int kMaxBokehSide = 4096;

inline bool finiteD(double v) { return std::isfinite(v); }

bool boundedD(const OfxRectD &r)
{
  return finiteD(r.x1) && finiteD(r.y2) && finiteD(r.x2) && finiteD(r.y1) &&
         r.x2 > r.x1 && r.y2 > r.y1;
}

// ---------------------------------------------------------------------------
// depthChannel
// ---------------------------------------------------------------------------

/** @brief The component the depth model reads when the knob asks for "the Z channel": the last
 * component of an RGBA clip and the first of anything else.
 *
 * This is @ref prepareCoC's own fallback (`comp == 4 ? 3 : 0`) written down here, so the plugin's
 * selector and the core's reader cannot disagree about which component a "Z" is. A depth map is
 * never the alpha of a picture — an RGBA *Source* used as its own depth with the default knob has
 * alpha 1 everywhere, which is a constant depth, which is an entirely in-focus render: the honest
 * answer for "this picture has no depth in it", and the reason the Depth input exists. */
int depthSlot(int ncomp) { return ncomp == 4 ? 3 : 0; }

/** @brief Names that mean "the Z channel of whatever this clip is".
 *
 * The default `depthChannel` is **Z**, because that is what Blender calls the pass (the brief's
 * reference point, DESIGN.md §1) and what its compositor names the channels inside it: the legacy Z pass
 * shipped Z, ZBack, ZDown, ZFront, ZMath and friends, and the modern one Z/WorldZ/Distance. Nuke
 * names the same data ZDown/ZBack/WorldZ/ObjectZ, After Effects has Scene Depth, and a RenderChan
 * or Krystal plan writes a layer plainly called Depth. Every one of those is a *name*, and a host
 * has already picked the channel before the clip reaches us: what the plugin has to decide is
 * which **component** of the delivered image to read, and for all of these names the answer is the
 * same — the depth slot. So the table exists to make the default's meaning explicit, and an
 * unrecognised name falls back to it rather than to nothing: a mistyped knob must not silently
 * render an in-focus image. `OfxLab::cleanChannelTokens` folds the case and drops the separators,
 * so `Z_depth`, `Z Down` and `zdepth` are the same entry. */
bool isDepthAlias(const std::string &t)
{
  static const char *const kAliases[] = {
    "z", "zdepth", "zback", "zdown", "zfront", "zforward", "zlinear", "zmath", "zview", "zcamera",
    "viewz", "worldz", "objectz", "cameraz", "scenedepth", "depth", "distance", "forward", "back",
    "front", "linear", "math"
  };
  for (std::size_t i = 0; i < sizeof(kAliases) / sizeof(kAliases[0]); ++i)
    if (t == kAliases[i])
      return true;
  return false;
}

/** @brief Which component of a delivered clip the depth model reads.
 *
 * Returns an index into the delivered pixel, or the caller's `depthSlot()` for anything the clip
 * does not have. Explicit `r`/`g`/`b`/`a` (or red/green/blue/alpha) select by index; a selection
 * the clip cannot answer — `a` on an RGB clip — is not a channel to guess at, so it takes the
 * depth slot instead. On a 1-component clip every selection lands on component 0, which is the
 * only channel there is. */
int resolveDepthChannel(const std::string &knob, int ncomp)
{
  const std::string t = OfxLab::cleanChannelTokens(knob);
  int idx = -1;
  if (t == "r" || t == "red")
    idx = 0;
  else if (t == "g" || t == "green")
    idx = 1;
  else if (t == "b" || t == "blue")
    idx = 2;
  else if (t == "a" || t == "alpha")
    idx = 3;
  else if (!isDepthAlias(t))
    idx = depthSlot(ncomp); // unrecognised: the default's answer, not an error
  else
    idx = depthSlot(ncomp);

  if (idx < 0 || idx >= ncomp)
    idx = depthSlot(ncomp);
  return idx;
}

// ---------------------------------------------------------------------------
// memory: what one render costs, measured before it allocates anything (D-40)
// ---------------------------------------------------------------------------

/** @brief One render's cost, derived from the exported helpers prepare() itself uses.
 *
 * A band of @p bandRows rows costs: the padded CoC map (a float CoC and a byte side per pixel of
 * the *work* area — the rect padded by the level set's widest reach), the composite accumulator
 * (4 colour plus 2 weight floats per output pixel), and one level's cell grids at a time: 10 floats
 * per cell (the two 5-plane reduced grids — the fill borrows the convolve's grid instead of owning
 * two more, which is why there are two grids and not four) plus that level's fill row scratch. The
 * cell counts come from @ref zd::cellForRadius and @ref zd::levelMargin — @ref zd::levelGeom derives
 * its grid from those same two helpers — and a level's grid is bounded by *this* band (levelGeom
 * clips the level's cell box to the rect it is given), which is what makes a band split reduce the
 * peak at all. It is an upper bound rather than the exact figure, and @ref
 * zd::LayeredRender::peakBytes is the number the render then reports. Every radius here is @ref
 * zd::cappedRadius'd with the same @ref zd::LayerOptions::spriteRadiusCap prepare() uses, so the
 * estimate and the allocation describe the same kernels (D-45).
 *
 * The sprite cache is counted too, because it is the other half of the render's footprint: a
 * kernel is `span² × planes` floats, a level set of 256 of them is not small, and an image kernel
 * additionally holds the mip chain it resamples from (a source plus each halving of it, 4/3 of the
 * source). It is also the half a band split cannot reduce, which is why @ref MemoryPlan reports it
 * separately. A capped ladder counts each kernel once: the cache keys on (radius, cell), both derived
 * from the capped radius, so the levels the cap clips share one kernel (D-45). */
struct MemoryPlan {
  int pad;             // px of gather on every side: ceil(maxKernelReach), the RoI the host is asked for
  int bandRows;        // rows to render per band so the total fits
  double spriteBytes;  // the kernels this level set can ask for
  double bandBytes;    // sprite cache + one band
  double deviceBytes;  // one band's device buffers, 0 when the render has no device (D-51)
  bool fits;           // false when even a 1-row band is over budget
  bool gpuFits;        // false when even a 1-row band is over the *device's* ceiling: CPU-only render
};

/** @brief One band's cost on the host and, when @p deviceCap is non-zero, on the device too.
 *
 * @p deviceCap is what a GPU render adds to the same band: gpu.cpp's @c Session keeps one buffer per piece of
 * state its kernels touch — the gathered picture, CoC and side windows, the accumulator, this level's two
 * grids, extraction's partial sums, the taps, the level radii — and every one of them grows only, to the
 * widest level the band visits, so mirroring them level by level and taking the widest is what a whole band
 * costs. The Session refuses anything over the device's own single-allocation ceiling (@ref
 * zd::gpu::maxAllocation), which is the number a caller passes here; keeping the band under it *in the plan*
 * is what makes a GPU band a decision rather than a failed clCreateBuffer halfway through a stage.
 *
 * A band the device cannot hold does not make the render impossible: @ref gpuFits says so, and the plugin
 * renders that band on the CPU. What halving for the device does cost is padding work — each band builds its
 * own CoC map over the rect padded by @ref zd::maxKernelReach, so more bands means more of that map — which
 * is the price of a device render that fits. */
MemoryPlan measureMemory(const zd::LevelSet &levels, double aspect, bool fill, double cap,
                         bool perChannelKernels, const zd::PlaneImage &image, int width, int height,
                         int imgComponents, size_t deviceCap)
{
  MemoryPlan m;
  const double reach = zd::maxKernelReach(levels, aspect, fill, cap);
  m.pad = reach > 0.0 ? (int)std::ceil(reach) : 0;
  const double R = (double)m.pad;
  m.bandRows = height > 0 ? height : 1;

  double sprites = 0.0, widestTaps = 0.0; // the device holds the *current* level's whole kernel in one tap
  // buffer (gpu.cpp's uploadKernel), so what it costs is the widest kernel, not the cache's sum.
  std::vector<long long> built; // the sprite cache keys on (radius, cell), and both are a function of
  // the capped radius, so every level the cap clips asks for the same kernel and the library holds one
  // (D-45). Counting each kernel once is what the cache then really contains.
  for (int i = 0; i < levels.count(); ++i) {
    const double r = zd::cappedRadius(levels.radius[(size_t)i], cap);
    const long long key = (long long)std::llround(r * 1024.0); // SpriteLibrary's own radius key
    bool same = false;
    for (size_t k = 0; k < built.size(); ++k) {
      if (built[k] == key) {
        same = true;
        break;
      }
    }
    if (same)
      continue;
    built.push_back(key);
    const int cell = zd::cellForRadius(r, aspect);
    const int span = zd::kernelSpan(r, (double)cell, aspect);
    // An analytic kernel is one coverage plane; an image kernel with `Per Channel` is one plane per
    // colour channel plus their mean, which is the caller's `perChannelKernels` because it comes from
    // @ref zd::usesPerChannelKernels — the library's own rule, asked of the library.
    const double planes = perChannelKernels ? 4.0 : 1.0;
    const double kernelBytes = (double)span * (double)span * planes * (double)sizeof(float);
    sprites += kernelBytes;
    if (kernelBytes > widestTaps)
      widestTaps = kernelBytes;
  }
  if (image.channels > 0 && image.width > 0 && image.height > 0) {
    const double src = (double)image.width * (double)image.height * (double)image.channels *
                       (double)sizeof(float);
    sprites += src + src / 3.0; // the picture plus its mip chain
  }
  m.spriteBytes = sprites;

  for (;;) {
    const double workW = (double)width + 2.0 * R;
    const double workH = (double)m.bandRows + 2.0 * R;

    double maps = workW * workH * ((double)sizeof(float) + (double)sizeof(unsigned char));
    maps += (double)width * (double)m.bandRows * 6.0 * (double)sizeof(float);

    double widest = 0.0, widestDev = 0.0;
    for (int i = 0; i < levels.count(); ++i) {
      const double r = zd::cappedRadius(levels.radius[(size_t)i], cap);
      const int cell = zd::cellForRadius(r, aspect);
      const int margin = zd::levelMargin(r, aspect, fill);
      const int half = zd::kernelSpan(r, (double)cell, aspect) / 2;
      const int dilate = margin - half - 2 > 0 ? margin - half - 2 : 0; // levelMargin's three terms
      const int ring = 1 + margin; // the convolve's pad (one cell of bilinear read-back: levelGeom's
      // `pad`, which fillForeground no longer widens) plus the grid's zero ring, in cells
      const double cols = (double)width / (double)cell + 1.0 + 2.0 * (double)ring;
      const double rows = (double)m.bandRows / (double)cell + 1.0 + 2.0 * (double)ring;
      const double line = std::max(cols, rows) + 2.0 * (double)dilate + 1.0; // the fill's row scratch,
      // as long as the widest row it dilates through (levelGeom counts the same thing as `line`)
      const double bytes = cols * rows * 10.0 * (double)sizeof(float) + line * 2.0 * (double)sizeof(float);
      if (bytes > widest)
        widest = bytes;

      // The same level on the device: the two reduced grids, on the same lattice and with the same ring
      // (gpu.cpp's ensureGrid takes cells × 5 floats × 2), plus extraction's partial sums, which only the
      // tiled path asks for and only when a cell is wider than one tile — a cell that a single
      // kGpuTile-wide tile covers is written by the direct kernel and needs nothing between the two
      // stages. The cell box here is the extraction box (convolved box widened by half + dilate, which is
      // what Session::stageExtract computes from cellRowsFor) rather than the grid, so it is the smaller
      // number; the ring above is what makes it fit inside the grid.
      double dev = cols * rows * 10.0 * (double)sizeof(float);
      if (cell > zd::gpu::kGpuTile) {
        const int tiles = (cell + zd::gpu::kGpuTile - 1) / zd::gpu::kGpuTile;
        const double exCols = (double)width / (double)cell + 1.0 + 2.0 * ((double)half + (double)dilate);
        const double exRows = (double)m.bandRows / (double)cell + 1.0 + 2.0 * ((double)half + (double)dilate);
        dev += exCols * exRows * (double)tiles * (double)tiles * 5.0 * (double)sizeof(double);
      }
      if (dev > widestDev)
        widestDev = dev;
    }

    // The band's device-side windows, whose extent is the same padded area the CoC map is built over
    // (gpu.cpp clips them to the delivered plane, which is never larger, so this is the upper bound).
    // imgComponents is the source's component count, because the window holds the picture the source
    // delivered and not the output's.
    const double window = workW * workH * ((double)(imgComponents > 0 ? imgComponents : 1) * (double)sizeof(float) +
                                           (double)sizeof(float) + (double)sizeof(unsigned char));
    const double devAccum = (double)width * (double)m.bandRows * 6.0 * (double)sizeof(float);
    m.deviceBytes = widestDev + window + devAccum + widestTaps + (double)levels.count() * sizeof(double);

    m.bandBytes = sprites + maps + widest;
    const bool hostFits = m.bandBytes <= (double)kMemoryBudget;
    const bool deviceFits = m.deviceBytes <= (double)deviceCap;
    if ((hostFits && (deviceCap == 0 || deviceFits)) || m.bandRows <= 1)
      break;
    m.bandRows /= 2; // halve until it fits, down to a single row
  }

  m.fits = m.bandBytes <= (double)kMemoryBudget;
  m.gpuFits = deviceCap > 0 && m.deviceBytes <= (double)deviceCap;
  return m;
}

// ---------------------------------------------------------------------------
// OFX images seen as the core's planes
// ---------------------------------------------------------------------------

/** @brief The picture, as a `zd::Plane`: no copy, the image's own bounds, and **straight** colour.
 *
 * The renderer's contract is straight on both sides (layered.h: premultiplication is the only form
 * in which a coverage weight and a colour weight can be convolved by the same aperture), and a host
 * delivers whichever space its format says. So the divide happens here, on the way in, and the
 * multiply on the way out (the same reading TimeWarp's `LumaRowPlane` makes). A scanline outside
 * the delivered bounds is absent rather than guessed at, which is the core's own rule.
 *
 * Straight deliveries are handed through untouched — no row is copied when there is nothing to
 * convert — and so is a 1-component clip, whose single component *is* its density. */
class ColourRowPlane : public zd::Plane {
  OFX::Image *_img;
  bool _premult;
  bool _divide;
  mutable std::vector<float> _buf; // the last row computed; valid until the next row()
  mutable int _bufY;

public:
  ColourRowPlane()
    : _img(0), _premult(false), _divide(false), _bufY(std::numeric_limits<int>::min()) {}

  void set(OFX::Image *img)
  {
    _img = img;
    _divide = false;
    comp = 4;
    if (!img) {
      x1 = y1 = x2 = y2 = 0;
      _buf.clear();
      _bufY = std::numeric_limits<int>::min();
      return;
    }
    const OfxRectI b = img->getBounds();
    x1 = b.x1;
    y1 = b.y1;
    x2 = b.x2;
    y2 = b.y2;
    comp = (img->getPixelComponents() == OFX::ePixelComponentRGBA) ? 4 : 1;
    _premult = (img->getPreMultiplication() == OFX::eImagePreMultiplied);
    _divide = (_premult && comp == 4);
    _buf.assign((size_t)std::max(0, x2 - x1) * (size_t)comp, 0.0f);
    _bufY = std::numeric_limits<int>::min();
  }

  virtual const float *row(int y) const
  {
    if (!_img || y < y1 || y >= y2)
      return 0;
    const float *px = (const float *)_img->getPixelAddress(x1, y);
    if (!px)
      return 0; // the host did not deliver this scanline
    if (!_divide)
      return px;
    if (_bufY == y)
      return _buf.data();

    const int w = x2 - x1;
    for (int i = 0; i < w; ++i) {
      const float a = px[i * 4 + 3];
      const float inv = a > 0.0f ? 1.0f / a : 0.0f;
      // Colour under a == 0 is undefined; 0 is what the core reads as "nothing here", and a == 0 is
      // passed through unchanged because alpha is a density, not a transform of the colour.
      _buf[i * 4 + 0] = px[i * 4 + 0] * inv;
      _buf[i * 4 + 1] = px[i * 4 + 1] * inv;
      _buf[i * 4 + 2] = px[i * 4 + 2] * inv;
      _buf[i * 4 + 3] = a;
    }
    _bufY = y;
    return _buf.data();
  }
};

/** @brief The depth channel, as a `zd::Plane`. Read **raw**, never unpremultiplied.
 *
 * A distance is a distance whatever the picture's premultiplication state is, and dividing one by
 * an alpha that describes the *surface's coverage* turns a valid depth into a smaller depth — i.e.
 * into a nearer object that was not there. Only the channel the selector picked is handed over:
 * the depth slot of a delivered clip is exactly what @ref prepareCoC reads, so that case is the
 * image itself with no copy at all, while an explicit `g` or `b` is unpacked into a 1-component
 * plane so the core reads it as the whole signal.
 *
 * A scanline the depth image does not deliver holds no geometry, and @ref prepareCoC reads that as
 * "at infinity, on the far side", i.e. blurred at `maxBlur`. That is why the RoI pads the Depth
 * clip by the same reach as the Source: the pixels *outside* the window are what a level's blur
 * gathers from, and getting less than that is a visibly wrong edge rather than a crash. */
class DepthRowPlane : public zd::Plane {
  OFX::Image *_img;
  int _delivered;
  int _pick;
  bool _unpack;
  mutable std::vector<float> _buf;
  mutable int _bufY;

public:
  DepthRowPlane()
    : _img(0), _delivered(0), _pick(0), _unpack(false), _bufY(std::numeric_limits<int>::min()) {}

  void set(OFX::Image *img, int ncomp, int pick)
  {
    _img = img;
    comp = _delivered = ncomp;
    _pick = pick;
    _unpack = (pick != depthSlot(ncomp));
    if (_unpack)
      comp = 1; // the core reads a 1-component plane as the whole signal
    if (!img) {
      x1 = y1 = x2 = y2 = 0;
      _buf.clear();
      _bufY = std::numeric_limits<int>::min();
      return;
    }
    const OfxRectI b = img->getBounds();
    x1 = b.x1;
    y1 = b.y1;
    x2 = b.x2;
    y2 = b.y2;
    _buf.assign((size_t)std::max(0, x2 - x1), 0.0f);
    _bufY = std::numeric_limits<int>::min();
  }

  /** @brief How a delivered row is addressed: its stride, and which component of it the model
   * reads. An unpacked row is one component wide and holds the picked channel, so it reports 1
   * and 0; a whole image keeps its own stride and the depth slot inside it. The
   * <c>preview = Depth</c> output has to address the same component the model reads, which is the
   * only reason these are public. */
  int stride() const { return _unpack ? 1 : (_delivered > 0 ? _delivered : 1); }
  int channel() const { return _unpack ? 0 : _pick; }

  virtual const float *row(int y) const
  {
    if (!_img || _delivered <= 0 || y < y1 || y >= y2)
      return 0;
    const float *px = (const float *)_img->getPixelAddress(x1, y);
    if (!px)
      return 0;
    if (!_unpack)
      return px; // the depth slot is the component prepareCoC itself reads out of this stride
    if (_bufY == y)
      return _buf.data();
    const int w = x2 - x1;
    for (int i = 0; i < w; ++i)
      _buf[i] = px[i * _delivered + _pick];
    _bufY = y;
    return _buf.data();
  }
};

/** @brief The core's output plane, one scanline at a time.
 *
 * The composite is written per row and post-processed per row (channel merge-back, mask/mix, the
 * re-multiply for a premultiplied host), so a whole-band RGBA copy would be 16 bytes per pixel of
 * pure overhead for a stage that only ever looks at one row. `row()` answers for exactly one row,
 * which is also what makes a mistake visible: the core skips a null row rather than guessing. */
class ScratchRowPlane : public zd::OutputPlane {
  std::vector<float> _px;
  int _y;

public:
  ScratchRowPlane() : _y(std::numeric_limits<int>::min()) {}

  void reset(int xLo, int xHi, int ncomp)
  {
    x1 = xLo;
    x2 = xHi;
    y1 = 0;
    y2 = 0;
    comp = ncomp;
    _px.assign((size_t)std::max(0, xHi - xLo) * (size_t)ncomp, 0.0f);
    _y = std::numeric_limits<int>::min();
  }

  void setRow(int y)
  {
    _y = y;
    y1 = y;
    y2 = y + 1;
  }

  const float *values() const { return _px.data(); }

  virtual float *row(int y) { return y == _y ? _px.data() : 0; }
};

/** @brief The bokeh picture, read out of the Bokeh clip into the shape `zd::PlaneImage` wants. */
bool readPlaneImage(OFX::Image *img, zd::PlaneImage &out)
{
  if (!img)
    return false;
  const OfxRectI b = img->getBounds();
  const int w = b.x2 - b.x1, h = b.y2 - b.y1;
  if (w <= 0 || h <= 0)
    return false;
  const int ch = (img->getPixelComponents() == OFX::ePixelComponentRGBA) ? 4 : 1;
  const bool premult = (img->getPreMultiplication() == OFX::eImagePreMultiplied);

  out.width = w;
  out.height = h;
  out.channels = ch;
  out.rowStride = w * ch;
  out.px.assign((size_t)w * (size_t)h * (size_t)ch, 0.0f);

  for (int y = 0; y < h; ++y) {
    const float *px = (const float *)img->getPixelAddress(b.x1, b.y1 + y);
    if (!px)
      continue; // stays zero: a hole in the aperture is a hole
    float *dstRow = &out.px[(size_t)y * (size_t)out.rowStride];
    if (ch == 1) {
      for (int x = 0; x < w; ++x)
        dstRow[x] = px[x]; // a single channel is the aperture's density
      continue;
    }
    for (int x = 0; x < w; ++x) {
      float r = px[x * 4 + 0], g = px[x * 4 + 1], b2 = px[x * 4 + 2];
      const float a = px[x * 4 + 3];
      if (premult && a > 0.0f) {
        r /= a;
        g /= a;
        b2 /= a;
      }
      dstRow[x * 4 + 0] = r;
      dstRow[x * 4 + 1] = g;
      dstRow[x * 4 + 2] = b2;
      dstRow[x * 4 + 3] = a; // straight, like every other plane the core reads
    }
  }
  return true;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////
// parameters

namespace {

/** @brief The whole knob table, already clamped by the core's own `sanitise()` overloads.
 *
 * The three option structs are the core's, so the plugin has one place to write and the model one
 * place to read (the precedent for the whole file). Defaults are the structs' defaults, which are
 * also what the descriptor declares — knob table, model and descriptor are one set of numbers
 * (DESIGN.md §2). */
struct Params {
  zd::FocusOptions focus;
  zd::LayerOptions layers;
  zd::BokehOptions bokeh;

  int preview;         // zd::PreviewOutput
  double mix;          // [0,1], the dissolve weight of D-20
  bool maskInvert;
  bool useGPU;         // hand each level visit's two heavy stages to an OpenCL device (D-51)
  std::string depthChannel;
  bool sel[4];         // the `channels` knob, default all

  Params()
    : preview(zd::ePreviewResult), mix(1.0), maskInvert(false), useGPU(false), depthChannel("r")
  {
    sel[0] = sel[1] = sel[2] = sel[3] = true;
  }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////
// plugin instance

class ZDefocusZPlugin : public OFX::ImageEffect {
protected:
  // do not need to delete these, the ImageEffect is managing them for us
  OFX::Clip *dstClip_;
  OFX::Clip *srcClip_;
  OFX::Clip *depthClip_; // General context only, optional
  OFX::Clip *bokehClip_; // General context only, optional
  OFX::Clip *maskClip_;  // General context only, optional (D-20)

  // Depth
  OFX::ChoiceParam *depthMode_;
  OFX::BooleanParam *depthInvert_;
  OFX::DoubleParam *focusValue_, *focusRange_, *depthNear_, *depthFar_, *cocScale_;
  OFX::StringParam *depthChannel_;

  // Blur + optics
  OFX::ChoiceParam *blurFrom_;
  OFX::DoubleParam *blurSize_, *maxBlur_, *minBlur_;
  OFX::DoubleParam *aperture_, *focalLength_, *sensorWidth_, *depthScale_;

  // Layers + bloom
  OFX::IntParam *layers_;
  OFX::DoubleParam *layerSpacing_, *layerCurve_, *dither_;
  OFX::BooleanParam *fillForeground_;
  OFX::DoubleParam *spriteRadiusCap_; // cost only: the largest radius a kernel is built for (D-45)
  OFX::BooleanParam *bloom_;
  OFX::DoubleParam *bloomThreshold_, *bloomGain_;
  OFX::BooleanParam *bloomGamma_;

  // Bokeh
  OFX::ChoiceParam *shape_, *bokehSource_;
  OFX::DoubleParam *discShape_, *aspect_, *rotation_, *roundness_, *innerSize_, *innerFeather_,
      *innerBrightness_, *catadioptricSize_;
  OFX::IntParam *blades_;
  OFX::BooleanParam *catadioptric_;

  // Output
  OFX::ChoiceParam *preview_;
  OFX::StringParam *channels_;
  OFX::DoubleParam *mix_;
  OFX::BooleanParam *maskInvert_;

  // Performance
  OFX::BooleanParam *useGPU_; // which device runs a level visit's two heavy stages (D-51)

  // The kernel library, kept per instance: a kernel is a pure function of (options, radius, cell)
  // and the only knobs that decide those are read as constants, so the same level set asks for the
  // same kernels on every frame and rebuilding them per frame would be the bug bokeh.h's
  // built()/reused() tally exists to catch. Cleared when the Bokeh picture or the level set changes
  // (a cached kernel built from another picture is the wrong answer to the same question), and when the
  // level set or the radius cap changes, because that is what the kernels were built for (D-45).
  std::unique_ptr<zd::SpriteLibrary> _sprites;
  zd::BokehOptions _spriteOptions;   // the options _sprites was built from
  zd::LevelSet _spriteLevels;        // the level set its kernels were built for
  zd::PlaneImage _spriteImage;       // the bokeh picture it samples
  bool _haveSpriteImage;
  double _spriteCap;                 // the spriteRadiusCap those kernels were capped by (D-45)

  mutable bool _keyframed; // sticky: false once this host refuses a timed getter
  bool _gpuLogged;         // sticky per render: the CPU-fallback reason is logged once per render, not
                           // once per band, because a band split makes it repeat by design

public:
  ZDefocusZPlugin(OfxImageEffectHandle handle);

  virtual void render(const OFX::RenderArguments &args) override;
  virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip *&identityClip,
                          double &identityTime) override;
  virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                    OFX::RegionOfInterestSetter &rois) override;
  virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args,
                                     OfxRectD &rod) override;

  virtual void purgeCaches() override;
  virtual void changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName) override;

private:
  Params readParams(double t) const;
  double readDouble(OFX::DoubleParam *p, double t) const;

  int gatherPad(const Params &p) const;
  double imageWidthPx(double t, const OfxRectD &fallback) const;

  /** @brief Make sure `_sprites` can answer this render's kernel requests, and only then. */
  void prepareSprites(const Params &p, const zd::LevelSet &levels, OFX::Image *bokehImg);

  /** @brief The diagnostic outputs. Replaces the render, and with it the mask/mix dissolve. */
  void renderPreview(const Params &p, const zd::LevelSet &levels, const zd::RenderRect &rect,
                     OFX::Image *dst, const DepthRowPlane &depth, double blurSizePx, int dstComp);

  /** @brief One scratch row -> destination pixels: re-multiply, merge back the unselected
   * channels, then the mask/mix dissolve (D-20), all in the destination's own space.
   *
   * The source operand is converted into that space first, because @c srcPremult and @c dstPremult
   * need not agree; it is read from the source's own delivered bounds, because the render window is
   * wider than the picture by this node's gather pad. See the note in the definition. */
  void emitRow(OFX::Image *dst, const OFX::Image *src, const ScratchRowPlane &scratch, int srcComp,
               int dstComp, bool srcPremult, bool dstPremult, int x1, int x2, int y, const Params &p,
               const OfxLab::MaskReader &mask) const;

  /** @brief The all-sharp fast path, at the plugin's level: the source rows, verbatim.
   *
   * @ref zd::LayeredRender::copyInputThrough is the same answer, but through the core's straight
   * planes and out again, which is exact in the maths and not bit-for-bit in float32. When the
   * source is exactly what the output has to be — which is what "every CoC is 0" means, for every
   * mix, mask and channel selection, because an effect equal to the source dissolves to itself —
   * the copy is the honest version of it. Callers only take it when the source and destination are
   * the same component set *and* the same premultiplication, which is the only case in which a
   * byte-for-byte copy is the answer; otherwise the core's own straight-space path does the work, one
   * row at a time, and @ref emitRow converts the source rows into the destination's space. */
  void copySourceRows(OFX::Image *dst, const OFX::Image *src, int dstComp, int x1, int x2, int yLo,
                      int yHi) const;

  /** @brief Say out loud that this render is going to run on the CPU, and why.
   *
   * A GPU that is not there, or that will not take this band, is not a render failure — the CPU path is the
   * same render (D-51) — so the only honest thing to do with a fallback is *report* it: a user who ticked
   * the box and sees no speed-up deserves to know whether the reason is "no OpenCL device here", "this
   * card cannot hold this band" or "this level reads rows the gather window cut out". Once per render,
   * because the reason does not change from band to band and a band split would otherwise print it hundreds
   * of times.
   *
   * Two channels, because a host decides what it shows: @c eMessageLog — the OFX call for a sentence that
   * is not an error, and the one a host is free to keep out of the user's face, which Natron 2.6 does — and
   * the file named by the env var the Support library reads, which is what a host that shows nothing still
   * leaves behind. The first is wrapped in a try because the message suite is optional in OFX 1.4: a host
   * without it makes the Support library throw, and an exception leaving a render entry point would turn a
   * fallback into the error this knob exists to never be. */
  void reportGpuFallback(const std::string &why);
};

namespace {

/** @brief Is this the same bokeh picture the sprite library is already sampling?
 *
 * Byte-for-byte, because the only cheaper question ("same bounds?") is not the same question: a
 * user can repaint a bokeh map without resizing it, and a kernel cached from the old painting is
 * the wrong answer to the same radius. The comparison is O(pixels) once per render, against a
 * picture already capped at kMaxBokehSide per axis, which is the cheaper side of a kernel build. */
bool sameBokehImage(const zd::PlaneImage &a, const zd::PlaneImage &b)
{
  return a.width == b.width && a.height == b.height && a.channels == b.channels && a.px == b.px;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////
// instance

ZDefocusZPlugin::ZDefocusZPlugin(OfxImageEffectHandle handle)
  : OFX::ImageEffect(handle), dstClip_(0), srcClip_(0), depthClip_(0), bokehClip_(0), maskClip_(0),
    _haveSpriteImage(false), _spriteCap(0.0), _keyframed(true), _gpuLogged(false)
{
  dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
  srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
  if (getContext() == OFX::eContextGeneral) {
    depthClip_ = fetchClip("Depth");
    bokehClip_ = fetchClip("Bokeh");
    maskClip_ = fetchClip("Mask");
  }

  depthMode_ = fetchChoiceParam("depthMode");
  depthInvert_ = fetchBooleanParam("depthInvert");
  focusValue_ = fetchDoubleParam("focusValue");
  focusRange_ = fetchDoubleParam("focusRange");
  depthNear_ = fetchDoubleParam("depthNear");
  depthFar_ = fetchDoubleParam("depthFar");
  cocScale_ = fetchDoubleParam("cocScale");
  depthChannel_ = fetchStringParam("depthChannel");

  blurFrom_ = fetchChoiceParam("blurFrom");
  blurSize_ = fetchDoubleParam("blurSize");
  maxBlur_ = fetchDoubleParam("maxBlur");
  minBlur_ = fetchDoubleParam("minBlur");
  aperture_ = fetchDoubleParam("aperture");
  focalLength_ = fetchDoubleParam("focalLength");
  sensorWidth_ = fetchDoubleParam("sensorWidth");
  depthScale_ = fetchDoubleParam("depthScale");

  layers_ = fetchIntParam("layers");
  layerSpacing_ = fetchDoubleParam("layerSpacing");
  layerCurve_ = fetchDoubleParam("layerCurve");
  dither_ = fetchDoubleParam("dither");
  fillForeground_ = fetchBooleanParam("fillForeground");
  spriteRadiusCap_ = fetchDoubleParam("spriteRadiusCap");
  bloom_ = fetchBooleanParam("bloom");
  bloomThreshold_ = fetchDoubleParam("bloomThreshold");
  bloomGain_ = fetchDoubleParam("bloomGain");
  bloomGamma_ = fetchBooleanParam("bloomGamma");

  shape_ = fetchChoiceParam("shape");
  discShape_ = fetchDoubleParam("discShape");
  aspect_ = fetchDoubleParam("aspect");
  rotation_ = fetchDoubleParam("rotation");
  blades_ = fetchIntParam("blades");
  roundness_ = fetchDoubleParam("roundness");
  innerSize_ = fetchDoubleParam("innerSize");
  innerFeather_ = fetchDoubleParam("innerFeather");
  innerBrightness_ = fetchDoubleParam("innerBrightness");
  catadioptric_ = fetchBooleanParam("catadioptric");
  catadioptricSize_ = fetchDoubleParam("catadioptricSize");
  bokehSource_ = fetchChoiceParam("bokehSource");

  preview_ = fetchChoiceParam("preview");
  channels_ = fetchStringParam("channels");
  mix_ = fetchDoubleParam("mix");
  maskInvert_ = fetchBooleanParam("maskInvert");
  useGPU_ = fetchBooleanParam("useGPU");
}

/** @brief One double knob, read at @p t when this host interpolates, and statically when it does
 * not (the TimeWarp precedent, D-21): the Support library's timed getter throws on a host without
 * keyframe interpolation, so the refusal is noticed once per instance and every knob is then simply
 * constant. A depth field can be animated — a rack focus is the whole reason these knobs are here. */
double ZDefocusZPlugin::readDouble(OFX::DoubleParam *knob, double t) const
{
  if (_keyframed) {
    try {
      return knob->getValueAtTime(t);
    } catch (const std::exception &) {
      _keyframed = false;
    }
  }
  return knob->getValue();
}

/** @brief The knob table, read through the core's own option structs and clamped by the core's own
 * `sanitise()` overloads, so the numbers the render uses are the numbers the model documents.
 *
 * The shape, layer, flag and channel-selection knobs are read as **constants**: they decide what a
 * kernel or a level set *is*, so a value that varied over time would make a cached kernel depend on
 * the frame it was built on (the TimeWarp `quality` knob's same reasoning). The depth, blur, optics
 * and mix knobs are read at @p t, because those are the ones a focus pull animates. */
Params ZDefocusZPlugin::readParams(double t) const
{
  Params p;

  depthMode_->getValue(p.focus.depthMode);
  p.focus.depthInvert = depthInvert_->getValue();
  p.focus.focusValue = readDouble(focusValue_, t);
  p.focus.focusRange = readDouble(focusRange_, t);
  p.focus.depthNear = readDouble(depthNear_, t);
  p.focus.depthFar = readDouble(depthFar_, t);
  p.focus.cocScale = readDouble(cocScale_, t);

  blurFrom_->getValue(p.focus.blurFrom);
  p.focus.blurSize = readDouble(blurSize_, t);
  p.focus.maxBlur = readDouble(maxBlur_, t);
  p.focus.minBlur = readDouble(minBlur_, t);
  p.focus.aperture = readDouble(aperture_, t);
  p.focus.focalLength = readDouble(focalLength_, t);
  p.focus.sensorWidth = readDouble(sensorWidth_, t);
  p.focus.depthScale = readDouble(depthScale_, t);

  layers_->getValue(p.layers.layers);
  p.layers.layerSpacing = readDouble(layerSpacing_, t);
  p.layers.layerCurve = readDouble(layerCurve_, t);
  p.layers.dither = readDouble(dither_, t);
  p.layers.fillForeground = fillForeground_->getValue();
  p.layers.spriteRadiusCap = readDouble(spriteRadiusCap_, t);
  p.layers.bloom = bloom_->getValue();
  p.layers.bloomThreshold = readDouble(bloomThreshold_, t);
  p.layers.bloomGain = readDouble(bloomGain_, t);
  p.layers.bloomGamma = bloomGamma_->getValue();

  shape_->getValue(p.bokeh.shape);
  p.bokeh.discShape = readDouble(discShape_, t);
  p.bokeh.aspect = readDouble(aspect_, t);
  p.bokeh.rotation = readDouble(rotation_, t);
  blades_->getValue(p.bokeh.blades);
  p.bokeh.roundness = readDouble(roundness_, t);
  p.bokeh.innerSize = readDouble(innerSize_, t);
  p.bokeh.innerFeather = readDouble(innerFeather_, t);
  p.bokeh.innerBrightness = readDouble(innerBrightness_, t);
  p.bokeh.catadioptric = catadioptric_->getValue();
  p.bokeh.catadioptricSize = readDouble(catadioptricSize_, t);
  bokehSource_->getValue(p.bokeh.source);

  preview_->getValue(p.preview);
  mix_->getValue(p.mix);
  p.maskInvert = maskInvert_->getValue();
  depthChannel_->getValue(p.depthChannel);
  useGPU_->getValue(p.useGPU); // read as a constant: a device is chosen for a render, not per frame

  std::string sel;
  channels_->getValue(sel);
  OfxLab::parseChannels(sel, p.sel, "all"); // the default is every channel

  // One set of constants for the descriptor's ranges and the model's clamps (zd.h, bokeh.h,
  // layered.h), so a knob cannot offer a value the maths would then have to refuse.
  zd::sanitise(p.focus);
  zd::sanitise(p.layers);
  zd::sanitise(p.bokeh);
  return p;
}

namespace {

/** @brief The fallback sentence's second channel: a file, but only one somebody asked for.
 *
 * @c eMessageLog is the right OFX call and the first thing tried, and what the Support library's own
 * @c OFX::Log would be the second — except that its `open()` is compiled out unless the library was
 * built with `DEBUG`, so in a shipped plugin its `print()` writes to a handle that was never opened.
 * Writing the line here instead costs a `fopen` and keeps the rule that a plugin does not create files
 * next to a host unprompted: the path is the env var the Support library itself reads
 * (`OFX_PLUGIN_LOGFILE`), and without it nothing is opened, opened again, or appended to.
 *
 * It runs even when the message suite worked. A host is free to keep a log-level message wherever it
 * likes — Natron 2.6 does not put one on its console at all — and "we fell back, here is why" is worth
 * nothing if the only proof that it happened is a sentence nobody can read. */
void logGpuFallbackLine(const std::string &line)
{
  const char *path = std::getenv("OFX_PLUGIN_LOGFILE");
  if (!path || !*path)
    return;
  FILE *f = std::fopen(path, "a");
  if (!f)
    return; // a log line is never a reason to fail a render
  std::fprintf(f, "%s\n", line.c_str());
  std::fclose(f);
}

/* gpu.cpp's own refusal sentences already end with a "— CPU path" / "— the render stays on the CPU"
 * tail, which the prefix below has just said.  Dropping it keeps the reason and prints it once: this is
 * the one line a user reads to find out why ticking a checkbox did nothing, so it should read as one
 * sentence.  A tail that does not name the CPU is part of the reason and stays. */
std::string dropCpuTail(const std::string &why)
{
  const std::string dash = " \xe2\x80\x94 "; // " — "
  const size_t at = why.rfind(dash);
  if (at == std::string::npos)
    return why;
  const std::string tail = why.substr(at + dash.size());
  if (tail.find("CPU") == std::string::npos)
    return why;
  return why.substr(0, at);
}

} // namespace

void ZDefocusZPlugin::reportGpuFallback(const std::string &why)
{
  if (_gpuLogged)
    return; // one sentence per render: a band split re-asks the same question and gets the same answer
  _gpuLogged = true;

  const std::string line = "ZDefocusZ: rendering this band on the CPU — " + dropCpuTail(why);
  try {
    sendMessage(OFX::Message::eMessageLog, "ofxlab.gpu", line);
  } catch (const std::exception &) {
    // The message suite is optional in OFX 1.4 and a host without it makes the Support library throw.
    // Swallowing that is the point: an exception leaving a render entry point would turn a fallback into
    // the error the knob is written so it never is.
  }
  logGpuFallbackLine(line);
}

/** @brief The gather the render needs on every side of the window, in pixels.
 *
 * @ref zd::maxKernelReach over the level set this knob set builds — the same helper
 * @ref zd::LayeredRender::prepare uses to pad its own CoC map — so the padding the host is asked for
 * and the padding the renderer reads are one number and cannot drift (D-38). It is deliberately
 * **not** multiplied by the render scale: the region of interest is expressed in canonical
 * coordinates, a host scales those up for a partial-quality render, and the renderer pads the rect it
 * is handed by the same integer, which is at least as much gather in canonical units either way.
 * `spriteRadiusCap` narrows it, because the gather is a property of the kernels the render is willing
 * to build and not of the deepest CoC the depth measured (D-45). */
int ZDefocusZPlugin::gatherPad(const Params &p) const
{
  const zd::LevelSet levels =
      zd::buildLevelSet(p.focus, p.layers.layers, p.layers.layerSpacing, p.layers.layerCurve);
  const double aspect = p.bokeh.aspect > 0.0 ? p.bokeh.aspect : 1.0;
  const double reach = zd::maxKernelReach(levels, aspect, p.layers.fillForeground, p.layers.spriteRadiusCap);
  return reach > 0.0 ? (int)std::ceil(reach) : 0;
}

/** @brief The image width in pixels, which is what turns a *lens* CoC in millimetres into one in
 * pixels (@ref zd::resolveBlurSize), and therefore what `aperture`/`focalLength` mean on this clip.
 *
 * Measured on the **Source clip's** region of definition and its own pixel aspect ratio — never on
 * this plugin's padded RoD and never on the render window — because the render window is a tile or a
 * crop of the image, and a lens's circle of confusion is a fraction of the *frame*, not of the tile.
 * Falling back to the output clip's RoD, and then to the render window, keeps a node with no source
 * connected answering something finite. A non-finite or zero aspect ratio means the host did not say:
 * square pixels, which is what every clip here is described as. */
double ZDefocusZPlugin::imageWidthPx(double t, const OfxRectD &fallback) const
{
  OfxRectD rod;
  if (srcClip_ && srcClip_->isConnected())
    rod = srcClip_->getRegionOfDefinition(t);
  else if (dstClip_)
    rod = dstClip_->getRegionOfDefinition(t);

  if (!boundedD(rod))
    rod = fallback;
  if (!boundedD(rod))
    return 0.0; // nothing finite to measure: the lens mode resolves to no blur, which is honest

  double par = 1.0;
  if (srcClip_ && srcClip_->isConnected())
    par = srcClip_->getPixelAspectRatio();
  else if (dstClip_)
    par = dstClip_->getPixelAspectRatio();
  if (!finiteD(par) || par <= 0.0)
    par = 1.0;

  return (rod.x2 - rod.x1) / par;
}

/** @brief Make sure the kernel library can answer this render, and only rebuild what changed.
 *
 * Three separate reasons to rebuild, and each one is checked separately because they change at
 * different rates: the **options** (a knob changed → the kernels mean different apertures), the
 * **bokeh picture** (a repainted map, same size, same options), and the **level set** (a keyframed
 * `maxBlur` or `layerSpacing` asks for radii the cache has never seen, and without this check those
 * slots would accumulate for every frame of the animation), and the **radius cap** — what the cache is
 * asked for is the capped radius, so a cap change is the same kind of change and the uncapped kernels
 * it stops the render building must not stay behind. A cached kernel built from another
 * picture is the wrong answer to the same question, which is why bokeh.h's own `setSourceImage`
 * destroys the whole cache and this function only calls it when the picture actually differs.
 *
 * Must run before the memory estimate: the estimate counts the kernels and the mip chain. */
void ZDefocusZPlugin::prepareSprites(const Params &p, const zd::LevelSet &levels, OFX::Image *bokehImg)
{
  // Only an image shape samples a picture; an analytic shape must not be made to read one.
  zd::PlaneImage img;
  bool have = false;
  if (p.bokeh.shape == zd::eShapeImage && bokehImg && bokehClip_ && bokehClip_->isConnected()) {
    const OfxRectI b = bokehImg->getBounds();
    const int w = b.x2 - b.x1, h = b.y2 - b.y1;
    if (w > kMaxBokehSide || h > kMaxBokehSide)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported); // a plate, not an aperture (D-40)
    have = readPlaneImage(bokehImg, img);
  }

  if (!_sprites || !(p.bokeh == _spriteOptions)) {
    _sprites.reset(new zd::SpriteLibrary(p.bokeh)); // sanitises its own copy; p.bokeh is already
    _spriteOptions = p.bokeh;                       // sanitised, and sanitise() is idempotent, so
    _spriteLevels = zd::LevelSet();                 // comparing the two is comparing the same thing
    _spriteImage = zd::PlaneImage();
    _haveSpriteImage = false;
  }

  if (!sameBokehImage(img, _spriteImage) || have != _haveSpriteImage) {
    if (have)
      _sprites->setSourceImage(img);
    else if (_haveSpriteImage)
      _sprites->clear(); // the picture went away: an image shape with nothing on the clip gets the
    _spriteImage = have ? std::move(img) : zd::PlaneImage();
    _haveSpriteImage = have;
  }

  if (!(levels.radius == _spriteLevels.radius) || p.layers.spriteRadiusCap != _spriteCap) {
    _sprites->clear();
    _spriteLevels = levels;
    _spriteCap = p.layers.spriteRadiusCap; // the cap is part of what the cache holds: a capped ladder
  }                                        // asks for kernels this cache has never seen, and a cache that
                                           // kept the uncapped ones would hold exactly what the cap is
                                           // there to stop the render building (D-45)
}

////////////////////////////////////////////////////////////////////////////////
// out

void ZDefocusZPlugin::copySourceRows(OFX::Image *dst, const OFX::Image *src, int dstComp, int x1,
                                     int x2, int yLo, int yHi) const
{
  if (x2 <= x1 || yHi <= yLo)
    return;
  const size_t n = (size_t)(x2 - x1) * (size_t)dstComp;
  // The row is asked for at the *source's own* left bound, never at the render window's: this node's
  // render window reaches past the picture by its gather pad, and a scanline asked for at a column the
  // host did not deliver is not a row at all (the note in @ref emitRow has the measured case). The
  // columns the source does not deliver become transparent black, which is what the rest of the core
  // reads them as. Both clips here are the same component set and the same premultiplication, which is
  // what makes a byte copy the right answer for the rest (render's `verbatim` test guarantees it).
  const OfxRectI sb = src ? src->getBounds() : OfxRectI{0, 0, 0, 0};
  for (int y = yLo; y < yHi; ++y) {
    float *d = (float *)dst->getPixelAddress(x1, y);
    if (!d)
      continue; // the destination does not deliver this scanline
    const float *s =
        (src && y >= sb.y1 && y < sb.y2) ? (const float *)src->getPixelAddress(sb.x1, y) : 0;
    if (!s) {
      for (size_t i = 0; i < n; ++i)
        d[i] = 0.0f; // the source is not there either: transparent black
      continue;
    }
    for (int x = x1; x < x2; ++x) {
      float *dp = d + (size_t)(x - x1) * (size_t)dstComp;
      if (x < sb.x1 || x >= sb.x2) {
        for (int k = 0; k < dstComp; ++k)
          dp[k] = 0.0f;
      } else {
        std::memcpy(dp, s + (size_t)(x - sb.x1) * (size_t)dstComp, (size_t)dstComp * sizeof(float));
      }
    }
  }
}

/** @brief The last stage of a composited row, in the destination's own space.
 *
 * Four steps, in the order that makes each one exact:
 *
 *   0. the Source pixel is read in the *source's* premultiplication and converted into the
 *      destination's, because the two clips need not agree and the dissolve below has both of them
 *      as operands. The core did the same thing on the way in (@ref ColourRowPlane) and step 1 does
 *      it on the way out; when the two flags match this is the identity, which is exactly the case
 *      render takes as the cheaper byte-for-byte copy instead. The row is addressed at the source's
 *      own bounds, and a pixel outside them has no source operand at all (see the note in the body).
 *   1. the core's straight colour becomes the host's premultiplied colour, with the alpha the render
 *      actually has. A 1-component render needs no multiply at all: layered.h reads such a plane as
 *      grey with full coverage, so its composited alpha *is* 1 wherever a level landed and the value
 *      written there is already the density.
 *   2. the channels the `channels` knob does not select come from the Source, otherwise unchanged —
 *      which is how an RGBA render keeps its own alpha, and how `luma`-style partial processing works.
 *   3. the mask/mix dissolve of D-20, `out = src + mix·(invert ? 1−mask : mask)·(effect − src)`, in
 *      the same space. A channel that passed through has effect == src, so it is untouched by the
 *      dissolve whatever the mask says — no special case needed, and none taken.
 */
void ZDefocusZPlugin::emitRow(OFX::Image *dst, const OFX::Image *src, const ScratchRowPlane &scratch,
                              int srcComp, int dstComp, bool srcPremult, bool dstPremult, int x1,
                              int x2, int y, const Params &p, const OfxLab::MaskReader &mask) const
{
  float *drow = (float *)dst->getPixelAddress(x1, y);
  if (!drow)
    return;
  // The source row is asked for at the *source's own* bounds, not at the render window's. This node's
  // render window reaches past the picture by its gather pad, and a host is not obliged to deliver a
  // scanline for a column outside the image it made: TimeWarp and SharpenZ already address their
  // source rows this way. Asking here for a column that is not there used to answer null for the whole
  // row, which silently dropped the Source operand of the dissolve below (measured in Natron, and the
  // reason the `channels` merge-back looked like it was copying black; DECISIONS.md D-42).
  const OfxRectI sb = src ? src->getBounds() : OfxRectI{0, 0, 0, 0};
  const float *srow = (src && y >= sb.y1 && y < sb.y2)
                          ? (const float *)src->getPixelAddress(sb.x1, y)
                          : 0;
  const float *erow = scratch.values();
  const int ecomp = scratch.comp;
  const bool haveMask = mask.active();
  // Every step below works in the destination's space, and the source row arrives in the *source's*.
  // The two agree whenever the render took the byte-for-byte copy instead (render's `verbatim` test),
  // so this conversion is the other half of that test. Only a 4-component plane has a colour and an
  // alpha to move between them: a 1-component plane's single value *is* its density, which is why
  // layered.h reads such a plane as grey with full coverage and no multiply applies to it.
  const bool both4 = (srcComp == 4 && dstComp == 4);
  const bool divide = both4 && srcPremult && !dstPremult;
  const bool premultiply = both4 && !srcPremult && dstPremult;

  for (int x = x1; x < x2; ++x) {
    const size_t ei = (size_t)(x - x1) * (size_t)ecomp;
    float *d = drow + (size_t)(x - x1) * (size_t)dstComp;

    float sbuf[4];
    const float *s = 0;
    if (srow && x >= sb.x1 && x < sb.x2) {
      s = srow + (size_t)(x - sb.x1) * (size_t)srcComp;
      if (divide || premultiply) {
        const float a = s[3];
        // Under a == 0 a premultiplied colour is 0 and an unpremultiplied one is undefined: 0 is the
        // reading the core uses for "nothing here", and it is what dividing by zero would produce.
        const float k = divide ? (a > 0.0f ? 1.0f / a : 0.0f) : a;
        sbuf[0] = s[0] * k;
        sbuf[1] = s[1] * k;
        sbuf[2] = s[2] * k;
        sbuf[3] = a; // alpha is a density, never a transform of the colour
        s = sbuf;
      }
    }

    float eff[4];
    if (dstComp == 4) {
      const float a = erow[ei + 3];
      const float m = dstPremult ? a : 1.0f; // straight -> premultiplied, with the render's own alpha
      eff[0] = erow[ei + 0] * m;
      eff[1] = erow[ei + 1] * m;
      eff[2] = erow[ei + 2] * m;
      eff[3] = a;
    } else {
      eff[0] = erow[ei];
    }

    // An Alpha clip has one channel, so either of the two letters that can name it selects it: `a`
    // because that is the channel an alpha clip is, `r` because a host that shows a channel selector
    // for a 1-component clip labels it with its first component. Documented in DESIGN.md §2.
    for (int c = 0; c < dstComp; ++c) {
      const bool on = dstComp == 4 ? p.sel[c] : (p.sel[0] || p.sel[3]);
      if (!on)
        eff[c] = s ? s[c] : 0.0f;
    }

    double t = p.mix;
    if (haveMask)
      t = OfxLab::maskMix(p.mix, mask.valueAt(x, y), p.maskInvert);
    for (int c = 0; c < dstComp; ++c)
      d[c] = OfxLab::blendOut(s ? s[c] : 0.0f, eff[c], t);
  }
}

////////////////////////////////////////////////////////////////////////////////
// previews

/** @brief The diagnostic outputs, which replace the render entirely.
 *
 * A preview is an instrument, so it is grey, its alpha is 1, and it bypasses the mask and the mix —
 * a diagnostic that has been dissolved against the input at 40% is not measuring anything. Writing
 * alpha 1 is also the one choice that reads the same in a premultiplied and an unpremultiplied host.
 *
 * `Depth` and `CoC` are why the depth semantics are testable at all: they show the number the model
 * read and the radius it turned it into, in the same window the render uses. `Layers` shows the level
 * assignment the renderer *actually performs* — it calls the exported @ref zd::pixelPair, the same
 * call the extraction stage makes — rather than a re-derivation, so what a user sees is what the blur
 * does (0.5 is the in-focus level, above it nearer, below it farther, in units of the level set).
 * `Bokeh` is the largest kernel in the ladder, upsampled by its own cell, which is the aperture
 * this knob set builds and the quickest way to see what `shape`/`blades`/`innerSize` are doing. `spriteRadiusCap` is part of that aperture: the preview shows the kernel the
 * render would really build, capped kernel and all (D-45).
 */
void ZDefocusZPlugin::renderPreview(const Params &p, const zd::LevelSet &levels,
                                    const zd::RenderRect &rect, OFX::Image *dst,
                                    const DepthRowPlane &depth, double blurSizePx, int dstComp)
{
  const int w = rect.width();
  const double maxBlur = p.focus.maxBlur;
  const int L = levels.count();
  const double rmax = L > 0 ? levels.radius[(size_t)L - 1] : 0.0;

  std::vector<float> line((size_t)w, 0.0f);
  const zd::CoCMap coc = (p.preview == zd::ePreviewCoC || p.preview == zd::ePreviewLayers)
                             ? zd::prepareCoC(depth, rect, p.focus, blurSizePx)
                             : zd::CoCMap();

  // the bokeh preview: the widest kernel this level set can ask for (spriteRadiusCap included, so it is
  // the aperture the render builds and not the deepest CoC the depth measured), nearest-neighbour
  // upsampled by the cell it gathers at, centred in the window
  const zd::Kernel *boot = 0;
  float bootMax = 0.0f;
  if (p.preview == zd::ePreviewBokeh && L > 0 && rmax > 0.0) {
    const double aspect = p.bokeh.aspect > 0.0 ? p.bokeh.aspect : 1.0;
    const double rboot = zd::cappedRadius(rmax, p.layers.spriteRadiusCap); // the kernel the render would
    boot = &_sprites->kernel(rboot, (double)zd::cellForRadius(rboot, aspect)); // actually build, cap
    const float *cov = boot->plane(boot->coveragePlane()); // the aperture's own plane, whatever
    for (int i = 0; i < boot->span * boot->span; ++i)      // the shape
      if (cov[i] > bootMax)
        bootMax = cov[i]; // the brightest tap becomes 1. bokeh.h normalises a plane to unit
  }                       // *mass*, so a wide kernel's every tap is below 1 and truncating the
  if (!(bootMax > 0.0f)) // max to an int made the scale 0 and the whole preview 1e-3 (D-43).
    bootMax = 1.0f;      // The maximum *value* is the same whichever tap attains it, so this
                         // cannot depend on the order the plane was scanned in.

  for (int y = rect.y1; y < rect.y2; ++y) {
    float *d = (float *)dst->getPixelAddress(rect.x1, y);
    if (!d)
      continue;

    if (p.preview == zd::ePreviewDepth) {
      const float *dr = depth.row(y);
      const int xA = std::max(rect.x1, depth.x1), xB = std::min(rect.x2, depth.x2);
      const size_t stride = (size_t)depth.stride();
      const size_t chan = (size_t)depth.channel();
      for (int x = 0; x < w; ++x)
        line[x] = 0.0f; // no depth delivered here: 0, which the model itself reads as "no geometry"
      if (dr)
        for (int x = xA; x < xB; ++x) {
          // raw, in the clip's own units: the same component the CoC model was built from
          line[x - rect.x1] = dr[(size_t)(x - depth.x1) * stride + chan];
        }
    } else if (p.preview == zd::ePreviewCoC) {
      for (int x = 0; x < w; ++x)
        line[x] = maxBlur > 0.0 ? (float)(coc.cocAt(rect.x1 + x, y) / maxBlur) : 0.0f; // 1 == maxBlur
    } else if (p.preview == zd::ePreviewLayers) {
      for (int x = 0; x < w; ++x) {
        const int px = rect.x1 + x;
        const zd::LevelPair pr = zd::pixelPair((double)coc.cocAt(px, y), px, y, levels, p.layers);
        const int i = pr.w1 > pr.w0 ? pr.i1 : pr.i0; // the level that owns the pixel
        double v = 0.5;                              // the in-focus level, mid-grey
        if (i > 0 && rmax > 0.0) {
          const double f = levels.radius[(size_t)i] / rmax;
          // far side: bigger CoC == farther away; near side: bigger CoC == nearer to the camera
          v = coc.sideAt(px, y) == zd::eSideNear ? 0.5 * (1.0 + f) : 0.5 * (1.0 - f);
        }
        line[x] = (float)v;
      }
    } else if (p.preview == zd::ePreviewBokeh) {
      for (int x = 0; x < w; ++x)
        line[x] = 0.0f;
      if (boot) {
        const int half = boot->half();
        const int cell = boot->cell > 0.0 ? (int)boot->cell : 1;
        const int cx = rect.x1 + w / 2, cy = rect.y1 + rect.height() / 2;
        const float *cov = boot->plane(boot->coveragePlane());
        const double inv = 1.0 / (double)bootMax;
        // this row's tap row: the cell block the kernel occupies at this height. Painting every
        // tap row into every output row smeared the aperture into one row repeated down the
        // frame, which is not a picture of a shape (D-43).
        const int dy = (int)std::floor((double)(y - cy + cell / 2) / (double)cell);
        if (dy >= -half && dy <= half) {
          for (int dx = -half; dx <= half; ++dx) {
            const double v = (double)cov[(size_t)(dy + half) * (size_t)boot->span + (size_t)(dx + half)];
            if (!(v > 0.0))
              continue;
            const int x0 = cx + dx * cell - cell / 2, x1 = x0 + cell;
            for (int px = x0; px < x1; ++px) {
              if (px < rect.x1 || px >= rect.x2)
                continue;
              line[px - rect.x1] = (float)(v * inv);
            }
          }
        }
      } // no CoC ladder, no kernel: black, because there is no aperture this knob set builds
    }

    if (dstComp == 4) {
      for (int x = 0; x < w; ++x) {
        d[x * 4 + 0] = line[x];
        d[x * 4 + 1] = line[x];
        d[x * 4 + 2] = line[x];
        d[x * 4 + 3] = 1.0f;
      }
    } else {
      for (int x = 0; x < w; ++x)
        d[x] = line[x];
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// render

/* One render, in the order the core's driving recipe states (layered.h): resolve the knobs,
 * fetch the pixels, make sure the kernels exist, measure what the render costs, then drive the
 * level visits band by band.
 *
 * The bands are the memory story (D-40): a level's buffers are bounded by the rect it is given,
 * so `prepare()` is called once per band with a different rect and the peak falls with the band
 * height. `prepare()` resets every piece of state it holds, which is what lets one renderer serve
 * the whole set of bands. Within a band the stages run serially, in the order the core requires;
 * the parallelism a host wants comes from its own tiles and frames, exactly as in TimeWarp and
 * SharpenZ (rule 3 of AGENTS.md: no threads inside a plugin). */
void ZDefocusZPlugin::render(const OFX::RenderArguments &args)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  // v1 scope: float32 only, RGBA or Alpha (DECISIONS.md D-07, and the descriptor offers exactly
  // those two, so a host asking for anything else is asking for something not described).
  if (dstClip_->getPixelDepth() != OFX::eBitDepthFloat)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

  const OFX::PixelComponentEnum dstComponents = dstClip_->getPixelComponents();
  int dstComp = 0;
  if (dstComponents == OFX::ePixelComponentRGBA)
    dstComp = 4;
  else if (dstComponents == OFX::ePixelComponentAlpha)
    dstComp = 1;
  else
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
  const bool dstPremult = (dstClip_->getPreMultiplication() == OFX::eImagePreMultiplied);

  std::unique_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
  if (!dst)
    OFX::throwSuiteStatusException(kOfxStatFailed);

  // The render window, clipped to what the destination actually delivers: a host may ask for a
  // tile, and the image behind it has bounds of its own.
  const OfxRectI db = dst->getBounds();
  zd::RenderRect rect;
  rect.x1 = std::max(args.renderWindow.x1, db.x1);
  rect.y1 = std::max(args.renderWindow.y1, db.y1);
  rect.x2 = std::min(args.renderWindow.x2, db.x2);
  rect.y2 = std::min(args.renderWindow.y2, db.y2);
  if (rect.empty())
    return; // nothing to write, nothing to allocate

  const Params p = readParams(args.time);
  _gpuLogged = false; // this render has not yet said anything about a device (reportGpuFallback)
  const zd::LevelSet levels =
      zd::buildLevelSet(p.focus, p.layers.layers, p.layers.layerSpacing, p.layers.layerCurve);

  std::unique_ptr<OFX::Image> src;
  if (srcClip_ && srcClip_->isConnected())
    src.reset(srcClip_->fetchImage(args.time));

  int srcComp = 0;
  bool srcPremult = false;
  if (src) {
    // No bit-depth or component conversion in v1: a node whose Source and Output formats differ
    // is the host's to fix, not ours to guess at (the SharpenZ precedent).
    if (src->getPixelDepth() != OFX::eBitDepthFloat || src->getPixelComponents() != dstComponents)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    srcComp = dstComp;
    srcPremult = (src->getPreMultiplication() == OFX::eImagePreMultiplied);
  }

  if (!src) {
    // No picture to defocus: transparent black, which is what "there is nothing here" looks like
    // in every case the rest of this function would have produced.
    copySourceRows(dst.get(), 0, dstComp, rect.x1, rect.x2, rect.y1, rect.y2);
    return;
  }

  // Depth: the Depth clip when one is connected, otherwise the Source itself. Which component that
  // means is the `depthChannel` knob's question (see resolveDepthChannel); the default answers
  // "the Z channel of whatever this clip is", which is how a Blender Z pass or a Nuke ZDown
  // arrives on a single input.
  std::unique_ptr<OFX::Image> depthImg;
  OFX::Image *depthSrc = 0;
  int depthComp = 0;
  if (depthClip_ && depthClip_->isConnected()) {
    depthImg.reset(depthClip_->fetchImage(args.time));
    if (depthImg) {
      if (depthImg->getPixelDepth() != OFX::eBitDepthFloat)
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
      if (depthImg->getPixelComponents() == OFX::ePixelComponentRGBA)
        depthComp = 4;
      else if (depthImg->getPixelComponents() == OFX::ePixelComponentAlpha)
        depthComp = 1;
      else
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported); // not a component set described
      depthSrc = depthImg.get();
    }
  }
  if (!depthSrc) {
    depthSrc = src.get(); // the depth lives in the picture
    depthComp = srcComp;
  }

  ColourRowPlane image;
  image.set(src.get());
  DepthRowPlane depth;
  depth.set(depthSrc, depthComp, resolveDepthChannel(p.depthChannel, depthComp));

  // The bokeh picture, read only when an image shape can use it, and only then fed to the kernel
  // library (bokeh.h: an analytic shape must not be made to sample a picture).
  std::unique_ptr<OFX::Image> bokeh;
  if (p.bokeh.shape == zd::eShapeImage && bokehClip_ && bokehClip_->isConnected()) {
    bokeh.reset(bokehClip_->fetchImage(args.time));
    if (bokeh && bokeh->getPixelDepth() != OFX::eBitDepthFloat)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
  }
  prepareSprites(p, levels, bokeh.get()); // may refuse an oversized picture: before the estimate,
                                          // which counts the mip chain it built

  // The CoC at infinite depth, in px. The lens mode needs the *image* width, not this window's
  // width, because a circle of confusion is a fraction of the frame (see imageWidthPx).
  OfxRectD winFallback;
  winFallback.x1 = (double)rect.x1;
  winFallback.y1 = (double)rect.y1;
  winFallback.x2 = (double)rect.x2;
  winFallback.y2 = (double)rect.y2;
  const double blurSizePx = zd::resolveBlurSize(p.focus, imageWidthPx(args.time, winFallback));

  const double aspect = p.bokeh.aspect > 0.0 ? p.bokeh.aspect : 1.0;
  const bool perChan = zd::usesPerChannelKernels(p.bokeh, _spriteImage);

  /* The device's ceiling for one band, asked of the device *before* the plan is made. Zero when there is no
   * device to plan for: the checkbox off, or a preview — an instrument this plugin writes on the CPU by
   * design (D-43), so probing a GPU for a render that cannot use one would be a dlopen and a device query
   * for nothing. Otherwise it is the largest single allocation this device hands out (zd::gpu::maxAllocation),
   * which is the number gpu.cpp's Session refuses a band over: knowing it here is what lets the band split
   * hand the device a band it will accept instead of being refused halfway through a stage. */
  const bool wantsGpu = p.useGPU && p.preview == zd::ePreviewResult;
  const size_t deviceCap = wantsGpu ? zd::gpu::maxAllocation() : 0;
  const MemoryPlan mem =
      measureMemory(levels, aspect, p.layers.fillForeground, p.layers.spriteRadiusCap, perChan, _spriteImage,
                    rect.width(), rect.height(), srcComp, deviceCap);
  if (!mem.fits) {
    // Not even a one-row band fits this instance's budget: say so rather than allocate and be the
    // process's OOM candidate. The knob that caused it is `maxBlur` (a CoC is also a kernel size).
    // The device's ceiling is *not* part of this test — a device that cannot hold the band is a CPU
    // render, not a failed one — so `fits` stays the host's own question (measureMemory).
    OFX::throwSuiteStatusException(kOfxStatErrMemory);
  }

  // The checkbox is a request. The plan's answer is arithmetic (the band split that keeps the device under
  // its own ceiling), and gpu.cpp's is the runtime answer for whatever the plan could not see; both end in
  // the same CPU loop below, with the reason reported once (reportGpuFallback).
  bool useGpu = wantsGpu && mem.gpuFits;
  if (wantsGpu && !mem.gpuFits) {
    /* A ceiling of zero is not a band that is too big: it means the probe never found a device, and
     * `gpuFits` is false for that reason too (`maxAllocation` answers 0 when there is no device).  The
     * memory sentence would then blame the render's size for a machine that has nothing to size, so ask
     * the probe which of the two it is and say the fact it actually learned (D-51).  The numbers are
     * reported only where a device is really present and really too small. */
    if (!zd::gpu::available()) {
      std::string why = zd::gpu::probeMessage();
      if (why.empty())
        why = "this machine has no OpenCL GPU device this renderer can use";
      reportGpuFallback(why);
    } else {
      const unsigned long long need = (unsigned long long)(mem.deviceBytes / (1024.0 * 1024.0));
      const unsigned long long cap = (unsigned long long)(deviceCap / (1024.0 * 1024.0));
      reportGpuFallback("even a one-row band needs " + std::to_string(need) +
                        " MB of device buffers and this OpenCL device hands out at most " + std::to_string(cap) +
                        " MB in one allocation");
    }
    useGpu = false;
  }

  // A preview replaces the render, and with it the dissolve: an instrument dissolved against its
  // own input at 40 % is measuring nothing.
  if (p.preview != zd::ePreviewResult) {
    renderPreview(p, levels, rect, dst.get(), depth, blurSizePx, dstComp);
    return;
  }

  std::unique_ptr<OFX::Image> mask;
  OfxLab::MaskReader maskReader; // inactive is "fully on", which is D-20's rule for an unconnected mask
  if (maskClip_ && maskClip_->isConnected()) {
    mask.reset(maskClip_->fetchImage(args.time));
    if (mask) {
      if (mask->getPixelComponents() == OFX::ePixelComponentRGBA)
        maskReader.set(mask.get(), 3);
      else if (mask->getPixelComponents() == OFX::ePixelComponentAlpha)
        maskReader.set(mask.get(), 0);
      else
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  }

  zd::LayeredRender rend(p.layers, *_sprites, levels);
  ScratchRowPlane scratch;
  scratch.reset(rect.x1, rect.x2, dstComp);

  // The all-sharp copy is bit-for-bit only when the two spaces agree; when they do not, the exact
  // answer is the core's own copy *through* the straight planes, which emitRow then converts.
  const bool verbatim = (srcComp == dstComp && srcPremult == dstPremult);
  const int bandRows = mem.bandRows > 0 ? mem.bandRows : 1;

  for (int bandY = rect.y1; bandY < rect.y2; bandY += bandRows) {
    if (abort())
      return;
    const int bandHi = std::min(rect.y2, bandY + bandRows);
    zd::RenderRect band;
    band.x1 = rect.x1;
    band.y1 = bandY;
    band.x2 = rect.x2;
    band.y2 = bandHi;

    if (!rend.prepare(depth, image, band, p.focus, blurSizePx))
      continue; // prepare resets everything it holds, so one renderer serves every band

    if (rend.plan().allSharp) {
      // Every CoC in this band's work area is 0: the render is its own input.
      if (verbatim) {
        copySourceRows(dst.get(), src.get(), dstComp, band.x1, band.x2, band.y1, band.y2);
        continue;
      }
      // One row at a time, because @c scratch is a single row and answers @c row(y) only for the row
      // it currently holds: copying the band first would find no row to copy into and silently
      // emit whatever the buffer already held (D-42).
      for (int y = band.y1; y < band.y2; ++y) {
        scratch.setRow(y);
        rend.copyInputThrough(scratch, y, y + 1);
        emitRow(dst.get(), src.get(), scratch, srcComp, dstComp, srcPremult, dstPremult, band.x1,
                band.x2, y, p, maskReader);
      }
      continue;
    }

    const std::vector<zd::LevelVisit> &order = rend.levelOrder();

    /* The whole effect of the checkbox: the same visits, in the same order, over the same rows, with
     * extraction and the convolution run on the device (gpu.h's `renderBand` walks this renderer's own
     * levelOrder() and hands back its accumulator). `fillLevel` stays on the CPU grid in both paths, which
     * is why the two grids round trip once per visit — keeping one coverage fill for both devices is worth
     * more than the upload it costs (DESIGN.md §7).
     *
     * A false return leaves this band exactly as `prepare()` left it: nothing a Session holds is written
     * back to the renderer before its finish, so the CPU loop below renders *this band* rather than
     * continuing a half-rendered one, and the renderer's own level state is reset by the next beginLevel. */
    bool onDevice = false;
    if (useGpu) {
      std::string why;
      if (zd::gpu::renderBand(rend, why))
        onDevice = true;
      else
        reportGpuFallback(why); // the reason, once per render; the CPU path is the same render
    }

    if (!onDevice) {
      for (std::size_t v = 0; v < order.size(); ++v) {
        if (!rend.beginLevel(order[v]))
          continue; // the plan already filtered these out; beginLevel is the same test
        rend.extractRows(band.y1, band.y2);
        rend.fillLevel(); // the one whole-level stage: a max-filter needs the whole row it dilates, and
                          // it dilates what extractRows just wrote, before the convolution smooths it
        rend.blurRows(band.y1, band.y2);
        rend.compositeRows(band.y1, band.y2);
        rend.finishLevel();
      }
    }

    for (int y = band.y1; y < band.y2; ++y) {
      scratch.setRow(y);
      rend.writeOutput(scratch, y, y + 1);
      emitRow(dst.get(), src.get(), scratch, srcComp, dstComp, srcPremult, dstPremult, band.x1,
            band.x2, y, p, maskReader);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// identity

/* Reported only for the cases where the output *is* the input, whatever the depth says:

     mix == 0                                  the dissolve is off (D-20)
     maskInvert with nothing on the Mask        an unconnected mask is fully on, so inverting it is 0
                                              (D-29), and the dissolve is off the same way
     channels == none                          every channel merges back from the Source

   Every one of those is `out = src + t·(effect − src)` with `t == 0`, or an effect that equals the
   source in every channel the output has, which is exact in any case the render would have produced.

   Deliberately *not* claimed: `maxBlur == 0`, or a scene that is entirely in focus. The renderer's
   own `allSharp` fast path already copies the input verbatim in those cases, and claiming identity
   for them would mean computing the CoC map here to find out — the same work the render does, to
   save the same work. A host is told about the cases that cost nothing to know. */
bool ZDefocusZPlugin::isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip *&identityClip,
                                 double &identityTime)
{
  const Params p = readParams(args.time);

  if (p.preview != zd::ePreviewResult)
    return false; // a diagnostic output is not the input
  if (!srcClip_ || !srcClip_->isConnected())
    return false; // there is nothing to pass through

  identityClip = srcClip_;
  identityTime = args.time; // no retime, no gather: this frame, unchanged

  if (p.mix <= 0.0)
    return true;
  if (p.maskInvert && !(maskClip_ && maskClip_->isConnected()))
    return true;

  bool any = false;
  for (int c = 0; c < 4; ++c)
    any = any || p.sel[c];
  return !any; // `none`: the merge-back takes every channel from the Source
}

////////////////////////////////////////////////////////////////////////////////
// RoI / RoD
//
// The render gathers from up to `gatherPad` px *outside* the window on every side, because the
// pixels outside a level's box are what that level's blur and its fill gather *from* — and the
// number is @ref zd::maxKernelReach over the level set, the same helper
// @ref zd::LayeredRender::prepare uses to pad its own CoC map (D-38). Asking for less is not a
// cheaper render, it is a visibly wrong edge, and asking for more is a slower one, so the two are
// the same function of the same knobs.
//
// The Depth clip is padded exactly as far as the Source: its pixels decide the CoC of the pixels a
// level gathers from, so a depth map truncated at the window edge blurs pixels the picture says are
// in focus. The Mask needs the window and nothing more (a per-pixel weight). The Bokeh clip is the
// odd one: it is sampled through the kernel library, never at an output pixel, so it is asked for
// its whole region of definition — which is also why its clip descriptor refuses tiles.

void ZDefocusZPlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                          OFX::RegionOfInterestSetter &rois)
{
  if (bokehClip_ && bokehClip_->isConnected()) {
    const OfxRectD rod = bokehClip_->getRegionOfDefinition(args.time);
    if (boundedD(rod))
      rois.setRegionOfInterest(*bokehClip_, rod);
  }

  const Params p = readParams(args.time);
  const int pad = gatherPad(p);
  if (pad <= 0)
    return; // a level set that blurs nothing gathers nothing

  OfxRectD roi = args.regionOfInterest;
  roi.x1 -= (double)pad;
  roi.y1 -= (double)pad;
  roi.x2 += (double)pad;
  roi.y2 += (double)pad;

  if (srcClip_ && srcClip_->isConnected())
    rois.setRegionOfInterest(*srcClip_, roi);
  if (depthClip_ && depthClip_->isConnected())
    rois.setRegionOfInterest(*depthClip_, roi);
}

/* The defocus spreads content outward by the same reach, so the RoD is the Source's, widened by it.
 * Not widened for the previews, which never write a gathered pixel — but a node whose RoD depends on
 * a knob the user is dragging is worse than a node with a slightly generous RoD, so the same number
 * is answered in every mode. An unbounded Source RoD means the host has no idea either, and the
 * honest answer to a host that asks is "I do not know" (false), which is the Support library's own
 * default. */
bool ZDefocusZPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  if (!srcClip_ || !srcClip_->isConnected())
    return false;

  const OfxRectD srcRod = srcClip_->getRegionOfDefinition(args.time);
  if (!boundedD(srcRod))
    return false;

  const Params p = readParams(args.time);
  const int pad = gatherPad(p);
  rod = srcRod;
  rod.x1 -= (double)pad;
  rod.y1 -= (double)pad;
  rod.x2 += (double)pad;
  rod.y2 += (double)pad;
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// caches

/* The kernels and the level set they were built for are caches in the sense that matters: they are
 * the expensive part, they are pure functions of the knob set, and they are rebuilt when the knob
 * set changes. The bokeh *picture* is not dropped here, because a purge does not mean the input
 * changed; @ref changedClip is what says that. */
void ZDefocusZPlugin::purgeCaches()
{
  if (_sprites)
    _sprites->clear();
  _spriteLevels = zd::LevelSet();
  _spriteCap = 0.0;
}

void ZDefocusZPlugin::changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName)
{
  (void)args;
  if (clipName != "Bokeh")
    return; // Source/Depth/Mask are read afresh by every render and hold nothing this node caches

  if (_sprites)
    _sprites->clear(); // every kernel this picture built (bokeh.h: a cached kernel from another
  _spriteImage = zd::PlaneImage();
  _haveSpriteImage = false; // picture is the wrong answer to the same radius)
}

////////////////////////////////////////////////////////////////////////////////
// factory

mDeclarePluginFactory(ZDefocusZPluginFactory, {}, {});

using namespace OFX;

/** @brief Put a knob inside a group, if this host has groups at all (TimeWarp's @c reparent, and
 * the same reasoning: `kOfxParamPropParent` is a property a host may not implement, the Support
 * library throws when a host refuses one, and a describe action that throws is a plugin that does
 * not load in that host. A knob whose group was refused still works at the root of the hierarchy.) */
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

void ZDefocusZPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  desc.setLabels("ZDefocusZ", "ZDefocusZ",
                 "Depth-driven defocus: reads a depth/Z channel, turns it into a circle-of-confusion "
                 "map, splits that map into up to 256 discrete blur sizes and composites the levels "
                 "back to front through a per-level bokeh kernel. Takes its depth from the Depth input "
                 "or, when nothing is connected there, from the Source itself, and can use a Bokeh "
                 "input as the aperture instead of the analytic one.");
  desc.setPluginGrouping("OFX Lab");

  // General carries the Depth, Bokeh and Mask inputs; Filter is kept for hosts that only chain
  // effects in filter context, where a mask is not allowed (the SharpenZ/TimeWarp precedent, D-20).
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedContext(eContextFilter);

  // v1 scope: float32 only (DECISIONS.md D-07)
  desc.addSupportedBitDepth(eBitDepthFloat);

  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(true); // the render is rect-driven and every stage is row-ranged
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false); // the lens mode measures the Source clip's pixels
}

/** @brief The knob table.
 *
 * Ranges, defaults and help come from the core headers' own constants (zd.h, bokeh.h, layered.h),
 * and the defaults are those headers' own option-struct defaults, so descriptor, model and
 * DESIGN.md §2 are one set of numbers (the D-33 precedent). Nuke's ZDefocus is the semantic
 * reference (references/zdefocus/nuke_zdefocus_knobs.md) — the knob *names* follow it where they
 * mean the same thing, and where this node does something different (the soft layer seams, the four
 * depth modes, the depth channel selector) the help says so. */
void ZDefocusZPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
  // Source: the picture, and — when nothing is connected to Depth — the depth channel too.
  OFX::ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setTemporalClipAccess(false);
  srcClip->setSupportsTiles(true);
  srcClip->setIsMask(false);

  if (context == eContextGeneral) {
    // Depth input. Optional: an unconnected Depth means the depth channel is read out of Source,
    // which is how a single-input Z pass arrives. RGBA and Alpha only, because those are the
    // component sets this node reads and `depthChannel` picks a component out of them.
    OFX::ClipDescriptor *depthClip = desc.defineClip("Depth");
    depthClip->addSupportedComponent(ePixelComponentRGBA);
    depthClip->addSupportedComponent(ePixelComponentAlpha);
    depthClip->setOptional(true);
    depthClip->setTemporalClipAccess(false);
    depthClip->setSupportsTiles(true);
    depthClip->setIsMask(false);

    // Bokeh input: the aperture picture, sampled by the kernel library rather than at an output
    // pixel, so it is never tiled (a cropped bokeh would be a different aperture).
    OFX::ClipDescriptor *bokehClip = desc.defineClip("Bokeh");
    bokehClip->addSupportedComponent(ePixelComponentRGBA);
    bokehClip->addSupportedComponent(ePixelComponentAlpha);
    bokehClip->setOptional(true);
    bokehClip->setTemporalClipAccess(false);
    bokehClip->setSupportsTiles(false);
    bokehClip->setIsMask(false);

    // Mask input: General context only (filter contexts forbid masks). Optional and alpha-only;
    // Natron then adds its standard "enable mask" + mask-channel selector knobs and delivers the
    // chosen channel as alpha (D-20, D-29).
    OFX::ClipDescriptor *maskClip = desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setOptional(true);
    maskClip->setTemporalClipAccess(false);
    maskClip->setSupportsTiles(true);
    maskClip->setIsMask(true);
  }

  // Output, in every supported context: a host wires its node straight from the descriptor, and
  // Natron dereferences getClip("Output") under an assert() only, so an undescribed Output takes the
  // host down the moment the node is created (D-28).
  OFX::ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(ePixelComponentRGBA);
  dstClip->addSupportedComponent(ePixelComponentAlpha);
  dstClip->setSupportsTiles(true);

  // One set of defaults for the descriptor and the model: the option structs *are* the defaults.
  const zd::FocusOptions foc;
  const zd::LayerOptions lay;
  const zd::BokehOptions bok;

  OFX::GroupParamDescriptor *gDepth = 0, *gBlur = 0, *gOptics = 0, *gLayers = 0, *gBokeh = 0,
                            *gBloom = 0, *gOutput = 0, *gPerformance = 0;
  try {
    gDepth = desc.defineGroupParam("depthGroup");
    gDepth->setLabels("Depth", "Depth", "What the depth channel means and where the focal plane is.");
    gDepth->setOpen(true);
    gBlur = desc.defineGroupParam("blurGroup");
    gBlur->setLabels("Blur", "Blur", "How large the blur at infinite depth is, and its clamp.");
    gBlur->setOpen(true);
    gOptics = desc.defineGroupParam("opticsGroup");
    gOptics->setLabels("Optics", "Optics",
                       "Blur > Source: Lens only — the lens that decides the blur size.");
    gOptics->setOpen(false);
    gLayers = desc.defineGroupParam("layerGroup");
    gLayers->setLabels("Layers", "Layers", "The discrete blur sizes the scene is split into.");
    gLayers->setOpen(false);
    gBokeh = desc.defineGroupParam("bokehGroup");
    gBokeh->setLabels("Bokeh", "Bokeh", "The aperture every level is convolved with.");
    gBokeh->setOpen(false);
    gBloom = desc.defineGroupParam("bloomGroup");
    gBloom->setLabels("Bloom", "Bloom", "Boost the highlights before the blur spreads them.");
    gBloom->setOpen(false);
    gOutput = desc.defineGroupParam("outputGroup");
    gOutput->setLabels("Output", "Output", "Channels, diagnostics and the mask/mix dissolve.");
    gOutput->setOpen(true);
    gPerformance = desc.defineGroupParam("performanceGroup");
    gPerformance->setLabels("Performance", "Performance",
                            "Which device runs the heavy half of a level visit.");
    gPerformance->setOpen(false);
  } catch (const std::exception &) {
    // No groups in this host: every knob is described at the root level instead, where it behaves
    // exactly the same (the nesting is decoration, the knobs are the interface).
    gDepth = gBlur = gOptics = gLayers = gBokeh = gBloom = gOutput = gPerformance = 0;
  }

  // ---------------------------------------------------------------- Depth ----
  OFX::ChoiceParamDescriptor *depthMode = desc.defineChoiceParam("depthMode");
  depthMode->setLabels("Depth Mode", "Depth Mode",
                       "What the numbers in the depth channel mean. Distance: the value is the distance "
                       "from the camera to the surface, in the same unit focusValue uses (Blender's Z "
                       "pass, Nuke's ZDown/WorldZ). Inverse distance: the value is 1/distance, which is "
                       "what many renderers write and what makes a Z-of-infinity background read as a "
                       "finite number (Blender's ZBack-style passes, Nuke's ZBack). Normalized: the value "
                       "is 0..1 between depthNear and depthFar, which is what a hand-painted or a "
                       "normalised depth map is, and the only mode where depthNear/depthFar matter. Blur: "
                       "there is no distance at all — the channel *is* a blur amount, cocScale px per unit "
                       "above or below focusValue, which is how a Defocus-style mask or an art-directed "
                       "blur map arrives. A value of 0, negative or NaN is no geometry, and is read as "
                       "infinitely far away.");
  depthMode->appendOption("Distance");
  depthMode->appendOption("Inverse distance");
  depthMode->appendOption("Normalized");
  depthMode->appendOption("Blur");
  depthMode->setDefault(foc.depthMode);
  reparent(gDepth, depthMode);

  OFX::BooleanParamDescriptor *depthInvert = desc.defineBooleanParam("depthInvert");
  depthInvert->setLabels("Invert Depth", "Invert Depth",
                         "Negate the channel before the model reads it. Blender looks down -Z, so a "
                         "surface 10 m in front of the camera arrives as -10; check this and it becomes "
                         "+10. It also reverses which end of the ladder is nearest, which is what the "
                         "negation means for the composite order.");
  depthInvert->setDefault(foc.depthInvert);
  reparent(gDepth, depthInvert);

  OFX::StringParamDescriptor *depthChannel = desc.defineStringParam("depthChannel");
  depthChannel->setLabels("Depth Channel", "Depth Channel",
                          "Which component of the clip the model reads. The default Z means \"the depth "
                          "channel of whatever this clip is\": the last component of an RGBA clip, the "
                          "only component of a one-channel clip. The other names the renderers use for "
                          "the same data (ZBack, ZDown, ZFront, ZLinear, ZMath, WorldZ, ViewZ, ObjectZ, "
                          "CameraZ, SceneDepth, Depth, Distance) all mean the same thing here, because "
                          "the host has already picked the layer before the clip reaches this node — and "
                          "an unrecognised name takes that same default rather than silently rendering "
                          "an in-focus image. To read a specific component instead, type r, g, b or a "
                          "(or red/green/blue/alpha): that component is unpacked on its own, which is how "
                          "a depth map parked in the green of a multi-channel pass is read. A selection "
                          "the clip does not have (a on an RGB clip) takes the depth channel's own slot. "
                          "The channel is read raw: a distance is a distance whatever the picture's "
                          "premultiplication is. preview = Depth shows exactly what this knob picked.");
  depthChannel->setStringType(eStringTypeSingleLine);
  depthChannel->setDefault("r");
  reparent(gDepth, depthChannel);

  OFX::DoubleParamDescriptor *focusValue = desc.defineDoubleParam("focusValue");
  focusValue->setLabels("Focus Distance", "Focus Distance",
                        "The distance the image is sharp at, in the depth channel's own units (in Normalized "
                        "mode, in units between depthNear and depthFar; in Blur mode, the channel value whose "
                        "blur is 0, and there it may be 0). Keyframe it for a rack focus. A scene measured in "
                        "metres wants metres here, which is what Blender's unit system is.");
  focusValue->setRange(zd::kDepthMin, zd::kDepthMax);
  focusValue->setDisplayRange(0.0, 100.0);
  focusValue->setDefault(foc.focusValue);
  reparent(gDepth, focusValue);

  OFX::DoubleParamDescriptor *focusRange = desc.defineDoubleParam("focusRange");
  focusRange->setLabels("Focus Range", "Focus Range",
                        "How thick the fully-sharp slice around the focal plane is, in the same units: "
                        "distances within half of it take minBlur instead of whatever the geometry says. 0 "
                        "means one infinitely thin plane, which is what a render pass actually is.");
  focusRange->setRange(0.0, zd::kDepthMax);
  focusRange->setDisplayRange(0.0, 100.0);
  focusRange->setDefault(foc.focusRange);
  reparent(gDepth, focusRange);

  OFX::DoubleParamDescriptor *depthNear = desc.defineDoubleParam("depthNear");
  depthNear->setLabels("Depth Near", "Depth Near",
                       "Normalized mode only: the distance a channel value of 0 stands for.");
  depthNear->setRange(zd::kDepthMin, zd::kDepthMax);
  depthNear->setDisplayRange(0.0, 100.0);
  depthNear->setDefault(foc.depthNear);
  reparent(gDepth, depthNear);

  OFX::DoubleParamDescriptor *depthFar = desc.defineDoubleParam("depthFar");
  depthFar->setLabels("Depth Far", "Depth Far",
                      "Normalized mode only: the distance a channel value of 1 stands for. Together with "
                      "Depth Near it is what turns a 0..1 depth map back into distances, and therefore what "
                      "makes the same map produce the same blur at 2 k and at 20 k.");
  depthFar->setRange(zd::kDepthMin, zd::kDepthMax);
  depthFar->setDisplayRange(0.0, 1000.0);
  depthFar->setDefault(foc.depthFar);
  reparent(gDepth, depthFar);

  OFX::DoubleParamDescriptor *cocScale = desc.defineDoubleParam("cocScale");
  cocScale->setLabels("Blur Scale", "Blur Scale",
                      "Blur mode only: pixels of circle of confusion per unit the channel sits above or "
                      "below Focus Distance. The channel's own sign decides which side of the focal plane "
                      "the pixel is on, which is the only ordering a blur map can imply.");
  cocScale->setRange(zd::kCocScaleMin, zd::kCocScaleMax);
  cocScale->setDisplayRange(0.0, 100.0);
  cocScale->setDefault(foc.cocScale);
  reparent(gDepth, cocScale);

  // ----------------------------------------------------------------- Blur ----
  OFX::ChoiceParamDescriptor *blurFrom = desc.defineChoiceParam("blurFrom");
  blurFrom->setLabels("Blur Source", "Blur Source",
                      "Where the circle of confusion at infinite depth comes from. Size types it directly "
                      "in pixels (Nuke's size), which is the answer for a render pass where the camera is "
                      "not a lens. Lens derives it from aperture, focal length and sensor width as "
                      "f^2 / (N (u_f - f)) in millimetres and converts to pixels with the Source clip's "
                      "width and pixel aspect, so the blur is a fraction of the frame rather than a number "
                      "of pixels and the same setting works at any resolution. A lens focused at or inside "
                      "itself has no finite CoC at infinity and produces no blur at all.");
  blurFrom->appendOption("Size");
  blurFrom->appendOption("Lens");
  blurFrom->setDefault(foc.blurFrom);
  reparent(gBlur, blurFrom);

  OFX::DoubleParamDescriptor *blurSize = desc.defineDoubleParam("blurSize");
  blurSize->setLabels("Blur Size", "Blur Size",
                      "Blur Source: Size only. The circle of confusion, in pixels, a surface at infinite "
                      "depth gets; every other distance takes its share of it (0 at the focal plane, half "
                      "of it at twice the focus distance, and past 1 nearer than the focus plane). Keyframe "
                      "it to open or close an aperture over time.");
  blurSize->setRange(zd::kCoCMin, zd::kCoCMax);
  blurSize->setDisplayRange(0.0, 256.0);
  blurSize->setDefault(foc.blurSize);
  reparent(gBlur, blurSize);

  OFX::DoubleParamDescriptor *maxBlur = desc.defineDoubleParam("maxBlur");
  maxBlur->setLabels("Max Blur", "Max Blur",
                     "The largest circle of confusion any pixel may get, in pixels, whatever the geometry "
                     "says — Nuke's max_size, and the knob that bounds the ladder: the level set is built "
                     "from 0 to this. It is also the size of the largest kernel, so it is the knob the "
                     "per-render memory cap watches: the gather every level needs, the RoI this node asks "
                     "its host for, and the cost of the whole render all grow with it, and a value whose "
                     "render will not fit in 512 MB is reported as a memory error rather than attempted.");
  maxBlur->setRange(zd::kCoCMin, zd::kCoCMax);
  maxBlur->setDisplayRange(0.0, 256.0);
  maxBlur->setDefault(foc.maxBlur);
  reparent(gBlur, maxBlur);

  OFX::DoubleParamDescriptor *minBlur = desc.defineDoubleParam("minBlur");
  minBlur->setLabels("Min Blur", "Min Blur",
                     "The floor: the circle of confusion inside Focus Range, and the smallest kernel the "
                     "ladder has. Above Max Blur it is read as Max Blur, because a clamp has to be ordered. "
                     "A small non-zero value here is what keeps a supposedly sharp plate from being *exactly* "
                     "the input when you wanted everything slightly soft.");
  minBlur->setRange(zd::kCoCMin, zd::kCoCMax);
  minBlur->setDisplayRange(0.0, 64.0);
  minBlur->setDefault(foc.minBlur);
  reparent(gBlur, minBlur);

  // --------------------------------------------------------------- Optics ----
  OFX::DoubleParamDescriptor *aperture = desc.defineDoubleParam("aperture");
  aperture->setLabels("Aperture (f-stop)", "Aperture (f-stop)",
                      "Lens mode only: the f-number N. A smaller number is a wider aperture and a bigger "
                      "blur. It is the only one of the three lens numbers a focus pull actually changes.");
  aperture->setRange(zd::kApertureMin, zd::kApertureMax);
  aperture->setDisplayRange(0.5, 32.0);
  aperture->setDefault(foc.aperture);
  reparent(gOptics, aperture);

  OFX::DoubleParamDescriptor *focalLength = desc.defineDoubleParam("focalLength");
  focalLength->setLabels("Focal Length", "Focal Length",
                         "Lens mode only: focal length in millimetres. The blur grows with f^2, which is why "
                         "a long lens is hard to focus and a wide one is not.");
  focalLength->setRange(zd::kFocalMin, zd::kFocalMax);
  focalLength->setDisplayRange(10.0, 300.0);
  focalLength->setDefault(foc.focalLength);
  reparent(gOptics, focalLength);

  OFX::DoubleParamDescriptor *sensorWidth = desc.defineDoubleParam("sensorWidth");
  sensorWidth->setLabels("Sensor Width", "Sensor Width",
                         "Lens mode only: the width the focal length is quoted against, in millimetres — 36 "
                         "is full frame, 24ish is Super 35, and getting this wrong scales the blur, it does "
                         "not change its shape.");
  sensorWidth->setRange(zd::kSensorMin, zd::kSensorMax);
  sensorWidth->setDisplayRange(1.0, 100.0);
  sensorWidth->setDefault(foc.sensorWidth);
  reparent(gOptics, sensorWidth);

  OFX::DoubleParamDescriptor *depthScale = desc.defineDoubleParam("depthScale");
  depthScale->setLabels("Depth Unit", "Depth Unit",
                        "Lens mode only: millimetres per unit of the depth channel, so the lens maths and the "
                        "depth map agree about how far away the focal plane is. 1000 is the answer when the "
                        "scene is in metres, which Blender's unit system is; 1 is the answer when the pass is "
                        "already in millimetres.");
  depthScale->setRange(zd::kDepthScaleMin, zd::kDepthScaleMax);
  depthScale->setDisplayRange(1.0, 10000.0);
  depthScale->setDefault(foc.depthScale);
  reparent(gOptics, depthScale);

  // -------------------------------------------------------------- Layers ----
  OFX::IntParamDescriptor *layers = desc.defineIntParam("layers");
  layers->setLabels("Layers", "Layers",
                    "How many distinct blur sizes the ladder has between the focal plane and Max Blur "
                    "(Nuke's nLayers, ceiling 256). 0 derives it from Max Blur / Layer Spacing, which is the "
                    "default and the answer for most plates: a level every Layer Spacing pixels of CoC. More "
                    "levels order the scene more finely — a foreground object and the background behind it get "
                    "different kernels instead of the same one — at the cost of one kernel and one composite "
                    "pass each. This is not the number of pixels of blur: it is the number of *steps* of blur.");
  layers->setRange(0, zd::kLayerMax);
  layers->setDisplayRange(0, 64);
  layers->setDefault(lay.layers);
  reparent(gLayers, layers);

  OFX::DoubleParamDescriptor *layerSpacing = desc.defineDoubleParam("layerSpacing");
  layerSpacing->setLabels("Layer Spacing", "Layer Spacing",
                          "Pixels of circle of confusion between two adjacent levels when Layers is 0. 4 is the "
                          "default: a 64 px Max Blur then asks for 16 levels. Ignored when Layers is typed.");
  layerSpacing->setRange(zd::kLayerSpacingMin, zd::kLayerSpacingMax);
  layerSpacing->setDisplayRange(0.5, 32.0);
  layerSpacing->setDefault(lay.layerSpacing);
  reparent(gLayers, layerSpacing);

  OFX::DoubleParamDescriptor *layerCurve = desc.defineDoubleParam("layerCurve");
  layerCurve->setLabels("Layer Curve", "Layer Curve",
                        "How the levels are spread between 0 and Max Blur: level k sits at "
                        "Max Blur x (k/L)^curve. 1 is even steps, above 1 packs the levels near the focal plane "
                        "(more distinct small blurs, which is what a shallow-depth-of-field shot is mostly "
                        "made of), below 1 packs them near Max Blur. The sign convention is the opposite of "
                        "Nuke's layerCurve, because here the curve bends the radius sequence.");
  layerCurve->setRange(zd::kLayerCurveMin, zd::kLayerCurveMax);
  layerCurve->setDisplayRange(0.25, 4.0);
  layerCurve->setDefault(lay.layerCurve);
  reparent(gLayers, layerCurve);

  OFX::DoubleParamDescriptor *dither = desc.defineDoubleParam("dither");
  dither->setLabels("Dither", "Dither",
                    "How much the level assignment is jittered along the level axis, as a percentage of half "
                    "the local step between levels. Layer splits band: a surface whose CoC crosses a level "
                    "boundary changes kernel in one step, and along a smoothly curved object that step is a "
                    "visible line. Two things remove it here — every pixel is shared between the two levels "
                    "that bracket its CoC, and this dithers which two that is, turning what is left of the "
                    "banding into fine noise while leaving the average blur where it was. 100 is the default "
                    "and the full amount; 0 is the banding, and is what preview = Layers shows.");
  dither->setRange(0.0, zd::kDitherMax);
  dither->setDisplayRange(0.0, 100.0);
  dither->setDefault(lay.dither);
  reparent(gLayers, dither);

  OFX::BooleanParamDescriptor *fillForeground = desc.defineBooleanParam("fillForeground");
  fillForeground->setLabels("Fill Foreground", "Fill Foreground",
                            "Nuke's fill_foreground, on by default like there. A defocused layer's own content "
                            "thins out — a thin dark object on a bright background becomes a smear with holes "
                            "in it — so each layer's *coverage* is dilated by its own blur radius and its colour "
                            "scaled by the same ratio, which fills those holes with the aperture that object "
                            "already had. It never lowers a coverage, changes nothing where the coverage was "
                            "already full, and is a no-op on the in-focus level. Off is the honest "
                            "'defocus only what is there' behaviour, and the one worth comparing against when "
                            "a plate starts gaining halos.");
  fillForeground->setDefault(lay.fillForeground);
  reparent(gLayers, fillForeground);

  OFX::DoubleParamDescriptor *spriteRadiusCap = desc.defineDoubleParam("spriteRadiusCap");
  spriteRadiusCap->setLabels("Sprite Radius Cap", "Sprite Radius Cap",
                             "The largest radius a kernel is ever built for, in pixels; 0 is uncapped, which is "
                             "the default and changes nothing. A cost knob and not a look knob: the level set, "
                             "the CoC map and each pixel's assignment stay exactly what the depth says, but a "
                             "level whose CoC is wider than this blurs with a kernel of this radius instead of "
                             "its own, so deep defocus saturates at the cap rather than getting deeper. What "
                             "costs follows the kernel and not the CoC — the gather window each level reads, "
                             "the RoI this node asks its host for, the padded CoC map and the sprite cache all "
                             "grow as a little over twice the widest radius, so a ladder whose widest kernel is "
                             "4096 px wants 9945 px of padding on every side (5850 with fillForeground off) "
                             "where a 512 px cap wants 1241. Levels the cap clips share "
                             "one cached kernel. Lower it when a plate's far field is unbounded and the defocus "
                             "you can see is what you would rather pay for.");
  spriteRadiusCap->setRange(zd::kSpriteRadiusCapMin, zd::kSpriteRadiusCapMax);
  spriteRadiusCap->setDisplayRange(0.0, 1024.0);
  spriteRadiusCap->setDefault(lay.spriteRadiusCap);
  reparent(gLayers, spriteRadiusCap);

  // -------------------------------------------------------------- Bokeh ----
  OFX::ChoiceParamDescriptor *shape = desc.defineChoiceParam("shape");
  shape->setLabels("Bokeh Shape", "Bokeh Shape",
                   "The aperture every level is convolved with. Disc: a circular aperture, and Disc "
                   "Profile dissolves it from a Gaussian (0, no edge at all) to a hard disc (1, the "
                   "geometric circle). Bladed: an iris polygon — Blades sides, Roundness blends it "
                   "toward its circumscribed circle, Rotation turns it, and the inner polygon knobs "
                   "make the obstruction/secondary-spectrum look; Catadioptric punches a central hole "
                   "for a mirror lens' donut bokeh. Image: the Bokeh input *is* the kernel — a "
                   "photograph of a real aperture, or anything painted — resampled from that picture's "
                   "own mip chain, and kept per channel so a coloured aperture defocuses each channel "
                   "as its own shape. Aspect and Rotation apply to all three.");
  shape->appendOption("Disc");
  shape->appendOption("Bladed");
  shape->appendOption("Image");
  shape->setDefault(bok.shape);
  reparent(gBokeh, shape);

  OFX::DoubleParamDescriptor *discShape = desc.defineDoubleParam("discShape");
  discShape->setLabels("Disc Profile", "Disc Profile",
                       "Disc shape only: Nuke's shape, same ends. 0 is a Gaussian, which is the softest "
                       "blur there is and has no visible edge anywhere; 1 is a hard disc, which is what a "
                       "real aperture is and is what makes a point of light read as a disc. Between them "
                       "is how soft an out-of-focus highlight looks.");
  discShape->setRange(zd::kShapeMin, zd::kShapeMax);
  discShape->setDisplayRange(0.0, 1.0);
  discShape->setDefault(bok.discShape);
  reparent(gBokeh, discShape);

  OFX::DoubleParamDescriptor *aspect = desc.defineDoubleParam("aspect");
  aspect->setLabels("Aspect", "Aspect",
                    "Squeezes the aperture along one axis at a time: below 1 squeezes x, above 1 squeezes "
                    "y — Nuke's aspect, and the way to fake the cat's-eye bokeh of vignetting. It moves "
                    "the aperture's extent and never its energy (the area is preserved), so it stretches "
                    "the blur without dimming it, and it is the one bokeh knob that also changes how far "
                    "the render has to gather: the reach grows along the axis it lengthens.");
  aspect->setRange(zd::kAspectMin, zd::kAspectMax);
  aspect->setDisplayRange(0.25, 4.0);
  aspect->setDefault(bok.aspect);
  reparent(gBokeh, aspect);

  OFX::DoubleParamDescriptor *rotation = desc.defineDoubleParam("rotation");
  rotation->setLabels("Rotation", "Rotation",
                      "Turns the aperture, in degrees, counter-clockwise. Every shape, image included — a "
                      "photographed aperture arriving at an angle needs this and nothing else.");
  rotation->setRange(zd::kRotationMin, zd::kRotationMax);
  rotation->setDisplayRange(-180.0, 180.0);
  rotation->setDefault(bok.rotation);
  reparent(gBokeh, rotation);

  OFX::IntParamDescriptor *blades = desc.defineIntParam("blades");
  blades->setLabels("Blades", "Blades",
                    "Bladed shape only: how many sides the iris polygon has. 3 is a triangle, 4 a square, "
                    "6 a hexagon; a real lens is usually 5 to 9 depending on its aperture ring, and a "
                    "prime lens' bokeh shape is basically this number.");
  blades->setRange(zd::kBladesMin, zd::kBladesMax);
  blades->setDisplayRange(3, 16);
  blades->setDefault(bok.blades);
  reparent(gBokeh, blades);

  OFX::DoubleParamDescriptor *roundness = desc.defineDoubleParam("roundness");
  roundness->setLabels("Roundness", "Roundness",
                       "Bladed shape only: how far the polygon is blended toward its circumscribed circle. "
                       "0 is straight sides (the pure polygon, the most visible blades), 1 is a circle. "
                       "The support radius is what is interpolated, so the aperture's *area* runs from the "
                       "polygon's to pi — which is why a rounder blade count is also a slightly brighter "
                       "blur.");
  roundness->setRange(zd::kRoundMin, zd::kRoundMax);
  roundness->setDisplayRange(0.0, 1.0);
  roundness->setDefault(bok.roundness);
  reparent(gBokeh, roundness);

  OFX::DoubleParamDescriptor *innerSize = desc.defineDoubleParam("innerSize");
  innerSize->setLabels("Inner Size", "Inner Size",
                       "Bladed shape only: the radius of a second, inner polygon, as a fraction of the "
                       "outer one. This is the obstruction in a real lens' exit pupil, and it is what the "
                       "inner two knobs have somewhere to live.");
  innerSize->setRange(zd::kInnerSizeMin, zd::kInnerSizeMax);
  innerSize->setDisplayRange(0.0, 1.0);
  innerSize->setDefault(bok.innerSize);
  reparent(gBokeh, innerSize);

  OFX::DoubleParamDescriptor *innerFeather = desc.defineDoubleParam("innerFeather");
  innerFeather->setLabels("Inner Feather", "Inner Feather",
                          "Bladed shape only: how the inner polygon's edge is softened, and in which "
                          "direction — above 0.5 fades outward, below 0.5 fades inward, 0.5 is a hard edge "
                          "with no feather at all. Nuke's inner_feather, same convention.");
  innerFeather->setRange(zd::kInnerFeatherMin, zd::kInnerFeatherMax);
  innerFeather->setDisplayRange(0.0, 1.0);
  innerFeather->setDefault(bok.innerFeather);
  reparent(gBokeh, innerFeather);

  OFX::DoubleParamDescriptor *innerBrightness = desc.defineDoubleParam("innerBrightness");
  innerBrightness->setLabels("Inner Brightness", "Inner Brightness",
                             "Bladed shape only: how bright the inner polygon is against the outer one. 0 "
                             "leaves a dark centre (the look of a lens with a heavy obstruction), 1 is the "
                             "same brightness as the rest of the aperture, above 1 puts light back in the "
                             "middle, which is what a secondary spectrum reads as.");
  innerBrightness->setRange(zd::kInnerBrightnessMin, zd::kInnerBrightnessMax);
  innerBrightness->setDisplayRange(0.0, 2.0);
  innerBrightness->setDefault(bok.innerBrightness);
  reparent(gBokeh, innerBrightness);

  OFX::BooleanParamDescriptor *catadioptric = desc.defineBooleanParam("catadioptric");
  catadioptric->setLabels("Catadioptric", "Catadioptric",
                          "Bladed shape only: punch a hole in the middle of the aperture, which is the "
                          "donut bokeh a mirror lens makes — a bright ring with nothing in it, and the "
                          "reason out-of-focus highlights look like rings rather than discs.");
  catadioptric->setDefault(bok.catadioptric);
  reparent(gBokeh, catadioptric);

  OFX::DoubleParamDescriptor *catadioptricSize = desc.defineDoubleParam("catadioptricSize");
  catadioptricSize->setLabels("Hole Size", "Hole Size",
                              "Catadioptric only: the radius of that hole, as a fraction of the aperture's. "
                              "0.3 is an ordinary mirror lens; near the maximum it becomes a thin ring and "
                              "the blur loses most of its light.");
  catadioptricSize->setRange(zd::kHoleMin, zd::kHoleMax);
  catadioptricSize->setDisplayRange(0.0, 0.95);
  catadioptricSize->setDefault(bok.catadioptricSize);
  reparent(gBokeh, catadioptricSize);

  OFX::ChoiceParamDescriptor *bokehSource = desc.defineChoiceParam("bokehSource");
  bokehSource->setLabels("Bokeh Channels", "Bokeh Channels",
                         "Image shape only: how the Bokeh picture defines the shape. Per Channel builds one "
                         "kernel per channel — a red ring defocuses red light as a red ring and green light "
                         "however its own channel says, which is chromatic aberration and painted bokeh, and "
                         "is why the input may be a colour photograph at all. Alpha takes the picture's "
                         "alpha as the shape and applies that same shape to every channel (Nuke's "
                         "filter_channel without use_input_channels). Mean takes the average of its channels "
                         "the same way, which is the answer for a grey aperture photograph with no alpha.");
  bokehSource->appendOption("Per Channel");
  bokehSource->appendOption("Alpha");
  bokehSource->appendOption("Mean");
  bokehSource->setDefault(bok.source);
  reparent(gBokeh, bokehSource);

  // --------------------------------------------------------------- Bloom ----
  OFX::BooleanParamDescriptor *bloom = desc.defineBooleanParam("bloom");
  bloom->setLabels("Bloom", "Bloom",
                   "Boost the pixels above Bloom Threshold by Bloom Gain *before* the blur spreads them, which "
                   "is what makes a specular highlight bloom instead of merely smearing. Off by default. The "
                   "boost is applied to the picture, not to a level, so a highlight keeps it whatever layer it "
                   "ends up in.");
  bloom->setDefault(lay.bloom);
  reparent(gBloom, bloom);

  OFX::DoubleParamDescriptor *bloomThreshold = desc.defineDoubleParam("bloomThreshold");
  bloomThreshold->setLabels("Bloom Threshold", "Bloom Threshold",
                            "The luma a pixel has to be above to be boosted, in 0..1 in the encoded (display) "
                            "space Nuke's record uses — which is why it stays 0..1 whatever the working space "
                            "is. Bloom Gamma is what decides which space is compared.");
  bloomThreshold->setRange(zd::kBloomThresholdMin, zd::kBloomThresholdMax);
  bloomThreshold->setDisplayRange(0.0, 1.0);
  bloomThreshold->setDefault(lay.bloomThreshold);
  reparent(gBloom, bloomThreshold);

  OFX::DoubleParamDescriptor *bloomGain = desc.defineDoubleParam("bloomGain");
  bloomGain->setLabels("Bloom Gain", "Bloom Gain",
                       "How much a pixel above the threshold is multiplied by. 1 is a no-op whatever the "
                       "threshold is, and it is also what the render detects and skips.");
  bloomGain->setRange(zd::kBloomGainMin, zd::kBloomGainMax);
  bloomGain->setDisplayRange(1.0, 10.0);
  bloomGain->setDefault(lay.bloomGain);
  reparent(gBloom, bloomGain);

  OFX::BooleanParamDescriptor *bloomGamma = desc.defineBooleanParam("bloomGamma");
  bloomGamma->setLabels("Bloom Gamma", "Bloom Gamma",
                        "Run the threshold comparison and the gain through a 2.2-encoded space (Nuke's "
                        "bloom_gamma, 'a 2.2 gamma through the blur'): both the comparison and the boost happen "
                        "on encoded luma, which for a power gain is the same as scaling linear colour by "
                        "gain^(1/2.2). Off compares and boosts linearly.");
  bloomGamma->setDefault(lay.bloomGamma);
  reparent(gBloom, bloomGamma);

  // -------------------------------------------------------------- Output ----
  OFX::ChoiceParamDescriptor *preview = desc.defineChoiceParam("preview");
  preview->setLabels("Preview", "Preview",
                     "Replace the result with a diagnostic, bypassing the mask and the mix (an instrument "
                     "dissolved against its own input is measuring nothing). All five are grey with alpha 1, "
                     "so they read the same in a premultiplied host and an unpremultiplied one. Result is the "
                     "render. Depth is the component the Depth Channel knob picked, raw, in the clip's own "
                     "units — where 0/negative means 'no geometry, read as infinitely far'. CoC is the circle "
                     "of confusion that produced, normalised so 1 is Max Blur. Layers is the level each pixel "
                     "actually landed in (the assignment the renderer performs, not a re-derivation): 0.5 is "
                     "the in-focus level, below it is farther, above it nearer, and the bands between levels "
                     "are what Dither and Layer Curve are doing. Bokeh is the largest kernel the ladder builds, "
                     "upsampled to the cell it gathers at, which is the aperture this knob set has.");
  preview->appendOption("Result");
  preview->appendOption("Depth");
  preview->appendOption("CoC");
  preview->appendOption("Layers");
  preview->appendOption("Bokeh");
  preview->setDefault(zd::ePreviewResult);
  reparent(gOutput, preview);

  OFX::StringParamDescriptor *channels = desc.defineStringParam("channels");
  channels->setLabels("Channels", "Channels",
                      "Channels to defocus: any combination of r, g, b, a (or all / none). The others are "
                      "copied from the input unchanged, which is how an RGBA render keeps its own alpha while "
                      "only its colour is defocused. On a one-channel clip, either a or r selects it: it has "
                      "one channel, and hosts label it either way. 'none' makes the node a pass-through.");
  channels->setStringType(eStringTypeSingleLine);
  channels->setDefault("all");
  reparent(gOutput, channels);

  OFX::DoubleParamDescriptor *mix = desc.defineDoubleParam("mix");
  mix->setLabels("Mix", "Mix", "Dissolve between the input (0) and the defocused result (1).");
  mix->setRange(0, 1);
  mix->setDisplayRange(0, 1);
  mix->setDefault(1.0);
  reparent(gOutput, mix);

  OFX::BooleanParamDescriptor *maskInvert = desc.defineBooleanParam("maskInvert");
  maskInvert->setLabels("Mask Invert", "Mask Invert",
                        "Invert the mask: black areas get the defocus, white areas pass the input through. "
                        "With nothing connected to the Mask input the mask is fully on, so checked with an "
                        "empty Mask this node outputs its input unchanged (D-20, D-29).");
  maskInvert->setDefault(false);
  reparent(gOutput, maskInvert);

  // -------------------------------------------------------- Performance ----
  OFX::BooleanParamDescriptor *useGPU = desc.defineBooleanParam("useGPU");
  useGPU->setLabels("Use GPU", "Use GPU",
                    "Run the two stages that do the arithmetic — gathering the picture into each level's "
                    "cell grid, and convolving that grid with that level's aperture — on an OpenCL GPU "
                    "instead of the CPU. Off is the default, and off is the code that was here before: the "
                    "depth model, the level ladder, the lattices, the kernel taps, the coverage fill, the "
                    "composite's maths, the previews, the mask/mix dissolve and the gather padding this "
                    "node asks its host for are one implementation, and this knob changes only *which "
                    "device executes a stage*. It is a checkbox rather than an automatic choice because a "
                    "device that is present is not a device that helps every render: what it is paid for is "
                    "the convolution, whose share of a pass moves with the CoC (DESIGN.md §7's measured "
                    "ladder), while the window that has to be copied to the card — the padded gather area, "
                    "the CoC map and the side flags — grows with the same CoC. Where a device will not take "
                    "a band (no OpenCL installable client, no GPU it can open, no double precision, this "
                    "band's buffers over the largest single allocation the device hands out, or a level that "
                    "reads scanlines the delivered plane does not hold) that band is rendered on the CPU, "
                    "the reason is reported once per render as a log message and never as an error, and the "
                    "picture is the same either way.");
  useGPU->setDefault(false);
  reparent(gPerformance, useGPU);
}

OFX::ImageEffect *ZDefocusZPluginFactory::createInstance(OfxImageEffectHandle handle,
                                                        OFX::ContextEnum /*context*/)
{
  return new ZDefocusZPlugin(handle);
}

namespace OFX
{
namespace Plugin
{
void getPluginIDs(OFX::PluginFactoryArray &ids)
{
  static ZDefocusZPluginFactory p("net.ofxlab.zdefocusz", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
