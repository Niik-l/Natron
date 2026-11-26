// UVGen — OFX host-facing plugin shell (DECISIONS.md D-73, D-74).
//
// Emits a UV ramp: the fraction of the input's resolution at every output pixel, red = x/width,
// green = y/height (the backlog line **D-73** quotes verbatim is the spec of record, and D-73 states
// why there is no gizmo to port).
//
// The shell's whole job is to pick the ONE rect the fractions are stated against and hand it to the
// OFX-free core in uvcore.cpp, which decides every number the delivered slots hold (D-74). It never
// reads a Source pixel: this plugin's output is a function of geometry alone, so Source is a
// resolution and format donor, nothing more — which is also why it takes no mask (a mask would mean
// blending with a source it never needs to read; ARCHITECTURE §8).
//
// The reference rect, in priority order (D-74), is: the Source clip's region of definition, else the
// project rect the host published for this clip, else the bounds of the Output image the host
// allocated. What it is *not* is this node's own Output RoD: GridWrap learned that asking a node's own
// Output clip for its RoD re-enters that node's own RoD action (D-68), and a generator is the worst
// case for that — the action has nothing to descend into. Nor is it a tile's bounds, which is why the
// descriptor answers `setSupportsTiles(false)`: a tiled host would report a per-tile RoD, and the ramp
// would restart inside each tile instead of spanning the frame once.

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.H"

#include "uvcore.h"

namespace {

/** @brief A rect the host reported, in fractions or in whole pixels, as the core models it.
  *
  * A widening, not a promise: `uv::rectUsable` is what decides whether a rect may be used, because a
  * host property is a number a host may not publish at all (the Support getters throw on a missing
  * one) or may publish as garbage — the GridWrap idiom of guarding every host read (D-66). */
uv::Rect rectFrom(const OfxRectD &r)
{
  return uv::Rect(r.x1, r.y1, r.x2, r.y2);
}

uv::Rect rectFrom(const OfxRectI &r)
{
  return uv::Rect(double(r.x1), double(r.y1), double(r.x2), double(r.y2));
}

} // namespace

////////////////////////////////////////////////////////////////////////////////
// the pass that writes the ramp

/** @brief Paints the UV ramp into the Output image, one row at a time.
 *
 * `ImageProcessor::process()` hands each thread a horizontal band of the render window and slices only
 * along y (ofxsProcessing.H:79-97), which is exactly the shape of this work: a texel's u depends on x
 * alone and its v on y alone, so a band needs nothing from another band. The rect, the ramp and the
 * knobs were settled once on the main thread before this object was built, so a worker thread touches
 * no host property and no parameter — `_ramp` and `_opt` are copies, not references into state a host
 * could still be editing.
 *
 * Rows go through `Image::getPixelAddress`, which is bounds-checked and per-row safe for whatever row
 * stride the host chose, including a negative one (kOfxImagePropRowBytes). `uv::fillRow` writes exactly
 * `nchans` interleaved floats per pixel, which is the layout of such a row; `render` has already
 * rejected a depth this plugin does not write, so the row is float32 and `nchans` is 1, 2, 3 or 4 — the
 * component count of a plane the descriptor described (D-74). */
class UVGenProcessor : public OFX::ImageProcessor {
public:
  UVGenProcessor(OFX::ImageEffect &effect, const uv::Ramp &ramp, const uv::Options &opt)
    : OFX::ImageProcessor(effect), _opt(opt), _ramp(ramp) {}

  virtual void multiThreadProcessImages(OfxRectI window)
  {
    if (window.x1 >= window.x2 || window.y1 >= window.y2 || !_dstImg)
      return;

    const int nchans = _dstImg->getPixelComponentCount();
    if (nchans < 1 || nchans > 4)
      return; // a layout `describeInContext` never described: write nothing rather than guess a stride

    for (int y = window.y1; y < window.y2; ++y) {
      if (_effect.abort())
        break;
      float *row = static_cast<float *>(_dstImg->getPixelAddress(window.x1, y));
      if (!row)
        continue; // a row the host will not hand out: the rest of the band is still its own business
      uv::fillRow(row, nchans, _opt, _ramp, window.x1, y, window.x2 - window.x1);
    }
  }

private:
  UVGenProcessor(const UVGenProcessor &other);
  UVGenProcessor &operator=(const UVGenProcessor &other);

