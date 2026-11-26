/*
  gridwrap.cpp — GridWrap: a Bézier grid warp, for Natron and any other OFX host.

  What a grid warp is: two control lattices — the *source* grid and the *destination* grid — each
  one a Coons patch of cubic Bézier grid lines, and one warp: the pixel at an output position p is
  read from where the operative grid maps (u,v) to p. `output`/`warp`/`mix` say which of the two
  lattices is the operative one and how far the image has travelled from the source grid towards
  the destination grid; `background` says what fills the area the grid does not reach. All of that
  maths is host-free and unit-tested (tests/gw_grid_test.cpp, gw_resample_test.cpp,
  gw_render_test.cpp) and lives in `grid.{h,cpp}`, `resample.{h,cpp}` and `render.{h,cpp}`. This
  file is the half that talks to a host: the knob table (ARCHITECTURE.md §3.2), the four input
  clips, the gather requests, the lattice rewrite a topology change needs, and the identity report.

  Inputs: `Source` (the picture the source grid was drawn on), `Dest` (the picture the destination
  grid was drawn on — a morph dissolves one into the other), an optional `Background` (what
  `background = on bg` fills an uncovered pixel with), and the standard optional alpha `Mask`
  (D-20, General context only).

  Every grid knob is a *fraction of the grid basis*, never a pixel (D-53): a position is
  0..1 across the basis rect and a handle is a fraction of the basis width (u arms) or height
  (v arms). Defaults, clamps, and the lattice rebuild a `rows`/`cols` change needs are therefore
  the same numbers at 1920x1080 and at 128x128, the descriptor can state them without knowing any
  format, and this plugin never has to rewrite its own knobs because a clip changed size. Pixels
  appear only where `grid.{h,cpp}` resolves the fractions against a basis.

  The lattice travels as ordinary OFX parameters because OFX 1.4 — and Natron's HostSupport — have
  no array parameter: 8x8 points x 5 knobs x 2 grids = 640 `Double2D` knobs, all of them described
  once at `describe` time, and the ones outside the current topology hidden with `setIsSecret` at
  runtime. Two extra secret integers, `grid_topo_rows`/`grid_topo_cols`, record the topology those
  fractions were normalised for; when `rows`/`cols` no longer match them the plugin rewrites the
  lattice in fraction space over the unit square (which is exactly why the knobs are fractions),
  keys included. See DECISIONS.md D-53.

  v1 scope: float32 only (D-07, the SharpenZ/ZDefocusZ precedent) and CPU only (D-04/D-06). The grid is
  edited through its knobs *and* through a Viewer overlay that draws the lattice and drags it
  (`GridWrapOverlay` below, D-78): the hit-test / drag geometry is host-free in `interact.{h,cpp}` and the
  GL calls are reached through `dlopen` in `gl.{h,cpp}`, so this file holds only the OFX half — read the
  knobs, write a knob, ask for a redraw. ARCHITECTURE.md §3.2 states what the overlay reproduces.
*/

#include "ofxsImageEffect.h"
#include "ofxsProcessing.H"   // Support/Plugins/include (ofxsProcessing.H, capital H)

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "grid.h"
#include "render.h"
#include "resample.h"
#include "interact.h"   // the host-free overlay behaviour: hit-test + drag solve (D-78)
#include "gl.h"         // the overlay's GL draw calls, dlopen'd (D-78, precedent D-51)

#include "ofxlab_channels.h"   // the "channels" knob parser (D-06, D-17)
#include "ofxlab_maskmix.h"    // the mask value rules (D-20); GridWrap's dissolve is the core's

// ---------------------------------------------------------------------------
// Rects, and the two ways a rect leaves this file
// ---------------------------------------------------------------------------
// Everything here works in canonical OFX image coordinates — the format's image plane, rooted at its
// 0,0, x right, y increasing *up* the picture (grid.h's `Rect` comment cites the two header sentences
// that say so) — which is the same space the core's windows, footprints and sample positions use
// (D-60). A rect that goes *out* is either a request — floored/ceiled to whole
// pixels, because that is what a host has to allocate — or a report, which is the same box.

namespace {

inline double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

// `skew_x` / `skew_y` are degrees, and grid.h's `affineFor` reads them through `tan()`: at 90 degrees
// the affine is infinite and every control point becomes NaN. The descriptor's hard range stops a
// slider reaching that, and this stops a script (D-63). The same number the descriptor states.
const double kMaxSkewDeg = 85.0;

inline double clampAngleDeg(double v)
{
  if (!std::isfinite(v)) return 0.0;
  if (v > kMaxSkewDeg) return kMaxSkewDeg;
  if (v < -kMaxSkewDeg) return -kMaxSkewDeg;
  return v;
}

// How far out a rect a host may be asked to allocate. A knob a script wrote as 1e12, or a Transform
// scale of 1e9, is a finite number and a legitimate grid, but a request rect at that coordinate is
// one a host cannot allocate and a `long` conversion cannot represent. Clamping the *rect* (never a
// knob, never a grid) is what keeps that a black edge instead of undefined behaviour.
const double kCoordLimit = 1.0e9;

inline double clampCoord(double v)
{
  if (!std::isfinite(v)) return 0.0;
  if (v > kCoordLimit) return kCoordLimit;
  if (v < -kCoordLimit) return -kCoordLimit;
  return v;
}

inline bool boundedD(const OfxRectD& r)
{
  return std::isfinite(r.x1) && std::isfinite(r.y1) && std::isfinite(r.x2) && std::isfinite(r.y2) &&
         r.x2 > r.x1 && r.y2 > r.y1;
}

inline gw::Rect rectFrom(const OfxRectD& r)
{
  return gw::Rect(clampCoord(r.x1), clampCoord(r.y1), clampCoord(r.x2), clampCoord(r.y2));
}
inline gw::Rect rectFrom(const OfxRectI& r)
{
  return gw::Rect(double(r.x1), double(r.y1), double(r.x2), double(r.y2));
}

// The whole-pixel box a host can be asked for. Outward, because a request that misses one tap is a
// visibly wrong edge, and the core's own requests already carry the pad they need (D-61).
inline OfxRectD rectRequest(const gw::Rect& r)
{
  OfxRectD o;
  o.x1 = std::floor(clampCoord(r.x0));
  o.y1 = std::floor(clampCoord(r.y0));
  o.x2 = std::ceil(clampCoord(r.x1));
  o.y2 = std::ceil(clampCoord(r.y1));
  if (o.x2 < o.x1) o.x2 = o.x1;
  if (o.y2 < o.y1) o.y2 = o.y1;
  return o;
}

// ---------------------------------------------------------------------------
// A clip's region of definition, told apart from "the host has no answer"
// ---------------------------------------------------------------------------
/* Three answers, because a plugin has to tell them apart. Nothing connected is not the same as a
 * connected clip whose extent the host will not say, and only the second one means "this node cannot
 * bound its own output". The distinction is what the RoD action turns on (see
 * GridWrapPlugin::getRegionOfDefinition), and the throw is why it is wrapped at all: the Support
 * library's `Clip::getRegionOfDefinition` rethrows the suite status when a host refuses the call
 * (ofxsImageEffect.cpp), and a plugin may not let an exception escape an OFX entry point (AGENTS.md
 * rule 1). A refusal is information, not a fault. */
enum ClipRod { kClipRodNone = 0, kClipRodUnknown, kClipRodRect };

/* Only ever called on *input* clips, and that is a rule, not a simplification: asking a node's own
 * Output clip for its region of definition asks the node to compute its own RoD (see
 * GridWrapPlugin::hostFrame, D-68). `kOfxImageClipPropConnected` is what makes that safe here — a host
 * answers it from the clip's upstream node, so `!isConnected()` means genuinely nothing attached. */
ClipRod clipRod(OFX::Clip* clip, double t, gw::Rect& out)
{
  out = gw::Rect();
  if (!clip) return kClipRodNone;
  if (!clip->isConnected()) return kClipRodNone;

  try {
    const OfxRectD rod = clip->getRegionOfDefinition(t);
    if (!boundedD(rod)) return kClipRodUnknown;   // unbounded, or a host that answers {0,0,0,0}
    out = gw::Rect::normalised(rectFrom(rod));
    return out.empty() ? kClipRodUnknown : kClipRodRect;
  } catch (const std::exception&) {
    return kClipRodUnknown;
  }
}

// ---------------------------------------------------------------------------
// Grid knob names and the handle table
// ---------------------------------------------------------------------------
// `src_r2c3_up`, `dst_r2c3_left`: no dots, because a host turns a parameter name into a script
// token and a dotted name is a *member* reference there. The suffixes name the four arms of a
// point by the direction they leave in, which is grid.h's pairing (a line (r,c)->(r,c+1) uses
// `c_right` and the next point's `c_left`), so a knob's meaning is readable off its own name.

const char* const kWhichNames[2] = { "src", "dst" };

std::string gridKnobName(const char* which, int r, int c, const char* suffix)
{
  std::string n(which);
  n += "_r";
  n += std::to_string(r);
  n += "c";
  n += std::to_string(c);
  if (suffix) {
    n += '_';
    n += suffix;
  }
  return n;
}

struct HandleSpec {
  int which;           // grid.h's handle index (kHVNeg = the -v arm, kHUPos = the +u arm)
  const char* suffix;  // the knob-name suffix
  const char* label;   // what a host shows
  const char* axis;    // which basis extent its fractions are stated against, for the hint
};

// The table order *is* the handle order, so a hint cannot be attached to the wrong arm: row h
// carries `which == h`, which is how the knobs are described and how readFractions()/fracPair()
// index a FracPoint. The two vertical names follow the arm's direction in canonical pixels, which
// is how the Viewer sees it (grid.h): `up` is the +v arm, and its neutral default sits *above* the
// point. Flipping these two names is what D-65 measured them for.
const HandleSpec kHandleSpecs[gw::kHandleCount] = {
  { gw::kHVNeg, "down",  "Down",  "height" },
  { gw::kHVPos, "up",    "Up",    "height" },
  { gw::kHUNeg, "left",  "Left",  "width" },
  { gw::kHUPos, "right", "Right", "width" }
};

// ---------------------------------------------------------------------------
// The descriptor defaults: the neutral lattice (D-53)
// ---------------------------------------------------------------------------
// A slot the *default* topology (5x5) reaches holds the 5x5 neutral lattice, and a slot only a
// grown topology can reach holds the 8x8 one. Both are neutral, so a knob the user has never
// touched is never a degenerate control point; which of the two is the default does not matter
// either way, because growing `rows`/`cols` rewrites every slot in-topology from the old lattice
// (the rebuild preserves the map), and the rewrite never reads a slot the old topology did not
// have.
//
// Positions i/(cols-1), j/(rows-1), handles ±1/(3(rows-1)) of the basis height for the up/down arms
// and ±1/(3(cols-1)) of its width for the left/right ones — grid.h's `neutralFractions`, which is
// also what real GridWarp dumps hold (references/gridwrap/nuke_gridwrap_knobs.md).

const std::vector<gw::FracPoint>& neutralFractionsDefault()
{
  static const std::vector<gw::FracPoint> f = gw::neutralFractions(gw::kDefaultPoints,
                                                                   gw::kDefaultPoints);
  return f;
}

const std::vector<gw::FracPoint>& neutralFractionsMax()
{
  static const std::vector<gw::FracPoint> f = gw::neutralFractions(gw::kMaxPoints, gw::kMaxPoints);
  return f;
}

gw::FracPoint knobDefaultFractions(int r, int c)
{
  if (r < gw::kDefaultPoints && c < gw::kDefaultPoints)
    return neutralFractionsDefault()[std::size_t(r) * std::size_t(gw::kDefaultPoints) + std::size_t(c)];
  return neutralFractionsMax()[std::size_t(r) * std::size_t(gw::kMaxPoints) + std::size_t(c)];
}

// ---------------------------------------------------------------------------
// A fetched image, seen as the core's window
// ---------------------------------------------------------------------------
/* The core gathers through `gw::SourceWindow`: interleaved floats, addressed in the image's own
 * coordinates, reading outside the window giving the edge pixel it ran into. An OFX image is the
 * same thing with two things attached that the core does not know about — a component set that may
 * not be the one the *render* has, and a row stride in *bytes* that a host is free to pad, or even
 * to make negative (kOfxImagePropRowBytes: "row bytes can be negative, which allows hosts with a
 * native top down row order to pass image into OFX without having to repack pixels").

 * Which component a render reads: the core takes the first `nchans` channels of every pixel it
 * touches, so an alpha-only render over an RGBA image is the *same window with its base moved to
 * component 3* — the interleaving stays 4 floats per pixel, and channel 0 of each pixel of that
 * window is the alpha of it. That is `pick` below, and it is why the window's channel count is the
 * image's and not the render's.

 * The copy path is for a stride this arithmetic cannot index (zero or negative, or not a whole
 * number of floats): the rows are read one at a time through the host's own addressing and laid
 * down in *increasing canonical y*, tight. `Image::getPixelAddress` is the only addressing a plugin
 * is allowed, and it is per-row safe for any stride, while a direct window is not. A negative
 * `rowBytes` is exactly what a native top-down host hands for OFX's y-up space (ofxImageEffect.h:885),
 * and this path is where that host's row order stops mattering: both paths are anchored by
 * coordinate, and neither ever says "buffer row 0 is the top of the picture" (D-64). */
class InputWindow {
public:
  InputWindow() : _win() {}

  // Adopts `img` as a window over its whole bounds. Null/empty image, an empty bounds or a depth
  // this plugin does not render leaves the window empty, which the core reads as zero — the same
  // thing an unconnected input means (render.h's RenderInputs). A `scratch` it fills lives as long
  // as this object does, because it is the caller's.
  void bind(const OFX::Image* img, int nchans, std::vector<float>& scratch)
  {
    _win = gw::SourceWindow();
    if (!img) return;

    const OfxRectI b = img->getBounds();
    const int w = b.x2 - b.x1;
    const int h = b.y2 - b.y1;
    if (w <= 0 || h <= 0) return;
    if (img->getPixelDepth() != OFX::eBitDepthFloat) return;   // v1 float32 only (D-07)

    const int ncomp = img->getPixelComponentCount();
    if (ncomp <= 0) return;

    const int pick = (nchans == 1 && ncomp == 4) ? 3 : 0;
    const int rowBytes = img->getRowBytes();
    const int fp = int(sizeof(float));

    // The direct path needs a stride the window arithmetic can index: positive, a whole number of
    // floats, and at least one row's worth of them (resample.h's SourceWindow states
    // `rowStride >= width * channels` as its invariant, and a gather reads `pixel(row(y), x)` for x
    // up to x0 + w - 1). A padded-but-shorter stride would read the next row's pixels as this row's.
    const int rowStride = rowBytes / fp;
    if (rowBytes > 0 && rowBytes % fp == 0 && rowStride >= w * ncomp) {
      const float* base = static_cast<const float*>(img->getPixelAddress(b.x1, b.y1));
      if (!base) return;
      _win = gw::SourceWindow(base + pick, rowStride, w, h, b.x1, b.y1, ncomp);
      return;
    }

    scratch.assign(std::size_t(w) * std::size_t(h) * std::size_t(ncomp), 0.0f);
    for (int y = 0; y < h; ++y) {
      const float* row = static_cast<const float*>(img->getPixelAddress(b.x1, b.y1 + y));
      if (!row) continue;   // a row the host did not deliver stays zero
      std::copy(row, row + std::size_t(w) * std::size_t(ncomp),
                scratch.begin() + std::size_t(y) * std::size_t(w) * std::size_t(ncomp));
    }
    _win = gw::SourceWindow(scratch.data() + pick, w * ncomp, w, h, b.x1, b.y1, ncomp);
  }

  const gw::SourceWindow& window() const { return _win; }

private:
  InputWindow(const InputWindow&);             // owns nothing, but a window into a caller's buffer
  InputWindow& operator=(const InputWindow&);  // must not be copied out of that buffer's lifetime

  gw::SourceWindow _win;
};

// The Mask clip, read the way every OFX Lab plugin reads one: one component, 0 outside the mask
// image, 1 when nothing is connected (D-20, D-29). The core asks for a mask *value* per output
// pixel, which is the same contract as OfxLab::MaskReader, so this is an adapter and not a second
// rule (render.h's MaskSampler, common/ofxlab_maskmix.h).
class OfxMaskSampler : public gw::MaskSampler {
public:
  OfxMaskSampler() {}

  void set(OFX::Image* img, int comp) { _reader.set(img, comp); }

  virtual double valueAt(int x, int y) const { return _reader.valueAt(x, y); }

private:
  OfxLab::MaskReader _reader;
};

// ---------------------------------------------------------------------------
// One sentence, when a rewrite is refused
// ---------------------------------------------------------------------------
/* @c eMessageLog is the right OFX call and the first thing a plugin tries, and @c OFX::Log::print
 * would be the second — except its `open()` is compiled out unless the Support library was built with
 * @c DEBUG, so in a shipped plugin `print()` writes to a handle that was never opened. Natron shows a
 * plugin's message log nowhere at all (DECISIONS D-51), which is why ZDefocusZ also writes the same
 * sentence to the file the env var @c OFX_PLUGIN_LOGFILE names, and why the offscreen probe reads it
 * from there. The same channel, the same rule: without the env var nothing is opened, opened again, or
 * appended to, and a plugin does not create files next to a host unprompted.
 *
 * What it is for: the grid buttons (D-82), which write exactly one sentence per press, and one case in
 * the topology rewrite — a lattice animated at more keys than this plugin is willing to rewrite (see
 * kMaxRewriteKeys), where the honest answer is to say so, keep the user's keyframes, and let the render
 * resample the grid instead. A silent no-op would leave a grid that looks unchanged with knobs that now
 * mean something else. */

void logGridNote(const char* note)
{
  const char* path = std::getenv("OFX_PLUGIN_LOGFILE");
  if (!path || !*path) return;

  std::FILE* f = std::fopen(path, "a");
  if (!f) return;
  std::fprintf(f, "GridWrap: %s\n", note);
  std::fclose(f);
}

// ---------------------------------------------------------------------------
// The measurement stamp (D-71)
// ---------------------------------------------------------------------------
/* What this plugin *asked a host for* is not the same fact as what the host *delivered*, and only
 * the second can be graded against a rendered frame. The probe env var turns on a block of lines
 * that says, once per entry point that touches the pixels: the render window, what the RoI call was
 * answered with, the region of definition that was computed and from what, whether IsIdentity was
 * asked and what it answered, and for every fetched image its bounds, interleaved component count,
 * pixel depth and a hash of each component plane.
 *
 * The gate is `OFXLAB_GW_PROBE` plus the same log channel as `logGridNote`, so a normal session —
 * a DCC, a batch render, a user — writes nothing: no file is opened, no line is added. With the var
 * set and no `OFX_PLUGIN_LOGFILE`, the fixed fallback path is used. The block is built in memory and
 * leaves as one `fwrite`, so two renders of a host's thread pool cannot interleave a half-line, and
 * every block is framed by a `nonce=` it shares with its `end` line, which is how the analyser pairs
 * one render's evidence with that render's frame. A block is bounded in size and says so rather than
 * growing without end, and every stamp call is wrapped so that an instrument can never be the reason
 * an image effect fails.
 *
 * What is deliberately *not* stamped: this node's own Output clip's region of definition — the Support
 * library answers that by calling back into `getRegionOfDefinition` (DECISIONS D-68), so it is a
 * question, never a fact. The *input* clips' regions of definition are stamped, but only the ones the
 * plugin's own RoD pass already obtained: the stamp never issues a clip RoD call of its own, so a
 * measurement cannot add an entry-point round trip to a render that was not doing one already. */

// The gate: a log path when the probe var is set, null when it is not.
const char* probeGate()
{
  const char* on = std::getenv("OFXLAB_GW_PROBE");
  if (!on || !*on) return 0;
  const char* path = std::getenv("OFX_PLUGIN_LOGFILE");
  if (path && *path) return path;
  return "/tmp/ofxlab_gw_probe.txt";
}

unsigned nextProbeNonce()
{
  static std::atomic<unsigned> counter(0);
  return ++counter;
}

// FNV-1a (32-bit offset basis, 64-bit variant), one float32 as its four bytes least significant
// first. `tests/gw_core_probe.cpp`'s `hashFloats` and the offscreen probe's GridWrap analyser use the
// same seed, prime and byte order, so the same picture in the same space hashes the same number in
// all three (that analyser was never committed and is gone - D-77 - which is why the plugin's own copy
// of the rule is written here). A hash is only ever compared with a hash taken in the *same* space: the
// plugin hashes host-space rows in increasing canonical y, and a reader has to mirror host space to
// canonical space before hashing it (DECISIONS D-64/D-70).
const unsigned long long kProbeFnvSeed = 1469598582492834036ULL;
const unsigned long long kProbeFnvPrime = 1099511628211ULL;

inline void probeHashFloat(unsigned long long& h, float v)
{
  std::uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof bits);
  for (int b = 0; b < 4; ++b) {
    h ^= static_cast<unsigned long long>((bits >> (b * 8)) & 0xFFu);
    h *= kProbeFnvPrime;
  }
}

/* The hash of one component plane of a fetched image over `win` clipped to the image's own bounds:
 * rows in increasing canonical y (the image rect's minimum y row first), columns in increasing x,
 * `comp` of each pixel. A null image, an empty rect or a component the image does not have all
 * return the seed, which is what "there was nothing to hash" means. */
unsigned long long probeHashPlane(const OFX::Image* img, const gw::Rect& win, int comp)
{
  unsigned long long h = kProbeFnvSeed;
  if (!img || comp < 0) return h;

  const OfxRectI b = img->getBounds();
  const int ncomp = img->getPixelComponentCount();
  if (b.x2 <= b.x1 || b.y2 <= b.y1 || ncomp <= 0 || comp >= ncomp) return h;
  if (img->getPixelDepth() != OFX::eBitDepthFloat) return h;       // only float32 planes are hashed

  int x0 = std::max(b.x1, static_cast<int>(std::floor(win.x0)));
  int x1 = std::min(b.x2, static_cast<int>(std::ceil(win.x1)));
  int y0 = std::max(b.y1, static_cast<int>(std::floor(win.y0)));
  int y1 = std::min(b.y2, static_cast<int>(std::ceil(win.y1)));
  if (x1 <= x0 || y1 <= y0) return h;

  for (int y = y0; y < y1; ++y) {
    const float* row = static_cast<const float*>(img->getPixelAddress(b.x1, y));
    if (!row) continue;
    for (int x = x0; x < x1; ++x)
      probeHashFloat(h, row[(static_cast<std::size_t>(x - b.x1)) * static_cast<std::size_t>(ncomp) +
                            static_cast<std::size_t>(comp)]);
  }
  return h;
}

/* The digest of the *knob-side* lattice, which is the only side both the plugin and the predictor
 * can read in the same space: the resolved grid positions live in a host's frame (or the Source
 * clip's, or the boundary rect's), and the predictor is handed canonical knobs. Seeds on the
 * stamped rows/cols and then walks the points in lattice order — position, then the four handles in
 * handle order (kHVNeg, kHVPos, kHUNeg, kHUPos — the row order of the local `kHandleSpecs` table)
 * — each value rounded to float32 first, because these are knob values
 * round-tripped through a host's double. Two lattices that differ by a knob edit, or that differ in
 * which stamped topology they were read under, get different digests. */
unsigned long long probeDigestFractions(int rows, int cols, const std::vector<gw::FracPoint>& pts)
{
  unsigned long long h = kProbeFnvSeed;
  probeHashFloat(h, static_cast<float>(rows));
  probeHashFloat(h, static_cast<float>(cols));
  for (std::size_t i = 0; i < pts.size(); ++i) {
    probeHashFloat(h, static_cast<float>(pts[i].x));
    probeHashFloat(h, static_cast<float>(pts[i].y));
    for (int k = 0; k < gw::kHandleCount; ++k) {
      probeHashFloat(h, static_cast<float>(pts[i].h[k][0]));
      probeHashFloat(h, static_cast<float>(pts[i].h[k][1]));
    }
  }
  return h;
}

// A stamp block: one header, up to kProbeBlockCap bytes of payload, one `end` with the same nonce.
class ProbeBlock
{
 public:
  static const std::size_t kLineCap = 768;
  static const std::size_t kBlockCap = 8192;

  ProbeBlock(const char* path, const char* kind)
      : _path(path), _nonce(nextProbeNonce()), _truncated(false)
  {
    add("GWPROBE %s nonce=%u", kind, _nonce);
  }

  unsigned nonce() const { return _nonce; }

  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    if (_truncated) return;
    char line[kLineCap];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    // The cap is charged for what the line *is*, not for the widest line it could have been: a
    // render block is a couple of kilobytes of short lines, and charging 768 bytes for each of them
    // would report a block as truncated when it is not (D-71).
    if (n < 0 || _text.size() + static_cast<std::size_t>(n) + 1 > kBlockCap) { _truncated = true; return; }
    _text.append(line, static_cast<std::size_t>(n));
    _text.push_back('\n');
  }

  // One write, after the whole block exists. Called once, from the entry point that built it.
  void emit()
  {
    std::string text = _text;
    if (_truncated) text += "GWPROBE truncated nonce=" + std::to_string(_nonce) + "\n";
    text += "GWPROBE end nonce=" + std::to_string(_nonce) + "\n";

    std::FILE* f = std::fopen(_path.c_str(), "a");
    if (!f) return;
    std::setvbuf(f, 0, _IOFBF, text.size() + 64);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fflush(f);
    std::fclose(f);
  }

 private:
  std::string _path;
  unsigned _nonce;
  bool _truncated;
  std::string _text;
};

// What a component set is called in the stamp.
inline const char* probeComps(OFX::PixelComponentEnum comp)
{
  switch (comp) {
    case OFX::ePixelComponentRGBA:  return "rgba";
    case OFX::ePixelComponentRGB:   return "rgb";
    case OFX::ePixelComponentAlpha: return "alpha";
    default:                        return "none";
  }
}

// A rect as the four numbers the analyser reads. Doubles print `%.9g` — the same width
// `tests/gw_core_probe.cpp`'s `numText`/`rectText` use — so a rect the plugin computed and the rect the
// predictor predicted are the same text when they are the same rect. A rect with a non-finite end prints
// the word instead: every rect the core hands out here came through `clampCoord`, so this is the guard
// that keeps `kOfxInfinityDouble` from arriving as `inf` in a line the analyser has to parse (D-71).
inline std::string stampRect(const gw::Rect& r)
{
  if (!std::isfinite(r.x0) || !std::isfinite(r.y0) || !std::isfinite(r.x1) || !std::isfinite(r.y1))
    return "unbounded";
  char b[128];
  std::snprintf(b, sizeof b, "%.9g %.9g %.9g %.9g", r.x0, r.y0, r.x1, r.y1);
  return std::string(b);
}

