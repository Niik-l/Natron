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

#include "LensWarp.h"

#include <cmath>
#include <algorithm>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

#include <libmv/simple_pipeline/camera_intrinsics.h>

NATRON_NAMESPACE_ENTER

struct LensWarpPrivate
{
    KnobChoiceWPtr direction;
    KnobDoubleWPtr focalMm;
    KnobDoubleWPtr sensorWidthMm;
    KnobDoubleWPtr principalPoint;   // 2D, px; (0,0) = image center
    KnobDoubleWPtr k1, k2, k3;
};

LensWarp::LensWarp(NodePtr node)
    : EffectInstance(node)
    , _imp(new LensWarpPrivate())
{
}

LensWarp::~LensWarp()
{
}

std::string
LensWarp::getPluginDescription() const
{
    return "Undistort or redistort a plate with the CameraTracker's polynomial lens model "
           "(k1/k2/k3 radial, focal-normalized — identical math to the camera solve).\n\n"
           "Workflow: solve with 'Refine Lens Distortion' on, press 'Create Undistort Node' "
           "on the CameraTracker, comp and render pinhole elements (ScanlineRender/Camera3D) "
           "over the undistorted plate, then apply a Redistort as the final step.";
}

std::string
LensWarp::getInputLabel(int /*inputNb*/) const
{
    return "Source";
}

bool
LensWarp::isInputOptional(int /*inputNb*/) const
{
    return false;
}

void
LensWarp::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
}

void
LensWarp::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
LensWarp::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Direction"));
        k->setName("direction");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Undistort", "Undistort",
                                       "Remove lens distortion: output is the pinhole image the "
                                       "solved camera lives in. Use on the plate before pinhole "
                                       "comp/render."));
        entries.push_back(ChoiceOption("Redistort", "Redistort",
                                       "Re-apply lens distortion: use on the finished comp as the "
                                       "last step so it matches the original photography."));
        k->populateChoices(entries);
        k->setHintToolTip(tr("Undistort removes the lens bend (plate -> pinhole); Redistort "
                             "puts it back (pinhole comp -> match original photography)."));
        page->addKnob(k);
        _imp->direction = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focal Length (mm)"));
        k->setName("focalLengthMm");
        k->setDefaultValue(35.0);
        k->setHintToolTip(tr("Focal length the distortion coefficients were solved at "
                             "(the coefficients are focal-normalized, so this and Sensor "
                             "Width must match the solve)."));
        page->addKnob(k);
        _imp->focalMm = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Sensor Width (mm)"));
        k->setName("sensorWidth");
        k->setDefaultValue(24.576);
        page->addKnob(k);
        _imp->sensorWidthMm = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Principal Point"), 2);
        k->setName("principalPoint");
        k->setDefaultValue(0.0, 0);
        k->setDefaultValue(0.0, 1);
        k->setHintToolTip(tr("Distortion center in pixels; (0, 0) means the image center."));
        page->addKnob(k);
        _imp->principalPoint = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("K1"));
        k->setName("k1");
        k->setDefaultValue(0.0);
        page->addKnob(k);
        _imp->k1 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("K2"));
        k->setName("k2");
        k->setDefaultValue(0.0);
        page->addKnob(k);
        _imp->k2 = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("K3"));
        k->setName("k3");
        k->setDefaultValue(0.0);
        page->addKnob(k);
        _imp->k3 = k;
    }
}

StatusEnum
LensWarp::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const RectI srcBounds = srcImg->getBounds();
    const RectI outBounds = outImg->getBounds();
    const int srcW = srcBounds.width(), srcH = srcBounds.height();
    if (srcW <= 0 || srcH <= 0) return eStatusOK;
    const int nComp = std::min(std::min((int)srcImg->getComponentsCount(),
                                        (int)outImg->getComponentsCount()), 4);

    // Lens model — the SAME object the solve uses, in the source image's
    // pixel space at the current render scale.
    const double focalMm = _imp->focalMm.lock()->getValue();
    const double sensorW = _imp->sensorWidthMm.lock()->getValue();
    const double fpx = (sensorW > 0.0) ? (focalMm / sensorW) * (double)srcW : (double)srcW;
    double ppx = _imp->principalPoint.lock()->getValue(0);
    double ppy = _imp->principalPoint.lock()->getValue(1);
    const double scale = args.mappedScale.toOfxPointD().x;
    if (std::abs(ppx) < 1e-9 && std::abs(ppy) < 1e-9) {
        ppx = srcW / 2.0;
        ppy = srcH / 2.0;
    } else {
        ppx *= scale;
        // knob is in Natron bottom-up full-res pixels; model works top-down
        ppy = srcH - 1.0 - ppy * scale;
    }

    libmv::PolynomialCameraIntrinsics intrinsics;
    intrinsics.SetImageSize(srcW, srcH);
    intrinsics.SetFocalLength(fpx, fpx);
    intrinsics.SetPrincipalPoint(ppx, ppy);
    intrinsics.SetRadialDistortion(_imp->k1.lock()->getValue(),
                                   _imp->k2.lock()->getValue(),
                                   _imp->k3.lock()->getValue());

    const bool undistort = (_imp->direction.lock()->getValue() == 0);

    Image::ReadAccess ra(srcImg.get());
    Image::WriteAccess wa(outImg.get());
    static const float zero[4] = {0, 0, 0, 0};

    auto safePixel = [&](int cx, int cy) -> const float* {
        if (cx < srcBounds.x1 || cx >= srcBounds.x2 ||
            cy < srcBounds.y1 || cy >= srcBounds.y2) {
            return zero;
        }
        const float* p = (const float*)ra.pixelAt(cx, cy);
        return p ? p : zero;
    };

    // The model's y axis is top-down (same convention as the solve, which
    // feeds libmv mvY = (rod.y2-1) - natronY); Natron images are bottom-up.
    const double yTop = (double)srcBounds.y2 - 1.0;

    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            const double px = (double)x;
            const double pyTopDown = yTop - (double)y;

            double sx, syTopDown;
            if (undistort) {
                // Output = pinhole image. Where in the DISTORTED source does
                // this pinhole pixel live? Normalize by K, then apply the
                // forward distortion — closed form.
                const double nx = (px - ppx) / fpx;
                const double ny = (pyTopDown - ppy) / fpx;
                intrinsics.ApplyIntrinsics(nx, ny, &sx, &syTopDown);
            } else {
                // Output = distorted image. Which pinhole pixel lands here?
                // Invert the distortion (iterative), then project through K.
                double nx, ny;
                intrinsics.InvertIntrinsics(px, pyTopDown, &nx, &ny);
                sx = ppx + nx * fpx;
                syTopDown = ppy + ny * fpx;
            }
            const double srcFx = sx;
            const double srcFy = yTop - syTopDown;

            // Bilinear sample.
            const double bx = srcFx - 0.5, by = srcFy - 0.5;
            const int ix = (int)std::floor(bx), iy = (int)std::floor(by);
            const double fx = bx - ix, fy = by - iy;
            const float* p00 = safePixel(ix, iy);
            const float* p10 = safePixel(ix + 1, iy);
            const float* p01 = safePixel(ix, iy + 1);
            const float* p11 = safePixel(ix + 1, iy + 1);
            for (int c = 0; c < nComp; ++c) {
                const double top = p01[c] * (1.0 - fx) + p11[c] * fx;
                const double bot = p00[c] * (1.0 - fx) + p10[c] * fx;
                dst[c] = (float)(bot * (1.0 - fy) + top * fy);
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT

#include "moc_LensWarp.cpp"
