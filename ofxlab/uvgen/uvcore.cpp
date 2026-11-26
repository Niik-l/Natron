// UVGen: the UV-ramp core — definitions (DECISIONS.md D-73, D-74).
//
// OFX-free by construction: <cmath> and <string> plus the shared channel parser, and nothing else.
// What a caller hands this file is a reference rect and a knob set; what it gets back is a texel
// origin plus span, and the value every delivered slot holds at a pixel. The OFX layer in uvgen.cpp
// never decides a number of its own — it picks the reference rect out of what the host reports
// (D-74 states that chain) and then asks `fillRow` for the row.

#include "uvcore.h"

#include <cmath>
#include <cstddef>

#include "ofxlab_channels.h"   // the "channels" knob parser (D-06, D-17)

namespace uv {

// A selected channel list that ends up selecting *nothing* — the word "none", or the letters of a
// channel this output does not have — is resolved back to the default here rather than in the plugin,
// so a hand-built Options and one read off a host behave the same way. A UV node that writes no
// channel is a black-and-transparent image, which is not the picture the knob asked for.
static void defaultWhenNothingSelected(bool sel[4])
{
  if (sel[0] || sel[1] || sel[2] || sel[3])
    return;
  OfxLab::parseChannelTokens(OfxLab::cleanChannelTokens(kDefaultChannels), sel);
}

Options::Options()
  : flipV(false)
  , scaleU(kDefaultScale)
  , scaleV(kDefaultScale)
  , offsetU(kDefaultOffset)
  , offsetV(kDefaultOffset)
  , wrap(false)
{
  OfxLab::parseChannels(kDefaultChannels, sel, kDefaultChannels);
}

void Options::sanitise()
{
  defaultWhenNothingSelected(sel);

  // A host clamps to the described range, but a knob can still arrive as a NaN/inf through an
  // animation or a host that does not clamp, and a NaN ramp poisons every pixel it touches. An
  // unusable number becomes that knob's default *before* the range clamp, because the point of a
  // default is that it is inside the range.
  if (!std::isfinite(scaleU))
    scaleU = kDefaultScale;
  if (!std::isfinite(scaleV))
    scaleV = kDefaultScale;
  if (!std::isfinite(offsetU))
    offsetU = kDefaultOffset;
  if (!std::isfinite(offsetV))
    offsetV = kDefaultOffset;

  if (scaleU < kMinScale) scaleU = kMinScale;
  if (scaleU > kMaxScale) scaleU = kMaxScale;
  if (scaleV < kMinScale) scaleV = kMinScale;
  if (scaleV > kMaxScale) scaleV = kMaxScale;
  if (offsetU < kMinOffset) offsetU = kMinOffset;
  if (offsetU > kMaxOffset) offsetU = kMaxOffset;
  if (offsetV < kMinOffset) offsetV = kMinOffset;
  if (offsetV > kMaxOffset) offsetV = kMaxOffset;
}

bool rectUsable(const Rect &r)
{
  if (!(std::isfinite(r.x1) && std::isfinite(r.y1) && std::isfinite(r.x2) && std::isfinite(r.y2)))
    return false;

  // OFX rects are half-open in the sense that a w-wide image spans [x1, x1 + w), so an empty rect has
  // equal ends. Both spans also have to be inside kMaxRectSpan: a rect that is not is an unbounded
  // RoD or a mistake, not a resolution.
  const double w = r.x2 - r.x1;
  const double h = r.y2 - r.y1;
  if (!(w > 0.0) || !(h > 0.0))
    return false;
  if (w > kMaxRectSpan || h > kMaxRectSpan)
    return false;
  return true;
}

Ramp resolveRamp(const Rect &r)
{
  // Round outward to whole texels: the ramp covers every texel the rect touches, so a rect that
  // starts at -0.5 owns the texel at -1 as well. The span is floored at 1 so a degenerate rect (an
  // input reported as 1x1, or a rect thinner than one texel) still yields a division by 1 instead of
  // by 0 or by a negative number.
  Ramp out;
  out.x1 = std::floor(r.x1);
  out.y1 = std::floor(r.y1);
  out.w = std::ceil(r.x2) - out.x1;
  out.h = std::ceil(r.y2) - out.y1;
  if (!(out.w >= 1.0))
    out.w = 1.0;
  if (!(out.h >= 1.0))
    out.h = 1.0;
  return out;
}

void parseChannels(const std::string &value, bool sel[4])
{
  OfxLab::parseChannels(value, sel, kDefaultChannels);
  defaultWhenNothingSelected(sel);
}

double applyKnobs(double fraction, double scale, double offset, bool wrap)
{
  // A fraction is only non-finite if a caller hands a broken ramp (an infinite span), and `sanitise`
  // already guarantees scale/offset are finite. Either way a NaN must not reach a pixel, because one
  // NaN in a row poisons the whole render and nothing downstream can tell it apart from the ramp.
  if (!std::isfinite(fraction))
    fraction = 0.0;

  double v = fraction * scale + offset;
  if (wrap) {
    v = v - std::floor(v);   // x - floor(x): inside [0,1), and an exact integer lands on 0.0
    // Rounding: for a |v| large enough that one ulp is bigger than the gap to the next integer, the
    // subtraction can land on exactly 1.0. The documented range is [0,1), and 1.0 *is* 0.0 of the next
    // period, so the endpoint is pinned to 0.0 rather than delivered as 1.0.
    if (!(v >= 0.0) || v >= 1.0)
      v = 0.0;
  }
  return std::isfinite(v) ? v : 0.0;
}

double uFraction(int col, const Options &opt, const Ramp &ramp)
{
  return applyKnobs((double(col) - ramp.x1) / ramp.w, opt.scaleU, opt.offsetU, opt.wrap);
}

double vFraction(int row, const Options &opt, const Ramp &ramp)
{
  // y is up in the reference rect, so `row == ramp.y1` is the bottom row. `flipV` hands the same
  // fractions out from the other end of that span: same multiset, opposite direction (the header
  // calls that out, and tests/uv_gen_test.cpp checks exactly that pair of statements).
  const double fraction = opt.flipV ? (ramp.y1 + ramp.h - 1.0 - double(row)) / ramp.h
                                    : (double(row) - ramp.y1) / ramp.h;
  return applyKnobs(fraction, opt.scaleV, opt.offsetV, opt.wrap);
}

int logicalChannel(int slot, int nchans)
{
  // The mapping GridWrap's render core uses (render.cpp's `logicalChannel`): a 1-component plane's
  // single channel *is* the alpha channel, so `channels a` selects it and a 1-channel output can only
  // carry u. Anything outside the four roles takes the alpha role, which is the safe fill.
  if (nchans == 1)
    return 3;
  if (slot >= 0 && slot < 4)
    return slot;
  return 3;
}

double fillForRole(int role)
{
  return role == 3 ? 1.0 : 0.0;   // opaque alpha, zeroed colour
}

void availableRoles(int nchans, bool out[4])
{
  out[0] = out[1] = out[2] = out[3] = false;
  if (nchans < 1)
    return;                      // nothing to filter against; the plugin refuses this before calling

  // Not a hand-written table: the planes that exist are exactly the roles `logicalChannel` says the
  // slots hold, so availability and the slot mapping cannot drift apart. nchans > 4 is clamped by the
  // loop bound because a slot index only means something inside the four roles.
  const int slots = nchans > 4 ? 4 : nchans;
  for (int slot = 0; slot < slots; ++slot)
    out[logicalChannel(slot, nchans)] = true;
}

void restrictSelection(bool sel[4], int nchans)
{
  bool avail[4];
  availableRoles(nchans, avail);
  if (!(avail[0] || avail[1] || avail[2] || avail[3]))
    return;   // no usable layout: leave the selection as it is, rather than inventing one

  // Pass 1: keep what was asked for and can be written. A role with no plane cannot be written *or*
  // filled, so keeping it selected only makes the knob look like it did nothing (that is how
  // `channels a` on an RGB delivery came to look like the node painting black).
  bool kept[4];
  for (int r = 0; r < 4; ++r)
    kept[r] = sel[r] && avail[r];

  // Pass 2: nothing writable was asked for, so ask for what this node is *for* — the backlog's pair —
  // restricted the same way. On an RGB delivery this is R=u, G=v, B filled: a UV map minus a channel
  // that buffer never had.
  if (!(kept[0] || kept[1] || kept[2] || kept[3])) {
    bool fallback[4];
    parseChannels(kDefaultChannels, fallback);
    for (int r = 0; r < 4; ++r)
      kept[r] = fallback[r] && avail[r];
  }

  // Pass 3: not even the default role has a plane here. A 1-component plane is the alpha role and the
  // only ramp it can carry is u, so write every plane it has rather than a constant.
  if (!(kept[0] || kept[1] || kept[2] || kept[3])) {
    for (int r = 0; r < 4; ++r)
      kept[r] = avail[r];
  }

  for (int r = 0; r < 4; ++r)
    sel[r] = kept[r];
}

SlotValue slotValue(int slot, int nchans, const Options &opt)
{
  // `opt.sel` has already been filtered against this buffer's planes (`Options::restrictTo`), so a role
  // named here always has a slot, and an unselected one is what fills.
  const int role = logicalChannel(slot, nchans);
  SlotValue out;
  if (!opt.sel[role]) {
    out.kind = kSlotFill;
    out.fill = fillForRole(role);
    return out;
  }
  // One fraction per role: green carries v, red, blue and alpha carry u.
  out.kind = role == 1 ? kSlotV : kSlotU;
  out.fill = 0.0;
  return out;
}

void fillRow(float *dst, int nchans, const Options &opt, const Ramp &ramp, int col0, int row, int count)
{
  if (!dst || nchans <= 0 || count <= 0)
    return;

  // Resolved once per row, not once per pixel: the ramp is the same across a row, and the only thing
  // that changes per pixel is the column. nchans is whatever the host resolved this clip to — the
  // descriptor describes RGBA, and a host intersects that with the connected Source's components, so 1,
  // 2, 3 and 4 all arrive (D-74) — and the scratch arrays stay on the stack either way.
  int kinds[4];
  float fills[4];
  for (int k = 0; k < nchans; ++k) {
    const SlotValue sv = slotValue(k, nchans, opt);
    kinds[k] = sv.kind;
    fills[k] = float(sv.fill);
  }

  const double v = vFraction(row, opt, ramp);
  for (int i = 0; i < count; ++i) {
    const double u = uFraction(col0 + i, opt, ramp);
    float *px = dst + std::size_t(i) * nchans;
    for (int k = 0; k < nchans; ++k) {
      if (kinds[k] == kSlotV)
        px[k] = float(v);
      else if (kinds[k] == kSlotU)
        px[k] = float(u);
      else
        px[k] = fills[k];
    }
  }
}

} // namespace uv
