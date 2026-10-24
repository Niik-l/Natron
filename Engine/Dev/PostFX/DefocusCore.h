/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEFOCUSCORE_H
#define NATRON_ENGINE_DEFOCUSCORE_H

/**
 * DefocusCore: the image-processing engine behind the LensDefocus node.
 *
 * Pure C++17 with no Natron or Qt dependency, so the standalone testbed compiles the
 * same sources. Algorithm (see complex_tools/zdefocus/plan/01_v1_2d_node.md):
 *
 *   1. decode depth to a signed circle-of-confusion radius per pixel (thin lens, 1/Z linear)
 *   2. split pixels on depth edges into a front and a back sample (aliased Z vs AA beauty)
 *   3. bucket samples into slices spaced in CoC space, nearest first
 *   4. per slice, fill the background behind nearer slices (edge extend within the band
 *      where the nearer blur can reveal it), or use a clean background plate
 *   5. per output strip, scatter every slice back to front with its own per-sample kernel
 *      (convex aperture shape written as row spans into a row-difference buffer, so cost is
 *      O(radius) per sample), large radii at reduced resolution, highlights at full res,
 *      and composite each slice over the result
 *
 * Coordinates: one shared integer pixel grid, y up (Natron convention). Pixel (x, y) has its
 * centre at (x, y). All images are float, premultiplied RGBA unless stated.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace defocus {

class LensTable;   // LensTrace.h

struct Rect
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0; // [x1, x2) x [y1, y2)

    Rect() {}
    Rect(int ax1, int ay1, int ax2, int ay2) : x1(ax1), y1(ay1), x2(ax2), y2(ay2) {}
    int width() const { return x2 > x1 ? x2 - x1 : 0; }
    int height() const { return y2 > y1 ? y2 - y1 : 0; }
    bool isEmpty() const { return width() == 0 || height() == 0; }
    bool contains(int x, int y) const { return x >= x1 && x < x2 && y >= y1 && y < y2; }
};

// A float image window. rowStride is in floats; channels is the pixel stride in floats.
struct ImageView
{
    float* data = nullptr;
    Rect bounds;
    int channels = 4;
    std::ptrdiff_t rowStride = 0;

    bool valid() const { return data != nullptr && !bounds.isEmpty(); }
    float* pixel(int x, int y) const
    {
        return data + (std::ptrdiff_t)(y - bounds.y1) * rowStride + (std::ptrdiff_t)(x - bounds.x1) * channels;
    }
};

struct ConstImageView
{
    const float* data = nullptr;
    Rect bounds;
    int channels = 4;
    std::ptrdiff_t rowStride = 0;

    ConstImageView() {}
    ConstImageView(const ImageView& v) : data(v.data), bounds(v.bounds), channels(v.channels), rowStride(v.rowStride) {}
    bool valid() const { return data != nullptr && !bounds.isEmpty(); }
    const float* pixel(int x, int y) const
    {
        return data + (std::ptrdiff_t)(y - bounds.y1) * rowStride + (std::ptrdiff_t)(x - bounds.x1) * channels;
    }
};

enum DepthConventionEnum
{
    eDepthPlanarZ = 0,   // distance along the camera axis (ScanlineRender depth.Z, Cycles Depth)
    eDepthRadial,        // distance along the pixel's ray (FastVolumeRender Depth)
    eDepthInverse,       // 1/Z, far = 0 (Nuke convention)
    eDepthNormalized,    // 0..1 remapped between Normalized Near/Far (AI / 2.5D depth maps)
    eDepthDirectCoC,     // the value is the signed CoC radius in pixels (full-res)
    eDepthPosition,      // planar depth computed from the Position plane (needs camera space or a camera)
};

enum SpaceEnum
{
    eSpaceWorld = 0,
    eSpaceCamera,        // camera space; either sign of the view axis is accepted
};

// Optional camera, for the Position/Normal features: depth from P, a focus point locked to the
// world, filling hidden background along the surface plane. Camera space: +X right, +Y up, looking
// down -Z.
struct CameraModel
{
    bool valid = false;
    double origin[3] = {0., 0., 0.};                    // world position
    double rot[9] = {1., 0., 0., 0., 1., 0., 0., 0., 1.};// camera-to-world rotation, row-major (columns = camera axes)
    double focalPx = 1000.;                             // focal length, render-scale px
    double centreX = 0., centreY = 0.;                  // principal point, render-scale px (pixel corners at integers)

    // World point -> camera space.
    void toCamera(const double w[3], double c[3]) const
    {
        const double d[3] = {w[0] - origin[0], w[1] - origin[1], w[2] - origin[2]};
        for (int k = 0; k < 3; ++k) {
            c[k] = rot[0 * 3 + k] * d[0] + rot[1 * 3 + k] * d[1] + rot[2 * 3 + k] * d[2];
        }
    }
    // World direction -> camera space.
    void dirToCamera(const double w[3], double c[3]) const
    {
        for (int k = 0; k < 3; ++k) {
            c[k] = rot[0 * 3 + k] * w[0] + rot[1 * 3 + k] * w[1] + rot[2 * 3 + k] * w[2];
        }
    }
    // Planar depth of a world point (> 0 in front of the camera).
    double planarDepth(const double w[3]) const
    {
        double c[3];
        toCamera(w, c);
        return -c[2];
    }
};

// Builds a CameraModel from OpenEXR worldToCamera / worldToNDC header matrices (row-vector
// convention, as Karma, RenderMan and Arnold write them), for an image of width x height pixels
// (the display window) at the given render scale. Either camera-axis sign is handled. Returns
// false if the matrices are degenerate.
bool cameraFromExrMatrices(const double worldToCamera[16], const double worldToNDC[16],
                           double width, double height, double renderScale, CameraModel* out);

// Focus Tilt / Swing (degrees, Settings convention) that lay the focus plane along a surface with
// camera-space normal nCam. Returns false for a surface seen edge-on.
bool focusTiltFromNormal(const double nCam[3], double* tiltDeg, double* swingDeg);

enum CocModeEnum
{
    eCocCamera = 0,          // focal length, f-number, focus distance, film back
    eCocSizeAtInfinity,      // user gives the CoC radius at infinity in pixels; focus distance still used
};

enum EdgeModeEnum
{
    eEdgeOff = 0,
    eEdgeMinMax,     // split edge pixels 50/50 between the 3x3 front and back depth
    eEdgeUnmix,      // split by projecting the pixel colour between front and back neighbour colours
};

enum BorderModeEnum
{
    eBorderBlack = 0,
    eBorderRepeat,
};

enum OutputModeEnum
{
    eOutputResult = 0,
    eOutputFocus,        // red = near, green = in focus, blue = far
    eOutputSignedCoC,    // R = signed CoC px, G = |CoC|, B = clamped
    eOutputSlices,       // false-colour slice index
    eOutputEdge,         // front coverage of split edge pixels
    eOutputSurfaceEdges, // slicing decisions: red = outline split by Position/Normal, yellow = split
                         // by the blur-radius test, green = kept whole by Position/Normal where the
                         // radius test would split, blue = Position/Normal ignored (disagree with
                         // depth: volumes, glass); elsewhere the dimmed image
};

// Procedural lens character. All zero = plain disc.
struct LensSettings
{
    int blades = 0;                 // 0 = circular, else 3..16
    double curvature = 0.;          // 0 = straight blades, 1 = circle
    double rotationDeg = 0.;
    double squeeze = 1.;            // anamorphic: > 1 = taller oval (area preserving)
    double catEye = 0.;             // optical vignetting strength at the frame corner (0..1+)
    double catEyeStart = 0.;        // normalised field radius where clipping starts
    double swirl = 0.;              // tangential stretch at the frame corner (Petzval swirl)
    double sphericalAberration = 0.;// > 0 = bright rim behind focus, bright centre in front
    double onionAmount = 0.;        // concentric ring modulation (0..1)
    double onionFrequency = 6.;     // rings across the radius
    double axialCA = 0.;            // signed CoC offset in px: R gets +a, B gets -a (full-res px)
    int profileSteps = 6;           // nested shapes used to approximate a radial profile

    // A traced real lens (LensTrace.h). When set, its kernels replace cat's eye, swirl and spherical
    // aberration (the blades, rotation and squeeze still shape the iris).
    std::shared_ptr<const LensTable> table;
    double character = 1.;          // 0 = ideal thin lens .. 1 = the traced lens as is
    bool chromatic = true;          // trace R, G and B separately (axial colour; 3x scatter)
};

// Image stages of a real lens, applied to the result (Result output only), radial about the optical
// centre. Maps are sampled uniformly over the normalised field radius 0..rhoMax (1 = fieldRadiusPx)
// and held beyond it. Empty maps = that stage off; all off = the plain result, untouched.
struct LensStages
{
    double rhoMax = 1.;
    // Per channel R, G, B: the output pixel at radius rho takes the result at radius rho * scale
    // (distortion, focus breathing, lateral colour). Alpha follows green.
    std::vector<float> scale[3];
    // Multiplies colour (not alpha): natural vignetting.
    std::vector<float> illum;

    bool geometric() const { return !scale[0].empty() || !scale[1].empty() || !scale[2].empty(); }
    bool active() const { return geometric() || !illum.empty(); }
};

struct Settings
{
    // Depth decoding
    DepthConventionEnum depthConvention = eDepthPlanarZ;
    double farThreshold = 0.;       // > 0: depth >= this is treated as infinity (ScanlineRender writes camera far)
    bool zeroIsInfinity = true;     // Cycles / FastVolume write 0 where nothing was hit
    double normalizedNear = 0.1;
    double normalizedFar = 100.;
    bool normalizedInvert = false;  // true: 1 = near
    bool depthPremultiplied = false;// depth was multiplied by alpha upstream
    double worldToMm = 1000.;       // scene units to millimetres (1 unit = 1 m)

    // CoC
    CocModeEnum cocMode = eCocCamera;
    double focalLengthMm = 50.;
    double fNumber = 2.8;
    double focusDistance = 5.;      // scene units
    double filmbackWidthMm = 36.;
    double sizeAtInfinityPx = 10.;  // radius in full-res px (eCocSizeAtInfinity)
    double formatWidthPx = 1920.;   // full-res pixels across the film back
    double renderScale = 1.;        // multiplies every pixel quantity
    double nearGain = 1.;
    double farGain = 1.;
    double maxRadiusPx = 256.;      // full-res px

    // Field (for position-dependent kernels), in render-scale pixel coordinates
    double opticalCentreX = 0.;
    double opticalCentreY = 0.;
    double fieldRadiusPx = 1.;      // distance from the centre to the frame corner

    // Camera for the Position/Normal features (optional).
    CameraModel camera;

    // Tilted focus plane (tilt-shift / Scheimpflug), degrees. The plane turns about the pivot pixel,
    // which stays at Focus Distance. Tilt > 0: focus recedes towards the top of the frame (a floor);
    // Swing > 0: towards the right.
    double focusTiltDeg = 0.;
    double focusSwingDeg = 0.;
    double focusPivotX = 0.;        // render-scale px
    double focusPivotY = 0.;

    LensSettings lens;

    // Quality
    double sliceStep = 0.15;        // relative CoC step between slices
    double fullResRadius = 32.;     // render-scale px; bigger radii scatter at reduced resolution
    double highlightThreshold = 8.; // luminance above which a sample scatters at full res (0 = off)
    int maxHighlights = 50000;
    EdgeModeEnum edgeMode = eEdgeUnmix;
    double edgeThresholdPx = 1.;    // CoC difference across a 3x3 that counts as a depth edge
    bool fillBehind = true;
    // With Position/Normal: put every occlusion edge between slices, however small its blur
    // difference (no seams; more slices, slower). Off = the radius test decides, as without P/N.
    bool splitOcclusionEdges = true;
    BorderModeEnum border = eBorderRepeat;  // a real lens sees past the frame edge; Black darkens borders

    OutputModeEnum output = eOutputResult;
    LensStages stages;
    double mix = 1.;
    int threads = 0;                // 0 = hardware concurrency
    // Scatter (step 5) on the GPU (DefocusGpu, wgpu); the CPU scatters when it can't (Stats::gpuMessage)
    bool useGpu = false;
};

// Deep samples: a list per pixel, each sample `stride` floats with RGBA premultiplied by its own
// alpha (not attenuated by the samples in front, as in OpenEXR deep), planar depth Z (scene
// units) and optionally ZBack (> Z: a volume segment). `at` returns a pixel's samples (core,
// bottom-up coordinates) and their count, or null. Order within a pixel doesn't matter.
struct DeepView
{
    Rect bounds;
    int stride = 0;
    int iR = 0, iG = 1, iB = 2, iA = 3, iZ = 4, iZBack = -1;
    std::function<const float*(int x, int y, int* count)> at;
    bool valid() const { return stride > 0 && iZ >= 0 && iA >= 0 && (bool)at; }
};

struct Inputs
{
    ConstImageView source;          // RGBA premultiplied (channels >= 3; alpha = 1 if absent)
    ConstImageView depth;           // depth in channel depthChannel
    int depthChannel = 0;
    ConstImageView cleanBackground; // optional RGBA premultiplied, used to fill behind foreground
    ConstImageView mask;            // optional; limits the effect
    int maskChannel = 0;
    // Optional surface geometry (one sample per pixel, aliased like depth). With both, depth edges
    // are told apart from steep surfaces (a floor at a grazing angle) and hidden background is filled
    // along the surface plane.
    ConstImageView position;        // xyz in channels positionChannel..+2
    int positionChannel = 0;
    SpaceEnum positionSpace = eSpaceWorld;
    ConstImageView normal;          // xyz in channels normalChannel..+2
    int normalChannel = 0;
    SpaceEnum normalSpace = eSpaceWorld;
    // Deep: when valid, every deep sample is scattered at its own blur (hidden layers are real,
    // so they show behind blurred foreground without guessing; samples in one pixel composite
    // front to back). `source` should then be the flattened deep image and `depth` its front depth
    // (planar Z): they still give the silhouette, the slicing outlines and the colours of the
    // background fill where the deep image has nothing behind.
    DeepView deep;
};

struct Stats
{
    std::int64_t samples = 0;
    std::int64_t edgeSplits = 0;
    std::int64_t filled = 0;
    std::int64_t highlights = 0;
    std::int64_t clamped = 0;
    int slices = 0;
    double maxAbsRadius = 0.;       // render-scale px, after clamping
    double secondsDecode = 0.;
    double secondsFill = 0.;
    double secondsScatter = 0.;
    bool gpu = false;               // the scatter ran on the GPU
    std::string gpuMessage;         // Settings::useGpu but the CPU scattered: why
};

// Signed CoC radius in full-res pixels (before gains and clamp) for a depth in scene units.
// Negative in front of the focus plane, positive behind it.
double signedCocRadiusAtDepth(const Settings& s, double depthUnits);

// Decode one depth sample to a planar distance in scene units (+inf for background). Returns
// NaN for eDepthDirectCoC, which carries no distance. Radial depth is treated as planar here.
double decodeDepthUnits(const Settings& s, double value);

// Focus distance (scene units) of the pixel at (x, y) (render-scale px, pixel corners at integers):
// Focus Distance, or where the pixel's ray meets the tilted focus plane.
double focusDistanceAt(const Settings& s, double x, double y);

// Signed CoC radius in full-res pixels (before gains and clamp) for a depth seen at (x, y)
// (render-scale px, pixel corners at integers), with a tilted focus plane.
double signedCocRadiusAt(const Settings& s, double depthUnits, double x, double y);

// Upper bound of how far (render-scale px) the result can extend beyond the source window.
double maxPaddingPx(const Settings& s);

/**
 * Render the effect into output (RGBA, channels >= 4). output.bounds may differ from the source
 * bounds (e.g. expanded by maxPaddingPx). errorMap (optional, channels >= 3, same bounds as output):
 * R = fraction of the pixel that came from guessed (filled) background, G = CoC was clamped,
 * B = 0. Returns false if aborted() returned true.
 */
bool render(const Inputs& in,
            const Settings& s,
            const ImageView& output,
            const ImageView* errorMap,
            Stats* stats,
            const std::function<bool()>& aborted);

} // namespace defocus

#endif // NATRON_ENGINE_DEFOCUSCORE_H
