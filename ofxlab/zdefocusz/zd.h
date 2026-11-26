// ZDefocusZ: the depth -> circle-of-confocus model (DECISIONS.md D-34).
//
// OFX-free on purpose, like TimeWarp's flow.h/motion.h: this is the whole depth
// semantics of the plugin, and it is checked headlessly against closed-form
// values (tests/zd_core_test.cpp) before a single pixel is convolved. It lives in
// plugins/zdefocusz/ because only this plugin uses it (D-06).
//
// What a defocus needs per pixel is one number: the **circle of confusion** (CoC),
// the diameter in pixels of the blur disc that point is recorded as. Everything
// downstream - the layer split, the kernel size, the composite - is driven by that
// number alone. So this file owns exactly two things: turning a depth channel into a
// CoC, and turning a range of CoCs into the discrete set of blur sizes the renderer
// actually convolves.
//
// ---------------------------------------------------------------------------
// The model: a ratio, not a unit
// ---------------------------------------------------------------------------
// A thin lens focused at distance `u_f` records an object at distance `u` as a disc
// of diameter
//
//     c(u) = A * |v' - v| / v',   with 1/v' = 1/f - 1/u,  A = f / N
//
// (f focal length, N f-stop, v the image distance the lens is focused at). Rearranged
// (see cocAtInfinityFromLens) that is
//
//     c(u) = [ f^2 / (N (u_f - f)) ] * | 1 - u_f / u |
//            \______ one constant ______/   \___ the defocus factor F ___/
//
// i.e. **the blur at any depth is the blur at infinity times F = |1 - focus/depth|**.
// F is 0 at the focus plane, 1 at infinite depth, and grows past 1 for objects nearer
// than the focus plane - which is Nuke's documented behaviour too ("blur nearer the
// camera than the focus plane may be larger than the blur at infinity"). Nothing here
// needs to know what a unit *is*: depth, `focusValue` and `depthScale` just have to
// share one unit, and the only length that enters the pixel domain is the resolved CoC.
//
// `blurSize` is therefore that one constant: **CoC at infinite depth, in pixels** -
// either typed directly (Nuke's `size`) or derived from a lens.
//
// ---------------------------------------------------------------------------
// The depth channel
// ---------------------------------------------------------------------------
// OFX 1.4 image clips have no named channels, only component sets (D-17), so a plugin
// cannot ask a host for "the Z channel": it fetches whatever the host delivers and
// selects a component itself. `depthChannel` (a string knob, resolved in the plugin)
// says *which* component; `depthMode` says what its numbers *mean*:
//
//   Distance          the value IS a distance from the camera (Blender's Z pass,
//                     Nuke's `depth`)
//   Inverse distance  the value is 1/distance (RenderMan-style z_depth)
//   Normalized        the value is a 0..1 fraction between `depthNear` and `depthFar`
//                     (OpenGL window depth at 0 = near, Blender's clip range, Nuke's
//                     `far = 0` / `far = 1` - both orderings covered by swapping the
//                     two knobs)
//   Blur              the value IS already a blur: CoC = cocScale * |value - focus|,
//                     which is Nuke's `direct` mode (`size 1` = use the depth values
//                     as blur sizes) and the mode to use when someone hands this node
//                     a CoC map instead of a depth map
//
// `depthInvert` negates the channel before the mapping, which is how camera-space Z
// gets handled (Blender looks down -Z, so a 10 m surface reads -10) and how Nuke's
// four negated `math` variants are expressed. Negating also reverses the layer order,
// as it does there.
//
// A value that is zero, negative or NaN means "no geometry here". That is a background
// sentinel in most renderers (and Blender writes `clip_end` there instead), and the
// conservative reading for a defocus is *far away*, so such a pixel gets the CoC at
// infinity. Distances that collapse to 0 (an object at the focal point) instead run F
// to infinity and land on `maxBlur`, which is exactly what Nuke's `max_size` is for.
#ifndef OFXLAB_ZDEFOCUSZ_ZD_H
#define OFXLAB_ZDEFOCUSZ_ZD_H

#include <vector>

