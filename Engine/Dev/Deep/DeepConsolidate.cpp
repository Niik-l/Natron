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

#include "DeepConsolidate.h"

#include <cassert>
#include <limits>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepConsolidatePrivate
{
    KnobBoolWPtr removeZeroAlpha;
    KnobDoubleWPtr alphaThreshold;
    KnobBoolWPtr mergeOverlapping;
    KnobDoubleWPtr mergeThreshold;
    KnobBoolWPtr sortByDepth;
    KnobStringWPtr info;
};


DeepConsolidate::DeepConsolidate(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepConsolidatePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepConsolidate::~DeepConsolidate()
{
}

std::string
DeepConsolidate::getPluginDescription() const
{
    return tr("Tidy and optimize deep images by merging overlapping samples, "
              "removing zero-alpha samples, and optionally reducing sample count. "
              "This improves downstream performance and fixes common deep image artifacts.").toStdString();
}

void
DeepConsolidate::addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepConsolidate::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepConsolidate::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                                bool* /*defaultG*/,
                                                bool* /*defaultB*/,
                                                bool* /*defaultA*/) const
{
    return false;
}

void
DeepConsolidate::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobBoolPtr removeZeroAlpha = AppManager::createKnob<KnobBool>(this, tr("Remove Zero Alpha"));
    removeZeroAlpha->setName("removeZeroAlpha");
    removeZeroAlpha->setHintToolTip(tr("Remove samples with alpha less than or equal to the threshold."));
    removeZeroAlpha->setAnimationEnabled(false);
    removeZeroAlpha->setDefaultValue(true);
    page->addKnob(removeZeroAlpha);
    _imp->removeZeroAlpha = removeZeroAlpha;

    KnobDoublePtr alphaThreshold = AppManager::createKnob<KnobDouble>(this, tr("Alpha Threshold"));
    alphaThreshold->setName("alphaThreshold");
    alphaThreshold->setHintToolTip(tr("Samples with alpha at or below this value are considered zero and removed."));
    alphaThreshold->setAnimationEnabled(true);
    alphaThreshold->setDefaultValue(0.0001);
    alphaThreshold->setMinimum(0.0);
    alphaThreshold->setDisplayMinimum(0.0);
    alphaThreshold->setDisplayMaximum(0.1);
    page->addKnob(alphaThreshold);
    _imp->alphaThreshold = alphaThreshold;

    KnobBoolPtr mergeOverlapping = AppManager::createKnob<KnobBool>(this, tr("Merge Overlapping"));
    mergeOverlapping->setName("mergeOverlapping");
    mergeOverlapping->setHintToolTip(tr("Merge samples with nearly identical Z depth values."));
    mergeOverlapping->setAnimationEnabled(false);
    mergeOverlapping->setDefaultValue(true);
    page->addKnob(mergeOverlapping);
    _imp->mergeOverlapping = mergeOverlapping;

    KnobDoublePtr mergeThreshold = AppManager::createKnob<KnobDouble>(this, tr("Merge Threshold"));
    mergeThreshold->setName("mergeThreshold");
    mergeThreshold->setHintToolTip(tr("Z distance within which two samples are considered overlapping and merged."));
    mergeThreshold->setAnimationEnabled(true);
    mergeThreshold->setDefaultValue(0.001);
    mergeThreshold->setMinimum(0.0);
    page->addKnob(mergeThreshold);
    _imp->mergeThreshold = mergeThreshold;

    KnobBoolPtr sortByDepth = AppManager::createKnob<KnobBool>(this, tr("Sort By Depth"));
    sortByDepth->setName("sortByDepth");
    sortByDepth->setHintToolTip(tr("Ensure samples are sorted front-to-back by Z value."));
    sortByDepth->setAnimationEnabled(false);
    sortByDepth->setDefaultValue(true);
    page->addKnob(sortByDepth);
    _imp->sortByDepth = sortByDepth;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Enable options above to consolidate deep samples.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepConsolidate::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    if (_imp->removeZeroAlpha.lock().get() == k ||
        _imp->alphaThreshold.lock().get() == k ||
        _imp->mergeOverlapping.lock().get() == k ||
        _imp->mergeThreshold.lock().get() == k ||
        _imp->sortByDepth.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepConsolidate::getRegionOfDefinition(U64 /*hash*/,
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
DeepConsolidate::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepConsolidate::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    bool doRemoveZero = _imp->removeZeroAlpha.lock()->getValue();
    double alphaThreshVal = _imp->alphaThreshold.lock()->getValue();
    bool doMerge = _imp->mergeOverlapping.lock()->getValue();
    double mergeThreshVal = _imp->mergeThreshold.lock()->getValue();
    bool doSort = _imp->sortByDepth.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");
    int zBackIdx = srcDeep->findChannelIndex("ZBack");

    if (aIdx < 0 && zIdx < 0) {
        // No alpha or Z channel — pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // Working buffer for per-pixel processing
    // We process each pixel: sort, remove zero-alpha, merge overlapping
    // Then do a two-pass: count surviving samples, allocate, fill

    // First pass: count surviving samples per pixel
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    // Temporary storage for processed samples per pixel
    // We need to do this in two passes, so we store intermediate results
    // Actually, we need the processed data for the second pass, so let's
    // store it in a vector of vectors

    // For efficiency, process pixel-by-pixel and store results temporarily
    struct PixelResult {
        int sampleCount;
        std::vector<float> data; // nChannels * sampleCount
    };

    int width = dw.x2 - dw.x1;
    int height = dw.y2 - dw.y1;
    std::vector<PixelResult> pixelResults(width * height);

    long long totalSamplesIn = 0;
    long long totalSamplesOut = 0;

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            int pixIdx = (y - dw.y1) * width + (x - dw.x1);
            totalSamplesIn += nSamples;

            if (nSamples == 0) {
                pixelResults[pixIdx].sampleCount = 0;
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);

            // Build index array for sorting
            std::vector<int> indices(nSamples);
            for (int s = 0; s < nSamples; ++s) {
                indices[s] = s;
            }

            // Sort by Z if requested
            if (doSort && zIdx >= 0) {
                // Insertion sort on indices by Z value
                for (int i = 1; i < nSamples; ++i) {
                    int key = indices[i];
                    float keyZ = srcData[key * nChannels + zIdx];
                    int j = i - 1;
                    while (j >= 0 && srcData[indices[j] * nChannels + zIdx] > keyZ) {
                        indices[j + 1] = indices[j];
                        --j;
                    }
                    indices[j + 1] = key;
                }
            }

            // Copy samples in sorted order into working buffer
            std::vector<float> working(nSamples * nChannels);
            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + indices[s] * nChannels;
                float* dst = &working[s * nChannels];
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }
            }

            // Remove zero-alpha samples
            int count = nSamples;
            if (doRemoveZero && aIdx >= 0) {
                int writePos = 0;
                for (int s = 0; s < count; ++s) {
                    float a = working[s * nChannels + aIdx];
                    if (a > alphaThreshVal) {
                        if (writePos != s) {
                            float* dst = &working[writePos * nChannels];
                            const float* src = &working[s * nChannels];
                            for (int c = 0; c < nChannels; ++c) {
                                dst[c] = src[c];
                            }
                        }
                        ++writePos;
                    }
                }
                count = writePos;
            }

            // Merge overlapping samples
            if (doMerge && zIdx >= 0 && aIdx >= 0 && count > 1) {
                int writePos = 0;
                for (int s = 0; s < count; ++s) {
                    if (writePos > 0) {
                        float z1 = working[(writePos - 1) * nChannels + zIdx];
                        float z2 = working[s * nChannels + zIdx];
                        if (std::fabs(z1 - z2) < mergeThreshVal) {
                            // Merge sample s into writePos-1
                            float* merged = &working[(writePos - 1) * nChannels];
                            const float* cur = &working[s * nChannels];

                            float a1 = merged[aIdx];
                            float a2 = cur[aIdx];
                            float mergedA = a1 + a2 - a1 * a2; // correct composite

                            // Z: min of the two
                            if (zIdx >= 0) {
                                merged[zIdx] = std::min(merged[zIdx], cur[zIdx]);
                            }
                            // ZBack: max of the two
                            if (zBackIdx >= 0) {
                                float zb1 = merged[zBackIdx];
                                float zb2 = cur[zBackIdx];
                                merged[zBackIdx] = std::max(zb1, zb2);
                            }

                            // RGB channels: unpremult, average, re-premult
                            if (mergedA > 0.0f) {
                                for (int c = 0; c < nChannels; ++c) {
                                    if (c == aIdx || c == zIdx || c == zBackIdx) {
                                        continue;
                                    }
                                    // Alpha-weighted blend of channel values
                                    float v1 = merged[c];
                                    float v2 = cur[c];
                                    merged[c] = (v1 * a1 + v2 * a2) / mergedA;
                                }
                            }

                            merged[aIdx] = mergedA;
                            continue; // don't advance writePos
                        }
                    }
                    // No merge — copy forward
                    if (writePos != s) {
                        float* dst = &working[writePos * nChannels];
                        const float* src = &working[s * nChannels];
                        for (int c = 0; c < nChannels; ++c) {
                            dst[c] = src[c];
                        }
                    }
                    ++writePos;
                }
                count = writePos;
            }

            totalSamplesOut += count;
            pixelResults[pixIdx].sampleCount = count;
            pixelResults[pixIdx].data.resize(count * nChannels);
            for (int i = 0; i < count * nChannels; ++i) {
                pixelResults[pixIdx].data[i] = working[i];
            }
        }
    }

    // Set sample counts and allocate
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int pixIdx = (y - dw.y1) * width + (x - dw.x1);
            result->setSampleCount(x, y, pixelResults[pixIdx].sampleCount);
        }
    }

    result->allocateFromSampleCounts();

    // Fill pass: copy processed data into result
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int pixIdx = (y - dw.y1) * width + (x - dw.x1);
            int count = pixelResults[pixIdx].sampleCount;
            if (count == 0) {
                continue;
            }

            float* dstData = result->getSampleData(x, y);
            if (!dstData) {
                continue;
            }

            const float* srcProcessed = pixelResults[pixIdx].data.data();
            for (int i = 0; i < count * nChannels; ++i) {
                dstData[i] = srcProcessed[i];
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Samples in: " << totalSamplesIn
       << " | Samples out: " << totalSamplesOut
       << " | Reduced by: " << (totalSamplesIn - totalSamplesOut);
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

#include "moc_DeepConsolidate.cpp"
