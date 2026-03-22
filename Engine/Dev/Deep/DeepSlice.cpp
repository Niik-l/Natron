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

#include "DeepSlice.h"

#include <cassert>
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


struct DeepSlicePrivate
{
    KnobDoubleWPtr nearClip;
    KnobDoubleWPtr farClip;
    KnobStringWPtr info;
};


DeepSlice::DeepSlice(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepSlicePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepSlice::~DeepSlice()
{
}

std::string
DeepSlice::getPluginDescription() const
{
    return tr("Extract deep samples within a Z depth range. "
              "Only samples whose Z value falls within [Near Clip, Far Clip] are kept. "
              "Samples outside this range are discarded.\n\n"
              "Use this to isolate objects at specific depths in a deep image.").toStdString();
}

void
DeepSlice::addAcceptedComponents(int /*inputNb*/,
                                 std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepSlice::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepSlice::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                          bool* /*defaultG*/,
                                          bool* /*defaultB*/,
                                          bool* /*defaultA*/) const
{
    return false;
}

void
DeepSlice::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr nearClip = AppManager::createKnob<KnobDouble>(this, tr("Near Clip"));
    nearClip->setName("nearClip");
    nearClip->setHintToolTip(tr("Minimum Z depth. Samples with Z less than this value are discarded."));
    nearClip->setAnimationEnabled(true);
    nearClip->setDefaultValue(0.0);
    nearClip->setMinimum(0.0);
    page->addKnob(nearClip);
    _imp->nearClip = nearClip;

    KnobDoublePtr farClip = AppManager::createKnob<KnobDouble>(this, tr("Far Clip"));
    farClip->setName("farClip");
    farClip->setHintToolTip(tr("Maximum Z depth. Samples with Z greater than this value are discarded. "
                               "Set to 0 for unlimited (no far clip)."));
    farClip->setAnimationEnabled(true);
    farClip->setDefaultValue(0.0);
    farClip->setMinimum(0.0);
    page->addKnob(farClip);
    _imp->farClip = farClip;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set Near/Far Clip to slice the deep image by depth.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepSlice::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    if (_imp->nearClip.lock().get() == k || _imp->farClip.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepSlice::getRegionOfDefinition(U64 /*hash*/,
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
DeepSlice::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepSlice::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    double nearClipVal = _imp->nearClip.lock()->getValue();
    double farClipVal = _imp->farClip.lock()->getValue();

    // If farClip is 0, treat as unlimited
    if (farClipVal <= 0.0) {
        farClipVal = std::numeric_limits<double>::max();
    }

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int zIdx = srcDeep->findChannelIndex("Z");
    if (zIdx < 0) {
        // No Z channel — can't slice, pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // First pass: count how many samples survive per pixel
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                result->setSampleCount(x, y, 0);
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);
            int kept = 0;
            for (int s = 0; s < nSamples; ++s) {
                float z = srcData[s * nChannels + zIdx];
                if (z >= nearClipVal && z <= farClipVal) {
                    ++kept;
                }
            }
            result->setSampleCount(x, y, kept);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy surviving samples
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

            int outIdx = 0;
            for (int s = 0; s < nSamples; ++s) {
                float z = srcData[s * nChannels + zIdx];
                if (z >= nearClipVal && z <= farClipVal) {
                    const float* src = srcData + s * nChannels;
                    float* dst = dstData + outIdx * nChannels;
                    for (int c = 0; c < nChannels; ++c) {
                        dst[c] = src[c];
                    }
                    ++outIdx;
                }
            }
        }
    }

    // Update info
    std::ostringstream ss;
    ss << "Samples kept: near=" << nearClipVal << " far=";
    if (farClipVal >= std::numeric_limits<double>::max() / 2.0) {
        ss << "unlimited";
    } else {
        ss << farClipVal;
    }
    ss << " | Total samples: " << result->totalSamples();
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

#include "moc_DeepSlice.cpp"
