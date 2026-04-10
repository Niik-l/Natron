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

#include "ColorMatrix.h"
#include "LinearAlgebra.h"

#include <cmath>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

struct ColorMatrixPrivate
{
    // 3x3 color matrix: row = output channel, col = input channel
    // outR = Rr*inR + Rg*inG + Rb*inB
    // outG = Gr*inR + Gg*inG + Gb*inB
    // outB = Br*inR + Bg*inG + Bb*inB
    KnobColorWPtr outputRed;   // 3-component: Rr, Rg, Rb
    KnobColorWPtr outputGreen; // 3-component: Gr, Gg, Gb
    KnobColorWPtr outputBlue;  // 3-component: Br, Bg, Bb

    KnobBoolWPtr invert;
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;
    KnobDoubleWPtr mix;
};

ColorMatrix::ColorMatrix(NodePtr node)
    : EffectInstance(node)
    , _imp(new ColorMatrixPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

ColorMatrix::~ColorMatrix()
{
}

std::string
ColorMatrix::getPluginDescription() const
{
    return "Apply a 3x3 color matrix to RGB channels.\n\n"
           "Each output channel is a linear combination of input RGB. "
           "Use Invert to reverse the transform (e.g., to undo a color space conversion).\n\n"
           "Mix blends between the original and transformed image.";
}

void
ColorMatrix::addAcceptedComponents(int /*inputNb*/,
                                   std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
ColorMatrix::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ColorMatrix::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ColorMatrix::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    // Output Red row (3-component color knob: r, g, b)
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Output Red"), 3);
        k->setName("outputRed"); k->setAnimationEnabled(true);
        k->setDefaultValue(1.0, 0); k->setDefaultValue(0.0, 1); k->setDefaultValue(0.0, 2);
        k->setDisplayMinimum(-2.0, 0); k->setDisplayMaximum(2.0, 0);
        k->setDisplayMinimum(-2.0, 1); k->setDisplayMaximum(2.0, 1);
        k->setDisplayMinimum(-2.0, 2); k->setDisplayMaximum(2.0, 2);
        page->addKnob(k); _imp->outputRed = k;
    }

    // Output Green row
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Output Green"), 3);
        k->setName("outputGreen"); k->setAnimationEnabled(true);
        k->setDefaultValue(0.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(0.0, 2);
        k->setDisplayMinimum(-2.0, 0); k->setDisplayMaximum(2.0, 0);
        k->setDisplayMinimum(-2.0, 1); k->setDisplayMaximum(2.0, 1);
        k->setDisplayMinimum(-2.0, 2); k->setDisplayMaximum(2.0, 2);
        page->addKnob(k); _imp->outputGreen = k;
    }

    // Output Blue row
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Output Blue"), 3);
        k->setName("outputBlue"); k->setAnimationEnabled(true);
        k->setDefaultValue(0.0, 0); k->setDefaultValue(0.0, 1); k->setDefaultValue(1.0, 2);
        k->setDisplayMinimum(-2.0, 0); k->setDisplayMaximum(2.0, 0);
        k->setDisplayMinimum(-2.0, 1); k->setDisplayMaximum(2.0, 1);
        k->setDisplayMinimum(-2.0, 2); k->setDisplayMaximum(2.0, 2);
        page->addKnob(k); _imp->outputBlue = k;
    }

    // Separator
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    // Invert
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert"));
        k->setName("invert"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Compute and apply the inverse of the matrix. "
                              "Use this to reverse a color transform."));
        page->addKnob(k); _imp->invert = k;
    }

    // Clamp
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Clamp Black"));
        k->setName("clampBlack"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Clamp negative output values to 0."));
        page->addKnob(k); _imp->clampBlack = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Clamp White"));
        k->setName("clampWhite"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Clamp output values above 1.0."));
        k->setAddNewLine(false);
        page->addKnob(k); _imp->clampWhite = k;
    }

    // Mix
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Blend between original (0) and transformed (1) image."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
ColorMatrix::getRegionOfDefinition(U64 /*hash*/,
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
ColorMatrix::render(const RenderActionArgs& args)
{
    // Get source image
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, true,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;

    // Get output image
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    // Read matrix from knobs
    ColorChartMath::Mat3 M;
    {
        KnobColorPtr rk = _imp->outputRed.lock();
        KnobColorPtr gk = _imp->outputGreen.lock();
        KnobColorPtr bk = _imp->outputBlue.lock();
        for (int c = 0; c < 3; ++c) {
            M.m[0][c] = rk->getValueAtTime(args.time, c);
            M.m[1][c] = gk->getValueAtTime(args.time, c);
            M.m[2][c] = bk->getValueAtTime(args.time, c);
        }
    }

    bool invert = _imp->invert.lock()->getValueAtTime(args.time);
    bool doClampBlack = _imp->clampBlack.lock()->getValueAtTime(args.time);
    bool doClampWhite = _imp->clampWhite.lock()->getValueAtTime(args.time);
    double mix = _imp->mix.lock()->getValueAtTime(args.time);

    // Invert the matrix if requested
    if (invert) {
        ColorChartMath::Mat3 inv;
        if (!ColorChartMath::mat3Invert(M, inv)) {
            // Singular matrix — pass through unchanged
            setPersistentMessage(eMessageTypeWarning, "Matrix is singular and cannot be inverted.");
            // Fall through with identity
            M = ColorChartMath::Mat3(); // identity
        } else {
            clearPersistentMessage(false);
            M = inv;
        }
    }

    RectI srcBounds = srcImg->getBounds();
    RectI outBounds = outImg->getBounds();
    int srcNComp = srcImg->getComponents().getNumComponents();
    int nComp = std::min(srcNComp, 4);

    Image::ReadAccess srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());

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
                float r = src[0], g = src[1], b = src[2];
                float outR = (float)(M.m[0][0] * r + M.m[0][1] * g + M.m[0][2] * b);
                float outG = (float)(M.m[1][0] * r + M.m[1][1] * g + M.m[1][2] * b);
                float outB = (float)(M.m[2][0] * r + M.m[2][1] * g + M.m[2][2] * b);

                if (doClampBlack) {
                    outR = std::max(outR, 0.0f);
                    outG = std::max(outG, 0.0f);
                    outB = std::max(outB, 0.0f);
                }
                if (doClampWhite) {
                    outR = std::min(outR, 1.0f);
                    outG = std::min(outG, 1.0f);
                    outB = std::min(outB, 1.0f);
                }

                // Mix
                if (mix < 1.0) {
                    float invMix = 1.0f - (float)mix;
                    outR = outR * (float)mix + r * invMix;
                    outG = outG * (float)mix + g * invMix;
                    outB = outB * (float)mix + b * invMix;
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
