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

#include "VRExpoGlow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "VRFilterCore.h"

NATRON_NAMESPACE_ENTER

struct VRExpoGlowPrivate
{
    KnobDoubleWPtr size;
    KnobDoubleWPtr falloff;
    KnobIntWPtr    iterations;
    KnobDoubleWPtr threshold;
    KnobDoubleWPtr softness;
    KnobDoubleWPtr gain;
    KnobChoiceWPtr combine;
    KnobBoolWPtr   glowOnly;
    KnobDoubleWPtr mix;
};

enum CombineEnum
{
    eCombinePlus = 0,
    eCombineScreen,
};

VRExpoGlow::VRExpoGlow(NodePtr node)
    : EffectInstance(node)
    , _imp(new VRExpoGlowPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VRExpoGlow::~VRExpoGlow()
{
}

std::string
VRExpoGlow::getPluginDescription() const
{
    return "Fusion-style Exponential Glow, seam-safe for VR / 360 equirectangular footage "
           "(fast C++ implementation).\n\n"
           "The thresholded highlights are cascaded down a wrap-aware image pyramid; each "
           "level contributes with an exponentially decaying weight (Falloff^level), giving "
           "the wide, very smooth glow falloff of an exponential glow without any large "
           "convolutions. Horizontal passes wrap across the 0/360 seam, vertical passes "
           "clamp at the poles.\n\n"
           "Expects an equirectangular (2:1) input. Work in scene-linear for physical "
           "highlight spread.";
}

void
VRExpoGlow::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
VRExpoGlow::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VRExpoGlow::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
VRExpoGlow::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"));
        k->setName("size"); k->setDefaultValue(25.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(200.0);
        k->setHintToolTip(tr("Base spread of the glow (pixels at the equator). The core "
                             "highlight is pre-blurred by roughly Size/4 before the "
                             "exponential cascade."));
        page->addKnob(k); _imp->size = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Falloff"));
        k->setName("falloff"); k->setDefaultValue(0.55); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Per-level weight multiplier of the exponential cascade. "
                             "Lower = tighter core, higher = more spread."));
        page->addKnob(k); _imp->falloff = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Iterations"));
        k->setName("iterations"); k->setDefaultValue(6);
        k->setMinimum(1); k->setMaximum(12);
        k->setDisplayMinimum(1); k->setDisplayMaximum(12);
        k->setHintToolTip(tr("Number of pyramid levels in the cascade. Each level doubles "
                             "the glow radius."));
        page->addKnob(k); _imp->iterations = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Threshold"));
        k->setName("threshold"); k->setDefaultValue(0.85); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Values below this are not part of the glow source. In "
                             "scene-linear, 1.0 isolates the clipped highlights."));
        page->addKnob(k); _imp->threshold = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Softness"));
        k->setName("softness"); k->setDefaultValue(0.5); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Eases the threshold cut with a smoothstep (0 = hard cut, "
                             "1 = fully eased)."));
        page->addKnob(k); _imp->softness = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gain"));
        k->setName("gain"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Multiplier on the glow before it is combined back."));
        page->addKnob(k); _imp->gain = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Combine"));
        k->setName("combine");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("plus",   "Plus",   "Add the glow over the source."));
        opts.push_back(ChoiceOption("screen", "Screen", "Screen the glow over the source "
                                                         "(preserves highlights better)."));
        k->populateChoices(opts);
        k->setDefaultValue(eCombineScreen);
        k->setHintToolTip(tr("How the glow is combined back over the source."));
        page->addKnob(k); _imp->combine = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Glow Only"));
        k->setName("glowOnly"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Output only the glow itself (combine it downstream with your "
                             "own Merge)."));
        page->addKnob(k); _imp->glowOnly = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Blend between original (0) and fully glowed (1) output."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