namespace zd {

/** @brief What the depth channel's numbers mean. */
enum DepthMode {
  eDepthDistance = 0,
  eDepthInverseDistance,
  eDepthNormalized,
  eDepthBlur,
  kNumDepthModes
};

/** @brief Where `blurSize` (the CoC at infinite depth) comes from. */
enum BlurSource {
  eBlurFromSize = 0, // typed: Nuke's `size`
  eBlurFromLens,     // derived from aperture / focal length / sensor width
  kNumBlurSources
};

/** @brief The kernel's shape when no bokeh image is connected. */
enum FilterType {
  eFilterDisc = 0,
  eFilterBladed, // analytic iris polygon
  eFilterImage,  // the Bokeh clip, resampled to the required radius
  kNumFilterTypes
};

/** @brief Diagnostic outputs, the instrument the depth semantics are measured with. */
enum PreviewOutput {
  ePreviewResult = 0,
  ePreviewDepth,  // the component the depth selector picked
  ePreviewCoC,    // the CoC map, normalised by maxBlur
  ePreviewLayers, // the layer each pixel landed in (Nuke's `layer setup`)
  ePreviewBokeh,  // the kernel at the largest radius
  kNumPreviews
};

// ---------------------------------------------------------------------------
// Knob ranges, declared once: the descriptor offers these and the model clamps to
// them, so "what a host can be offered" and "what the maths accepts" cannot drift
// apart (the precedent is D-33's kFlowIters/kFlowLambda in flow.h).
// ---------------------------------------------------------------------------

// A CoC is a radius in pixels: 0 (perfectly in focus) to a cap. The cap is the
// largest number a *knob* may hold; the per-render memory cap is a separate, smaller
// thing (spriteRadiusCap) because a CoC is also a kernel size.
const double kCoCMin = 0.0, kCoCMax = 4096.0;

// Distances: `focusValue`, `depthNear`, `depthFar` all live in the depth channel's own
// units, so they share one range. 1e-9..1e12 covers millimetres to light-seconds
// without a user having to think about it.
const double kDepthMin = 1e-9, kDepthMax = 1e12;

// `cocScale` (Blur mode): pixels of CoC per unit of |depth - focus|.
const double kCocScaleMin = 0.0, kCocScaleMax = 1e6;

// Optics. f-stops below ~0.5 and focal lengths below 1 mm are not a lens; a sensor
// width is 0.1..1000 mm; `depthScale` converts a depth unit to millimetres, so it
// covers micrometres (1e-6) to a gigametre (1e9) and `1000` is the answer whenever the
// scene is in metres, which Blender's unit system is.
const double kApertureMin = 0.5, kApertureMax = 64.0;
const double kFocalMin = 1.0, kFocalMax = 2000.0;
const double kSensorMin = 0.1, kSensorMax = 1000.0;
const double kDepthScaleMin = 1e-6, kDepthScaleMax = 1e9;

// Layers (D-35): the number of *distinct blur sizes* between the focus plane and
// `maxBlur`. 0 asks the model to derive it from `maxBlur / layerSpacing`; 256 is
// Nuke's documented ceiling.
const int kLayerMin = 2, kLayerMax = 256;
const double kLayerSpacingMin = 0.5, kLayerSpacingMax = 64.0;
const double kLayerCurveMin = 0.25, kLayerCurveMax = 4.0;

/** @brief Everything the depth -> CoC model reads, already clamped to the ranges above.
 *
 * One struct for the whole depth half of the knob table, so the plugin's readParams()
 * has one place to write and the model one place to read. `depthMode`/`blurFrom`/
 * `filterType`/`preview` are the enums above.
 */
struct FocusOptions {
  int depthMode = eDepthDistance;
  bool depthInvert = false;

  double focusValue = 1.0;   // distance of the fully-in-focus plane
  double focusRange = 0.0;   // thickness of the in-focus slice around it
  double depthNear = 0.1;    // Normalized mode: value 0 maps to this distance
  double depthFar = 100.0;   // Normalized mode: value 1 maps to this distance
  double cocScale = 1.0;     // Blur mode

  int blurFrom = eBlurFromSize;
  double blurSize = 32.0;    // CoC at infinite depth, px (Nuke's `size`)
  double maxBlur = 64.0;     // clamp, whatever the geometry says (Nuke's `max_size`)
  double minBlur = 0.0;      // floor: the CoC inside the in-focus slice

