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

#include "DeepDepthWarp.h"

#include <cassert>
#include <limits>
#include <sstream>
#include <vector>
#include <cmath>
#include <algorithm>

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


struct DeepDepthWarpPrivate
{
    KnobChoiceWPtr mode;
    KnobDoubleWPtr inputNear;
    KnobDoubleWPtr inputFar;
    KnobDoubleWPtr outputNear;
    KnobDoubleWPtr outputFar;
    KnobDoubleWPtr offset;
    KnobDoubleWPtr scale;
    KnobDoubleWPtr power;
    KnobBoolWPtr clampOutput;
    KnobStringWPtr info;
};


DeepDepthWarp::DeepDepthWarp(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepDepthWarpPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepDepthWarp::~DeepDepthWarp()
{
}

std::string
DeepDepthWarp::getPluginDescription() const
{
    return tr("Non-linearly remap Z depth values in a deep image. "
              "Compress, expand, or shift depth ranges with artistic control. "
              "Like a grade node but for the Z channel.").toStdString();
}

void
DeepDepthWarp::addAcceptedComponents(int /*inputNb*/,
                                     std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepDepthWarp::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepDepthWarp::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                              bool* /*defaultG*/,
                                              bool* /*defaultB*/,
                                              bool* /*defaultA*/) const
{
    return false;
}

void
DeepDepthWarp::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobChoicePtr mode = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
    mode->setName("mode");
    mode->setHintToolTip(tr("Depth remapping mode.\n"
                            "Remap Range: Map input depth range to output depth range.\n"
                            "Offset: Add a constant offset to all Z values.\n"
                            "Scale: Multiply all Z values by a scale factor.\n"
                            "Power: Apply a power curve to normalized depth values."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Remap Range", "Remap Range", ""));
        entries.push_back(ChoiceOption("Offset", "Offset", ""));
        entries.push_back(ChoiceOption("Scale", "Scale", ""));
        entries.push_back(ChoiceOption("Power", "Power", ""));
        mode->populateChoices(entries);
    }
    mode->setDefaultValue(0);
    page->addKnob(mode);
    _imp->mode = mode;

    KnobDoublePtr inputNear = AppManager::createKnob<KnobDouble>(this, tr("Input Near"));
    inputNear->setName("inputNear");
    inputNear->setHintToolTip(tr("Input depth range start."));
    inputNear->setAnimationEnabled(true);
    inputNear->setDefaultValue(0.0);
    inputNear->setMinimum(0.0);
    page->addKnob(inputNear);
    _imp->inputNear = inputNear;

    KnobDoublePtr inputFar = AppManager::createKnob<KnobDouble>(this, tr("Input Far"));
    inputFar->setName("inputFar");
    inputFar->setHintToolTip(tr("Input depth range end."));
    inputFar->setAnimationEnabled(true);
    inputFar->setDefaultValue(100.0);
    inputFar->setMinimum(0.0);
    page->addKnob(inputFar);
    _imp->inputFar = inputFar;

    KnobDoublePtr outputNear = AppManager::createKnob<KnobDouble>(this, tr("Output Near"));
    outputNear->setName("outputNear");
    outputNear->setHintToolTip(tr("Output depth range start."));
    outputNear->setAnimationEnabled(true);
    outputNear->setDefaultValue(0.0);
    outputNear->setMinimum(0.0);
    page->addKnob(outputNear);
    _imp->outputNear = outputNear;

    KnobDoublePtr outputFar = AppManager::createKnob<KnobDouble>(this, tr("Output Far"));
    outputFar->setName("outputFar");
    outputFar->setHintToolTip(tr("Output depth range end."));
    outputFar->setAnimationEnabled(true);
    outputFar->setDefaultValue(100.0);
    outputFar->setMinimum(0.0);
    page->addKnob(outputFar);
    _imp->outputFar = outputFar;

    KnobDoublePtr offset = AppManager::createKnob<KnobDouble>(this, tr("Offset"));
    offset->setName("offset");
    offset->setHintToolTip(tr("Constant depth offset added to all Z values (Offset mode)."));
    offset->setAnimationEnabled(true);
    offset->setDefaultValue(0.0);
    page->addKnob(offset);
    _imp->offset = offset;

    KnobDoublePtr scale = AppManager::createKnob<KnobDouble>(this, tr("Scale"));
    scale->setName("scale");
    scale->setHintToolTip(tr("Scale factor applied to all Z values (Scale mode)."));
    scale->setAnimationEnabled(true);
    scale->setDefaultValue(1.0);
    scale->setMinimum(0.001);
    page->addKnob(scale);
    _imp->scale = scale;

    KnobDoublePtr power = AppManager::createKnob<KnobDouble>(this, tr("Power"));
    power->setName("power");
    power->setHintToolTip(tr("Power curve exponent applied to normalized depth (Power mode). "
                             "Values less than 1 compress near depths, greater than 1 compress far depths."));
    power->setAnimationEnabled(true);
    power->setDefaultValue(1.0);
    power->setMinimum(0.1);
    power->setDisplayMinimum(0.1);
    power->setDisplayMaximum(10.0);
    page->addKnob(power);
    _imp->power = power;

    KnobBoolPtr clampOutput = AppManager::createKnob<KnobBool>(this, tr("Clamp Output"));
    clampOutput->setName("clampOutput");
    clampOutput->setHintToolTip(tr("Clamp output Z values to the range [Output Near, Output Far]."));
    clampOutput->setAnimationEnabled(false);
    clampOutput->setDefaultValue(false);
    page->addKnob(clampOutput);
    _imp->clampOutput = clampOutput;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Select a mode to remap depth values.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepDepthWarp::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec /*view*/,
                            double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    if (_imp->mode.lock().get() == k ||
        _imp->inputNear.lock().get() == k ||
        _imp->inputFar.lock().get() == k ||
        _imp->outputNear.lock().get() == k ||
        _imp->outputFar.lock().get() == k ||
        _imp->offset.lock().get() == k ||
        _imp->scale.lock().get() == k ||
        _imp->power.lock().get() == k ||
        _imp->clampOutput.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepDepthWarp::getRegionOfDefinition(U64 /*hash*/,
                                     double /*time*/,
                                     const RenderScale& /*scale*/,
                                     ViewIdx /*view*/,
                                     RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), 0, RenderScale(), ViewIdx(0), rod, &isProjectFormat);
}

DeepImagePtr
DeepDepthWarp::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepDepthWarp::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    int modeVal = _imp->mode.lock()->getValue();
    double inputNearVal = _imp->inputNear.lock()->getValue();
    double inputFarVal = _imp->inputFar.lock()->getValue();
    double outputNearVal = _imp->outputNear.lock()->getValue();
    double outputFarVal = _imp->outputFar.lock()->getValue();
    double offsetVal = _imp->offset.lock()->getValue();
    double scaleVal = _imp->scale.lock()->getValue();
    double powerVal = _imp->power.lock()->getValue();
    bool doClamp = _imp->clampOutput.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    int zBackIdx = srcDeep->findChannelIndex("ZBack");

    if (zIdx < 0) {
        // No Z channel — pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    double inputRange = inputFarVal - inputNearVal;
    double outputRange = outputFarVal - outputNearVal;

    // Lambda to warp a single Z value
    auto warpZ = [&](float z) -> float {
        float out = z;
        switch (modeVal) {
        case 0: // Remap Range
            if (std::fabs(inputRange) > 1e-10) {
                out = (float)(outputNearVal + ((double)z - inputNearVal) / inputRange * outputRange);
            }
            break;
        case 1: // Offset
            out = (float)((double)z + offsetVal);
            break;
        case 2: // Scale
            out = (float)((double)z * scaleVal);
            break;
        case 3: // Power
            if (std::fabs(inputRange) > 1e-10) {
                double normalized = ((double)z - inputNearVal) / inputRange;
                // Clamp normalized to [0,1] before pow to avoid NaN
                normalized = std::max(0.0, std::min(1.0, normalized));
                out = (float)(inputNearVal + std::pow(normalized, powerVal) * inputRange);
            }
            break;
        }
        if (doClamp) {
            float clampMin = (float)std::min(outputNearVal, outputFarVal);
            float clampMax = (float)std::max(outputNearVal, outputFarVal);
            out = std::max(clampMin, std::min(clampMax, out));
        }
        return out;
    };

    // Same sample count as input — no samples added or removed
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    // First pass: set sample counts
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy and warp
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);
            if (!dstData) {
                continue;
            }

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                // Copy all channels
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Warp Z
                dst[zIdx] = warpZ(src[zIdx]);

                // Warp ZBack if present
                if (zBackIdx >= 0) {
                    dst[zBackIdx] = warpZ(src[zBackIdx]);
                }
            }
        }
    }

    // Update info
    std::ostringstream ss;
    const char* modeNames[] = {"Remap Range", "Offset", "Scale", "Power"};
    ss << "Mode: " << modeNames[modeVal]
       << " | Total samples: " << result->totalSamples();
    KnobStringPtr infoKnob = _imp->info.lock();
    if (infoKnob) {
        infoKnob->setValue(ss.str());
    }

    _lastDeepImage = result;

    // Produce flattened preview
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (outImg) {
        result->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepDepthWarp.cpp"
