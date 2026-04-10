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

#include "SphericalTransform.h"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <chrono>

#include "SphericalProjections.h"
#include "../../AppInstance.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

using namespace SphericalProjections;

// ---- Projection choice labels ----
// Projection choices — matches Nuke's dropdown order
enum UIProjection {
    eUIProjectionLatLong = 0,
    eUIProjectionCubemap,
    eUIProjectionRectilinear,
    eUIProjectionFisheye,
    eUIProjectionMirrorBall,
    eUIProjectionCount
};

static const char* kProjectionLabels[] = {
    "LatLong",
    "Cubemap",
    "Rectilinear",
    "Fisheye",
    "Mirror Ball",
};
static const int kNumProjections = eUIProjectionCount;

static const char* kCubemapPackingLabels[] = {
    "LL-Cross",
    "6x1",
    "3x2",
};

// Fisheye sub-type choices — matches Nuke's "Type" dropdown
enum UIFisheyeType {
    eUIFisheyeEquidistant = 0,
    eUIFisheyeEquisolid,
    eUIFisheyeStereographic,
    eUIFisheyeOrthographic,
    eUIFisheyeCount
};

static const char* kFisheyeTypeLabels[] = {
    "Equidistant",
    "Equisolid",
    "Stereographic",
    "Orthographic",
};

// Map UI projection + fisheye type to internal ProjectionType
static ProjectionType mapUIToProjection(int uiProj, int fisheyeType)
{
    switch (uiProj) {
    case eUIProjectionLatLong:      return eProjectionLatLong;
    case eUIProjectionCubemap:      return eProjectionCubemap;
    case eUIProjectionRectilinear:  return eProjectionRectilinear;
    case eUIProjectionFisheye:
        switch (fisheyeType) {
        case eUIFisheyeEquidistant:   return eProjectionFisheyeEquidistant;
        case eUIFisheyeEquisolid:     return eProjectionFisheyeEquisolid;
        case eUIFisheyeStereographic: return eProjectionFisheyeStereographic;
        case eUIFisheyeOrthographic:  return eProjectionFisheyeOrthographic;
        default:                       return eProjectionFisheyeEquidistant;
        }
    case eUIProjectionMirrorBall:   return eProjectionMirrorBall;
    default:                         return eProjectionLatLong;
    }
}

// Filter types
enum FilterType {
    eFilterImpulse = 0,
    eFilterBilinear,
    eFilterCubic,
    eFilterMitchell,
};

// ---- Private data ----

// Rotation modes — matches Nuke's dropdown
enum RotationMode {
    eRotationModeLook = 0,
    eRotationModePanTiltRoll,
    eRotationModeRotationAngles,
    eRotationModeCount
};

static const char* kRotationModeLabels[] = {
    "Look",
    "Pan-Tilt-Roll",
    "Rotation Angles",
};

// Rotation orders for Rotation Angles mode
enum RotationOrder {
    eRotOrderXYZ = 0, eRotOrderXZY, eRotOrderYXZ,
    eRotOrderYZX, eRotOrderZXY, eRotOrderZYX,
    eRotOrderCount
};

static const char* kRotationOrderLabels[] = {
    "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX",
};

struct SphericalTransformPrivate
{
    // Input section
    KnobChoiceWPtr inputProjection;
    KnobChoiceWPtr fisheyeTypeInput;
    KnobChoiceWPtr cubemapFormatInput;
    KnobChoiceWPtr cubemapPackingInput;
    KnobChoiceWPtr cubemapFaceInput;
    KnobDoubleWPtr focalInput;
    KnobDoubleWPtr sensorInputW;
    KnobDoubleWPtr sensorInputH;
    KnobChoiceWPtr rotationModeInput;
    KnobDoubleWPtr panInput;         // Pan-Tilt-Roll: x
    KnobDoubleWPtr tiltInput;        // Pan-Tilt-Roll: y
    KnobDoubleWPtr rollInput;        // Pan-Tilt-Roll: z
    KnobDoubleWPtr lookDirXInput;       // Look: direction x (pan)
    KnobDoubleWPtr lookDirYInput;       // Look: direction y (tilt)
    KnobDoubleWPtr lookAngleInput;      // Look: angle (roll around look axis)
    KnobChoiceWPtr rotOrderInput;       // Rotation Angles: order
    KnobDoubleWPtr rotAngleXInput;      // Rotation Angles: X
    KnobDoubleWPtr rotAngleYInput;      // Rotation Angles: Y
    KnobDoubleWPtr rotAngleZInput;      // Rotation Angles: Z

    // Output section
    KnobChoiceWPtr outputProjection;
    KnobChoiceWPtr fisheyeTypeOutput;
    KnobChoiceWPtr cubemapFormatOutput;
    KnobChoiceWPtr cubemapPackingOutput;
    KnobChoiceWPtr cubemapFaceOutput;
    KnobDoubleWPtr focalOutput;
    KnobDoubleWPtr sensorOutputW;
    KnobDoubleWPtr sensorOutputH;
    KnobChoiceWPtr rotationModeOutput;
    KnobDoubleWPtr panOutput;
    KnobDoubleWPtr tiltOutput;
    KnobDoubleWPtr rollOutput;
    KnobDoubleWPtr lookDirXOutput;
    KnobDoubleWPtr lookDirYOutput;
    KnobDoubleWPtr lookAngleOutput;
    KnobChoiceWPtr rotOrderOutput;
    KnobDoubleWPtr rotAngleXOutput;
    KnobDoubleWPtr rotAngleYOutput;
    KnobDoubleWPtr rotAngleZOutput;

    // Filter
    KnobChoiceWPtr filter;
    KnobBoolWPtr blackOutside;

    // Format
    KnobChoiceWPtr formatMode;
    KnobDoubleWPtr scaleWidth;

    // Swap button
    KnobButtonWPtr swapButton;
};


// ---- Constructor / Destructor ----

