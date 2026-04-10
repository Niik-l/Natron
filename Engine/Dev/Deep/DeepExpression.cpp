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

#include "DeepExpression.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
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

struct DeepExpressionPrivate
{
    // Per-channel multiply and add
    KnobDoubleWPtr mulR, mulG, mulB, mulA;
    KnobDoubleWPtr addR, addG, addB, addA;

    // Z modification
    KnobDoubleWPtr mulZ, addZ;

    // Depth range filter
    KnobDoubleWPtr zMin, zMax;

    // Clamp
    KnobBoolWPtr clampOutput;

    // Remove samples
    KnobBoolWPtr removeBelowAlpha;
    KnobDoubleWPtr alphaThreshold;
};


DeepExpression::DeepExpression(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepExpressionPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepExpression::~DeepExpression()
{
}

std::string
DeepExpression::getPluginDescription() const
{
    return tr("Apply per-sample modifications to deep data.\n\n"
              "For each channel, the formula is: output = input * multiply + add\n\n"
              "Depth Range: Only modify samples whose Z falls within [Z Min, Z Max]. "
              "Set Z Max to 0 for unlimited.\n\n"
              "Remove Below Alpha: Discard samples whose alpha falls below the threshold "
              "after modification. Useful for cleaning up near-zero samples.").toStdString();
}

void
DeepExpression::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepExpression::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepExpression::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepExpression::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Multiply
    KnobDoublePtr mR = AppManager::createKnob<KnobDouble>(this, tr("Multiply R"));
    mR->setName("mulR"); mR->setDefaultValue(1.0); mR->setDisplayMinimum(0.0); mR->setDisplayMaximum(4.0);
    mR->setAnimationEnabled(true); page->addKnob(mR); _imp->mulR = mR;

    KnobDoublePtr mG = AppManager::createKnob<KnobDouble>(this, tr("Multiply G"));
    mG->setName("mulG"); mG->setDefaultValue(1.0); mG->setDisplayMinimum(0.0); mG->setDisplayMaximum(4.0);
    mG->setAnimationEnabled(true); page->addKnob(mG); _imp->mulG = mG;

    KnobDoublePtr mB = AppManager::createKnob<KnobDouble>(this, tr("Multiply B"));
    mB->setName("mulB"); mB->setDefaultValue(1.0); mB->setDisplayMinimum(0.0); mB->setDisplayMaximum(4.0);
    mB->setAnimationEnabled(true); page->addKnob(mB); _imp->mulB = mB;

    KnobDoublePtr mA = AppManager::createKnob<KnobDouble>(this, tr("Multiply A"));
    mA->setName("mulA"); mA->setDefaultValue(1.0); mA->setDisplayMinimum(0.0); mA->setDisplayMaximum(4.0);
    mA->setAnimationEnabled(true); page->addKnob(mA); _imp->mulA = mA;

    // Add
    KnobDoublePtr aR = AppManager::createKnob<KnobDouble>(this, tr("Add R"));
    aR->setName("addR"); aR->setDefaultValue(0.0); aR->setDisplayMinimum(-1.0); aR->setDisplayMaximum(1.0);
    aR->setAnimationEnabled(true); page->addKnob(aR); _imp->addR = aR;

    KnobDoublePtr aG = AppManager::createKnob<KnobDouble>(this, tr("Add G"));
    aG->setName("addG"); aG->setDefaultValue(0.0); aG->setDisplayMinimum(-1.0); aG->setDisplayMaximum(1.0);
    aG->setAnimationEnabled(true); page->addKnob(aG); _imp->addG = aG;

    KnobDoublePtr aB = AppManager::createKnob<KnobDouble>(this, tr("Add B"));
    aB->setName("addB"); aB->setDefaultValue(0.0); aB->setDisplayMinimum(-1.0); aB->setDisplayMaximum(1.0);
    aB->setAnimationEnabled(true); page->addKnob(aB); _imp->addB = aB;

    KnobDoublePtr aA = AppManager::createKnob<KnobDouble>(this, tr("Add A"));
    aA->setName("addA"); aA->setDefaultValue(0.0); aA->setDisplayMinimum(-1.0); aA->setDisplayMaximum(1.0);
    aA->setAnimationEnabled(true); page->addKnob(aA); _imp->addA = aA;

    // Z modification
    KnobDoublePtr mZ = AppManager::createKnob<KnobDouble>(this, tr("Multiply Z"));
    mZ->setName("mulZ"); mZ->setDefaultValue(1.0); mZ->setDisplayMinimum(0.01); mZ->setDisplayMaximum(10.0);
    mZ->setAnimationEnabled(true); page->addKnob(mZ); _imp->mulZ = mZ;

    KnobDoublePtr aZ = AppManager::createKnob<KnobDouble>(this, tr("Add Z"));
    aZ->setName("addZ"); aZ->setDefaultValue(0.0); aZ->setDisplayMinimum(-100.0); aZ->setDisplayMaximum(100.0);
    aZ->setAnimationEnabled(true); page->addKnob(aZ); _imp->addZ = aZ;

    // Depth range
    KnobPagePtr rangePage = AppManager::createKnob<KnobPage>(this, tr("Range"));

    KnobDoublePtr zMin = AppManager::createKnob<KnobDouble>(this, tr("Z Min"));
    zMin->setName("zMin"); zMin->setDefaultValue(0.0); zMin->setMinimum(0.0);
    zMin->setDisplayMinimum(0.0); zMin->setDisplayMaximum(1000.0);
    zMin->setHintToolTip(tr("Only modify samples with Z >= this value."));
    rangePage->addKnob(zMin); _imp->zMin = zMin;

    KnobDoublePtr zMax = AppManager::createKnob<KnobDouble>(this, tr("Z Max"));
    zMax->setName("zMax"); zMax->setDefaultValue(0.0); zMax->setMinimum(0.0);
    zMax->setDisplayMinimum(0.0); zMax->setDisplayMaximum(1000.0);
    zMax->setHintToolTip(tr("Only modify samples with Z <= this value. Set to 0 for unlimited."));
    rangePage->addKnob(zMax); _imp->zMax = zMax;

    KnobBoolPtr clamp = AppManager::createKnob<KnobBool>(this, tr("Clamp Output"));
    clamp->setName("clamp"); clamp->setDefaultValue(false);
    clamp->setHintToolTip(tr("Clamp output RGBA values to [0, 1] range."));
    rangePage->addKnob(clamp); _imp->clampOutput = clamp;

    KnobBoolPtr removeBelow = AppManager::createKnob<KnobBool>(this, tr("Remove Below Alpha"));
    removeBelow->setName("removeBelowAlpha"); removeBelow->setDefaultValue(false);
    removeBelow->setHintToolTip(tr("Remove samples whose alpha falls below the threshold after modification."));
    rangePage->addKnob(removeBelow); _imp->removeBelowAlpha = removeBelow;

    KnobDoublePtr alphaThresh = AppManager::createKnob<KnobDouble>(this, tr("Alpha Threshold"));
    alphaThresh->setName("alphaThreshold"); alphaThresh->setDefaultValue(0.001);
    alphaThresh->setMinimum(0.0); alphaThresh->setDisplayMinimum(0.0); alphaThresh->setDisplayMaximum(0.1);
    rangePage->addKnob(alphaThresh); _imp->alphaThreshold = alphaThresh;
}

