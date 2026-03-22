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

#include "DeepFlatten.h"

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


DeepFlatten::DeepFlatten(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepFlatten::~DeepFlatten()
{
}

std::string
DeepFlatten::getPluginDescription() const
{
    return tr("Flatten a deep image into a standard flat RGBA image by compositing all depth samples "
              "front-to-back at each pixel. Connect the input to a DeepRead node or another deep-producing node. "
              "The output is a standard flat image that can be used with any regular Natron node.").toStdString();
}

void
DeepFlatten::addAcceptedComponents(int /*inputNb*/,
                                   std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepFlatten::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepFlatten::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                            bool* /*defaultG*/,
                                            bool* /*defaultB*/,
                                            bool* /*defaultA*/) const
{
    return false;
}

void
DeepFlatten::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect a DeepRead node to the input.");
    page->addKnob(info);
}

StatusEnum
DeepFlatten::getRegionOfDefinition(U64 /*hash*/,
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

StatusEnum
DeepFlatten::render(const RenderActionArgs& args)
{
    EffectInstancePtr input = getInput(0);
    if (!input) {
        return eStatusFailed;
    }

    // Try to get the deep image from the input
    DeepImagePtr deepImg = getDeepImageFromEffect(input.get());

    if (!deepImg) {
        // No deep data available — fall back to identity pass-through
        // Just copy the input's flat image
        assert(!args.outputPlanes.empty());
        const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();

        RectI roiPixel;
        ImagePtr srcImg = getImage(0, args.time, args.originalScale, args.view,
                                   NULL, &output.first, false, true,
                                   eStorageModeRAM, 0, &roiPixel);
        if (!srcImg) {
            return eStatusFailed;
        }

        if ((srcImg->getComponents() != output.second->getComponents()) ||
            (srcImg->getBitDepth() != output.second->getBitDepth())) {
            srcImg->convertToFormat(args.roi,
                                    getApp()->getDefaultColorSpaceForBitDepth(srcImg->getBitDepth()),
                                    getApp()->getDefaultColorSpaceForBitDepth(output.second->getBitDepth()),
                                    3, true, false, output.second.get());
        } else {
            output.second->pasteFrom(*srcImg, args.roi,
                                     output.second->usesBitMap() && srcImg->usesBitMap());
        }
        return eStatusOK;
    }

    // We have deep data — flatten it
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (!outImg) {
        return eStatusFailed;
    }

    deepImg->flattenToImage(outImg.get());

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepFlatten.cpp"