SphericalTransform::SphericalTransform(NodePtr node)
    : EffectInstance(node)
    , _imp(new SphericalTransformPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

SphericalTransform::~SphericalTransform()
{
}

std::string
SphericalTransform::getPluginDescription() const
{
    return tr("Convert images between spherical projection types. "
              "Supports LatLong (equirectangular), Rectilinear (perspective), "
              "Fisheye (Equidistant, Equisolid, Stereographic, Orthographic), "
              "and MirrorBall (chrome sphere). "
              "Each of Input and Output has independent projection and rotation controls. "
              "Equivalent to Nuke's SphericalTransform node.").toStdString();
}

std::string
SphericalTransform::getInputLabel(int /*inputNb*/) const
{
    return "Source";
}

bool
SphericalTransform::isInputOptional(int /*inputNb*/) const
{
    return false;
}

void
SphericalTransform::addAcceptedComponents(int /*inputNb*/,
                                          std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
SphericalTransform::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
SphericalTransform::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ---- Helper to check if UI projection needs camera params ----
static bool projectionNeedsCameraParams(int uiProj)
{
    return uiProj == eUIProjectionRectilinear || uiProj == eUIProjectionFisheye;
}

static bool projectionIsFisheye(int uiProj)
{
    return uiProj == eUIProjectionFisheye;
}

static bool projectionIsCubemap(int uiProj)
{
    return uiProj == eUIProjectionCubemap;
}


// ---- Knobs ----

void
SphericalTransform::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("SphericalTransform"));

    // ---- Input Section ----
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Input"));
        page->addKnob(sep);

        KnobChoicePtr proj = AppManager::createKnob<KnobChoice>(this, tr("Projection"));
        proj->setName("inputProjection");
        proj->setHintToolTip(tr("Projection type of the input image."));
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < kNumProjections; ++i) {
                entries.push_back(ChoiceOption(kProjectionLabels[i], "", ""));
            }
            proj->populateChoices(entries);
        }
        proj->setDefaultValue(eUIProjectionLatLong);
        proj->setAnimationEnabled(false);
        page->addKnob(proj);
        _imp->inputProjection = proj;

        KnobChoicePtr fishType = AppManager::createKnob<KnobChoice>(this, tr("Type"));
        fishType->setName("fisheyeTypeInput");
        fishType->setHintToolTip(tr("Fisheye projection model."));
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eUIFisheyeCount; ++i) {
                entries.push_back(ChoiceOption(kFisheyeTypeLabels[i], "", ""));
            }
            fishType->populateChoices(entries);
        }
        fishType->setDefaultValue(eUIFisheyeEquidistant);
        fishType->setAnimationEnabled(false);
        fishType->setSecret(true);  // Hidden unless Fisheye selected
        page->addKnob(fishType);
        _imp->fisheyeTypeInput = fishType;

        KnobChoicePtr cubeFmt = AppManager::createKnob<KnobChoice>(this, tr("Format"));
        cubeFmt->setName("cubemapFormatInput");
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("Image", "", "Single packed image"));
            entries.push_back(ChoiceOption("Views", "", "Multi-view (not yet supported)"));
            entries.push_back(ChoiceOption("Faces", "", "Separate face inputs (not yet supported)"));
            cubeFmt->populateChoices(entries);
        }
        cubeFmt->setDefaultValue(0);
        cubeFmt->setAnimationEnabled(false);
        cubeFmt->setSecret(true);
        page->addKnob(cubeFmt);
        _imp->cubemapFormatInput = cubeFmt;

        KnobChoicePtr cubePack = AppManager::createKnob<KnobChoice>(this, tr("Packing"));
        cubePack->setName("cubemapPackingInput");
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eCubemapPackingCount; ++i)
                entries.push_back(ChoiceOption(kCubemapPackingLabels[i], "", ""));
            cubePack->populateChoices(entries);
        }
        cubePack->setDefaultValue(eCubemapLLCross);
        cubePack->setAnimationEnabled(false);
        cubePack->setSecret(true);
        page->addKnob(cubePack);
        _imp->cubemapPackingInput = cubePack;

        KnobChoicePtr cubeFace = AppManager::createKnob<KnobChoice>(this, tr("Face"));
        cubeFace->setName("cubemapFaceInput");
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("+X", "", ""));
            entries.push_back(ChoiceOption("-X", "", ""));
            entries.push_back(ChoiceOption("+Y", "", ""));
            entries.push_back(ChoiceOption("-Y", "", ""));
            entries.push_back(ChoiceOption("+Z", "", ""));
            entries.push_back(ChoiceOption("-Z", "", ""));
            cubeFace->populateChoices(entries);
        }
        cubeFace->setDefaultValue(4); // +Z (front)
        cubeFace->setAnimationEnabled(false);
        cubeFace->setSecret(true);
        page->addKnob(cubeFace);
        _imp->cubemapFaceInput = cubeFace;

        KnobDoublePtr focal = AppManager::createKnob<KnobDouble>(this, tr("Focal"));
        focal->setName("focalInput");
        focal->setHintToolTip(tr("Focal length in mm (for Rectilinear and Fisheye projections)."));
        focal->setDefaultValue(16.0);
        focal->setMinimum(1.0);
        focal->setDisplayMinimum(4.0);
        focal->setDisplayMaximum(200.0);
        focal->setAnimationEnabled(true);
        focal->setSecret(true);
        page->addKnob(focal);
        _imp->focalInput = focal;

        KnobDoublePtr sensorW = AppManager::createKnob<KnobDouble>(this, tr("Sensor Width"));
        sensorW->setName("sensorInputW");
        sensorW->setHintToolTip(tr("Sensor width in mm."));
        sensorW->setDefaultValue(36.0);
        sensorW->setMinimum(1.0);
        sensorW->setDisplayMinimum(1.0);
        sensorW->setDisplayMaximum(100.0);
        sensorW->setAnimationEnabled(false);
        sensorW->setSecret(true);
        page->addKnob(sensorW);
        _imp->sensorInputW = sensorW;

        KnobDoublePtr sensorH = AppManager::createKnob<KnobDouble>(this, tr("Sensor Height"));
        sensorH->setName("sensorInputH");
        sensorH->setHintToolTip(tr("Sensor height in mm."));
        sensorH->setDefaultValue(24.0);
        sensorH->setMinimum(1.0);
        sensorH->setDisplayMinimum(1.0);
        sensorH->setDisplayMaximum(100.0);
        sensorH->setAnimationEnabled(false);
        sensorH->setSecret(true);
        page->addKnob(sensorH);
        _imp->sensorInputH = sensorH;

        // Rotation mode
        KnobChoicePtr rotMode = AppManager::createKnob<KnobChoice>(this, tr("Rotation"));
        rotMode->setName("rotationModeInput");
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eRotationModeCount; ++i)
                entries.push_back(ChoiceOption(kRotationModeLabels[i], "", ""));
            rotMode->populateChoices(entries);
        }
        rotMode->setDefaultValue(eRotationModePanTiltRoll);
        rotMode->setAnimationEnabled(false);
        page->addKnob(rotMode);
        _imp->rotationModeInput = rotMode;

        // Pan-Tilt-Roll knobs (visible by default)
        KnobDoublePtr pan = AppManager::createKnob<KnobDouble>(this, tr("Pan Tilt Roll x"));
        pan->setName("panInput");
        pan->setHintToolTip(tr("Pan (x): left/right rotation in degrees."));
        pan->setDefaultValue(0.0);
        pan->setDisplayMinimum(-180.0);
        pan->setDisplayMaximum(180.0);
        pan->setAnimationEnabled(true);
        page->addKnob(pan);
        _imp->panInput = pan;

        KnobDoublePtr tilt = AppManager::createKnob<KnobDouble>(this, tr("y"));
        tilt->setName("tiltInput");
        tilt->setHintToolTip(tr("Tilt (y): up/down rotation in degrees. Positive = look up."));
        tilt->setDefaultValue(0.0);
        tilt->setDisplayMinimum(-90.0);
        tilt->setDisplayMaximum(90.0);
        tilt->setAnimationEnabled(true);
        page->addKnob(tilt);
        _imp->tiltInput = tilt;

        KnobDoublePtr roll = AppManager::createKnob<KnobDouble>(this, tr("z"));
        roll->setName("rollInput");
        roll->setHintToolTip(tr("Roll (z): clockwise/counter-clockwise rotation in degrees."));
        roll->setDefaultValue(0.0);
        roll->setDisplayMinimum(-180.0);
        roll->setDisplayMaximum(180.0);
        roll->setAnimationEnabled(true);
        page->addKnob(roll);
        _imp->rollInput = roll;

        // Look mode knobs (hidden by default)
        KnobDoublePtr lookX = AppManager::createKnob<KnobDouble>(this, tr("Direction x"));
        lookX->setName("lookDirXInput");
        lookX->setHintToolTip(tr("Look direction x: pan in degrees."));
        lookX->setDefaultValue(0.0);
        lookX->setDisplayMinimum(-180.0);
        lookX->setDisplayMaximum(180.0);
        lookX->setAnimationEnabled(true);
        lookX->setSecret(true);
        page->addKnob(lookX);
        _imp->lookDirXInput = lookX;

        KnobDoublePtr lookY = AppManager::createKnob<KnobDouble>(this, tr("y"));
        lookY->setName("lookDirYInput");
        lookY->setHintToolTip(tr("Look direction y: tilt in degrees."));
        lookY->setDefaultValue(0.0);
        lookY->setDisplayMinimum(-90.0);
        lookY->setDisplayMaximum(90.0);
        lookY->setAnimationEnabled(true);
        lookY->setSecret(true);
        page->addKnob(lookY);
        _imp->lookDirYInput = lookY;

        KnobDoublePtr lookA = AppManager::createKnob<KnobDouble>(this, tr("Angle"));
        lookA->setName("lookAngleInput");
        lookA->setHintToolTip(tr("Rotation around the look direction (roll)."));
        lookA->setDefaultValue(0.0);
        lookA->setDisplayMinimum(-180.0);
        lookA->setDisplayMaximum(180.0);
        lookA->setAnimationEnabled(true);
        lookA->setSecret(true);
        page->addKnob(lookA);
        _imp->lookAngleInput = lookA;

        // Rotation Angles mode knobs (hidden by default)
        KnobChoicePtr rotOrd = AppManager::createKnob<KnobChoice>(this, tr("Rotation Order"));
        rotOrd->setName("rotOrderInput");
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eRotOrderCount; ++i)
                entries.push_back(ChoiceOption(kRotationOrderLabels[i], "", ""));
            rotOrd->populateChoices(entries);
        }
        rotOrd->setDefaultValue(eRotOrderZXY);
        rotOrd->setAnimationEnabled(false);
        rotOrd->setSecret(true);
        page->addKnob(rotOrd);
        _imp->rotOrderInput = rotOrd;

        KnobDoublePtr raX = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        raX->setName("rotAngleXInput");
        raX->setDefaultValue(0.0);
        raX->setDisplayMinimum(-180.0);
        raX->setDisplayMaximum(180.0);
        raX->setAnimationEnabled(true);
        raX->setSecret(true);
        page->addKnob(raX);
        _imp->rotAngleXInput = raX;

        KnobDoublePtr raY = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        raY->setName("rotAngleYInput");
        raY->setDefaultValue(0.0);
        raY->setDisplayMinimum(-180.0);
        raY->setDisplayMaximum(180.0);
        raY->setAnimationEnabled(true);
        raY->setSecret(true);
        page->addKnob(raY);
        _imp->rotAngleYInput = raY;

        KnobDoublePtr raZ = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        raZ->setName("rotAngleZInput");
        raZ->setDefaultValue(0.0);
        raZ->setDisplayMinimum(-180.0);
        raZ->setDisplayMaximum(180.0);
        raZ->setAnimationEnabled(true);
        raZ->setSecret(true);
        page->addKnob(raZ);
        _imp->rotAngleZInput = raZ;
    }

    // ---- Output Section ----
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Output"));
        page->addKnob(sep);

        KnobChoicePtr proj = AppManager::createKnob<KnobChoice>(this, tr("Projection"));
        proj->setName("outputProjection");
        proj->setHintToolTip(tr("Projection type of the output image."));
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < kNumProjections; ++i) {
                entries.push_back(ChoiceOption(kProjectionLabels[i], "", ""));
            }
            proj->populateChoices(entries);
        }
        proj->setDefaultValue(eUIProjectionLatLong);
        proj->setAnimationEnabled(false);
        page->addKnob(proj);
        _imp->outputProjection = proj;

        KnobChoicePtr fishType = AppManager::createKnob<KnobChoice>(this, tr("Type"));
        fishType->setName("fisheyeTypeOutput");
        fishType->setHintToolTip(tr("Fisheye projection model."));
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eUIFisheyeCount; ++i) {
                entries.push_back(ChoiceOption(kFisheyeTypeLabels[i], "", ""));
            }
            fishType->populateChoices(entries);
        }
        fishType->setDefaultValue(eUIFisheyeEquidistant);
        fishType->setAnimationEnabled(false);
        fishType->setSecret(true);
        page->addKnob(fishType);
        _imp->fisheyeTypeOutput = fishType;

        KnobChoicePtr cubeFmt = AppManager::createKnob<KnobChoice>(this, tr("Format"));
        cubeFmt->setName("cubemapFormatOutput");
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("Image", "", "Single packed image"));
            entries.push_back(ChoiceOption("Views", "", "Multi-view (not yet supported)"));
            entries.push_back(ChoiceOption("Faces", "", "Separate face inputs (not yet supported)"));
            cubeFmt->populateChoices(entries);
        }
        cubeFmt->setDefaultValue(0);
        cubeFmt->setAnimationEnabled(false);
        cubeFmt->setSecret(true);
        page->addKnob(cubeFmt);
        _imp->cubemapFormatOutput = cubeFmt;

        KnobChoicePtr cubePack = AppManager::createKnob<KnobChoice>(this, tr("Packing"));
        cubePack->setName("cubemapPackingOutput");
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eCubemapPackingCount; ++i)
                entries.push_back(ChoiceOption(kCubemapPackingLabels[i], "", ""));
            cubePack->populateChoices(entries);
        }
        cubePack->setDefaultValue(eCubemapLLCross);
        cubePack->setAnimationEnabled(false);
        cubePack->setSecret(true);
        page->addKnob(cubePack);
        _imp->cubemapPackingOutput = cubePack;

        KnobChoicePtr cubeFace = AppManager::createKnob<KnobChoice>(this, tr("Face"));
        cubeFace->setName("cubemapFaceOutput");
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("+X", "", ""));
            entries.push_back(ChoiceOption("-X", "", ""));
            entries.push_back(ChoiceOption("+Y", "", ""));
            entries.push_back(ChoiceOption("-Y", "", ""));
            entries.push_back(ChoiceOption("+Z", "", ""));
            entries.push_back(ChoiceOption("-Z", "", ""));
            cubeFace->populateChoices(entries);
        }
        cubeFace->setDefaultValue(4); // +Z (front)
        cubeFace->setAnimationEnabled(false);
        cubeFace->setSecret(true);
        page->addKnob(cubeFace);
        _imp->cubemapFaceOutput = cubeFace;

        KnobDoublePtr focal = AppManager::createKnob<KnobDouble>(this, tr("Focal"));
        focal->setName("focalOutput");
        focal->setHintToolTip(tr("Focal length in mm (for Rectilinear and Fisheye projections)."));
        focal->setDefaultValue(16.0);
        focal->setMinimum(1.0);
        focal->setDisplayMinimum(4.0);
        focal->setDisplayMaximum(200.0);
        focal->setAnimationEnabled(true);
        focal->setSecret(true);
        page->addKnob(focal);
        _imp->focalOutput = focal;

        KnobDoublePtr sensorW = AppManager::createKnob<KnobDouble>(this, tr("Sensor Width"));
        sensorW->setName("sensorOutputW");
        sensorW->setHintToolTip(tr("Sensor width in mm."));
        sensorW->setDefaultValue(36.0);
        sensorW->setMinimum(1.0);
        sensorW->setDisplayMinimum(1.0);
        sensorW->setDisplayMaximum(100.0);
        sensorW->setAnimationEnabled(false);
        sensorW->setSecret(true);
        page->addKnob(sensorW);
        _imp->sensorOutputW = sensorW;

        KnobDoublePtr sensorH = AppManager::createKnob<KnobDouble>(this, tr("Sensor Height"));
        sensorH->setName("sensorOutputH");
        sensorH->setHintToolTip(tr("Sensor height in mm."));
        sensorH->setDefaultValue(24.0);
        sensorH->setMinimum(1.0);
        sensorH->setDisplayMinimum(1.0);
        sensorH->setDisplayMaximum(100.0);
        sensorH->setAnimationEnabled(false);
        sensorH->setSecret(true);
        page->addKnob(sensorH);
        _imp->sensorOutputH = sensorH;

        // Rotation mode
        KnobChoicePtr rotMode = AppManager::createKnob<KnobChoice>(this, tr("Rotation"));
        rotMode->setName("rotationModeOutput");
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eRotationModeCount; ++i)
                entries.push_back(ChoiceOption(kRotationModeLabels[i], "", ""));
            rotMode->populateChoices(entries);
        }
        rotMode->setDefaultValue(eRotationModePanTiltRoll);
        rotMode->setAnimationEnabled(false);
        page->addKnob(rotMode);
        _imp->rotationModeOutput = rotMode;

        // Pan-Tilt-Roll
        KnobDoublePtr pan = AppManager::createKnob<KnobDouble>(this, tr("Pan Tilt Roll x"));
        pan->setName("panOutput");
        pan->setHintToolTip(tr("Pan (x): left/right rotation in degrees."));
        pan->setDefaultValue(0.0);
        pan->setDisplayMinimum(-180.0);
        pan->setDisplayMaximum(180.0);
        pan->setAnimationEnabled(true);
        page->addKnob(pan);
        _imp->panOutput = pan;

        KnobDoublePtr tilt = AppManager::createKnob<KnobDouble>(this, tr("y"));
        tilt->setName("tiltOutput");
        tilt->setHintToolTip(tr("Tilt (y): up/down rotation in degrees. Positive = look up."));
        tilt->setDefaultValue(0.0);
        tilt->setDisplayMinimum(-90.0);
        tilt->setDisplayMaximum(90.0);
        tilt->setAnimationEnabled(true);
        page->addKnob(tilt);
        _imp->tiltOutput = tilt;

        KnobDoublePtr roll = AppManager::createKnob<KnobDouble>(this, tr("z"));
        roll->setName("rollOutput");
        roll->setHintToolTip(tr("Roll (z): clockwise/counter-clockwise rotation in degrees."));
        roll->setDefaultValue(0.0);
        roll->setDisplayMinimum(-180.0);
        roll->setDisplayMaximum(180.0);
        roll->setAnimationEnabled(true);
        page->addKnob(roll);
        _imp->rollOutput = roll;

        // Look mode
        KnobDoublePtr lookX = AppManager::createKnob<KnobDouble>(this, tr("Direction x"));
        lookX->setName("lookDirXOutput");
        lookX->setDefaultValue(0.0);
        lookX->setDisplayMinimum(-180.0);
        lookX->setDisplayMaximum(180.0);
        lookX->setAnimationEnabled(true);
        lookX->setSecret(true);
        page->addKnob(lookX);
        _imp->lookDirXOutput = lookX;

        KnobDoublePtr lookY = AppManager::createKnob<KnobDouble>(this, tr("y"));
        lookY->setName("lookDirYOutput");
        lookY->setDefaultValue(0.0);
        lookY->setDisplayMinimum(-90.0);
        lookY->setDisplayMaximum(90.0);
        lookY->setAnimationEnabled(true);
        lookY->setSecret(true);
        page->addKnob(lookY);
        _imp->lookDirYOutput = lookY;

        KnobDoublePtr lookA = AppManager::createKnob<KnobDouble>(this, tr("Angle"));
        lookA->setName("lookAngleOutput");
        lookA->setHintToolTip(tr("Rotation around the look direction (roll)."));
        lookA->setDefaultValue(0.0);
        lookA->setDisplayMinimum(-180.0);
        lookA->setDisplayMaximum(180.0);
        lookA->setAnimationEnabled(true);
        lookA->setSecret(true);
        page->addKnob(lookA);
        _imp->lookAngleOutput = lookA;

        // Rotation Angles mode
        KnobChoicePtr rotOrd = AppManager::createKnob<KnobChoice>(this, tr("Rotation Order"));
        rotOrd->setName("rotOrderOutput");
        {
            std::vector<ChoiceOption> entries;
            for (int i = 0; i < eRotOrderCount; ++i)
                entries.push_back(ChoiceOption(kRotationOrderLabels[i], "", ""));
            rotOrd->populateChoices(entries);
        }
        rotOrd->setDefaultValue(eRotOrderZXY);
        rotOrd->setAnimationEnabled(false);
        rotOrd->setSecret(true);
        page->addKnob(rotOrd);
        _imp->rotOrderOutput = rotOrd;

        KnobDoublePtr raX = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        raX->setName("rotAngleXOutput");
        raX->setDefaultValue(0.0);
        raX->setDisplayMinimum(-180.0);
        raX->setDisplayMaximum(180.0);
        raX->setAnimationEnabled(true);
        raX->setSecret(true);
        page->addKnob(raX);
        _imp->rotAngleXOutput = raX;

        KnobDoublePtr raY = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        raY->setName("rotAngleYOutput");
        raY->setDefaultValue(0.0);
        raY->setDisplayMinimum(-180.0);
        raY->setDisplayMaximum(180.0);
        raY->setAnimationEnabled(true);
        raY->setSecret(true);
        page->addKnob(raY);
        _imp->rotAngleYOutput = raY;

        KnobDoublePtr raZ = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        raZ->setName("rotAngleZOutput");
        raZ->setDefaultValue(0.0);
        raZ->setDisplayMinimum(-180.0);
        raZ->setDisplayMaximum(180.0);
        raZ->setAnimationEnabled(true);
        raZ->setSecret(true);
        page->addKnob(raZ);
        _imp->rotAngleZOutput = raZ;
    }

    // ---- Filter Section ----
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);

        KnobChoicePtr filter = AppManager::createKnob<KnobChoice>(this, tr("Filter"));
        filter->setName("filter");
        filter->setHintToolTip(tr("Pixel interpolation filter."));
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("Impulse", "", "Nearest neighbor (no filtering)"));
            entries.push_back(ChoiceOption("Bilinear", "", "Bilinear interpolation"));
            entries.push_back(ChoiceOption("Cubic", "", "Catmull-Rom bicubic (sharp, Nuke default)"));
            entries.push_back(ChoiceOption("Mitchell", "", "Mitchell-Netravali (smooth, hides pixelation)"));
            filter->populateChoices(entries);
        }
        filter->setDefaultValue(eFilterCubic);
        filter->setAnimationEnabled(false);
        page->addKnob(filter);
        _imp->filter = filter;

        KnobBoolPtr black = AppManager::createKnob<KnobBool>(this, tr("Black Outside"));
        black->setName("blackOutside");
        black->setHintToolTip(tr("Set pixels outside the source image bounds to black."));
        black->setDefaultValue(true);
        black->setAnimationEnabled(false);
        page->addKnob(black);
        _imp->blackOutside = black;
    }

    // ---- Format Section ----
    {
        KnobChoicePtr fmt = AppManager::createKnob<KnobChoice>(this, tr("Format"));
        fmt->setName("formatMode");
        fmt->setHintToolTip(tr("How the output size is determined.\n"
                                "To Scale: output width = input width * scale.\n"
                                "To Width: output has the specified width in pixels."));
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("To Scale", "", "Output width = input width * scale"));
            entries.push_back(ChoiceOption("To Width", "", "Output has specified pixel width"));
            fmt->populateChoices(entries);
        }
        fmt->setDefaultValue(0);
        fmt->setAnimationEnabled(false);
        page->addKnob(fmt);
        _imp->formatMode = fmt;

        KnobDoublePtr scale = AppManager::createKnob<KnobDouble>(this, tr("Width"));
        scale->setName("scaleWidth");
        scale->setHintToolTip(tr("When Format = To Scale: scale factor (1.0 = same as input).\n"
                                  "When Format = To Width: output width in pixels."));
        scale->setDefaultValue(1.0);
        scale->setMinimum(0.01);
        scale->setDisplayMinimum(0.1);
        scale->setDisplayMaximum(4.0);
        scale->setAnimationEnabled(true);
        page->addKnob(scale);
        _imp->scaleWidth = scale;
    }

    // ---- Swap Button ----
    {
        KnobButtonPtr swap = AppManager::createKnob<KnobButton>(this, tr("Swap Input/Output"));
        swap->setName("swapInputOutput");
        swap->setHintToolTip(tr("Swap all Input and Output settings."));
        page->addKnob(swap);
        _imp->swapButton = swap;
    }
}


