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

#include "DeepChannelMath.h"

#include <cassert>
#include <limits>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "../../ChoiceOption.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepChannelMathPrivate
{
    KnobChoiceWPtr operation;
    KnobChoiceWPtr targetChannel;
    KnobDoubleWPtr value;
    KnobStringWPtr info;
};


DeepChannelMath::DeepChannelMath(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepChannelMathPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepChannelMath::~DeepChannelMath()
{
}

std::string
DeepChannelMath::getPluginDescription() const
{
    return tr("Perform per-sample math operations on deep image channels. "
              "Multiply, add, or set channels to constant values.").toStdString();
}

void
DeepChannelMath::addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepChannelMath::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepChannelMath::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                                bool* /*defaultG*/,
                                                bool* /*defaultB*/,
                                                bool* /*defaultA*/) const
{
    return false;
}

void
DeepChannelMath::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobChoicePtr operation = AppManager::createKnob<KnobChoice>(this, tr("Operation"));
    operation->setName("operation");
    operation->setHintToolTip(tr("Math operation to apply to the target channel:\n"
                                 "- Multiply: channel = channel * value\n"
                                 "- Add: channel = channel + value\n"
                                 "- Set: channel = value"));
    operation->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Multiply", "Multiply", ""));
        entries.push_back(ChoiceOption("Add", "Add", ""));
        entries.push_back(ChoiceOption("Set", "Set", ""));
        operation->populateChoices(entries);
    }
    operation->setDefaultValue(0);
    page->addKnob(operation);
    _imp->operation = operation;

    KnobChoicePtr targetChannel = AppManager::createKnob<KnobChoice>(this, tr("Target Channel"));
    targetChannel->setName("targetChannel");
    targetChannel->setHintToolTip(tr("Which channel to apply the operation to."));
    targetChannel->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("R", "R", ""));
        entries.push_back(ChoiceOption("G", "G", ""));
        entries.push_back(ChoiceOption("B", "B", ""));
        entries.push_back(ChoiceOption("A", "A", ""));
        targetChannel->populateChoices(entries);
    }
    targetChannel->setDefaultValue(0);
    page->addKnob(targetChannel);
    _imp->targetChannel = targetChannel;

    KnobDoublePtr value = AppManager::createKnob<KnobDouble>(this, tr("Value"));
    value->setName("value");
    value->setHintToolTip(tr("Value to use in the math operation."));
    value->setAnimationEnabled(true);
    value->setDefaultValue(1.0);
    page->addKnob(value);
    _imp->value = value;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Per-sample channel math: multiply, add, or set.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepChannelMath::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    if (_imp->operation.lock().get() == k ||
        _imp->targetChannel.lock().get() == k ||
        _imp->value.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepChannelMath::getRegionOfDefinition(U64 /*hash*/,
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
DeepChannelMath::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepChannelMath::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    int opVal = _imp->operation.lock()->getValue();
    int targetVal = _imp->targetChannel.lock()->getValue();
    double mathValue = _imp->value.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    // Map target choice to channel name
    const char* targetNames[] = {"R", "G", "B", "A"};
    const char* targetName = targetNames[std::max(0, std::min(3, targetVal))];
    int targetIdx = srcDeep->findChannelIndex(targetName);

    // First pass: same sample counts
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            result->setSampleCount(x, y, nSamples);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy all samples and apply operation
    int samplesModified = 0;

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

                // Apply operation to target channel
                if (targetIdx >= 0) {
                    if (opVal == 0) {
                        // Multiply
                        dst[targetIdx] *= (float)mathValue;
                    } else if (opVal == 1) {
                        // Add
                        dst[targetIdx] += (float)mathValue;
                    } else if (opVal == 2) {
                        // Set
                        dst[targetIdx] = (float)mathValue;
                    }
                    ++samplesModified;
                }
            }
        }
    }

    // Update info
    const char* opNames[] = {"Multiply", "Add", "Set"};
    std::ostringstream ss;
    ss << "Op: " << opNames[std::max(0, std::min(2, opVal))]
       << " | Channel: " << targetName
       << " | Value: " << mathValue
       << " | Samples modified: " << samplesModified;
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

#include "moc_DeepChannelMath.cpp"