  // Optics (only read when blurFrom == eBlurFromLens).
  double aperture = 2.8;     // f-stop
  double focalLength = 50.0; // mm
  double sensorWidth = 36.0; // mm
  double depthScale = 1000.0; // millimetres per depth unit
};

/** @brief Sanitise a FocusOptions in place: clamp every knob to the range the
 * descriptor offers, and enforce the two relations the model needs (a negative CoC is
 * not a thing, and `minBlur` above `maxBlur` would make the clamp unordered).
 * Returns nothing: the knobs end up legal, because a host may hand back whatever it
 * stored and the plugin may be animated into an unreasonable value. */
void sanitise(FocusOptions &o);

/** @brief The distance a raw channel value stands for, in the depth unit `focusValue`
 * uses. Infinity for the "no geometry" sentinel. Only `Normalized` and `Inverse
 * distance` actually transform the value; `Distance` returns it, and `Blur` mode has no
 * distance at all (it returns the value unchanged, so a preview of "depth" still shows
 * the channel the user picked). */
double mappedDistance(double raw, const FocusOptions &o);

/** @brief The defocus factor F = |1 - focus/depth|: 0 at the focus plane, 1 at infinite
 * depth, past 1 nearer than the focus plane, infinity at the focal point. */
double defocusFactor(double distance, const FocusOptions &o);

/** @brief The CoC at infinite depth, in pixels.
 *
 * `blurFrom == eBlurFromSize` returns the `blurSize` knob. `eBlurFromLens` evaluates
 * f^2 / (N (u_f - f)) in millimetres and converts with `imageWidthPx / sensorWidth`
 * (square pixels assumed), where u_f = focusValue * depthScale. A lens focused *at or
 * inside* itself (u_f <= f) has no finite CoC at infinity: that returns 0, i.e. no
 * defocus at all, which is the honest answer rather than a negative kernel.
 *
 * `imageWidthPx` is the width of the *image*, not of a tile. OFX 1.4 has no image-format
 * property and a region of interest is a tile, so the plugin passes the Output clip's
 * region-of-definition width divided by that clip's reported pixel aspect ratio - see
 * D-34, and a host that reports no aspect ratio means square pixels. */
double resolveBlurSize(const FocusOptions &o, double imageWidthPx);

/** @brief The CoC radius in pixels for one raw depth value: the resolved CoC at
 * infinity times the defocus factor, clamped to [minBlur, maxBlur], and `minBlur`
 * inside the in-focus slice (`|distance - focusValue| <= focusRange / 2`). Never
 * negative, never above maxBlur. */
double cocRadius(double raw, const FocusOptions &o, double blurSizePx);

// ---------------------------------------------------------------------------
// Layers: the discrete blur sizes, and the soft seams between them
// ---------------------------------------------------------------------------
// Splitting the image into layers is what keeps a defocus *ordered*: each layer has one
// kernel, and compositing the layers back to front preserves which object is in front
// (D-35). The cost of a layer split is the classic artefact - a pixel whose CoC lands
// just below a boundary gets the small kernel, the one just above gets the big one, and
// the seam shows as a hard band along a smoothly curved surface.
//
// So the level set here does two things Nuke's `nLayers` + `layerCurve` alone does not:
// a pixel is assigned to the **two** levels that bracket its CoC with a linear weight
// between them (a soft matte, which is a continuous cross-fade of two kernels rather
// than a jump), and `dither` jitters the assignment along the level axis by up to half
// the local spacing, which turns whatever is left of the banding into fine noise while
// leaving the *average* blur where it was (the jitter is symmetric and the blend is
// linear in CoC, so the mean is preserved).
struct LevelSet {
  std::vector<double> radius; // ascending; radius[0] == 0 is the in-focus level

  int count() const { return (int)radius.size(); }
  bool empty() const { return radius.empty(); }

  /** @brief The two levels a CoC falls between, and how far along they are blended.
   *
   * `i0 <= i1` are level indices, `t` is in [0,1]: t == 0 means the CoC *is* radius[i0],
   * t == 1 means it is radius[i1] (which happens exactly at a level's upper edge). A CoC
   * of 0 lands on the sharp level (i0 = i1 = 0, t = 0). A CoC at or above the top level
   * saturates at the top (i0 = i1 = count()-1, t = 0). `i0 != i1` is the soft seam: the
   * pixel contributes to both layers, weight 1-t and t. */
  void bracket(double coc, int *i0, int *i1, double *t) const;

  /** @brief The spacing between the two levels bracketing @p coc (0 at the top level).
   * Used as the dither amplitude, so the jitter is half a step *locally* rather than a
   * fixed number of pixels everywhere. */
  double spacingAt(double coc) const;
};

/** @brief How many blurred levels `maxBlur` asks for at @p layerSpacing pixels apiece
 * (clamped to [kLayerMin, kLayerMax]). A `maxBlur` of 0 gives the minimum: nothing to
 * blur, but a level set must still exist. */
int autoLayerCount(double maxBlur, double layerSpacing);

/** @brief Build the level set for a knob set: radius[0] = 0 (sharp), then
 * `maxBlur * (k / L) ^ layerCurve` for k = 1..L with L = `layers` (0 = auto).
 * `layerCurve > 1` concentrates the levels near the focus plane (more distinct small
 * blurs), `< 1` concentrates them near maxBlur - the same intent as Nuke's `layerCurve`
 * with the opposite sign convention, because here the curve bends the *radius* sequence
 * instead of a spacing curve. Degenerate inputs (maxBlur 0, L 1) still produce a usable,
 * strictly ascending set. */
LevelSet buildLevelSet(const FocusOptions &o, int layers, double layerSpacing, double layerCurve);

/** @brief A stable hash of an absolute pixel coordinate, in [0,1).
 *
 * Depends on (x, y) and nothing else - not on the band the pixel happens to be rendered
 * in, not on the order bands were processed, not on the image size. That is the same
 * guarantee D-31 exists for in TimeWarp: a per-pixel result must be identical under any
 * partition of the render window, or a tiled host and an untiled one would disagree. */
double pixelHash(int x, int y);

/** @brief The CoC used for layer assignment: @p coc jittered by up to half the local
 * level spacing (via @ref pixelHash) when @p enabled, left untouched otherwise. The
 * jitter never pushes a CoC outside [0, maxBlur]. */
double ditheredCoC(double coc, int x, int y, const LevelSet &levels, bool enabled);

} // namespace zd

#endif