  uv::Options _opt; // by value: the worker threads share nothing but the read-only ramp
  uv::Ramp _ramp;
};

////////////////////////////////////////////////////////////////////////////////
// plugin instance

class UVGenPlugin : public OFX::ImageEffect {
protected:
  // do not need to delete these, the ImageEffect is managing them for us
  OFX::Clip *dstClip_;
  OFX::Clip *srcClip_; // described optional: a resolution and format donor, never a pixel source

  OFX::StringParam *channels_;
  OFX::DoubleParam *scaleU_, *scaleV_, *offsetU_, *offsetV_;
  OFX::BooleanParam *flipV_, *wrap_;

  mutable bool _keyframed; // sticky, the TimeWarp / ZDefocusZ reading (D-21)

public:
  UVGenPlugin(OfxImageEffectHandle handle)
    : OFX::ImageEffect(handle), dstClip_(0), srcClip_(0), _keyframed(true)
  {
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);

    channels_ = fetchStringParam("channels");
    scaleU_   = fetchDoubleParam("scaleU");
    scaleV_   = fetchDoubleParam("scaleV");
    offsetU_  = fetchDoubleParam("offsetU");
    offsetV_  = fetchDoubleParam("offsetV");
    flipV_    = fetchBooleanParam("flipV");
    wrap_     = fetchBooleanParam("wrap");
  }

  virtual void render(const OFX::RenderArguments &args) override;
  virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &,
                                    OFX::RegionOfInterestSetter &rois) override
  {
    if (srcClip_)
      rois.setRegionOfInterest(*srcClip_, OfxRectD{ 0., 0., 0., 0. });
  }

  /** @brief The ramp's own rect, rounded out to whole texels — declared non-const because that is how
    * the Support library declares the virtual (ofxsImageEffect.h:1111), so a `const` override here
    * would silently define a new function and leave the action unanswered. */
  virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) override;

private:
  // The knob table, read through the core's own Options and clamped by its `sanitise()`, so the numbers
  // the render uses are the numbers the knob table documents (the ZDefocusZ idiom).
  uv::Options readOptions(double time) const;

  // The two rect candidates that come from a host, each usable or not. Both answer the out-param and
  // return whether it may be used, because a missing host property is information, not a fault.
  bool sourceRod(double time, uv::Rect &out) const;
  bool projectRect(uv::Rect &out) const;

  double readDouble(OFX::DoubleParam *knob, double t) const;
};

/** @brief One double knob, read at @p t when this host interpolates and statically when it does not (the
  * TimeWarp / ZDefocusZ precedent, D-21): the Support library's timed getter throws on a host without
  * keyframe interpolation, so the refusal is noticed once per instance and every knob is then simply
  * constant. A UV ramp is usually static, but a scrolling `offsetU` is how this node drives a texture
  * pan, so it is keyframable. */
double UVGenPlugin::readDouble(OFX::DoubleParam *knob, double t) const
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

/** @brief The knobs as the core wants them.
 *
 * `channels` is read as a constant: it decides which slot carries which ramp value, so a keyframed
 * version of it would be a different image at every frame with nothing in the descriptor to say so.
 * `flipV` and `wrap` likewise, the way ZDefocusZ reads its flags (D-38). The four numbers are read at
 * @p time and then clamped by `sanitise()`, which is also what makes a selection naming nothing come
 * out as the documented default. */
uv::Options UVGenPlugin::readOptions(double time) const
{
  uv::Options opt;

  std::string value;
  channels_->getValue(value);
  uv::parseChannels(value, opt.sel);

  opt.flipV = flipV_->getValue();
  opt.wrap  = wrap_->getValue();

  opt.scaleU  = readDouble(scaleU_, time);
  opt.scaleV  = readDouble(scaleV_, time);
  opt.offsetU = readDouble(offsetU_, time);
  opt.offsetV = readDouble(offsetV_, time);

  opt.sanitise();
  return opt;
}

