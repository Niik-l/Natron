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

#include "VRGlow.h"

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

struct VRGlowPrivate
{
    KnobDoubleWPtr size;
    KnobDoubleWPtr ratio;
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

VRGlow::VRGlow(NodePtr node)
    : EffectInstance(node)
    , _imp(new VRGlowPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VRGlow::~VRGlow()
{
}

std::string
VRGlow::getPluginDescription() const
{
    return "Seam-safe glow for VR / 360 equirectangular footage.\n\n"
           "Isolates the highlights (Threshold with a Soft knee), spreads them with a stack "
           "of gaussian kernels of geometrically increasing size (Kawase bloom) computed on a "
           "wrap-aware pyramid, and adds or screens the glow back over the source. The "
           "filtering wraps horizontally across the 0/360 seam and stays isotropic on the "
           "sphere near the poles, so no seam line appears in the headset view.\n\n"
           "Expects an equirectangular (2:1) input. Work in scene-linear for physical "
           "highlight spread.";
}

void
VRGlow::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
VRGlow::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VRGlow::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
VRGlow::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"));
        k->setName("size"); k->setDefaultValue(25.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(200.0);
        k->setHintToolTip(tr("Base glow sigma in pixels at the equator. The kernel stack "
                             "spans Size, Size*Ratio, Size*Ratio^2, ... for Iterations "
                             "kernels."));
        page->addKnob(k); _imp->size = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Ratio"));
        k->setName("ratio"); k->setDefaultValue(2.0); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Geometric growth factor between successive kernels. 2 matches "
                             "the reference bloom implementation."));
        page->addKnob(k); _imp->ratio = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Iterations"));
        k->setName("iterations"); k->setDefaultValue(5);
        k->setMinimum(1); k->setMaximum(10);
        k->setDisplayMinimum(1); k->setDisplayMaximum(10);
        k->setHintToolTip(tr("Number of kernels in the stack. Higher = wider, heavier-tailed "
                             "glow."));
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
VRGlow::render(const RenderActionArgs& args)
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
    const double ratioVal   = std::max(1.0, _imp->ratio.lock()->getValueAtTime(args.time));
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

    // Wrap-aware pyramid (each level halves, wrap in x, clamp in y).
    const int maxLevels = 12;
    std::vector<std::pair<int, int> > sizes;
    std::vector<std::vector<float> > levels =
        VRFilterCore::buildPyramid(&glowSrc[0], W, H, nComp, maxLevels, &sizes);
    const int nLevels = (int)levels.size();

    // Accumulate the kernel stack: kernel i has full-res sigma = size * ratio^i and is
    // evaluated on the pyramid level that keeps its local sigma in the cheap range.
    std::vector<float> glow((size_t)W * H * nComp, 0.0f);
    const float weight = 1.0f / (float)iterations;
    VRFilterCore::BlurParams bp;
    bp.sizeXFactor = 1.0;
    bp.quality = 2;
    bp.latitudeComp = true;
    for (int i = 0; i < iterations; ++i) {
        const double sigmaFull = sizeVal * pxScale * std::pow(ratioVal, (double)i);
        if (sigmaFull < 1e-4) continue;
        // Pick the level whose local sigma lands near 1.5 pixels.
        int level = (int)std::floor(std::log2(std::max(1.0, sigmaFull / 1.5)) + 0.5);
        if (level < 0) level = 0;
        if (level > nLevels - 1) level = nLevels - 1;
        const int lw = sizes[level].first;
        const int lh = sizes[level].second;
        const double sigmaLocal = sigmaFull / std::pow(2.0, (double)level);
        bp.sigmaX = sigmaLocal;
        bp.sigmaY = sigmaLocal;
        bp.width = lw;
        bp.height = lh;
        std::vector<float> blurred((size_t)lw * lh * nComp);
        VRFilterCore::wrapBlur(&levels[level][0], &blurred[0], lw, lh, nComp, bp,
                               std::function<bool()>( [this]() { return aborted(); } ));
        VRFilterCore::upsampleAccumulateWrap(&blurred[0], lw, lh, &glow[0], W, H, nComp, weight);
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

#include "moc_VRGlow.cpp"