// ---- knobChanged ----

bool
SphericalTransform::knobChanged(KnobI* k,
                                 ValueChangedReasonEnum /*reason*/,
                                 ViewSpec /*view*/,
                                 double /*time*/,
                                 bool /*originatedFromMainThread*/)
{
    // Show/hide camera params based on projection type
    KnobChoicePtr inProj = _imp->inputProjection.lock();
    KnobChoicePtr outProj = _imp->outputProjection.lock();

    if (inProj.get() == k) {
        int val = inProj->getValue();
        fprintf(stderr, "[SphericalTransform] Input projection changed → %s\n",
                (val >= 0 && val < kNumProjections) ? kProjectionLabels[val] : "unknown");
        fflush(stderr);
        bool showCam = projectionNeedsCameraParams(val);
        bool showFish = projectionIsFisheye(val);
        bool showCube = projectionIsCubemap(val);
        _imp->fisheyeTypeInput.lock()->setSecret(!showFish);
        _imp->cubemapFormatInput.lock()->setSecret(!showCube);
        if (showCube) {
            int fmt = _imp->cubemapFormatInput.lock()->getValue();
            _imp->cubemapPackingInput.lock()->setSecret(fmt != 0);  // Image
            _imp->cubemapFaceInput.lock()->setSecret(fmt != 2);     // Faces
        } else {
            _imp->cubemapPackingInput.lock()->setSecret(true);
            _imp->cubemapFaceInput.lock()->setSecret(true);
        }
        _imp->focalInput.lock()->setSecret(!showCam);
        _imp->sensorInputW.lock()->setSecret(!showCam);
        _imp->sensorInputH.lock()->setSecret(!showCam);
        return true;
    }

    if (outProj.get() == k) {
        int val = outProj->getValue();
        fprintf(stderr, "[SphericalTransform] Output projection changed → %s\n",
                (val >= 0 && val < kNumProjections) ? kProjectionLabels[val] : "unknown");
        fflush(stderr);
        bool showCam = projectionNeedsCameraParams(val);
        bool showFish = projectionIsFisheye(val);
        bool showCube = projectionIsCubemap(val);
        _imp->fisheyeTypeOutput.lock()->setSecret(!showFish);
        _imp->cubemapFormatOutput.lock()->setSecret(!showCube);
        if (showCube) {
            int fmt = _imp->cubemapFormatOutput.lock()->getValue();
            _imp->cubemapPackingOutput.lock()->setSecret(fmt != 0);
            _imp->cubemapFaceOutput.lock()->setSecret(fmt != 2);
        } else {
            _imp->cubemapPackingOutput.lock()->setSecret(true);
            _imp->cubemapFaceOutput.lock()->setSecret(true);
        }
        _imp->focalOutput.lock()->setSecret(!showCam);
        _imp->sensorOutputW.lock()->setSecret(!showCam);
        _imp->sensorOutputH.lock()->setSecret(!showCam);
        refreshMetadata_public(true);
        return true;
    }

    // Cubemap format change — show Packing or Face
    if (_imp->cubemapFormatInput.lock().get() == k) {
        int fmt = _imp->cubemapFormatInput.lock()->getValue();
        _imp->cubemapPackingInput.lock()->setSecret(fmt != 0);
        _imp->cubemapFaceInput.lock()->setSecret(fmt != 2);
        return true;
    }
    if (_imp->cubemapFormatOutput.lock().get() == k) {
        int fmt = _imp->cubemapFormatOutput.lock()->getValue();
        _imp->cubemapPackingOutput.lock()->setSecret(fmt != 0);
        _imp->cubemapFaceOutput.lock()->setSecret(fmt != 2);
        refreshMetadata_public(true);
        return true;
    }

    // Refresh format when output-affecting knobs change
    if (_imp->sensorOutputW.lock().get() == k ||
        _imp->sensorOutputH.lock().get() == k ||
        _imp->focalOutput.lock().get() == k ||
        _imp->formatMode.lock().get() == k ||
        _imp->scaleWidth.lock().get() == k ||
        _imp->cubemapPackingOutput.lock().get() == k) {
        refreshMetadata_public(true);
        return true;
    }

    // Rotation mode visibility — helper lambda
    auto updateRotModeVisibility = [](int mode,
                                      KnobDoubleWPtr& pan, KnobDoubleWPtr& tilt, KnobDoubleWPtr& roll,
                                      KnobDoubleWPtr& lookDirX, KnobDoubleWPtr& lookDirY,
                                      KnobDoubleWPtr& lookAngle,
                                      KnobChoiceWPtr& rotOrd,
                                      KnobDoubleWPtr& raX, KnobDoubleWPtr& raY, KnobDoubleWPtr& raZ) {
        bool isPTR = (mode == eRotationModePanTiltRoll);
        bool isLook = (mode == eRotationModeLook);
        bool isRA = (mode == eRotationModeRotationAngles);
        pan.lock()->setSecret(!isPTR);
        tilt.lock()->setSecret(!isPTR);
        roll.lock()->setSecret(!isPTR);
        lookDirX.lock()->setSecret(!isLook);
        lookDirY.lock()->setSecret(!isLook);
        lookAngle.lock()->setSecret(!isLook);
        rotOrd.lock()->setSecret(!isRA);
        raX.lock()->setSecret(!isRA);
        raY.lock()->setSecret(!isRA);
        raZ.lock()->setSecret(!isRA);
    };

    KnobChoicePtr inRotMode = _imp->rotationModeInput.lock();
    if (inRotMode.get() == k) {
        updateRotModeVisibility(inRotMode->getValue(),
            _imp->panInput, _imp->tiltInput, _imp->rollInput,
            _imp->lookDirXInput, _imp->lookDirYInput, _imp->lookAngleInput,
            _imp->rotOrderInput,
            _imp->rotAngleXInput, _imp->rotAngleYInput, _imp->rotAngleZInput);
        return true;
    }

    KnobChoicePtr outRotMode = _imp->rotationModeOutput.lock();
    if (outRotMode.get() == k) {
        updateRotModeVisibility(outRotMode->getValue(),
            _imp->panOutput, _imp->tiltOutput, _imp->rollOutput,
            _imp->lookDirXOutput, _imp->lookDirYOutput, _imp->lookAngleOutput,
            _imp->rotOrderOutput,
            _imp->rotAngleXOutput, _imp->rotAngleYOutput, _imp->rotAngleZOutput);
        return true;
    }

    // Swap button
    KnobButtonPtr swapBtn = _imp->swapButton.lock();
    if (swapBtn.get() == k) {
        // Swap projection types
        int tmpProj = inProj->getValue();
        inProj->setValue(outProj->getValue());
        outProj->setValue(tmpProj);

        // Swap focal
        KnobDoublePtr fIn = _imp->focalInput.lock(), fOut = _imp->focalOutput.lock();
        double tmpF = fIn->getValue();
        fIn->setValue(fOut->getValue());
        fOut->setValue(tmpF);

        // Swap sensor W
        KnobDoublePtr swIn = _imp->sensorInputW.lock(), swOut = _imp->sensorOutputW.lock();
        double tmpSW = swIn->getValue();
        swIn->setValue(swOut->getValue());
        swOut->setValue(tmpSW);

        // Swap sensor H
        KnobDoublePtr shIn = _imp->sensorInputH.lock(), shOut = _imp->sensorOutputH.lock();
        double tmpSH = shIn->getValue();
        shIn->setValue(shOut->getValue());
        shOut->setValue(tmpSH);

        // Swap rotation
        KnobDoublePtr pIn = _imp->panInput.lock(), pOut = _imp->panOutput.lock();
        KnobDoublePtr tIn = _imp->tiltInput.lock(), tOut = _imp->tiltOutput.lock();
        KnobDoublePtr rIn = _imp->rollInput.lock(), rOut = _imp->rollOutput.lock();
        double tmpP = pIn->getValue(), tmpT = tIn->getValue(), tmpR = rIn->getValue();
        pIn->setValue(pOut->getValue()); pOut->setValue(tmpP);
        tIn->setValue(tOut->getValue()); tOut->setValue(tmpT);
        rIn->setValue(rOut->getValue()); rOut->setValue(tmpR);

        // Update visibility
        bool showIn = projectionNeedsCameraParams(inProj->getValue());
        _imp->focalInput.lock()->setSecret(!showIn);
        _imp->sensorInputW.lock()->setSecret(!showIn);
        _imp->sensorInputH.lock()->setSecret(!showIn);

        bool showOut = projectionNeedsCameraParams(outProj->getValue());
        _imp->focalOutput.lock()->setSecret(!showOut);
        _imp->sensorOutputW.lock()->setSecret(!showOut);
        _imp->sensorOutputH.lock()->setSecret(!showOut);

        return true;
    }

    // Format mode changed — adjust Width knob range
    KnobChoicePtr fmtMode = _imp->formatMode.lock();
    if (fmtMode.get() == k) {
        KnobDoublePtr sw = _imp->scaleWidth.lock();
        if (fmtMode->getValue() == 0) {
            // To Scale
            sw->setDisplayMinimum(0.1);
            sw->setDisplayMaximum(4.0);
            if (sw->getValue() > 100.0) sw->setValue(1.0);
        } else {
            // To Width (pixels)
            sw->setDisplayMinimum(256.0);
            sw->setDisplayMaximum(8192.0);
            if (sw->getValue() < 10.0) sw->setValue(1920.0);
        }
        return true;
    }

    return false;
}


