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

#include "VRChromaticAberration.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <QtConcurrentMap> // blockingMap

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "VRFilterCore.h"

NATRON_NAMESPACE_ENTER

namespace {

const double kPi = 3.14159265358979323846;

// Horizontal offset multiplier at buffer row y (0 = one pole, H-1 = the other):
// 1/cos(latitude), capped, mirroring VRFilterCore's latitude sigma rule.
double
latitudeOffsetMult(int row, int H, double maxStretch)
{
    if (H <= 1) return 1.0;
    const double v = ((double)row + 0.5) / (double)H; // 0..1 across the latitude range
    const double lat = kPi * (v - 0.5);               // -pi/2 .. pi/2
    double c = std::cos(lat);
    if (c < 0.0) c = 0.0;
    const double floor_ = 1.0 / std::max(1.0, maxStretch);
    if (c < floor_) c = floor_;
    return 1.0 / c;
}

// Smoothstep ramp for the radial falloff: 0 at radius start, 1 at radius end.
// Radii are normalized (1.0 = half the image height).
double
falloffWeight(double rNorm, double start, double end)
{
    if (end <= start) return rNorm >= end ? 1.0 : 0.0;
    double t = (rNorm - start) / (end - start);
    if (t < 0.0) t = 0.0;
    else if (t > 1.0) t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}

} // anonymous namespace

struct VRChromaticAberrationPrivate
{
    KnobDoubleWPtr centerX;
    KnobDoubleWPtr centerY;
    KnobDoubleWPtr aberration;
    KnobDoubleWPtr shiftX;
    KnobDoubleWPtr shiftY;
    KnobDoubleWPtr rotation;
    KnobDoubleWPtr falloffStart;
    KnobDoubleWPtr falloffEnd;
    KnobDoubleWPtr mult[3]; // red, green, blue
    KnobDoubleWPtr blur[3]; // red, green, blue
    KnobBoolWPtr   latitudeComp;
    KnobDoubleWPtr mix;
};

