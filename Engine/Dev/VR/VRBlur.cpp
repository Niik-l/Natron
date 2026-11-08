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

#include "VRBlur.h"

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

struct VRBlurPrivate
{
    KnobDoubleWPtr size;
    KnobDoubleWPtr sizeXFactor;
    KnobChoiceWPtr quality;
    KnobBoolWPtr   latitudeComp;
    KnobDoubleWPtr mix;
};

VRBlur::VRBlur(NodePtr node)
    : EffectInstance(node)
    , _imp(new VRBlurPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VRBlur::~VRBlur()
{
}

std::string
VRBlur::getPluginDescription() const
{
    return "Seam-safe blur for VR / 360 equirectangular footage (the CaraVR C_Blur analog).\n\n"
           "A standard 2D blur breaks on latlong maps: it leaves a harsh vertical seam at the "
           "0/360-degree wrap and over-blurs the stretched pixels near the poles. Blur VR wraps "
           "the filter kernel horizontally across the seam, clamps at the poles and (with "
           "Latitude Compensation on) grows the horizontal sigma by 1/cos(latitude) toward the "
           "poles so the blur stays isotropic on the sphere.\n\n"
           "Expects an equirectangular (2:1) input; convert other projections with "
           "SphericalTransform first.";
}

void
VRBlur::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
VRBlur::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VRBlur::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
VRBlur::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"));
        k->setName("size"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(200.0);
        k->setHintToolTip(tr("Blur sigma in pixels at the equator (at the current proxy "
                             "scale). The horizontal kernel wraps across the 0/360 seam."));
        page->addKnob(k); _imp->size = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size x Factor"));
        k->setName("sizeXFactor"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Extra horizontal stretch of the blur along the wrap axis. "
                             "1 = isotropic (for a 2:1 equirectangular input)."));
        page->addKnob(k); _imp->sizeXFactor = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Quality"));
        k->setName("quality");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("low",    "Low",    "Kernel extent 2*sigma. Fastest."));
        opts.push_back(ChoiceOption("medium", "Medium", "Kernel extent 3*sigma."));
        opts.push_back(ChoiceOption("high",   "High",   "Kernel extent 4*sigma. Smoothest."));
        k->populateChoices(opts);
        k->setDefaultValue(1); // medium
        k->setHintToolTip(tr("How far the gaussian kernel extends before truncation."));
        page->addKnob(k); _imp->quality = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Latitude Compensation"));
        k->setName("latitudeComp"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Scale the horizontal sigma by 1/cos(latitude) toward the poles "
                             "(capped at 8x) so the blur is isotropic on the sphere instead of "
                             "smearing features horizontally near the poles. Turn off for a flat "
                             "2D blur that matches the stock Blur node."));
        page->addKnob(k); _imp->latitudeComp = k;
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
        k->setHintToolTip(tr("Blend between original (0) and fully blurred (1) output."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
VRBlur::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const double sizeVal      = std::max(0.0, _imp->size.lock()->getValueAtTime(args.time));
    const double sizeXFactor  = std::max(0.0, _imp->sizeXFactor.lock()->getValueAtTime(args.time));
    const int    quality      = std::max(1, _imp->quality.lock()->getValue() + 1);
    const bool   latitudeComp = _imp->latitudeComp.lock()->getValue() != 0;
    const double mixVal       = _imp->mix.lock()->getValueAtTime(args.time);

    const RectI srcBounds = srcImg->getBounds();
    const RectI outBounds = outImg->getBounds();
    const int W = srcBounds.x2 - srcBounds.x1;
    const int H = srcBounds.y2 - srcBounds.y1;
    if (W <= 0 || H <= 0) return eStatusOK;

    int nComp = std::min(srcImg->getComponents().getNumComponents(),
                         outImg->getComponents().getNumComponents());
    if (nComp > 4) nComp = 4;
    if (nComp <= 0) return eStatusOK;

    // Sigma in mip pixels at the current render scale.
    const double pxScale = args.mappedScale.toOfxPointD().x;
    VRFilterCore::BlurParams p;
    p.sigmaX = sizeVal * pxScale;
    p.sigmaY = sizeVal * pxScale;
    p.sizeXFactor = sizeXFactor;
    p.quality = quality;
    p.latitudeComp = latitudeComp;
    p.width = W;
    p.height = H;

    // Copy the source into an interleaved working buffer (row 0 = bottom row).
    std::vector<float> bufA((size_t)W * H * nComp);
    std::vector<float> bufB((size_t)W * H * nComp);
    {
        Image::ReadAccess ra(srcImg.get());
        for (int y = 0; y < H; ++y) {
            const float* srow = (const float*)ra.pixelAt(srcBounds.x1, srcBounds.y1 + y);
            float* drow = &bufA[(size_t)y * W * nComp];
            if (srow) {
                std::memcpy(drow, srow, (size_t)W * nComp * sizeof(float));
            } else {
                std::memset(drow, 0, (size_t)W * nComp * sizeof(float));
            }
        }
    }

    VRFilterCore::wrapBlur(&bufA[0], &bufB[0], W, H, nComp, p,
                           std::function<bool()>( [this]() { return aborted(); } ));

    // Write the requested region, mixing against the source.
    {
        Image::ReadAccess ra(srcImg.get());
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
                const float* srcPix = (const float*)ra.pixelAt(x, y);
                const float* filPix = &bufB[((size_t)by * W + bx) * nComp];
                if (fMix >= 1.0f) {
                    for (int c = 0; c < nComp; ++c) dst[c] = filPix[c];
                } else {
                    for (int c = 0; c < nComp; ++c) {
                        dst[c] = filPix[c] * fMix + (srcPix ? srcPix[c] : 0.0f) * invMix;
                    }
                }
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT

#include "moc_VRBlur.cpp"