// ---- Region of Definition ----

StatusEnum
SphericalTransform::getRegionOfDefinition(U64 /*hash*/,
                                           double time,
                                           const RenderScale& scale,
                                           ViewIdx view,
                                           RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;

    RectD inputRod;
    bool isProjectFormat = false;
    StatusEnum st = input->getRegionOfDefinition_public(input->getHash(), time, scale, view, &inputRod, &isProjectFormat);
    if (st != eStatusOK) return st;

    double inputW = inputRod.x2 - inputRod.x1;
    double inputH = inputRod.y2 - inputRod.y1;

    int fmtMode = _imp->formatMode.lock()->getValue();
    double scaleVal = _imp->scaleWidth.lock()->getValue();

    double outW, outH;
    if (fmtMode == 0) {
        // To Scale: output width = input width * scale
        outW = inputW * scaleVal;
    } else {
        // To Width: output width = scaleVal (pixel value)
        outW = scaleVal;
    }

    // Determine output height based on output projection
    int outUIProj = _imp->outputProjection.lock()->getValue();
    if (outUIProj == eUIProjectionLatLong) {
        outH = outW / 2.0;
    } else if (outUIProj == eUIProjectionMirrorBall) {
        outH = outW;
    } else if (outUIProj == eUIProjectionCubemap) {
        int cubeFmt = _imp->cubemapFormatOutput.lock()->getValue();
        if (cubeFmt == 2) {
            // Faces mode: single square face
            outH = outW;
        } else {
            int packing = _imp->cubemapPackingOutput.lock()->getValue();
            if (packing == eCubemapLLCross)    outH = outW * 3.0 / 4.0;
            else if (packing == eCubemap6x1)   outH = outW / 6.0;
            else if (packing == eCubemap3x2)   outH = outW * 2.0 / 3.0;
            else                                outH = outW * 3.0 / 4.0;
        }
    } else {
        double sensorW = _imp->sensorOutputW.lock()->getValue();
        double sensorH = _imp->sensorOutputH.lock()->getValue();
        outH = outW * sensorH / sensorW;
    }

    rod->x1 = inputRod.x1;
    rod->y1 = inputRod.y1;
    rod->x2 = inputRod.x1 + outW;
    rod->y2 = inputRod.y1 + outH;

    return eStatusOK;
}