// A 64-bit hash as the sixteen hex digits the analyser reads, most significant group first.
inline std::string hexText(unsigned long long h)
{
  char b[24];
  std::snprintf(b, sizeof b, "%04x%04x%04x%04x",
                unsigned((h >> 48) & 0xffffULL), unsigned((h >> 32) & 0xffffULL),
                unsigned((h >> 16) & 0xffffULL), unsigned(h & 0xffffULL));
  return std::string(b);
}

// One component plane of one image, hashed over the part of `win` the image actually holds.
inline void stampHash(ProbeBlock& b, const char* label, int comp, const OFX::Image* img,
                      const gw::Rect& win)
{
  b.add("hash %s %d %s", label, comp, hexText(probeHashPlane(img, win, comp)).c_str());
}

// A digest, with the two numbers it is keyed on in front of it: the rows/cols of a lattice, or nothing
// else a hash line needs.
inline void stampDigest(ProbeBlock& b, const char* label, int a, int c, unsigned long long h)
{
  b.add("%s %d %d %s", label, a, c, hexText(h).c_str());
}

// The rect an image's own bounds describe, in the plugin's doubles. `Image::getBounds()` is an integer
// rect on this host (`OfxRectI`), already half-open, and every hash is taken over a rect that came from
// the image being hashed: `probeHashPlane` clips its window with floor/ceil, so it must never be handed
// an infinite sentinel (D-71).
inline gw::Rect boundsRect(const OFX::Image* img)
{
  if (!img) return gw::Rect();
  return rectFrom(img->getBounds());
}

} // namespace

/** The gate around one stamp: a stamp that throws must never become a render failure, so every hook
 * catches everything and the plugin carries on without the measurement — a host that refuses a property
 * read mid-stamp is a real case, not a bug in the stamp (AGENTS.md rule 1: a plugin exception must never
 * escape an entry point, and a measurement is no reason to introduce one). */
#define OFXLAB_STAMP(call)                \
  do {                                    \
    if (!probeGate()) break;              \
    try {                                 \
      call;                               \
    } catch (const std::exception&) {     \
    } catch (...) {                       \
    }                                     \
  } while (false)

// ---------------------------------------------------------------------------
// The knob panel's shape
// ---------------------------------------------------------------------------
// 64 point slots per grid are described unconditionally, so `rows`/`cols` can be grown without a
// host meeting a parameter it has never heard of. `gw::kMaxPoints` is what makes that number, and
// it is the same constant the core clamps to (D-53). Two constants come out of it and they are not
// interchangeable: `kGridSlots` is how many slots a grid's knob arrays hold, `kGridStride` is the
// row stride used to address one, so slot (r,c) is `r * kGridStride + c` inside `kGridSlots` entries.

const int kGridSlots = gw::kMaxPoints * gw::kMaxPoints;   // 64 slots per grid
const int kGridStride = gw::kMaxPoints;                   // 8: the row stride of that slot layout

// "Already equal", for a knob read at a frame: both the overlay drag (D-80) and the key buttons (D-82)
// ask a lattice knob whether it already holds the value about to be written to it, and skip the write when
// it does — that is what keeps a press that changed nothing from leaving a key and an undo step. The
// comparison is a tolerance, not `==`, because the value came out of a host's curve evaluator.
const double kKeyEps = 1e-12;

// The four buttons one grid gets, duplicated across the two grids (D-82). These **names** are the whole
// plumbing: the descriptor loop defines them, the fetch loop reads them back, and `changedParam` compares
// the name the host reports against them, so a name cannot drift between the panel and the behaviour.
// Panel order is these tables read *across a row*: one grid's four actions on one line, then the other grid's
// four on the line below it (D-86), because "copy the source grid, paste it onto the destination" is the
// sequence the user asked for and it has to read top to bottom. A label is host-facing; a name is not, and the
// `_source` / `_destination` suffix is what makes which grid a button means readable in a log line.
enum { kBtnCopy = 0, kBtnPaste = 1, kBtnSetKeys = 2, kBtnDeleteKeys = 3,
       kBtnPerGrid = 4, kBtnCount = kBtnPerGrid * gw::kFamilyCount };

/* The two descriptor properties that put a row of knobs on one panel line (D-86).  Neither name exists in the
   vendored OFX 1.4 SDK — they come from Nuke's extension header, which Natron ships and compiles against
   (build/natron-openfx/include/nuke/fnPublicOfxExtensions.h:54-79: `kOfxParamPropLayoutHint` with values
   Normal 0 / Divider 1 / **NoNewLine 2** = "have the next parameter start on the same line as this", and
   `kOfxParamPropLayoutPadWidth`, "only pertinent with kOfxParamPropLayoutHint==2").  Natron's HostSupport fork
   declares both, writable, on every parameter descriptor (ofxhParam.cpp:290-291) and its Engine reads them back
   into the knob (see the describe site).  Written here by literal name and value rather than by macro, because
   rule 2 keeps this translation unit inside the frozen SDK, and `propSetInt(…, false)` is what makes a host
   that does not know the property ignore it instead of throwing. */
const char* const kParamPropLayoutHint = "OfxParamPropLayoutHint";
const int kLayoutHintNormal = 0;       // break the line after this knob: starts the *next* grid's row
const int kLayoutHintNoNewLine = 2;    // the next knob continues on this knob's line
const char* const kParamPropLayoutPadWidth = "OfxParamPropLayoutPadWidth";
const int kGridButtonRowGap = 12;      // the gap a host leaves between two buttons sharing a row

const char* const kGridButtonNames[kBtnPerGrid][gw::kFamilyCount] = {
  { "copy_source",        "copy_destination"        },
  { "paste_source",       "paste_destination"       },
  { "set_keys_source",    "set_keys_destination"    },
  { "delete_keys_source", "delete_keys_destination" },
};

const char* const kGridButtonLabels[kBtnPerGrid][gw::kFamilyCount] = {
  { "Copy Source Grid",        "Copy Destination Grid"        },
  { "Paste Source Grid",       "Paste Destination Grid"       },
  { "Set Keys Source Grid",    "Set Keys Destination Grid"    },
  { "Delete Keys Source Grid", "Delete Keys Destination Grid" },
};

// What an action means, written once because it means the same thing about either grid: the label says *which*
// grid a button is about, the help says what pressing it does.
const char* const kGridButtonHelp[kBtnPerGrid] = {
  "Snapshot this grid's stamped lattice into this node's clipboard — one point position and four handles per "
  "slot, every one read at the current frame, `rows` x `cols` included. Writes nothing: it is the read half of "
  "a copy/paste, and `paste` is the write half. Copy the source grid, then paste onto the destination grid, and "
  "the two lattices become identical wherever their stamps match.",
  "Write that clipboard into this grid's stamped lattice at the current frame. Refused when nothing has been "
  "copied yet, or when the clipboard's `rows` x `cols` is not this node's: two lattices of different sizes have "
  "no slot-for-slot meaning, so change `rows`/`cols` or copy again. A knob this grid already animates is "
  "written as a key at this frame and keeps the rest of its animation; a static knob is written outright, so a "
  "paste never animates a static grid by surprise — that is what `set keys` is for.",
  "Add a key at the current frame to every point and handle knob of this grid — keyed with the value each one "
  "already reads there, which is what makes a lattice you just set by hand hold that shape at this frame while "
  "the frames around it stay free to move. A knob with no key here becomes animated.",
  "Delete the key at the current frame from every point and handle knob of this grid that has one. This frame "
  "only: a lattice keyed on other frames keeps those keys, and a knob with no key at this frame is left "
  "exactly as it was.",
};

// Which (action, grid) a reported name is, as an index into these same two tables — action first, so
// `idx / gw::kFamilyCount` is the action and `idx % gw::kFamilyCount` the grid. Returns -1 for "not one
// of ours", which is how `changedParam` tells a button press apart from every other knob it is told about.
// The panel speaks names, so this is the only place a name becomes a button.
inline int gridButtonIndex(const std::string& name)
{
  for (int a = 0; a < kBtnPerGrid; ++a)
    for (int f = 0; f < gw::kFamilyCount; ++f)
      if (name == kGridButtonNames[a][f])
        return a * gw::kFamilyCount + f;
  return -1;
}

/* One grid's knobs: 64 positions and 64 x 4 handles, in the topology-independent slot order
 * `r * kGridStride + c`. A grid at 5x5 uses slots 0..24 (rows 0..4 of that layout), which is why
 * growing `rows` never moves a knob a user has already edited: slot (r,c) means the same control
 * point at 5x5 and at 8x8, and the lattice rewrite re-reads the slots the *old* topology had. */
struct GridKnobs {
  GridKnobs()
    : pos(std::size_t(kGridSlots), 0),
      handles(std::size_t(kGridSlots) * std::size_t(gw::kHandleCount), 0) {}

  // Every read and every write of a lattice knob goes through these two, and they return the slot
  // itself rather than a copy, so the constructor can fill it and nothing else has to repeat the
  // addressing. The asserts are what keep this layout honest: an index past `kGridSlots` is a
  // stride mistake, and `vector::operator[]` out of its range is silent until the host's libc turns
  // it into an abort, which is exactly how this layout first failed inside a real host.
  OFX::Double2DParam*& position(int r, int c)
  {
    assert(r >= 0 && r < gw::kMaxPoints && c >= 0 && c < gw::kMaxPoints);
    return pos[std::size_t(r) * std::size_t(kGridStride) + std::size_t(c)];
  }
  const OFX::Double2DParam* position(int r, int c) const
  {
    assert(r >= 0 && r < gw::kMaxPoints && c >= 0 && c < gw::kMaxPoints);
    return pos[std::size_t(r) * std::size_t(kGridStride) + std::size_t(c)];
  }
  OFX::Double2DParam*& handle(int r, int c, int h)
  {
    assert(r >= 0 && r < gw::kMaxPoints && c >= 0 && c < gw::kMaxPoints &&
           h >= 0 && h < gw::kHandleCount);
    return handles[(std::size_t(r) * std::size_t(kGridStride) + std::size_t(c)) *
                   std::size_t(gw::kHandleCount) + std::size_t(h)];
  }
  const OFX::Double2DParam* handle(int r, int c, int h) const
  {
    assert(r >= 0 && r < gw::kMaxPoints && c >= 0 && c < gw::kMaxPoints &&
           h >= 0 && h < gw::kHandleCount);
    return handles[(std::size_t(r) * std::size_t(kGridStride) + std::size_t(c)) *
                   std::size_t(gw::kHandleCount) + std::size_t(h)];
  }

  // Flat arrays, in the slot order `r * kGridStride + c` (and, for the handles, that slot times
  // handle count). `kGridStride` is the row stride and `kGridSlots = kGridStride * kGridStride` is
  // how many slots the layout holds: the two are different numbers, and every address here uses the
  // stride while every size and every `s < kGridSlots` loop bound uses the count. Named so that no
  // data member shares a name with one of the accessors: a member function hides a data member of
  // the same name, and the accessor bodies would then be calling a vector. Zero-initialised, so a
  // name a host refused to describe is a null pointer here rather than an uninitialised one (every
  // name is described, so it is a guard, not a case).
  std::vector<OFX::Double2DParam*> pos;
  std::vector<OFX::Double2DParam*> handles;
};

// The transform family a grid carries, which is Nuke's GridWarpTracker Transform tab. Two knobs
// hold the "extra matrix": a 2x3 affine, one Double3D per row, because a third row would be a
// projective map and grid.h's `Affine` has nowhere to put it (D-53).
struct TransformKnobs {
  TransformKnobs()
    : translate(0), rotate(0), scale(0), skewX(0), skewY(0), skewOrder(0), center(0),
      useMatrix(0), matrixRow0(0), matrixRow1(0), group(0) {}

  OFX::Double2DParam* translate;
  OFX::DoubleParam* rotate;
  OFX::Double2DParam* scale;
  OFX::DoubleParam* skewX;
  OFX::DoubleParam* skewY;
  OFX::ChoiceParam* skewOrder;
  OFX::Double2DParam* center;
  OFX::BooleanParam* useMatrix;
  OFX::Double3DParam* matrixRow0;
  OFX::Double3DParam* matrixRow1;
  OFX::GroupParam* group;
};

std::string transformKnobName(const char* which, const char* suffix)
{
  std::string n(which);
  n += '_';
  n += suffix;
  return n;
}

// `boundary_box`: which rect the grid's fractions are stated against. Plugin-side, because the
// core resolves fractions against a rect someone hands it (grid.h's `resolveGrid`) and only a host
// knows what rects exist. `union` is the union of every connected gatherable clip's region of
// definition (the format when a clip fills it, which is the common case); `format` asks for the
// largest of those boxes instead, which is the answer when one input is a small inset plate and the
// grid should still be drawn across the whole picture. OFX 1.4 has no format property on a clip, so
// "the format" can only be read as "the biggest RoD on hand".
enum BoundaryMode { kBoundaryUnion = 0, kBoundaryFormat = 1, kBoundaryModeCount };

const char* const kBoundaryModeNames[kBoundaryModeCount] = { "union", "format" };

// ---------------------------------------------------------------------------
// The render pass
// ---------------------------------------------------------------------------
/* One GridWrapProcessor per render() call; the base class then slices its window across the host's
 * threads, and each thread calls multiThreadProcessImages with its own sub-window.
 *
 * What is shared and what is not follows from the core (D-55): the four input windows and the mask
 * reader are read-only, so one instance each is fine across threads, but `gw::WarpRender` carries
 * the inversion lattice, and that lattice's `find` reuses candidate scratch (grid.h's
 * `mutable std::vector<Cand> _cands`) — so every thread builds its own, for its own window. The
 * grids and the knob state were read once, on the main thread, before this object was constructed.
 *
 * The rows are written through `Image::getPixelAddress`, which is bounds-checked and per-row safe
 * for any row stride the host chose, including a negative one (kOfxImagePropRowBytes). renderRow
 * writes exactly `nchans` interleaved floats per output pixel, which is the layout of such a row. */
class GridWrapProcessor : public OFX::ImageProcessor {
public:
  GridWrapProcessor(OFX::ImageEffect& effect,
                    const gw::Grid& srcGrid,
                    const gw::Grid& dstGrid,
                    const gw::Options& opt,
                    const gw::RenderInputs& inputs,
                    const OfxMaskSampler& mask)
    : OFX::ImageProcessor(effect),
      _srcGrid(srcGrid), _dstGrid(dstGrid), _opt(opt), _inputs(inputs), _mask(mask) {}

  virtual void multiThreadProcessImages(OfxRectI window)
  {
    if (window.x1 >= window.x2 || window.y1 >= window.y2 || !_dstImg) return;

    const int nchans = _dstImg->getPixelComponentCount();
    if (nchans <= 0) return;

    // Per thread, per sub-window: the plan, the inversion lattice and the row scratch all belong to
    // the render being run, and the window is what selects the cells.
    gw::WarpRender wr(_srcGrid, _dstGrid, _opt);

    for (int y = window.y1; y < window.y2; ++y) {
      if (_effect.abort())
        break;   // a host that wants this render stopped gets it stopped, mid-row

      float* row = static_cast<float*>(_dstImg->getPixelAddress(window.x1, y));
      if (!row)
        continue;   // a row the host did not allocate for us (it clipped the window to the image)

      wr.renderRow(window.x1, window.x2, y, _inputs, _mask, row, nchans);
    }
  }

private:
  GridWrapProcessor(const GridWrapProcessor&);
  GridWrapProcessor& operator=(const GridWrapProcessor&);

  const gw::Grid& _srcGrid;
  const gw::Grid& _dstGrid;
  const gw::Options _opt;
  const gw::RenderInputs& _inputs;
  const OfxMaskSampler& _mask;
};

// ---------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------
class GridWrapPlugin : public OFX::ImageEffect {
public:
  explicit GridWrapPlugin(OfxImageEffectHandle handle);
  virtual ~GridWrapPlugin() {}

  virtual void render(const OFX::RenderArguments& args);
  virtual bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                          double& identityTime);
  virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments& args, OfxRectD& rod);
  virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args,
                                    OFX::RegionOfInterestSetter& rois);
  virtual void changedParam(const OFX::InstanceChangedArgs& args, const std::string& paramName);
  virtual void syncPrivateData();

private:
  // ---- clips
  OFX::Clip* _srcClip;
  OFX::Clip* _dstClip;
  OFX::Clip* _bgClip;
  OFX::Clip* _maskClip;
  OFX::Clip* _outClip;

  // ---- knobs that are not grid points
  OFX::StringParam* _channelsKnob;
  OFX::ChoiceParam* _outputKnob;
  OFX::DoubleParam* _warpKnob;
  OFX::DoubleParam* _mixKnob;
  OFX::ChoiceParam* _backgroundKnob;
  OFX::DoubleParam* _backgroundMixKnob;
  OFX::ChoiceParam* _boundaryKnob;
  OFX::IntParam* _renderResKnob;
  OFX::ChoiceParam* _filterKnob;
  OFX::BooleanParam* _fringeKnob;
  OFX::BooleanParam* _injectKnob;
  OFX::BooleanParam* _maskInvertKnob;
  OFX::BooleanParam* _useSourceTransformKnob;
  OFX::IntParam* _rowsKnob;
  OFX::IntParam* _colsKnob;
  OFX::IntParam* _topoRowsKnob;    // secret: the topology these fractions were normalised for
  OFX::IntParam* _topoColsKnob;
  OFX::BooleanParam* _showSrcGridKnob;   // panel visibility of one lattice (D-81). Each is one more term
  OFX::BooleanParam* _showDstGridKnob;   // in the same runtime secrecy ask D-53 measures. The overlay reads
                                         // the same two values (D-84): a hidden family is still drawn, but
                                         // dimmed, and no point or arm of it is pickable or draggable
  // ---- the grid buttons (D-82), indexed the way the knob families are: `[family][action]`, so a press
  // knows which grid it belongs to without a name lookup. A push button has **no value** — `PushButtonParam`
  // derives straight from `Param` and its descriptor adds nothing (ofxsParam.h:1639 and :668) — so there is
  // nothing here to read a state out of: the press *is* the event, and it arrives as a `changedParam` for
  // the button's own name.
  OFX::PushButtonParam* _buttons[gw::kFamilyCount][kBtnPerGrid];

  // The clipboard one `copy_*` fills and `paste_*` reads, in fraction space at the topology it was copied
  // **at**: one `FracPoint` per slot of that lattice, which is exactly the shape `readFractions` reads and
  // the five `fracPair` components a point's knobs hold. Stamped, because a fraction only means something
  // for the topology it was read for (D-53) — that is what lets a paste compare two lattices instead of
  // writing whatever happens to fit. One clipboard per instance, because the pair the user asked for is a
  // source-grid → destination-grid pair, and a copy is a per-instance thing, not a machine-wide one.
  bool _clipValid;
  int _clipRows;
  int _clipCols;
  std::vector<gw::FracPoint> _clip;

  OFX::Double2DParam* _gridBoundsMin;
  OFX::Double2DParam* _gridBoundsMax;

  GridKnobs _srcKnobs;
  GridKnobs _dstKnobs;
  TransformKnobs _srcT;
  TransformKnobs _dstT;

  // Guards against a host calling changedParam for a setValue *we* made while rewriting the lattice
  // (Natron does exactly that: HostSupport's paramSetValue routes a plugin-side set straight back
  // into kOfxActionInstanceChanged with kOfxChangePluginEdited).
  bool _rewriting;

  // A topology change this plugin was asked for and could not carry out (too many animation keys, or
  // a host that refused a knob write), remembered as the `rows`/`cols` that caused it: the
  // reconciliation does not retry an identical failed rewrite on every frame, while a *new* value is a
  // new attempt. Meanwhile the render's own `resampleGrid` fallback is what keeps the picture correct
  // (D-53).
  bool _topoStuck;
  int _stuckRows, _stuckCols;

  // Sticky: false only for a host that *rejects* the property write outright — see the catch in
  // `syncGridVisibility`. Nothing to do with Natron: measured there (D-53, correction), a runtime
  // `setIsSecret` moves the visibility the panel reports in both directions, so the flag stays true and
  // a grown or shrunk topology reveals or hides its own points.
  mutable bool _secretChangesOk;

  // Sticky: false once this host has refused a timed getter (the ZDefocusZ `_keyframed` precedent —
  // the Support library's getValueAtTime throws on a host without keyframe interpolation). A grid is
  // the one thing on this node a user *animates*, so every geometry knob is read at a time.
  mutable bool _keyframed;

  // ---- knob reading, all at one time so an animated grid is read as one grid
  double readDouble(OFX::DoubleParam* knob, double t) const;
  void readVec2(OFX::Double2DParam* knob, double t, double& x, double& y) const;
  void readVec3(OFX::Double3DParam* knob, double t, double& x, double& y, double& z) const;
  // `rows`, `cols`, the two stamps and `use_source_transform` are described non-animating, so they
  // are read as constants and never at a time: a topology is a property of the node, not of a frame,
  // and the stamp rewrite must not depend on which frame a host happened to be on.
  int rowsNow() const;
  int colsNow() const;
  int topoRowsNow() const;
  int topoColsNow() const;
  int boundaryNow() const;
  bool useSourceTransformNow() const;

  void readOptions(double t, gw::Options& opt) const;
  // The host's idea of the frame, from instance properties only. Never this node's Output clip's
  // region of definition: that question re-enters this node's own RoD action (D-68).
  gw::Rect hostFrame() const;
  gw::Rect boundaryBox(double t) const;
  gw::Rect gridBasis(double t) const;
  // The same basis, with the Source clip's region of definition supplied by the caller instead of
  // fetched: the RoD action has already asked for it, and the stamp must not ask a second question of
  // the host to print the answer to one it already has (D-71). `haveSrcRod` is true only when that
  // answer was a bounded rect.
  gw::Rect gridBasisFrom(double t, const gw::Rect& srcRod, bool haveSrcRod) const;

  // A knob's getters are non-const in the Support library (`getValue` / `getValueAtTime` are suite
  // calls), so the knob families are handed over unqualified and a family member is only ever read.
  void readTransform(TransformKnobs& knobs, double t, const gw::Rect& basis,
                     gw::Transform& out) const;

  // The two grids a render runs on: fractions resolved over the basis, then the transforms applied
  // in canonical pixels (D-60).
  void buildGrids(double t, gw::Grid& src, gw::Grid& dst);

  // The overlay's pair of grids: the same reading of the knobs and the same transforms, but resolved at
  // the **stamped** topology with no resample and no in-place transform. The overlay draws and grabs the
  // vertices the knobs actually hold, because those are the slots a drag writes (D-78).
  bool buildOverlayGrids(double t, gw::Grid& src, gw::Grid& dst, gw::Rect& basis,
                         gw::Affine& srcAf, gw::Affine& dstAf);

  // The knob one control point *is*: `handle < 0` is that point's position knob, `handle` in
  // 0..kHandleCount-1 is that handle's knob, and the family selects the grid. Every one of the 640 slots
  // is described whether or not it is inside the current topology (D-53), so this is bounded by
  // `gw::kMaxPoints` and not by `rows`/`cols` — a drag writes the slot the overlay drew, which is what
  // makes a grown topology's new points draggable. Returns 0 for a slot this plugin has no knob for, which
  // the overlay reads as "this drag is refused".
  OFX::Double2DParam* latticeKnob(int family, int row, int col, int handle);

  // The overlay's undo grouping. `ParamSet::beginEditBlock`/`endEditBlock` are **protected** in the
  // Support library (ofxsParam.h:1746/1768/1771) and the overlay is not a ParamSet, so the effect opens
  // and closes the block on its behalf: one block per press gesture, so dragging a lattice point is one
  // undo step. These are *not* the topology block — the two must never nest, which is why
  // `penDown` refuses to grab while `_rewriting` has the topology block open (D-78).
  void beginLatticeEdit() { beginEditBlock("Grid Point Drag"); }
  void endLatticeEdit() { endEditBlock(); }

  friend class GridWrapOverlay;

  // The topology the *knobs* are normalised for, which is the only one readFractions may read slots
  // for; `buildGrids` carries it onto the user's `rows`/`cols` with grid.h's rebuild if the two ever
  // disagree (D-53). `rows`/`cols` are clamped by the caller, so a slot index is always in range.
  void readFractions(double t, GridKnobs& knobs, int rows, int cols,
                     std::vector<gw::FracPoint>& out) const;

  // The one WarpRender a query needs: RoD, RoI and isIdentity all ask the *same* object the render
  // would build, so a request cannot disagree with the render that follows it (D-61). The window is
  // not part of it: `WarpRender`'s footprint, plan and gather pads are the grid's own geometry, and
  // only `sourceRequest`/`destRequest` take a window.
  gw::WarpRender makeRender(double t);

  // The identity verdict, with nothing host-facing in it and nothing stamped: the action below is a thin
  // wrapper around this, so the decision can be read (and stamped) without touching the host's references.
  bool decideIdentity(double t, OFX::Clip*& chosen);

  bool maskConnected() const;
  bool channelsEmpty() const;

  // ---- the measurement stamp (D-71). Each hook builds one block and emits it once, and none of them
  // is const: a lattice is read through `readFractions`, whose knob family is a non-const reference.
  // The lattice is re-read *here* rather than carried in a member from `buildGrids`, because a host
  // renders this node's frames concurrently and a member would be shared across those threads.
  void stampLattice(ProbeBlock& blk, double t);
  void stampRender(double t, const gw::WarpRender& wr, const gw::Rect& window,
                   const bool reads[4], OFX::Image* const imgs[4], OFX::Image* outImg,
                   int nchans, int maskComp);
  void stampRoi(double t, const gw::WarpRender& wr, const gw::Rect& window, const bool asked[4],
                const gw::Rect asks[4]);
  void stampRod(double t, const gw::WarpRender& wr, const ClipRod got[3], const gw::Rect rods[3],
                const gw::Rect& basis, bool bounded, const gw::Rect& answer);
  void stampIdentity(double t, bool verdict, const OFX::Clip* clip);

  // ---- the lattice rewrite a topology change needs, and the knob panel's visibility
  void syncGridVisibility();
  // No time argument: a rewrite reads a keyed lattice at *its own* key times (collectKeyTimes) and
  // writes at those same times, so the current frame is not one of the numbers it needs.
  void syncGridTopology();

  // Every key time one grid's knobs have, in its full 320-slot set: a rewrite has to redefine the
  // lattice at each of them, because a keyframe left at an old topology's value is a keyframe whose
  // fractions mean something else now. Per grid, because the two lattices are animated separately —
  // `use_source_transform` moves the *grid*, not its knobs. Not const, because a key count is read
  // through the Support library's non-const accessors. Returns nothing; the count and the cap are the
  // caller's.
  void collectKeyTimes(GridKnobs& knobs, std::set<double>& times);

  // Carry one grid's lattice from one topology to another, in fraction space over the unit square.
  // `keyed` says whether to write it back as keys (at every collected time) or as one static value.
  // false when a host refused a write, which is what keeps the topology stamps from being moved onto
  // a lattice the host never received.
  bool rewriteGridKnobs(GridKnobs& knobs, int fromRows, int fromCols, int toRows, int toCols,
                        const std::set<double>& times, bool keyed);

  // ---- the four buttons of one lattice (D-82)
  //
  // A press is always two passes: a read-only pass that works out whether anything has to land, and a
  // write pass that opens exactly one lattice edit block *only if* the first pass found something to
  // write. That is what makes "one press, one undo step" and "a press that changed nothing leaves no
  // undo step" the same rule rather than two things the host has to get right.
  //
  // Each returns how many knobs it actually wrote (copy reports the number of values it snapshotted, and
  // a refusal reports -1), and each logs **exactly one sentence per press** — success, no-op and refusal
  // all report what they did, and the counts a sentence claims are the counts the harness measures in the
  // knobs. A press that stayed silent would leave a user who just clicked a button unable to tell whether
  // the click reached this plugin at all, which is what the D-51 log channel exists to make decidable.

  // Snapshot this grid's stamped lattice — one position and four handles per slot inside the stamp —
  // read at frame `t` into this instance's one clipboard, stamps included.
  int copyGrid(int family, double t);

  // Write that clipboard into this grid's stamped lattice at frame `t`. Refused (-1) when the clipboard's
  // stamps are not this node's stamps, because two lattices of different sizes have no slot-for-slot
  // meaning: the caller has to change `rows`/`cols` first (or copy again).
  int pasteGrid(int family, double t);

  // Add a key at frame `t` to every knob inside this grid's stamp. The value keyed is the value the knob
  // already reads at `t` — that is what "add key on all points values" means — so an unkeyed knob becomes
  // animated and a knob already keyed at `t` is left alone. A knob with no keyframes anywhere counts as
  // un-keyed at `t`, which is what makes this button do its job on a node nobody has animated.
  int setGridKeys(int family, double t);

  // Delete the key at frame `t` from every knob inside this grid's stamp. Keys at any other frame are not
  // this button's business (TODO), and a knob with no keyframes anywhere is not planned at all: pressing
  // this on a grid nobody has animated is the press that changes nothing.
  int deleteGridKeys(int family, double t);

  // The knob suite's questions, each answered once and shared by the four buttons. A -1 answer is a host
  // that will not answer, which these buttons read as "refuse", not as "no key here" — `knobHasKeyAt` is
  // an int rather than a bool for exactly that reason: `delete_keys` has to tell those two apart before
  // it decides whether there is anything to delete.
  //
  // `knobKeyCount` is the census and `knobHasKeyAt` asks it *first*, because an empty key set makes the
  // search fail rather than report "nothing at this frame": the OFX key-search status is a search status,
  // and a host follows it literally on a knob that has no keyframes at all as much as on a knob whose keys
  // sit at another frame. So "how many keys at all" is the question that separates "this host will not
  // say" from "this knob is not animated", and only when there are keys does it ask where they are.
  int knobKeyCount(OFX::Double2DParam* knob);
  int knobHasKeyAt(OFX::Double2DParam* knob, double t);   // 1 has a key at `t`, 0 has none, -1 refused

  // Does this knob already read this pair at this frame? Same question a drag asks before it writes
  // (D-80), and the same answer keeps a press that changes nothing from opening an edit block.
  bool pairAtTimeMatches(OFX::Double2DParam* knob, double x, double y, double t) const;

  // Write one pair into one lattice knob at frame `t`: keyed when `keyed`, plain otherwise (see
  // `pasteGrid`). Returns 1 when the write landed and -1 when the host refused it. Whether the knob
  // already held the pair is `pairAtTimeMatches`' question, asked by the callers before they get here.
  int writePairAtTime(OFX::Double2DParam* knob, double x, double y, double t, bool keyed);

  // Key one lattice knob at frame `t` with the value it already reads there. Returns 1/-1 like
  // `writePairAtTime`, and a refused keyed write is never retried as a plain one — keying a knob *is*
  // what this button asks for, so a host that will not key it has not done the job.
  int keyPairAtTime(OFX::Double2DParam* knob, double t);

  // Delete the key at frame `t` from one knob. The host answers this whether or not a key was there, so
  // the change is measured by asking `knobKeyCount` again, not by the return of the delete.
  bool deleteKeyAtTime(OFX::Double2DParam* knob, double t);

  // The last state `syncGridVisibility` was asked to show: the stamped topology, plus one visibility
  // toggle per lattice (D-81). Visibility follows those four values, and they only change on a topology
  // rewrite or a toggle, so remembering what was last asked for is what keeps a per-frame
  // `syncPrivateData` from paying for 1280 property writes it cannot be asked to make twice.
  int _shownRows, _shownCols;
  bool _shownSrcGrid, _shownDstGrid;
};

