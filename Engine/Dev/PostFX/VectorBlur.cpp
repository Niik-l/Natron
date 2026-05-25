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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "VectorBlur.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {

enum BlurTypeEnum
{
    eBlurTypeUniform = 0,
    eBlurTypeGaussian = 1,
};

inline float sampleBilinearChannel(const Image::ReadAccess& ra,
                                    const RectI& bounds,
                                    int nComp,
                                    int comp,
                                    float fx, float fy)
{
    // Bilinear sample with edge clamp.
    int x0 = (int)std::floor(fx);
    int y0 = (int)std::floor(fy);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    auto clampX = [&](int v) { return std::max(bounds.x1, std::min(bounds.x2 - 1, v)); };
    auto clampY = [&](int v) { return std::max(bounds.y1, std::min(bounds.y2 - 1, v)); };

    int cx0 = clampX(x0), cx1 = clampX(x1);
    int cy0 = clampY(y0), cy1 = clampY(y1);

    auto pixAt = [&](int x, int y) -> const float* {
        return (const float*)ra.pixelAt(x, y);
    };

    const float* p00 = pixAt(cx0, cy0);
    const float* p10 = pixAt(cx1, cy0);
    const float* p01 = pixAt(cx0, cy1);
    const float* p11 = pixAt(cx1, cy1);

    if (!p00 || !p10 || !p01 || !p11) {
        return 0.0f;
    }
    if (comp >= nComp) return 0.0f;

    float v00 = p00[comp];
    float v10 = p10[comp];
    float v01 = p01[comp];
    float v11 = p11[comp];

    float v0 = v00 * (1.0f - tx) + v10 * tx;
    float v1 = v01 * (1.0f - tx) + v11 * tx;
    return v0 * (1.0f - ty) + v1 * ty;
}

} // anonymous namespace

struct VectorBlurPrivate
{
    KnobIntWPtr    dilateRadius;  // 0 = off; otherwise extend velocity field outward by this many pixels
    KnobDoubleWPtr scale;
    KnobDoubleWPtr offset;
    KnobBoolWPtr   invertVectors;
    KnobIntWPtr    maxSamples;
    KnobChoiceWPtr blurType;
    KnobDoubleWPtr motionFalloff;
    KnobBoolWPtr   normalize;
    KnobDoubleWPtr mix;
};

