/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
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

#include "DeepFromImage.h"

#include <cassert>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct DeepFromImagePrivate
{
    KnobDoubleWPtr defaultZ;
    KnobDoubleWPtr thickness;
};


DeepFromImage::DeepFromImage(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepFromImagePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepFromImage::~DeepFromImage()
{
}

std::string
DeepFromImage::getPluginDescription() const
{
    return tr("Convert a flat RGBA image into a single-sample-per-pixel deep image. "
              "Each pixel with alpha > 0 becomes one deep sample.\n\n"
              "Input 'RGBA': The flat color image (premultiplied).\n"
              "Input 'Z': Optional Z depth image. If connected, the red channel is used as "
              "the depth value for each sample. If not connected, the 'Default Z' parameter is used.\n\n"
              "'Thickness' controls ZBack - Z for each sample (0 = infinitely thin).").toStdString();
}

void
DeepFromImage::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepFromImage::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepFromImage::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepFromImage::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr defaultZ = AppManager::createKnob<KnobDouble>(this, tr("Default Z"));
    defaultZ->setName("defaultZ");
    defaultZ->setHintToolTip(tr("Z depth for all samples when no Z input is connected."));
    defaultZ->setAnimationEnabled(true);
    defaultZ->setDefaultValue(1.0);
    defaultZ->setMinimum(0.0);
    page->addKnob(defaultZ);
    _imp->defaultZ = defaultZ;

    KnobDoublePtr thickness = AppManager::createKnob<KnobDouble>(this, tr("Thickness"));
    thickness->setName("thickness");
    thickness->setHintToolTip(tr("Depth thickness for each sample (ZBack = Z + Thickness). "
                                  "Set to 0 for infinitely thin samples."));
    thickness->setAnimationEnabled(true);
    thickness->setDefaultValue(0.0);
    thickness->setMinimum(0.0);
    page->addKnob(thickness);
    _imp->thickness = thickness;
}

StatusEnum
DeepFromImage::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                     ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

DeepImagePtr
DeepFromImage::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepFromImage::render(const RenderActionArgs& args)
{
    // Get the flat RGBA image from input 0
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();

    RectI roiPixel;
    ImagePtr rgbaImg = getImage(0, args.time, args.originalScale, args.view,
                                NULL, &output.first, false, true,
                                eStorageModeRAM, 0, &roiPixel);
    if (!rgbaImg) {
        return eStatusFailed;
    }

    // Optionally get Z depth from input 1
    ImagePtr zImg;
    EffectInstancePtr zInput = getInput(1);
    if (zInput) {
        zImg = getImage(1, args.time, args.originalScale, args.view,
                        NULL, &output.first, false, true,
                        eStorageModeRAM, 0, &roiPixel);
    }

    double defaultZVal = _imp->defaultZ.lock()->getValue();
    double thicknessVal = _imp->thickness.lock()->getValue();

    RectI bounds = rgbaImg->getBounds();

    // Create deep image with 6 channels: R, G, B, A, Z, ZBack
    std::vector<std::string> chanNames = {"R", "G", "B", "A", "Z", "ZBack"};
    DeepImagePtr result = std::make_shared<DeepImage>(bounds, 6, chanNames);

    // First pass: set sample counts (1 for non-transparent pixels, 0 otherwise)
    {
        Image::ReadAccess ra(rgbaImg.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                const float* pix = (const float*)ra.pixelAt(x, y);
                if (pix && pix[3] > 0.0001f) {
                    result->setSampleCount(x, y, 1);
                } else {
                    result->setSampleCount(x, y, 0);
                }
            }
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: fill sample data
    {
        Image::ReadAccess ra(rgbaImg.get());
        Image::ReadAccess* zra = nullptr;
        Image::ReadAccess zraObj(zImg ? zImg.get() : rgbaImg.get());
        if (zImg) {
            zra = &zraObj;
        }

        for (int y = bounds.y1; y < bounds.y2; ++y) {
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                if (result->getSampleCount(x, y) == 0) continue;

                const float* pix = (const float*)ra.pixelAt(x, y);
                float* dst = result->getSampleData(x, y);

                // RGBA (already premultiplied)
                dst[0] = pix[0]; // R
                dst[1] = pix[1]; // G
                dst[2] = pix[2]; // B
                dst[3] = pix[3]; // A

                // Z depth
                float z = (float)defaultZVal;
                if (zra) {
                    const float* zPix = (const float*)zra->pixelAt(x, y);
                    if (zPix) {
                        z = zPix[0]; // Use red channel as depth
                    }
                }
                dst[4] = z;                          // Z
                dst[5] = z + (float)thicknessVal;    // ZBack
            }
        }
    }

    _lastDeepImage = result;

    // Copy flat image to output for preview
    ImagePtr outImg = output.second;
    if (outImg) {
        if ((rgbaImg->getComponents() != outImg->getComponents()) ||
            (rgbaImg->getBitDepth() != outImg->getBitDepth())) {
            rgbaImg->convertToFormat(args.roi,
                                     getApp()->getDefaultColorSpaceForBitDepth(rgbaImg->getBitDepth()),
                                     getApp()->getDefaultColorSpaceForBitDepth(outImg->getBitDepth()),
                                     3, true, false, outImg.get());
        } else {
            outImg->pasteFrom(*rgbaImg, args.roi,
                              outImg->usesBitMap() && rgbaImg->usesBitMap());
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepFromImage.cpp"