// ---------------------------------------------------------------------------
// The instance
// ---------------------------------------------------------------------------
/* Every knob the descriptor described is fetched here, including all 640 lattice knobs, because the
 * Support library's `fetch*Param` throws on a name the host does not know — and a host knows a name
 * only if `describeInContext` described it. Which is also why the groups are *not* fetched: the
 * Support library would happily hand back a GroupParam, but a group is a panel arrangement and this
 * plugin never reads one. (ZDefocusZ's constructor states the same rule: a host that refused a group
 * in `describe` has nothing to fetch.) */

GridWrapPlugin::GridWrapPlugin(OfxImageEffectHandle handle)
  : OFX::ImageEffect(handle),
    _srcClip(0), _dstClip(0), _bgClip(0), _maskClip(0), _outClip(0),
    _channelsKnob(0), _outputKnob(0), _warpKnob(0), _mixKnob(0), _backgroundKnob(0),
    _backgroundMixKnob(0), _boundaryKnob(0), _renderResKnob(0), _filterKnob(0),
    _fringeKnob(0), _injectKnob(0), _maskInvertKnob(0), _useSourceTransformKnob(0),
    _rowsKnob(0), _colsKnob(0), _topoRowsKnob(0), _topoColsKnob(0),
    _showSrcGridKnob(0), _showDstGridKnob(0),
    _buttons{}, _clipValid(false), _clipRows(0), _clipCols(0),
    _gridBoundsMin(0), _gridBoundsMax(0),
    _rewriting(false), _topoStuck(false), _stuckRows(-1), _stuckCols(-1),
    _secretChangesOk(true), _keyframed(true),
    // The topology the *descriptor* hid everything outside of, which is the topology the panel is
    // already showing before this instance exists: starting here is what keeps the first
    // `syncPrivateData` from asking a host for 640 property writes it has already been told.
    // The two toggles start on, which is the same answer the descriptor gave them (D-81).
    _shownRows(gw::kDefaultPoints), _shownCols(gw::kDefaultPoints),
    _shownSrcGrid(true), _shownDstGrid(true)
{
  _outClip = fetchClip(kOfxImageEffectOutputClipName);
  _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

  // The three extra inputs are described in the General context only (D-61), so they exist only in a
  // General instance. A Filter instance is the same node with one input: `output = source` /
  // `source warped` work, and the modes that read the destination get an absent clip, which the core
  // reads as an empty window — the same answer as black (render.h's RenderInputs).
  if (getContext() == OFX::eContextGeneral) {
    _dstClip = fetchClip("Dest");
    _bgClip = fetchClip("Background");
    _maskClip = fetchClip("Mask");
  }

  _channelsKnob    = fetchStringParam("channels");
  _outputKnob      = fetchChoiceParam("output");
  _warpKnob        = fetchDoubleParam("warp");
  _mixKnob         = fetchDoubleParam("mix");
  _backgroundKnob  = fetchChoiceParam("background");
  _backgroundMixKnob = fetchDoubleParam("background_mix");
  _boundaryKnob    = fetchChoiceParam("boundary_box");
  _renderResKnob   = fetchIntParam("render_res");
  _filterKnob      = fetchChoiceParam("filter");
  _fringeKnob      = fetchBooleanParam("fringe");
  _injectKnob      = fetchBooleanParam("inject");
  _maskInvertKnob  = fetchBooleanParam("maskInvert");
  _useSourceTransformKnob = fetchBooleanParam("use_source_transform");
  _rowsKnob        = fetchIntParam("rows");
  _colsKnob        = fetchIntParam("cols");
  _topoRowsKnob    = fetchIntParam("grid_topo_rows");
  _topoColsKnob    = fetchIntParam("grid_topo_cols");
  _showSrcGridKnob = fetchBooleanParam("show_source_grid");
  _showDstGridKnob = fetchBooleanParam("show_destination_grid");

  // The eight grid buttons (D-82), fetched in the same `[family][action]` shape they were declared in so
  // a press finds its knob without a second name lookup.
  for (int f = 0; f < gw::kFamilyCount; ++f)
    for (int a = 0; a < kBtnPerGrid; ++a)
      _buttons[f][a] = fetchPushButtonParam(kGridButtonNames[a][f]);
  _gridBoundsMin   = fetchDouble2DParam("grid_bounds_min");
  _gridBoundsMax   = fetchDouble2DParam("grid_bounds_max");

  // The lattice: 8x8 slots x (1 position + 4 handles) x 2 grids, in the slot order the struct
  // documents. `describe` created all 640 of each grid's knobs unconditionally, so all 640 fetch.
  for (int w = 0; w < 2; ++w) {
    GridKnobs& knobs = (w == 0) ? _srcKnobs : _dstKnobs;
    const char* which = kWhichNames[w];
    for (int r = 0; r < gw::kMaxPoints; ++r) {
      for (int c = 0; c < gw::kMaxPoints; ++c) {
        knobs.position(r, c) = fetchDouble2DParam(gridKnobName(which, r, c, 0));
        for (int h = 0; h < gw::kHandleCount; ++h)
          knobs.handle(r, c, h) =
              fetchDouble2DParam(gridKnobName(which, r, c, kHandleSpecs[h].suffix));
      }
    }
  }

  // The two Transform families.
  TransformKnobs* ts[2] = { &_srcT, &_dstT };
  for (int i = 0; i < 2; ++i) {
    const char* which = kWhichNames[i];
    TransformKnobs& k = *ts[i];
    k.translate  = fetchDouble2DParam(transformKnobName(which, "translate"));
    k.rotate     = fetchDoubleParam(transformKnobName(which, "rotate"));
    k.scale      = fetchDouble2DParam(transformKnobName(which, "scale"));
    k.skewX      = fetchDoubleParam(transformKnobName(which, "skew_x"));
    k.skewY      = fetchDoubleParam(transformKnobName(which, "skew_y"));
    k.skewOrder  = fetchChoiceParam(transformKnobName(which, "skew_order"));
    k.center     = fetchDouble2DParam(transformKnobName(which, "center"));
    k.useMatrix  = fetchBooleanParam(transformKnobName(which, "use_matrix"));
    k.matrixRow0 = fetchDouble3DParam(transformKnobName(which, "matrix_row0"));
    k.matrixRow1 = fetchDouble3DParam(transformKnobName(which, "matrix_row1"));
  }
}

// ---------------------------------------------------------------------------
// Reading knobs
// ---------------------------------------------------------------------------
// One time for the whole read, so an animated grid is read as one grid and not as a mixture of two
// frames (the TimeWarp/ZDefocusZ rule, D-21). Which knobs are read at a time and which as constants
// is DECISIONS D-53's split: the geometry travels with the frame, the node's shape does not.

double GridWrapPlugin::readDouble(OFX::DoubleParam* knob, double t) const
{
  if (_keyframed) {
    try {
      return knob->getValueAtTime(t);
    } catch (const std::exception&) {
      _keyframed = false; // this host does not interpolate: every knob is a constant from here on
    }
  }
  return knob->getValue();
}

void GridWrapPlugin::readVec2(OFX::Double2DParam* knob, double t, double& x, double& y) const
{
  if (_keyframed) {
    try {
      knob->getValueAtTime(t, x, y);
      return;
    } catch (const std::exception&) {
      _keyframed = false;
    }
  }
  knob->getValue(x, y);
}

void GridWrapPlugin::readVec3(OFX::Double3DParam* knob, double t, double& x, double& y, double& z) const
{
  if (_keyframed) {
    try {
      knob->getValueAtTime(t, x, y, z);
      return;
    } catch (const std::exception&) {
      _keyframed = false;
    }
  }
  knob->getValue(x, y, z);
}

int GridWrapPlugin::rowsNow() const
{
  int v = gw::kDefaultPoints;
  _rowsKnob->getValue(v);
  return gw::clampPoints(v); // the descriptor's range and the model's clamp are the same constants
}

int GridWrapPlugin::colsNow() const
{
  int v = gw::kDefaultPoints;
  _colsKnob->getValue(v);
  return gw::clampPoints(v);
}

int GridWrapPlugin::topoRowsNow() const
{
  int v = gw::kDefaultPoints;
  _topoRowsKnob->getValue(v);
  return gw::clampPoints(v);
}

int GridWrapPlugin::topoColsNow() const
{
  int v = gw::kDefaultPoints;
  _topoColsKnob->getValue(v);
  return gw::clampPoints(v);
}

int GridWrapPlugin::boundaryNow() const
{
  int v = 0;
  _boundaryKnob->getValue(v);
  if (v < 0 || v >= int(kBoundaryModeCount)) v = int(kBoundaryUnion);
  return v;
}

bool GridWrapPlugin::useSourceTransformNow() const
{
  return _useSourceTransformKnob->getValue();
}

bool GridWrapPlugin::maskConnected() const
{
  return _maskClip && _maskClip->isConnected();
}

bool GridWrapPlugin::channelsEmpty() const
{
  bool sel[4] = { false, false, false, false };
  std::string text;
  _channelsKnob->getValue(text);
  OfxLab::parseChannels(text, sel, "all");
  return !(sel[0] || sel[1] || sel[2] || sel[3]);
}

void GridWrapPlugin::readOptions(double t, gw::Options& opt) const
{
  // The three menu knobs are read as an index into the core's own name table, which is the table the
  // descriptor built its options from in the same order (D-63). An index out of range is a host that
  // has fewer options than it was given, and the core's default is the answer.
  int idx = 0;
  _outputKnob->getValue(idx);
  if (idx < 0 || idx >= int(gw::kOutputModeCount)) idx = int(gw::kOutputSourceWarped);
  opt.output = gw::outputModeOrDefault(gw::kOutputModeNames[idx], gw::kOutputSourceWarped);

  idx = 0;
  _backgroundKnob->getValue(idx);
  if (idx < 0 || idx >= int(gw::kBackgroundModeCount)) idx = int(gw::kBackgroundBlack);
  opt.background = gw::backgroundModeOrDefault(gw::kBackgroundModeNames[idx], gw::kBackgroundBlack);

  idx = 0;
  _filterKnob->getValue(idx);
  if (idx < 0 || idx >= int(gw::kFilterCount)) idx = int(gw::kDefaultFilter);
  opt.filter = gw::filterOrDefault(gw::kFilterNames[idx], gw::kDefaultFilter);

  int res = gw::kDefaultRenderRes;
  _renderResKnob->getValue(res);
  opt.renderRes = gw::clampRenderRes(res); // `render_res` is described non-animating

  opt.warp = readDouble(_warpKnob, t);
  opt.mix = readDouble(_mixKnob, t);
  opt.backgroundMix = readDouble(_backgroundMixKnob, t);

  opt.fringe = _fringeKnob->getValue();
  opt.inject = _injectKnob->getValue();
  opt.maskInvert = _maskInvertKnob->getValue();

  std::string text;
  _channelsKnob->getValue(text);
  OfxLab::parseChannels(text, opt.sel, "all"); // the same parser the other three plugins use (D-17)

  // A report, never a limit: the render takes its gather gains per cell from the plan, which are
  // never larger than this (render.h). Left at 1.0 so nothing here pretends to know the grid's
  // stretch before the grid has been built.
  opt.maxStretch = 1.0;

  opt.clamp(); // the core's own ranges, so a knob a host wrote outside its slider is still a value
}

// ---------------------------------------------------------------------------
// The rect the grid's fractions are stated against
// ---------------------------------------------------------------------------
/* Two steps, because two questions are being asked.

   `boundaryBox` answers "what rects does this node have on hand": every connected gatherable clip's
   region of definition, unioned (`union`) or taken as the largest single one (`format`). It is the
   fallback for the next question, and when nothing on the node is bounded it answers with the host's
   frame below.

   `gridBasis` answers "which rect do the knob fractions describe", and its first choice is the
   **Source** clip's region of definition — the picture the source grid was drawn on, which is what
   "a position is 0..1 across the basis" means for the common `output = source warped` node. A node
   with no Source falls back to the host's own idea of the frame and then to the boundary box, so the
   grid always has *some* rect to be resolved against.

   `hostFrame` is that second choice, and it is deliberately *not* the Output clip's region of
   definition. `kOfxImageEffectPropProjectOffset` / `…ProjectSize` / `…ProjectExtent` are instance
   properties a host answers from the **project** — in Natron, `getProjectDefaultFormat()`
   (OfxImageEffectInstance.cpp:319-377) — so answering them involves no node, no action and no cache.
   An output clip's RoD is the other thing: a host answers it by asking the clip's *associated node*,
   and for an output clip that node is this very effect (Natron's OfxClipInstance.cpp:1559-1572 returns
   `effect` whenever `_isOutput`), so the question re-enters this plugin's own RoD action. Measured on
   Natron 2.6, that cycle ran 29451 frames deep — 3681 turns of `getRegionOfDefinition` → `makeRender`
   → `buildGrids` → `gridBasis` → `Clip::getRegionOfDefinition` → the host → `getRegionOfDefinition`
   again, ~48 KB a turn — until the 8 MB thread stack was exhausted and the process took a raw
   SIGSEGV with `rsp` below the `[stack]` mapping. D-68.

   The fallbacks are ordered by how much they know: an offset plus a size keeps a frame that does not
   start at the origin, and an extent alone is rooted at 0,0 (ofxImageEffect.h:765-800). Both throw
   when a host does not carry the property — the Support library's `propGetDouble` defaults to
   `throwOnFailure = true` — so each is asked inside a `try`: an instance property a host refuses is
   information, not a fault, and nothing may escape an entry point (AGENTS.md rule 1).

   `grid bounds` then re-rects that basis, in fractions of it: the default pair (0,0)-(1,1) maps a
   rect onto itself, so the default *is* "no override" rather than a special case, a reversed pair is
   swapped, and an empty or degenerate one is the basis unchanged. (D-62.) */

gw::Rect GridWrapPlugin::hostFrame() const
{
  // Offset + size first: a host with a frame that does not start at 0,0 (Nuke's format x1) keeps it.
  try {
    const OfxPointD off = getProjectOffset();
    const OfxPointD size = getProjectSize();
    if (std::isfinite(off.x) && std::isfinite(off.y) &&
        std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0 && size.y > 0.0) {
      const double x0 = clampCoord(off.x), y0 = clampCoord(off.y);
      const gw::Rect r = gw::Rect::normalised(
          gw::Rect(x0, y0, clampCoord(x0 + size.x), clampCoord(y0 + size.y)));
      if (!r.empty()) return r;
    }
  } catch (const std::exception&) {
  } // this host has no project size to say: ask the extent

  try {
    const OfxPointD ext = getProjectExtent();
    if (std::isfinite(ext.x) && std::isfinite(ext.y) && ext.x > 0.0 && ext.y > 0.0) {
      const gw::Rect r = gw::Rect(0.0, 0.0, clampCoord(ext.x), clampCoord(ext.y)); // rooted at 0,0
      if (!r.empty()) return r;
    }
  } catch (const std::exception&) {
  }

  return gw::Rect(); // the host has not said how big a frame is: callers fall back again
}

gw::Rect GridWrapPlugin::boundaryBox(double t) const
{
  OFX::Clip* clips[4] = { _srcClip, _dstClip, _bgClip, _maskClip };

  gw::Rect uni;
  gw::Rect biggest;
  double bestArea = -1.0;

  // Only the clips that *answer* contribute. A connected clip whose extent the host will not say
  // contributes nothing here, because a basis has to be a rect: `boundary_box` chooses a coordinate
  // space, it does not promise an output extent, and it is the promise the RoD action is strict about.
  for (int i = 0; i < 4; ++i) {
    gw::Rect r;
    if (clipRod(clips[i], t, r) != kClipRodRect) continue;

    uni = gw::Rect::unite(uni, r);
    const double area = (r.x1 - r.x0) * (r.y1 - r.y0);
    if (area > bestArea) {
      bestArea = area;
      biggest = r;
    }
  }

  if (uni.empty()) {
    // Nothing connected: the host's own frame is what a render will be asked to fill, and it is the
    // only rect here that can be asked for without asking this node about itself (D-68). An empty
    // answer is legitimate too — resolveGrid substitutes the unit square for a basis with no extent
    // (grid.cpp), which keeps every fraction finite.
    const gw::Rect frame = hostFrame();
    return frame.empty() ? gw::Rect() : frame;
  }

  return boundaryNow() == int(kBoundaryFormat) ? biggest : uni;
}

gw::Rect GridWrapPlugin::gridBasisFrom(double t, const gw::Rect& srcRod, bool haveSrcRod) const
{
  gw::Rect base;

  if (haveSrcRod) base = srcRod;

  if (base.empty()) base = hostFrame();
  if (base.empty()) base = boundaryBox(t);
  if (base.empty()) return gw::Rect();

  double fx0 = 0.0, fy0 = 0.0, fx1 = 1.0, fy1 = 1.0;
  readVec2(_gridBoundsMin, t, fx0, fy0);
  readVec2(_gridBoundsMax, t, fx1, fy1);

  const gw::Rect f = gw::Rect::normalised(gw::Rect(finiteOr(fx0, 0.0), finiteOr(fy0, 0.0),
                                                   finiteOr(fx1, 1.0), finiteOr(fy1, 1.0)));
  if (f.empty()) return base; // an empty or collapsed override means the basis, unchanged

  const double w = base.x1 - base.x0;
  const double h = base.y1 - base.y0;
  const gw::Rect out = gw::Rect::normalised(
      gw::Rect(base.x0 + f.x0 * w, base.y0 + f.y0 * h, base.x0 + f.x1 * w, base.y0 + f.y1 * h));

  return out.empty() ? base : out;
}

gw::Rect GridWrapPlugin::gridBasis(double t) const
{
  gw::Rect srcRod;
  const bool have = clipRod(_srcClip, t, srcRod) == kClipRodRect;
  return gridBasisFrom(t, srcRod, have);
}

// ---------------------------------------------------------------------------
/* The knobs are fractions and degrees; `gw::Transform` works in the grid's own canonical pixels,
 * because a rotation angle and a scale factor are meaningless until there is a rect to rotate *in*
 * (grid.h's `affineFor`). So this is where fractions become pixels: `translate` and `center` are
 * stated across the basis, and everything else is already a pure number.
 *
 * `skew` is clamped here rather than left to the descriptor: `affineFor` takes `tan(skewDeg)`, and a
 * host that lets a script write 90 would get an infinite affine and a grid of NaN. The clamp is the
 * descriptor's own hard range, written a second time where it is used. (D-63.) */

void GridWrapPlugin::readTransform(TransformKnobs& knobs, double t, const gw::Rect& basis,
                                   gw::Transform& out) const
{
  out = gw::Transform(); // one place for the defaults, so a missing knob is the identity

  const double w = basis.x1 - basis.x0;
  const double h = basis.y1 - basis.y0;

  double tx = 0.0, ty = 0.0;
  readVec2(knobs.translate, t, tx, ty);
  out.tx = finiteOr(tx, 0.0) * w;
  out.ty = finiteOr(ty, 0.0) * h;

  out.rotateDeg = finiteOr(readDouble(knobs.rotate, t), 0.0);

  double sx = 1.0, sy = 1.0;
  readVec2(knobs.scale, t, sx, sy);
  out.sx = finiteOr(sx, 1.0);
  out.sy = finiteOr(sy, 1.0);

  double kx = 0.0, ky = 0.0;
  kx = readDouble(knobs.skewX, t);
  ky = readDouble(knobs.skewY, t);
  out.skewXDeg = clampAngleDeg(finiteOr(kx, 0.0));
  out.skewYDeg = clampAngleDeg(finiteOr(ky, 0.0));

  int order = 0;
  knobs.skewOrder->getValue(order);
  out.skewOrderXY = (order <= 0); // option 0 is `XY`, which is grid.h's `skewOrderXY == true`

  double cx = 0.5, cy = 0.5;
  readVec2(knobs.center, t, cx, cy);
  out.cx = basis.x0 + finiteOr(cx, 0.5) * w;
  out.cy = basis.y0 + finiteOr(cy, 0.5) * h;

  out.useMatrix = knobs.useMatrix->getValue();
  out.matrix3x3 = false; // a 3x3 would be a projective map, and `Affine` has nowhere to put one

  double r0x = 1.0, r0y = 0.0, r0z = 0.0, r1x = 0.0, r1y = 1.0, r1z = 0.0;
  readVec3(knobs.matrixRow0, t, r0x, r0y, r0z);
  readVec3(knobs.matrixRow1, t, r1x, r1y, r1z);
  out.m[0] = finiteOr(r0x, 1.0);
  out.m[1] = finiteOr(r0y, 0.0);
  out.m[2] = finiteOr(r0z, 0.0);
  out.m[3] = finiteOr(r1x, 0.0);
  out.m[4] = finiteOr(r1y, 1.0);
  out.m[5] = finiteOr(r1z, 0.0);
  out.m[6] = 0.0;
  out.m[7] = 0.0;
  out.m[8] = 1.0;
}

// ---------------------------------------------------------------------------
// The two grids a render runs on
// ---------------------------------------------------------------------------
/* Fractions first, pixels second, and the stamped topology first of all (D-53): a knob's fraction is
 * only meaningful for the topology it was written for, so the slots are read at the stamp, resolved
 * at the stamp, and only then carried onto the `rows`/`cols` the user is asking for — with grid.h's
 * rebuild, which is the same operation the rewrite would have applied to the knobs themselves. When
 * the two topologies disagree, either the rewrite has not run yet or a host refused it; either way
 * this is where the render stays correct instead of reading 25 knobs and calling that an 8x8 grid.
 *
 * The transforms come last and go in canonical pixels, because a rotation is a rotation *in a rect*.
 * `applyTransform` maps the points and the handles (a handle is a tangent) and refuses a singular
 * linear part, in which case the grid it was given is the grid that renders — a collapsed grid is not
 * un-collapsible, and the user's other knobs still work. */

