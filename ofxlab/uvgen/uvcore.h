// UVGen: the UV-ramp core (DECISIONS.md D-73, D-74).
//
// OFX-free, like grid.h / resample.h / render.h / zd.h: every number this plugin writes is decided
// here, and every claim in this file is checked headlessly (tests/uv_gen_test.cpp) before the OFX
// layer in uvgen.cpp asks a host for a pixel. What this file decides is what a texel's u and v are,
// which way v runs, what the knobs do to them, and which channel carries which one.
//
// There is no `references/` spec for this plugin, and that is worth saying out loud: Foundry ships no
// node named UVGen (its UV knobs live on geometry nodes, and `UVProject` / `GeoProjectUV` / `UVTile`
// / `UVLayout` are a different job — projecting an image onto a mesh, not emitting a ramp). So the
// spec of record is the backlog line this plugin was created from, which **D-73** quotes verbatim:
// *"uvGen node … it just generates uv-image for current input resolution: that is as simple as
// red:x/width, green:y/height"*. D-73 records the four things that sentence does not answer, and this
// file is the answer to them.
//
// The picture, per output pixel at OFX pixel coordinates (col, row):
//
//   1. The ramp basis (`Ramp`, from `resolveRamp`). The plugin hands the core one reference rect — the
//      rect the fractions are stated against — and the core floors/ceils it to an integer texel origin
//      plus span. `u = (col - ramp.x1) / ramp.w`. A texel-anchored ramp, not a centre-sampled one: the texel at
//      the rect's own origin reads u = 0 and the last texel reads (w-1)/w, which is the ramp a shader
//      wants when it feeds this image straight back as a texture coordinate (`kMaxRectSpan` bounds the
//      rect the core will accept: a rect whose ends are unbounded or absurdly far apart is not a
//      resolution anyone is rendering at, so the plugin keeps looking for a usable one).
//
//   2. Which way y runs. The reference rect is the canonical OFX rect — the same space as grid.h's
//      `Rect`: **y is up**, so `row == ramp.y1` is the *bottom* row of the picture (grid.h states it, and
//      D-64 / D-70 measured both directions of that claim against a real host's delivered frames). So
//      with `flipV` off, v grows *upward*: the first stored scanline of the delivered image, which is
//      the canonical top row, carries the largest v. `flipV` reverses the row order the fractions are
//      handed out in — same multiset of v values, opposite direction — which is the direction a GL /
//      Vulkan texture expects (row 0 = v 0). Neither direction ever reaches v = 1: the ramp is
//      texel-corner anchored, so its last value is (h-1)/h (D-74).
//
//   3. The knobs, in one order (`applyKnobs`, and the order is stated only here): the fraction is
//      scaled, then offset, then optionally wrapped as `x - floor(x)`. Wrap therefore maps an exact
//      integer onto 0.0 — 1.0 lands on 0.0, and a negative value lands inside [0,1) — and without wrap
//      a value outside [0,1) is delivered as it is: pixels the reference rect does not cover get
//      extrapolated u/v rather than clamped, because clamping is a different picture and no knob asks
//      for it.
//
//   4. Which channel carries which fraction (`slotValue`). The selected channels carry the ramp, and
//      one fraction per role: red, blue and alpha carry **u**, green carries **v** — v has exactly one
//      role, so `channels rg` and `channels rgb` and `channels rga` all deliver the same v in green.
//      The *slot* → role mapping is the one GridWrap already uses (render.cpp's `logicalChannel`: a
//      1-component plane is the alpha role), re-declared here because rule 6 of AGENTS.md keeps a helper
//      in a plugin until a second plugin needs it moved. The consequence is worth naming: a 1-channel
//      output can only carry **u**. A slot that is not selected is filled — 0.0 for a colour role, 1.0
//      for the alpha role, because an opaque alpha is what a generated picture needs and a transparent
//      one would be thrown away by the host's own merge. So a selected role with no plane is not a thing
//      either — that case is resolved before `slotValue` is ever asked, by item 5.
//
//   5. A selection is filtered against the planes that actually exist (`restrictSelection`, **D-74**).
//      A host resolves a clip's components as an intersection — what this plugin describes AND what the
//      connected Source delivers — so an alpha-less 3-component Source delivers an Output with **no
//      alpha plane at all**, and `channels a` would then name a role with no slot: half-applied, and the
//      picture looks like the node wrote black. So a selected role with no plane is **dropped** and the
//      selection re-resolves: named ∩ available, else the default (rg) ∩ available, else every available
//      role (a 1-component plane's only role is A, and u is the only ramp it can carry). Availability is
//      not a guess from nchans alone either: it is the image of `logicalChannel` over slots [0, nchans),
//      so the same mapping decides both questions.

#ifndef OFXLAB_UVGEN_UVCORE_H
#define OFXLAB_UVGEN_UVCORE_H

#include <string>