VectorBlur::VectorBlur(NodePtr node)
    : EffectInstance(node)
    , _imp(new VectorBlurPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VectorBlur::~VectorBlur()
{
}

std::string
VectorBlur::getPluginDescription() const
{
    return "Per-pixel motion blur driven by a Velocity vector field.\n\n"
           "Consumes the Velocity AOV produced by ScanlineRender's GLSL/MRT pipeline (Phase 3D) "
           "and blurs the beauty along the per-pixel screen-space velocity vectors.\n\n"
           "Modeled on Nuke's VectorBlur. Gather-based algorithm: for each output pixel, walk N "
           "samples along the velocity vector and average them. Sample count scales with vector "
           "magnitude.\n\n"
           "Wiring: input 0 = Source (color to blur), input 1 = Vectors (an RGB image where R "
           "is the x velocity and G is the y velocity, pixels per frame). Use DevShuffle to "
           "extract ScanlineRender's Velocity AOV into RGB and feed it into input 1. If input 1 "
           "is not wired the node passes Source through unchanged.";
}

void
VectorBlur::addAcceptedComponents(int /*inputNb*/,
                                   std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
VectorBlur::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VectorBlur::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
VectorBlur::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale"));
        k->setName("scale"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Multiplies the velocity vector magnitude. "
                              "1.0 = full per-frame motion. Increase for longer trails."));
        page->addKnob(k); _imp->scale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shutter Offset"));
        k->setName("offset"); k->setDefaultValue(-0.5); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Shutter timing relative to current frame. "
                              "-0.5 = motion centered on frame (Nuke convention). "
                              "0 = motion ends at frame. -1 = motion ends at previous frame."));
        page->addKnob(k); _imp->offset = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert Vectors"));
        k->setName("invertVectors"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Flip velocity direction before applying blur."));
        page->addKnob(k); _imp->invertVectors = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Samples"));
        k->setName("maxSamples"); k->setDefaultValue(32);
        k->setMinimum(1); k->setDisplayMinimum(1); k->setDisplayMaximum(128);
        k->setHintToolTip(tr("Cap on per-pixel sample count. Higher = smoother long trails "
                              "but proportionally slower render."));
        page->addKnob(k); _imp->maxSamples = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Dilate Radius"));
        k->setName("dilateRadius"); k->setDefaultValue(32);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(128);
        k->setHintToolTip(tr("Extends the velocity field outward by this many pixels before the "
                              "main gather (max-magnitude dilation). This is the key knob that "
                              "lets motion blur trails extend OUTSIDE the moving geometry's "
                              "silhouette — without it, background pixels have zero velocity "
                              "and can't be blurred. Set to 0 to disable (faster but trails "
                              "won't appear past the original edge). Default 32 is good for "
                              "modest motion; increase to cover long trails."));
        page->addKnob(k); _imp->dilateRadius = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Blur Type"));
        k->setName("blurType");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("uniform",  "Uniform",  "Flat-box weighting along the motion vector."));
        opts.push_back(ChoiceOption("gaussian", "Gaussian", "Gaussian falloff centered on current pixel."));
        k->populateChoices(opts);
        k->setDefaultValue(eBlurTypeGaussian);
        k->setHintToolTip(tr("Sample weighting profile along the motion vector."));
        page->addKnob(k); _imp->blurType = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Motion Falloff"));
        k->setName("motionFalloff"); k->setDefaultValue(0.33);
        k->setMinimum(0.05); k->setDisplayMinimum(0.05); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Gaussian shape parameter (only used when Blur Type = Gaussian). "
                              "Smaller = tighter falloff, sharper trail."));
        page->addKnob(k); _imp->motionFalloff = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Normalize"));
        k->setName("normalize"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Normalize sample weights to reduce tearing at velocity "
                              "discontinuities. Recommended ON."));
        page->addKnob(k); _imp->normalize = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Blend between original (0) and fully blurred (1) output."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
VectorBlur::getRegionOfDefinition(U64 /*hash*/,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    StatusEnum st = input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
    if (st != eStatusOK && st != eStatusReplyDefault) return st;

    // Expand the bbox by dilateRadius so trails can extend past the source bbox.
    // Using dilateRadius (rather than scale * maxSamples) keeps the RoI bounded —
    // trail length is naturally capped at dilateRadius because background pixels
    // beyond that distance don't get a velocity from dilation and stay in passthrough.
    KnobIntPtr dilateK = _imp->dilateRadius.lock();
    int dilatePx = dilateK ? std::max(0, dilateK->getValue()) : 32;
    if (dilatePx > 0) {
        const OfxPointD sp = scale.toOfxPointD();
        double padX = (double)dilatePx / std::max(1e-6, sp.x);
        double padY = (double)dilatePx / std::max(1e-6, sp.y);
        rod->x1 -= padX; rod->x2 += padX;
        rod->y1 -= padY; rod->y2 += padY;
    }
    return eStatusOK;
}