VRChromaticAberration::VRChromaticAberration(NodePtr node)
    : EffectInstance(node)
    , _imp(new VRChromaticAberrationPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VRChromaticAberration::~VRChromaticAberration()
{
}

std::string
VRChromaticAberration::getPluginDescription() const
{
    return "Seam-safe chromatic aberration for VR / 360 equirectangular footage, modeled "
           "on Nukepedia's \"More Chromatic Aberration\" gizmo.\n\n"
           "Red, Green and Blue are displaced independently — radially from an adjustable "
           "Center, plus a per-channel Shift and a per-channel Rotation around the Center "
           "for stylized splits — and each channel can be blurred on its own. The Falloff "
           "ramp eases the radial offset from zero at the Center to full strength outward "
           "(the gizmo's godrays-driven start-to-end transition, adapted to a latlong "
           "radial ramp).\n\n"
           "A standard 2D chromatic aberration tears a vertical seam at the 0/360-degree "
           "wrap of a latlong map. ChromaticAberration VR samples every channel through "
           "the VR seam rules: the bilinear fetch wraps horizontally across the seam, "
           "clamps at the poles and (with Latitude Compensation on) grows the horizontal "
           "offset by 1/cos(latitude) toward the poles so the split stays isotropic on "
           "the sphere. Alpha is never displaced.\n\n"
           "Expects an equirectangular (2:1) input; convert other projections with "
           "SphericalTransform first.";
}

void
VRChromaticAberration::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
VRChromaticAberration::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VRChromaticAberration::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
VRChromaticAberration::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center X"));
        k->setName("centerX"); k->setDefaultValue(0.5); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Horizontal origin of the radial aberration, normalized over "
                             "the image width (0.5 = the center of the latlong map)."));
        page->addKnob(k); _imp->centerX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center Y"));
        k->setName("centerY"); k->setDefaultValue(0.5); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Vertical origin of the radial aberration, normalized over "
                             "the image height (0.5 = the equator on a latlong map)."));
        page->addKnob(k); _imp->centerY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Aberration"));
        k->setName("aberration"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Radial RGB displacement in pixels at the equator (at the "
                             "current proxy scale). Each channel moves outward by "
                             "Aberration x its channel multiplier; negative multipliers "
                             "move a channel inward. The displacement wraps across the "
                             "0/360 seam."));
        page->addKnob(k); _imp->aberration = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shift X"));
        k->setName("shiftX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Constant horizontal translation per channel, in pixels "
                             "(scaled by the channel multiplier). Unlike the radial "
                             "Aberration this is not shaped by the Falloff ramp."));
        page->addKnob(k); _imp->shiftX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shift Y"));
        k->setName("shiftY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Constant vertical translation per channel, in pixels "
                             "(scaled by the channel multiplier). Unlike the radial "
                             "Aberration this is not shaped by the Falloff ramp."));
        page->addKnob(k); _imp->shiftY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotation"));
        k->setName("rotation"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-45.0); k->setDisplayMaximum(45.0);
        k->setHintToolTip(tr("Rotates each channel around the Center by this many degrees "
                             "(scaled by the channel multiplier) — the stylized swirl "
                             "split from the More Chromatic Aberration gizmo."));
        page->addKnob(k); _imp->rotation = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Falloff Start"));
        k->setName("falloffStart"); k->setDefaultValue(0.1); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Radius (normalized, 1.0 = half the image height) where the "
                             "radial offset starts ramping up from zero. The smooth "
                             "start-to-end transition is the VR adaptation of the gizmo's "
                             "godrays-driven offset."));
        page->addKnob(k); _imp->falloffStart = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Falloff End"));
        k->setName("falloffEnd"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Radius (normalized, 1.0 = half the image height) where the "
                             "radial offset reaches full strength. End <= Start gives a "
                             "hard step at the Center."));
        page->addKnob(k); _imp->falloffEnd = k;
    }

    static const char* const multNames[3]   = { "redMult", "greenMult", "blueMult" };
    static const char* const multLabels[3]  = { "Red Mult", "Green Mult", "Blue Mult" };
    static const double      multDefaults[3] = { 1.0, 0.0, -1.0 };
    static const char* const blurNames[3]   = { "redBlur", "greenBlur", "blueBlur" };
    static const char* const blurLabels[3]  = { "Red Blur", "Green Blur", "Blue Blur" };

    for (int c = 0; c < 3; ++c) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(multLabels[c]));
        k->setName(multNames[c]); k->setDefaultValue(multDefaults[c]); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-2.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("How much of Aberration, Shift and Rotation this channel "
                             "gets: 0 = untouched, 1 = outward, -1 = inward (the classic "
                             "split is R 1 / G 0 / B -1)."));
        page->addKnob(k); _imp->mult[c] = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }
    for (int c = 0; c < 3; ++c) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(blurLabels[c]));
        k->setName(blurNames[c]); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(200.0);
        k->setHintToolTip(tr("Gaussian sigma in pixels at the equator for a per-channel "
                             "blur applied after the displacement (the gizmo's "
                             "per-channel blur). The kernel wraps across the 0/360 seam "
                             "and compensates latitude like Blur VR."));
        page->addKnob(k); _imp->blur[c] = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Latitude Compensation"));
        k->setName("latitudeComp"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Scale the horizontal offsets and per-channel blur by "
                             "1/cos(latitude) toward the poles (capped at 8x) so the split "
                             "stays isotropic on the sphere. Turn off for a flat 2D "
                             "chromatic aberration."));
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
        k->setHintToolTip(tr("Blend between original (0) and fully aberrated (1) output."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
VRChromaticAberration::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const double aberration  = std::max(0.0, _imp->aberration.lock()->getValueAtTime(args.time));
    const double shiftXVal   = _imp->shiftX.lock()->getValueAtTime(args.time);
    const double shiftYVal   = _imp->shiftY.lock()->getValueAtTime(args.time);
    const double rotationVal = _imp->rotation.lock()->getValueAtTime(args.time);
    const double fStart      = std::max(0.0, _imp->falloffStart.lock()->getValueAtTime(args.time));
    const double fEnd        = std::max(0.0, _imp->falloffEnd.lock()->getValueAtTime(args.time));
    const bool   latitudeComp = _imp->latitudeComp.lock()->getValue() != 0;
    const double mixVal      = _imp->mix.lock()->getValueAtTime(args.time);
    const double centerX     = _imp->centerX.lock()->getValueAtTime(args.time);
    const double centerY     = _imp->centerY.lock()->getValueAtTime(args.time);

    const RectI srcBounds = srcImg->getBounds();
    const RectI outBounds = outImg->getBounds();
    const int W = srcBounds.x2 - srcBounds.x1;
    const int H = srcBounds.y2 - srcBounds.y1;
    if (W <= 0 || H <= 0) return eStatusOK;

    int nComp = std::min(srcImg->getComponents().getNumComponents(),
                         outImg->getComponents().getNumComponents());
    if (nComp > 4) nComp = 4;
    if (nComp <= 0) return eStatusOK;

    // Chromatic aberration is a color effect: alpha-only planes pass through untouched.
    const int nColor = std::min(nComp, 3);

    // Pixel amounts at the current render (mip) scale.
    const double pxScale = args.mappedScale.toOfxPointD().x;

    double mult[3], cosR[3], sinR[3], shiftXc[3], shiftYc[3], blurS[3];
    for (int c = 0; c < 3; ++c) {
        mult[c] = (c < nColor) ? _imp->mult[c].lock()->getValueAtTime(args.time) : 0.0;
        blurS[c] = (c < nColor)
            ? std::max(0.0, _imp->blur[c].lock()->getValueAtTime(args.time)) * pxScale : 0.0;
        const double rotRad = rotationVal * mult[c] * kPi / 180.0;
        cosR[c] = std::cos(rotRad);
        sinR[c] = std::sin(rotRad);
        shiftXc[c] = shiftXVal * mult[c] * pxScale;
        shiftYc[c] = shiftYVal * mult[c] * pxScale;
    }

    const bool offsetsIdentity =
        aberration < 1e-9 && std::fabs(shiftXVal) < 1e-9 &&
        std::fabs(shiftYVal) < 1e-9 && std::fabs(rotationVal) < 1e-9;

    const float fMix = (float)std::max(0.0, std::min(1.0, mixVal));

    if (fMix <= 0.0f || nColor == 0) {
        // Nothing to do: blit the source straight through.
        Image::ReadAccess ra(srcImg.get());
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
                const float* srcPix = (const float*)ra.pixelAt(x, y);
                if (srcPix) {
                    std::memcpy(dst, srcPix, (size_t)nComp * sizeof(float));
                } else {
                    for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                }
            }
        }
        return eStatusOK;
    }

    // Copy the source into an interleaved working buffer (row 0 = bottom row).
    std::vector<float> bufA((size_t)W * H * nComp);
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

    if (offsetsIdentity) {
        // Identity offsets: the source is the result before the per-channel blur.
        std::vector<float> bufB(bufA);
        for (int c = 0; c < nColor; ++c) {
            if (blurS[c] < 1e-4) continue;
            VRFilterCore::BlurParams p;
            p.sigmaX = blurS[c];
            p.sigmaY = blurS[c];
            p.quality = 2; // medium: kernel extent 3*sigma
            p.latitudeComp = latitudeComp;
            p.width = W;
            p.height = H;
            std::vector<float> monoA((size_t)W * H);
            std::vector<float> monoB((size_t)W * H);
            for (int y = 0; y < H; ++y) {
                const float* srow = &bufB[(size_t)y * W * nComp];
                float* drow = &monoA[(size_t)y * W];
                for (int x = 0; x < W; ++x) drow[x] = srow[(size_t)x * nComp + c];
            }
            VRFilterCore::wrapBlur(&monoA[0], &monoB[0], W, H, 1, p,
                                   std::function<bool()>( [this]() { return aborted(); } ));
            for (int y = 0; y < H; ++y) {
                const float* srow = &monoB[(size_t)y * W];
                float* drow = &bufB[(size_t)y * W * nComp];
                for (int x = 0; x < W; ++x) drow[(size_t)x * nComp + c] = srow[x];
            }
        }

        // Write the requested region, mixing against the source.
        Image::ReadAccess ra(srcImg.get());
        Image::WriteAccess wa(outImg.get());
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
        return eStatusOK;
    }

    // Displace each color channel with the per-pixel offset field.
    const double radAmt = aberration * pxScale;
    const double cx = centerX * (double)W;
    const double cy = centerY * (double)H;
    const double rNormH = 0.5 * (double)H;
    std::vector<float> bufB((size_t)W * H * nComp);

    auto processRow = [&](int y) {
        const double latScale = latitudeComp ? latitudeOffsetMult(y, H, 8.0) : 1.0;
        const float* srcRow = &bufA[(size_t)y * W * nComp];
        float* dstRow = &bufB[(size_t)y * W * nComp];
        const double py = (double)y + 0.5;
        const double qy = py - cy;
        const double dy = qy;
        for (int x = 0; x < W; ++x) {
            const double px = (double)x + 0.5;
            const double qx = px - cx;
            const double dx = qx;
            const double r = std::sqrt(dx * dx + dy * dy);
            const double w = falloffWeight(r / rNormH, fStart, fEnd);
            double dirX = 0.0, dirY = 0.0;
            if (r > 1e-9) {
                dirX = dx / r;
                dirY = dy / r;
            }
            const float* srcPix = srcRow + (size_t)x * nComp;
            float* dstPix = dstRow + (size_t)x * nComp;
            float samp[4];
            for (int c = 0; c < nComp; ++c) {
                if (c >= nColor) {
                    dstPix[c] = srcPix[c]; // alpha (and any extra channel) untouched
                    continue;
                }
                const double offX = (dirX * radAmt * mult[c] * w + shiftXc[c]) * latScale;
                const double offY = dirY * radAmt * mult[c] * w + shiftYc[c];
                const double sx = cx + (qx * cosR[c] + qy * sinR[c]) + offX;
                const double sy = cy + (-qx * sinR[c] + qy * cosR[c]) + offY;
                VRFilterCore::wrapBilinearSample(&bufA[0], W, H, nComp, sx, sy, samp);
                dstPix[c] = samp[c];
            }
        }
    };

    // Batched parallel rows; abort lives in the render thread's TLS, so the poll runs
    // between batches (same pattern as VRFilterCore / SphericalTransform).
    {
        const int kBatchRows = 64;
        for (int batchY = 0; batchY < H; batchY += kBatchRows) {
            if (aborted()) return eStatusFailed;
            const int batchEnd = std::min(batchY + kBatchRows, H);
            std::vector<int> rows;
            rows.reserve(batchEnd - batchY);
            for (int y = batchY; y < batchEnd; ++y) rows.push_back(y);
            QtConcurrent::blockingMap(rows, processRow);
        }
    }

    // Per-channel blur (the gizmo's per-channel blur), seam-safe via wrapBlur.
    for (int c = 0; c < nColor; ++c) {
        if (blurS[c] < 1e-4) continue;
        VRFilterCore::BlurParams p;
        p.sigmaX = blurS[c];
        p.sigmaY = blurS[c];
        p.quality = 2; // medium: kernel extent 3*sigma
        p.latitudeComp = latitudeComp;
        p.width = W;
        p.height = H;
        std::vector<float> monoA((size_t)W * H);
        std::vector<float> monoB((size_t)W * H);
        for (int y = 0; y < H; ++y) {
            const float* srow = &bufB[(size_t)y * W * nComp];
            float* drow = &monoA[(size_t)y * W];
            for (int x = 0; x < W; ++x) drow[x] = srow[(size_t)x * nComp + c];
        }
        VRFilterCore::wrapBlur(&monoA[0], &monoB[0], W, H, 1, p,
                               std::function<bool()>( [this]() { return aborted(); } ));
        for (int y = 0; y < H; ++y) {
            const float* srow = &monoB[(size_t)y * W];
            float* drow = &bufB[(size_t)y * W * nComp];
            for (int x = 0; x < W; ++x) drow[(size_t)x * nComp + c] = srow[x];
        }
    }

    // Write the requested region, mixing against the source.
    {
        Image::ReadAccess ra(srcImg.get());
        Image::WriteAccess wa(outImg.get());
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

#include "moc_VRChromaticAberration.cpp"
