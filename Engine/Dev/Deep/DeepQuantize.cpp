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

#include "DeepQuantize.h"

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


struct DeepQuantizePrivate
{
    KnobDoubleWPtr gridSize;
    KnobDoubleWPtr offset;
    KnobStringWPtr info;
};


DeepQuantize::DeepQuantize(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepQuantizePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepQuantize::~DeepQuantize()
{
}

std::string
DeepQuantize::getPluginDescription() const
{
    return tr("Snap deep sample Z values to a regular depth grid. "
              "Useful for regularizing irregular deep data or reducing depth "
              "precision for optimization.").toStdString();
}

void
DeepQuantize::addAcceptedComponents(int /*inputNb*/,
                                    std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepQuantize::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepQuantize::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                             bool* /*defaultG*/,
                                             bool* /*defaultB*/,
                                             bool* /*defaultA*/) const
{
    return false;
}

void
DeepQuantize::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr gridSize = AppManager::createKnob<KnobDouble>(this, tr("Grid Size"));
    gridSize->setName("gridSize");
    gridSize->setHintToolTip(tr("Size of the depth grid. Z values are snapped to the nearest multiple of this value. "
                                "Smaller values preserve more depth detail."));
    gridSize->setAnimationEnabled(true);
    gridSize->setDefaultValue(1.0);
    gridSize->setMinimum(0.001);
    gridSize->setDisplayMinimum(0.001);
    gridSize->setDisplayMaximum(100.0);
    page->addKnob(gridSize);
    _imp->gridSize = gridSize;

    KnobDoublePtr offset = AppManager::createKnob<KnobDouble>(this, tr("Offset"));
    offset->setName("offset");
    offset->setHintToolTip(tr("Offset applied to the grid. The quantized Z = round((Z - offset) / gridSize) * gridSize + offset."));
    offset->setAnimationEnabled(true);
    offset->setDefaultValue(0.0);
    page->addKnob(offset);
    _imp->offset = offset;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Snap deep Z values to a regular depth grid.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepQuantize::knobChanged(KnobI* k,
                           ValueChangedReasonEnum /*reason*/,
                           ViewSpec /*view*/,
                           double /*time*/,
                           bool /*originatedFromMainThread*/)
{
    if (_imp->gridSize.lock().get() == k || _imp->offset.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepQuantize::getRegionOfDefinition(U64 /*hash*/,
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
DeepQuantize::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepQuantize::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double gridSizeVal = _imp->gridSize.lock()->getValue();
    double offsetVal = _imp->offset.lock()->getValue();

    // Ensure grid size is valid
    if (gridSizeVal < 0.001) {
        gridSizeVal = 0.001;
    }

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    int zBackIdx = srcDeep->findChannelIndex("ZBack");

    // First pass: same sample counts
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            result->setSampleCount(x, y, nSamples);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy all samples and snap Z values
    int samplesQuantized = 0;

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

                // Snap Z to grid: Z = round((Z - offset) / gridSize) * gridSize + offset
                if (zIdx >= 0) {
                    float z = dst[zIdx];
                    float quantized = (float)(std::round((z - offsetVal) / gridSizeVal) * gridSizeVal + offsetVal);
                    dst[zIdx] = quantized;
                    ++samplesQuantized;
                }

                // Snap ZBack to grid if present
                if (zBackIdx >= 0) {
                    float zb = dst[zBackIdx];
                    float quantized = (float)(std::round((zb - offsetVal) / gridSizeVal) * gridSizeVal + offsetVal);
                    dst[zBackIdx] = quantized;
                }
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Grid Size: " << gridSizeVal
       << " | Offset: " << offsetVal
       << " | Samples quantized: " << samplesQuantized
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

#include "moc_DeepQuantize.cpp"
