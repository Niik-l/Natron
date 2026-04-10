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

#include "DeepRecolor.h"

#include <cassert>
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


DeepRecolor::DeepRecolor(NodePtr node)
    : EffectInstance(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepRecolor::~DeepRecolor()
{
}

std::string
DeepRecolor::getPluginDescription() const
{
    return tr("Replace the color (RGB) of deep image samples with color from a flat image input, "
              "while preserving the deep alpha and depth structure. This allows re-shading or "
              "re-lighting deep renders without re-rendering the deep data.\n\n"
              "Input 'Deep': The deep image whose structure (alpha, Z) is preserved.\n"
              "Input 'Color': A flat RGBA image whose RGB values are applied to each deep sample.\n\n"
              "Each deep sample's RGB is replaced with the flat image's RGB at that pixel, "
              "premultiplied by the sample's alpha. Alpha, Z, and ZBack are unchanged.").toStdString();
}

void
DeepRecolor::addAcceptedComponents(int /*inputNb*/,
                                   std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepRecolor::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepRecolor::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                            bool* /*defaultG*/,
                                            bool* /*defaultB*/,
                                            bool* /*defaultA*/) const
{
    return false;
}

void
DeepRecolor::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect a deep node to 'Deep' and a flat render to 'Color'.");
    page->addKnob(info);
}

StatusEnum
DeepRecolor::getRegionOfDefinition(U64 /*hash*/,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

DeepImagePtr
DeepRecolor::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepRecolor::render(const RenderActionArgs& args)
{
    // Get the deep image from input 0
    EffectInstancePtr deepInput = getInput(0);
    if (!deepInput) {
        return eStatusFailed;
    }

    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());
    if (!srcDeep) {
        return eStatusFailed;
    }

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    // Find channel indices in the deep image
    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");

    if (rIdx < 0 || gIdx < 0 || bIdx < 0 || aIdx < 0) {
        return eStatusFailed;
    }

    // Get the flat color image from input 1
    EffectInstancePtr colorInput = getInput(1);

    // FIX: Use RenderScale() (full res) instead of args.originalScale
    // Since this node declares eSupportsNo for render scale, we always
    // render at full resolution. args.originalScale may be 0.5 (proxy).
    ImagePtr colorImg;
    if (colorInput) {
        RectI roiPixel;
        colorImg = getImage(1, args.time, args.mappedScale, args.view,
                            NULL, NULL, false, false,
                            eStorageModeRAM, 0, &roiPixel);
    }

    // DEBUG logging
    if (colorInput && !colorImg) {
        setPersistentMessage(eMessageTypeWarning, "DeepRecolor: Failed to fetch color image from input 1.");
    } else if (colorImg) {
        clearPersistentMessage(false);
    }

    // Create a new deep image with the same structure
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    // Copy sample counts
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            result->setSampleCount(x, y, srcDeep->getSampleCount(x, y));
        }
    }
    result->allocateFromSampleCounts();

    // Get color image bounds for coordinate mapping
    RectI colorBounds;
    if (colorImg) {
        colorBounds = colorImg->getBounds();
    }

    // Recolor: copy all sample data, replacing RGB with flat image colors
    std::unique_ptr<Image::ReadAccess> colorRA;
    if (colorImg) {
        colorRA.reset(new Image::ReadAccess(colorImg.get()));
    }

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            if (nSamples == 0) {
                continue;
            }

            const float* srcData = srcDeep->getSampleData(x, y);
            float* dstData = result->getSampleData(x, y);

            // Map deep pixel coords to color image coords
            // Deep images from OIIO are top-down (y=0 at top), but Natron's
            // Image class is bottom-up (y=0 at bottom). We need to flip Y.
            float flatR = 0, flatG = 0, flatB = 0;
            bool hasColor = false;

            if (colorImg) {
                // Deep image pixels are in OIIO top-down order (y=0 at top).
                // Natron's flat Image is bottom-up (y=0 at bottom).
                // We use normalized coordinates to map between them, which
                // handles both the Y-flip and any resolution difference.
                float normX = (float)(x - dw.x1) / std::max(1, dw.width() - 1);
                float normY = (float)(y - dw.y1) / std::max(1, dw.height() - 1);

                // Flip Y: normY=0 in deep (top) maps to normY=1 in Natron (top)
                float flippedNormY = 1.0f - normY;

                int colorX = colorBounds.x1 + (int)(normX * (colorBounds.width() - 1) + 0.5f);
                int colorY = colorBounds.y1 + (int)(flippedNormY * (colorBounds.height() - 1) + 0.5f);

                // Clamp to color bounds
                colorX = std::max(colorBounds.x1, std::min(colorBounds.x2 - 1, colorX));
                colorY = std::max(colorBounds.y1, std::min(colorBounds.y2 - 1, colorY));

                const float* colorPix = (const float*)colorRA->pixelAt(colorX, colorY);
                if (colorPix) {
                    float flatA = colorPix[3];
                    if (flatA > 0.0001f) {
                        flatR = colorPix[0] / flatA;
                        flatG = colorPix[1] / flatA;
                        flatB = colorPix[2] / flatA;
                    } else {
                        flatR = colorPix[0];
                        flatG = colorPix[1];
                        flatB = colorPix[2];
                    }
                    hasColor = true;
                }
            }

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                // Copy all channels first (preserves Z, ZBack, any extras)
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Replace RGB with flat color, premultiplied by sample alpha
                if (hasColor) {
                    float sampleA = src[aIdx];
                    dst[rIdx] = flatR * sampleA;
                    dst[gIdx] = flatG * sampleA;
                    dst[bIdx] = flatB * sampleA;
                }
            }
        }
    }

    // Store the result for downstream nodes
    _lastDeepImage = result;

    // Also produce a flattened preview for the Viewer
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

#include "moc_DeepRecolor.cpp"
