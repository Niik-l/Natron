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

#include "DeepReformat.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../ChoiceOption.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepReformatPrivate
{
    KnobIntWPtr outputWidth;
    KnobIntWPtr outputHeight;
    KnobChoiceWPtr format;
    KnobChoiceWPtr resizeType;
    KnobStringWPtr info;
};


DeepReformat::DeepReformat(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepReformatPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepReformat::~DeepReformat()
{
}

std::string
DeepReformat::getPluginDescription() const
{
    return tr("Resize a deep image to a new resolution using nearest-neighbor sampling. "
              "Each output pixel maps to the nearest input pixel — deep samples are copied "
              "without interpolation.\n\n"
              "Use this to match deep image resolution to a different format (e.g. when the "
              "beauty render and deep render have different resolutions).").toStdString();
}

void
DeepReformat::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepReformat::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepReformat::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepReformat::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobChoicePtr format = AppManager::createKnob<KnobChoice>(this, tr("Format"));
    format->setName("format");
    format->setHintToolTip(tr("Select a preset format, or choose 'Custom' to set Width/Height manually."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Custom", "", "Use Width/Height values below"));
        entries.push_back(ChoiceOption("PC_Video 640x480", "", "640x480"));
        entries.push_back(ChoiceOption("NTSC 720x486 0.91", "", "720x486"));
        entries.push_back(ChoiceOption("PAL 720x576 1.09", "", "720x576"));
        entries.push_back(ChoiceOption("NTSC_16:9 720x486 1.21", "", "720x486 wide"));
        entries.push_back(ChoiceOption("PAL_16:9 720x576 1.46", "", "720x576 wide"));
        entries.push_back(ChoiceOption("HD_720 1280x720", "", "1280x720"));
        entries.push_back(ChoiceOption("HD 1920x1080", "", "1920x1080"));
        entries.push_back(ChoiceOption("UHD_4K 3840x2160", "", "3840x2160"));
        entries.push_back(ChoiceOption("1K_Super_35(full-ap) 1024x778", "", "1024x778"));
        entries.push_back(ChoiceOption("1K_Cinemascope 914x778 2.00", "", "914x778"));
        entries.push_back(ChoiceOption("2K_Super_35(full-ap) 2048x1556", "", "2048x1556"));
        entries.push_back(ChoiceOption("2K_Cinemascope 1828x1556 2.00", "", "1828x1556"));
        entries.push_back(ChoiceOption("2K_DCP 2048x1080", "", "2048x1080"));
        entries.push_back(ChoiceOption("4K_Super_35(full-ap) 4096x3112", "", "4096x3112"));
        entries.push_back(ChoiceOption("4K_Cinemascope 3656x3112 2.00", "", "3656x3112"));
        entries.push_back(ChoiceOption("4K_DCP 4096x2160", "", "4096x2160"));
        entries.push_back(ChoiceOption("square_256 256x256", "", "256x256"));
        entries.push_back(ChoiceOption("square_512 512x512", "", "512x512"));
        entries.push_back(ChoiceOption("square_1K 1024x1024", "", "1024x1024"));
        entries.push_back(ChoiceOption("square_2K 2048x2048", "", "2048x2048"));
        entries.push_back(ChoiceOption("Half", "", "Half the input resolution"));
        entries.push_back(ChoiceOption("Quarter", "", "Quarter the input resolution"));
        format->populateChoices(entries);
    }
    format->setDefaultValue(0);
    page->addKnob(format);
    _imp->format = format;

    KnobIntPtr width = AppManager::createKnob<KnobInt>(this, tr("Width"));
    width->setName("outputWidth");
    width->setHintToolTip(tr("Output width in pixels. Set to 0 to use the input width."));
    width->setAnimationEnabled(false);
    width->setDefaultValue(0);
    width->setMinimum(0);
    width->setDisplayMinimum(0);
    width->setDisplayMaximum(8192);
    page->addKnob(width);
    _imp->outputWidth = width;

    KnobIntPtr height = AppManager::createKnob<KnobInt>(this, tr("Height"));
    height->setName("outputHeight");
    height->setHintToolTip(tr("Output height in pixels. Set to 0 to use the input height."));
    height->setAnimationEnabled(false);
    height->setDefaultValue(0);
    height->setMinimum(0);
    height->setDisplayMinimum(0);
    height->setDisplayMaximum(8192);
    page->addKnob(height);
    _imp->outputHeight = height;

    KnobChoicePtr resizeType = AppManager::createKnob<KnobChoice>(this, tr("Resize Type"));
    resizeType->setName("resizeType");
    resizeType->setHintToolTip(tr("How to handle the resize.\n"
                                   "Scale: Scale to fit the new resolution.\n"
                                   "Crop: Crop/pad to the new resolution without scaling."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Scale", "", "Scale to fit the new resolution"));
        entries.push_back(ChoiceOption("Crop", "", "Crop/pad without scaling"));
        resizeType->populateChoices(entries);
    }
    resizeType->setDefaultValue(0);
    page->addKnob(resizeType);
    _imp->resizeType = resizeType;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set output Width/Height. 0 = use input size.");
    page->addKnob(info);
    _imp->info = info;
}

// Resolve output dimensions from format preset + custom width/height
static void
resolveOutputSize(int formatChoice, int customW, int customH, int srcW, int srcH,
                  int& outW, int& outH)
{
    switch (formatChoice) {
        case 1:  outW = 640;  outH = 480;  break;  // PC_Video
        case 2:  outW = 720;  outH = 486;  break;  // NTSC
        case 3:  outW = 720;  outH = 576;  break;  // PAL
        case 4:  outW = 720;  outH = 486;  break;  // NTSC 16:9
        case 5:  outW = 720;  outH = 576;  break;  // PAL 16:9
        case 6:  outW = 1280; outH = 720;  break;  // HD 720
        case 7:  outW = 1920; outH = 1080; break;  // HD 1080
        case 8:  outW = 3840; outH = 2160; break;  // UHD 4K
        case 9:  outW = 1024; outH = 778;  break;  // 1K Super35
        case 10: outW = 914;  outH = 778;  break;  // 1K Cinemascope
        case 11: outW = 2048; outH = 1556; break;  // 2K Super35
        case 12: outW = 1828; outH = 1556; break;  // 2K Cinemascope
        case 13: outW = 2048; outH = 1080; break;  // 2K DCP
        case 14: outW = 4096; outH = 3112; break;  // 4K Super35
        case 15: outW = 3656; outH = 3112; break;  // 4K Cinemascope
        case 16: outW = 4096; outH = 2160; break;  // 4K DCP
        case 17: outW = 256;  outH = 256;  break;  // square 256
        case 18: outW = 512;  outH = 512;  break;  // square 512
        case 19: outW = 1024; outH = 1024; break;  // square 1K
        case 20: outW = 2048; outH = 2048; break;  // square 2K
        case 21: outW = srcW / 2; outH = srcH / 2; break;  // Half
        case 22: outW = srcW / 4; outH = srcH / 4; break;  // Quarter
        default: // Custom
            outW = (customW > 0) ? customW : srcW;
            outH = (customH > 0) ? customH : srcH;
            break;
    }
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
}

StatusEnum
DeepReformat::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                    ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;

    // Get upstream RoD from render pipeline, not side-channel
    RectD upstreamRoD;
    bool isProjectFormat = false;
    StatusEnum status = input->getRegionOfDefinition_public(
        input->getHash(), time, scale, view, &upstreamRoD, &isProjectFormat);
    if (status != eStatusOK) return status;

    int srcW = (int)(upstreamRoD.x2 - upstreamRoD.x1);
    int srcH = (int)(upstreamRoD.y2 - upstreamRoD.y1);

    int formatChoice = _imp->format.lock()->getValue();
    int customW = _imp->outputWidth.lock()->getValue();
    int customH = _imp->outputHeight.lock()->getValue();

    int outW, outH;
    resolveOutputSize(formatChoice, customW, customH, srcW, srcH, outW, outH);

    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = outW;
    rod->y2 = outH;

    return eStatusOK;
}

DeepImagePtr
DeepReformat::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepReformat::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    const RectI& srcDW = srcDeep->getDataWindow();
    int srcW = srcDW.width();
    int srcH = srcDW.height();

    int formatChoice = _imp->format.lock()->getValue();
    int customW = _imp->outputWidth.lock()->getValue();
    int customH = _imp->outputHeight.lock()->getValue();
    int resizeType = _imp->resizeType.lock()->getValue();

    int outW, outH;
    resolveOutputSize(formatChoice, customW, customH, srcW, srcH, outW, outH);

    if (outW <= 0) outW = srcW;
    if (outH <= 0) outH = srcH;

    // If same size AND origin is already (0,0), pass through
    if (outW == srcW && outH == srcH && srcDW.x1 == 0 && srcDW.y1 == 0) {
        _lastDeepImage = srcDeep;
    } else {
        int nChannels = srcDeep->getNumChannels();
        const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

        RectI outDW;
        outDW.x1 = 0;
        outDW.y1 = 0;
        outDW.x2 = outW;
        outDW.y2 = outH;

        DeepImagePtr result = std::make_shared<DeepImage>(outDW, nChannels, chanNames);

        if (resizeType == 0) {
            // Scale mode: nearest-neighbor sampling
            float scaleX = (float)srcW / (float)outW;
            float scaleY = (float)srcH / (float)outH;

            // First pass: set sample counts
            for (int y = 0; y < outH; ++y) {
                int srcY = srcDW.y1 + std::min((int)(y * scaleY), srcH - 1);
                for (int x = 0; x < outW; ++x) {
                    int srcX = srcDW.x1 + std::min((int)(x * scaleX), srcW - 1);
                    result->setSampleCount(x, y, srcDeep->getSampleCount(srcX, srcY));
                }
            }

            result->allocateFromSampleCounts();

            // Second pass: copy sample data
            for (int y = 0; y < outH; ++y) {
                int srcY = srcDW.y1 + std::min((int)(y * scaleY), srcH - 1);
                for (int x = 0; x < outW; ++x) {
                    int srcX = srcDW.x1 + std::min((int)(x * scaleX), srcW - 1);
                    int nSamples = srcDeep->getSampleCount(srcX, srcY);
                    if (nSamples == 0) continue;

                    const float* srcData = srcDeep->getSampleData(srcX, srcY);
                    float* dstData = result->getSampleData(x, y);
                    for (int i = 0; i < nSamples * nChannels; ++i) {
                        dstData[i] = srcData[i];
                    }
                }
            }
        } else {
            // Crop mode: copy pixels that overlap, zero the rest
            for (int y = 0; y < outH; ++y) {
                for (int x = 0; x < outW; ++x) {
                    int srcX = srcDW.x1 + x;
                    int srcY = srcDW.y1 + y;
                    if (srcX >= srcDW.x1 && srcX < srcDW.x2 &&
                        srcY >= srcDW.y1 && srcY < srcDW.y2) {
                        result->setSampleCount(x, y, srcDeep->getSampleCount(srcX, srcY));
                    } else {
                        result->setSampleCount(x, y, 0);
                    }
                }
            }

            result->allocateFromSampleCounts();

            for (int y = 0; y < outH; ++y) {
                for (int x = 0; x < outW; ++x) {
                    int srcX = srcDW.x1 + x;
                    int srcY = srcDW.y1 + y;
                    int nSamples = result->getSampleCount(x, y);
                    if (nSamples == 0) continue;

                    const float* srcData = srcDeep->getSampleData(srcX, srcY);
                    float* dstData = result->getSampleData(x, y);
                    for (int i = 0; i < nSamples * nChannels; ++i) {
                        dstData[i] = srcData[i];
                    }
                }
            }
        }

        // Update info
        std::ostringstream ss;
        ss << "Input: " << srcW << "x" << srcH << " -> Output: " << outW << "x" << outH;
        if (resizeType == 0) ss << " (Scale)";
        else ss << " (Crop)";
        KnobStringPtr infoKnob = _imp->info.lock();
        if (infoKnob) infoKnob->setValue(ss.str());

        _lastDeepImage = result;
    }

    // Flattened preview
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (outImg && _lastDeepImage) {
        const RectI& deepDW = _lastDeepImage->getDataWindow();
        _lastDeepImage->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepReformat.cpp"
