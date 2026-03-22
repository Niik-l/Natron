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

#include "DeepColorCorrect.h"

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

struct DeepColorCorrectPrivate
{
    // Master controls
    KnobDoubleWPtr saturation;
    KnobDoubleWPtr contrast;
    KnobDoubleWPtr gamma;
    KnobDoubleWPtr gain;
    KnobDoubleWPtr offset;

    // Depth masking
    KnobDoubleWPtr zMin, zMax;
    KnobDoubleWPtr zFalloff;

    // Mix
    KnobDoubleWPtr mix;
};


DeepColorCorrect::DeepColorCorrect(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepColorCorrectPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepColorCorrect::~DeepColorCorrect()
{
}

std::string
DeepColorCorrect::getPluginDescription() const
{
    return tr("Color correct deep image samples with depth-range masking.\n\n"
              "Master controls affect all samples (or only those in the depth range):\n"
              "- Saturation: 0 = monochrome, 1 = no change, 2 = double saturation\n"
              "- Contrast: 0 = flat grey, 1 = no change\n"
              "- Gamma: 1 = no change, < 1 = brighten, > 1 = darken\n"
              "- Gain: multiplier on RGB values\n"
              "- Offset: added to RGB values after gain\n\n"
              "Depth Range: Z Min/Max limits which samples are affected.\n"
              "Z Falloff: smooth transition at the range boundaries.\n"
              "Mix: blend between original and corrected (0 = original, 1 = full correction).").toStdString();
}

void
DeepColorCorrect::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepColorCorrect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepColorCorrect::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepColorCorrect::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr sat = AppManager::createKnob<KnobDouble>(this, tr("Saturation"));
    sat->setName("saturation"); sat->setDefaultValue(1.0);
    sat->setDisplayMinimum(0.0); sat->setDisplayMaximum(4.0);
    sat->setAnimationEnabled(true); page->addKnob(sat); _imp->saturation = sat;

    KnobDoublePtr con = AppManager::createKnob<KnobDouble>(this, tr("Contrast"));
    con->setName("contrast"); con->setDefaultValue(1.0);
    con->setDisplayMinimum(0.0); con->setDisplayMaximum(4.0);
    con->setAnimationEnabled(true); page->addKnob(con); _imp->contrast = con;

    KnobDoublePtr gam = AppManager::createKnob<KnobDouble>(this, tr("Gamma"));
    gam->setName("gamma"); gam->setDefaultValue(1.0);
    gam->setMinimum(0.01); gam->setDisplayMinimum(0.0); gam->setDisplayMaximum(5.0);
    gam->setAnimationEnabled(true); page->addKnob(gam); _imp->gamma = gam;

    KnobDoublePtr gn = AppManager::createKnob<KnobDouble>(this, tr("Gain"));
    gn->setName("gain"); gn->setDefaultValue(1.0);
    gn->setDisplayMinimum(0.0); gn->setDisplayMaximum(4.0);
    gn->setAnimationEnabled(true); page->addKnob(gn); _imp->gain = gn;

    KnobDoublePtr off = AppManager::createKnob<KnobDouble>(this, tr("Offset"));
    off->setName("offset"); off->setDefaultValue(0.0);
    off->setDisplayMinimum(-1.0); off->setDisplayMaximum(1.0);
    off->setAnimationEnabled(true); page->addKnob(off); _imp->offset = off;

    KnobDoublePtr mx = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
    mx->setName("mix"); mx->setDefaultValue(1.0);
    mx->setMinimum(0.0); mx->setMaximum(1.0);
    mx->setDisplayMinimum(0.0); mx->setDisplayMaximum(1.0);
    mx->setAnimationEnabled(true); page->addKnob(mx); _imp->mix = mx;

    // Depth masking page
    KnobPagePtr maskPage = AppManager::createKnob<KnobPage>(this, tr("Depth Range"));

    KnobDoublePtr zMin = AppManager::createKnob<KnobDouble>(this, tr("Z Min"));
    zMin->setName("zMin"); zMin->setDefaultValue(0.0); zMin->setMinimum(0.0);
    zMin->setDisplayMinimum(0.0); zMin->setDisplayMaximum(1000.0);
    zMin->setHintToolTip(tr("Only affect samples with Z >= this value."));
    maskPage->addKnob(zMin); _imp->zMin = zMin;

    KnobDoublePtr zMax = AppManager::createKnob<KnobDouble>(this, tr("Z Max"));
    zMax->setName("zMax"); zMax->setDefaultValue(0.0); zMax->setMinimum(0.0);
    zMax->setDisplayMinimum(0.0); zMax->setDisplayMaximum(1000.0);
    zMax->setHintToolTip(tr("Only affect samples with Z <= this value. 0 = unlimited."));
    maskPage->addKnob(zMax); _imp->zMax = zMax;

    KnobDoublePtr falloff = AppManager::createKnob<KnobDouble>(this, tr("Z Falloff"));
    falloff->setName("zFalloff"); falloff->setDefaultValue(0.0); falloff->setMinimum(0.0);
    falloff->setDisplayMinimum(0.0); falloff->setDisplayMaximum(100.0);
    falloff->setHintToolTip(tr("Smooth transition width at depth range boundaries."));
    maskPage->addKnob(falloff); _imp->zFalloff = falloff;
}

