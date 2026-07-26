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

#include "DeepUtils.h"

#include "DeepImage.h"
#ifdef NATRON_HAVE_OPENIMAGEIO
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/deepdata.h>
#endif

#include "DeepColorCorrect.h"
#include "DeepCrop.h"
#include "DeepExpression.h"
#include "DeepFromImage.h"
#include "DeepGrade.h"
#include "DeepHoldout.h"
#include "DeepMerge.h"
#include "DeepRead.h"
#include "DeepRecolor.h"
#include "DeepReformat.h"
#include "DeepSlice.h"
#include "DeepSplit.h"
#include "DeepPositionMatte.h"
#include "DeepFog.h"
#include "DeepAO.h"
#include "DeepRelight.h"
#include "DeepContactShadow.h"
#include "DeepConsolidate.h"
#include "DeepDepthWarp.h"
#include "DeepSampleFilter.h"
#include "DeepNormalize.h"
#include "DeepChannelMath.h"
#include "DeepQuantize.h"
#include "DeepNormalMatte.h"
#include "DeepVelocityMatte.h"
#include "DeepFromFrames.h"
#include "DeepBlend.h"
#include "DeepTransform.h"
#include "Blast.h"
#ifdef NATRON_CYCLES
#include "../Cycles/CyclesRender.h"
#include "../Scene3D/CyclesRenderPass.h"
#endif

NATRON_NAMESPACE_ENTER

DeepImagePtr
getDeepImageFromEffect(EffectInstance* effect)
{
    if (!effect) {
        return DeepImagePtr();
    }

    // Try each known deep node type.
    // When adding a new deep node that produces deep output via getDeepImage(),
    // add it to this list.

    if (DeepRead* n = dynamic_cast<DeepRead*>(effect)) return n->getDeepImage();
    if (DeepMerge* n = dynamic_cast<DeepMerge*>(effect)) return n->getDeepImage();
    if (DeepRecolor* n = dynamic_cast<DeepRecolor*>(effect)) return n->getDeepImage();
    if (DeepSlice* n = dynamic_cast<DeepSlice*>(effect)) return n->getDeepImage();
    if (DeepGrade* n = dynamic_cast<DeepGrade*>(effect)) return n->getDeepImage();
    if (DeepHoldout* n = dynamic_cast<DeepHoldout*>(effect)) return n->getDeepImage();
    if (DeepFromImage* n = dynamic_cast<DeepFromImage*>(effect)) return n->getDeepImage();
    if (DeepReformat* n = dynamic_cast<DeepReformat*>(effect)) return n->getDeepImage();
    if (DeepCrop* n = dynamic_cast<DeepCrop*>(effect)) return n->getDeepImage();
    if (DeepTransform* n = dynamic_cast<DeepTransform*>(effect)) return n->getDeepImage();
    if (DeepExpression* n = dynamic_cast<DeepExpression*>(effect)) return n->getDeepImage();
    if (DeepColorCorrect* n = dynamic_cast<DeepColorCorrect*>(effect)) return n->getDeepImage();
    if (DeepSplit* n = dynamic_cast<DeepSplit*>(effect)) return n->getDeepImage();
    if (DeepPositionMatte* n = dynamic_cast<DeepPositionMatte*>(effect)) return n->getDeepImage();
    if (DeepFog* n = dynamic_cast<DeepFog*>(effect)) return n->getDeepImage();
    if (DeepAO* n = dynamic_cast<DeepAO*>(effect)) return n->getDeepImage();
    if (DeepRelight* n = dynamic_cast<DeepRelight*>(effect)) return n->getDeepImage();
    if (DeepContactShadow* n = dynamic_cast<DeepContactShadow*>(effect)) return n->getDeepImage();
    if (DeepConsolidate* n = dynamic_cast<DeepConsolidate*>(effect)) return n->getDeepImage();
    if (DeepDepthWarp* n = dynamic_cast<DeepDepthWarp*>(effect)) return n->getDeepImage();
    if (DeepSampleFilter* n = dynamic_cast<DeepSampleFilter*>(effect)) return n->getDeepImage();
    if (DeepNormalize* n = dynamic_cast<DeepNormalize*>(effect)) return n->getDeepImage();
    if (DeepChannelMath* n = dynamic_cast<DeepChannelMath*>(effect)) return n->getDeepImage();
    if (DeepQuantize* n = dynamic_cast<DeepQuantize*>(effect)) return n->getDeepImage();
    if (DeepNormalMatte* n = dynamic_cast<DeepNormalMatte*>(effect)) return n->getDeepImage();
    if (DeepVelocityMatte* n = dynamic_cast<DeepVelocityMatte*>(effect)) return n->getDeepImage();
    if (DeepFromFrames* n = dynamic_cast<DeepFromFrames*>(effect)) return n->getDeepImage();
    if (DeepBlend* n = dynamic_cast<DeepBlend*>(effect)) return n->getDeepImage();
    // Blast: deep passthrough minus blasted samples (cloud must originate
    // from a DeepToPoints — returns null otherwise)
    if (Blast* n = dynamic_cast<Blast*>(effect)) return n->getDeepImage();
#ifdef NATRON_CYCLES
    // CyclesRender: native deep render output (Deep Output knob)
    if (CyclesRender* n = dynamic_cast<CyclesRender*>(effect)) return n->getDeepImage();
    if (CyclesRenderPass* n = dynamic_cast<CyclesRenderPass*>(effect)) return n->getDeepImage();
#endif

    return DeepImagePtr();
}

bool
writeDeepImageEXR(const DeepImagePtr& deep, const std::string& path,
                  std::string* errOut,
                  const std::string& compression)
{
#ifdef NATRON_HAVE_OPENIMAGEIO
    if (!deep) {
        if (errOut) *errOut = "No deep image to write.";
        return false;
    }
    if (path.empty()) {
        if (errOut) *errOut = "No output file path set.";
        return false;
    }

    const RectI& dw = deep->getDataWindow();
    const int nChannels = deep->getNumChannels();
    const std::vector<std::string>& chanNames = deep->getChannelNames();

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
    spec.attribute("compression", compression.empty() ? "zips" : compression.c_str());

    auto out = OIIO::ImageOutput::create(path);
    if (!out) {
        if (errOut) *errOut = std::string("Could not create output: ") + OIIO::geterror();
        return false;
    }
    if (!out->open(path, spec)) {
        if (errOut) *errOut = std::string("Could not open: ") + out->geterror();
        return false;
    }

    OIIO::DeepData deepdata;
    deepdata.init(spec);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            const int pixel = (y - dw.y1) * dw.width() + (x - dw.x1);
            deepdata.set_samples(pixel, deep->getSampleCount(x, y));
        }
    }
    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            const int pixel = (y - dw.y1) * dw.width() + (x - dw.x1);
            const int nSamples = deep->getSampleCount(x, y);
            if (nSamples == 0) continue;
            const float* srcData = deep->getSampleData(x, y);
            for (int s = 0; s < nSamples; ++s) {
                for (int c = 0; c < nChannels; ++c) {
                    deepdata.set_deep_value(pixel, c, s, srcData[s * nChannels + c]);
                }
            }
        }
    }

    if (!out->write_deep_scanlines(0, dw.height(), 0, deepdata)) {
        if (errOut) *errOut = std::string("Error writing: ") + out->geterror();
        out->close();
        return false;
    }
    out->close();
    return true;
#else
    (void)deep; (void)path;
    if (errOut) *errOut = "Deep EXR writing requires OpenImageIO.";
    return false;
#endif
}

NATRON_NAMESPACE_EXIT
