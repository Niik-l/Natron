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

#include "DeepLayerBreak.h"

#include <algorithm>
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


struct DeepLayerBreakPrivate
{
    KnobDoubleWPtr nearDepth;
    KnobDoubleWPtr farDepth;
    KnobStringWPtr info;
};


DeepLayerBreak::DeepLayerBreak(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepLayerBreakPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepLayerBreak::~DeepLayerBreak()
{
}

std::string
DeepLayerBreak::getPluginDescription() const
{
    return tr("Flatten specific depth bands from a deep image. "
              "Set near/far depth to extract and flatten only samples within that range. "
              "Like DeepSlice + DeepFlatten in one node.").toStdString();
}

void
DeepLayerBreak::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepLayerBreak::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepLayerBreak::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepLayerBreak::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr nearDepth = AppManager::createKnob<KnobDouble>(this, tr("Near Depth"));
    nearDepth->setName("nearDepth");
    nearDepth->setHintToolTip(tr("Minimum Z depth. Only samples with Z >= this value are included in the flattened output."));
    nearDepth->setAnimationEnabled(true);
    nearDepth->setDefaultValue(0.0);
    nearDepth->setMinimum(0.0);
    page->addKnob(nearDepth);
    _imp->nearDepth = nearDepth;

    KnobDoublePtr farDepth = AppManager::createKnob<KnobDouble>(this, tr("Far Depth"));
    farDepth->setName("farDepth");
    farDepth->setHintToolTip(tr("Maximum Z depth. Only samples with Z <= this value are included. "
                                "Set to 0 for unlimited (no far clip)."));
    farDepth->setAnimationEnabled(true);
    farDepth->setDefaultValue(0.0);
    farDepth->setMinimum(0.0);
    page->addKnob(farDepth);
    _imp->farDepth = farDepth;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set Near/Far Depth to flatten a specific depth band.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepLayerBreak::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/, double time, bool /*originatedFromMainThread*/)
{
    if (_imp->nearDepth.lock().get() == k || _imp->farDepth.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepLayerBreak::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                      ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

StatusEnum
DeepLayerBreak::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    double nearDepthVal = _imp->nearDepth.lock()->getValue();
    double farDepthVal = _imp->farDepth.lock()->getValue();

    if (farDepthVal <= 0.0) {
        farDepthVal = std::numeric_limits<double>::max();
    }

    const RectI& dw = srcDeep->getDataWindow();
    int w = dw.width();
    int h = dw.height();
    int nChannels = srcDeep->getNumChannels();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    // Flatten samples within the depth range into an RGBA buffer
    std::vector<float> resultBuf(w * h * 4, 0.0f);

    int samplesUsed = 0;

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* data = srcDeep->getSampleData(x, y);

            // Front-to-back compositing of samples within range
            float accumR = 0.0f, accumG = 0.0f, accumB = 0.0f, accumA = 0.0f;

            for (int s = 0; s < nSamples; ++s) {
                // Check depth range
                if (zIdx >= 0) {
                    float z = data[s * nChannels + zIdx];
                    if (z < nearDepthVal || z > farDepthVal) {
                        continue;
                    }
                }

                float sR = (rIdx >= 0) ? data[s * nChannels + rIdx] : 0.0f;
                float sG = (gIdx >= 0) ? data[s * nChannels + gIdx] : 0.0f;
                float sB = (bIdx >= 0) ? data[s * nChannels + bIdx] : 0.0f;
                float sA = (aIdx >= 0) ? data[s * nChannels + aIdx] : 1.0f;

                sA = std::max(0.0f, std::min(1.0f, sA));

                // Front-to-back over operation
                float oneMinusAccumA = 1.0f - accumA;
                accumR += sR * oneMinusAccumA;
                accumG += sG * oneMinusAccumA;
                accumB += sB * oneMinusAccumA;
                accumA += sA * oneMinusAccumA;

                ++samplesUsed;

                if (accumA >= 1.0f) {
                    accumA = 1.0f;
                    break;
                }
            }

            int px = x - dw.x1;
            int py = y - dw.y1;
            int pixIdx = (py * w + px) * 4;
            resultBuf[pixIdx + 0] = accumR;
            resultBuf[pixIdx + 1] = accumG;
            resultBuf[pixIdx + 2] = accumB;
            resultBuf[pixIdx + 3] = accumA;
        }
    }

    // Write to output image (Y-flip pattern)
    assert(!args.outputPlanes.empty());
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    RectI outBounds = outImg->getBounds();
    int outW = outBounds.width();
    int outH = outBounds.height();

    Image::WriteAccess wa(outImg.get());
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        float normY = (float)(y - outBounds.y1) / std::max(1, outH - 1);
        int srcRow = (int)((1.0f - normY) * (h - 1) + 0.5f);
        srcRow = std::max(0, std::min(h - 1, srcRow));

        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            float normX = (float)(x - outBounds.x1) / std::max(1, outW - 1);
            int srcCol = (int)(normX * (w - 1) + 0.5f);
            srcCol = std::max(0, std::min(w - 1, srcCol));

            int srcIdx = (srcRow * w + srcCol) * 4;
            dst[0] = resultBuf[srcIdx + 0];
            dst[1] = resultBuf[srcIdx + 1];
            dst[2] = resultBuf[srcIdx + 2];
            dst[3] = resultBuf[srcIdx + 3];
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Near: " << nearDepthVal << " | Far: ";
    if (farDepthVal >= std::numeric_limits<double>::max() / 2.0) {
        ss << "unlimited";
    } else {
        ss << farDepthVal;
    }
    ss << " | Image: " << w << "x" << h
       << " | Samples flattened: " << samplesUsed;
    _imp->info.lock()->setValue(ss.str());

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepLayerBreak.cpp"
