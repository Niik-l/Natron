/*
  SharpenZ — OFX Lab.

  Gizmo node graph -> our single-pass pipeline:

    Unpremult1                 unpremult knob (divide RGB by A where A > 0)
    Remove1 + Merge1(copy)     "channels" knob: restrict processing to the
                               selected channels; the rest pass through.
                               (We implement the user-visible semantics:
                               sharpen selected channels, merge back.)
    Saturation1 (luma mode)    working signal L = (R+G+B)/3
    Blur1 (box, size r/q)      prefiltered copy when radius/q > 1
    BlinkScript FastWeightedBlur
        Radius  = max(radius/quality, 1)          (float)
        Quality = int(min(quality, radius))       (samples per axis)
        grid samples at (int(R*i), int(R*j)), i,j in [-Q, Q], edge-clamped
        weight w = 1 / (1 + (bias*(sample - src))^2)
        Sb = sum(w*sample) / sum(w)
    MergeExpression2           out = Sb == 0 ? 0 : (S/Sb)^(sharpen+1) * Sb
                               (alpha handled the same way when selected)
    MergeExpression1 (luma)    RGB' = L == 0 ? 0 : RGB * L'/L   (hue kept)
    Merge2 (copy)              only the selected channels are replaced
    Premult1                   re-premultiply when unpremult is on
    (SharpenZ addition)        out = src + mix * mask * (effect - src),
                               premultiplied space, mask = Mask clip alpha
                               (or the channel Natron's mask selector hands
                               us), clamped to [0,1]; maskInvert flips it.

  Notes on faithful edge behaviour:
  - Blink's eEdgeClamped => samples outside the gathered image clamp to the
    nearest available value (no darkening at edges).
  - pow() is only evaluated for a positive base (Nuke's expression engine
    yields 0 for negative base / fractional exponent); out becomes 0 there.
  - Nuke's box filter of float size S: weight 1 for |d| <= floor(S) and
    weight S - floor(S) at d = ±ceil(S), normalised by 2S+1. Implemented
    separably in two passes.

  Notes on the Mask input (D-20):
  - Declared only in the General context (Natron's filter context forbids
    masks; Natron prefers General when both are declared).
  - The clip is optional and alpha-only: Natron adds its standard
    "enable mask" checkbox + mask-channel selector and delivers the chosen
    channel as an alpha image; Nuke/Resolve feed alpha as well.
  - Unconnected mask = fully on. Pixels outside the mask image = 0.
  - t = mix * (maskInvert ? 1-mask : mask); out = src + t*(effect - src)
    per component, premultiplied space. t==1 reproduces the pure gizmo
    output bit-for-bit; mix==0 skips the whole filter (straight source copy).

  v1 scope: float32 only (DECISIONS.md D-07), CPU only (D-06).
*/

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

#include "ofxlab_maskmix.h"   // mask/mix dissolve, shared with TimeWarp (D-20)
#include "ofxlab_channels.h"   // "channels" knob parser, shared header (D-06, D-17)

////////////////////////////////////////////////////////////////////////////////
// effective parameters (mirrors the gizmo's TCL expressions)

struct Params {
  double sharpen = 0.5;   // gain exponent is sharpen + 1 (MergeExpression2)
  double radius  = 100.0;
  double bias    = 40.0;
  int quality    = 15;
  bool lumaOnly  = true;
  bool unpremult = true;
  double mix     = 1.0;   // SharpenZ: dissolve to source at 0 (D-20)
  bool maskInvert = false;
  bool sel[4]    = {true, true, true, false}; // "channels" knob, default rgb

  // FastWeightedBlur_Radius = max(radius/quality, 1)
  double stride() const { return std::max(radius / (double)quality, 1.0); }

  // FastWeightedBlur_Quality = int(min(quality, radius))
  int samples() const { return (int)std::min((double)quality, radius); }

  // Blur1: disable {{parent.radius/parent.quality <= 1}}
  bool blurActive() const { return radius / (double)quality > 1.0; }

  // source gather half-extent: grid span int(stride*Q) plus the box-blur
  // gather radius (only when the prefilter blur is active), +1 slack so the
  // separable blur never hits the scratch-buffer edge for relevant pixels
  int pad() const {
    int p = (int)std::ceil(stride() * samples());
    if (blurActive())
      p += (int)std::ceil(stride());
    return p + 1;
  }
};