void GridWrapPlugin::readFractions(double t, GridKnobs& knobs, int rows, int cols,
                                   std::vector<gw::FracPoint>& out) const
{
  out.assign(std::size_t(rows) * std::size_t(cols), gw::FracPoint());

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      gw::FracPoint& f = out[std::size_t(r) * std::size_t(cols) + std::size_t(c)];

      double x = 0.0, y = 0.0;
      readVec2(knobs.position(r, c), t, x, y);
      f.x = x;
      f.y = y;

      for (int h = 0; h < gw::kHandleCount; ++h)
        readVec2(knobs.handle(r, c, h), t, f.h[h][0], f.h[h][1]);
    }
  }
}

void GridWrapPlugin::buildGrids(double t, gw::Grid& outSrc, gw::Grid& outDst)
{
  const int rows = rowsNow(), cols = colsNow();
  const int stampR = topoRowsNow(), stampC = topoColsNow();
  const gw::Rect basis = gridBasis(t);

  std::vector<gw::FracPoint> knobs;

  readFractions(t, _srcKnobs, stampR, stampC, knobs);
  outSrc = gw::resolveGrid(stampR, stampC, basis, knobs);
  if (stampR != rows || stampC != cols)
    outSrc = gw::resampleGrid(outSrc, rows, cols, basis);

  readFractions(t, _dstKnobs, stampR, stampC, knobs);
  outDst = gw::resolveGrid(stampR, stampC, basis, knobs);
  if (stampR != rows || stampC != cols)
    outDst = gw::resampleGrid(outDst, rows, cols, basis);

  gw::Transform srcT, dstT;
  readTransform(_srcT, t, basis, srcT);
  readTransform(_dstT, t, basis, dstT);

  gw::applyTransform(outSrc, gw::affineFor(srcT));

  // `use_source_transform` (Nuke's GridWarpTracker knob): the destination grid inherits the source
  // grid's transform *before* its own, which is what keeps the two lattices travelling together while
  // a morph runs and lets the dst knobs describe only the difference. Order matters here, and it is
  // the order Nuke's page states the checkbox in.
  if (useSourceTransformNow())
    gw::applyTransform(outDst, gw::affineFor(srcT));

  gw::applyTransform(outDst, gw::affineFor(dstT));
}

// `outer` applied after `inner` — the composition `buildGrids` performs in place when a grid carries
// two transforms, spelled out here because grid.h applies an affine to a grid rather than composing
// two of them (grid.h:374-378).
static gw::Affine composeAffine(const gw::Affine& outer, const gw::Affine& inner)
{
  gw::Affine r;
  r.a00 = outer.a00 * inner.a00 + outer.a01 * inner.a10;
  r.a01 = outer.a00 * inner.a01 + outer.a01 * inner.a11;
  r.tx = outer.a00 * inner.tx + outer.a01 * inner.ty + outer.tx;
  r.a10 = outer.a10 * inner.a00 + outer.a11 * inner.a10;
  r.a11 = outer.a10 * inner.a01 + outer.a11 * inner.a11;
  r.ty = outer.a10 * inner.tx + outer.a11 * inner.ty + outer.ty;
  return r;
}

bool GridWrapPlugin::buildOverlayGrids(double t, gw::Grid& src, gw::Grid& dst, gw::Rect& basis,
                                       gw::Affine& srcAf, gw::Affine& dstAf)
{
  basis = gridBasis(t);
  if (basis.empty()) return false; // nothing to draw, and nothing for a drag to map back into

  const int rows = topoRowsNow(), cols = topoColsNow();

  std::vector<gw::FracPoint> knobs;

  readFractions(t, _srcKnobs, rows, cols, knobs);
  src = gw::resolveGrid(rows, cols, basis, knobs);

  readFractions(t, _dstKnobs, rows, cols, knobs);
  dst = gw::resolveGrid(rows, cols, basis, knobs);

  gw::Transform srcT, dstT;
  readTransform(_srcT, t, basis, srcT);
  readTransform(_dstT, t, basis, dstT);

  srcAf = gw::affineFor(srcT);
  dstAf = gw::affineFor(dstT);
  if (useSourceTransformNow())
    dstAf = composeAffine(dstAf, srcAf); // the same composition buildGrids applies in place, in the same order

  return true;
}

OFX::Double2DParam* GridWrapPlugin::latticeKnob(int family, int row, int col, int handle)
{
  if (row < 0 || col < 0 || row >= gw::kMaxPoints || col >= gw::kMaxPoints)
    return 0;
  GridKnobs& knobs = (family == gw::kFamilyDst) ? _dstKnobs : _srcKnobs;
  if (handle < 0)
    return knobs.position(row, col);
  if (handle >= gw::kHandleCount)
    return 0;
  return knobs.handle(row, col, handle);
}

gw::WarpRender GridWrapPlugin::makeRender(double t)
{
  gw::Grid src, dst;
  buildGrids(t, src, dst);

  gw::Options opt;
  readOptions(t, opt);

  return gw::WarpRender(src, dst, opt);
}

// ---------------------------------------------------------------------------
// The measurement stamp (D-71)
// ---------------------------------------------------------------------------
/* The four hooks, in the order they can appear in a log. A lattice is re-read here rather than carried
 * in from `buildGrids` (see the header note above), and every rect printed here is printed with the
 * predictor's own `%.9g`, so a stamp line and a `gw_core_probe` line describing the same number are the
 * same text. */
void GridWrapPlugin::stampLattice(ProbeBlock& blk, double t)
{
  if (!probeGate())
    return;
  const int rows = topoRowsNow();
  const int cols = topoColsNow();
  std::vector<gw::FracPoint> pts;
  readFractions(t, _srcKnobs, rows, cols, pts);
  stampDigest(blk, "lattice src", rows, cols, probeDigestFractions(rows, cols, pts));
  readFractions(t, _dstKnobs, rows, cols, pts);
  stampDigest(blk, "lattice dst", rows, cols, probeDigestFractions(rows, cols, pts));
}

void GridWrapPlugin::stampRoi(double t, const gw::WarpRender& wr, const gw::Rect& window,
                              const bool asked[4], const gw::Rect asks[4])
{
  const char* const path = probeGate();
  if (!path)
    return;
  static const char* const kClipNames[4] = { "src", "dst", "bg", "mask" };
  ProbeBlock blk(path, "roi");
  blk.add("t %.9g", t);
  blk.add("window %s", stampRect(window).c_str());
  // What *we* answered, per clip: HostSupport pre-fills the render window as each connected clip's RoI
  // before this action runs, so an empty answer is not "no constraint" — it is "whatever the host had
  // already decided" (D-69). The analyser needs both numbers: whether the clip was asked at all, and
  // which of the two we said.
  for (int i = 0; i < 4; ++i)
    blk.add("ask %s %d %s %s", kClipNames[i], asked[i] ? 1 : 0,
            asked[i] ? (asks[i].empty() ? "empty" : "requested") : "notasked",
            stampRect(asks[i]).c_str());
  blk.add("foot %s", stampRect(wr.footprint()).c_str());
  blk.add("pad %.9g", wr.maxGatherPad());
  blk.emit();
}

void GridWrapPlugin::stampRod(double t, const gw::WarpRender& wr, const ClipRod got[3],
                              const gw::Rect rods[3], const gw::Rect& basis, bool bounded,
                              const gw::Rect& answer)
{
  const char* const path = probeGate();
  if (!path)
    return;
  static const char* const kRodClipNames[3] = { "src", "dst", "bg" };
  static const char* const kClipRodNames[3] = { "none", "unbounded", "rect" };
  ProbeBlock blk(path, "rod");
  blk.add("t %.9g", t);
  blk.add("basis %s", stampRect(basis).c_str());
  blk.add("foot %s", stampRect(wr.footprint()).c_str());
  for (int i = 0; i < 3; ++i) {
    blk.add("cliprod %s %s %s", kRodClipNames[i], kClipRodNames[int(got[i])],
            stampRect(rods[i]).c_str());
  }
  blk.add("rod answered %d %s", bounded ? 1 : 0, stampRect(answer).c_str());
  blk.emit();
}

void GridWrapPlugin::stampIdentity(double t, bool verdict, const OFX::Clip* clip)
{
  const char* const path = probeGate();
  if (!path)
    return;
  ProbeBlock blk(path, "identity");
  blk.add("t %.9g", t);
  blk.add("identity %d %s", verdict ? 1 : 0, clip ? clip->name().c_str() : "none");
  // A digest instead of a hash, because fetching an image to hash it here would be a fetch outside a
  // render action. The lattice is what makes the two lattices a host can hand this action distinguishable.
  stampLattice(blk, t);
  blk.emit();
}

void GridWrapPlugin::stampRender(double t, const gw::WarpRender& wr, const gw::Rect& window,
                                 const bool reads[4], OFX::Image* const imgs[4], OFX::Image* outImg,
                                 int nchans, int maskComp)
{
  const char* const path = probeGate();
  if (!path)
    return;
  static const char* const kClipNames[4] = { "src", "dst", "bg", "mask" };
  const gw::Options& opt = wr.options();

  char sel[5] = { '0', '0', '0', '0', 0 };
  for (int k = 0; k < 4; ++k)
    sel[k] = opt.sel[k] ? '1' : '0';

  ProbeBlock blk(path, "render");
  blk.add("t %.9g", t);
  blk.add("window %s", stampRect(window).c_str());
  // The `output` and `background` names contain spaces ("source warped", "on bg"), so they are not put
  // on a token-split line with anything else: each gets a line of its own, where the name *is* the rest
  // of the line. The filter names are single words, so the filter can stay here.
  blk.add("opts %d %s %.9g %.9g %.9g %d %d %d %s", opt.renderRes,
          gw::filterName(opt.filter), opt.warp, opt.mix, opt.backgroundMix,
          opt.fringe ? 1 : 0, opt.inject ? 1 : 0, opt.maskInvert ? 1 : 0, sel);
  blk.add("mode %s", gw::kOutputModeNames[opt.output]);
  blk.add("bkg %s", gw::kBackgroundModeNames[opt.background]);
  blk.add("reads %d %d %d %d", reads[0] ? 1 : 0, reads[1] ? 1 : 0, reads[2] ? 1 : 0,
          reads[3] ? 1 : 0);
  blk.add("foot %s", stampRect(wr.footprint()).c_str());
  // The plan as the three numbers that summarise it: the same rule priced per res would print
  // res-scaled cells and blow the block cap for no extra information.
  blk.add("cells %d %d", int(wr.plan().size()), int(wr.inverterCellCount()));
  blk.add("planbounds %s", stampRect(wr.planBounds()).c_str());
  blk.add("pad %.9g", wr.maxGatherPad());
  stampLattice(blk, t);
  for (int i = 0; i < 4; ++i) {
    const OFX::Image* img = imgs[i];
    const int ncomp = img ? img->getPixelComponentCount() : 0;
    const int pick = (nchans == 1 && ncomp == 4) ? 3 : 0;
    blk.add("img %s %d %d %s %d %s %d", kClipNames[i], img ? 1 : 0, reads[i] ? 1 : 0,
            img ? stampRect(boundsRect(img)).c_str() : "none", ncomp,
            img ? probeComps(img->getPixelComponents()) : "none", pick);
    if (img)
      stampHash(blk, kClipNames[i], pick, img, boundsRect(img));
  }
  if (reads[3])
    blk.add("maskcomp %d", maskComp);
  const int oncomp = outImg ? outImg->getPixelComponentCount() : 0;
  blk.add("out %s %d %s", outImg ? stampRect(boundsRect(outImg)).c_str() : "none", oncomp,
          outImg ? probeComps(outImg->getPixelComponents()) : "none");
  // The delivered channels, hashed one plane each: what a host actually hands its own output back is
  // the number the analyser pairs a block against.
  for (int k = 0; k < oncomp; ++k)
    if (outImg)
      stampHash(blk, "out", k, outImg, boundsRect(outImg));
  blk.emit();
}

// ---------------------------------------------------------------------------
// Carrying the lattice across a topology change (D-53)
// ---------------------------------------------------------------------------
/* `rows` and `cols` are ordinary knobs: a user changes one in the middle of an animation, on a node
 * whose 640 knobs of fractions were all written for the topology being changed away from. Reconciling
 * that is a knob rewrite, and every part of it follows from one rule — the rewrite may change what the
 * slots *mean*, it may not change what the user put in them.
 *
 * Which makes it:
 *
 *   - **Fraction space, over the unit square.** The old lattice is read at the stamped topology,
 *     resolved over `Rect(0,0,1,1)` — the one basis whose answer depends on no clip's size or format —
 *     `resampleGrid` the render applies when the stamps and `rows`/`cols` disagree, which is the point:
 *     the rewrite is an optimisation of the fallback, not a second mapping the two can disagree about.
 *     (A knob rewrite is the only way a *host* can show the new points; correctness never needs it.)
 *   - **Read before write, at every key time.** What a slot should hold now depends on the whole old
 *     lattice at that time — a resample evaluates the old Bézier surface at new parameter positions —
 *     so pass 1 collects every lattice, pass 2 writes them.
 *   - **Keyframes kept, not rebuilt.** A keyed knob is written with `setValueAtTime` at each of its own
 *     key times, which leaves the key count and the interpolation types a user chose alone.
 *     `deleteAllKeys` would throw away how the grid was animated, and a knob that had no keys is written
 *     once, so it stays a constant.
 *   - **Only the slots the new topology reaches.** A slot outside it is not read by the render either;
 *     writing it would put numbers in knobs that mean nothing.
 *   - **Added slots first.** If a host refuses a write partway through, the slots left stale are then
 *     the ones the *old* topology never had — which is precisely what the stamps still say the render
 *     may not read — so the lattice the user had survives whole.
 *   - **Stamps last.** They are what declares the topology, so they move only once the lattice they
 *     describe has been written. If anything failed, the node keeps declaring the topology it still
 *     holds, and the render's resample is what carries it to the one the user asked for.
 *   - **Bounded, and it says so.** A lattice animated at more distinct key times than
 *     `kMaxRewriteKeys` is one this plugin will not rewrite: 64 knobs each carrying dozens of keys is a
 *     script that outlives a knob panel, and a rewrite is not the only way to be correct. So it stops,
 *     keeps the keyframes, leaves the stamps alone, and prints which of the two happened to the log
 *     channel the other plugins use (D-51) — a silent no-op would leave knobs whose numbers now mean
 *     something else. */

namespace {

// The cap on distinct key times one grid's lattice is animated at, for a rewrite. Over it, the
// render-time resample is what answers, and the knob panel keeps showing the old lattice.
const int kMaxRewriteKeys = 32;

// The pair of one lattice slot a knob carries. `-1` is the point's own position; `0..3` are its four
// handles, in grid.h's kHVNeg/kHVPos/kHUNeg/kHUPos order, which is the order the handle knobs were
// described in. One lattice slot is one `FracPoint`, and a `FracPoint` holds the position *and* the
// handles, so writing a knob means choosing which of its five pairs to write - and writing the position
// into all five is what a real host measured after growing `rows`/`cols`: every handle read its point's
// position back (D-64). `comp` is why that cannot happen again.
inline void fracPair(const gw::FracPoint& f, int comp, double& x, double& y)
{
  if (comp < 0) {
    x = f.x;
    y = f.y;
    return;
  }
  assert(comp < gw::kHandleCount);
  x = f.h[comp][0];
  y = f.h[comp][1];
}

// One lattice slot onto one knob: the static case is one write, the animated case is one write per key
// time, in the same order the lattices were collected. Returns false when this host refused the write.
bool writeFracKnob(OFX::Double2DParam* knob,
                   const std::vector<std::vector<gw::FracPoint> >& lattices,
                   const std::set<double>& times,
                   bool keyed, std::size_t idx, int comp)
{
  if (!knob) return false;
  if (lattices.empty()) return false;

  try {
    if (!keyed || times.empty()) {
      double x = 0.0, y = 0.0;
      fracPair(lattices[0][idx], comp, x, y);
      knob->setValue(x, y);
      return true;
    }

    if (lattices.size() != times.size()) return false;   // the two passes must agree, index by index

    std::size_t i = 0;
    for (std::set<double>::const_iterator it = times.begin(); it != times.end(); ++it, ++i) {
      double x = 0.0, y = 0.0;
      fracPair(lattices[i][idx], comp, x, y);
      knob->setValueAtTime(*it, x, y);   // implicitly adds a key: an un-animated knob becomes one
    }
    return true;
  } catch (const std::exception&) {
    return false;   // a host that will not let a plugin write its knobs
  }
}

} // namespace

void GridWrapPlugin::collectKeyTimes(GridKnobs& knobs, std::set<double>& times)
{
  // The whole 320-knob set of one grid, not just the in-topology ones: a key a user put on a point the
  // current stamps do not reach is still a key this node carries, and a rewrite that ignored it would be
  // reading a lattice at times it has not accounted for. The Support library's `getNumKeys` is "0 if the
  // param is not animating", so the common case is 320 cheap property reads and no work.
  for (std::size_t s = 0; s < std::size_t(kGridSlots); ++s) {
    OFX::Double2DParam* slots[1 + gw::kHandleCount];
    slots[0] = knobs.position(int(s / std::size_t(kGridStride)), int(s % std::size_t(kGridStride)));
    for (int h = 0; h < gw::kHandleCount; ++h)
      slots[1 + h] = knobs.handle(int(s / std::size_t(kGridStride)),
                                  int(s % std::size_t(kGridStride)), h);

    for (int i = 0; i < 1 + gw::kHandleCount; ++i) {
      OFX::Double2DParam* knob = slots[i];
      if (!knob) continue;

      unsigned int n = 0;
      try {
        n = knob->getNumKeys();
      } catch (const std::exception&) {
        n = 0;   // a host that will not report keys: treat this knob as a constant
      }

      for (unsigned int k = 0; k < n; ++k) {
        double t = 0.0;
        try {
          t = knob->getKeyTime(int(k));
        } catch (const std::exception&) {
          continue;
        }
        if (!std::isfinite(t)) continue;

        times.insert(t);
        if (times.size() > std::size_t(kMaxRewriteKeys))
          return;   // past the cap: the caller answers with the resample, not with a rewrite
      }
    }
  }
}

bool GridWrapPlugin::rewriteGridKnobs(GridKnobs& knobs, int fromRows, int fromCols,
                                     int toRows, int toCols,
                                     const std::set<double>& times, bool keyed)
{
  // The unit square is the basis a rewrite resolves over, and it is the only one that can be: the two
  // topologies have to give the *same* fractions out of it, and a real basis is a rect this node may not
  // even have (no input connected). It is also what makes the rewrite format-independent, which is the
  // whole point of stating a grid in fractions (D-53).
  const gw::Rect unit(0.0, 0.0, 1.0, 1.0);

  // ---- pass 1: every old lattice, at the topology it was written for -----------------------------
  std::vector<std::vector<gw::FracPoint> > oldLattices;
  std::vector<std::vector<gw::FracPoint> > newLattices;

  if (keyed && !times.empty()) {
    oldLattices.reserve(times.size());
    for (std::set<double>::const_iterator it = times.begin(); it != times.end(); ++it) {
      std::vector<gw::FracPoint> f;
      readFractions(*it, knobs, fromRows, fromCols, f);
      oldLattices.push_back(f);
    }
  } else {
    // One value, so one lattice — and reading a knob that has no keys at any time is reading that value.
    std::vector<gw::FracPoint> f;
    readFractions(0.0, knobs, fromRows, fromCols, f);
    oldLattices.push_back(f);
  }

  newLattices.reserve(oldLattices.size());
  for (std::size_t i = 0; i < oldLattices.size(); ++i) {
    const gw::Grid from = gw::resolveGrid(fromRows, fromCols, unit, oldLattices[i]);
    const gw::Grid to = gw::resampleGrid(from, toRows, toCols, unit);

    std::vector<gw::FracPoint> f = to.fractions();
    if (f.size() != std::size_t(toRows) * std::size_t(toCols)) {
      // Cannot happen after resolveGrid + resampleGrid, and worth stating: writing a short lattice into a
      // full set of knobs is how a bug would turn into a partly-neutral grid.
      f = gw::neutralFractions(toRows, toCols);
    }
    newLattices.push_back(f);
  }

  // ---- pass 2: write, slots the new topology *adds* first ----------------------------------------
  bool ok = true;
  for (int pass = 0; pass < 2; ++pass) {
    for (int r = 0; r < toRows; ++r) {
      for (int c = 0; c < toCols; ++c) {
        const bool added = (r >= fromRows || c >= fromCols);
        if ((pass == 0) != added) continue;

        const std::size_t idx = std::size_t(r) * std::size_t(toCols) + std::size_t(c);
        if (!writeFracKnob(knobs.position(r, c), newLattices, times, keyed, idx, -1)) ok = false;
        for (int h = 0; h < gw::kHandleCount; ++h) {
          if (!writeFracKnob(knobs.handle(r, c, h), newLattices, times, keyed, idx, h)) ok = false;
        }
      }
    }
  }

  return ok;
}

// ---------------------------------------------------------------------------
// What the knob panel shows
// ---------------------------------------------------------------------------
/* The slots outside the topology the stamps declare are secret, which is the only thing that keeps a
 * 640-knob node usable: at the default 5x5 a user sees 25 points and their handles, not 64.
 *
 * It is a *descriptor* property first and a runtime request second, and a host answers both. Natron 2.6
 * was measured (_bin/gw_secret_probe.py): the slots described outside the default 5x5 report
 * `getIsVisible() == false`; growing `rows`/`cols` to 8x8 makes those same slots report true; shrinking
 * them to 3x8 and then 5x8 makes them report false again. The panel follows the plugin's runtime
 * `setIsSecret` in both directions. The mechanism is the property, not HostSupport's callback: Support's
 * `Param::setIsSecret` writes `kOfxParamPropSecret` into the instance property set
 * (Support/Library/ofxsParams.cpp:1238), HostSupport registers a notify hook for that property
 * (HostSupport/src/ofxhParam.cpp:692), that callback `Instance::setSecret()` is an empty function
 * (`:716`), and `Base::getSecret()` reads the property back (`:154`) — the property is what Natron's
 * reported visibility follows. `_shownRows`/`_shownCols` are what stop a per-frame `syncPrivateData`
 * from paying for 640 property writes this plugin has already asked for.
 *
 * What that measurement closes is the claim this file used to make — that a runtime ask is something
 * Natron answers by doing nothing, and so a Natron panel lists all 640 slots. It does not; the correction
 * is DECISIONS.md.
 *
 * A slot is listed when *both* answers say shown: it sits inside the stamped topology, and its grid's
 * toggle (`show_source_grid` / `show_destination_grid`, D-81) is on. A toggle is a second term in this one
 * mechanism rather than a second mechanism, because the thing being hidden is the same thing — a knob the
 * panel has no business listing — and a host is already wired to answer that term.
 *
 * The *first* term is no render-time condition, and neither is the second: the render reads every knob of
 * the stamped topology whatever either one says, because a hidden knob still holds its value. What the
 * second term *is* a condition for is the Viewer overlay (D-84, which supersedes what D-81 recorded here):
 * a grid whose toggle is off is still drawn there, but dimmed, and none of its points or handles can be
 * selected or dragged. So a toggle now shapes the node in both places a lattice is visible — `output =
 * source` warps through one lattice, and the other one's knobs stay out of the panel *and* out of the way
 * in the Viewer. */

void GridWrapPlugin::syncGridVisibility()
{
  if (!_secretChangesOk) return;

  const int rows = topoRowsNow(), cols = topoColsNow();   // the topology the *knobs* are read at
  const bool shown[2] = { _showSrcGridKnob->getValue(), _showDstGridKnob->getValue() };
  if (rows == _shownRows && cols == _shownCols && shown[0] == _shownSrcGrid && shown[1] == _shownDstGrid)
    return;

  GridKnobs* grids[2] = { &_srcKnobs, &_dstKnobs };

  try {
    for (int g = 0; g < 2; ++g) {
      for (int r = 0; r < gw::kMaxPoints; ++r) {
        for (int c = 0; c < gw::kMaxPoints; ++c) {
          const bool secret = !(shown[g] && r < rows && c < cols);

          grids[g]->position(r, c)->setIsSecret(secret);
          for (int h = 0; h < gw::kHandleCount; ++h)
            grids[g]->handle(r, c, h)->setIsSecret(secret);
        }
      }
    }
  } catch (const std::exception&) {
    // A host that rejects the property write outright (`setIsSecret` is a property on the instance, not
    // one every host implements). Sticky, so the next frame does not try 640 times again; the
    // describe-time secrets already describe this node's default topology.
    _secretChangesOk = false;
    return;
  }

  _shownRows = rows;
  _shownCols = cols;
  _shownSrcGrid = shown[0];
  _shownDstGrid = shown[1];
}

// ---------------------------------------------------------------------------
// The four buttons of one lattice (D-82)
// ---------------------------------------------------------------------------
namespace {

// What a log sentence calls one of the two lattices — the same pairing the descriptor and the overlay
// use: family 0 is the grid whose points the Source is read through, family 1 the destination grid.
const char* gridFamilyWord(int family)
{
  return family == 0 ? "source" : "destination";
}

} // namespace

int GridWrapPlugin::knobKeyCount(OFX::Double2DParam* knob)
{
  if (!knob) return -1;
  try {
    return static_cast<int>(knob->getNumKeys());
  } catch (const std::exception&) {
    return -1;   // this host does not answer the question at all
  }
}

int GridWrapPlugin::knobHasKeyAt(OFX::Double2DParam* knob, double t)
{
  const int count = knobKeyCount(knob);
  if (count < 0) return -1;   // a census this host refuses to take is not an answer of "no key here"
  if (count == 0) return 0;   // no keyframes anywhere: there cannot be one at this frame

  try {
    // `near` is the search that asks "is there a key at this frame" rather than "where is the curve
    // around this frame"; the Support library turns a host's "no such key" into -1 without throwing.
    // Asking it only once the census says keys exist is what keeps an *un-animated* knob from reading as
    // a host that refuses: Natron answers that search with a failure status on an empty key set, and a
    // failure status is exactly what -1 means here.
    return knob->getKeyIndex(t, OFX::eKeySearchNear) >= 0 ? 1 : 0;
  } catch (const std::exception&) {
    return -1;
  }
}

