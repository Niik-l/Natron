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

#include "DeepTransform.h"

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
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct DeepTransformPrivate
{
    KnobDoubleWPtr translateX;
    KnobDoubleWPtr translateY;
    KnobDoubleWPtr translateZ;
    KnobDoubleWPtr scaleZ;
};


DeepTransform::DeepTransform(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepTransformPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepTransform::~DeepTransform()
{
}

std::string
DeepTransform::getPluginDescription() const
{
    return tr("Transform deep image data in 3D space.\n\n"
              "Translate X/Y: Shift the image in pixel space (offset applied to pixel coordinates).\n"
              "Translate Z: Shift all depth samples by a constant offset.\n"
              "Scale Z: Multiply all depth values by a scale factor.\n\n"
              "This allows repositioning deep elements without re-rendering.").toStdString();
}

void
DeepTransform::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepTransform::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepTransform::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepTransform::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr tx = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
    tx->setName("translateX");
    tx->setHintToolTip(tr("Shift the deep image horizontally in pixels."));
    tx->setAnimationEnabled(true);
    tx->setDefaultValue(0.0);
    tx->setDisplayMinimum(-1000.0);
    tx->setDisplayMaximum(1000.0);
    page->addKnob(tx);
    _imp->translateX = tx;

    KnobDoublePtr ty = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
    ty->setName("translateY");
    ty->setHintToolTip(tr("Shift the deep image vertically in pixels."));
    ty->setAnimationEnabled(true);
    ty->setDefaultValue(0.0);
    ty->setDisplayMinimum(-1000.0);
    ty->setDisplayMaximum(1000.0);
    page->addKnob(ty);
    _imp->translateY = ty;

    KnobDoublePtr tz = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
    tz->setName("translateZ");
    tz->setHintToolTip(tr("Shift all depth samples by this offset. Positive moves samples further away."));
    tz->setAnimationEnabled(true);
    tz->setDefaultValue(0.0);
    tz->setDisplayMinimum(-100.0);
    tz->setDisplayMaximum(100.0);
    page->addKnob(tz);
    _imp->translateZ = tz;

    KnobDoublePtr sz = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
    sz->setName("scaleZ");
    sz->setHintToolTip(tr("Multiply all depth values by this factor. Values > 1 push samples further away."));
    sz->setAnimationEnabled(true);
    sz->setDefaultValue(1.0);
    sz->setMinimum(0.001);
    sz->setDisplayMinimum(0.1);
    sz->setDisplayMaximum(10.0);
    page->addKnob(sz);
    _imp->scaleZ = sz;
}

StatusEnum
DeepTransform::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                     ViewIdx /*view*/, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;

    // Get upstream RoD and apply XY translation
    RectD upstreamRoD;
    bool isProjectFormat = false;
    StatusEnum st = input->getRegionOfDefinition_public(
        input->getHash(), 0, RenderScale(), ViewIdx(0), &upstreamRoD, &isProjectFormat);
    if (st != eStatusOK) return st;

    double tx = _imp->translateX.lock()->getValue();
    double ty = _imp->translateY.lock()->getValue();

    rod->x1 = upstreamRoD.x1 + tx;
    rod->y1 = upstreamRoD.y1 + ty;
    rod->x2 = upstreamRoD.x2 + tx;
    rod->y2 = upstreamRoD.y2 + ty;

    return eStatusOK;
}

DeepImagePtr
DeepTransform::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepTransform::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    double txVal = _imp->translateX.lock()->getValue();
    double tyVal = _imp->translateY.lock()->getValue();
    double tzVal = _imp->translateZ.lock()->getValue();
    double szVal = _imp->scaleZ.lock()->getValue();

    int tx = (int)std::round(txVal);
    int ty = (int)std::round(tyVal);

    const RectI& srcDW = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    int zBackIdx = srcDeep->findChannelIndex("ZBack");

    // New data window shifted by XY translation
    RectI outDW;
    outDW.x1 = srcDW.x1 + tx;
    outDW.y1 = srcDW.y1 + ty;
    outDW.x2 = srcDW.x2 + tx;
    outDW.y2 = srcDW.y2 + ty;

    DeepImagePtr result = std::make_shared<DeepImage>(outDW, nChannels, chanNames);

    // Copy sample counts (shifted)
    for (int y = srcDW.y1; y < srcDW.y2; ++y) {
        for (int x = srcDW.x1; x < srcDW.x2; ++x) {
            result->setSampleCount(x + tx, y + ty, srcDeep->getSampleCount(x, y));
        }
    }
    result->allocateFromSampleCounts();

    // Copy and transform sample data
    for (int y = srcDW.y1; y < srcDW.y2; ++y) {
        for (int x = srcDW.x1; x < srcDW.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x + tx, y + ty);

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                // Copy all channels
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Transform Z: scale then translate
                if (zIdx >= 0) {
                    dst[zIdx] = src[zIdx] * (float)szVal + (float)tzVal;
                }
                if (zBackIdx >= 0) {
                    dst[zBackIdx] = src[zBackIdx] * (float)szVal + (float)tzVal;
                }
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

#include "moc_DeepTransform.cpp"
