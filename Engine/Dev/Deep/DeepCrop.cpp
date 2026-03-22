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

#include "DeepCrop.h"

#include <algorithm>
#include <cassert>
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


struct DeepCropPrivate
{
    KnobIntWPtr cropX1, cropY1, cropX2, cropY2;
};


DeepCrop::DeepCrop(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepCropPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepCrop::~DeepCrop()
{
}

std::string
DeepCrop::getPluginDescription() const
{
    return tr("Crop a deep image in XY. Only pixels within the crop region are kept. "
              "Deep samples outside the crop rectangle are discarded.").toStdString();
}

void
DeepCrop::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepCrop::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepCrop::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepCrop::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobIntPtr x1 = AppManager::createKnob<KnobInt>(this, tr("Left"));
    x1->setName("cropX1"); x1->setDefaultValue(0); x1->setAnimationEnabled(true);
    x1->setDisplayMinimum(0); x1->setDisplayMaximum(4096);
    page->addKnob(x1); _imp->cropX1 = x1;

    KnobIntPtr y1 = AppManager::createKnob<KnobInt>(this, tr("Bottom"));
    y1->setName("cropY1"); y1->setDefaultValue(0); y1->setAnimationEnabled(true);
    y1->setDisplayMinimum(0); y1->setDisplayMaximum(4096);
    page->addKnob(y1); _imp->cropY1 = y1;

    KnobIntPtr x2 = AppManager::createKnob<KnobInt>(this, tr("Right"));
    x2->setName("cropX2"); x2->setDefaultValue(0); x2->setAnimationEnabled(true);
    x2->setDisplayMinimum(0); x2->setDisplayMaximum(4096);
    x2->setHintToolTip(tr("Right edge of crop. Set to 0 to use input width."));
    page->addKnob(x2); _imp->cropX2 = x2;

    KnobIntPtr y2 = AppManager::createKnob<KnobInt>(this, tr("Top"));
    y2->setName("cropY2"); y2->setDefaultValue(0); y2->setAnimationEnabled(true);
    y2->setDisplayMinimum(0); y2->setDisplayMaximum(4096);
    y2->setHintToolTip(tr("Top edge of crop. Set to 0 to use input height."));
    page->addKnob(y2); _imp->cropY2 = y2;
}

StatusEnum
DeepCrop::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                ViewIdx /*view*/, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;

    // Get upstream RoD from render pipeline, not side-channel
    RectD upstreamRoD;
    bool isProjectFormat = false;
    StatusEnum status = input->getRegionOfDefinition_public(
        input->getHash(), 0, RenderScale(), ViewIdx(0), &upstreamRoD, &isProjectFormat);
    if (status != eStatusOK) return status;

    int x1 = _imp->cropX1.lock()->getValue();
    int y1 = _imp->cropY1.lock()->getValue();
    int x2 = _imp->cropX2.lock()->getValue();
    int y2 = _imp->cropY2.lock()->getValue();

    if (x2 <= 0) x2 = (int)upstreamRoD.x2;
    if (y2 <= 0) y2 = (int)upstreamRoD.y2;

    rod->x1 = x1;
    rod->y1 = y1;
    rod->x2 = x2;
    rod->y2 = y2;

    return eStatusOK;
}

DeepImagePtr
DeepCrop::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepCrop::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    const RectI& srcDW = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int cx1 = _imp->cropX1.lock()->getValue();
    int cy1 = _imp->cropY1.lock()->getValue();
    int cx2 = _imp->cropX2.lock()->getValue();
    int cy2 = _imp->cropY2.lock()->getValue();

    if (cx2 <= 0) cx2 = srcDW.x2;
    if (cy2 <= 0) cy2 = srcDW.y2;

    RectI cropRect;
    cropRect.x1 = cx1;
    cropRect.y1 = cy1;
    cropRect.x2 = cx2;
    cropRect.y2 = cy2;

    // Intersect crop with source data window
    RectI outDW = cropRect.intersect(srcDW);
    if (outDW.isNull() || outDW.width() <= 0 || outDW.height() <= 0) {
        // Empty result
        RectI emptyDW;
        emptyDW.x1 = cx1; emptyDW.y1 = cy1; emptyDW.x2 = cx1; emptyDW.y2 = cy1;
        _lastDeepImage = std::make_shared<DeepImage>(emptyDW, nChannels, chanNames);
        return eStatusOK;
    }

    DeepImagePtr result = std::make_shared<DeepImage>(outDW, nChannels, chanNames);

    // Copy sample counts
    for (int y = outDW.y1; y < outDW.y2; ++y) {
        for (int x = outDW.x1; x < outDW.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }

    result->allocateFromSampleCounts();

    // Copy sample data
    for (int y = outDW.y1; y < outDW.y2; ++y) {
        for (int x = outDW.x1; x < outDW.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);
            for (int i = 0; i < nSamples * nChannels; ++i) {
                dstData[i] = srcData[i];
            }
        }
    }

    _lastDeepImage = result;

    // Flattened preview
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (outImg && _lastDeepImage) {
        _lastDeepImage->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepCrop.cpp"
