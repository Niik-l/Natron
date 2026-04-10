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

#include "DeepPositionMatte.h"

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
#include "../../ChoiceOption.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER


struct DeepPositionMattePrivate
{
    KnobChoiceWPtr shape;
    KnobDoubleWPtr centerX;
    KnobDoubleWPtr centerY;
    KnobDoubleWPtr centerZ;
    KnobDoubleWPtr sizeX;
    KnobDoubleWPtr sizeY;
    KnobDoubleWPtr sizeZ;
    KnobDoubleWPtr falloff;
    KnobBoolWPtr invert;
    KnobStringWPtr info;
};


DeepPositionMatte::DeepPositionMatte(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepPositionMattePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepPositionMatte::~DeepPositionMatte()
{
}

std::string
DeepPositionMatte::getPluginDescription() const
{
    return tr("Generate a matte from deep sample 3D positions using sphere, cube, or cylinder shapes. "
              "Each sample's position is derived from its pixel XY and depth Z. "
              "Useful for isolating objects by spatial location.\n\n"
              "Samples inside the shape retain their alpha; samples outside have alpha reduced to zero. "
              "The falloff parameter creates a smooth transition at the shape boundary.").toStdString();
}

void
DeepPositionMatte::addAcceptedComponents(int /*inputNb*/,
                                         std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DeepPositionMatte::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepPositionMatte::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                                  bool* /*defaultG*/,
                                                  bool* /*defaultB*/,
                                                  bool* /*defaultA*/) const
{
    return false;
}

void
DeepPositionMatte::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobChoicePtr shape = AppManager::createKnob<KnobChoice>(this, tr("Shape"));
    shape->setName("shape");
    shape->setHintToolTip(tr("The geometric shape used for position testing. "
                             "Sphere: ellipsoidal distance. "
                             "Cube: axis-aligned box (Chebyshev distance). "
                             "Cylinder: cylindrical along Y axis."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Sphere", "Sphere", ""));
        entries.push_back(ChoiceOption("Cube", "Cube", ""));
        entries.push_back(ChoiceOption("Cylinder", "Cylinder", ""));
        shape->populateChoices(entries);
    }
    shape->setDefaultValue(0);
    page->addKnob(shape);
    _imp->shape = shape;

    KnobDoublePtr centerX = AppManager::createKnob<KnobDouble>(this, tr("Center X"));
    centerX->setName("centerX");
    centerX->setHintToolTip(tr("X coordinate of the shape center in pixel space."));
    centerX->setAnimationEnabled(true);
    centerX->setDefaultValue(960.0);
    page->addKnob(centerX);
    _imp->centerX = centerX;

    KnobDoublePtr centerY = AppManager::createKnob<KnobDouble>(this, tr("Center Y"));
    centerY->setName("centerY");
    centerY->setHintToolTip(tr("Y coordinate of the shape center in pixel space."));
    centerY->setAnimationEnabled(true);
    centerY->setDefaultValue(540.0);
    page->addKnob(centerY);
    _imp->centerY = centerY;

    KnobDoublePtr centerZ = AppManager::createKnob<KnobDouble>(this, tr("Center Z"));
    centerZ->setName("centerZ");
    centerZ->setHintToolTip(tr("Z depth coordinate of the shape center."));
    centerZ->setAnimationEnabled(true);
    centerZ->setDefaultValue(50.0);
    page->addKnob(centerZ);
    _imp->centerZ = centerZ;

    KnobDoublePtr sizeX = AppManager::createKnob<KnobDouble>(this, tr("Size X"));
    sizeX->setName("sizeX");
    sizeX->setHintToolTip(tr("Half-extent of the shape along the X axis in pixel space."));
    sizeX->setAnimationEnabled(true);
    sizeX->setDefaultValue(100.0);
    sizeX->setMinimum(0.001);
    page->addKnob(sizeX);
    _imp->sizeX = sizeX;

    KnobDoublePtr sizeY = AppManager::createKnob<KnobDouble>(this, tr("Size Y"));
    sizeY->setName("sizeY");
    sizeY->setHintToolTip(tr("Half-extent of the shape along the Y axis in pixel space."));
    sizeY->setAnimationEnabled(true);
    sizeY->setDefaultValue(100.0);
    sizeY->setMinimum(0.001);
    page->addKnob(sizeY);
    _imp->sizeY = sizeY;

    KnobDoublePtr sizeZ = AppManager::createKnob<KnobDouble>(this, tr("Size Z"));
    sizeZ->setName("sizeZ");
    sizeZ->setHintToolTip(tr("Half-extent of the shape along the Z (depth) axis."));
    sizeZ->setAnimationEnabled(true);
    sizeZ->setDefaultValue(50.0);
    sizeZ->setMinimum(0.001);
    page->addKnob(sizeZ);
    _imp->sizeZ = sizeZ;

    KnobDoublePtr falloff = AppManager::createKnob<KnobDouble>(this, tr("Falloff"));
    falloff->setName("falloff");
    falloff->setHintToolTip(tr("Falloff distance beyond the shape boundary. "
                               "0 means a hard edge. Larger values create a smooth transition "
                               "from inside (matte=1) to outside (matte=0)."));
    falloff->setAnimationEnabled(true);
    falloff->setDefaultValue(0.0);
    falloff->setMinimum(0.0);
    page->addKnob(falloff);
    _imp->falloff = falloff;

    KnobBoolPtr invert = AppManager::createKnob<KnobBool>(this, tr("Invert"));
    invert->setName("invert");
    invert->setHintToolTip(tr("Invert the matte. When enabled, samples inside the shape are "
                              "attenuated and samples outside are preserved."));
    invert->setDefaultValue(false);
    page->addKnob(invert);
    _imp->invert = invert;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Configure shape and center to generate a position matte.");
    page->addKnob(info);
    _imp->info = info;
}

bool
DeepPositionMatte::knobChanged(KnobI* k,
                               ValueChangedReasonEnum /*reason*/,
                               ViewSpec /*view*/,
                               double /*time*/,
                               bool /*originatedFromMainThread*/)
{
    if (_imp->shape.lock().get() == k ||
        _imp->centerX.lock().get() == k ||
        _imp->centerY.lock().get() == k ||
        _imp->centerZ.lock().get() == k ||
        _imp->sizeX.lock().get() == k ||
        _imp->sizeY.lock().get() == k ||
        _imp->sizeZ.lock().get() == k ||
        _imp->falloff.lock().get() == k ||
        _imp->invert.lock().get() == k) {
        return true;
    }
    return false;
}

StatusEnum
DeepPositionMatte::getRegionOfDefinition(U64 /*hash*/,
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
DeepPositionMatte::getDeepImage() const
{
    return _lastDeepImage;
}

StatusEnum
DeepPositionMatte::render(const RenderActionArgs& args)
{
    EffectInstancePtr deepInput = getInput(0);
    DeepImagePtr srcDeep = getDeepImageFromEffect(deepInput.get());

    if (!srcDeep) {
        return eStatusFailed;
    }

    int shapeIdx = _imp->shape.lock()->getValue();
    double cx = _imp->centerX.lock()->getValue();
    double cy = _imp->centerY.lock()->getValue();
    double cz = _imp->centerZ.lock()->getValue();
    double sx = _imp->sizeX.lock()->getValue();
    double sy = _imp->sizeY.lock()->getValue();
    double sz = _imp->sizeZ.lock()->getValue();
    double falloffVal = _imp->falloff.lock()->getValue();
    bool invertMatte = _imp->invert.lock()->getValue();

    const RectI& dw = srcDeep->getDataWindow();
    int nChannels = srcDeep->getNumChannels();
    const std::vector<std::string>& chanNames = srcDeep->getChannelNames();

    int rIdx = srcDeep->findChannelIndex("R");
    int gIdx = srcDeep->findChannelIndex("G");
    int bIdx = srcDeep->findChannelIndex("B");
    int aIdx = srcDeep->findChannelIndex("A");
    int zIdx = srcDeep->findChannelIndex("Z");

    if (zIdx < 0) {
        // No Z channel — can't compute position matte, pass through
        _lastDeepImage = srcDeep;
        return eStatusOK;
    }

    // Average size for falloff normalization
    double avgSize = (sx + sy + sz) / 3.0;

    // First pass: all samples are kept (sample count unchanged)
    DeepImagePtr result = std::make_shared<DeepImage>(dw, nChannels, chanNames);

    for (int y = dw.y1; y < dw.y2; ++y) {
        for (int x = dw.x1; x < dw.x2; ++x) {
            int nSamples = srcDeep->getSampleCount(x, y);
            result->setSampleCount(x, y, nSamples);
        }
    }

    result->allocateFromSampleCounts();

    // Second pass: copy samples and modify alpha/RGB by matte value
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

            for (int s = 0; s < nSamples; ++s) {
                const float* src = srcData + s * nChannels;
                float* dst = dstData + s * nChannels;

                // Copy all channels first
                for (int c = 0; c < nChannels; ++c) {
                    dst[c] = src[c];
                }

                // Compute 3D position of this sample
                double px = static_cast<double>(x);
                double py = static_cast<double>(y);
                double pz = static_cast<double>(src[zIdx]);

                // Normalized distances from center
                double dx = (px - cx) / sx;
                double dy = (py - cy) / sy;
                double dz = (pz - cz) / sz;

                // Compute distance based on shape
                double dist = 0.0;
                switch (shapeIdx) {
                    case 0: // Sphere
                        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        break;
                    case 1: // Cube
                        dist = std::max(std::max(std::fabs(dx), std::fabs(dy)), std::fabs(dz));
                        break;
                    case 2: // Cylinder (along Y axis)
                        dist = std::max(std::sqrt(dx * dx + dz * dz), std::fabs(dy));
                        break;
                    default:
                        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        break;
                }

                // Compute matte value
                double matte = 0.0;
                if (dist <= 1.0) {
                    matte = 1.0;
                } else if (falloffVal > 0.0 && avgSize > 0.0) {
                    double falloffNorm = falloffVal / avgSize;
                    if (falloffNorm > 0.0) {
                        matte = 1.0 - (dist - 1.0) / falloffNorm;
                        if (matte < 0.0) {
                            matte = 0.0;
                        }
                    }
                }

                // Invert if requested
                if (invertMatte) {
                    matte = 1.0 - matte;
                }

                // Apply matte to alpha and premultiplied RGB
                float matteF = static_cast<float>(matte);
                if (aIdx >= 0) {
                    dst[aIdx] *= matteF;
                }
                if (rIdx >= 0) {
                    dst[rIdx] *= matteF;
                }
                if (gIdx >= 0) {
                    dst[gIdx] *= matteF;
                }
                if (bIdx >= 0) {
                    dst[bIdx] *= matteF;
                }
            }
        }
    }

    // Update info
    std::ostringstream ss;
    const char* shapeNames[] = {"Sphere", "Cube", "Cylinder"};
    ss << "Shape: " << (shapeIdx >= 0 && shapeIdx <= 2 ? shapeNames[shapeIdx] : "Unknown")
       << " | Center: (" << cx << ", " << cy << ", " << cz << ")"
       << " | Size: (" << sx << ", " << sy << ", " << sz << ")";
    if (falloffVal > 0.0) {
        ss << " | Falloff: " << falloffVal;
    }
    if (invertMatte) {
        ss << " | Inverted";
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

#include "moc_DeepPositionMatte.cpp"
