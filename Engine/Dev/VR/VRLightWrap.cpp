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

#include "VRLightWrap.h"

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

// Inputs
enum
{
    eInputSource = 0,  // foreground (carries the alpha matte)
    eInputBackground,  // background the light wraps from
};

struct VRLightWrapPrivate
{
    KnobDoubleWPtr amount;
    KnobDoubleWPtr wrapSize;
    KnobDoubleWPtr edgeSoftness;
    KnobChoiceWPtr combine;
    KnobDoubleWPtr mix;
};

enum CombineEnum
{
    eCombinePlus = 0,
    eCombineScreen,
};

VRLightWrap::VRLightWrap(NodePtr node)
    : EffectInstance(node)
    , _imp(new VRLightWrapPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VRLightWrap::~VRLightWrap()
{
}

std::string
VRLightWrap::getPluginDescription() const
{
    return "Wraps light from the Background around the foreground edge, seam-safe for "
           "VR / 360 equirectangular comps.\n\n"
           "The Background is blurred with the seam-safe VR blur (the glow crosses the "
           "0/360-degree wrap instead of stopping at it), masked to the foreground's edge "
           "band from the Source alpha, and combined (Plus or Screen) over the "
           "over(Source, Background) composite. Edge Softness shapes the band: 0 gives the "
           "classic gizmo behavior (wrap everywhere the foreground is absent), larger "
           "values confine the wrap to a band just inside the foreground edge where the "
           "light physically wraps.\n\n"
           "Expects equirectangular (2:1) inputs. The foreground alpha drives the wrap; "
           "without a Background the node passes the Source through.";
}

std::string
VRLightWrap::getInputLabel(int inputNb) const
{
    switch (inputNb) {
    case eInputSource:     return "Source";
    case eInputBackground: return "Background";
    default: return "";
    }
}

bool
VRLightWrap::isInputOptional(int inputNb) const
{
    return inputNb == eInputBackground;
}

void
VRLightWrap::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
VRLightWrap::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VRLightWrap::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

StatusEnum
VRLightWrap::getRegionOfDefinition(U64 /*hash*/,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    // The output is the foreground composite: RoD of the Source only (the default
    // implementation would union the Background's RoD).
    EffectInstancePtr input = getInput(eInputSource);
    if (!input) return eStatusFailed;
    bool isProject;
    StatusEnum st = input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
    if (st != eStatusOK && st != eStatusReplyDefault) return st;
    return eStatusOK;
}

void
VRLightWrap::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Wrap Amount"));
        k->setName("wrapAmount"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Strength of the wrapped light."));
        page->addKnob(k); _imp->amount = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Wrap Size"));
        k->setName("wrapSize"); k->setDefaultValue(50.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(300.0);
        k->setHintToolTip(tr("Blur sigma (pixels at the equator) of the background light "
                             "that wraps around the edge. The blur wraps across the 0/360 "
                             "seam."));
        page->addKnob(k); _imp->wrapSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Edge Softness"));
        k->setName("edgeSoftness"); k->setDefaultValue(10.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Width of the edge band the wrap is confined to (pixels at "
                             "the equator). 0 = classic behavior: wrap everywhere the "
                             "foreground is absent."));
        page->addKnob(k); _imp->edgeSoftness = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Combine"));
        k->setName("combine");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("plus",   "Plus",   "Add the wrap over the composite."));
        opts.push_back(ChoiceOption("screen", "Screen", "Screen the wrap over the "
                                                         "composite."));
        k->populateChoices(opts);
        k->setDefaultValue(eCombineScreen);
        k->setHintToolTip(tr("How the wrap is combined over the composite."));
        page->addKnob(k); _imp->combine = k;
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
        k->setHintToolTip(tr("Blend between the plain composite (0) and the wrapped "
                             "composite (1)."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
VRLightWrap::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(eInputSource, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    RectI bgRoi;
    ImagePtr bgImg = getInput(eInputBackground)
                     ? getImage(eInputBackground, args.time, args.mappedScale, args.view,
                                NULL, NULL, false, false,
                                eStorageModeRAM, 0, &bgRoi)
                     : ImagePtr();

    const double amountVal   = std::max(0.0, _imp->amount.lock()->getValueAtTime(args.time));
    const double wrapSizeVal = std::max(0.0, _imp->wrapSize.lock()->getValueAtTime(args.time));
    const double softnessVal = std::max(0.0, _imp->edgeSoftness.lock()->getValueAtTime(args.time));
    const int    combine     = _imp->combine.lock()->getValue();
    const double mixVal      = _imp->mix.lock()->getValueAtTime(args.time);

    const RectI srcBounds = srcImg->getBounds();
    const RectI outBounds = outImg->getBounds();
    const int W = srcBounds.x2 - srcBounds.x1;
    const int H = srcBounds.y2 - srcBounds.y1;
    if (W <= 0 || H <= 0) return eStatusOK;

    int nComp = std::min(srcImg->getComponents().getNumComponents(),
                         outImg->getComponents().getNumComponents());
    if (nComp > 4) nComp = 4;
    if (nComp <= 0) return eStatusOK;
    const int srcNCompRaw = srcImg->getComponents().getNumComponents();
    const bool srcHasAlpha = (srcNCompRaw >= 4);

    const double pxScale = args.mappedScale.toOfxPointD().x;

    // Internal buffers are always RGBA (4 channels): the over composite and the wrap
    // math need alpha even when the inputs are RGB. The output write clips to nComp.
    const int kBufCh = 4;

    // Copy the source into an interleaved RGBA buffer (alpha = 1 when absent, so the
    // composite degenerates to the source).
    std::vector<float> src((size_t)W * H * kBufCh);
    {
        Image::ReadAccess ra(srcImg.get());
        for (int y = 0; y < H; ++y) {
            const float* srow = (const float*)ra.pixelAt(srcBounds.x1, srcBounds.y1 + y);
            float* drow = &src[(size_t)y * W * kBufCh];
            if (srow) {
                for (int x = 0; x < W; ++x) {
                    for (int c = 0; c < kBufCh; ++c) {
                        drow[x * kBufCh + c] = (c < srcNCompRaw) ? srow[x * srcNCompRaw + c] : 1.0f;
                    }
                }
            } else {
                std::memset(drow, 0, (size_t)W * kBufCh * sizeof(float));
            }
        }
    }

    // No background: passthrough of the source.
    if (!bgImg) {
        Image::WriteAccess wa(outImg.get());
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
                const float* srcPix = &src[((size_t)by * W + bx) * kBufCh];
                for (int c = 0; c < nComp; ++c) dst[c] = srcPix[c];
            }
        }
        return eStatusOK;
    }

    int bgNComp = bgImg->getComponents().getNumComponents();
    if (bgNComp > 4) bgNComp = 4;
    const bool bgHasAlpha = (bgNComp >= 4);

    // Background RGBA buffer over the source bounds (bg sampled at absolute
    // coordinates, zero where the background image does not reach).
    std::vector<float> bg((size_t)W * H * kBufCh);
    {
        Image::ReadAccess bra(bgImg.get());
        const RectI bgBounds = bgImg->getBounds();
        for (int y = 0; y < H; ++y) {
            const int gy = srcBounds.y1 + y;
            const bool yOk = (gy >= bgBounds.y1 && gy < bgBounds.y2);
            float* drow = &bg[(size_t)y * W * kBufCh];
            for (int x = 0; x < W; ++x) {
                float* d = drow + (size_t)x * kBufCh;
                const int gx = srcBounds.x1 + x;
                if (yOk && gx >= bgBounds.x1 && gx < bgBounds.x2) {
                    const float* p = (const float*)bra.pixelAt(gx, gy);
                    if (p) {
                        for (int c = 0; c < kBufCh; ++c) {
                            d[c] = (c < bgNComp) ? p[c] : 1.0f;
                        }
                        if (!bgHasAlpha) d[3] = 1.0f;
                        continue;
                    }
                }
                for (int c = 0; c < kBufCh; ++c) d[c] = 0.0f;
            }
        }
    }

    // Blurred background light (seam-safe).
    std::vector<float> bgWrapped((size_t)W * H * kBufCh);
    if (wrapSizeVal > 1e-4) {
        VRFilterCore::BlurParams bp;
        bp.sigmaX = wrapSizeVal * pxScale;
        bp.sigmaY = wrapSizeVal * pxScale;
        bp.sizeXFactor = 1.0;
        bp.quality = 2;
        bp.latitudeComp = true;
        bp.width = W;
        bp.height = H;
        VRFilterCore::wrapBlur(&bg[0], &bgWrapped[0], W, H, kBufCh, bp,
                               std::function<bool()>( [this]() { return aborted(); } ));
    } else {
        std::memcpy(&bgWrapped[0], &bg[0], (size_t)W * H * kBufCh * sizeof(float));
    }
    if ( aborted() ) return eStatusOK;

    // Edge band from the source alpha: band = max(0, blur(1-a) - (1-a)) puts the wrap on
    // the foreground edge; softness = 0 keeps the classic (1-a) mask.
    std::vector<float> band((size_t)W * H);
    {
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            const float a = src[i * kBufCh + 3];
            band[i] = 1.0f - a;
        }
        if (softnessVal > 1e-4) {
            std::vector<float> blurredBand((size_t)W * H);
            VRFilterCore::BlurParams bp;
            bp.sigmaX = softnessVal * pxScale;
            bp.sigmaY = softnessVal * pxScale;
            bp.sizeXFactor = 1.0;
            bp.quality = 2;
            bp.latitudeComp = true;
            bp.width = W;
            bp.height = H;
            VRFilterCore::wrapBlur(&band[0], &blurredBand[0], W, H, 1, bp,
                                   std::function<bool()>( [this]() { return aborted(); } ));
            for (size_t i = 0; i < (size_t)W * H; ++i) {
                float b = blurredBand[i] - band[i];
                band[i] = b > 0.0f ? b : 0.0f;
            }
        }
    }
    if ( aborted() ) return eStatusOK;

    // Composite and wrap.
    {
        Image::WriteAccess wa(outImg.get());
        const float fAmount = (float)amountVal;
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
                const size_t pixIdx = (size_t)by * W + bx;
                const float* srcPix = &src[pixIdx * kBufCh];
                const float* bgPix = &bg[pixIdx * kBufCh];
                const float* wrapPix = &bgWrapped[pixIdx * kBufCh];
                const float sa = srcPix[3];
                const float ba = bgPix[3];

                // over(src, bg), premultiplied convention.
                float comp[4];
                for (int c = 0; c < 3; ++c) {
                    comp[c] = srcPix[c] + (1.0f - sa) * bgPix[c];
                }
                comp[3] = sa + (1.0f - sa) * ba;

                // Wrapped light, masked to the edge band.
                const float w = band[pixIdx] * fAmount;
                float result[4];
                for (int c = 0; c < kBufCh; ++c) result[c] = comp[c];
                if (combine == eCombineScreen) {
                    for (int c = 0; c < 3; ++c) {
                        const float g = wrapPix[c] * w;
                        result[c] = comp[c] + g - comp[c] * g;
                    }
                } else { // plus
                    for (int c = 0; c < 3; ++c) {
                        result[c] = comp[c] + wrapPix[c] * w;
                    }
                }

                if (fMix < 1.0f) {
                    for (int c = 0; c < nComp; ++c) {
                        dst[c] = result[c] * fMix + comp[c] * invMix;
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

#include "moc_VRLightWrap.cpp"
