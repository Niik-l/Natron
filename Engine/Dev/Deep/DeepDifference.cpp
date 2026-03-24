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

#include "DeepDifference.h"

#include <algorithm>
#include <cassert>
#include <cmath>
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


struct DeepDifferencePrivate
{
    KnobChoiceWPtr mode;
    KnobDoubleWPtr gain;
    KnobStringWPtr info;
};


DeepDifference::DeepDifference(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepDifferencePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepDifference::~DeepDifference()
{
}

std::string
DeepDifference::getPluginDescription() const
{
    return tr("Compute the difference between two deep images for quality control. "
              "Output shows where samples differ in color, alpha, or depth.").toStdString();
}

void
DeepDifference::addAcceptedComponents(int /*inputNb*/,
                                      std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepDifference::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepDifference::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepDifference::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobChoicePtr mode = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
    mode->setName("mode");
    mode->setHintToolTip(tr("Which aspect of the deep images to compare."));
    mode->setAnimationEnabled(false);
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Color Difference", "Color Difference", ""));
        entries.push_back(ChoiceOption("Alpha Difference", "Alpha Difference", ""));
        entries.push_back(ChoiceOption("Sample Count Difference", "Sample Count Difference", ""));
        mode->populateChoices(entries);
    }
    mode->setDefaultValue(0);
    page->addKnob(mode);
    _imp->mode = mode;

    KnobDoublePtr gain = AppManager::createKnob<KnobDouble>(this, tr("Gain"));
    gain->setName("gain");
    gain->setHintToolTip(tr("Multiplier applied to the difference output for visibility."));
    gain->setAnimationEnabled(true);
    gain->setDefaultValue(1.0);
    gain->setMinimum(0.0);
    gain->setDisplayMinimum(0.0);
    gain->setDisplayMaximum(100.0);
    page->addKnob(gain);
    _imp->gain = gain;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect two deep images to compute their difference.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepDifference::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if (_imp->mode.lock().get() == k ||
        _imp->gain.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepDifference::getRegionOfDefinition(U64 /*hash*/,
                                      double /*time*/,
                                      const RenderScale& /*scale*/,
                                      ViewIdx /*view*/,
                                      RectD* rod)
{
    // Union of both inputs
    EffectInstancePtr inputA = getInput(0);
    EffectInstancePtr inputB = getInput(1);
    if (!inputA && !inputB) return eStatusFailed;

    bool isProjectFormat = false;
    RectD rodA, rodB;
    bool hasA = false, hasB = false;

    if (inputA) {
        if (inputA->getRegionOfDefinition_public(inputA->getHash(), 0, RenderScale(), ViewIdx(0), &rodA, &isProjectFormat) == eStatusOK) {
            hasA = true;
        }
    }
    if (inputB) {
        if (inputB->getRegionOfDefinition_public(inputB->getHash(), 0, RenderScale(), ViewIdx(0), &rodB, &isProjectFormat) == eStatusOK) {
            hasB = true;
        }
    }

    if (hasA && hasB) {
        rod->x1 = std::min(rodA.x1, rodB.x1);
        rod->y1 = std::min(rodA.y1, rodB.y1);
        rod->x2 = std::max(rodA.x2, rodB.x2);
        rod->y2 = std::max(rodA.y2, rodB.y2);
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
DeepDifference::render(const RenderActionArgs& args)
{
    EffectInstancePtr inputA = getInput(0);
    EffectInstancePtr inputB = getInput(1);

    DeepImagePtr deepA = getDeepImageFromEffect(inputA.get());
    DeepImagePtr deepB = getDeepImageFromEffect(inputB.get());

    if (!deepA && !deepB) {
        return eStatusFailed;
    }

    int modeVal = _imp->mode.lock()->getValue();
    double gainVal = _imp->gain.lock()->getValue();

    // Flatten both deep images to get RGBA buffers for comparison
    // We need the union data window
    RectI dwA, dwB;
    int wA = 0, hA = 0, wB = 0, hB = 0;

    if (deepA) {
        dwA = deepA->getDataWindow();
        wA = dwA.width();
        hA = dwA.height();
    }
    if (deepB) {
        dwB = deepB->getDataWindow();
        wB = dwB.width();
        hB = dwB.height();
    }

    // Compute union window
    RectI dw;
    if (deepA && deepB) {
        dw.x1 = std::min(dwA.x1, dwB.x1);
        dw.y1 = std::min(dwA.y1, dwB.y1);
        dw.x2 = std::max(dwA.x2, dwB.x2);
        dw.y2 = std::max(dwA.y2, dwB.y2);
    } else if (deepA) {
        dw = dwA;
    } else {
        dw = dwB;
    }

    int w = dw.width();
    int h = dw.height();

    // Flatten A
    std::vector<float> flatA(w * h * 4, 0.0f);
    if (deepA) {
        int nChA = deepA->getNumChannels();
        int rIdxA = deepA->findChannelIndex("R");
        int gIdxA = deepA->findChannelIndex("G");
        int bIdxA = deepA->findChannelIndex("B");
        int aIdxA = deepA->findChannelIndex("A");

        for (int y = dwA.y1; y < dwA.y2; ++y) {
            for (int x = dwA.x1; x < dwA.x2; ++x) {
                int nSamples = deepA->getSampleCount(x, y);
                if (nSamples == 0) continue;
                const float* data = deepA->getSampleData(x, y);

                // Simple front-to-back composite
                float accR = 0, accG = 0, accB = 0, accA = 0;
                for (int s = 0; s < nSamples; ++s) {
                    const float* smp = data + s * nChA;
                    float sA = (aIdxA >= 0) ? smp[aIdxA] : 0.0f;
                    float sR = (rIdxA >= 0) ? smp[rIdxA] : 0.0f;
                    float sG = (gIdxA >= 0) ? smp[gIdxA] : 0.0f;
                    float sB = (bIdxA >= 0) ? smp[bIdxA] : 0.0f;
                    float rem = 1.0f - accA;
                    accR += sR * rem;
                    accG += sG * rem;
                    accB += sB * rem;
                    accA += sA * rem;
                    if (accA >= 1.0f) break;
                }

                int px = x - dw.x1;
                int py = y - dw.y1;
                int idx = (py * w + px) * 4;
                flatA[idx + 0] = accR;
                flatA[idx + 1] = accG;
                flatA[idx + 2] = accB;
                flatA[idx + 3] = accA;
            }
        }
    }

    // Flatten B
    std::vector<float> flatB(w * h * 4, 0.0f);
    if (deepB) {
        int nChB = deepB->getNumChannels();
        int rIdxB = deepB->findChannelIndex("R");
        int gIdxB = deepB->findChannelIndex("G");
        int bIdxB = deepB->findChannelIndex("B");
        int aIdxB = deepB->findChannelIndex("A");

        for (int y = dwB.y1; y < dwB.y2; ++y) {
            for (int x = dwB.x1; x < dwB.x2; ++x) {
                int nSamples = deepB->getSampleCount(x, y);
                if (nSamples == 0) continue;
                const float* data = deepB->getSampleData(x, y);

                float accR = 0, accG = 0, accB = 0, accA = 0;
                for (int s = 0; s < nSamples; ++s) {
                    const float* smp = data + s * nChB;
                    float sA = (aIdxB >= 0) ? smp[aIdxB] : 0.0f;
                    float sR = (rIdxB >= 0) ? smp[rIdxB] : 0.0f;
                    float sG = (gIdxB >= 0) ? smp[gIdxB] : 0.0f;
                    float sB = (bIdxB >= 0) ? smp[bIdxB] : 0.0f;
                    float rem = 1.0f - accA;
                    accR += sR * rem;
                    accG += sG * rem;
                    accB += sB * rem;
                    accA += sA * rem;
                    if (accA >= 1.0f) break;
                }

                int px = x - dw.x1;
                int py = y - dw.y1;
                int idx = (py * w + px) * 4;
                flatB[idx + 0] = accR;
                flatB[idx + 1] = accG;
                flatB[idx + 2] = accB;
                flatB[idx + 3] = accA;
            }
        }
    }

    // Compute difference
    std::vector<float> resultBuf(w * h * 4, 0.0f);
    float g = (float)gainVal;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = (y * w + x) * 4;
            float dr, dg_val, db, da;

            if (modeVal == 0) {
                // Color Difference
                dr = std::fabs(flatA[idx + 0] - flatB[idx + 0]) * g;
                dg_val = std::fabs(flatA[idx + 1] - flatB[idx + 1]) * g;
                db = std::fabs(flatA[idx + 2] - flatB[idx + 2]) * g;
                da = std::max(dr, std::max(dg_val, db));
            } else if (modeVal == 1) {
                // Alpha Difference
                float alphaDiff = std::fabs(flatA[idx + 3] - flatB[idx + 3]) * g;
                dr = alphaDiff;
                dg_val = alphaDiff;
                db = alphaDiff;
                da = alphaDiff;
            } else {
                // Sample Count Difference
                int px = x + dw.x1;
                int py = y + dw.y1;
                int countA = 0, countB = 0;
                if (deepA && px >= dwA.x1 && px < dwA.x2 && py >= dwA.y1 && py < dwA.y2) {
                    countA = deepA->getSampleCount(px, py);
                }
                if (deepB && px >= dwB.x1 && px < dwB.x2 && py >= dwB.y1 && py < dwB.y2) {
                    countB = deepB->getSampleCount(px, py);
                }
                float countDiff = std::fabs((float)(countA - countB)) * g;
                dr = countDiff;
                dg_val = countDiff;
                db = countDiff;
                da = countDiff;
            }

            resultBuf[idx + 0] = dr;
            resultBuf[idx + 1] = dg_val;
            resultBuf[idx + 2] = db;
            resultBuf[idx + 3] = da;
        }
    }

    // Write to output Image (Y-flip pattern from DeepGodRays)
    assert(!args.outputPlanes.empty());
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    RectI outBounds = outImg->getBounds();
    int outW = outBounds.width();
    int outH = outBounds.height();

    Image::WriteAccess wa(outImg.get());
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        // Y-flip: Natron bottom-up, deep top-down
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
    const char* modeNames[] = { "Color Difference", "Alpha Difference", "Sample Count Difference" };
    std::ostringstream ss;
    ss << "Mode: " << modeNames[std::min(modeVal, 2)]
       << " | Gain: " << gainVal
       << " | Image: " << w << "x" << h;
    _imp->info.lock()->setValue(ss.str());

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepDifference.cpp"