/** @brief The Source clip's region of definition, if there is a Source and the host will say what it is.
 *
 * This is the "current input resolution" of the backlog line, and it is the only candidate that knows
 * what the input actually is. It is asked of the **Source** clip and never of the Output clip: GridWrap
 * measured what happens when a node asks its own output for its RoD — the host answers by asking the
 * clip's associated node, which for an output clip is this very effect, so the question re-enters this
 * plugin's own RoD action until the stack is gone (D-68, measured on Natron 2.6). A generator is the
 * worst case for that cycle because the action it re-enters has nothing to descend into.
 *
 * The Support library's `Clip::getRegionOfDefinition` rethrows the suite status when a host refuses the
 * call, so this too is a guarded read: a host that will not answer for a clip is a candidate that does
 * not apply, not an error. */
bool UVGenPlugin::sourceRod(double time, uv::Rect &out) const
{
  if (!srcClip_ || !srcClip_->isConnected())
    return false; // standing alone as a generator: the input has no resolution to contribute

  try {
    out = rectFrom(srcClip_->getRegionOfDefinition(time));
  } catch (const std::exception &) {
    return false;
  }
  return uv::rectUsable(out);
}

/** @brief The rect the host published for this clip, in the same order GridWrap asks it (D-66).
 *
 * `kOfxImageEffectPropProjectOffset` / `…ProjectSize` / `…ProjectExtent` are instance properties a host
 * answers from the **project** — in Natron, `getProjectDefaultFormat()` (OfxImageEffectInstance.cpp:319-
 * 377) — so answering them involves no node, no action and no cache, which is what makes them safe to
 * ask where an output RoD is not. Each is asked inside its own `try`: the Support getters default to
 * `throwOnFailure = true`, so a host that does not carry a property throws, and nothing may escape an
 * entry point (AGENTS.md rule 1).
 *
 * Offset plus size first, because that pair keeps a frame that does not start at the origin (Nuke's
 * format x1); an extent alone is rooted at 0,0 (ofxImageEffect.h:765-800). */
bool UVGenPlugin::projectRect(uv::Rect &out) const
{
  try {
    const OfxPointD off = getProjectOffset();
    const OfxPointD size = getProjectSize();
    if (std::isfinite(off.x) && std::isfinite(off.y) &&
        std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0 && size.y > 0.0) {
      out = uv::Rect(off.x, off.y, off.x + size.x, off.y + size.y);
      if (uv::rectUsable(out))
        return true;
    }
  } catch (const std::exception &) {
  } // this host has no project size to say: ask the extent

  try {
    const OfxPointD ext = getProjectExtent();
    out = uv::Rect(0.0, 0.0, ext.x, ext.y); // rooted at 0,0
  } catch (const std::exception &) {
    return false;
  }
  return uv::rectUsable(out);
}

/** @brief Fill the render window with the ramp stated against the reference rect.
 *
 * The order is: the Output the host asked for, then what it may not be, then the rect the fractions are
 * stated against, then the pass. The order matters because the two refusals are the cheap ones and a
 * generator with no input connected has no clip format to argue about. */
