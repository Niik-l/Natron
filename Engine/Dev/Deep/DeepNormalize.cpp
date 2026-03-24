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

#include "DeepNormalize.h"

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


struct DeepNormalizePrivate
{
    KnobBoolWPtr clampAlpha;
    KnobBoolWPtr fixNaN;
    KnobDoubleWPtr maxAlpha;
    KnobStringWPtr info;
};


DeepNormalize::DeepNormalize(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepNormalizePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepNormalize::~DeepNormalize()
{
}

std::string
DeepNormalize::getPluginDescription() const
{
    return tr("Fix broken deep data by ensuring per-sample alpha values are valid and "
              "accumulated alpha never exceeds 1.0. Also clamps negative values and "
              "fixes NaN/Inf.").toStdString();
}

void
DeepNormalize::addAcceptedComponents(int /*inputNb*/,
                                     std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepNormalize::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepNormalize::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                              bool* /*defaultG*/,
                                              bool* /*defaultB*/,
                                              bool* /*defaultA*/) const
{
    return false;
}

void
DeepNormalize::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobBoolPtr clampAlpha = AppManager::createKnob<KnobBool>(this, tr("Clamp Alpha"));
    clampAlpha->setName("clampAlpha");
    clampAlpha->setHintToolTip(tr("Clamp per-sample alpha to [0, Max Alpha]. "
                                  "Also ensures accumulated alpha never exceeds Max Alpha."));
    clampAlpha->setAnimationEnabled(false);
    clampAlpha->setDefaultValue(true);
    page->addKnob(clampAlpha);
    _imp->clampAlpha = clampAlpha;

    KnobBoolPtr fixNaN = AppManager::createKnob<KnobBool>(this, tr("Fix NaN/Inf"));
    fixNaN->setName("fixNaN");
    fixNaN->setHintToolTip(tr("Replace NaN and Inf values with 0."));
    fixNaN->setAnimationEnabled(false);
    fixNaN->setDefaultValue(true);
    page->addKnob(fixNaN);
    _imp->fixNaN = fixNaN;

    KnobDoublePtr maxAlpha = AppManager::createKnob<KnobDouble>(this, tr("Max Alpha"));
    maxAlpha->setName("maxAlpha");
    maxAlpha->setHintToolTip(tr("Maximum allowed alpha value per sample."));
    maxAlpha->setAnimationEnabled(true);
    maxAlpha->setDefaultValue(1.0);
    maxAlpha->setMinimum(0.0);
    maxAlpha->setMaximum(1.0);
    maxAlpha->setDisplayMinimum(0.0);
    maxAlpha->setDisplayMaximum(1.0);
    page->addKnob(maxAlpha);
    _imp->maxAlpha = maxAlpha;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Normalize deep data: clamp alpha, fix NaN/Inf.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepNormalize::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec /*view*/,
                            double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    if (_imp->clampAlpha.lock().get() == k ||
        _imp->fixNaN.lock().get() == k ||
        _imp->maxAlpha.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepNormalize::getRegionOfDefinition(U64 /*hash*/,
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
DeepNormalize::getDeepImage() const
{
    return _lastDeepImage;
}

static inline bool isNanOrInf(float v)
{
    return std::isnan(v) || std::isinf(v);
}

StatusEnum
DeepNormalize::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    bool doClampAlpha = _imp->clampAlpha.lock()->getValue();
    bool doFixNaN = _imp->fixNaN.lock()->getValue();
    double maxAlphaVal = _imp->maxAlpha.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");

    // First pass: same sample counts (we keep all samples, just fix values)
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            result->setSampleCount(x, y, nSamples);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy and fix values
    int fixedNaNCount = 0;
    int clampedAlphaCount = 0;

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

            float accumAlpha = 0.0f;

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                // Copy all channels
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Fix NaN/Inf
                if (doFixNaN) {
                    for (int c = 0; c < nChannels; ++c) {
                        if (isNanOrInf(dst[c])) {
                            dst[c] = 0.0f;
                            ++fixedNaNCount;
                        }
                    }
                }

                // Clamp alpha
                if (doClampAlpha && aIdx >= 0) {
                    float a = dst[aIdx];
                    if (a < 0.0f) {
                        dst[aIdx] = 0.0f;
                        ++clampedAlphaCount;
                    } else if (a > (float)maxAlphaVal) {
                        dst[aIdx] = (float)maxAlphaVal;
                        ++clampedAlphaCount;
                    }

                    // Check accumulated alpha
                    accumAlpha += dst[aIdx];
                    if (accumAlpha > (float)maxAlphaVal) {
                        // Reduce this sample's alpha so accumulated doesn't exceed max
                        float excess = accumAlpha - (float)maxAlphaVal;
                        dst[aIdx] = std::max(0.0f, dst[aIdx] - excess);
                        accumAlpha = (float)maxAlphaVal;
                        ++clampedAlphaCount;
                    }
                }

                // Clamp RGB to non-negative (premultiplied data should not be negative)
                if (rIdx >= 0 && dst[rIdx] < 0.0f) dst[rIdx] = 0.0f;
                if (gIdx >= 0 && dst[gIdx] < 0.0f) dst[gIdx] = 0.0f;
                if (bIdx >= 0 && dst[bIdx] < 0.0f) dst[bIdx] = 0.0f;
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Total samples: " << result->totalSamples()
       << " | NaN/Inf fixed: " << fixedNaNCount
       << " | Alpha clamped: " << clampedAlphaCount
       << " | Max Alpha: " << maxAlphaVal;
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

#include "moc_DeepNormalize.cpp"