bool GridWrapPlugin::pairAtTimeMatches(OFX::Double2DParam* knob, double x, double y, double t) const
{
  if (!knob) return false;
  double haveX = 0.0, haveY = 0.0;
  try {
    readVec2(knob, t, haveX, haveY);   // the same read the render uses, keyed knobs included
  } catch (const std::exception&) {
    return false;
  }
  return std::abs(haveX - x) <= kKeyEps && std::abs(haveY - y) <= kKeyEps;
}

int GridWrapPlugin::writePairAtTime(OFX::Double2DParam* knob, double x, double y, double t, bool keyed)
{
  if (!knob) return -1;
  try {
    if (keyed) {
      knob->setValueAtTime(t, x, y);   // implicitly adds a key: an un-animated knob becomes one
      return 1;
    }
    knob->setValue(x, y);              // the whole timeline holds this pair, and no key is added
    return 1;
  } catch (const std::exception&) {
    return -1;   // a host that will not let the plugin write its knobs
  }
}

int GridWrapPlugin::keyPairAtTime(OFX::Double2DParam* knob, double t)
{
  if (!knob) return -1;

  double x = 0.0, y = 0.0;
  try {
    readVec2(knob, t, x, y);
  } catch (const std::exception&) {
    return -1;
  }
  return writePairAtTime(knob, x, y, t, true);
}

bool GridWrapPlugin::deleteKeyAtTime(OFX::Double2DParam* knob, double t)
{
  const int before = knobKeyCount(knob);
  if (before <= 0) return false;              // refused, or nothing animated in it
  if (knobHasKeyAt(knob, t) != 1) return false;   // no key at this frame, or it refused to look

  try {
    knob->deleteKeyAtTime(t);
  } catch (const std::exception&) {
    return false;
  }

  // A host answers this whether or not a key came out (Natron's own delete reports success whenever the
  // call was legal), so the change is measured by re-asking the key count and not by the return of the
  // delete.
  const int after = knobKeyCount(knob);
  return after >= 0 && after < before;
}

int GridWrapPlugin::copyGrid(int family, double t)
{
  GridKnobs& knobs = (family == 0) ? _srcKnobs : _dstKnobs;
  const int rows = topoRowsNow(), cols = topoColsNow();

  std::vector<gw::FracPoint> snap;
  try {
    // The same read the render does, at this frame: keyed knobs included, and in the stamped topology,
    // because a fraction is only meaningful for the topology its knobs were normalised for.
    readFractions(t, knobs, rows, cols, snap);
  } catch (const std::exception&) {
    logGridNote("copy refused: this host would not let the grid be read");
    return -1;
  }

  const int values = rows * cols * (1 + gw::kHandleCount);   // one point and four handles per slot

  // One clipboard per instance, stamps included: that is what lets a copy off one grid land on the other
  // and what makes a paste onto a node whose stamps have moved a refusal instead of a half-paste.
  _clipRows = rows;
  _clipCols = cols;
  _clip.swap(snap);
  _clipValid = true;

  char msg[160];
  std::snprintf(msg, sizeof msg, "copied the %s grid: a %d x %d lattice, %d point and handle values at "
                "frame %.0f", gridFamilyWord(family), rows, cols, values, t);
  logGridNote(msg);
  return values;
}

int GridWrapPlugin::pasteGrid(int family, double t)
{
  if (!_clipValid || _clip.empty()) {
    logGridNote("paste refused: nothing has been copied yet");
    return -1;
  }

  const int rows = topoRowsNow(), cols = topoColsNow();
  if (rows != _clipRows || cols != _clipCols) {
    char msg[240];
    std::snprintf(msg, sizeof msg,
                  "paste refused: the clipboard is a %d x %d lattice and this node's grid is %d x %d; "
                  "change rows/cols, or copy again", _clipRows, _clipCols, rows, cols);
    logGridNote(msg);
    return -1;
  }

  GridKnobs& knobs = (family == 0) ? _srcKnobs : _dstKnobs;

  int planned = 0;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const gw::FracPoint& fp = _clip[static_cast<std::size_t>(r) * cols + c];
      for (int h = -1; h < gw::kHandleCount; ++h) {
        OFX::Double2DParam* knob = latticeKnob(family, r, c, h);
        if (!knob) continue;
        double x = 0.0, y = 0.0;
        fracPair(fp, h, x, y);
        if (!pairAtTimeMatches(knob, x, y, t)) ++planned;
      }
    }
  }
  if (planned == 0) {
    char msg[200];
    std::snprintf(msg, sizeof msg, "paste onto the %s grid changed nothing: the clipboard already matches "
                  "this node's %d x %d lattice at frame %.0f", gridFamilyWord(family), rows, cols, t);
    logGridNote(msg);
    return 0;   // a paste that changes nothing: no keys, no undo step
  }

  int wrote = 0, failed = 0;
  beginEditBlock("Grid Paste");
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const gw::FracPoint& fp = _clip[static_cast<std::size_t>(r) * cols + c];
      for (int h = -1; h < gw::kHandleCount; ++h) {
        OFX::Double2DParam* knob = latticeKnob(family, r, c, h);
        if (!knob) continue;
        double x = 0.0, y = 0.0;
        fracPair(fp, h, x, y);
        if (pairAtTimeMatches(knob, x, y, t)) continue;
        // Keyed only where that knob was already animated: a paste onto a static grid must not silently
        // make it an animated one (see `pasteGrid`'s declaration).
        const bool keyed = knobKeyCount(knob) > 0;
        const int rslt = writePairAtTime(knob, x, y, t, keyed);
        if (rslt == 1) ++wrote;
        else ++failed;
      }
    }
  }
  endEditBlock();

  char msg[200];
  if (failed)
    std::snprintf(msg, sizeof msg, "paste onto the %s grid wrote %d of %d values; this host refused the "
                  "rest", gridFamilyWord(family), wrote, planned);
  else
    std::snprintf(msg, sizeof msg, "pasted onto the %s grid: %d point and handle values at frame %.0f",
                  gridFamilyWord(family), wrote, t);
  logGridNote(msg);
  return wrote;
}

int GridWrapPlugin::setGridKeys(int family, double t)
{
  GridKnobs& knobs = (family == 0) ? _srcKnobs : _dstKnobs;
  const int rows = topoRowsNow(), cols = topoColsNow();

  int planned = 0, unknown = 0;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      for (int h = -1; h < gw::kHandleCount; ++h) {
        OFX::Double2DParam* knob = latticeKnob(family, r, c, h);
        if (!knob) continue;
        const int has = knobHasKeyAt(knob, t);
        if (has == 1) continue;      // already keyed at this frame: nothing to add
        if (has < 0) ++unknown;
        else ++planned;
      }
    }
  }
  if (unknown) {
    logGridNote("set_keys refused: this host would not say whether the grid is animated");
    return -1;
  }
  if (planned == 0) {
    char msg[200];
    std::snprintf(msg, sizeof msg, "set_keys on the %s grid added no keys: every value already had one "
                  "at frame %.0f", gridFamilyWord(family), t);
    logGridNote(msg);
    return 0;   // every value already has a key here: no edit block
  }

  int keyed = 0, failed = 0;
  beginEditBlock("Grid Set Keys");
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      for (int h = -1; h < gw::kHandleCount; ++h) {
        OFX::Double2DParam* knob = latticeKnob(family, r, c, h);
        if (!knob) continue;
        if (knobHasKeyAt(knob, t) == 1) continue;
        if (keyPairAtTime(knob, t) == 1) ++keyed;
        else ++failed;
      }
    }
  }
  endEditBlock();

  char msg[200];
  if (failed)
    std::snprintf(msg, sizeof msg, "set_keys on the %s grid keyed %d of %d values at frame %.0f; this "
                  "host refused the rest", gridFamilyWord(family), keyed, planned, t);
  else
    std::snprintf(msg, sizeof msg, "set_keys on the %s grid keyed %d point and handle values at frame "
                  "%.0f", gridFamilyWord(family), keyed, t);
  logGridNote(msg);
  return keyed;
}

int GridWrapPlugin::deleteGridKeys(int family, double t)
{
  GridKnobs& knobs = (family == 0) ? _srcKnobs : _dstKnobs;
  const int rows = topoRowsNow(), cols = topoColsNow();

  int planned = 0, unknown = 0;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      for (int h = -1; h < gw::kHandleCount; ++h) {
        OFX::Double2DParam* knob = latticeKnob(family, r, c, h);
        if (!knob) continue;
        const int has = knobHasKeyAt(knob, t);
        if (has == 1) ++planned;   // a key at this frame; `knobHasKeyAt` already proved keys exist
        else if (has < 0) ++unknown;
      }
    }
  }
  if (unknown) {
    logGridNote("delete_keys refused: this host would not say whether the grid is animated");
    return -1;
  }
  if (planned == 0) {
    char msg[200];
    std::snprintf(msg, sizeof msg, "delete_keys on the %s grid removed no keys: none was keyed at frame "
                  "%.0f", gridFamilyWord(family), t);
    logGridNote(msg);
    return 0;   // nothing keyed at this frame: no edit block
  }

  int removed = 0;
  beginEditBlock("Grid Delete Keys");
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      for (int h = -1; h < gw::kHandleCount; ++h) {
        OFX::Double2DParam* knob = latticeKnob(family, r, c, h);
        if (!knob) continue;
        if (knobHasKeyAt(knob, t) != 1) continue;
        if (deleteKeyAtTime(knob, t)) ++removed;
      }
    }
  }
  endEditBlock();

  char msg[200];
  if (removed < planned)
    std::snprintf(msg, sizeof msg, "delete_keys on the %s grid removed %d of %d keys at frame %.0f; "
                  "this host would not remove the rest", gridFamilyWord(family), removed, planned, t);
  else
    std::snprintf(msg, sizeof msg, "delete_keys on the %s grid removed %d keys at frame %.0f",
                  gridFamilyWord(family), removed, t);
  logGridNote(msg);
  return removed;
}

// ---------------------------------------------------------------------------
// The reconciliation itself
// ---------------------------------------------------------------------------
void GridWrapPlugin::syncGridTopology()
{
  if (_rewriting) return;   // a write we made, arriving back as a change (Natron does this)

  const int rows = rowsNow(), cols = colsNow();
  const int stampR = topoRowsNow(), stampC = topoColsNow();

  if (stampR == rows && stampC == cols) {
    syncGridVisibility();   // usually a no-op: the topology has not moved since the last ask
    return;
  }

  if (_topoStuck && _stuckRows == rows && _stuckCols == cols) {
    // This exact change was already attempted and refused. The render's resample is what answers it;
    // retrying the same 640-knob rewrite on every frame would only make the render slower.
    syncGridVisibility();
    return;
  }

  GridKnobs* grids[2] = { &_srcKnobs, &_dstKnobs };
  std::set<double> times[2];
  bool keyed[2] = { false, false };
  bool tooMany = false;

  for (int g = 0; g < 2; ++g) {
    collectKeyTimes(*grids[g], times[g]);
    keyed[g] = !times[g].empty();
    if (times[g].size() > std::size_t(kMaxRewriteKeys)) tooMany = true;
  }

  if (tooMany) {
    logGridNote("grid animation has more keys than this plugin re-keys on a rows/cols change: "
                "the keyframes are kept as they are and the lattice is resampled at render time");
    _topoStuck = true;
    _stuckRows = rows;
    _stuckCols = cols;
    syncGridVisibility();
    return;
  }

  // One undo block for the whole rewrite: a user who changes `rows` and does not like the result gets one
  // step back, not a lattice half-restored. The Support library ignores the suite status here, so these
  // two calls cannot throw and cannot leave the block open.
  _rewriting = true;
  beginEditBlock("Grid Topology");

  bool ok = true;
  for (int g = 0; g < 2; ++g) {
    if (!rewriteGridKnobs(*grids[g], stampR, stampC, rows, cols, times[g], keyed[g])) ok = false;
  }

  if (ok) {
    // Stamps last, and both of them in the same block.
    try {
      _topoRowsKnob->setValue(rows);
      _topoColsKnob->setValue(cols);
    } catch (const std::exception&) {
      ok = false;   // a host that will not take the stamp keeps the node declaring the old topology
    }
  }

  endEditBlock();
  _rewriting = false;

  if (!ok) {
    logGridNote("a rows/cols change could not be written back to this host's knobs: "
                "the lattice is resampled at render time instead");
    _topoStuck = true;
    _stuckRows = rows;
    _stuckCols = cols;
  }

  syncGridVisibility();
}

////////////////////////////////////////////////////////////////////////////////
// changedParam / syncPrivateData
/* Only two things need reconciling here. `grid bounds` is out of it by construction: a fraction is
 * stated relative to whatever the basis currently is (D-62), so changing the basis re-rects the grid
 * without a single knob changing — which is also why this plugin has no `changedClip` override at all,
 * where the other ones have something to rebuild when a clip changes.
 *
 * The other one is the pair of panel toggles (D-81): they change nothing this plugin renders, and what
 * they move is which lattice knobs the host is currently listing plus which grid the Viewer overlay lets
 * the pen pick (D-84) — so the reconciliation is the visibility ask and nothing else.
 *
 * `syncPrivateData` is the belt on the braces: a host that restores a script, or that lets a user type a
 * `rows` value straight into a script, may never report the change to us, and `kOfxActionSyncPrivateData`
 * is the one action a host promises before it renders what it just loaded. */

void GridWrapPlugin::changedParam(const OFX::InstanceChangedArgs& args, const std::string& paramName)
{
  if (_rewriting) return;   // our own setValue, routed back at us

  const int btn = gridButtonIndex(paramName);
  if (btn >= 0)
  {
    /* A button press is a request for an action, not a knob whose value the next render should read, so
     * it is handled here and nowhere else — and only for the reason a press actually arrives as. Natron
     * reports every other reason (its own knob refresh, a project load) as `kOfxChangePluginEdited`, and
     * answering those would run a grid action on the host's schedule instead of the user's. */
    if (args.reason != OFX::eChangeUserEdit)
      return;

    const int action = btn / gw::kFamilyCount;
    const int family = btn % gw::kFamilyCount;
    switch (action)
    {
    case kBtnCopy:       copyGrid(family, args.time);       break;
    case kBtnPaste:      pasteGrid(family, args.time);      break;
    case kBtnSetKeys:    setGridKeys(family, args.time);    break;
    case kBtnDeleteKeys: deleteGridKeys(family, args.time); break;
    default: break;
    }
    return;
  }

  if (paramName == "show_source_grid" || paramName == "show_destination_grid")
  {
    syncGridVisibility();   // the toggles are read there, and only there
    return;
  }

  if (paramName != "rows" && paramName != "cols" &&
      paramName != "grid_topo_rows" && paramName != "grid_topo_cols")
    return;   // every other knob is read afresh by the next render, and needs no rewrite

  syncGridTopology();
}

void GridWrapPlugin::syncPrivateData()
{
  syncGridTopology();
}

////////////////////////////////////////////////////////////////////////////////
// isIdentity
/* Three claims, and each one is a fact about what the core's composition does, read off the same
 * `WarpRender` the render would build (D-59, D-61):
 *
 *   1. `isSourceCopy()` — `mix == 0` with the source grid being the grid the render samples through,
 *      `background = on src`, `background_mix == 0` and `inject` off. Then the warped sample *is* the
 *      source pixel at the output pixel (render.cpp's `_srcCopy`, which is a nearest read, not a gather),
 *      the uncovered pixels are filled with that same pixel, and the mask dissolve and the `channels`
 *      merge-back both dissolve a value towards itself. Covered or not, whatever a mask says, every
 *      channel comes out the Source pixel it started at, which is why the core's predicate does not ask
 *      about `channels`, `fringe`, `maskInvert` or a connected Mask at all.
 *      `background_mix` is excluded because dissolving a value towards an equal value is not the same
 *      *float* the host would have handed back, and a host that takes this claim hands back its input
 *      image untouched.
 *   2. `maskInvert` with nothing on Mask: an unconnected mask is fully on (D-29), inverted that is fully
 *      *off*, and the dissolve's base — the Source pixel at this output pixel — is what every channel
 *      takes. `inject` is the one thing that would write something the Source does not have.
 *   3. `channels == none`: the merge-back takes every channel from the base, which is again the Source
 *      pixel at the output pixel. Same `inject` exception.
 *
 * What is *not* claimed: a render that happens to look like its input. `warp` pinned so both grids are
 * equal, `output = source` with `background = on bg`, a frame that is entirely flat — every one of those
 * costs a grid build and a plan to find out, and the claim is meant to cost less than the render it
 * avoids. The core's own copy paths (`_identity`, `_srcCopy`, `_dstCopy`) already make them cheap. */

bool GridWrapPlugin::decideIdentity(double t, OFX::Clip*& chosen)
{
  if (!_srcClip || !_srcClip->isConnected()) return false;   // there is nothing to pass through

  const gw::WarpRender wr = makeRender(t);
  const gw::Options& opt = wr.options();

  if (wr.isSourceCopy()) {
    chosen = _srcClip;
    return true;   // the same frame, the same pixels, unchanged
  }

  if (opt.inject) return false;   // it writes the mask value into alpha: not the Source's own pixel

  if (opt.maskInvert && !maskConnected()) {
    chosen = _srcClip;
    return true;
  }

  if (!opt.sel[0] && !opt.sel[1] && !opt.sel[2] && !opt.sel[3]) {
    chosen = _srcClip;
    return true;
  }

  return false;
}

bool GridWrapPlugin::isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                                double& identityTime)
{
  // A local of ours, written to the host's references only on a true verdict — which is what the frozen
  // Support action does as well (ofxsImageEffect.cpp's `isIdentityAction` keeps its own `identityClip`
  // local and sets the out-args only when the verdict is true). So a false verdict leaves the host's
  // clip null no matter which clip this helper read on the way to saying no.
  OFX::Clip* chosen = nullptr;
  const bool verdict = decideIdentity(args.time, chosen);
  if (verdict) {
    identityClip = chosen;
    identityTime = args.time;
  }
  OFXLAB_STAMP(stampIdentity(args.time, verdict, chosen));
  return verdict;
}

////////////////////////////////////////////////////////////////////////////////
// RoD / RoI
/* The RoD is "where this node can write": the operative grid's own footprint — the forward-mapped
 * lattice's cell bounds, unioned with the rect it was resolved against, because the copy modes write
 * input pixels at their own positions — plus the region of definition of every input whose pixels a
 * render can *put in the output*: Source (the base of the mask dissolve, the merge-back, and
 * `background = on src`), Dest (`background = on dst` and the morph), Background (`on bg`). An input
 * that contributes pixels widens what the node can write, so it belongs on the RoD; the Mask limits what
 * the node writes and never adds a pixel, so it does not (D-61). A request may be bigger than what a
 * render writes; an RoD smaller than the host's own frame is a host clipping output the plugin would
 * have filled, which is why an input whose extent the host cannot bound makes the honest answer `false`.
 *
 * The RoI is the other direction, and it is answered by the same object: `sourceRequest`/`destRequest`
 * take the render window and return the canonical rect the gather reads, carrying each used cell's own
 * gather pad *and* the whole-pixel rounding (render.h). Flooring and ceiling that rect is all this
 * plugin adds. Background and Mask are nearest-read inputs — a background pixel is read at an output
 * pixel, a mask value is read at an output pixel — so they are asked for exactly the window, with no
 * pad, which is what tests/gw_render_test.cpp's request section measures against what a render actually
 * touches. */

bool GridWrapPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments& args, OfxRectD& rod)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  const gw::WarpRender wr = makeRender(args.time);

  gw::Rect acc = wr.footprint();

  OFX::Clip* clips[3] = { _srcClip, _dstClip, _bgClip };
  ClipRod got[3] = { kClipRodNone, kClipRodNone, kClipRodNone };
  gw::Rect rods[3];
  bool unknown = false;
  // Collect, then decide. An input whose extent the host cannot bound is an answer we refuse, and the
  // stamp has to record *which* one made us refuse it; returning from inside the loop would erase the
  // other two readings and the grid's own basis (D-71).
  for (int i = 0; i < 3; ++i) {
    got[i] = clipRod(clips[i], args.time, rods[i]);
    if (got[i] == kClipRodNone) continue;              // not connected: contributes nothing
    if (got[i] != kClipRodRect) { unknown = true; continue; } // connected and unbounded: we do not know either
    acc = gw::Rect::unite(acc, rods[i]);
  }

  const bool bounded = !unknown && !acc.empty();       // nothing on the node is bounded, and neither is the grid
  if (bounded) {
    rod.x1 = acc.x0;
    rod.y1 = acc.y0;
    rod.x2 = acc.x1;
    rod.y2 = acc.y1;
  }
  // The basis line re-derives what `buildGrids` used a moment earlier, from the Source RoD this loop has
  // already in hand: `makeRender` → `gridBasis` asked the host that question once, and a host caches the
  // answer, so asking it again is a lookup, not a second measurement (D-68/D-71).
  OFXLAB_STAMP(stampRod(args.time, wr, got, rods,
                         gridBasisFrom(args.time, rods[0], got[0] == kClipRodRect), bounded, acc));
  return bounded;
}

void GridWrapPlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args,
                                          OFX::RegionOfInterestSetter& rois)
{
  // `args.regionOfInterest` *is* the window this call is being asked about
  // (RegionsOfInterestArguments carries time / renderScale / regionOfInterest and nothing else), so the
  // window the core is handed is the window the render that follows will be given.
  const gw::Rect window = rectFrom(args.regionOfInterest);

  const gw::WarpRender wr = makeRender(args.time);
  const bool maskConnected = _maskClip && _maskClip->isConnected();

  /* Ask each connected input for exactly the pixels this render can read, and for nothing at all where it
   * reads none (D-64). "Nothing" has to be *said*, because HostSupport answers an unset request with the
   * render window itself (ofxhImageEffect.cpp:1966-1979 — it pre-fills that default for every connected
   * clip, then reads its own default back when the effect leaves the property alone): silence here means
   * "the whole window". The core's four RoI getters give both answers directly — the gather window with the
   * pad the filter owes where the input is read, an empty rect at this window's own corner where it is not,
   * and Natron drops an empty answer from the RoI map altogether (OfxEffectInstance.cpp:1607-1637).
   *
   * What makes this a rule and not an optimisation is what a needless request costs. Natron answers a
   * clip's image request by scanning the images it has already rendered for this effect for the first key
   * that matches — and that key carries no input number (ImageKey.h:41-57; EffectInstance.cpp's
   * getImageFromCacheAndConvertIfNeeded scans args.inputImagesList ignoring which input it came from), so
   * any input this node asks for is one the host may answer with another input's pixels. Probe12 measured
   * exactly that: a GridWrap with Source and Dest on two readers of the same shape wrote the *Source*'s
   * picture into a render that reads only the Dest. An input asked for and not read buys nothing and can
   * only deliver the wrong pixels. */
  OFX::Clip* clips[4] = { _srcClip, _dstClip, _bgClip, _maskClip };
  const gw::Rect asks[4] = { wr.sourceRoI(window, maskConnected),
                             wr.destRoI(window, maskConnected),
                             wr.backgroundRoI(window, maskConnected),
                             wr.maskRoI(window, maskConnected) };
  for (int i = 0; i < 4; ++i) {
    if (!clips[i] || !clips[i]->isConnected()) continue;
    rois.setRegionOfInterest(*clips[i], rectRequest(asks[i]));
  }

  const bool asked[4] = { clips[0] && clips[0]->isConnected(), clips[1] && clips[1]->isConnected(),
                          clips[2] && clips[2]->isConnected(), clips[3] && clips[3]->isConnected() };
  OFXLAB_STAMP(stampRoi(args.time, wr, window, asked, asks));
}

////////////////////////////////////////////////////////////////////////////////
// render
/* The plumbing, in the order the rules arrive:
 *
 *   1. float32 only, 1..4 interleaved channels (D-07). RGB is a legitimate render here: the core reads
 *      `nchans` channels of every pixel it touches in order, so an RGB render is r, g, b and a channel
 *      an input does not have reads 0, and `inject` has no channel to overwrite (render.h's
 *      `alphaIndex`).
 *   2. The grids and the knobs are read *first*, because the knobs decide which inputs this render can
 *      read at all, and only those are fetched — each one fetched and *validated here, on this thread*,
 *      before a single row is handed to a host thread, so a depth or component set this plugin does not
 *      render leaves through `kOfxStatErrUnsupported` out of an entry point rather than out of a worker
 *      thread where an exception cannot be translated (AGENTS.md rule 1, D-55). Not fetching an input the
 *      render does not read is the second half of the rule stated in getRegionsOfInterest above: asking for
 *      an input is what lets Natron hand this node another input's already-rendered image (D-64).
 *   3. The grids are read once, at one time, and the windows are read once. What the threads share after
 *      that is read-only state; what each thread builds is its own `gw::WarpRender`, because the
 *      inversion lattice's `find` reuses candidate scratch (grid.h) and so is not shareable.
 *   4. Rows are written through `Image::getPixelAddress`, which is bounds-checked, per-row safe for any
 *      stride a host chose (including a negative one), and returns null for a row the host did not
 *      allocate — a host that clipped our window to its image is a host that gets the rows it asked for. */