void UVGenPlugin::render(const OFX::RenderArguments &args)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  std::unique_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
  if (!dst)
    OFX::throwSuiteStatusException(kOfxStatFailed);

  // float32 only, v1 (D-07), and only the layouts the descriptor described. The Output's component
  // count is what decides which role lands in which slot (uv::logicalChannel, the GridWrap D-57
  // convention), so an unexpected count is refused rather than mapped by guesswork. The count is also
  // what the channel selection is filtered against a few lines below, and it is *not* 4 just because
  // the descriptor describes RGBA: a host resolves a clip's components as an intersection with the
  // connected Source's (measured: an alpha-less PNG Source delivers a 3-component opaque Output).
  if (dst->getPixelDepth() != OFX::eBitDepthFloat)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
  const int nchans = dst->getPixelComponentCount();
  if (nchans < 1 || nchans > 4)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

  // A connected Source may disagree about what it can deliver, the way SharpenZ refuses an unpremultiplied
  // mask (D-05/D-11): the clip's format is what the node's shape was resolved from, and a format the
  // plugin cannot write is a host asking for something the descriptor never promised. Its pixels are still
  // never read — this plugin's output is geometry, not samples. The guard is gated on the connection
  // because the clip is optional: an unconnected clip's resolved format is not something to argue about.
  if (srcClip_ && srcClip_->isConnected()) {
    const OFX::PixelComponentEnum comps = srcClip_->getPixelComponents();
    if (comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB &&
        comps != OFX::ePixelComponentAlpha)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    if (srcClip_->getPixelDepth() != OFX::eBitDepthFloat)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported); // no depth conversion in v1
  }

  const OfxRectI bounds = dst->getBounds();

  // The reference rect, in the priority order D-74 states, and never a tile's bounds — the descriptor
  // answers setSupportsTiles(false) precisely so a host cannot hand this action a per-tile rect and make
  // the ramp restart inside each tile instead of spanning the frame once.
  uv::Rect ref;
  if (!sourceRod(args.time, ref) && !projectRect(ref)) {
    ref = rectFrom(bounds); // the last candidate is the one a host always answered: the output's bounds
    if (!uv::rectUsable(ref))
      throw std::runtime_error("UVGen: no usable rect to state a UV ramp against");
  }

  // Paint the window the host asked for, clipped to the image the host allocated. `ImageProcessor`
  // refuses an empty window by looping over no rows, and `getPixelAddress` is bounds-checked, but an
  // empty window is also a host asking for nothing inside a frame-sized rect — and only a painted pixel
  // carries a ramp value, so this returns without painting (D-74).
  OfxRectI window = args.renderWindow;
  window.x1 = std::max(window.x1, bounds.x1);
  window.y1 = std::max(window.y1, bounds.y1);
  window.x2 = std::min(window.x2, bounds.x2);
  window.y2 = std::min(window.y2, bounds.y2);
  if (window.x1 >= window.x2 || window.y1 >= window.y2)
    return;

  uv::Options opt = readOptions(args.time);

  // The selection is then resolved against the planes this buffer actually has (**D-74**): naming a role
  // with no plane is not half-applied, it re-resolves, so `channels a` on a delivery that has no alpha
  // plane delivers a UV map instead of a black picture. Doing it here, at the one place that knows the
  // delivered layout, is what keeps `slotValue` and `fillRow` layout-blind.
  opt.restrictTo(nchans);

  const uv::Ramp ramp = uv::resolveRamp(ref);

  UVGenProcessor processor(*this, ramp, opt);
  processor.setDstImg(dst.get());
  processor.setRenderWindow(window);
  processor.process();

}

/** @brief The area this node's output covers, rounded out to whole texels.
 *
 * Answering with the ramp's rect rather than saying nothing is what keeps the Output RoD and the painted
 * ramp the same rect: the core floors and ceils the reference rect, so the RoD is the texel grid the
 * values were counted on. With no Source connected there is nothing to widen, and `false` is the Support
 * default — this plugin's RoD is its output's bounds (the ZDefocusZ pattern, D-44). */
bool UVGenPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  uv::Rect ref;
  if (!sourceRod(args.time, ref))
    return false;

  const uv::Ramp ramp = uv::resolveRamp(ref);
  rod = OfxRectD{ ramp.x1, ramp.y1, ramp.x1 + ramp.w, ramp.y1 + ramp.h };
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// factory

mDeclarePluginFactory(UVGenPluginFactory, {}, {});

using namespace OFX;

void UVGenPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  desc.setLabels("UVGen", "UVGen",
                 "Generates a UV map image at the current input resolution: red and blue carry x/width, "
                 "green carries y/height, measured from the input's region of definition in OFX space. "
                 "Scale, offset, wrap and a flip are provided for wiring ramps into other nodes.");
  desc.setPluginGrouping("OFX Lab");

  // General first, so a host that takes the first declared context (Natron does) gets the one with the
  // optional Source; Filter is what makes this node appear in a filter search. Same order as SharpenZ.
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedContext(eContextFilter);

  // v1 scope: float32 only, the same bound every plugin in this repo carries (D-07). A descriptor that
  // declares no bit depth describes a plugin that can deliver nothing, and hosts are free to act on
  // that — this one is mandatory in the Support library's own examples.
  desc.addSupportedBitDepth(eBitDepthFloat);

  // Tiles are answered false at effect level too, so no host slices this node's render (D-74); multiple
  // resolution is left at the Support default (true) because the ramp is stated against a rect, not a
  // fixed size, so it scales with whatever format a host resolves the node to.
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void UVGenPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
  // Source clip: optional, because a UV ramp needs no image to exist. Connected, it is what "current
  // input resolution" means; unconnected, the node is a generator and the rect comes from the host's
  // project or the output's bounds (D-74).
  OFX::ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentRGB);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setOptional(true);
  srcClip->setSupportsTiles(false); // a per-tile clip RoD would restart the ramp per tile
  srcClip->setIsMask(false);

  // Output clip, described in every supported context: Natron dereferences getClip("Output") under an
  // assert() alone, so an undescribed Output crashes the host the moment the node is created (D-28).
  OFX::ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(ePixelComponentRGBA);
  dstClip->addSupportedComponent(ePixelComponentRGB);
  dstClip->addSupportedComponent(ePixelComponentAlpha);
  dstClip->setSupportsTiles(false);

  // Effect settings: no mask input and no mix, because there is nothing to dissolve an effect away
  // from — this node never reads a source pixel (ARCHITECTURE §8). No knob group either: seven knobs
  // that all belong to one ramp do not need a folder.
  OFX::StringParamDescriptor *channels = desc.defineStringParam("channels");
  channels->setLabels("Channels", "Channels",
                      "Which channels carry the ramp: r and b and a carry u = x/width, g carries "
                      "v = y/height. Accepts r g b a in any combination, all or rgba for every "
                      "channel, none for no ramp. Empty, none or an unrecognised string falls back "
                      "to rg, the backlog's red-and-green map. A role this output has no plane for is "
                      "dropped rather than half-applied: an RGB delivery has no alpha channel, and a "
                      "one-plane delivery carries only u.");
  channels->setStringType(eStringTypeSingleLine);
  channels->setDefault(uv::kDefaultChannels);

  OFX::BooleanParamDescriptor *flipV = desc.defineBooleanParam("flipV");
  flipV->setLabels("Flip V", "FlipV",
                   "Run v from the top row down instead of the bottom row up. OFX y grows upward, so "
                   "the default counts v from the bottom row; this is the texture-map order, where v "
                   "grows downward and the first row of the image holds v = 0.");
  flipV->setDefault(false);

  OFX::DoubleParamDescriptor *scaleU = desc.defineDoubleParam("scaleU");
  scaleU->setLabels("Scale U", "ScaleU", "Multiply u by this before it is wrapped.");
  scaleU->setRange(uv::kMinScale, uv::kMaxScale);
  scaleU->setDisplayRange(uv::kMinScale, uv::kMaxScale);
  scaleU->setDefault(uv::kDefaultScale);

  OFX::DoubleParamDescriptor *scaleV = desc.defineDoubleParam("scaleV");
  scaleV->setLabels("Scale V", "ScaleV", "Multiply v by this before it is wrapped.");
  scaleV->setRange(uv::kMinScale, uv::kMaxScale);
  scaleV->setDisplayRange(uv::kMinScale, uv::kMaxScale);
  scaleV->setDefault(uv::kDefaultScale);

  OFX::DoubleParamDescriptor *offsetU = desc.defineDoubleParam("offsetU");
  offsetU->setLabels("Offset U", "OffsetU",
                     "Add this to the scaled u, in whole UV units (1.0 shifts by one full ramp).");
  offsetU->setRange(uv::kMinOffset, uv::kMaxOffset);
  offsetU->setDisplayRange(uv::kMinOffset, uv::kMaxOffset);
  offsetU->setDefault(uv::kDefaultOffset);

  OFX::DoubleParamDescriptor *offsetV = desc.defineDoubleParam("offsetV");
  offsetV->setLabels("Offset V", "OffsetV",
                     "Add this to the scaled v, in whole UV units (1.0 shifts by one full ramp).");
  offsetV->setRange(uv::kMinOffset, uv::kMaxOffset);
  offsetV->setDisplayRange(uv::kMinOffset, uv::kMaxOffset);
  offsetV->setDefault(uv::kDefaultOffset);

  OFX::BooleanParamDescriptor *wrap = desc.defineBooleanParam("wrap");
  wrap->setLabels("Wrap", "Wrap",
                  "Fold u and v back into [0,1) with x - floor(x), so scale and offset repeat the "
                  "ramp. Off leaves the scaled values as they are, including outside [0,1] and "
                  "below zero.");
  wrap->setDefault(false);
}

OFX::ImageEffect *UVGenPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context)
{
  return new UVGenPlugin(handle);
}

namespace OFX {
namespace Plugin {
  void getPluginIDs(OFX::PluginFactoryArray &ids)
  {
    static UVGenPluginFactory p("net.ofxlab.uvgen", 1, 0);
    ids.push_back(&p);
  }
}
}