// ---- Output Format Metadata ----

StatusEnum
SphericalTransform::getPreferredMetadata(NodeMetadata& metadata)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusOK;

    RectI inputFormat = input->getOutputFormat();
    double inputW = inputFormat.x2 - inputFormat.x1;

    int fmtMode = _imp->formatMode.lock()->getValue();
    double scaleVal = _imp->scaleWidth.lock()->getValue();

    double outW;
    if (fmtMode == 0) {
        outW = inputW * scaleVal;
    } else {
        outW = scaleVal;
    }

    double outH;
    int outUIProj = _imp->outputProjection.lock()->getValue();
    if (outUIProj == eUIProjectionLatLong) {
        outH = outW / 2.0;
    } else if (outUIProj == eUIProjectionMirrorBall) {
        outH = outW;
    } else if (outUIProj == eUIProjectionCubemap) {
        int cubeFmt = _imp->cubemapFormatOutput.lock()->getValue();
        if (cubeFmt == 2) {
            outH = outW;  // Faces: single square face
        } else {
            int packing = _imp->cubemapPackingOutput.lock()->getValue();
            if (packing == eCubemapLLCross)    outH = outW * 3.0 / 4.0;
            else if (packing == eCubemap6x1)   outH = outW / 6.0;
            else if (packing == eCubemap3x2)   outH = outW * 2.0 / 3.0;
            else                                outH = outW * 3.0 / 4.0;
        }
    } else {
        double sensorW = _imp->sensorOutputW.lock()->getValue();
        double sensorH = _imp->sensorOutputH.lock()->getValue();
        outH = outW * sensorH / sensorW;
    }

    RectI outputFormat;
    outputFormat.x1 = 0;
    outputFormat.y1 = 0;
    outputFormat.x2 = (int)std::round(outW);
    outputFormat.y2 = (int)std::round(outH);
    metadata.setOutputFormat(outputFormat);

    return eStatusOK;
}


