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
#include <sstream>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Format.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../Project.h"
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

    // Sensor / project-format aspect info — discoverable mismatch + 1-click fix.
    KnobStringWPtr aspectInfo;
    KnobButtonWPtr matchAspectButton;
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
        k->setName("vAperture");
        // Pre-compute the V aperture so the new camera matches the project's
        // aspect ratio from the start (24.576mm H × (projectH / projectW)).
        // Saves the user clicking "Match Project Aspect" on every camera they
        // create. Falls back to 18.672mm (35mm Academy) when no project is
        // available — same as the legacy default.
        double defaultVAp = 18.672;
        AppInstancePtr app = getApp();
        if (app && app->getProject()) {
            Format fmt;
            app->getProject()->getProjectDefaultFormat(&fmt);
            const double pw = (double)fmt.width();
            const double ph = (double)fmt.height();
            if (pw > 0 && ph > 0) {
                defaultVAp = 24.576 * (ph / pw);
            }
        }
        k->setDefaultValue(defaultVAp); k->setAnimationEnabled(true);
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

    // --- Aspect info (informational, refreshed live) ---
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Aspect Info"));
        k->setName("aspectInfo"); k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false); k->setIsPersistent(false);
        k->setAsMultiLine();
        k->setHintToolTip(tr("Sensor aspect (H/V aperture) vs the current project format aspect. "
                              "Use the Match Project Aspect button to align the camera's V aperture to the project."));
        lensPage->addKnob(k); _imp->aspectInfo = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Match Project Aspect"));
        k->setName("matchAspect");
        k->setHintToolTip(tr("Set V Aperture = H Aperture x (project height / project width). "
                              "Keeps H Aperture and focal length untouched."));
        lensPage->addKnob(k); _imp->matchAspectButton = k;
    }

    refreshAspectInfo();
}

// ==================== Aspect info + Match Project Aspect button ====================

bool
Camera3DNode::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/, double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (!k) return false;

    KnobIPtr hapKnob = _imp->hAperture.lock();
    KnobIPtr vapKnob = _imp->vAperture.lock();
    KnobIPtr matchKnob = _imp->matchAspectButton.lock();

    // "Match Project Aspect" button → compute new V Aperture from project format.
    if (matchKnob && k == matchKnob.get()) {
        AppInstancePtr app = getApp();
        if (app && app->getProject()) {
            Format fmt;
            app->getProject()->getProjectDefaultFormat(&fmt);
            const double pw = fmt.width();
            const double ph = fmt.height();
            KnobDoublePtr hap = _imp->hAperture.lock();
            KnobDoublePtr vap = _imp->vAperture.lock();
            if (pw > 0 && ph > 0 && hap && vap) {
                const double hA = hap->getValue();
                const double newVA = hA * (ph / pw);
                vap->setValue(newVA);
                refreshAspectInfo();
            }
        }
        return true;
    }

    // Aperture knob changes → refresh info string.
    if ((hapKnob && k == hapKnob.get()) || (vapKnob && k == vapKnob.get())) {
        refreshAspectInfo();
        return true;
    }

    return false;
}

void
Camera3DNode::refreshAspectInfo()
{
    KnobDoublePtr hap = _imp->hAperture.lock();
    KnobDoublePtr vap = _imp->vAperture.lock();
    KnobStringPtr info = _imp->aspectInfo.lock();
    if (!hap || !vap || !info) return;

    const double hA = hap->getValue();
    const double vA = vap->getValue();
    const double sensorAspect = (vA > 1e-6) ? (hA / vA) : 0.0;

    int pw = 0, ph = 0;
    double projAspect = 0.0;
    AppInstancePtr app = getApp();
    if (app && app->getProject()) {
        Format fmt;
        app->getProject()->getProjectDefaultFormat(&fmt);
        pw = fmt.width();
        ph = fmt.height();
        if (ph > 0) projAspect = (double)pw / (double)ph;
    }

    std::ostringstream ss;
    ss.precision(3);
    ss << std::fixed;
    ss << "Sensor: " << hA << " x " << vA << " mm  (aspect " << sensorAspect << ")\n";
    if (pw > 0 && ph > 0) {
        ss << "Project: " << pw << " x " << ph << "  (aspect " << projAspect << ")";
        if (sensorAspect > 0 && projAspect > 0) {
            const double diff = std::fabs(sensorAspect - projAspect) / projAspect;
            if (diff > 0.001) {
                ss << "\n[!] Sensor aspect differs from project aspect."
                   << " Click \"Match Project Aspect\" to align the V aperture.";
            } else {
                ss << "\nSensor aspect matches project.";
            }
        }
    } else {
        ss << "Project format: unknown";
    }
    info->setValue(ss.str());
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