StatusEnum
DeepExpression::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                      ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

DeepImagePtr
DeepExpression::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepExpression::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    // Read knob values
    float mulR = (float)_imp->mulR.lock()->getValue();
    float mulG = (float)_imp->mulG.lock()->getValue();
    float mulB = (float)_imp->mulB.lock()->getValue();
    float mulA = (float)_imp->mulA.lock()->getValue();
    float addR = (float)_imp->addR.lock()->getValue();
    float addG = (float)_imp->addG.lock()->getValue();
    float addB = (float)_imp->addB.lock()->getValue();
    float addA = (float)_imp->addA.lock()->getValue();
    float mulZ = (float)_imp->mulZ.lock()->getValue();
    float addZ = (float)_imp->addZ.lock()->getValue();
    float zMinVal = (float)_imp->zMin.lock()->getValue();
    float zMaxVal = (float)_imp->zMax.lock()->getValue();
    bool clamp = _imp->clampOutput.lock()->getValue();
    bool removeBelow = _imp->removeBelowAlpha.lock()->getValue();
    float alphaThresh = (float)_imp->alphaThreshold.lock()->getValue();

    if (zMaxVal <= 0.0f) zMaxVal = std::numeric_limits<float>::max();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");
    int zBackIdx = srcDeep->findChannelIndex("ZBack");

    // First pass: count surviving samples (if removeBelow is enabled)
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    if (removeBelow) {
        // Need two passes — count then copy
        for (int y = dw.y1; y < dw.y2; ++y) {
            for (int x = dw.x1; x < dw.x2; ++x) {
                int nSamples = srcDeep->getSampleCount(x, y);
                if (nSamples == 0) { result->setSampleCount(x, y, 0); continue; }

                const float* srcData = srcDeep->getSampleData(x, y);
                int kept = 0;
                for (int s = 0; s < nSamples; ++s) {
                    const float* src = srcData + s * nChannels;
                    float z = (zIdx >= 0) ? src[zIdx] : 0.0f;
                    float a = (aIdx >= 0) ? src[aIdx] : 1.0f;

                    // Apply modification to alpha
                    if (z >= zMinVal && z <= zMaxVal) {
                        a = a * mulA + addA;
                    }
                    if (a >= alphaThresh) ++kept;
                }
                result->setSampleCount(x, y, kept);
            }
        }
    } else {
        // No removal — same sample counts
        for (int y = dw.y1; y < dw.y2; ++y) {
            for (int x = dw.x1; x < dw.x2; ++x) {
                result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
            }
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: apply modifications
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);
            if (!dstData) continue;

            int outIdx = 0;
            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float z = (zIdx >= 0) ? src[zIdx] : 0.0f;
                bool inRange = (z >= zMinVal && z <= zMaxVal);

                // Check if this sample survives removal
                if (removeBelow && inRange) {
                    float a = (aIdx >= 0) ? src[aIdx] : 1.0f;
                    a = a * mulA + addA;
                    if (a < alphaThresh) continue;
                }

                float* dst = dstData + outIdx * nChannels;

                // Copy all channels first
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Apply modifications only if in depth range
                if (inRange) {
                    if (rIdx >= 0) dst[rIdx] = src[rIdx] * mulR + addR;
                    if (gIdx >= 0) dst[gIdx] = src[gIdx] * mulG + addG;
                    if (bIdx >= 0) dst[bIdx] = src[bIdx] * mulB + addB;
                    if (aIdx >= 0) dst[aIdx] = src[aIdx] * mulA + addA;
                    if (zIdx >= 0) dst[zIdx] = src[zIdx] * mulZ + addZ;
                    if (zBackIdx >= 0) dst[zBackIdx] = src[zBackIdx] * mulZ + addZ;

                    if (clamp) {
                        if (rIdx >= 0) dst[rIdx] = std::max(0.0f, std::min(1.0f, dst[rIdx]));
                        if (gIdx >= 0) dst[gIdx] = std::max(0.0f, std::min(1.0f, dst[gIdx]));
                        if (bIdx >= 0) dst[bIdx] = std::max(0.0f, std::min(1.0f, dst[bIdx]));
                        if (aIdx >= 0) dst[aIdx] = std::max(0.0f, std::min(1.0f, dst[aIdx]));
                    }
                }

                ++outIdx;
            }
        }
    }

    _lastDeepImage = result;

    // Flattened preview
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

#include "moc_DeepExpression.cpp"