VRExpoGlow::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const double sizeVal    = std::max(0.0, _imp->size.lock()->getValueAtTime(args.time));
    const double falloffVal = std::max(0.0, std::min(1.0, _imp->falloff.lock()->getValueAtTime(args.time)));
    const int    iterations = std::max(1, _imp->iterations.lock()->getValue());
    const double threshold  = std::max(0.0, _imp->threshold.lock()->getValueAtTime(args.time));
    const double softness   = std::max(0.0, std::min(1.0, _imp->softness.lock()->getValueAtTime(args.time)));
    const double gainVal    = std::max(0.0, _imp->gain.lock()->getValueAtTime(args.time));
    const int    combine    = _imp->combine.lock()->getValue();
    const bool   glowOnly   = _imp->glowOnly.lock()->getValue() != 0;
    const double mixVal     = _imp->mix.lock()->getValueAtTime(args.time);

    const RectI srcBounds = srcImg->getBounds();
    const RectI outBounds = outImg->getBounds();
    const int W = srcBounds.x2 - srcBounds.x1;
    const int H = srcBounds.y2 - srcBounds.y1;
    if (W <= 0 || H <= 0) return eStatusOK;

    int nComp = std::min(srcImg->getComponents().getNumComponents(),
                         outImg->getComponents().getNumComponents());
    if (nComp > 4) nComp = 4;
    if (nComp <= 0) return eStatusOK;

    const double pxScale = args.mappedScale.toOfxPointD().x;
    const float fGain = (float)gainVal;

    // Copy the source into an interleaved buffer.
    std::vector<float> src((size_t)W * H * nComp);
    {
        Image::ReadAccess ra(srcImg.get());
        for (int y = 0; y < H; ++y) {
            const float* srow = (const float*)ra.pixelAt(srcBounds.x1, srcBounds.y1 + y);
            float* drow = &src[(size_t)y * W * nComp];
            if (srow) {
                std::memcpy(drow, srow, (size_t)W * nComp * sizeof(float));
            } else {
                std::memset(drow, 0, (size_t)W * nComp * sizeof(float));
            }
        }
    }

    // Threshold the highlights into the glow source.
    std::vector<float> glowSrc((size_t)W * H * nComp);
    VRFilterCore::thresholdGain(&src[0], &glowSrc[0], (size_t)W * H, nComp,
                                (float)threshold, (float)softness, 1.0f);

    // Base pre-blur: Size/4 sigma, seam-safe (cheap via the core's own pyramid path).
    if (sizeVal > 1e-4) {
        std::vector<float> blurred((size_t)W * H * nComp);
        VRFilterCore::BlurParams bp;
        bp.sigmaX = sizeVal * pxScale * 0.25;
        bp.sigmaY = sizeVal * pxScale * 0.25;
        bp.sizeXFactor = 1.0;
        bp.quality = 2;
        bp.latitudeComp = true;
        bp.width = W;
        bp.height = H;
        VRFilterCore::wrapBlur(&glowSrc[0], &blurred[0], W, H, nComp, bp,
                               std::function<bool()>( [this]() { return aborted(); } ));
        glowSrc.swap(blurred);
    }
    if ( aborted() ) return eStatusOK;

    // Exponential cascade: levels 1..iterations of the pyramid, weight falloff^level.
    const int maxLevels = iterations + 1;
    std::vector<std::pair<int, int> > sizes;
    std::vector<std::vector<float> > levels =
        VRFilterCore::buildPyramid(&glowSrc[0], W, H, nComp, maxLevels, &sizes);
    const int nLevels = (int)levels.size();

    std::vector<float> glow((size_t)W * H * nComp, 0.0f);
    double weightSum = 0.0;
    for (int i = 1; i < nLevels && i <= iterations; ++i) {
        weightSum += std::pow(falloffVal, (double)(i - 1));
    }
    if (weightSum < 1e-6) weightSum = 1.0;

    for (int i = 1; i < nLevels && i <= iterations; ++i) {
        const float weight = (float)(std::pow(falloffVal, (double)(i - 1)) / weightSum);
        const int lw = sizes[i].first;
        const int lh = sizes[i].second;
        VRFilterCore::upsampleAccumulateWrap(&levels[i][0], lw, lh, &glow[0], W, H, nComp, weight);
        if ( aborted() ) return eStatusOK;
    }

    // Combine.
    {
        Image::WriteAccess wa(outImg.get());
        const float fMix = (float)mixVal;
        const float invMix = 1.0f - fMix;
        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            const int by = y - srcBounds.y1;
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const int bx = x - srcBounds.x1;
                if (bx < 0 || bx >= W || by < 0 || by >= H) {
                    for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                    continue;
                }
                const float* srcPix = &src[((size_t)by * W + bx) * nComp];
                const float* glowPix = &glow[((size_t)by * W + bx) * nComp];

                float result[4];
                if (glowOnly) {
                    for (int c = 0; c < nComp; ++c) result[c] = glowPix[c] * fGain;
                    if (nComp == 4) result[3] = srcPix[3];
                } else if (combine == eCombineScreen) {
                    for (int c = 0; c < nComp; ++c) {
                        const float g = glowPix[c] * fGain;
                        result[c] = srcPix[c] + g - srcPix[c] * g;
                    }
                } else { // plus
                    for (int c = 0; c < nComp; ++c) {
                        result[c] = srcPix[c] + glowPix[c] * fGain;
                    }
                }

                if (fMix < 1.0f) {
                    for (int c = 0; c < nComp; ++c) {
                        dst[c] = result[c] * fMix + srcPix[c] * invMix;
                    }
                } else {
                    for (int c = 0; c < nComp; ++c) dst[c] = result[c];
                }
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT

#include "moc_VRExpoGlow.cpp"
