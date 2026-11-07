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

#ifndef NATRON_ENGINE_DEV_VR_VRFILTERCORE_H
#define NATRON_ENGINE_DEV_VR_VRFILTERCORE_H

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

/**
 * @brief Shared seam-safe latlong (equirectangular) filtering core for the VR tool nodes.
 *
 * A 2:1 equirectangular map is a distorted 2D representation of a sphere: the left and
 * right edges are the same 360-degree seam, and the top/bottom rows are the degenerate
 * poles where all longitudes converge. A naive 2D filter applied across the flat plane
 * produces a harsh vertical seam at the wrap boundary and smears features near the poles.
 *
 * Everything here works on raw interleaved float buffers (W*H*nComp, RGBA layout) so the
 * same machinery is shared by Blur VR / Sharpen VR / Glow VR / ExpoGlow VR / DepthBlur VR /
 * LightWrap VR. Two seam rules are enforced everywhere:
 *
 *   - Horizontal (X) always WRAPS: the kernel index is taken modulo the image width, so
 *     effects bleed smoothly across the 0/360-degree seam with no boundary artifact.
 *   - Vertical (Y) CLAMPS at the poles: rows outside [0, H) sample the nearest row.
 *
 * Latitude-aware blur: in an equirectangular map, one pixel of longitude at latitude phi
 * spans 1/cos(phi) times the arc of one pixel at the equator, so a blur that is isotropic
 * ON THE SPHERE must scale its horizontal sigma by 1/cos(phi) toward the poles (capped).
 * This is the pole-corrected / anisotropic filtering the VR toolsets (CaraVR etc.) apply.
 */

namespace VRFilterCore {

// Separable gaussian blur parameters.
struct BlurParams
{
    double sigmaX = 25.0;        // horizontal sigma in full-res pixels at the equator
    double sigmaY = 25.0;        // vertical sigma in full-res pixels
    double sizeXFactor = 1.0;    // extra horizontal stretch (the wrap-axis scale knob)
    int    quality = 2;          // 1..3: kernel extent = 2*sigma / 3*sigma / 4*sigma
    bool   latitudeComp = true;  // scale horizontal sigma by 1/cos(latitude) toward the poles
    double latMaxStretch = 8.0;  // cap on the 1/cos(latitude) growth (bounds the cost)
    int    width = 0;            // image width (pixels); wrap period
    int    height = 0;           // image height (pixels); pole rows are y=0 and y=height-1
};

/**
 * @brief Separable wrap-blur on an interleaved RGBA buffer.
 *
 * src and dst must NOT alias. src/dst are W*H*nComp floats (nComp <= 4). Row 0 is the
 * bottom image row (south pole), row H-1 the top (north pole) — though the latitude
 * profile is symmetric so either order works. The horizontal pass wraps (modulo W), the
 * vertical pass clamps at the poles. When latitudeComp is on, each row gets its own
 * horizontal sigma (sigmaX * sizeXFactor / max(cos(lat), 1/maxStretch)).
 *
 * Large sigma uses an internal downscale+upscale path so cost stays bounded. Rows are
 * processed in octave runs of similar sigma (matters when latitudeComp is on).
 *
 * aborted: optional poll callback, checked between row batches so a stale render bails
 * early (the render thread's abort state is not visible inside worker threads).
 */
void wrapBlur(const float* src, float* dst, int W, int H, int nComp, const BlurParams& p,
              const std::function<bool()>& aborted = std::function<bool()>());

/**
 * @brief Threshold + gain highlight isolation (used by Glow / ExpoGlow).
 *
 * Per color channel: out = clamp((v - threshold) / (1 - threshold)) * gain; when
 * softness > 0 the cut is eased with a smoothstep blend (0 = linear, 1 = full
 * smoothstep). Alpha (channel 3 of a 4-channel buffer) passes through unchanged.
 * count is the number of pixels; src/dst hold count*nComp floats.
 */
void thresholdGain(const float* src, float* dst, size_t count, int nComp,
                   float threshold, float softness, float gain);

/**
 * @brief 2x2 box downsample with horizontal wrap and vertical clamp.
 *
 * src is W*H*nComp, dst is outW*outH*nComp. outW/outH are set to the result size
 * (floor of W/2, H/2, at least 1). Odd edges wrap horizontally and clamp vertically.
 */
void downsample2xWrap(const float* src, float* dst, int W, int H, int nComp,
                      int* outW, int* outH);

/**
 * @brief Bilinear upsample with horizontal wrap and vertical clamp.
 *
 * src is sw*sh*nComp, dst is dw*dh*nComp (dw, dh any size). Used to bring a pyramid
 * level back to full resolution.
 */
void upsampleBilinearWrap(const float* src, int sw, int sh, float* dst, int dw, int dh, int nComp);

/**
 * @brief dst += weight * srcAdd (elementwise, count = W*H*nComp floats).
 */
void accumulate(float* dst, const float* srcAdd, size_t count, float weight);

/**
 * @brief dst += weight * upsampleBilinearWrap(src) in a single pass.
 *
 * The glow nodes accumulate several pyramid levels into one full-resolution buffer;
 * fusing the upsample with the weighted add saves a full-resolution pass per level.
 */
void upsampleAccumulateWrap(const float* src, int sw, int sh, float* dst, int dw, int dh,
                            int nComp, float weight);

/**
 * @brief Build a wrap-aware image pyramid for the bloom/glow nodes.
 *
 * levels[0] is a copy of src at (W,H); each next level is a 2x2 box downsample (wrap in
 * x, clamp in y) until both dimensions are <= 2 or maxLevels is reached. sizes[i] holds
 * the (w, h) of level i. Buffer i holds sizes[i].first * sizes[i].second * nComp floats.
 */
std::vector<std::vector<float> > buildPyramid(const float* src, int W, int H, int nComp,
                                              int maxLevels, std::vector<std::pair<int, int> >* sizes);

} // namespace VRFilterCore

#endif // NATRON_ENGINE_DEV_VR_VRFILTERCORE_H