StatusEnum
VectorBlur::render(const RenderActionArgs& args)
{
    // Pull the Color plane (default).
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;

    // Vectors come from optional input 1 as raw RGB (R = vx, G = vy, B = ignored).
    // User shuffles ScanlineRender's Velocity AOV into RGB upstream. Same
    // "data via a shuffled secondary input" pattern as PMask (Position pass).
    RectI velRoi;
    ImagePtr velImg = getInput(1)
                    ? getImage(1, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &velRoi)
                    : ImagePtr();
    // If no velocity input wired: passthrough.
    const bool haveVelocity = (velImg != nullptr);

    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const double scaleVal      = _imp->scale.lock()->getValueAtTime(args.time);
    const double offsetVal     = _imp->offset.lock()->getValueAtTime(args.time);
    const bool   invertVecs    = _imp->invertVectors.lock()->getValueAtTime(args.time) != 0;
    const int    maxSamples    = std::max(1, _imp->maxSamples.lock()->getValue());
    const int    dilateRadius  = std::max(0, _imp->dilateRadius.lock()->getValue());
    const int    blurTypeIdx   = _imp->blurType.lock()->getValue();
    const double motionFalloff = _imp->motionFalloff.lock()->getValueAtTime(args.time);
    const bool   normalize     = _imp->normalize.lock()->getValueAtTime(args.time) != 0;
    const double mixVal        = _imp->mix.lock()->getValueAtTime(args.time);

    const float fscale         = (float)scaleVal * (invertVecs ? -1.0f : 1.0f);
    const float foffset        = (float)offsetVal;
    const float ffalloffInv    = (float)(1.0 / std::max(0.05, motionFalloff));
    const float fmix           = (float)mixVal;
    const float invMix         = 1.0f - fmix;

    RectI srcBounds = srcImg->getBounds();
    RectI outBounds = outImg->getBounds();
    RectI velBounds = haveVelocity ? velImg->getBounds() : RectI();

    int srcNComp = srcImg->getComponents().getNumComponents();
    int outNComp = std::min(srcNComp, 4);
    int velNComp = haveVelocity ? velImg->getComponents().getNumComponents() : 0;

    Image::ReadAccess  srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());
    std::unique_ptr<Image::ReadAccess> velRa;
    if (haveVelocity) velRa.reset(new Image::ReadAccess(velImg.get()));

    // Passthrough early-out when no velocity is present.
    if (!haveVelocity) {
        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const float* src = (x >= srcBounds.x1 && x < srcBounds.x2 &&
                                    y >= srcBounds.y1 && y < srcBounds.y2)
                                 ? (const float*)srcRa.pixelAt(x, y) : nullptr;
                if (!src) { for (int c = 0; c < outNComp; ++c) dst[c] = 0.0f; continue; }
                for (int c = 0; c < outNComp; ++c) dst[c] = src[c];
            }
        }
        return eStatusOK;
    }

    // ============================================================
    // Velocity dilation (max-magnitude, 2-pass separable). Extends
    // the velocity field outward by dilateRadius pixels so background
    // pixels near moving geometry can gather along its velocity —
    // this is what makes motion-blur trails appear OUTSIDE the
    // original silhouette. Skipped when dilateRadius == 0.
    // ============================================================
    const int W = outBounds.x2 - outBounds.x1;
    const int H = outBounds.y2 - outBounds.y1;
    std::vector<float> dilated;  // interleaved [vx, vy] in output-bound coordinates
    if (dilateRadius > 0 && W > 0 && H > 0) {
        std::vector<float> horizMax((size_t)W * H * 2, 0.0f);
        dilated.assign((size_t)W * H * 2, 0.0f);

        // Pass 1: horizontal max-magnitude. For each output pixel, find the velocity
        // with the largest magnitude in a horizontal window of radius dilateRadius.
        #ifdef _OPENMP
        #pragma omp parallel for
        #endif
        for (int y = 0; y < H; ++y) {
            const int absY = y + outBounds.y1;
            const bool yIn = (absY >= velBounds.y1 && absY < velBounds.y2);
            for (int x = 0; x < W; ++x) {
                const int absX = x + outBounds.x1;
                float bestMag2 = 0.0f, bestVx = 0.0f, bestVy = 0.0f;
                if (yIn) {
                    const int sxLo = std::max(velBounds.x1, absX - dilateRadius);
                    const int sxHi = std::min(velBounds.x2, absX + dilateRadius + 1);
                    for (int sampleX = sxLo; sampleX < sxHi; ++sampleX) {
                        const float* vp = (const float*)velRa->pixelAt(sampleX, absY);
                        if (!vp || velNComp < 2) continue;
                        const float vxN = vp[0];
                        const float vyN = vp[1];
                        const float mag2 = vxN * vxN + vyN * vyN;
                        if (mag2 > bestMag2) {
                            bestMag2 = mag2; bestVx = vxN; bestVy = vyN;
                        }
                    }
                }
                horizMax[(size_t)(y * W + x) * 2 + 0] = bestVx;
                horizMax[(size_t)(y * W + x) * 2 + 1] = bestVy;
            }
        }

        // Pass 2: vertical max-magnitude on the horizMax buffer. Combined with pass 1,
        // each output pixel now holds the max-magnitude velocity within a
        // (2R+1)x(2R+1) square neighborhood of the original velocity field.
        #ifdef _OPENMP
        #pragma omp parallel for
        #endif
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float bestMag2 = 0.0f, bestVx = 0.0f, bestVy = 0.0f;
                const int syLo = std::max(0, y - dilateRadius);
                const int syHi = std::min(H, y + dilateRadius + 1);
                for (int sampleY = syLo; sampleY < syHi; ++sampleY) {
                    const float vxN = horizMax[(size_t)(sampleY * W + x) * 2 + 0];
                    const float vyN = horizMax[(size_t)(sampleY * W + x) * 2 + 1];
                    const float mag2 = vxN * vxN + vyN * vyN;
                    if (mag2 > bestMag2) {
                        bestMag2 = mag2; bestVx = vxN; bestVy = vyN;
                    }
                }
                dilated[(size_t)(y * W + x) * 2 + 0] = bestVx;
                dilated[(size_t)(y * W + x) * 2 + 1] = bestVy;
            }
        }
    }
    const bool useDilated = !dilated.empty();

    // Gather-based vector blur. For each output pixel:
    //   1. Read velocity v = Velocity.xy(p) * scale
    //   2. N = clamp(ceil(|v|), 1, maxSamples)
    //   3. for s in [0, N): t = s/(N-1) + offset; weight per blurType
    //      sample = bilinear(src, p + v*t)
    //   4. out = lerp(src(p), wsum/wTotal, mix)
    #ifdef _OPENMP
    #pragma omp parallel for
    #endif
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            // Original source pixel for the lerp.
            const float* srcP = (x >= srcBounds.x1 && x < srcBounds.x2 &&
                                 y >= srcBounds.y1 && y < srcBounds.y2)
                              ? (const float*)srcRa.pixelAt(x, y) : nullptr;
            if (!srcP) { for (int c = 0; c < outNComp; ++c) dst[c] = 0.0f; continue; }

            // Read velocity at this pixel — from the dilated buffer when available,
            // otherwise from the raw velocity image. Dilation lets background pixels
            // near moving geometry pick up a velocity and gather along it.
            float vx = 0.0f, vy = 0.0f;
            if (useDilated) {
                const size_t dIdx = ((size_t)(y - outBounds.y1) * W + (x - outBounds.x1)) * 2;
                vx = dilated[dIdx + 0] * fscale;
                vy = dilated[dIdx + 1] * fscale;
            } else if (x >= velBounds.x1 && x < velBounds.x2 &&
                       y >= velBounds.y1 && y < velBounds.y2) {
                const float* vp = (const float*)velRa->pixelAt(x, y);
                if (vp && velNComp >= 2) {
                    vx = vp[0] * fscale;
                    vy = vp[1] * fscale;
                }
            }

            const float speed = std::sqrt(vx * vx + vy * vy);
            int N = (int)std::ceil(speed);
            if (N < 1) N = 1;
            if (N > maxSamples) N = maxSamples;

            // Single sample → no blur, just write source for this pixel.
            if (N == 1) {
                for (int c = 0; c < outNComp; ++c) dst[c] = srcP[c];
                continue;
            }

            const float invN1 = 1.0f / (float)(N - 1);

            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float wSum = 0.0f;
            for (int s = 0; s < N; ++s) {
                const float t = (float)s * invN1 + foffset;
                const float fx = (float)x + vx * t;
                const float fy = (float)y + vy * t;

                float w;
                if (blurTypeIdx == eBlurTypeGaussian) {
                    const float a = t * ffalloffInv;
                    w = std::exp(-(a * a));
                } else {
                    w = 1.0f;
                }

                for (int c = 0; c < outNComp; ++c) {
                    acc[c] += sampleBilinearChannel(srcRa, srcBounds, srcNComp, c, fx, fy) * w;
                }
                wSum += w;
            }

            const float denom = normalize ? (wSum > 0.0f ? wSum : 1.0f) : (float)N;
            for (int c = 0; c < outNComp; ++c) {
                const float blurred = acc[c] / denom;
                dst[c] = fmix < 1.0f ? blurred * fmix + srcP[c] * invMix : blurred;
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
