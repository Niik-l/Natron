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

#include "DeepGrade.h"

#include <cassert>
#include <cmath>
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


struct DeepGradePrivate
{
    KnobDoubleWPtr gainR, gainG, gainB;
    KnobDoubleWPtr gammaR, gammaG, gammaB;
    KnobDoubleWPtr offsetR, offsetG, offsetB;
};


DeepGrade::DeepGrade(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepGradePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepGrade::~DeepGrade()
{
}

std::string
DeepGrade::getPluginDescription() const
{
    return tr("Color correct deep image samples. Applies gain, offset, and gamma to the RGB "
              "channels of each deep sample while preserving depth and alpha structure.\n\n"
              "Formula (per channel): out = (in * gain + offset) ^ (1/gamma)\n\n"
              "The correction is applied to the unpremultiplied color values.").toStdString();
}

void
DeepGrade::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepGrade::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepGrade::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepGrade::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Gain (0-4 slider range, like ColorCorrect)
    KnobDoublePtr gR = AppManager::createKnob<KnobDouble>(this, tr("Gain R"));
    gR->setName("gainR"); gR->setDefaultValue(1.0); gR->setMinimum(0.0); gR->setMaximum(10.0);
    gR->setDisplayMinimum(0.0); gR->setDisplayMaximum(4.0); gR->setAnimationEnabled(true);
    page->addKnob(gR); _imp->gainR = gR;

    KnobDoublePtr gG = AppManager::createKnob<KnobDouble>(this, tr("Gain G"));
    gG->setName("gainG"); gG->setDefaultValue(1.0); gG->setMinimum(0.0); gG->setMaximum(10.0);
    gG->setDisplayMinimum(0.0); gG->setDisplayMaximum(4.0); gG->setAnimationEnabled(true);
    page->addKnob(gG); _imp->gainG = gG;

    KnobDoublePtr gB = AppManager::createKnob<KnobDouble>(this, tr("Gain B"));
    gB->setName("gainB"); gB->setDefaultValue(1.0); gB->setMinimum(0.0); gB->setMaximum(10.0);
    gB->setDisplayMinimum(0.0); gB->setDisplayMaximum(4.0); gB->setAnimationEnabled(true);
    page->addKnob(gB); _imp->gainB = gB;

    // Gamma (0.01-5 slider range)
    KnobDoublePtr gamR = AppManager::createKnob<KnobDouble>(this, tr("Gamma R"));
    gamR->setName("gammaR"); gamR->setDefaultValue(1.0); gamR->setMinimum(0.01); gamR->setMaximum(10.0);
    gamR->setDisplayMinimum(0.0); gamR->setDisplayMaximum(5.0); gamR->setAnimationEnabled(true);
    page->addKnob(gamR); _imp->gammaR = gamR;

    KnobDoublePtr gamG = AppManager::createKnob<KnobDouble>(this, tr("Gamma G"));
    gamG->setName("gammaG"); gamG->setDefaultValue(1.0); gamG->setMinimum(0.01); gamG->setMaximum(10.0);
    gamG->setDisplayMinimum(0.0); gamG->setDisplayMaximum(5.0); gamG->setAnimationEnabled(true);
    page->addKnob(gamG); _imp->gammaG = gamG;

    KnobDoublePtr gamB = AppManager::createKnob<KnobDouble>(this, tr("Gamma B"));
    gamB->setName("gammaB"); gamB->setDefaultValue(1.0); gamB->setMinimum(0.01); gamB->setMaximum(10.0);
    gamB->setDisplayMinimum(0.0); gamB->setDisplayMaximum(5.0); gamB->setAnimationEnabled(true);
    page->addKnob(gamB); _imp->gammaB = gamB;

    // Offset (-1 to 1 slider range)
    KnobDoublePtr oR = AppManager::createKnob<KnobDouble>(this, tr("Offset R"));
    oR->setName("offsetR"); oR->setDefaultValue(0.0); oR->setMinimum(-10.0); oR->setMaximum(10.0);
    oR->setDisplayMinimum(-1.0); oR->setDisplayMaximum(1.0); oR->setAnimationEnabled(true);
    page->addKnob(oR); _imp->offsetR = oR;

    KnobDoublePtr oG = AppManager::createKnob<KnobDouble>(this, tr("Offset G"));
    oG->setName("offsetG"); oG->setDefaultValue(0.0); oG->setMinimum(-10.0); oG->setMaximum(10.0);
    oG->setDisplayMinimum(-1.0); oG->setDisplayMaximum(1.0); oG->setAnimationEnabled(true);
    page->addKnob(oG); _imp->offsetG = oG;

    KnobDoublePtr oB = AppManager::createKnob<KnobDouble>(this, tr("Offset B"));
    oB->setName("offsetB"); oB->setDefaultValue(0.0); oB->setMinimum(-10.0); oB->setMaximum(10.0);
    oB->setDisplayMinimum(-1.0); oB->setDisplayMaximum(1.0); oB->setAnimationEnabled(true);
    page->addKnob(oB); _imp->offsetB = oB;
}

StatusEnum
DeepGrade::getRegionOfDefinition(U64 /*hash*/,
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
DeepGrade::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepGrade::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    float gain[3] = {
        (float)_imp->gainR.lock()->getValue(),
        (float)_imp->gainG.lock()->getValue(),
        (float)_imp->gainB.lock()->getValue()
    };
    float gamma[3] = {
        (float)_imp->gammaR.lock()->getValue(),
        (float)_imp->gammaG.lock()->getValue(),
        (float)_imp->gammaB.lock()->getValue()
    };
    float offset[3] = {
        (float)_imp->offsetR.lock()->getValue(),
        (float)_imp->offsetG.lock()->getValue(),
        (float)_imp->offsetB.lock()->getValue()
    };

    // Precompute inverse gamma
    float invGamma[3];
    for (int i = 0; i < 3; ++i) {
        invGamma[i] = (gamma[i] > 0.001f) ? (1.0f / gamma[i]) : 1.0f;
    }

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");

    if (rIdx < 0 || gIdx < 0 || bIdx < 0 || aIdx < 0) {
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    int rgbIdx[3] = {rIdx, gIdx, bIdx};

    // Create result with same structure
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }
    result->allocateFromSampleCounts();

    // Apply grade to each sample
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) continue;

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                // Copy all channels first
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Apply grade to RGB (unpremultiply, grade, re-premultiply)
                float a = src[aIdx];
                if (a > 0.0001f) {
                    for (int i = 0; i < 3; ++i) {
                        int ci = rgbIdx[i];
                        float unpremult = src[ci] / a;
                        float graded = unpremult * gain[i] + offset[i];
                        graded = std::max(0.0f, graded);
                        if (std::abs(invGamma[i] - 1.0f) > 0.001f) {
                            graded = std::pow(graded, invGamma[i]);
                        }
                        dst[ci] = graded * a; // re-premultiply
                    }
                }
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

#include "moc_DeepGrade.cpp"
