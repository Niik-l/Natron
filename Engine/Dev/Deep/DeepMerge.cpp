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

#include "DeepMerge.h"

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

struct DeepMergePrivate
{
    DeepImagePtr mergedDeepImage;
    mutable QMutex deepImageMutex;

    DeepMergePrivate()
        : mergedDeepImage()
        , deepImageMutex()
    {
    }
};

DeepMerge::DeepMerge(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepMergePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepMerge::~DeepMerge()
{
}

std::string
DeepMerge::getPluginDescription() const
{
    return tr("Merge two deep images by interleaving their samples sorted by depth (Z). "
              "The result is a deep image containing all samples from both inputs, "
              "properly ordered front-to-back. This is the deep equivalent of a compositing operation. "
              "Connect two DeepRead nodes (or other deep sources) to inputs A and B.").toStdString();
}

void
DeepMerge::addAcceptedComponents(int /*inputNb*/,
                                 std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepMerge::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepMerge::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                          bool* /*defaultG*/,
                                          bool* /*defaultB*/,
                                          bool* /*defaultA*/) const
{
    return false;
}

DeepImagePtr
DeepMerge::getDeepImage() const
{
    QMutexLocker lock(&_imp->deepImageMutex);
    return _imp->mergedDeepImage;
}

void
DeepMerge::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect deep sources to inputs A and B.");
    page->addKnob(info);
}

StatusEnum
DeepMerge::getRegionOfDefinition(U64 /*hash*/,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    // Union of both inputs' RoDs
    RectD rodA, rodB;
    bool hasA = false, hasB = false;

    EffectInstancePtr inputA = getInput(0);
    EffectInstancePtr inputB = getInput(1);

    if (inputA) {
        bool isProjectFormatA = false;
        if (inputA->getRegionOfDefinition_public(inputA->getHash(), time, scale, view, &rodA, &isProjectFormatA) == eStatusOK) {
            hasA = true;
        }
    }
    if (inputB) {
        bool isProjectFormatB = false;
        if (inputB->getRegionOfDefinition_public(inputB->getHash(), time, scale, view, &rodB, &isProjectFormatB) == eStatusOK) {
            hasB = true;
        }
    }

    if (hasA && hasB) {
        rod->merge(rodA);
        rod->merge(rodB);
    } else if (hasA) {
        *rod = rodA;
    } else if (hasB) {
        *rod = rodB;
    } else {
        return eStatusFailed;
    }

    return eStatusOK;
}

StatusEnum
DeepMerge::render(const RenderActionArgs& args)
{
    EffectInstancePtr inputA = getInput(0);
    EffectInstancePtr inputB = getInput(1);

    DeepImagePtr deepA = getDeepImageFromEffect(inputA.get());
    DeepImagePtr deepB = getDeepImageFromEffect(inputB.get());

    // If neither input has deep data, fail
    if (!deepA && !deepB) {
        return eStatusFailed;
    }

    // If only one input, pass it through
    if (!deepA) {
        QMutexLocker lock(&_imp->deepImageMutex);
        _imp->mergedDeepImage = deepB;
    } else if (!deepB) {
        QMutexLocker lock(&_imp->deepImageMutex);
        _imp->mergedDeepImage = deepA;
    } else {
        // Merge both deep images by interleaving samples sorted by Z
        RectI windowA = deepA->getDataWindow();
        RectI windowB = deepB->getDataWindow();

        // Compute the union data window
        RectI mergedWindow;
        mergedWindow.x1 = std::min(windowA.x1, windowB.x1);
        mergedWindow.y1 = std::min(windowA.y1, windowB.y1);
        mergedWindow.x2 = std::max(windowA.x2, windowB.x2);
        mergedWindow.y2 = std::max(windowA.y2, windowB.y2);

        // Use the channel layout from input A (both must match for proper merge)
        const std::vector<std::string>& chNames = deepA->getChannelNames();
        int numCh = deepA->getNumChannels();

        // Find Z channel index for depth sorting
        int zIdx = deepA->findChannelIndex("Z");

        DeepImagePtr merged = std::make_shared<DeepImage>(mergedWindow, numCh, chNames);

        // First pass: set sample counts (sum of both inputs)
        for (int y = mergedWindow.y1; y < mergedWindow.y2; ++y) {
            for (int x = mergedWindow.x1; x < mergedWindow.x2; ++x) {
                int countA = deepA->getSampleCount(x, y);
                int countB = deepB->getSampleCount(x, y);
                merged->setSampleCount(x, y, countA + countB);
            }
        }

        merged->allocateFromSampleCounts();

        // Second pass: merge-sort samples by Z depth
        for (int y = mergedWindow.y1; y < mergedWindow.y2; ++y) {
            for (int x = mergedWindow.x1; x < mergedWindow.x2; ++x) {
                int countA = deepA->getSampleCount(x, y);
                int countB = deepB->getSampleCount(x, y);
                int totalCount = countA + countB;

                if (totalCount == 0) {
                    continue;
                }

                float* dest = merged->getSampleData(x, y);
                const float* srcA = deepA->getSampleData(x, y);
                const float* srcB = deepB->getSampleData(x, y);

                if (zIdx >= 0 && srcA && srcB) {
                    // Merge-sort by Z
                    int iA = 0, iB = 0, iOut = 0;
                    while (iA < countA && iB < countB) {
                        float zA = srcA[iA * numCh + zIdx];
                        float zB = srcB[iB * numCh + zIdx];

                        if (zA <= zB) {
                            std::copy(srcA + iA * numCh,
                                      srcA + (iA + 1) * numCh,
                                      dest + iOut * numCh);
                            ++iA;
                        } else {
                            std::copy(srcB + iB * numCh,
                                      srcB + (iB + 1) * numCh,
                                      dest + iOut * numCh);
                            ++iB;
                        }
                        ++iOut;
                    }
                    // Copy remaining from A
                    while (iA < countA) {
                        std::copy(srcA + iA * numCh,
                                  srcA + (iA + 1) * numCh,
                                  dest + iOut * numCh);
                        ++iA;
                        ++iOut;
                    }
                    // Copy remaining from B
                    while (iB < countB) {
                        std::copy(srcB + iB * numCh,
                                  srcB + (iB + 1) * numCh,
                                  dest + iOut * numCh);
                        ++iB;
                        ++iOut;
                    }
                } else {
                    // No Z channel or missing data — just concatenate
                    int offset = 0;
                    if (srcA) {
                        std::copy(srcA, srcA + countA * numCh, dest);
                        offset = countA * numCh;
                    }
                    if (srcB) {
                        std::copy(srcB, srcB + countB * numCh, dest + offset);
                    }
                }
            }
        }

        {
            QMutexLocker lock(&_imp->deepImageMutex);
            _imp->mergedDeepImage = merged;
        }
    }

    // Also produce a flattened RGBA preview for the Viewer
    DeepImagePtr resultDeep;
    {
        QMutexLocker lock(&_imp->deepImageMutex);
        resultDeep = _imp->mergedDeepImage;
    }

    if (resultDeep && !args.outputPlanes.empty()) {
        const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
        ImagePtr outImg = output.second;

        if (outImg) {
            resultDeep->flattenToImage(outImg.get());
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepMerge.cpp"