// MergeExpression2 guard: Sb==0 ? 0 : (S/Sb)^k * Sb, pow only for base > 0.
static inline float gainSharpen(float S, float Sb, double k)
{
  if (!(Sb > 0.0f))
    return 0.0f;
  double base = (double)S / (double)Sb;
  if (!(base > 0.0))
    return 0.0f;
  return (float)(std::pow(base, k) * (double)Sb);
}

static inline int clampi(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// Mask/mix helpers live in common/ofxlab_maskmix.h (D-20), shared with TimeWarp.
using OfxLab::maskMix;
using OfxLab::blendOut;

// Nuke "box" filter of float size R, applied separably with edge-clamped
// sampling: each tap samples at clamp(x+d) and the result is renormalised by
// the *nominal* kernel weight (2R+1); clamped taps repeat the edge value, so
// a constant signal stays constant (no dark edges — the gizmo's goal).
// Tap weights: 1 for |d| <= floor(R), R - floor(R) at d = ±(floor(R)+1).
// Implemented with sliding prefix sums: O(1) per pixel per axis.
static void boxBlurPlane(const float* inRaw, float* out, int W, int H, double R)
{
  // Local fix: non-finite pixels count as 0, so they cannot poison the running sums.
  std::vector<float> clean(inRaw, inRaw + (size_t)W * H);
  for (size_t i = 0; i < clean.size(); ++i)
    if (!std::isfinite(clean[i]))
      clean[i] = 0.f;
  const float* in = clean.data();
  const int ki = (int)std::floor(R);
  const double f = R - (double)ki;
  const double invDenom = 1.0 / (2.0 * R + 1.0);

  std::vector<double> tmp((size_t)W * H);
  std::vector<double> pre((size_t)std::max(W, H) + 1);

  // horizontal pass
  for (int y = 0; y < H; ++y) {
    const float* row = in + (size_t)y * W;
    pre[0] = 0.0;
    for (int i = 0; i < W; ++i)
      pre[i + 1] = pre[i] + (double)row[i];

    double* outRow = tmp.data() + (size_t)y * W;
    for (int i = 0; i < W; ++i) {
      // sum of taps d in [-ki, ki] sampled at clamp(i+d): the clamped runs
      // repeat the edge values, counted with their multiplicity
      const int lo = clampi(i - ki, 0, W - 1);
      const int hi = clampi(i + ki, 0, W - 1);
      const int leftRep = (i - ki < 0) ? (ki - i) : 0;   // taps pinned at 0
      const int rightRep = (i + ki > W - 1) ? (i + ki - (W - 1)) : 0;
      double val = (double)leftRep * row[0] + (pre[hi + 1] - pre[lo]) +
                   (double)rightRep * row[W - 1];
      if (f > 0.0)
        val += f * (row[clampi(i - ki - 1, 0, W - 1)] +
                    row[clampi(i + ki + 1, 0, W - 1)]);
      outRow[i] = val * invDenom;
    }
  }

  // vertical pass
  for (int x = 0; x < W; ++x) {
    pre[0] = 0.0;
    for (int j = 0; j < H; ++j)
      pre[j + 1] = pre[j] + tmp[(size_t)j * W + x];

    for (int j = 0; j < H; ++j) {
      const int lo = clampi(j - ki, 0, H - 1);
      const int hi = clampi(j + ki, 0, H - 1);
      const int topRep = (j - ki < 0) ? (ki - j) : 0;
      const int botRep = (j + ki > H - 1) ? (j + ki - (H - 1)) : 0;
      double val = (double)topRep * tmp[x] + (pre[hi + 1] - pre[lo]) +
                   (double)botRep * tmp[(size_t)(H - 1) * W + x];
      if (f > 0.0)
        val += f * (tmp[(size_t)clampi(j - ki - 1, 0, H - 1) * W + x] +
                    tmp[(size_t)clampi(j + ki + 1, 0, H - 1) * W + x]);
      out[(size_t)j * W + x] = (float)(val * invDenom);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// kernel

class SharpenBase : public OFX::ImageProcessor {
protected:
  OFX::Image *_srcImg;
  OFX::Image *_maskImg; // optional Mask clip image, may be null
  int _maskComp;        // component index to read from the mask image
  Params _p;

public:
  SharpenBase(OFX::ImageEffect &instance, const Params &p)
    : OFX::ImageProcessor(instance), _srcImg(0), _maskImg(0), _maskComp(0), _p(p) {}

  void setSrcImg(OFX::Image *v) { _srcImg = v; }
  void setMaskImg(OFX::Image *v, int comp)
  {
    _maskImg = v;
    _maskComp = comp;
  }
};

template <class PIX, int nComponents>
class SharpenProcessor : public SharpenBase {
public:
  SharpenProcessor(OFX::ImageEffect &instance, const Params &p) : SharpenBase(instance, p) {}

  void multiThreadProcessImages(OfxRectI procWindow) override
  {
    const int x1 = procWindow.x1, x2 = procWindow.x2;
    const int y1 = procWindow.y1, y2 = procWindow.y2;

    const Params &p = _p;
    // in a single-channel stream the gizmo's channel selection is moot
    bool sel[nComponents];
    for (int c = 0; c < nComponents; ++c)
      sel[c] = (nComponents == 4) ? p.sel[c] : true;
    const bool luma = p.lumaOnly && nComponents == 4;

    const OfxRectI srcB = _srcImg ? _srcImg->getBounds() : OfxRectI{0, 0, 0, 0};
    const bool haveSrc = _srcImg && srcB.x2 > srcB.x1 && srcB.y2 > srcB.y1;

    // The dissolve weight is 0 everywhere -> the output is the source, so skip
    // the whole filter. That happens at mix == 0, and also with maskInvert on and
    // no mask connected: an unconnected mask is fully on (D-20), so inverting it
    // turns the effect off completely (D-29). The copy uses the same
    // edge-clamped sampling as the main loop below so the output is continuous
    // across the shortcut.
    OfxLab::MaskReader maskReader;
    maskReader.set(_maskImg, _maskComp);
    const bool haveMask = maskReader.active();
    if (p.mix <= 0.0 || (p.maskInvert && !haveMask)) {
      for (int y = y1; y < y2; ++y) {
        if (_effect.abort())
          break;
        PIX *dstPix = (PIX *)_dstImg->getPixelAddress(x1, y);
        for (int x = x1; x < x2; ++x, dstPix += nComponents) {
          if (!haveSrc) {
            for (int c = 0; c < nComponents; ++c)
              dstPix[c] = (PIX)0;
            continue;
          }
          const PIX *row = (const PIX *)_srcImg->getPixelAddress(srcB.x1, clampi(y, srcB.y1, srcB.y2 - 1));
          const PIX *px = row + (size_t)(clampi(x, srcB.x1, srcB.x2 - 1) - srcB.x1) * nComponents;
          for (int c = 0; c < nComponents; ++c)
            dstPix[c] = px[c];
        }
      }
      return;
    }

    const int pad = p.pad();
    const double stride = p.stride();
    const int Q = p.samples();
    const bool doBlur = p.blurActive();
    const double kExp = p.sharpen + 1.0;
    const double bias = p.bias;

    const int bx1 = x1 - pad, bx2 = x2 + pad;
    const int by1 = y1 - pad, by2 = y2 + pad;
    const int W = bx2 - bx1, H = by2 - by1;
    const size_t plane = (size_t)W * H;

    // grid sample offsets, int() truncates toward zero like the gizmo
    std::vector<int> off(2 * Q + 1);
    for (int i = -Q; i <= Q; ++i)
      off[i + Q] = (int)(stride * i);

    // working signal: unpremultiplied (when unpremult is on); grey when luma
    const int nBuf = luma ? 1 : nComponents;
    std::vector<float> work((size_t)nBuf * plane);
    std::vector<float> blurred(doBlur ? (size_t)nBuf * plane : 0);

    for (int y = 0; y < H; ++y) {
      const int wy = by1 + y;
      const int cy = clampi(wy, srcB.y1, srcB.y2 - 1);
      const PIX *row = haveSrc
                         ? (const PIX *)_srcImg->getPixelAddress(srcB.x1, cy)
                         : nullptr;

      for (int x = 0; x < W; ++x) {
        if (!row)
          continue; // all-zero working values

        const int cx = clampi(bx1 + x, srcB.x1, srcB.x2 - 1);
        const PIX *px = row + (size_t)(cx - srcB.x1) * nComponents;

        float v[4];
        for (int c = 0; c < nComponents; ++c)
          v[c] = (float)px[c];

        const float a = (nComponents == 4) ? v[3] : 1.0f;
        if (p.unpremult && a > 0.0f) {
          v[0] /= a;
          if (nComponents == 4) {
            v[1] /= a;
            v[2] /= a;
          }
        }

        const size_t idx = (size_t)y * W + x;
        if (luma) {
          work[idx] = (v[0] + v[1] + v[2]) * (1.0f / 3.0f);
        } else {
          for (int c = 0; c < nComponents; ++c)
            work[(size_t)c * plane + idx] = v[c];
        }
      }
    }

    // prefiltered copy (gizmo Blur1: box filter, size radius/quality)
    if (doBlur) {
      for (int c = 0; c < nBuf; ++c)
        boxBlurPlane(work.data() + (size_t)c * plane,
                     blurred.data() + (size_t)c * plane, W, H, stride);
    }
    const float *B = doBlur ? blurred.data() : work.data();

    // per output pixel: weighted blur Sb, gain, recombine, re-premultiply,
    // then the mask/mix dissolve
    for (int y = y1; y < y2; ++y) {
      if (_effect.abort())
        break;

      PIX *dstPix = (PIX *)_dstImg->getPixelAddress(x1, y);
      const int ly = y - by1;

      const PIX *srcRow = haveSrc
                            ? (const PIX *)_srcImg->getPixelAddress(srcB.x1, clampi(y, srcB.y1, srcB.y2 - 1))
                            : nullptr;

      for (int x = x1; x < x2; ++x, dstPix += nComponents) {
        if (!haveSrc) {
          // no source here: black and transparent (gizmo: outside format = 0)
          for (int c = 0; c < nComponents; ++c)
            dstPix[c] = (PIX)0;
          continue;
        }

        const int lx = x - bx1;
        const size_t lidx = (size_t)ly * W + lx;

        // original (premultiplied) source pixel, for pass-through channels
        // and for the dissolve base
        const PIX *px = srcRow + (size_t)(clampi(x, srcB.x1, srcB.x2 - 1) - srcB.x1) * nComponents;

        // weighted blur + gain for one plane (component 0 when luma)
        auto blurGain = [&](const float *planeWork, const float *planeBlur, size_t i) {
          const float S = planeWork[i];
          double sum = 0.0, sumW = 0.0;
          for (int dj : off) {
            int by = ly + dj;
            if (by < 0) by = 0; else if (by >= H) by = H - 1;
            const float *brow = planeBlur + (size_t)by * W;
            for (int dx : off) {
              int bxi = lx + dx;
              if (bxi < 0) bxi = 0; else if (bxi >= W) bxi = W - 1;
              const double sample = (double)brow[bxi];
              const double d = bias * (sample - (double)S);
              const double w = 1.0 / (1.0 + d * d);
              sum += sample * w;
              sumW += w;
            }
          }
          const float Sb = sumW > 0.0 ? (float)(sum / sumW) : S;
          return gainSharpen(S, Sb, kExp);
        };

        // alpha is never divided by unpremult, so the raw source alpha works
        // for both the divide and the final re-premultiply
        const float alphaWork = (nComponents == 4) ? (float)px[3] : 1.0f;

        float eff[4]; // pure effect output (what the gizmo alone would write)

        if (luma) {
          // gizmo: Saturation1 -> FastWeightedBlur -> MergeExpression2 on L,
          // then MergeExpression1: RGB' = L == 0 ? 0 : RGB * L'/L
          const float L = work[lidx];
          const float Lgain = blurGain(work.data(), B, lidx);

          for (int c = 0; c < 3; ++c) {
            float out = 0.0f;
            if (L > 0.0f) {
              out = (float)px[c]; // original (premultiplied) source
              if (p.unpremult && alphaWork > 0.0f)
                out /= alphaWork; // MergeExpression1 "Ar" is unpremultiplied
              out *= Lgain / L;
              if (p.unpremult)
                out *= alphaWork; // Premult1
            }
            eff[c] = out;
          }
          eff[3] = px[3]; // MergeExpression1 expr3: alpha passes through
        } else {
          // pass 1: all channels in the working (possibly unpremultiplied) space
          float vals[4];
          bool proc[4];
          float outAlphaWork = alphaWork;

          for (int c = 0; c < nComponents; ++c) {
            if (sel[c]) {
              vals[c] = blurGain(work.data() + (size_t)c * plane,
                                 B + (size_t)c * plane, lidx);
              proc[c] = true;
              if (nComponents == 4 && c == 3)
                outAlphaWork = vals[c]; // sharpened alpha drives the premult
            } else {
              vals[c] = (float)px[c]; // untouched channels pass through premult
              proc[c] = false;
            }
          }

          // pass 2: re-premultiply the processed colour (Premult1)
          for (int c = 0; c < nComponents; ++c) {
            if (proc[c] && p.unpremult && nComponents == 4 && c < 3)
              vals[c] *= outAlphaWork;
            eff[c] = vals[c];
          }
        }

        // mask/mix dissolve (D-20), premultiplied space:
        //   out = src + mix * (invert ? 1-mask : mask) * (effect - src)
        // unprocessed channels have effect == src and are therefore unaffected.
        // An unconnected mask is fully on, so maskInvert without one is handled
        // by the shortcut above (D-29) and the plain mix applies here.
        double t = p.mix;
        if (haveMask)
          t = maskMix(p.mix, maskReader.valueAt(x, y), p.maskInvert);
        for (int c = 0; c < nComponents; ++c)
          dstPix[c] = (PIX)blendOut((float)px[c], eff[c], t);
      }
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
// plugin instance

class SharpenZPlugin : public OFX::ImageEffect {
protected:
  // do not need to delete these, the ImageEffect is managing them for us
  OFX::Clip *dstClip_;
  OFX::Clip *srcClip_;
  OFX::Clip *maskClip_; // only exists in the General context

  OFX::StringParam *channels_;
  OFX::DoubleParam *sharpen_, *radius_, *bias_, *mix_;
  OFX::IntParam *quality_;
  OFX::BooleanParam *lumaOnly_, *unpremult_, *maskInvert_;

public:
  SharpenZPlugin(OfxImageEffectHandle handle)
    : OFX::ImageEffect(handle), dstClip_(0), srcClip_(0), maskClip_(0)
  {
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    if (getContext() == OFX::eContextGeneral)
      maskClip_ = fetchClip("Mask");

    channels_    = fetchStringParam("channels");
    sharpen_     = fetchDoubleParam("sharpen");
    radius_      = fetchDoubleParam("radius");
    bias_        = fetchDoubleParam("bias");
    quality_     = fetchIntParam("quality");
    mix_         = fetchDoubleParam("mix");
    maskInvert_  = fetchBooleanParam("maskInvert");
    lumaOnly_    = fetchBooleanParam("luma_only");
    unpremult_   = fetchBooleanParam("unpremult");
  }

  virtual void render(const OFX::RenderArguments &args) override;
  virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &) override
  {
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
    return false; // full scale: the host's default region
  }
  virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                    OFX::RegionOfInterestSetter &rois) override;

  void setupAndProcess(SharpenBase &, const OFX::RenderArguments &args);

private:
  // read + sanitise the knobs (the gizmo's knobChanged clamps: radius/bias/
  // sharpen >= 0, quality >= 1; SharpenZ adds mix clamped to [0,1])
  Params readParams() const;
};

Params SharpenZPlugin::readParams() const
{
  Params p;
  p.sharpen     = std::max(0.0, sharpen_->getValue());
  p.radius      = std::max(0.0, radius_->getValue());
  p.bias        = std::max(0.0, bias_->getValue());
  p.quality     = std::max(1, quality_->getValue());
  p.mix         = std::min(1.0, std::max(0.0, mix_->getValue()));
  p.maskInvert  = maskInvert_->getValue();
  p.lumaOnly    = lumaOnly_->getValue();
  p.unpremult   = unpremult_->getValue();
  std::string channelsStr;
  channels_->getValue(channelsStr);
  OfxLab::parseChannels(channelsStr, p.sel, "rgb");  // the gizmo's default
  return p;
}

////////////////////////////////////////////////////////////////////////////////
// RoI: the filter gathers up to stride*quality (+ the box-blur gather) away;
// the Mask clip needs exactly the requested RoI (per-pixel), the default.

void SharpenZPlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                          OFX::RegionOfInterestSetter &rois)
{
  const Params p = readParams();
  const int pad = p.pad();

  OfxRectD roi = args.regionOfInterest;
  roi.x1 -= pad;
  roi.x2 += pad;
  roi.y1 -= pad;
  roi.y2 += pad;

  rois.setRegionOfInterest(*srcClip_, roi);
}

////////////////////////////////////////////////////////////////////////////////
// render plumbing

void SharpenZPlugin::setupAndProcess(SharpenBase &processor, const OFX::RenderArguments &args)
{
  std::unique_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
  if (!dst)
    OFX::throwSuiteStatusException(kOfxStatFailed);

  OFX::BitDepthEnum dstBitDepth = dst->getPixelDepth();
  OFX::PixelComponentEnum dstComponents = dst->getPixelComponents();

  std::unique_ptr<OFX::Image> src(srcClip_->fetchImage(args.time));

  if (src) {
    if (src->getPixelDepth() != dstBitDepth || src->getPixelComponents() != dstComponents)
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported); // no depth conversion in v1
  }

  // optional Mask clip (General context only); hosts deliver alpha (Natron
  // extracts the user-selected mask channel for us). RGBA is accepted too.
  std::unique_ptr<OFX::Image> mask;
  if (maskClip_ && maskClip_->isConnected()) {
    mask.reset(maskClip_->fetchImage(args.time));
    if (mask) {
      if (mask->getPixelComponents() == OFX::ePixelComponentRGBA)
        processor.setMaskImg(mask.get(), 3);
      else if (mask->getPixelComponents() == OFX::ePixelComponentAlpha)
        processor.setMaskImg(mask.get(), 0);
      else
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  }

  processor.setDstImg(dst.get());
  processor.setSrcImg(src.get());
  processor.setRenderWindow(args.renderWindow);
  processor.process();
}

void SharpenZPlugin::render(const OFX::RenderArguments &args)
{
  // Local fix: this plugin works in full-resolution pixels only. Failing here at a reduced
  // render scale makes the host render at full size and scale the result itself.
  if (args.renderScale.x != 1. || args.renderScale.y != 1.)
    OFX::throwSuiteStatusException(kOfxStatFailed);
  OFX::BitDepthEnum dstBitDepth = dstClip_->getPixelDepth();
  OFX::PixelComponentEnum dstComponents = dstClip_->getPixelComponents();

  // v1 scope: float32 only (DECISIONS.md D-07)
  if (dstBitDepth != OFX::eBitDepthFloat)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

  const Params p = readParams();

  if (dstComponents == OFX::ePixelComponentRGBA) {
    SharpenProcessor<float, 4> fred(*this, p);
    setupAndProcess(fred, args);
  } else if (dstComponents == OFX::ePixelComponentAlpha) {
    SharpenProcessor<float, 1> fred(*this, p);
    setupAndProcess(fred, args);
  } else {
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
  }
}

////////////////////////////////////////////////////////////////////////////////
// factory

mDeclarePluginFactory(SharpenZPluginFactory, {}, {});

using namespace OFX;

void SharpenZPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  desc.setLabels("SharpenZ", "SharpenZ", "An accelerated weighted sharpen filter: a sharpen equivalent that avoids negative values, dark edges and hue inversions. Includes a mask input and mix control.");
  desc.setPluginGrouping("OFX Lab");

  // General (not Filter) is what carries the Mask input; Natron picks
  // General when both are declared (DECISIONS.md D-20). Filter is kept for
  // hosts that only chain effects in filter context — they simply get no mask.
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedContext(eContextFilter);

  // v1 scope: float32 only; UByte/UShort come later (DECISIONS.md D-07)
  desc.addSupportedBitDepth(eBitDepthFloat);

  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(true);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void SharpenZPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
  OFX::ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setTemporalClipAccess(false);
  srcClip->setSupportsTiles(true);
  srcClip->setIsMask(false);

  // Mask input: General context only (filter contexts forbid masks). Optional
  // and alpha-only; Natron then auto-adds its standard "enable mask" +
  // mask-channel-selector knobs and delivers the chosen channel as alpha.
  if (context == eContextGeneral) { // describeInContext runs under `using namespace OFX` scope below
    OFX::ClipDescriptor *maskClip = desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setOptional(true);
    maskClip->setIsMask(true);
    maskClip->setTemporalClipAccess(false);
    maskClip->setSupportsTiles(true);
  }

  OFX::ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(ePixelComponentRGBA);
  dstClip->addSupportedComponent(ePixelComponentAlpha);
  dstClip->setSupportsTiles(true);

  // Knob order, ranges and defaults mirror the gizmo exactly (D-08). The
  // SharpenZ knobs use the canonical names "mix"/"maskInvert" so Natron
  // groups them with its own mask controls.

  OFX::StringParamDescriptor *channels = desc.defineStringParam("channels");
  channels->setLabels("Channels", "Channels", "Channels to apply the sharpening to: any combination of r, g, b, a (or all/none).");
  channels->setStringType(eStringTypeSingleLine);
  channels->setDefault("rgb");

  OFX::DoubleParamDescriptor *sharpen = desc.defineDoubleParam("sharpen");
  sharpen->setLabels("Sharpen", "Sharpen", "Increase the local contrast of your image. A value of 0 will leave the image untouched. Higher values will result in crispier images.");
  sharpen->setRange(0, 4);
  sharpen->setDisplayRange(0, 4);
  sharpen->setDefault(0.5);

  OFX::DoubleParamDescriptor *radius = desc.defineDoubleParam("radius");
  radius->setLabels("Radius", "Radius", "Radius to apply the sharpening to.");
  radius->setRange(0, 100);
  radius->setDisplayRange(0, 100);
  radius->setDefault(100);

  OFX::DoubleParamDescriptor *bias = desc.defineDoubleParam("bias");
  bias->setLabels("Bias", "Bias", "Controls the bias of the blurred copy used for sharpening, avoiding banding or negative lobes around edges. Higher values apply the sharpen only to similar pixels. A value of 0 is equivalent to a plain weighted sharpen.");
  bias->setRange(0, 100);
  bias->setDisplayRange(0, 100);
  bias->setDefault(40);

  OFX::IntParamDescriptor *quality = desc.defineIntParam("quality");
  quality->setLabels("Quality", "Quality", "The maximum number of samples per axis used for the smoothing. When using very large radius the filter can become very slow. Limiting the number of samples hugely increases performance at the cost of smoothing out some fine details.");
  quality->setRange(1, 100); // gizmo is unbounded; values > radius are ineffective anyway (Q = min(quality, radius))
  quality->setDisplayRange(1, 30);
  quality->setDefault(15);

  OFX::BooleanParamDescriptor *unpremult = desc.defineBooleanParam("unpremult");
  unpremult->setLabels("Unpremult", "Unpremult", "Unpremultiply before processing, premultiply after.");
  unpremult->setDefault(true);

  OFX::BooleanParamDescriptor *luma = desc.defineBooleanParam("luma_only");
  luma->setLabels("Luma Only", "Luma Only", "If checked, only the luminance of the rgb is sharpened, keeping the same hue and saturation as the input.");
  luma->setDefault(true);

  OFX::DoubleParamDescriptor *mix = desc.defineDoubleParam("mix");
  mix->setLabels("Mix", "Mix", "Dissolve between the input (0) and the sharpened result (1).");
  mix->setRange(0, 1);
  mix->setDisplayRange(0, 1);
  mix->setDefault(1);

  OFX::BooleanParamDescriptor *maskInvert = desc.defineBooleanParam("maskInvert");
  maskInvert->setLabels("Mask Invert", "Mask Invert", "Invert the mask: black areas get the effect, white areas pass through.");
  maskInvert->setDefault(false);
}

OFX::ImageEffect *SharpenZPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
  return new SharpenZPlugin(handle);
}

namespace OFX
{
namespace Plugin
{
void getPluginIDs(OFX::PluginFactoryArray &ids)
{
  static SharpenZPluginFactory p("net.ofxlab.sharpenz", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
