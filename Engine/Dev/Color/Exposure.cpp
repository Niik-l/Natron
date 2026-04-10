/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "Exposure.h"

#include <cmath>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

struct ExposurePrivate
{
    KnobDoubleWPtr master;   // master exposure in stops
    KnobDoubleWPtr red;      // per-channel offset in stops
    KnobDoubleWPtr green;
    KnobDoubleWPtr blue;
    KnobDoubleWPtr mix;
};

Exposure::Exposure(NodePtr node)
    : EffectInstance(node)
    , _imp(new ExposurePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

Exposure::~Exposure()
{
}

std::string
Exposure::getPluginDescription() const
{
    return "Adjust image exposure in stops (f-stops).\n\n"
           "Each channel is multiplied by 2^(master + channel_offset).\n"
           "1 stop = double the brightness. -1 stop = half the brightness.\n\n"
           "Per-channel R/G/B offsets allow independent color balance in exposure space.";
}

void
Exposure::addAcceptedComponents(int /*inputNb*/,
                                std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
Exposure::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Exposure::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Exposure::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Exposure"));
        k->setName("exposure"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-6.0); k->setDisplayMaximum(6.0);
        k->setHintToolTip(tr("Master exposure adjustment in stops. "
                              "1.0 = double brightness, -1.0 = half brightness."));
        page->addKnob(k); _imp->master = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Red"));
        k->setName("red"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-4.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Red channel exposure offset in stops (added to master)."));
        page->addKnob(k); _imp->red = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Green"));
        k->setName("green"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-4.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Green channel exposure offset in stops (added to master)."));
        page->addKnob(k); _imp->green = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Blue"));
        k->setName("blue"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-4.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Blue channel exposure offset in stops (added to master)."));
        page->addKnob(k); _imp->blue = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Blend between original (0) and adjusted (1) image."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
Exposure::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
}

StatusEnum
Exposure::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;

    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    double master = _imp->master.lock()->getValueAtTime(args.time);
    double rOff   = _imp->red.lock()->getValueAtTime(args.time);
    double gOff   = _imp->green.lock()->getValueAtTime(args.time);
    double bOff   = _imp->blue.lock()->getValueAtTime(args.time);
    double mix    = _imp->mix.lock()->getValueAtTime(args.time);

    // Precompute gain per channel: gain = 2^(master + offset)
    float gainR = (float)std::pow(2.0, master + rOff);
    float gainG = (float)std::pow(2.0, master + gOff);
    float gainB = (float)std::pow(2.0, master + bOff);

    RectI srcBounds = srcImg->getBounds();
    RectI outBounds = outImg->getBounds();
    int srcNComp = srcImg->getComponents().getNumComponents();
    int nComp = std::min(srcNComp, 4);

    Image::ReadAccess srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());

    float fmix = (float)mix;
    float invMix = 1.0f - fmix;

    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            const float* src = nullptr;
            if (x >= srcBounds.x1 && x < srcBounds.x2 &&
                y >= srcBounds.y1 && y < srcBounds.y2) {
                src = (const float*)srcRa.pixelAt(x, y);
            }

            if (!src) {
                for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                continue;
            }

            if (nComp >= 3) {
                float outR = src[0] * gainR;
                float outG = src[1] * gainG;
                float outB = src[2] * gainB;

                if (fmix < 1.0f) {
                    outR = outR * fmix + src[0] * invMix;
                    outG = outG * fmix + src[1] * invMix;
                    outB = outB * fmix + src[2] * invMix;
                }

                dst[0] = outR;
                dst[1] = outG;
                dst[2] = outB;
                if (nComp >= 4) dst[3] = src[3]; // alpha passthrough
            } else {
                for (int c = 0; c < nComp; ++c) dst[c] = src[c];
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