namespace uv {

// The knob table (ARCHITECTURE.md §3.5) read a second time as the numbers the core clamps to, so a
// descriptor range and a model clamp cannot drift apart (the D-60 pattern).
const char *const kDefaultChannels = "rg";   // the backlog's two channels

const double kDefaultScale  = 1.0;
const double kDefaultOffset = 0.0;
const double kMinScale      = 0.0;
const double kMaxScale      = 1024.0;
const double kMinOffset     = -1024.0;
const double kMaxOffset     = 1024.0;

// The widest reference rect the core will build a ramp from. A rect outside this is not a resolution,
// and `rectUsable` is what tells the plugin to go on looking for one (D-74).
const double kMaxRectSpan = 1.0e7;

/** @brief A rect in canonical OFX pixel space: y **up** (the space grid.h's `Rect` describes). The
  * field names are kept in OFX's own order so the plugin hands its OfxRectD over without renaming or
  * translating anything: y1 is the *lower* edge, y2 the upper one. */
struct Rect {
  double x1, y1, x2, y2;

  // Real constructors rather than in-class initialisers: under this project's -std=gnu++11 a struct
  // with NSDMIs is not an aggregate, so `Rect{a, b, c, d}` would not compile (grid.h's `Rect` and
  // render.h's `Options` declare ctors for the same reason).
  Rect() : x1(0.0), y1(0.0), x2(0.0), y2(0.0) {}
  Rect(double x1_, double y1_, double x2_, double y2_)
      : x1(x1_), y1(y1_), x2(x2_), y2(y2_) {}
};

/** @brief A reference rect floored/ceiled to whole texels: origin + span. */
struct Ramp {
  double x1, y1;   // texel origin
  double w, h;     // texel span, always >= 1

  Ramp() : x1(0.0), y1(0.0), w(0.0), h(0.0) {}
  Ramp(double x1_, double y1_, double w_, double h_) : x1(x1_), y1(y1_), w(w_), h(h_) {}
};

/** @brief The roles a buffer of @p nchans planes has a plane for, indexed by role (r, g, b, a): the
  * image of `logicalChannel` over the slots [0, nchans) — a 1-component plane has only the alpha role,
  * a 2-component one only R and G, and so on. An out-of-range nchans (the plugin refuses those) yields
  * no roles. */
void availableRoles(int nchans, bool out[4]);

/** @brief Resolve a selection against the planes that exist (**D-74**): a role with no plane is dropped
  * rather than left selected and unwritable, and what is left re-resolves —
  * `sel ∩ available`, else `kDefaultChannels ∩ available`, else every available role. Leaving a
  * selection untouched for an out-of-range nchans is what this does when there is nothing to filter
  * against. */
void restrictSelection(bool sel[4], int nchans);

/** @brief The knob set, sanitised. `sanitise()` is what makes a hand-built Options as safe as one the
  * plugin read off a host. */
struct Options {
  bool sel[4];                          // r, g, b, a — the ofxlab_channels.h order
  bool flipV;
  double scaleU, scaleV, offsetU, offsetV;
  bool wrap;

  Options();
  void sanitise();

  /** @brief Drop every selected role this buffer has no plane for (D-74): see `restrictSelection`. */
  void restrictTo(int nchans) { restrictSelection(sel, nchans); }
};

/** @brief Can a ramp be built from this rect: four finite ends, non-empty, spans inside kMaxRectSpan. */
bool rectUsable(const Rect &r);

/** @brief Floor/ceil a usable rect to a texel origin + span, with each span at least 1. */
Ramp resolveRamp(const Rect &r);

/** @brief Resolve a `channels` knob value. Unrecognised words and an empty string fall back to
  * `kDefaultChannels` through the shared parser (D-17); a word that legitimately selects *nothing*
  * ("none") also ends up as the default, because a UV node that writes no channel is not a picture
  * (this rule lives in `Options::sanitise`, so it holds for a core-built Options too). */
void parseChannels(const std::string &value, bool sel[4]);

/** @brief scale, then offset, then wrap — the one order the knobs have (stated at the top of this file). */
double applyKnobs(double fraction, double scale, double offset, bool wrap);

/** @brief u at a column, knobs included. */
double uFraction(int col, const Options &opt, const Ramp &ramp);

/** @brief v at a row, knobs included, in the direction `flipV` selects. */
double vFraction(int row, const Options &opt, const Ramp &ramp);

/** @brief What one delivered slot holds. A slot is either the u ramp, the v ramp, or a constant. */
enum SlotKind { kSlotFill = 0, kSlotU = 1, kSlotV = 2 };

struct SlotValue {
  int kind;      // SlotKind
  double fill;   // the constant, when kind == kSlotFill
};

/** @brief The role of a delivered slot, the same mapping GridWrap's render core uses: a 1-component
  * plane is the alpha role, otherwise slot k is channel k. */
int logicalChannel(int slot, int nchans);

/** @brief The constant a role gets when its channel is not selected. */
double fillForRole(int role);

/** @brief What the delivered slot at index @p slot holds for this knob set. */
SlotValue slotValue(int slot, int nchans, const Options &opt);

/** @brief Write @p count interleaved pixels starting at column @p col0 of @p row into @p dst
  * (@p nchans floats per pixel). The row's v is computed once; u is per column. */
void fillRow(float *dst, int nchans, const Options &opt, const Ramp &ramp, int col0, int row, int count);

} // namespace uv

#endif // OFXLAB_UVGEN_UVCORE_H
