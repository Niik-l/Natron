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
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DeepFromFrames.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepFromFramesPrivate
{
    KnobDoubleWPtr depth;
    KnobDoubleWPtr depthBack;
    KnobStringWPtr info;
};


DeepFromFrames::DeepFromFrames(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepFromFramesPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepFromFrames::~DeepFromFrames()
{
}

std::string
DeepFromFrames::getPluginDescription() const
{
    return tr("Convert a flat RGBA image into deep samples at a specified depth. "
              "Connect multiple DeepFromFrames to DeepMerge to build a deep image "
              "from separate 2D renders at different depths.").toStdString();
}

void
DeepFromFrames::addAcceptedComponents(int /*inputNb*/,
                                      std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepFromFrames::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepFromFrames::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                               bool* /*defaultG*/,
                                               bool* /*defaultB*/,
                                               bool* /*defaultA*/) const
{
    return false;
}

void
DeepFromFrames::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr depth = AppManager::createKnob<KnobDouble>(this, tr("Depth"));
    depth->setName("depth");
    depth->setHintToolTip(tr("Z depth value for the front of this layer's deep samples."));
    depth->setAnimationEnabled(true);
    depth->setDefaultValue(1.0);
    depth->setMinimum(0.0);
    depth->setDisplayMinimum(0.0);
    depth->setDisplayMaximum(100.0);
    page->addKnob(depth);
    _imp->depth = depth;

    KnobDoublePtr depthBack = AppManager::createKnob<KnobDouble>(this, tr("Depth Back"));
    depthBack->setName("depthBack");
    depthBack->setHintToolTip(tr("Z depth value for the back of this layer's deep samples. "
                                 "Set to 0 to use the same value as Depth (zero-thickness samples)."));
    depthBack->setAnimationEnabled(true);
    depthBack->setDefaultValue(0.0);
    depthBack->setMinimum(0.0);
    depthBack->setDisplayMinimum(0.0);
    depthBack->setDisplayMaximum(100.0);
    page->addKnob(depthBack);
    _imp->depthBack = depthBack;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set depth to convert a flat image into deep samples.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepFromFrames::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if (_imp->depth.lock().get() == k ||
        _imp->depthBack.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepFromFrames::getRegionOfDefinition(U64 /*hash*/,
                                      double time,
                                      const RenderScale& scale,
                                      ViewIdx view,
                                      RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

DeepImagePtr
DeepFromFrames::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepFromFrames::render(const RenderActionArgs& args)
{
    // Get the flat input image
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    EffectInstancePtr flatInput = getInput(0);
    if (!flatInput) {
        return eStatusFailed;
    }

    // Request the input image
    RectI inputRoI;
    if (outImg) {
        inputRoI = outImg->getBounds();
    } else {
        return eStatusFailed;
    }

    // Fetch the input image using standard getImage()
    RectI srcRoI;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoI);
    if (!srcImg) {
        return eStatusFailed;
    }

    double depthVal = _imp->depth.lock()->getValue();
    double depthBackVal = _imp->depthBack.lock()->getValue();

    // If depthBack is 0, use same as depth (zero-thickness)
    if (depthBackVal <= 0.0) {
        depthBackVal = depthVal;
    }

    RectI srcBounds = srcImg->getBounds();
    int w = srcBounds.width();
    int h = srcBounds.height();

    // Deep channels: R, G, B, A, Z, ZBack
    int nChannels = 6;
    std::vector<std::string> chanNames;
    chanNames.push_back("R");
    chanNames.push_back("G");
    chanNames.push_back("B");
    chanNames.push_back("A");
    chanNames.push_back("Z");
    chanNames.push_back("ZBack");

    RectI dw;
    dw.x1 = srcBounds.x1;
    dw.y1 = srcBounds.y1;
    dw.x2 = srcBounds.x2;
    dw.y2 = srcBounds.y2;

    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    // First pass: count samples (1 per pixel with A > 0, 0 otherwise)
    Image::ReadAccess ra(srcImg.get());

    for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
        for (int x = srcBounds.x1; x < srcBounds.x2; ++x) {
            const float* pix = (const float*)ra.pixelAt(x, y);
            if (pix) {
                float a = pix[3]; // Alpha
                result->setSampleCount(x, y, (a > 0.0f) ? 1 : 0);
            } else {
                result->setSampleCount(x, y, 0);
            }
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: fill sample data
    for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
        for (int x = srcBounds.x1; x < srcBounds.x2; ++x) {
            int nSamples = result->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* pix = (const float*)ra.pixelAt(x, y);
            if (!pix) continue;

            float* dstData = result->getSampleData(x, y);
            if (!dstData) continue;

            dstData[0] = pix[0]; // R
            dstData[1] = pix[1]; // G
            dstData[2] = pix[2]; // B
            dstData[3] = pix[3]; // A
            dstData[4] = (float)depthVal;     // Z
            dstData[5] = (float)depthBackVal; // ZBack
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Depth: " << depthVal
       << " | DepthBack: " << depthBackVal
       << " | Image: " << w << "x" << h
       << " | Total samples: " << result->totalSamples();
    KnobStringPtr infoKnob = _imp->info.lock();
    if (infoKnob) {
        infoKnob->setValue(ss.str());
    }

    _lastDeepImage = result;

    // Produce flattened preview
    if (outImg) {
        result->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepFromFrames.cpp"