void GridWrapPlugin::render(const OFX::RenderArguments& args)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  if (!_outClip) OFX::throwSuiteStatusException(kOfxStatFailed);

  gw::Grid srcGrid, dstGrid;
  buildGrids(args.time, srcGrid, dstGrid);

  gw::Options opt;
  readOptions(args.time, opt);

  const gw::WarpRender wr(srcGrid, dstGrid, opt);
  const bool maskConnected = _maskClip && _maskClip->isConnected();

  // Which of the four inputs can reach a delivered pixel, in this knob state. An input that cannot is not
  // fetched: its pixels would be asked of the host (getRegionsOfInterest already answered it with nothing),
  // and an input this node asks for is an input the host may answer with a *different* input's image.
  const bool reads[4] = { wr.readsSource(maskConnected), wr.readsDest(maskConnected),
                          wr.readsBackground(maskConnected), wr.readsMask(maskConnected) };

  std::unique_ptr<OFX::Image> dst(_outClip->fetchImage(args.time));
  if (!dst) OFX::throwSuiteStatusException(kOfxStatFailed);

  if (dst->getPixelDepth() != OFX::eBitDepthFloat)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);   // float32-first, v1 (D-07)

  const int nchans = dst->getPixelComponentCount();
  if (nchans < 1 || nchans > 4)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);   // RGBA / RGB / Alpha (D-57)

  OFX::Clip* clips[4] = { _srcClip, _dstClip, _bgClip, _maskClip };
  std::unique_ptr<OFX::Image> imgs[4];

  int maskComp = 0;   // a mask is read as one component: alpha of an RGBA mask, otherwise channel 0

  for (int i = 0; i < 4; ++i) {
    if (!reads[i]) continue;
    if (!clips[i] || !clips[i]->isConnected()) continue;

    imgs[i].reset(clips[i]->fetchImage(args.time));
    if (!imgs[i]) continue;   // nothing at this time: the window stays empty, and reads 0

    if (imgs[i]->getPixelDepth() != OFX::eBitDepthFloat)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    const OFX::PixelComponentEnum comp = imgs[i]->getPixelComponents();
    if (comp != OFX::ePixelComponentRGBA && comp != OFX::ePixelComponentRGB &&
        comp != OFX::ePixelComponentAlpha)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    if (i == 3 && comp == OFX::ePixelComponentRGBA) maskComp = 3;   // the same choice sharpenz makes (D-20)
  }

  // The four windows (three gatherable, one nearest) and their copy buffers, all on this thread: a
  // `scratch` a window points into has to outlive the render, which it does — it is declared here, next
  // to the images it was filled from.
  std::unique_ptr<OFX::Image> srcImg(std::move(imgs[0]));
  std::unique_ptr<OFX::Image> dstImg(std::move(imgs[1]));
  std::unique_ptr<OFX::Image> bgImg(std::move(imgs[2]));
  std::unique_ptr<OFX::Image> maskImg(std::move(imgs[3]));

  std::vector<float> srcScratch, dstScratch, bgScratch;
  InputWindow srcWin, dstWin, bgWin;
  srcWin.bind(srcImg.get(), nchans, srcScratch);
  dstWin.bind(dstImg.get(), nchans, dstScratch);
  bgWin.bind(bgImg.get(), nchans, bgScratch);

  gw::RenderInputs inputs;
  inputs.src = srcWin.window();
  inputs.dst = dstWin.window();
  inputs.bg = bgWin.window();

  // An unconnected Mask is `active() == false`, which `MaskReader` answers as fully on (D-29), and that
  // is what `inject` then writes: 1 into alpha, everywhere. An input that was skipped above arrives here as
  // a null image, which is the same "nothing at this time" case the core already answers as reading 0.
  OfxMaskSampler mask;
  mask.set(maskImg.get(), maskComp);

  GridWrapProcessor processor(*this, srcGrid, dstGrid, opt, inputs, mask);
  processor.setDstImg(dst.get());
  processor.setRenderWindow(args.renderWindow);
  processor.process();

  // Stamped last, after the rows are written, so the Output hashes describe the pixels a host was handed
  // rather than the buffer before it was filled (D-71).
  const gw::Rect window = rectFrom(args.renderWindow);
  OFX::Image* const raw[4] = { srcImg.get(), dstImg.get(), bgImg.get(), maskImg.get() };
  OFXLAB_STAMP(stampRender(args.time, wr, window, reads, raw, dst.get(), nchans, maskComp));
}

////////////////////////////////////////////////////////////////////////////////
// the Viewer overlay: the lattice, drawn and dragged (D-78)
//
// The whole point of this class is that the grid becomes something you *edit by hand* in the Viewer
// instead of only through 640 knob slots (the accepted limitation D-53 records). It is a thin shell on
// purpose: what a click means, what a drag writes, which control a press grabs, how big a grab is at
// the current zoom, and where a curve actually bends are all decided in `interact.{h,cpp}`, which sees
// neither OFX nor GL nor a host. What lives here is the two things only the plugin side can do — read
// the knobs and paint the picture.
//
// Coordinates, because getting them wrong puts every marker in the wrong place: `ViewerGL.cpp:321` sets
// up the Viewer with `glOrtho(left, right, bottom, top, …)` and an identity modelview, `Gui/ZoomContext.h`
// works in the same canonical image space rooted at the format's 0,0, and the pen position Natron hands
// the interact is that space (`_imp->zoomCtx.toZoomCoordinates(…)`). So a `gw::Affine` built from the
// basis plus the transform knobs maps canonical → canonical, and this class applies **no** transform of
// its own: the positions `latticeEdges` / `latticePoints` / `handleArms` return are drawn as they are. The
// only place screen space enters is the hit tolerance, which is a pixel count and so is divided by
// `pixelScale` — a per-axis division, because the Viewer's x and y scales differ whenever a plate's PAR
// and the Viewer's aspect do not agree.
//
// There is no bounds to set either: OFX 1.4 has no interact bounds action and HostSupport carries no
// such property, so the region a click may start a drag in is entirely the host's business (D-78). What
// this class can decide is what a click *lands on*, and a press that lands on nothing returns false so
// the Viewer keeps its pan and zoom.

namespace {

// Per-family hue. Two saturated-but-not-bright colours: bright enough to read on Natron's dark Viewer
// background, dark enough to read on a white plate, and far apart in hue so which lattice is being
// edited is obvious without needing an overlay label a host does not have to draw. The selected control
// is drawn in the host's suggested colour, which is white on Natron (`ofxhInteract.cpp:125`) — see
// `accentColour` for why that suggestion is checked against the plate rather than taken on trust.
void familyTint(int family, double out[3])
{
  if (family == gw::kFamilyDst) { out[0] = 1.00; out[1] = 0.62; out[2] = 0.25; }
  else                          { out[0] = 0.35; out[1] = 0.72; out[2] = 1.00; }
}

// The same hue, dimmed. Alpha is always 1.0 because the overlay's colour state is one of the four
// attrib groups Natron restores, but a translucent stroke over a plate would pick up whichever colour
// the Viewer drew before it, so the emphasis is brightness, not coverage.
void scaledColour(const double rgb[3], double k, double low, double out[3])
{
  for (int i = 0; i < 3; ++i)
  {
    double v = rgb[i] * k;
    out[i] = v < low ? low : v;
  }
}

// One component of a colour the host reported. `OfxRGBColourD` is a plain struct of three doubles in OFX
// 1.4 (`ofxPixels.h:80-83`) — not a union, not an array — so a component is named, not indexed, and a
// missing or non-finite one reads as mid-grey: neither of the extremes the contrast rule below chases.
double rgbComponent(OfxRGBColourD c, int i)
{
  const double v = i == 0 ? c.r : (i == 1 ? c.g : c.b);
  if (!std::isfinite(v))
    return 0.5;
  return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// Rec-709 luma: enough to answer "is the plate this overlay is drawn on dark or bright", which is the
// only question the two rules below ask.
double rgbLuminance(OfxRGBColourD c)
{
  return 0.2126 * rgbComponent(c, 0) + 0.7152 * rgbComponent(c, 1) + 0.0722 * rgbComponent(c, 2);
}

// The colour to draw a grabbed control in. `getSuggestedColour`'s bool means the host *has* a colour for
// overlays, not that the colour separates from whatever the Viewer is showing — Natron answers the
// property with a fixed white (`ofxhInteract.cpp:125`) regardless of the plate. So the suggestion is kept
// only where it is actually a contrast against the background, and otherwise the overlay picks the
// opposite extreme: near-black on a bright plate, white on a dark one (D-78).
void accentColour(bool hasSuggested, OfxRGBColourD suggested, OfxRGBColourD background, double out[3])
{
  const double bgLuma = rgbLuminance(background);
  if (hasSuggested && std::fabs(rgbLuminance(suggested) - bgLuma) >= 0.25)
  {
    for (int i = 0; i < 3; ++i)
      out[i] = rgbComponent(suggested, i);
    return;
  }
  const double v = bgLuma > 0.5 ? 0.05 : 1.0;
  out[0] = v; out[1] = v; out[2] = v;
}

// One cell boundary, stroked as the cubic it is rather than as a chord between its endpoints: a grid
// dragged by hand is judged by how its lines bend, and straight segments between lattice points would
// show a straight line where the render produces a curve. `flattenCubic` is the core's, so the polyline
// drawn here is subdivision of the same four controls `edgeCubic` integrates when it warps pixels —
// same curve, two consumers (D-78).
void strokeDrawnCubic(const gw::Vec cp[4], double tolerance)
{
  gw::Vec buf[64];
  const int n = gw::flattenCubic(cp, tolerance, buf, 64);
  gw::gl::begin(gw::gl::kLines);
  for (int i = 1; i < n; ++i)
  {
    gw::gl::vertex(buf[i - 1]);
    gw::gl::vertex(buf[i]);
  }
  gw::gl::end();
}

// A non-positive or non-finite `pixelScale` would turn the hit test's unit into infinity, so an absent
// scale is read as 1:1 rather than as "everything is grabbable".
gw::Vec pixelScaleSafe(OfxPointD ps)
{
  double x = std::isfinite(ps.x) && ps.x > 0.0 ? ps.x : 1.0;
  double y = std::isfinite(ps.y) && ps.y > 0.0 ? ps.y : 1.0;
  return gw::Vec(x, y);
}

// The overlay's one modifier. Only the two Control keys count: the platform's Command key arrives through
// HostSupport as `kOfxKey_Meta_L`/`_R` on macOS (D-83), and a third key would mean a chord the gesture
// rules do not describe.
bool isCtrlKey(int keySymbol)
{
  return keySymbol == kOfxKey_Control_L || keySymbol == kOfxKey_Control_R;
}

} // namespace

/** @brief The interact itself: one per GridWrap instance, created by the Support library through the
 * descriptor below, self-registering with its own effect (`ofxsInteract.cpp:305-313`).
 *
 * State is deliberately minimal, and what state there is belongs to the *gesture*, not to the grid: the
 * selection is re-derived from the knobs at every event (see `build`), so a knob edited from the panel,
 * an undo, or another Viewer's drag cannot leave this class holding a stale lattice. The grab remembers
 * the knob slot it writes rather than a knob name, because the slot is what both families and the four
 * handles of a point are distinguished by (`latticeKnob`).
 */
class GridWrapOverlay : public OFX::OverlayInteract
{
public:
  GridWrapOverlay(OfxInteractHandle handle, OFX::ImageEffect* effect)
    : OFX::OverlayInteract(handle),
      _plugin(dynamic_cast<GridWrapPlugin*>(effect ? effect : _effect)),
      _sel(),
      _grab(),
      _flipSel(),
      _deselect(),
      _penStart(),
      _ctrl(false),
      _dragging(false),
      _editing(false),
      _moved(false),
      _hasFlip(false),
      _glWarned(false)
  {
    if (!_plugin)
      logGridNote("overlay: the host did not hand the interact a GridWrap instance");
  }

  ~GridWrapOverlay() override { endEditBlockSafely(); }

  bool draw(const OFX::DrawArgs& args) override
  {
    if (!_plugin)
      return false;
    if (!gw::gl::available())
    {
      // The one sentence this feature can print about itself, and only once: the loader caches its
      // failure, so a Viewer that repaints at the drag rate must not log per frame.
      if (!_glWarned)
      {
        _glWarned = true;
        const std::string why = gw::gl::refusalReason();
        logGridNote(why.empty() ? "overlay: this host offers no GL to draw the lattice with" : why.c_str());
      }
      return false;
    }

    Scene sc;
    if (!build(args.time, sc))
      return false;   // no basis to draw on: see `build`

    const gw::Interactor in(sc.src, sc.dst, sc.basis, sc.srcAf, sc.dstAf);
    _sel.clampTo(in.rows(), in.cols());

    // A point's hit radius, in canonical units, for the ring this frame draws around the selected points
    // — the same `hitRadiusFor` the hit test uses, so what you see is the size of the grab it stands for,
    // including when a lattice is dense enough that the spacing, not the 8 px, decides. The visibility
    // flags ride along so the ring describes the grab this grid is actually offering (D-84).
    const gw::Vec ps = pixelScaleSafe(args.pixelScale);
    const gw::Vec radius = gw::hitRadiusFor(
        gw::HitOptions(gw::kInteractHitPixels, ps.x, ps.y, sc.showSrc, sc.showDst), in.latticeSpacing());

    // Background first, active family last, so the family being edited is the one on top.
    int active = _sel.valid() ? _sel.family : gw::kFamilySrc;
    for (int pass = 0; pass < 2; ++pass)
      for (int family = 0; family < gw::kFamilyCount; ++family)
      {
        if ((family == active) != (pass == 1))
          continue;
        drawFamily(in, sc, family, radius, args);
      }
    return true;
  }

  bool penDown(const OFX::PenArgs& args) override
  {
    if (!gw::gl::available())
      return false;
    if (!_plugin || _plugin->_rewriting)
      return false;   // mid-rewrite the knob set is not a consistent thing to grab

    Scene sc;
    if (!build(args.time, sc))
      return false;
    const gw::Interactor in(sc.src, sc.dst, sc.basis, sc.srcAf, sc.dstAf);
    _sel.clampTo(in.rows(), in.cols());

    const gw::Vec ps = pixelScaleSafe(args.pixelScale);
    const gw::HitOptions opt(gw::kInteractHitPixels, ps.x, ps.y, sc.showSrc, sc.showDst);
    gw::Control cands[gw::kInteractMaxCandidates];
    const int n = in.hitCandidates(gw::Vec(args.penPosition.x, args.penPosition.y), _sel, opt,
                                   cands, gw::kInteractMaxCandidates);
    if (n <= 0)
      return false;   // nothing here: the Viewer keeps this press for pan / zoom

    _grab = cands[0];

    // What a press does to the selection (D-85). Grabbing a handle arm leaves it exactly as it was: an arm
    // belongs to a point that is already selected, and an arm drag writes the one point it hangs off
    // (D-83). A plain press on a point selects that point, which is what the overlay did before. A press
    // with Ctrl held *extends* the selection with that point — and when the point is already in the set,
    // the set is kept whole for the length of the gesture (a press that grabs a point has to be able to
    // drag it) and the point comes back out at the release, only if nothing was dragged: that is the
    // Ctrl-click-to-deselect gesture, decided at the release for the same reason D-78's flip is, because
    // a host is free to deliver only the press and the release. Ctrl on a point of the *other* family
    // starts a fresh selection there, because one selection is always one family and a drag of that point
    // must not carry the first family's points along with it.
    _deselect = gw::Selection();
    if (_grab.handle < 0)
    {
      const bool sameFamily = _sel.valid() && _sel.family == _grab.family;
      if (_ctrl && sameFamily)
      {
        if (_sel.contains(_grab.row, _grab.col))
          _deselect = gw::Selection(_grab.family, _grab.row, _grab.col);
        else
          _sel.toggle(_grab.family, _grab.row, _grab.col);
      }
      else
        _sel.selectOnly(_grab.family, _grab.row, _grab.col);
    }

    // A src point and a dst point can land on the same pixel, which is exactly the situation a
    // hand-edited grid gets into when a warp starts as a copy. Releasing that press without dragging
    // selects the other family's point (D-78), so one spot in the Viewer reaches both lattices. Holding
    // Ctrl suppresses it: the release of a Ctrl press is already spoken for (it adds or removes a point),
    // and Ctrl has to stay a modifier a user can press without the grid answering something else.
    const gw::Control* twin = gw::Interactor::coincidentTwin(cands, n, _sel);
    _hasFlip = twin != 0 && !_ctrl;
    if (_hasFlip)
      _flipSel = gw::Selection(twin->family, twin->row, twin->col);

    _penStart = gw::Vec(args.penPosition.x, args.penPosition.y);
    _dragging = true;
    _moved = false;
    _editing = false;

    // The handle arms are drawn for the selected points only, so a press that changed the selection has
    // to repaint before the next event draws them.
    requestRedrawSafely();
    return true;
  }

  bool penMotion(const OFX::PenArgs& args) override
  {
    if (!_dragging)
      return false;
    const gw::Vec p(args.penPosition.x, args.penPosition.y);

    // The click-vs-drag decision is made on the *total* travel since the press, and `_penStart` is
    // never advanced here: a gate against the previous motion instead would let a long, slow drag that
    // moved a pixel per event look like a series of clicks and write nothing at all. Returning true
    // below the threshold traps the gesture anyway, which is what lets `penUp` judge the same press.
    Scene sc;
    if (movedEnough(_penStart, p, args) && build(args.time, sc))
    {
      _moved = true;
      applyDrag(args, sc, p);
    }
    return true;
  }

  bool penUp(const OFX::PenArgs& args) override
  {
    if (!_dragging)
      return false;
    const gw::Vec p(args.penPosition.x, args.penPosition.y);

    // The release position goes through the same solve as a motion, because a host is free to deliver
    // only the press and the release: a drag that never reported a motion must still write where the
    // pointer ended, and a press that never moved must still be able to flip the family (D-78) or take a
    // point back out of the selection (D-85).
    Scene sc;
    const bool built = build(args.time, sc);
    if (movedEnough(_penStart, p, args))
    {
      _moved = true;
      if (built)
        applyDrag(args, sc, p);
    }
    else if (_hasFlip)
    {
      _sel = _flipSel;   // a click on a spot two points share walks to the other one
      requestRedrawSafely();
    }
    else if (_deselect.valid())
    {
      _sel.toggle(_deselect.family, _deselect.row(), _deselect.col());   // a Ctrl-click on a selected
                                                                          // point takes it back out
      requestRedrawSafely();
    }

    _dragging = false;
    _hasFlip = false;
    _deselect = gw::Selection();
    _penStart = gw::Vec(0.0, 0.0);
    endEditBlockSafely();
    return true;
  }

  // Ctrl is the overlay's only modifier: it makes a handle drag move just that handle (D-83), and it is
  // what adds and removes points in a multi-point selection (D-85). A plugin overlay is not the keyboard
  // focus, so this is a *latch* and not a chord: the flag goes on at key-down and off at key-up, which is
  // also what lets it survive a host that stops delivering keys mid-press. No other key is read, and both
  // entry points return false, because a host that hands a plugin a key event is handing it to the plugin
  // first — claiming the event would take Ctrl+Z and every other shortcut away from the host's own
  // handler, which is not what a modifier for a grid gesture means.
  bool keyDown(const OFX::KeyArgs& args) override
  {
    if (isCtrlKey(args.keySymbol))
      _ctrl = true;
    return false;
  }

  bool keyUp(const OFX::KeyArgs& args) override
  {
    if (isCtrlKey(args.keySymbol))
      _ctrl = false;
    return false;
  }

private:
  GridWrapOverlay(const GridWrapOverlay&);
  GridWrapOverlay& operator=(const GridWrapOverlay&);

  // The grids, the basis and the two family affines, read from the knobs once per event. All five
  // members are default-constructible, and none of them is the interactor: `gw::Interactor` keeps
  // pointers to the two grids, so it is built per event *on top of* these and never stored here.
  struct Scene {
    gw::Grid src, dst;
    gw::Rect basis;
    gw::Affine srcAf, dstAf;
    bool showSrc, showDst;   // the panel's two checkboxes, read with the rest (D-84)
    Scene() : src(), dst(), basis(), srcAf(), dstAf(), showSrc(true), showDst(true) {}
  };

  bool build(double time, Scene& sc)
  {
    try {
      if (!_plugin || !_plugin->buildOverlayGrids(time, sc.src, sc.dst, sc.basis, sc.srcAf, sc.dstAf))
        return false;
    } catch (const std::exception &) {
      return false;
    } catch (...) {
      return false;
    }

    // A grid whose checkbox is off stays on the panel and off the hit test (D-84), so the two toggles are
    // read here, next to the lattice they gate. A knob that cannot answer leaves its grid grabbable,
    // which is the state the overlay was in before the toggles existed; a knob read failing for any other
    // reason is not a reason to stop drawing the grid, so this never fails the build.
    try {
      if (_plugin->_showSrcGridKnob)
        sc.showSrc = _plugin->_showSrcGridKnob->getValue();
      if (_plugin->_showDstGridKnob)
        sc.showDst = _plugin->_showDstGridKnob->getValue();
    } catch (const std::exception &) {
    } catch (...) {
    }

    // A grid with no area to draw on is a render-time fallback question, not a Viewer question:
    // `resolveGrid`'s unit-square substitute is what keeps a plate finite, and painting a substitute
    // lattice over someone's picture would be the plugin inventing a shape the knobs do not describe.
    const gw::Rect box = gw::Rect::normalised(sc.basis);
    if (box.empty() || !(box.width() > 0.0) || !(box.height() > 0.0))
      return false;
    return true;
  }

  // A family's whole lattice: its cell boundaries as cubics, its handle arms, and a marker per point.
  // Arms are drawn at every point and hittable only at the selected ones (the invariant at the top of
  // interact.h): a lattice whose arms appear only when a point is chosen reads as a list of points
  // until you find the point you wanted, and a lattice with 640 live arms is a Viewer where every press
  // grabs a handle.
  void drawFamily(const gw::Interactor& in, const Scene& sc, int family, const gw::Vec& radius,
                  const OFX::DrawArgs& args)
  {
    double tint[3];
    familyTint(family, tint);
    const bool active  = (family == (_sel.valid() ? _sel.family : gw::kFamilySrc));
    const bool visible = (family == gw::kFamilyDst ? sc.showDst : sc.showSrc);   // D-84

    // How dim a dimmed hue is allowed to be depends on what the Viewer is showing behind it: dim it too
    // far over a dark plate and the two families both collapse into the background. The factor is lifted
    // on a dark plate; the floor stays where it is, because a *raised floor* is what would flatten the
    // hue to grey (D-78).
    const bool dark = rgbLuminance(args.backGroundColour) < 0.35;
    const double kInactive = dark ? 0.72 : 0.38;
    const double kQuad     = dark ? 0.62 : 0.42;

    // A grid the panel has hidden (D-84) goes *further* down this same ladder rather than going unseen:
    // `gw::gl::color` carries an alpha the shim never gets to use, because nothing enables blending, so
    // value is the only dial available. A hidden grid is still a picture — it is the basis the warp
    // resolves through, and D-81's own measurement says a render reads every knob of the stamped
    // topology whichever way the checkboxes sit — but it wears no accent and offers no grab, which is
    // what the checkbox is for.
    const double kHidden     = dark ? 0.30 : 0.14;
    const double kHiddenQuad = dark ? 0.24 : 0.11;

    double base[3], mid[3];
    scaledColour(tint, visible ? (active ? 1.0 : kInactive) : kHidden, 0.10, base);
    scaledColour(tint, visible ? kQuad : kHiddenQuad, 0.10, mid);

    const gw::Rect box = gw::Rect::normalised(in.basis());
    const gw::Affine af = family == gw::kFamilyDst ? sc.dstAf : sc.srcAf;

    // The basis quad, dimmed: what the knob fractions are fractions *of* (D-53). It is drawn as the
    // family affine applied to the canonical corners, because `gw::Rect` is a box in canonical
    // coordinates, not a unit square, and the basis can sit anywhere in the frame.
    gw::Vec corners[4];
    corners[0] = af.map(gw::Vec(box.x0, box.y0));
    corners[1] = af.map(gw::Vec(box.x1, box.y0));
    corners[2] = af.map(gw::Vec(box.x1, box.y1));
    corners[3] = af.map(gw::Vec(box.x0, box.y1));
    gw::gl::color(mid[0], mid[1], mid[2], 1.0);
    gw::gl::lineWidth(1.0);
    gw::gl::begin(gw::gl::kLineLoop);
    for (int i = 0; i < 4; ++i)
      gw::gl::vertex(corners[i]);
    gw::gl::end();

    // Grid lines. Tolerance is a screen distance expressed in canonical units: a quarter of a pixel is
    // finer than the eye can see in the Viewer, and coarser than that is what makes a flat curve bend
    // visibly when you drag its endpoints.
    const double tol = 0.25 * (std::min(radius.x, radius.y));
    const std::vector<gw::DrawnEdge> edges = in.latticeEdges(family);
    gw::gl::color(base[0], base[1], base[2], 1.0);
    gw::gl::lineWidth(active && visible ? 1.5 : 1.0);
    for (std::vector<gw::DrawnEdge>::const_iterator it = edges.begin(); it != edges.end(); ++it)
    {
      if (it->horizontal != true)
        continue;   // the vertical edges are stroked in the same pass, this keeps the loop readable
      strokeDrawnCubic(it->cp, tol);
    }
    for (std::vector<gw::DrawnEdge>::const_iterator it = edges.begin(); it != edges.end(); ++it)
    {
      if (it->horizontal == true)
        continue;
      strokeDrawnCubic(it->cp, tol);
    }

    // Arms, at every point.
    gw::gl::color(base[0], base[1], base[2], 1.0);
    gw::gl::lineWidth(active && visible ? 1.5 : 1.0);
    const std::vector<gw::PolyPoint> pts = in.latticePoints(family);
    for (std::vector<gw::PolyPoint>::const_iterator it = pts.begin(); it != pts.end(); ++it)
      strokeArms(in, family, it->row, it->col, radius);

    // Point markers: a cross at every point, and for each selected point a cross in the accent colour.
    // Only the primary — `points[0]`, the point the selection was made *from* — gets the larger one, so a
    // multi-point set still shows which point a handle drag would belong to (D-85). Not focus-gated — OFX
    // 1.4 has no "is this overlay focused" property, the Support `gainFocus`/`loseFocus` pair is a no-op,
    // and the selection is the state that means something here. A hidden grid wears none of them either:
    // an accent marker advertises a grab, and a hidden grid offers none (D-84).
    OfxRGBColourD suggested;
    const bool hasSuggested = getSuggestedColour(suggested);
    double hi[3];
    accentColour(hasSuggested, suggested, args.backGroundColour, hi);
    for (std::vector<gw::PolyPoint>::const_iterator it = pts.begin(); it != pts.end(); ++it)
    {
      const bool selHere = visible && _sel.family == family && _sel.contains(it->row, it->col);
      const bool primaryHere = selHere && _sel.row() == it->row && _sel.col() == it->col;
      double hx = 0.5 * radius.x, hy = 0.5 * radius.y;
      if (hx <= 0.0) hx = 1.0;
      if (hy <= 0.0) hy = 1.0;
      if (primaryHere) { hx *= 1.6; hy *= 1.6; }
      gw::gl::color(selHere ? hi[0] : base[0], selHere ? hi[1] : base[1],
                    selHere ? hi[2] : base[2], 1.0);
      gw::gl::lineWidth(selHere ? (primaryHere ? 2.0 : 1.5) : 1.0);
      gw::gl::begin(gw::gl::kLines);
      gw::gl::vertex(gw::Vec(it->p.x - hx, it->p.y));
      gw::gl::vertex(gw::Vec(it->p.x + hx, it->p.y));
      gw::gl::vertex(gw::Vec(it->p.x, it->p.y - hy));
      gw::gl::vertex(gw::Vec(it->p.x, it->p.y + hy));
      gw::gl::end();
    }

    // The selected points' arms, re-stroked in the accent colour over the others, and a hit ring at each
    // of them. Every member of the set gets both, because every member of the set is point-grabbable and
    // arm-grabbable (D-85): a ring on the primary alone would advertise a grab only the primary has.
    if (visible && _sel.valid() && _sel.family == family)
    {
      gw::gl::color(hi[0], hi[1], hi[2], 1.0);
      gw::gl::lineWidth(1.5);
      for (int i = 0; i < _sel.count(); ++i)
      {
        const gw::SelectedPoint& sp = _sel.at(i);

        // Its drawn position, taken from the same pass that stroked the crosses: the arms are the
        // transformed ones, so they have to be drawn from where the Viewer put the point (D-78).
        gw::Vec pos(0.0, 0.0);
        bool found = false;
        for (std::vector<gw::PolyPoint>::const_iterator it = pts.begin(); it != pts.end(); ++it)
        {
          if (it->handle < 0 && it->row == sp.row && it->col == sp.col)
          {
            pos = it->p;
            found = true;
            break;
          }
        }
        if (!found)
          continue;   // a stale slot in a rewritten topology: `clampTo` already dropped it, this is the guard

        strokeArms(in, family, sp.row, sp.col, radius);

        // An axis-aligned ellipse rather than a circle, because the hit test is `(dx/rx)^2 + (dy/ry)^2
        // <= 1` with per-axis radii: a circle would advertise a grab the test does not implement on a
        // plate whose PAR is not 1:1.
        strokeHitRing(pos, radius, hi);
      }
    }
  }