StatusEnum
DeepColorCorrect::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                        ViewIdx /*view*/, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), 0, RenderScale(), ViewIdx(0), rod, &isProjectFormat);
}

DeepImagePtr
DeepColorCorrect::getDeepImage() const
{
    return _lastDeepImage;
}

// Compute depth mask weight: 1.0 = fully inside range, 0.0 = fully outside
static float
depthMaskWeight(float z, float zMin, float zMax, float falloff)
{
    if (falloff <= 0.0f) {
        return (z >= zMin && z <= zMax) ? 1.0f : 0.0f;
    }

    float weight = 1.0f;
    if (z < zMin) {
        weight = std::max(0.0f, 1.0f - (zMin - z) / falloff);
    } else if (z > zMax) {
        weight = std::max(0.0f, 1.0f - (z - zMax) / falloff);
    }
    return weight;
}

StatusEnum
DeepColorCorrect::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) return eStatusFailed;

    float satVal = (float)_imp->saturation.lock()->getValue();
    float conVal = (float)_imp->contrast.lock()->getValue();
    float gamVal = (float)_imp->gamma.lock()->getValue();
    float gainVal = (float)_imp->gain.lock()->getValue();
    float offVal = (float)_imp->offset.lock()->getValue();
    float mixVal = (float)_imp->mix.lock()->getValue();
    float zMinVal = (float)_imp->zMin.lock()->getValue();
    float zMaxVal = (float)_imp->zMax.lock()->getValue();
    float falloffVal = (float)_imp->zFalloff.lock()->getValue();

    if (zMaxVal <= 0.0f) zMaxVal = std::numeric_limits<float>::max();

    float invGamma = (gamVal > 0.001f) ? (1.0f / gamVal) : 1.0f;

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    // Same sample counts
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }
    result->allocateFromSampleCounts();

    // Apply color correction
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

                // Compute depth mask
                float z = (zIdx >= 0) ? src[zIdx] : 0.0f;
                float weight = depthMaskWeight(z, zMinVal, zMaxVal, falloffVal) * mixVal;

                if (weight < 0.001f) continue; // No modification needed

                // Get alpha for unpremult
                float a = (aIdx >= 0) ? src[aIdx] : 1.0f;
                if (a < 0.0001f) continue;

                // Unpremultiply RGB
                float r = (rIdx >= 0) ? src[rIdx] / a : 0.0f;
                float g = (gIdx >= 0) ? src[gIdx] / a : 0.0f;
                float b = (bIdx >= 0) ? src[bIdx] / a : 0.0f;

                // Apply saturation
                if (std::abs(satVal - 1.0f) > 0.001f) {
                    float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = luma + satVal * (r - luma);
                    g = luma + satVal * (g - luma);
                    b = luma + satVal * (b - luma);
                }

                // Apply contrast (around 0.5 pivot)
                if (std::abs(conVal - 1.0f) > 0.001f) {
                    r = 0.5f + conVal * (r - 0.5f);
                    g = 0.5f + conVal * (g - 0.5f);
                    b = 0.5f + conVal * (b - 0.5f);
                }

                // Apply gain
                r *= gainVal;
                g *= gainVal;
                b *= gainVal;

                // Apply offset
                r += offVal;
                g += offVal;
                b += offVal;

                // Apply gamma
                if (std::abs(invGamma - 1.0f) > 0.001f) {
                    r = (r > 0.0f) ? std::pow(r, invGamma) : 0.0f;
                    g = (g > 0.0f) ? std::pow(g, invGamma) : 0.0f;
                    b = (b > 0.0f) ? std::pow(b, invGamma) : 0.0f;
                }

                // Blend with original based on weight
                float origR = (rIdx >= 0) ? src[rIdx] / a : 0.0f;
                float origG = (gIdx >= 0) ? src[gIdx] / a : 0.0f;
                float origB = (bIdx >= 0) ? src[bIdx] / a : 0.0f;

                r = origR + weight * (r - origR);
                g = origG + weight * (g - origG);
                b = origB + weight * (b - origB);

                // Re-premultiply and write
                if (rIdx >= 0) dst[rIdx] = r * a;
                if (gIdx >= 0) dst[gIdx] = g * a;
                if (bIdx >= 0) dst[bIdx] = b * a;
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

#include "moc_DeepColorCorrect.cpp"
