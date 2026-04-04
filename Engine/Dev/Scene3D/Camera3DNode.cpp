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

#include "Camera3DNode.h"

#include <cmath>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct Camera3DNodePrivate
{
    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;

    // Lens
    KnobDoubleWPtr focalLength;
    KnobDoubleWPtr hAperture;
    KnobDoubleWPtr vAperture;

    // Clipping
    KnobDoubleWPtr nearClip;
    KnobDoubleWPtr farClip;

    // Projection
    KnobChoiceWPtr projectionType; // 0=perspective, 1=orthographic

    // Depth of Field (F-Stop on Lens page; other DOF params on CyclesRender)
    KnobDoubleWPtr fStop;
};


Camera3DNode::Camera3DNode(NodePtr node)
    : EffectInstance(node)
    , _imp(new Camera3DNodePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Camera3DNode::~Camera3DNode()
{
}

std::string
Camera3DNode::getPluginDescription() const
{
    return tr("Standalone 3D camera.\n\n"
              "Connect to ScanlineRender's 'cam' input, Project3D's 'cam' input, "
              "or any other node that accepts a camera.\n\n"
              "All parameters are animatable for camera moves.\n"
              "The camera frustum is displayed in the 3D viewport.\n\n"
              "To use a tracked camera from Maya/Houdini, use ReadAlembicCamera instead.").toStdString();
}

void
Camera3DNode::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Camera3DNode::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Camera3DNode::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
Camera3DNode::initializeKnobs()
{
    // --- Transform page ---
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(5.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        k->setName("rotateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        k->setName("rotateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        k->setName("rotateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateZ = k;
    }

    // --- Lens page ---
    KnobPagePtr lensPage = AppManager::createKnob<KnobPage>(this, tr("Lens"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focal Length (mm)"));
        k->setName("focalLength"); k->setDefaultValue(50.0); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(10.0); k->setDisplayMaximum(200.0);
        lensPage->addKnob(k); _imp->focalLength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("H Aperture (mm)"));
        k->setName("hAperture"); k->setDefaultValue(24.576); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(5.0); k->setDisplayMaximum(70.0);
        lensPage->addKnob(k); _imp->hAperture = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("V Aperture (mm)"));
        k->setName("vAperture"); k->setDefaultValue(18.672); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(5.0); k->setDisplayMaximum(70.0);
        lensPage->addKnob(k); _imp->vAperture = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Near Clip"));
        k->setName("nearClip"); k->setDefaultValue(0.1); k->setAnimationEnabled(false);
        k->setMinimum(0.001); k->setDisplayMinimum(0.01); k->setDisplayMaximum(100.0);
        lensPage->addKnob(k); _imp->nearClip = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Far Clip"));
        k->setName("farClip"); k->setDefaultValue(10000.0); k->setAnimationEnabled(false);
        k->setMinimum(1.0); k->setDisplayMinimum(100.0); k->setDisplayMaximum(100000.0);
        lensPage->addKnob(k); _imp->farClip = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Projection"));
        k->setName("projectionType");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("perspective", "Perspective", "Standard perspective projection"));
        entries.push_back(ChoiceOption("orthographic", "Orthographic", "Orthographic (parallel) projection"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        lensPage->addKnob(k); _imp->projectionType = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("F-Stop"));
        k->setName("fStop");
        k->setDefaultValue(2.8);
        k->setMinimum(0.1); k->setDisplayMinimum(0.5); k->setDisplayMaximum(32.0);
        k->setHintToolTip(tr("Lens f-stop. Controls depth of field strength when DOF is enabled on CyclesRender."));
        k->setAnimationEnabled(true);
        lensPage->addKnob(k); _imp->fStop = k;
    }
}

// ==================== CameraProvider interface ====================

void
Camera3DNode::getCameraPosition(double time,
                                double& tx, double& ty, double& tz,
                                double& rx, double& ry, double& rz) const
{
    tx = _imp->translateX.lock()->getValueAtTime(time);
    ty = _imp->translateY.lock()->getValueAtTime(time);
    tz = _imp->translateZ.lock()->getValueAtTime(time);
    rx = _imp->rotateX.lock()->getValueAtTime(time);
    ry = _imp->rotateY.lock()->getValueAtTime(time);
    rz = _imp->rotateZ.lock()->getValueAtTime(time);
}

double
Camera3DNode::getCameraFocalLength(double time) const
{
    return _imp->focalLength.lock()->getValueAtTime(time);
}

double
Camera3DNode::getCameraHAperture(double time) const
{
    return _imp->hAperture.lock()->getValueAtTime(time);
}

double
Camera3DNode::getCameraVAperture(double time) const
{
    return _imp->vAperture.lock()->getValueAtTime(time);
}

double
Camera3DNode::getCameraNear(double time) const
{
    return _imp->nearClip.lock()->getValueAtTime(time);
}

double
Camera3DNode::getCameraFar(double time) const
{
    return _imp->farClip.lock()->getValueAtTime(time);
}

double
Camera3DNode::getCameraFStop(double time) const
{
    KnobDoublePtr k = _imp->fStop.lock();
    return k ? k->getValueAtTime(time) : 2.8;
}

// ==================== RoD / Render ====================

StatusEnum
Camera3DNode::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                    ViewIdx /*view*/, RectD* rod)
{
    // Camera has no image output — return a 1x1 dummy
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Camera3DNode::render(const RenderActionArgs& args)
{
    // Camera produces no image — just fill output with black
    if (args.outputPlanes.empty()) return eStatusOK;

    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.0f; }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Camera3DNode.cpp"