  // One point's four handle arms plus the tick at each free end. Colour and width are the caller's, which
  // is what lets the same routine draw the faint arms of a whole lattice and the accent arms of the one
  // selected point. `handleArms` already returns the free end in drawn space, and
  // `Segment.horizontalHandle` names the handle's *axis* (H or U), not the direction the arm happens to
  // point after the family affine, so the tick is oriented from the drawn direction.
  void strokeArms(const gw::Interactor& in, int family, int row, int col, const gw::Vec& radius)
  {
    const std::vector<gw::Segment> arms = in.handleArms(family, row, col);
    const double m = 0.6 * std::min(radius.x, radius.y);
    for (std::vector<gw::Segment>::const_iterator it = arms.begin(); it != arms.end(); ++it)
    {
      gw::gl::begin(gw::gl::kLines);
      gw::gl::vertex(it->a);
      gw::gl::vertex(it->b);
      gw::gl::end();

      if (!(m > 0.0))
        continue;

      // A tick across the free end, across the arm's own direction, so an arm reads as a grab target
      // rather than as a line that simply stops.
      const double dx = it->b.x - it->a.x;
      const double dy = it->b.y - it->a.y;
      gw::gl::begin(gw::gl::kLines);
      if (std::fabs(dx) >= std::fabs(dy))
      {
        gw::gl::vertex(gw::Vec(it->b.x, it->b.y - m));
        gw::gl::vertex(gw::Vec(it->b.x, it->b.y + m));
      }
      else
      {
        gw::gl::vertex(gw::Vec(it->b.x - m, it->b.y));
        gw::gl::vertex(gw::Vec(it->b.x + m, it->b.y));
      }
      gw::gl::end();
    }
  }

  void strokeHitRing(const gw::Vec& c, const gw::Vec& radius, const double rgb[3])
  {
    if (!(radius.x > 0.0) || !(radius.y > 0.0))
      return;
    static const double kPi = 3.14159265358979323846;
    gw::gl::color(rgb[0], rgb[1], rgb[2], 1.0);
    gw::gl::lineWidth(1.0);
    gw::gl::begin(gw::gl::kLineLoop);
    for (int i = 0; i < 24; ++i)
    {
      const double a = (kPi * 2.0 * (double)i) / 24.0;
      gw::gl::vertex(gw::Vec(c.x + radius.x * std::cos(a), c.y + radius.y * std::sin(a)));
    }
    gw::gl::end();
  }

  // Total travel since the press, measured in screen pixels against a quarter-pixel threshold, which is
  // the smallest thing a Viewer's pen can be expected to exceed by accident. Canonical units cancel
  // against `pixelScale`, so the threshold is the same feel at any zoom.
  bool movedEnough(const gw::Vec& from, const gw::Vec& to, const OFX::PenArgs& args) const
  {
    const gw::Vec ps = pixelScaleSafe(args.pixelScale);
    double dx = (to.x - from.x) / ps.x;
    double dy = (to.y - from.y) / ps.y;
    if (!std::isfinite(dx) || !std::isfinite(dy))
    {
      dx = to.x - from.x;
      dy = to.y - from.y;
    }
    return (dx * dx + dy * dy) > 0.25;
  }

  // The OFX call a drag ends on. `dragFor` has already solved the geometry and clamped the result to the
  // declared ±10 range, so what is left is the write, and it is the same shape `writeFracKnob` uses for
  // the panel side of the same knob set.
  //
  // The write is keyed at the event's frame whatever the knob's animation state is (D-80), which is what
  // makes a moved point animated: it adds a key at the frame the drag happened on instead of rewriting
  // the value at every frame, and it does that without the user switching the host's auto-key on first.
  // The gate this replaces asked `getIsAnimating()`, and Natron describes every plugin knob as
  // non-animating (HostSupport `addValueParamProps`, ofxhParam.cpp:475), so on that host the keyed branch
  // was only ever reached by a knob someone had already put a keyframe on by hand.
  //
  // One slot of one press: look the knob up, read it at this frame, and key the new value only when the
  // read says it differs. Returns true when the knob was actually written, which is what `applyDrag`
  // counts on below.
  //
  // `dragFor` reports `changed` for any grabbable control rather than comparing values, so an event that
  // lands on the value the knob already holds is an ordinary event here — `penUp` re-solves the release
  // position whether or not the pen ever passed `movedEnough`, which is how a press that never moved can
  // still flip the family. Keying that value would add a key and change nothing else, so the write is
  // skipped, and with it the edit block: a press that went nowhere leaves nothing in the undo queue, and
  // a mirrored handle that was already the negative of its pair costs no second key.
  bool writeLatticeKnob(const OFX::PenArgs& args, const gw::DragWrite& w)
  {
    OFX::Double2DParam* knob = _plugin->latticeKnob(w.family, w.row, w.col, w.handle);
    if (!knob)
      return false;   // a slot outside the declared 32x32 wall writes no knob, by D-53's rule

    bool differs = true;
    try {
      double oldx = 0.0, oldy = 0.0;
      knob->getValueAtTime(args.time, oldx, oldy);
      differs = std::fabs(oldx - w.value.x) > kKeyEps || std::fabs(oldy - w.value.y) > kKeyEps;
    } catch (const std::exception &) {
      // A host that will not answer the read at this frame is not a reason to drop the gesture: assume
      // it differs, and let the write be accepted or refused on its own merits.
    }
    if (!differs)
      return false;

    if (!_editing)
    {
      // One edit block per press gesture, so a whole drag — every motion event, every selected point,
      // every mirror, and the release — is a single undo step, which is what the topology rewrite already
      // does for the 640 slots it touches in one go. Opened at the first write rather than at penDown,
      // because a press that grabs something but moves it nowhere must not put an empty change in the
      // undo queue.
      _plugin->beginLatticeEdit();
      _editing = true;
    }
    try {
      knob->setValueAtTime(args.time, w.value.x, w.value.y);
    } catch (const std::exception &) {
      // A host that will not key a knob it does not animate gets the plain write this call was before
      // D-80: the gesture still lands, it just does not become a key.
      knob->setValue(w.value.x, w.value.y);
    }
    return true;
  }

  // One press-event, as many knob writes as that press means. A single-point, single-control drag is the
  // degenerate case — one write, exactly as before multi-select and mirrored handles existed. The writes
  // this press can name are the grabbed slot, the rest of the selection riding on the same position delta
  // (D-85), and the opposite handle of a handle drag as the negative (D-83); all of them go through the
  // same read-then-key helper, and the first one that actually differs opens the edit block the whole
  // press is wrapped in, so a multi-point drag with mirrors is still **one** undo step.
  void applyDrag(const OFX::PenArgs& args, const Scene& sc, const gw::Vec& pointer)
  {
    const gw::Interactor in(sc.src, sc.dst, sc.basis, sc.srcAf, sc.dstAf);
    // The selection the gesture belongs to, and the Ctrl latch as the free-handle override: `dragFor`
    // decides which *knobs* this press means — the grabbed slot, the rest of the selection riding along
    // on the same delta (D-85), the opposite handle as the negative (D-83) — and the shell only performs
    // the writes it named, inside the one edit block this press owns.
    const gw::Drag d = in.dragFor(_grab, _sel, pointer, _ctrl);
    if (!d.changed)
      return;   // a point dragged outside the stamped topology writes no knob, by D-53's rule

    gw::DragWrite primary;
    primary.family = d.family;
    primary.row = d.row;
    primary.col = d.col;
    primary.handle = d.handle;
    primary.value = d.writeValue;
    primary.mirrored = false;

    try {
      bool wrote = writeLatticeKnob(args, primary);
      for (std::vector<gw::DragWrite>::const_iterator it = d.extraWrites.begin();
           it != d.extraWrites.end(); ++it)
      {
        if (writeLatticeKnob(args, *it))
          wrote = true;   // every companion write happens in the same block the primary opened
      }
      if (wrote)
        requestRedrawSafely();
    } catch (const std::exception &) {
      // A host that refuses the write leaves the gesture as it was; the drag is not a place where a
      // plugin gets to decide what the knob set contains.
      endEditBlockSafely();
    }
  }

  void requestRedrawSafely()
  {
    try {
      requestRedraw();
    } catch (const std::exception &) {
      // A host without an interact-redraw call repaints on its own schedule; nothing here depends on it.
    } catch (...) {
    }
  }

  void endEditBlockSafely()
  {
    if (!_editing)
      return;
    _editing = false;
    try {
      if (_plugin)
        _plugin->endLatticeEdit();
    } catch (const std::exception &) {
    } catch (...) {
    }
  }

  GridWrapPlugin* _plugin;
  gw::Selection _sel;      // the lattice points a press edits: one family, one or more points; re-clamped
                           // every event
  gw::Control _grab;       // the slot the gesture writes, from the core's ranked candidates
  gw::Selection _flipSel;  // the other family's point on the same pixel, if a click could mean either
  gw::Selection _deselect; // a Ctrl-pressed point already in the set, taken out at a release that dragged
                           // nothing (D-85)
  gw::Vec _penStart;
  bool _ctrl;              // the Ctrl latch: a free handle drag (D-83), a selection-editing press (D-85)
  bool _dragging;
  bool _editing;           // an edit block is open, owned by this gesture
  bool _moved;             // the gesture exceeded the click threshold, so it is not a family flip
  bool _hasFlip;
  bool _glWarned;
};

/** @brief The descriptor Natron needs in order to create the interact at all: `getMainEntry()` is what
 * gates `tryInitializeOverlayInteracts`, and a null main entry means no `_imp->overlayInteract` and
 * therefore no pen routing. The Support template supplies both halves.
 */
class GridWrapOverlayDescriptor : public OFX::DefaultEffectOverlayDescriptor<GridWrapOverlayDescriptor,
                                                                            GridWrapOverlay>
{
};

////////////////////////////////////////////////////////////////////////////////
// factory

mDeclarePluginFactory(GridWrapPluginFactory, {}, {});

using namespace OFX;

/** @brief Put a knob inside a group, if this host has groups at all (the TimeWarp and ZDefocusZ
 * `reparent`, and the same reasoning: `kOfxParamPropParent` is a property a host may not implement,
 * the Support library throws when a host refuses one, and a describe action that throws is a plugin
 * that does not load in that host. A knob whose group was refused still works at the root of the
 * hierarchy.) */
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

void GridWrapPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  desc.setLabels("GridWrap", "GridWrap",
                 "A Bezier grid warp after the model of Nuke's GridWarp: two control lattices, a "
                 "source grid and a destination grid, each one a surface of cubic Bezier grid lines "
                 "whose points carry four independent tangent handles. `output` and `morph` say which "
                 "lattice is operative and how far the image has travelled between the two; "
                 "`background` says what fills the area the grid does not reach. Every grid knob is a "
                 "fraction of the grid basis, so a saved grid is the same shape at any resolution.");

  desc.setPluginGrouping("OFX Lab");

  // General carries the Dest / Background / Mask inputs; Filter is kept for hosts that only chain
  // effects in filter context, and gets one input and the modes that need one (D-61). Natron picks
  // General when both are declared (D-20).
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedContext(eContextFilter);

  // v1 scope: float32 only (DECISIONS.md D-07), CPU only (D-04/D-06)
  desc.addSupportedBitDepth(eBitDepthFloat);

  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  // Tiles: every read this plugin does is inside a rect it reports through kOfxActionGetRegionsOfInterest
  // — the gather pad and the whole-pixel rounding are inside the same `WarpRender` the render then runs
  // (D-61), so a host that slices the window gets exactly the gather that slice needs, and a clip that
  // refused tiles would only cost it the savings. Nothing here reads a clip outside the window plus the
  // pad its own plan asked for.
  desc.setSupportsTiles(true);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);

  // The Viewer overlay that makes the lattice editable by hand (D-78). Without this call the Support
  // library never sets `kOfxImageEffectPluginPropOverlayInteractV1` (ofxsImageEffect.cpp:577-582), a host
  // never creates the interact, and the grid stays a knob-panel-only thing no matter what the overlay
  // class below can do.
  desc.setOverlayInteractDescriptor(new GridWrapOverlayDescriptor());
}

void GridWrapPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
  // ---------------------------------------------------------------------------
  // Clips
  // ---------------------------------------------------------------------------
  // Source: the picture the *source* grid was drawn on. It is the base of the mask dissolve, the
  // `channels` merge-back and `background = on src`, so it is the one input every mode can read.
  OFX::ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentRGB);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setOptional(false);
  srcClip->setTemporalClipAccess(false);
  srcClip->setSupportsTiles(true);
  srcClip->setIsMask(false);

  if (context == eContextGeneral) {
    // Dest: the picture the *destination* grid was drawn on. `output = destination` samples it through
    // the destination grid, and a morph dissolves it into the warped source — which is what makes a
    // GridWarp node a morph between two stills and not only a distortion of one.
    OFX::ClipDescriptor *dstClip = desc.defineClip("Dest");
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setOptional(true);
    dstClip->setTemporalClipAccess(false);
    dstClip->setSupportsTiles(true);
    dstClip->setIsMask(false);

    // Background: what `background = on bg` fills an uncovered pixel with, read at the output pixel and
    // not through the grid, so it is the one input never warped.
    OFX::ClipDescriptor *bgClip = desc.defineClip("Background");
    bgClip->addSupportedComponent(ePixelComponentRGBA);
    bgClip->addSupportedComponent(ePixelComponentRGB);
    bgClip->addSupportedComponent(ePixelComponentAlpha);
    bgClip->setOptional(true);
    bgClip->setTemporalClipAccess(false);
    bgClip->setSupportsTiles(true);
    bgClip->setIsMask(false);

    // Mask input: General context only (filter contexts forbid masks). Optional and alpha-only; Natron
    // then adds its standard "enable mask" + mask-channel selector knobs and delivers the chosen channel
    // as alpha, and an unconnected mask is fully on (D-20, D-29).
    OFX::ClipDescriptor *maskClip = desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setOptional(true);
    maskClip->setTemporalClipAccess(false);
    maskClip->setSupportsTiles(true);
    maskClip->setIsMask(true);
  }

  // Output, in every supported context: a host wires its node straight from the descriptor, and Natron
  // dereferences getClip("Output") under an assert() only, so an undescribed Output takes the host down
  // the moment the node is created (D-28).
  OFX::ClipDescriptor *outClip = desc.defineClip(kOfxImageEffectOutputClipName);
  outClip->addSupportedComponent(ePixelComponentRGBA);
  outClip->addSupportedComponent(ePixelComponentRGB);
  outClip->addSupportedComponent(ePixelComponentAlpha);
  outClip->setSupportsTiles(true);

  // ---------------------------------------------------------------------------
  // Groups
  // ---------------------------------------------------------------------------
  // The lattice gets a group per grid, so a node at 8x8 is two lattices and not one wall of 640 knobs.
  OFX::GroupParamDescriptor *gOutput = 0, *gGrid = 0, *gSrcGrid = 0, *gDstGrid = 0,
                            *gSrcT = 0, *gDstT = 0, *gRender = 0, *gMask = 0,
                            *gSrcBtns = 0, *gDstBtns = 0;
  try {
    gOutput = desc.defineGroupParam("outputGroup");
    gOutput->setLabels("Output", "Output", "What the node writes, and from which input.");
    gOutput->setOpen(true);

    gGrid = desc.defineGroupParam("gridGroup");
    gGrid->setLabels("Grid", "Grid", "The shape of the lattices and the rect their fractions are stated against.");
    gGrid->setOpen(true);

    /* One group of buttons per grid (D-86): the labelled half of how the eight buttons become **two rows**, a
     * source row and a destination row. The container is the half every hierarchical host understands: Natron's
     * knob group (KnobGroup) is its own container widget that lays its own children out, and it gets them
     * through `KnobHelper::setParentKnob`, which Natron's OFX bridge calls from the parameter's
     * `OfxParamPropParent` name — the property `OFX::ParamDescriptor::setGroupParent` writes (ofxsParams.cpp),
     * which is why the lattice's `sourceGroup` / `destinationGroup` / `sourceTransformGroup` … already appear
     * as panels here. And two group knobs never share a line, so a group per grid is what guarantees the two
     * rows exist at all: one grid's four actions laid out together, the other grid's four in a container of
     * their own below them. The group's label is the only row heading a host is guaranteed to show, and it is
     * what makes the rows *read* as source and destination rather than as eight loose buttons; a name the user
     * could not tell apart from its twin would make a paste land on the wrong grid. `setOpen(true)`: a closed
     * group is a button that has to be clicked before the row it exists to make is even visible. A host that
     * refuses `kOfxParamPropParent` keeps the eight buttons flat in `gridGroup`, where they were before.
     *
     * The *line inside* a group is the other half, and a plugin can ask for it. Natron translates the standard
     * OFX layout properties onto the knob in the one function that builds every parameter it hosts, push
     * buttons included: Engine/OfxImageEffectInstance.cpp::newParam(:464) reads `kOfxParamPropLayoutPadWidth`
     * into `KnobHelper::setSpacingBetweenItems` (:654) and `kOfxParamPropLayoutHint` into
     * `setAddNewLine(false)` / `setAddSeparator(true)` (:664-669). Natron's row law then reads only what those
     * setters left behind: a knob stays on the previous knob's line when that previous sibling's
     * `isNewLineActivated()` is false, and the gap it is given is that sibling's `getSpacingBetweenitems()`
     * (KnobGuiContainerHelper.cpp:404-425); the no-new-line case decrements the page's row and re-uses the last
     * row's QHBoxLayout (:679-700). Which fixes the polarity: the hint is NoNewLine on the first three buttons
     * of a grid's row and stays at the default 0 — "a new line after the parameter" — on the fourth, because
     * that is what drops the *other* grid's row onto the line below it. What stays out of reach is putting the
     * buttons in a *viewer* toolbar instead: those hints are Natron extensions
     * (`kNatronOfxParamPropInViewerContextLayoutHint == …AddNewLine`, `…InViewerContextLayoutPadWidth`;
     * build/natron-openfx/include/ofxNatron.h:737-768, read at Engine/OfxImageEffectInstance.cpp:679-684) and
     * the vendored 1.4 SDK ships no header for them (rule 2), so this plugin asks only for the panel rows a
     * host can be told about in every host. */
    gSrcBtns = desc.defineGroupParam("srcButtonsGroup");
    gSrcBtns->setLabels("Source Grid Buttons", "Source Grid Buttons",
                        "The four actions that read, write, key and unkey the source lattice: copy, paste, "
                        "set_keys, delete_keys.");
    gSrcBtns->setOpen(true);
    reparent(gGrid, gSrcBtns);

    gDstBtns = desc.defineGroupParam("dstButtonsGroup");
    gDstBtns->setLabels("Destination Grid Buttons", "Destination Grid Buttons",
                        "The four actions that read, write, key and unkey the destination lattice: copy, "
                        "paste, set_keys, delete_keys.");
    gDstBtns->setOpen(true);
    reparent(gGrid, gDstBtns);

    gSrcGrid = desc.defineGroupParam("srcGridGroup");
    gSrcGrid->setLabels("Source Grid", "Source Grid", "The lattice the Source input is sampled through.");
    gSrcGrid->setOpen(false);

    gDstGrid = desc.defineGroupParam("dstGridGroup");
    gDstGrid->setLabels("Destination Grid", "Destination Grid", "The lattice the Dest input is sampled through.");
    gDstGrid->setOpen(false);

    gSrcT = desc.defineGroupParam("srcTransformGroup");
    gSrcT->setLabels("Source Transform", "Source Transform", "Move / turn / scale the source lattice as a whole.");
    gSrcT->setOpen(false);

    gDstT = desc.defineGroupParam("dstTransformGroup");
    gDstT->setLabels("Destination Transform", "Destination Transform", "Move / turn / scale the destination lattice as a whole.");
    gDstT->setOpen(false);

    gRender = desc.defineGroupParam("renderGroup");
    gRender->setLabels("Render", "Render", "How the grid is inverted, and how a sample is filtered.");
    gRender->setOpen(false);

    gMask = desc.defineGroupParam("maskGroup");
    gMask->setLabels("Mask", "Mask", "The mask dissolve and what it may write into alpha.");
    gMask->setOpen(false);
  } catch (const std::exception &) {
    // No groups in this host: every knob is described at the root level instead, where it behaves
    // exactly the same (the nesting is decoration, the knobs are the interface).
    gOutput = gGrid = gSrcGrid = gDstGrid = gSrcT = gDstT = gRender = gMask = 0;
    gSrcBtns = gDstBtns = 0;
  }

  // ---------------------------------------------------------------------------
  // Output
  // ---------------------------------------------------------------------------
  // The two knobs `output` shares with a host's own mask controls keep their canonical names
  // (`channels`, `maskInvert`), like the other three plugins. The one name this plugin cannot share is
  // `mix`: Nuke's `mix` is the morph's second axis and is what this file's `mix` means, so the mask
  // dissolve here has no master percentage of its own — it is the mask value (times `fringe`'s
  // feathering), which is what Nuke's GridWarp does too. A host's own mask knobs still do not change
  // what reaches the plugin (D-20, D-29).

  OFX::StringParamDescriptor *channels = desc.defineStringParam("channels");
  channels->setLabels("Channels", "Channels",
                      "Channels to warp: any combination of r, g, b, a (or all / none). The others are "
                      "copied from the input unchanged, which is how an RGBA render keeps its own alpha "
                      "while only its colour is moved. On a one-channel clip, either a or r selects it: "
                      "it has one channel, and hosts label it either way. 'none' makes the node a "
                      "pass-through.");
  channels->setStringType(eStringTypeSingleLine);
  channels->setDefault("all");
  reparent(gOutput, channels);

  OFX::ChoiceParamDescriptor *output = desc.defineChoiceParam("output");
  output->setLabels("Output", "Output",
                    "Which lattice the image is sampled through, and how far it has travelled from the "
                    "source grid towards the destination grid. `source` is the Source input at the output "
                    "pixel; `source warped` reads it through the destination grid, which is the warp "
                    "almost everyone wants; `destination` reads the Dest input at the output pixel; "
                    "`destination warped` reads it through the source grid; `morph` dissolves the two "
                    "warped reads with `warp` and `mix` live, where `warp` chooses the lattice and `mix` "
                    "how far the image has moved along it.");
  for (int i = 0; i < int(gw::kOutputModeCount); ++i)
    output->appendOption(gw::kOutputModeNames[i]);   // the core's names, in the core's order (D-54)
  output->setDefault(int(gw::kOutputSourceWarped));
  reparent(gOutput, output);

  OFX::DoubleParamDescriptor *warp = desc.defineDoubleParam("warp");
  warp->setLabels("Warp", "Warp",
                  "In `morph` only: which of the two lattices is the operative one — 0 samples both "
                  "inputs through the source grid, 1 through the destination grid. Greyed out in the four "
                  "corner modes, which each pin it.");
  warp->setRange(0.0, 1.0);
  warp->setDisplayRange(0.0, 1.0);
  warp->setDefault(double(gw::kWarpDefaultNum));
  reparent(gOutput, warp);

  OFX::DoubleParamDescriptor *mix = desc.defineDoubleParam("mix");
  mix->setLabels("Mix", "Mix",
                 "In `morph` only: how far the image has travelled — 0 leaves every pixel where the "
                 "operative grid puts it, 1 moves it all the way to the other grid's position, and "
                 "between them the two reads are sampled through the blended grid. Greyed out in the four "
                 "corner modes, which each pin it. This is Nuke's `mix`, and it is *not* a mask mix: the "
                 "mask dissolve below has no percentage of its own.");
  mix->setRange(0.0, 1.0);
  mix->setDisplayRange(0.0, 1.0);
  mix->setDefault(double(gw::kMixDefaultNum));
  reparent(gOutput, mix);

  OFX::ChoiceParamDescriptor *background = desc.defineChoiceParam("background");
  background->setLabels("Background", "Background",
                        "What fills a pixel the operative grid does not reach — the area outside its "
                        "cells, or a cell the inversion could not resolve. `on src` and `on dst` read that "
                        "input at the *output* pixel (unwarped), `on bg` reads the Background input there, "
                        "and `on black` leaves it transparent black — which is this node's default (D-79).");
  for (int i = 0; i < int(gw::kBackgroundModeCount); ++i)
    background->appendOption(gw::kBackgroundModeNames[i]);
  background->setDefault(int(gw::kBackgroundBlack));
  reparent(gOutput, background);

  OFX::DoubleParamDescriptor *backgroundMix = desc.defineDoubleParam("background_mix");
  backgroundMix->setLabels("Background Mix", "Background Mix",
                           "How much of the background fills a *covered* pixel too: 0 leaves the warped "
                           "sample alone, 1 replaces it with the background everywhere, and between them "
                           "the render dissolves towards the background. Uncovered pixels take the "
                           "background whatever this says, which is the whole point of `background`.");
  backgroundMix->setRange(0.0, 1.0);
  backgroundMix->setDisplayRange(0.0, 1.0);
  backgroundMix->setDefault(0.0);
  reparent(gOutput, backgroundMix);

  // ---------------------------------------------------------------------------
  // Grid: topology, basis, and the two coordinate knobs
  // ---------------------------------------------------------------------------
  // `rows`/`cols` are the whole point of a grid tool, and they are the only knobs whose change makes
  // this plugin rewrite its own knobs (D-53). Non-animating, because a topology is a property of the
  // node and not of a frame: a lattice cannot be 5x5 on frame 10 and 8x8 on frame 11.
  OFX::IntParamDescriptor *rows = desc.defineIntParam("rows");
  rows->setLabels("Rows", "Rows",
                  "Control points of the grid, in lattice row order (row 0 is the one whose fraction of the "
                  "basis is 0, which is the *bottom* row of the picture — see the point knobs). Changing this "
                  "rewrites every lattice knob so the grid "
                  "keeps its shape at the new count, and the points the new rows add start neutral; a grid "
                  "animated at more keys than this plugin re-keys keeps its keyframes and is resampled at "
                  "render time instead, which the plugin says in its log.");
  rows->setRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  rows->setDisplayRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  rows->setDefault(int(gw::kDefaultPoints));
  rows->setAnimates(false);
  reparent(gGrid, rows);

  OFX::IntParamDescriptor *cols = desc.defineIntParam("cols");
  cols->setLabels("Columns", "Columns",
                  "Control points across the grid. The same rewrite as `rows`. Nuke's `divisions` reaches "
                  "20; the ceiling here is 8, because the lattice travels as ordinary OFX parameters and "
                  "8x8 is already 640 of them.");
  cols->setRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  cols->setDisplayRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  cols->setDefault(int(gw::kDefaultPoints));
  cols->setAnimates(false);
  reparent(gGrid, cols);

  // The panel toggles (D-81). They are placed here, directly under the two knobs that decide *how many*
  // slots a grid has, because they decide *whether* a grid's slots are listed at all. Declared before the
  // topology stamps so that the panel reads "rows, cols, show this grid" and then the internal machinery.
  // Non-animating: what a knob panel lists is a property of the node, not of a frame, and the visibility
  // ask these two feed is a property write the plugin makes once.
  OFX::BooleanParamDescriptor *showSrcGrid = desc.defineBooleanParam("show_source_grid");
  showSrcGrid->setLabels("Show Source Grid", "Show Source Grid",
                         "List the source grid's point and handle knobs in the panel. Off hides them: the "
                         "knobs still hold their values, the render still reads all of them, and the Viewer "
                         "overlay still draws that grid but draws it dimmed and refuses to select or drag "
                         "any of its points (D-84). `output = source` warps through this grid alone, so the "
                         "other grid's toggle is what a node like that turns off.");
  showSrcGrid->setDefault(true);
  showSrcGrid->setAnimates(false);
  reparent(gGrid, showSrcGrid);

  OFX::BooleanParamDescriptor *showDstGrid = desc.defineBooleanParam("show_destination_grid");
  showDstGrid->setLabels("Show Destination Grid", "Show Destination Grid",
                         "List the destination grid's point and handle knobs in the panel. The same knob as "
                         "`show_source_grid` for the other half of the lattice, and the same answer when it "
                         "is off: hidden knobs, unchanged values, unchanged render, and the same dimmed, "
                         "un-pickable overlay.");
  showDstGrid->setDefault(true);
  showDstGrid->setAnimates(false);
  reparent(gGrid, showDstGrid);

  /* The eight grid buttons (D-82), grouped one group per grid (D-86) directly under the two toggles that
   * decide whether a grid's knobs are on screen at all, because these act on exactly those knobs. Loop
   * order is the panel order: the source grid's four actions in the source row, then the destination
   * grid's four in the row below it. A push button carries no value and does not animate: it is a request
   * for an action at the frame the host is on, and `changedParam` is where that request is answered. */
  for (int f = 0; f < gw::kFamilyCount; ++f)
  {
    OFX::GroupParamDescriptor *gBtns = (f == gw::kFamilySrc) ? gSrcBtns : gDstBtns;
    for (int a = 0; a < kBtnPerGrid; ++a)
    {
      OFX::PushButtonParamDescriptor *btn = desc.definePushButtonParam(kGridButtonNames[a][f]);
      btn->setLabels(kGridButtonLabels[a][f], kGridButtonLabels[a][f], kGridButtonHelp[a]);
      /* No `setAnimates`, and no range or default: a push button descends from `ParamDescriptor`, not
       * `ValueParamDescriptor`, so it carries no value to key and the Support library offers no animacy
       * property to set on it. Natron honours the same shape — its button knob reports `canAnimate()`
       * false — which is what makes `set_keys` / `delete_keys` the only buttons that touch keyframes. */
      /* Put this grid's four buttons on one panel line, and the other grid's four on the line below it
       * (D-86). The hint is read by the host off the *descriptor*, so it is set here and nowhere else.
       * Its polarity is the trap: `NoNewLine` means "the *next* knob continues on *this* knob's line", so
       * it goes on the first three buttons of a row, and the row's last button keeps `Normal` — that
       * break-after is exactly what starts the second row. Natron's bridge (Engine/OfxImageEffectInstance.cpp
       * ::newParam, :664-669) turns the hint into `Knob::setAddNewLine` and the pad width into
       * `setSpacingBetweenItems` (:654), and its panel packs a group's children onto the previous line when
       * the earlier sibling asked for no new line (KnobGuiContainerHelper::initializeKnobVectorInternal,
       * knobcont.cpp:404-425 reads the sibling's hint, :660-706 reuses that row's layout). The nested `try`
       * is not decoration: the Support library logs a failed property write *before* it would throw, so
       * `false` suppresses the throw but not the log line, and a host whose HostSupport was built without
       * the Nuke extension flags does not declare these two properties at all. What that refusal costs is a
       * panel with each button on its own line, still inside its grid's labelled group. */
      const bool lastOfRow = (a == kBtnPerGrid - 1);
      try
      {
        btn->getPropertySet().propSetInt(kParamPropLayoutHint,
                                         lastOfRow ? kLayoutHintNormal : kLayoutHintNoNewLine, 0, false);
        if (!lastOfRow)
          btn->getPropertySet().propSetInt(kParamPropLayoutPadWidth, kGridButtonRowGap, 0, false);
      }
      catch (const std::exception &)
      {
        // A host that rejects the property keeps the buttons one per line: the groups are the row, the
        // layout hints are only the packing of a row.
      }
      reparent(gBtns, btn);
    }
  }

  // The topology stamps: secret, plugin-owned, and the reason a fraction means what it means. They say
  // which lattice the 640 knobs were normalised for, so a mismatch means "the rewrite has not happened"
  // and the render resamples instead of reading 25 knobs and calling that an 8x8 grid.
  OFX::IntParamDescriptor *topoRows = desc.defineIntParam("grid_topo_rows");
  topoRows->setLabels("Grid Rows", "Grid Rows",
                      "Internal: the row count these lattice knobs are normalised for. Written by the "
                      "plugin when `rows` changes; not meant to be edited.");
  topoRows->setRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  topoRows->setDisplayRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  topoRows->setDefault(int(gw::kDefaultPoints));
  topoRows->setAnimates(false);
  topoRows->setEvaluateOnChange(false);
  topoRows->setIsSecret(true);

  OFX::IntParamDescriptor *topoCols = desc.defineIntParam("grid_topo_cols");
  topoCols->setLabels("Grid Columns", "Grid Columns",
                      "Internal: the column count these lattice knobs are normalised for. Written by the "
                      "plugin when `cols` changes; not meant to be edited.");
  topoCols->setRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  topoCols->setDisplayRange(int(gw::kMinPoints), int(gw::kMaxPoints));
  topoCols->setDefault(int(gw::kDefaultPoints));
  topoCols->setAnimates(false);
  topoCols->setEvaluateOnChange(false);
  topoCols->setIsSecret(true);

  // The basis override, stated in fractions of the basis so that the default pair *is* "no override".
  OFX::Double2DParamDescriptor *boundsMin = desc.defineDouble2DParam("grid_bounds_min");
  boundsMin->setLabels("Grid Bounds Min", "Grid Bounds Min",
                       "Where the grid's own rect starts, as a fraction of the basis — the Source clip's "
                       "region of definition, or this node's frame when nothing is connected. 0,0 is that "
                       "rect's 0,0 corner — x right, y up the picture (D-64) — so the default is no "
                       "override; 0.1,0.2 moves the grid's 0..1 range "
                       "inward, which is how a grid is fitted to part of a picture. A pair the wrong way "
                       "round is swapped, and a pair with no area means the basis, unchanged.");
  boundsMin->setRange(-10.0, -10.0, 10.0, 10.0);
  boundsMin->setDisplayRange(0.0, 0.0, 1.0, 1.0);
  boundsMin->setDefault(0.0, 0.0);
  reparent(gGrid, boundsMin);

  OFX::Double2DParamDescriptor *boundsMax = desc.defineDouble2DParam("grid_bounds_max");
  boundsMax->setLabels("Grid Bounds Max", "Grid Bounds Max",
                       "Where the grid's own rect ends, as a fraction of the same basis. 1,1 is that "
                       "rect's bottom-right, so the default is no override. Fractions outside 0..1 are "
                       "allowed: 1.5 extends the grid's range past the basis, which is what a lattice "
                       "that must reach beyond the frame needs.");
  boundsMax->setRange(-10.0, -10.0, 10.0, 10.0);
  boundsMax->setDisplayRange(0.0, 0.0, 1.0, 1.0);
  boundsMax->setDefault(1.0, 1.0);
  reparent(gGrid, boundsMax);

  OFX::ChoiceParamDescriptor *boundary = desc.defineChoiceParam("boundary_box");
  boundary->setLabels("Boundary Box", "Boundary Box",
                      "Which rect the basis is taken from before `grid bounds` narrows it: `union` is every "
                      "connected input's region of definition merged together, `format` is the largest of "
                      "those boxes on its own. `format` is the answer when one input is a small inset plate "
                      "and the grid should still be drawn across the whole picture. OFX 1.4 has no format "
                      "property on a clip, so \"the format\" can only be read as \"the biggest box on hand\".");
  for (int i = 0; i < int(kBoundaryModeCount); ++i)
    boundary->appendOption(kBoundaryModeNames[i]);
  boundary->setDefault(int(kBoundaryUnion));
  boundary->setAnimates(false);
  reparent(gGrid, boundary);

  // ---------------------------------------------------------------------------
  // Render
  // ---------------------------------------------------------------------------
  OFX::IntParamDescriptor *renderRes = desc.defineIntParam("render_res");
  renderRes->setLabels("Render Resolution", "Render Resolution",
                       "How finely the grid is subdivided where it is inverted, subdivisions per cell side. "
                       "The render asks the grid where a pixel came from, and it answers by walking a "
                       "subdivision of the cell that pixel falls in: this is that subdivision, so it is "
                       "both the accuracy of a query and the number of boxes the query has to look at. The "
                       "forward mapping is computed once per render, so this costs memory and setup rather "
                       "than per-pixel time; Foundry's GridWarp uses 10.");
  renderRes->setRange(int(gw::kMinRenderRes), int(gw::kMaxRenderRes));
  renderRes->setDisplayRange(1, 20);
  renderRes->setDefault(int(gw::kDefaultRenderRes));
  renderRes->setAnimates(false);
  reparent(gRender, renderRes);


  OFX::ChoiceParamDescriptor *filter = desc.defineChoiceParam("filter");
  filter->setLabels("Filter", "Filter",
                    "How a sample that lands between two source pixels is reconstructed — the kernel the "
                    "gather reads with, everywhere the grid magnifies. `cubic` (Nuke's default) has no "
                    "negative lobes at all: no ringing on a hard edge, and the softest of the menu. `keys` "
                    "is sharper and rings a little, `mitchell` and `parzen` are the balanced pair, `notch` "
                    "is the flattest and deliberately removes fine patterns, `lanczos4`/`lanczos6`/`sinc4` "
                    "reach further for a strong magnification, and `impulse` takes the nearest pixel, which "
                    "is what you want when the grid is only translating and any blur is unwanted. The radius "
                    "of the chosen kernel is then multiplied by how much the cell it samples magnifies, so "
                    "the cost of a deep filter is paid where the grid actually stretches.");
  for (int i = 0; i < int(gw::kFilterCount); ++i)
    filter->appendOption(gw::kFilterNames[i]);   // the core's kernel table, named as it names it (D-57)
  filter->setDefault(int(gw::kDefaultFilter));
  filter->setAnimates(false);
  reparent(gRender, filter);

  // ---------------------------------------------------------------------------
  // Mask
  // ---------------------------------------------------------------------------
  // The dissolve itself has no percentage of its own here: its weight is the mask value, because the
  // `mix` name on this node is Nuke's morph axis (D-54). An unconnected mask is fully on, so `maskInvert`
  // with nothing connected means the effect off — the output is the input (D-20, D-29), which is also one
  // of the identity reports this plugin makes.
  OFX::BooleanParamDescriptor *fringe = desc.defineBooleanParam("fringe");
  fringe->setLabels("Fringe", "Fringe",
                    "Weight the mask dissolve by 4m(1-m) instead of by m: fully masked and fully "
                    "unmasked pixels get *nothing* of the effect, and only a mask value of 0.5 gets all of "
                    "it. That turns a mask's edge into the whole effect and its flat interiors into no "
                    "effect, which is what a grid warp wants on a hard matte — the warp lives on the "
                    "boundary and leaves the middle of a shape alone.");
  fringe->setDefault(false);
  fringe->setAnimates(false);
  reparent(gMask, fringe);

  OFX::BooleanParamDescriptor *inject = desc.defineBooleanParam("inject");
  inject->setLabels("Inject", "Inject",
                    "Write the mask's own value into the output's alpha, on an RGBA render: not the "
                    "dissolved alpha, the mask, which is how a grid warp hands its matte to the next node. "
                    "It also makes this node's output something the Source input does not have, which is "
                    "why it cancels the two identity reports that pass the Source straight through.");
  inject->setDefault(false);
  inject->setAnimates(false);
  reparent(gMask, inject);

  OFX::BooleanParamDescriptor *maskInvert = desc.defineBooleanParam("maskInvert");
  maskInvert->setLabels("Mask Invert", "Mask Invert",
                        "Invert the mask: black areas get the warp, white areas pass through. With nothing "
                        "connected to Mask this means *no effect at all*, because an unconnected mask is "
                        "fully on (D-29).");
  maskInvert->setDefault(false);
  maskInvert->setAnimates(false);
  reparent(gMask, maskInvert);

  // ---------------------------------------------------------------------------
  // The Transform tab, per grid
  // ---------------------------------------------------------------------------
  // One family per lattice, because the two lattices are what a morph is *between*, and a transform that
  // belongs to one of them has to be editable without touching the other. `use_source_transform` is the
  // exception that ties them together, and it sits with the destination because it is the destination
  // grid that inherits.
  static const char* const kOrderHelp =
      " Applied in this order: skew, then scale, then rotate about `center`, then translate, then the extra "
      "matrix if it is on. `translate` and `center` are fractions of the grid basis, so a translation is "
      "the same move at any resolution; angles are degrees, positive turns counter-clockwise as the Viewer "
      "looks at the picture.";

  const char* const kTransformSuffix[10] = { "translate", "rotate", "scale", "skew_x", "skew_y",
                                             "skew_order", "center", "use_matrix", "matrix_row0",
                                             "matrix_row1" };

  for (int w = 0; w < 2; ++w) {
    const char* which = kWhichNames[w];
    const char* gridName = (w == 0) ? "source" : "destination";
    OFX::GroupParamDescriptor* g = (w == 0) ? gSrcT : gDstT;

    OFX::Double2DParamDescriptor* translate = desc.defineDouble2DParam(transformKnobName(which, kTransformSuffix[0]));
    translate->setLabels("Translate", "Translate",
                         std::string("Move the whole ") + gridName + " lattice, in fractions of the grid "
                         "basis: 1 is one basis width to the right, 0.1 is a tenth of the picture." + kOrderHelp);
    translate->setRange(-10.0, -10.0, 10.0, 10.0);
    translate->setDisplayRange(-1.0, -1.0, 1.0, 1.0);
    translate->setDefault(0.0, 0.0);
    reparent(g, translate);

    OFX::DoubleParamDescriptor* rotate = desc.defineDoubleParam(transformKnobName(which, kTransformSuffix[1]));
    rotate->setLabels("Rotate", "Rotate",
                      std::string("Turn the whole ") + gridName + " lattice, in degrees, counter-clockwise "
                      "as the Viewer looks at the picture, about `center`. A Bezier lattice turns as a "
                      "sheet: the handles turn with the points, so the curvature travels too." + kOrderHelp);
    rotate->setRange(-3600.0, 3600.0);
    rotate->setDisplayRange(-180.0, 180.0);
    rotate->setDefault(0.0);
    reparent(g, rotate);

    OFX::Double2DParamDescriptor* scale = desc.defineDouble2DParam(transformKnobName(which, kTransformSuffix[2]));
    scale->setLabels("Scale", "Scale",
                     std::string("Multiply the ") + gridName + " lattice's distances from `center`: 1 is "
                     "unchanged, 2 doubles them, 0 collapses the lattice to a point — and a collapsed "
                     "lattice is not invertible, which is the one case this node refuses and renders "
                     "unmoved." + kOrderHelp);
    scale->setRange(-100.0, -100.0, 100.0, 100.0);
    scale->setDisplayRange(0.0, 0.0, 2.0, 2.0);
    scale->setDefault(1.0, 1.0);
    reparent(g, scale);

    OFX::DoubleParamDescriptor* skewX = desc.defineDoubleParam(transformKnobName(which, kTransformSuffix[3]));
    skewX->setLabels("Skew X", "Skew X",
                     std::string("Shear the ") + gridName + " lattice sideways, in degrees: how far a "
                     "vertical line leans. Clamped to +-85, because a skew at 90 is an infinite affine and "
                     "a lattice of infinities is not a grid." + kOrderHelp);
    skewX->setRange(-kMaxSkewDeg, kMaxSkewDeg);
    skewX->setDisplayRange(-45.0, 45.0);
    skewX->setDefault(0.0);
    reparent(g, skewX);

    OFX::DoubleParamDescriptor* skewY = desc.defineDoubleParam(transformKnobName(which, kTransformSuffix[4]));
    skewY->setLabels("Skew Y", "Skew Y",
                     std::string("Shear the ") + gridName + " lattice vertically, in degrees. The same "
                     "clamp, for the same reason." + kOrderHelp);
    skewY->setRange(-kMaxSkewDeg, kMaxSkewDeg);
    skewY->setDisplayRange(-45.0, 45.0);
    skewY->setDefault(0.0);
    reparent(g, skewY);

    OFX::ChoiceParamDescriptor* skewOrder = desc.defineChoiceParam(transformKnobName(which, kTransformSuffix[5]));
    skewOrder->setLabels("Skew Order", "Skew Order",
                         "Which skew is applied first. The two shears do not commute — they differ by a "
                         "scale-like term — so the order is a knob and not a preference.");
    skewOrder->appendOption("XY");
    skewOrder->appendOption("YX");
    skewOrder->setDefault(0);   // option 0 is `XY`, which is grid.h's `skewOrderXY == true`
    skewOrder->setAnimates(false);
    reparent(g, skewOrder);

    OFX::Double2DParamDescriptor* center = desc.defineDouble2DParam(transformKnobName(which, kTransformSuffix[6]));
    center->setLabels("Center", "Center",
                      std::string("The point the ") + gridName + " lattice turns and scales about, as a "
                      "fraction of the grid basis, so the default 0.5,0.5 is the middle of the picture."
                      + kOrderHelp);
    center->setRange(-10.0, -10.0, 10.0, 10.0);
    center->setDisplayRange(0.0, 0.0, 1.0, 1.0);
    center->setDefault(0.5, 0.5);
    reparent(g, center);

    OFX::BooleanParamDescriptor* useMatrix = desc.defineBooleanParam(transformKnobName(which, kTransformSuffix[7]));
    useMatrix->setLabels("Use Matrix", "Use Matrix",
                         std::string("Apply the extra matrix to the ") + gridName + " lattice as well, last "
                         "of all. The four knobs above are a readable shape; this is what you reach for when "
                         "the shape has to match something exact.");
    useMatrix->setDefault(false);
    useMatrix->setAnimates(false);
    reparent(g, useMatrix);

    OFX::Double3DParamDescriptor* row0 = desc.defineDouble3DParam(transformKnobName(which, kTransformSuffix[8]));
    row0->setLabels("Matrix Row 0", "Matrix Row 0",
                    "First row of the 2x3 affine: the output x of (x,y,1). A 3x3 would be a projective map "
                    "and a grid warp cannot invert one reliably, so a projective third row is not offered.");
    row0->setRange(-1.0e6, -1.0e6, -1.0e6, 1.0e6, 1.0e6, 1.0e6);
    row0->setDisplayRange(-2.0, -2.0, -2.0, 2.0, 2.0, 2.0);
    row0->setDefault(1.0, 0.0, 0.0);
    reparent(g, row0);

    OFX::Double3DParamDescriptor* row1 = desc.defineDouble3DParam(transformKnobName(which, kTransformSuffix[9]));
    row1->setLabels("Matrix Row 1", "Matrix Row 1", "Second row of the 2x3 affine: the output y of (x,y,1).");
    row1->setRange(-1.0e6, -1.0e6, -1.0e6, 1.0e6, 1.0e6, 1.0e6);
    row1->setDisplayRange(-2.0, -2.0, -2.0, 2.0, 2.0, 2.0);
    row1->setDefault(0.0, 1.0, 0.0);
    reparent(g, row1);

    if (w == 1) {
      // `use_source_transform` (Nuke's GridWarpTracker knob): the destination grid inherits the source
      // grid's transform *before* its own. It lives with the destination because it is the destination
      // lattice that inherits, and it is described non-animating because it says which of the two
      // families applies, which is a property of the node.
      OFX::BooleanParamDescriptor* useSrcT = desc.defineBooleanParam("use_source_transform");
      useSrcT->setLabels("Use Source Transform", "Use Source Transform",
                         "Apply the source lattice's transform to the destination lattice first, before the "
                         "destination's own. On, the two lattices travel together and the destination knobs "
                         "describe only the *difference* between them — which is what makes a morph hold its "
                         "shape while the whole thing is moved around. Off, the two are independent.");
      useSrcT->setDefault(true);
      useSrcT->setAnimates(false);
      reparent(g, useSrcT);
    }
  }

  // ---------------------------------------------------------------------------
  // The lattice: 640 knobs, described unconditionally (D-53)
  // ---------------------------------------------------------------------------
  /* A host's parameter set is fixed at describe time, so the points a grown topology needs have to exist
   * before the user grows it — and a knob that is *not* described is a knob a constructor cannot fetch and
   * a script cannot write. What keeps 640 of them usable is that the ones outside the default 5x5 topology
   * are described secret, which is the answer a host has when it builds its panel (Natron reads
   * `kOfxParamPropSecret` off the descriptor, ofxhParam.cpp), and which the plugin then asks the host to
   * revisit through `setIsSecret` when `rows`/`cols` move — an ask Natron answers by honouring it, in both
   * directions, which is what the measurement in DECISIONS.md (D-53, correction) found.
   *
   * The defaults are grid.h's neutral lattice: positions at i/(cols-1), j/(rows-1) and handles at a third
   * of the spacing, which is the cubic-Bezier "control at 1/3 of the chord on both ends" arc and the value
   * real GridWarp dumps hold (references/gridwrap/nuke_gridwrap_knobs.md). A slot only a grown topology can
   * reach defaults to the neutral lattice *of that grown topology*, so a point the user has never touched
   * is never a degenerate control point whatever the stamps say. */

  for (int w = 0; w < 2; ++w) {
    const char* which = kWhichNames[w];
    const char* gridName = (w == 0) ? "source" : "destination";
    OFX::GroupParamDescriptor* g = (w == 0) ? gSrcGrid : gDstGrid;

    for (int r = 0; r < gw::kMaxPoints; ++r) {
      for (int c = 0; c < gw::kMaxPoints; ++c) {
        const gw::FracPoint d = knobDefaultFractions(r, c);
        const bool shown = (r < gw::kDefaultPoints && c < gw::kDefaultPoints);

        {
          OFX::Double2DParamDescriptor* p = desc.defineDouble2DParam(gridKnobName(which, r, c, 0));
          p->setLabels(std::string("Point ") + std::to_string(r) + "," + std::to_string(c),
                       std::string("Point ") + std::to_string(r) + "," + std::to_string(c),
                       std::string("Control point at row ") + std::to_string(r) + ", column " +
                         std::to_string(c) + " of the " + gridName + " lattice, as a fraction of the grid "
                         "basis: 0,0 is that rect's 0,0 corner and 1,1 its 1,1 corner, x right and y "
                         "*increasing up the picture* (D-64), so a larger fraction is higher in the frame "
                         "and row 0 of the lattice is its bottom row. Fractions outside "
                         "0..1 are legal and are how a lattice reaches past the frame.");
          p->setRange(-10.0, -10.0, 10.0, 10.0);
          p->setDisplayRange(0.0, 0.0, 1.0, 1.0);
          p->setDefault(d.x, d.y);
          if (!shown) p->setIsSecret(true);
          reparent(g, p);
        }

        for (int h = 0; h < gw::kHandleCount; ++h) {
          const HandleSpec& spec = kHandleSpecs[h];
          OFX::Double2DParamDescriptor* k = desc.defineDouble2DParam(gridKnobName(which, r, c, spec.suffix));
          k->setLabels(std::string(spec.label) + " Handle", std::string(spec.label) + " Handle",
                       std::string("The ") + spec.suffix + " tangent handle of that " + gridName +
                         " point, as the full 2D offset of its control point: x in fractions of the basis "
                         "width, y in fractions of its height. The grid line leaving this point towards the "
                         + spec.label + " is the cubic Bezier through this handle and the matching handle of "
                         "the point on the other side of that line, so the two ends of a line are shaped "
                         "independently. Neutral is a third of the spacing along the " + spec.axis + ". The "
                         "name is the direction the Viewer shows, because in canonical coordinates `left`/"
                         "`right` are the -x/+x arms and `down`/`up` are the -y/+y arms: `up` is the handle "
                         "of the lattice line that leaves this point towards larger y, which sits *above* "
                         "the point (D-64, D-65). Nuke's own four handles are listed up, down, right, left "
                         "with no sign to read off, so this frame is what decides the pairing "
                         "(references/gridwrap/nuke_gridwrap_knobs.md).");
          k->setRange(-10.0, -10.0, 10.0, 10.0);
          k->setDisplayRange(-0.5, -0.5, 0.5, 0.5);
          k->setDefault(d.h[spec.which][0], d.h[spec.which][1]);
          if (!shown) k->setIsSecret(true);
          reparent(g, k);
        }
      }
    }
  }
}

OFX::ImageEffect *GridWrapPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
  return new GridWrapPlugin(handle);
}

namespace OFX
{
namespace Plugin
{
void getPluginIDs(OFX::PluginFactoryArray &ids)
{
  static GridWrapPluginFactory p("net.ofxlab.gridwrap", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