// ---- Render ----

StatusEnum
SphericalTransform::render(const RenderActionArgs& args)
{
    auto renderStart = std::chrono::high_resolution_clock::now();

    // Get source image
    auto fetchStart = std::chrono::high_resolution_clock::now();
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;
    auto fetchEnd = std::chrono::high_resolution_clock::now();
    double fetchMs = std::chrono::duration<double, std::milli>(fetchEnd - fetchStart).count();
    fprintf(stderr, "[SphericalTransform] Image fetch: %.1f ms (%dx%d)\n",
            fetchMs,
            srcImg->getBounds().x2 - srcImg->getBounds().x1,
            srcImg->getBounds().y2 - srcImg->getBounds().y1);
    fflush(stderr);

    // Get output image
    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    // Read parameters
    int inUIProj  = _imp->inputProjection.lock()->getValue();
    int outUIProj = _imp->outputProjection.lock()->getValue();
    int inFishType  = _imp->fisheyeTypeInput.lock()->getValue();
    int outFishType = _imp->fisheyeTypeOutput.lock()->getValue();
    CubemapPacking inCubePacking  = (CubemapPacking)_imp->cubemapPackingInput.lock()->getValue();
    CubemapPacking outCubePacking = (CubemapPacking)_imp->cubemapPackingOutput.lock()->getValue();
    int inCubeFormat  = _imp->cubemapFormatInput.lock()->getValue();
    int outCubeFormat = _imp->cubemapFormatOutput.lock()->getValue();
    int inCubeFace  = _imp->cubemapFaceInput.lock()->getValue();
    int outCubeFace = _imp->cubemapFaceOutput.lock()->getValue();

    double focalIn  = _imp->focalInput.lock()->getValue();
    double sensorInW = _imp->sensorInputW.lock()->getValue();
    double sensorInH = _imp->sensorInputH.lock()->getValue();

    double focalOut  = _imp->focalOutput.lock()->getValue();
    double sensorOutW = _imp->sensorOutputW.lock()->getValue();
    double sensorOutH = _imp->sensorOutputH.lock()->getValue();

    int filterType = _imp->filter.lock()->getValue();
    bool blackOut  = _imp->blackOutside.lock()->getValue();

    ProjectionType inProj  = mapUIToProjection(inUIProj, inFishType);
    ProjectionType outProj = mapUIToProjection(outUIProj, outFishType);

    // Build rotation matrices based on selected mode
    // Helper: build matrix from mode knobs
    auto buildMatrixForMode = [&](int mode, bool invert,
                                  KnobDoubleWPtr& pan, KnobDoubleWPtr& tilt, KnobDoubleWPtr& roll,
                                  KnobDoubleWPtr& lookDirX, KnobDoubleWPtr& lookDirY,
                                  KnobDoubleWPtr& lookAngle,
                                  KnobChoiceWPtr& rotOrd,
                                  KnobDoubleWPtr& raX, KnobDoubleWPtr& raY, KnobDoubleWPtr& raZ,
                                  double m[9]) {
        double sign = invert ? -1.0 : 1.0;
        switch (mode) {
        case eRotationModePanTiltRoll:
            buildRotationMatrix(sign * pan.lock()->getValue(),
                               sign * tilt.lock()->getValue(),
                               sign * roll.lock()->getValue(), m);
            break;
        case eRotationModeLook:
            buildRotationMatrixLook(sign * lookDirX.lock()->getValue(),
                                   sign * lookDirY.lock()->getValue(),
                                   sign * lookAngle.lock()->getValue(), m);
            break;
        case eRotationModeRotationAngles:
            buildRotationMatrixEuler(sign * raX.lock()->getValue(),
                                    sign * raY.lock()->getValue(),
                                    sign * raZ.lock()->getValue(),
                                    rotOrd.lock()->getValue(), m);
            break;
        default:
            // Identity
            for (int i = 0; i < 9; ++i) m[i] = (i % 4 == 0) ? 1.0 : 0.0;
            break;
        }
    };

    int inRotMode  = _imp->rotationModeInput.lock()->getValue();
    int outRotMode = _imp->rotationModeOutput.lock()->getValue();

    double matIn[9], matOut[9];
    buildMatrixForMode(inRotMode, false,
                       _imp->panInput, _imp->tiltInput, _imp->rollInput,
                       _imp->lookDirXInput, _imp->lookDirYInput, _imp->lookAngleInput,
                       _imp->rotOrderInput,
                       _imp->rotAngleXInput, _imp->rotAngleYInput, _imp->rotAngleZInput,
                       matIn);
    // Output rotation is inverted
    buildMatrixForMode(outRotMode, true,
                       _imp->panOutput, _imp->tiltOutput, _imp->rollOutput,
                       _imp->lookDirXOutput, _imp->lookDirYOutput, _imp->lookAngleOutput,
                       _imp->rotOrderOutput,
                       _imp->rotAngleXOutput, _imp->rotAngleYOutput, _imp->rotAngleZOutput,
                       matOut);

    // Source image info
    RectI srcBounds = srcImg->getBounds();
    int srcW = srcBounds.x2 - srcBounds.x1;
    int srcH = srcBounds.y2 - srcBounds.y1;
    int srcNComp = srcImg->getComponents().getNumComponents();

    // Output image info
    RectI outBounds = outImg->getBounds();
    int outW = outBounds.x2 - outBounds.x1;
    int outH = outBounds.y2 - outBounds.y1;

    if (outW <= 0 || outH <= 0 || srcW <= 0 || srcH <= 0) return eStatusOK;

    // Get source pixels
    Image::ReadAccess srcRa(srcImg.get());
    int nComp = std::min(srcNComp, 4);

    static const float zero[4] = {0, 0, 0, 0};

    // Helper: fetch pixel with bounds check, returns zero if out of bounds
    auto safePixel = [&](int cx, int cy) -> const float* {
        if (cx < srcBounds.x1 || cx >= srcBounds.x2 ||
            cy < srcBounds.y1 || cy >= srcBounds.y2) {
            return zero;
        }
        const float* p = (const float*)srcRa.pixelAt(cx, cy);
        return p ? p : zero;
    };

    // Process output pixels
    {
        Image::WriteAccess wa(outImg.get());

        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;

                // Normalize output pixel to [0,1]
                // Natron: y1=bottom, y2=top. Projections: v=0 top, v=1 bottom.
                double ou = ((double)(x - outBounds.x1) + 0.5) / (double)outW;
                double ov = 1.0 - ((double)(y - outBounds.y1) + 0.5) / (double)outH;

                // Step 1: Output pixel → 3D direction
                double dx, dy, dz;
                if (outProj == eProjectionCubemap && outCubeFormat == 2) {
                    // Faces mode: output is a single cube face
                    directionFromCubeFace(outCubeFace, ou, ov, dx, dy, dz);
                } else {
                    pixelToDirection(outProj, ou, ov,
                                     focalOut, sensorOutW, sensorOutH,
                                     outCubePacking,
                                     dx, dy, dz);
                }

                // Cubemap empty cell check (LL-Cross has unused cells)
                if (dx == 0 && dy == 0 && dz == 0) {
                    for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                    continue;
                }

                // Step 2: Apply output rotation (inverse)
                double rx, ry, rz;
                applyRotation(matOut, dx, dy, dz, rx, ry, rz);

                // Step 3: Apply input rotation
                double rx2, ry2, rz2;
                applyRotation(matIn, rx, ry, rz, rx2, ry2, rz2);

                // Step 4: 3D direction → source pixel
                double su, sv;
                bool valid;
                if (inProj == eProjectionCubemap && inCubeFormat == 2) {
                    // Faces mode input: map direction to single face UV
                    double fu, fv;
                    int hitFace = cubeFaceFromDirection(rx2, ry2, rz2, fu, fv);
                    valid = (hitFace == inCubeFace);
                    su = fu;
                    sv = fv;
                } else {
                    valid = directionToPixel(inProj, rx2, ry2, rz2,
                                              focalIn, sensorInW, sensorInH,
                                              inCubePacking,
                                              su, sv);
                }

                if (!valid) {
                    for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                    continue;
                }

                // Convert normalized [0,1] to source pixel coordinates
                // Flip sv back: projection v=0 is top, Natron y1 is bottom
                double srcFx = su * (double)srcW + (double)srcBounds.x1;
                double srcFy = (1.0 - sv) * (double)srcH + (double)srcBounds.y1;

                // Step 5: Sample source image with selected filter
                if (filterType == eFilterImpulse) {
                    // Nearest neighbor
                    const float* pix = safePixel((int)std::floor(srcFx),
                                                 (int)std::floor(srcFy));
                    for (int c = 0; c < nComp; ++c) dst[c] = pix[c];

                } else if (filterType == eFilterBilinear) {
                    // Bilinear: 2x2 kernel
                    double px = srcFx - 0.5;
                    double py = srcFy - 0.5;
                    int ix = (int)std::floor(px);
                    int iy = (int)std::floor(py);
                    float fx = (float)(px - ix);
                    float fy = (float)(py - iy);

                    const float* p00 = safePixel(ix,     iy);
                    const float* p10 = safePixel(ix + 1, iy);
                    const float* p01 = safePixel(ix,     iy + 1);
                    const float* p11 = safePixel(ix + 1, iy + 1);

                    float w00 = (1.0f - fx) * (1.0f - fy);
                    float w10 = fx * (1.0f - fy);
                    float w01 = (1.0f - fx) * fy;
                    float w11 = fx * fy;

                    for (int c = 0; c < nComp; ++c) {
                        dst[c] = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
                    }

                } else {
                    // Bicubic: 4x4 kernel (Cubic = Catmull-Rom, Mitchell = Mitchell-Netravali)
                    double px = srcFx - 0.5;
                    double py = srcFy - 0.5;
                    int ix = (int)std::floor(px);
                    int iy = (int)std::floor(py);
                    float fx = (float)(px - ix);
                    float fy = (float)(py - iy);

                    bool useMitchell = (filterType == eFilterMitchell);

                    // Compute 1D weights
                    float wx[4], wy[4];
                    for (int i = 0; i < 4; ++i) {
                        float tx = fx - (float)(i - 1);
                        float ty = fy - (float)(i - 1);
                        wx[i] = useMitchell ? mitchellWeight(tx) : cubicWeight(tx);
                        wy[i] = useMitchell ? mitchellWeight(ty) : cubicWeight(ty);
                    }

                    // Separable 4x4 convolution
                    for (int c = 0; c < nComp; ++c) {
                        float val = 0.0f;
                        float wSum = 0.0f;
                        for (int j = 0; j < 4; ++j) {
                            for (int i = 0; i < 4; ++i) {
                                float w = wx[i] * wy[j];
                                const float* p = safePixel(ix + i - 1, iy + j - 1);
                                val += p[c] * w;
                                wSum += w;
                            }
                        }
                        dst[c] = (wSum > 1e-6f) ? val / wSum : 0.0f;
                    }
                }
            }
        }
    }

    auto renderEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
    double transformMs = std::chrono::duration<double, std::milli>(renderEnd - fetchEnd).count();
    fprintf(stderr, "[SphericalTransform] Render: %.1f ms (transform: %.1f ms, total: %.1f ms) — %dx%d output\n",
            totalMs, transformMs, totalMs, outW, outH);
    fflush(stderr);

    return eStatusOK;
}


NATRON_NAMESPACE_EXIT

#include "moc_SphericalTransform.cpp"
