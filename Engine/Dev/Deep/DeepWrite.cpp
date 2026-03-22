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

#include "DeepWrite.h"

#include <cassert>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "DeepUtils.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

#ifdef NATRON_HAVE_OPENIMAGEIO
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/deepdata.h>
#endif

NATRON_NAMESPACE_ENTER

struct DeepWritePrivate
{
    KnobFileWPtr filePath;
    KnobButtonWPtr renderButton;
    KnobStringWPtr info;
};


DeepWrite::DeepWrite(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepWritePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepWrite::~DeepWrite()
{
}

std::string
DeepWrite::getPluginDescription() const
{
    return tr("Write deep image data to a deep EXR file. "
              "Connect a deep-producing node to the input and set the output file path. "
              "The deep data (all samples per pixel) is written using OpenImageIO.").toStdString();
}

void
DeepWrite::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepWrite::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepWrite::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
DeepWrite::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobFilePtr filePath = AppManager::createKnob<KnobFile>(this, tr("File"));
    filePath->setName("filename");
    filePath->setHintToolTip(tr("Output file path for the deep EXR. Use .exr extension."));
    filePath->setAnimationEnabled(false);
    page->addKnob(filePath);
    _imp->filePath = filePath;

    KnobButtonPtr renderBtn = AppManager::createKnob<KnobButton>(this, tr("Write"));
    renderBtn->setName("render");
    renderBtn->setHintToolTip(tr("Write the deep image to disk."));
    page->addKnob(renderBtn);
    _imp->renderButton = renderBtn;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Set output path and click Write.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepWrite::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    if (_imp->renderButton.lock().get() == k) {
        // Trigger a render when the button is pressed
#ifdef NATRON_HAVE_OPENIMAGEIO
        EffectInstancePtr deepInput = getInput(0);
        DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

        if (!srcDeep) {
            setPersistentMessage(eMessageTypeError, tr("No deep image connected to input.").toStdString());
            return true;
        }

        std::string path = _imp->filePath.lock()->getValue();
        if (path.empty()) {
            setPersistentMessage(eMessageTypeError, tr("No output file path set.").toStdString());
            return true;
        }

        const RectI& dw = srcDeep->getDataWindow();
        int nChannels = srcDeep->getNumChannels();
        const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

        // Create OIIO output spec
        OIIO::ImageSpec spec(dw.width(), dw.height(), nChannels);
        spec.deep = true;
        spec.x = dw.x1;
        spec.y = dw.y1;
        spec.channelnames.clear();
        spec.channelformats.clear();
        for (int c = 0; c < nChannels; ++c) {
            spec.channelnames.push_back(chanNames[c]);
            spec.channelformats.push_back(OIIO::TypeDesc::FLOAT);
        }
        spec.attribute("compression", "zips");

        auto out = OIIO::ImageOutput::create(path);
        if (!out) {
            setPersistentMessage(eMessageTypeError,
                std::string("Could not create output: ") + OIIO::geterror());
            return true;
        }

        if (!out->open(path, spec)) {
            setPersistentMessage(eMessageTypeError,
                std::string("Could not open: ") + out->geterror());
            return true;
        }

        OIIO::DeepData deepdata;
        deepdata.init(spec);

        // Set sample counts
        for (int y = dw.y1; y < dw.y2; ++y) {
            for (int x = dw.x1; x < dw.x2; ++x) {
                int pixel = (y - dw.y1) * dw.width() + (x - dw.x1);
                deepdata.set_samples(pixel, srcDeep->getSampleCount(x, y));
            }
        }

        // Copy sample data
        for (int y = dw.y1; y < dw.y2; ++y) {
            for (int x = dw.x1; x < dw.x2; ++x) {
                int pixel = (y - dw.y1) * dw.width() + (x - dw.x1);
                int nSamples = srcDeep->getSampleCount(x, y);
                if (nSamples == 0) continue;

                const float* srcData = srcDeep->getSampleData(x, y);
                for (int s = 0; s < nSamples; ++s) {
                    for (int c = 0; c < nChannels; ++c) {
                        deepdata.set_deep_value(pixel, c, s, srcData[s * nChannels + c]);
                    }
                }
            }
        }

        if (!out->write_deep_scanlines(0, dw.height(), 0, deepdata)) {
            setPersistentMessage(eMessageTypeError,
                std::string("Error writing: ") + out->geterror());
            return true;
        }

        out->close();

        std::ostringstream ss;
        ss << "Written: " << path << " (" << dw.width() << "x" << dw.height()
           << ", " << srcDeep->totalSamples() << " samples)";
        KnobStringPtr infoKnob = _imp->info.lock();
        if (infoKnob) {
            infoKnob->setValue(ss.str());
        }
        clearPersistentMessage(false);
#else
        setPersistentMessage(eMessageTypeError, tr("DeepWrite requires OpenImageIO.").toStdString());
#endif
        return true;
    }
    return false;
}

StatusEnum
DeepWrite::getRegionOfDefinition(U64 /*hash*/,
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

StatusEnum
DeepWrite::render(const RenderActionArgs& args)
{
    // Pass through the flattened preview from the deep input
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (srcDeep && outImg) {
        srcDeep->flattenToImage(outImg.get());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepWrite.cpp"
