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
#include "RotationConventions.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct Camera3DNodePrivate
{
    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobBoolWPtr lockTransform;

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

    // Viewport display only — length of the frustum gizmo drawn in the 3D viewport.
    KnobDoubleWPtr frustumDisplayLength;
    KnobBoolWPtr showMotionTrail;

    // Sensor / project-format aspect info — discoverable mismatch + 1-click fix.
    KnobStringWPtr aspectInfo;
    KnobButtonWPtr matchAspectButton;

    // Motion page
    KnobBoolWPtr   handheldEnable;
    KnobDoubleWPtr handheldAmount, handheldRotation, handheldTranslation;
    KnobDoubleWPtr handheldFrequency, handheldRoughness, handheldRoll;
    KnobIntWPtr    handheldSeed;
    KnobBoolWPtr   pushEnable;
    KnobChoiceWPtr pushMode;        // 0 dolly (move along the view axis), 1 zoom (focal length)
    KnobDoubleWPtr pushDistance, pushFocalDelta;
    KnobIntWPtr    pushStart, pushEnd;
    KnobChoiceWPtr pushEase;        // 0 linear, 1 ease in-out, 2 ease in, 3 ease out
};

// ==================== Motion layer helpers ====================

// Value noise (same construction as Volume3D's smoothNoise3D), used 1-D along
// time with a per-channel offset so pan / tilt / roll / x / y / z decorrelate.
static float motionHash3D(float x, float y, float z)
{
    float n = sinf(x * 127.1f + y * 311.7f + z * 74.7f) * 43758.5453f;
    return n - floorf(n);
}
static float motionSmoothNoise(float x, float y, float z)
{
    float ix = floorf(x), iy = floorf(y), iz = floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    fz = fz * fz * (3.0f - 2.0f * fz);
    float v000 = motionHash3D(ix, iy, iz),         v100 = motionHash3D(ix + 1, iy, iz);
    float v010 = motionHash3D(ix, iy + 1, iz),     v110 = motionHash3D(ix + 1, iy + 1, iz);
    float v001 = motionHash3D(ix, iy, iz + 1),     v101 = motionHash3D(ix + 1, iy, iz + 1);
    float v011 = motionHash3D(ix, iy + 1, iz + 1), v111 = motionHash3D(ix + 1, iy + 1, iz + 1);
    float v00 = v000 + fx * (v100 - v000), v10 = v010 + fx * (v110 - v010);
    float v01 = v001 + fx * (v101 - v001), v11 = v011 + fx * (v111 - v011);
    float v0 = v00 + fy * (v10 - v00),     v1 = v01 + fy * (v11 - v01);
    return v0 + fz * (v1 - v0);
}
// Three-octave fBm in [-1, 1]: octave 0 is the slow drift, the higher octaves
// the jitter; `roughness` (0..1) is the weight ratio between octaves, so 0 is
// pure drift and 1 is equal-weight jitter.
static double motionFbm(double t, double channel, double roughness)
{
    static const float kFreq[3] = { 1.0f, 2.17f, 4.73f };
    double n = 0.0, wsum = 0.0, w = 1.0;
    for (int o = 0; o < 3; ++o) {
        const float v = motionSmoothNoise((float)(t * kFreq[o]) + 17.3f * o, (float)channel * 7.31f + 3.7f * o, 0.5f);
        n += w * ((double)v - 0.5) * 2.0;
        wsum += w;
        w *= roughness;
    }
    return wsum > 0.0 ? n / wsum : 0.0;
}
static double motionEase(double s, int mode)
{
    s = std::max(0.0, std::min(1.0, s));
    switch (mode) {
        case 1: return s * s * (3.0 - 2.0 * s);       // ease in-out
        case 2: return s * s;                         // ease in
        case 3: return 1.0 - (1.0 - s) * (1.0 - s);   // ease out
        default: return s;                            // linear
    }
}


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
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Lock Transform"));
        k->setName("lockTransform");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("Lock Translate/Rotate against edits — the panel knobs, the "
                             "viewport gizmo AND look-through navigation (orbit/pan/dolly "
                             "while viewing through this camera). Protects imported or "
                             "tracked animation (Alembic cameras, CameraTracker solves) "
                             "from being accidentally moved. Set automatically by "
                             "CameraTracker's Create Camera3D."));
        xformPage->addKnob(k); _imp->lockTransform = k;
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

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Frustum Display Length"));
        k->setName("frustumDisplayLength");
        k->setDefaultValue(3.0);
        k->setMinimum(0.1); k->setDisplayMinimum(0.5); k->setDisplayMaximum(20.0);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false); // viewport gizmo only — never triggers a re-render
        k->setHintToolTip(tr("Length of the camera frustum drawn in the 3D viewport. "
                             "Display only — does not affect rendering or the camera itself."));
        lensPage->addKnob(k); _imp->frustumDisplayLength = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Show Motion Trail"));
        k->setName("showMotionTrail");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false); // viewport only
        k->setHintToolTip(tr("Draw the path this camera's keyframes produce in the 3D viewport when the "
                             "camera is selected: one dot per frame, larger dots on keyframes, a marker at "
                             "the current frame. Display only."));
        lensPage->addKnob(k); _imp->showMotionTrail = k;
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

    // --- Motion page: procedural layers on top of the keyframes ---
    KnobPagePtr motionPage = AppManager::createKnob<KnobPage>(this, tr("Motion"));
    auto mkDouble = [&](const char* name, const QString& label, double def, double dmin, double dmax,
                        const QString& hint) -> KnobDoublePtr {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, label);
        k->setName(name); k->setDefaultValue(def); k->setAnimationEnabled(true);
        k->setDisplayMinimum(dmin); k->setDisplayMaximum(dmax);
        k->setHintToolTip(hint);
        motionPage->addKnob(k);
        return k;
    };
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Handheld"));
        sep->setName("sepHandheld"); motionPage->addKnob(sep);
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Handheld"));
        k->setName("handheldEnable"); k->setDefaultValue(false); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Add operator-style shake on top of the keyframed camera: layered noise on "
                             "pan / tilt / roll and a little on position, in camera space. The keyframes "
                             "are not changed; every renderer and the 3D viewport see the result."));
        motionPage->addKnob(k); _imp->handheldEnable = k;
    }
    _imp->handheldAmount = mkDouble("handheldAmount", tr("Amount"), 1.0, 0.0, 2.0,
        tr("Master scale for the shake. Animate it to ramp the shake in and out."));
    _imp->handheldRotation = mkDouble("handheldRotation", tr("Rotation (deg)"), 0.5, 0.0, 5.0,
        tr("Peak pan / tilt in degrees. 0.2-0.5 reads as a steady operator, 1-2 as walking."));
    _imp->handheldTranslation = mkDouble("handheldTranslation", tr("Translation"), 0.02, 0.0, 1.0,
        tr("Peak position drift in world units, applied sideways / up-down / forward in camera space."));
    _imp->handheldFrequency = mkDouble("handheldFrequency", tr("Frequency (Hz)"), 1.0, 0.1, 10.0,
        tr("Speed of the slowest layer of the shake, in cycles per second at the project frame rate."));
    _imp->handheldRoughness = mkDouble("handheldRoughness", tr("Roughness"), 0.5, 0.0, 1.0,
        tr("Mix of fast jitter over the slow drift. 0 = smooth float, 1 = nervous."));
    _imp->handheldRoll = mkDouble("handheldRoll", tr("Roll Weight"), 0.3, 0.0, 1.0,
        tr("How much of the rotation amount goes into roll. Real handheld rolls less than it pans."));
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("handheldSeed"); k->setDefaultValue(0); k->setDisplayMinimum(0); k->setDisplayMaximum(100);
        k->setHintToolTip(tr("Different seeds give different shakes of the same character."));
        motionPage->addKnob(k); _imp->handheldSeed = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Push-in"));
        sep->setName("sepPush"); motionPage->addKnob(sep);
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Push-in"));
        k->setName("pushEnable"); k->setDefaultValue(false); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Move the camera along its own view axis (or change the focal length) between "
                             "two frames, with an ease, on top of the keyframes."));
        motionPage->addKnob(k); _imp->pushEnable = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
        k->setName("pushMode");
        std::vector<ChoiceOption> e;
        e.push_back(ChoiceOption("Dolly", "", "Move the camera forward along its view axis"));
        e.push_back(ChoiceOption("Zoom", "", "Change the focal length instead (dolly-zoom when combined with keyed position)"));
        k->populateChoices(e); k->setDefaultValue(0);
        motionPage->addKnob(k); _imp->pushMode = k;
    }
    _imp->pushDistance = mkDouble("pushDistance", tr("Distance"), 1.0, -20.0, 20.0,
        tr("Dolly: how far the camera has moved along its view axis by the end frame. Positive pushes in."));
    _imp->pushFocalDelta = mkDouble("pushFocalDelta", tr("Focal Change (mm)"), 15.0, -50.0, 100.0,
        tr("Zoom: focal length added by the end frame. Positive zooms in."));
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Start Frame"));
        k->setName("pushStart"); k->setDefaultValue(1); k->setDisplayMinimum(0); k->setDisplayMaximum(500);
        motionPage->addKnob(k); _imp->pushStart = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("End Frame"));
        k->setName("pushEnd"); k->setDefaultValue(50); k->setDisplayMinimum(0); k->setDisplayMaximum(500);
        motionPage->addKnob(k); _imp->pushEnd = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Ease"));
        k->setName("pushEase");
        std::vector<ChoiceOption> e;
        e.push_back(ChoiceOption("Linear", "", ""));
        e.push_back(ChoiceOption("Ease In-Out", "", "Slow start and end"));
        e.push_back(ChoiceOption("Ease In", "", "Slow start"));
        e.push_back(ChoiceOption("Ease Out", "", "Slow end"));
        k->populateChoices(e); k->setDefaultValue(1);
        motionPage->addKnob(k); _imp->pushEase = k;
    }

    refreshAspectInfo();
}

// ==================== Aspect info + Match Project Aspect button ====================

bool
Camera3DNode::isTransformLocked() const
{
    KnobBoolPtr lk = _imp->lockTransform.lock();
    return lk && lk->getValue();
}

void
Camera3DNode::applyTransformLock()
{
    const bool locked = isTransformLocked();
    KnobDoubleWPtr knobs[6] = { _imp->translateX, _imp->translateY, _imp->translateZ,
                                _imp->rotateX, _imp->rotateY, _imp->rotateZ };
    for (int i = 0; i < 6; ++i) {
        if (KnobDoublePtr kk = knobs[i].lock()) {
            kk->setAllDimensionsEnabled(!locked);
        }
    }
}

void
Camera3DNode::onKnobsLoaded()
{
    // Re-apply the lock's enabled/disabled state after project load.
    applyTransformLock();
}

bool
Camera3DNode::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/, double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (!k) return false;

    if (KnobBoolPtr lockK = _imp->lockTransform.lock()) {
        if (k == lockK.get()) {
            applyTransformLock();
            return true;
        }
    }

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
    double focal = 0.0;   // unused here; the focal getter applies its own layer
    applyMotionLayers(time, tx, ty, tz, rx, ry, rz, focal);
}

double
Camera3DNode::getCameraFocalLength(double time) const
{
    double focal = _imp->focalLength.lock()->getValueAtTime(time);
    double tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0;
    applyMotionLayers(time, tx, ty, tz, rx, ry, rz, focal);
    return focal;
}

void
Camera3DNode::applyMotionLayers(double time, double& tx, double& ty, double& tz,
                                double& rx, double& ry, double& rz, double& focal) const
{
    KnobBoolPtr hh = _imp->handheldEnable.lock();
    KnobBoolPtr pu = _imp->pushEnable.lock();
    const bool handheld = hh && hh->getValueAtTime(time);
    const bool push = pu && pu->getValue();
    if (!handheld && !push) return;

    // Camera-space axes at the keyframed rotation: column-vector R = Rz*Ry*Rx,
    // camera looks down its local -Z.
    double R[3][3];
    RotationConventions::compose(rx, ry, rz, R);
    const double fwd[3] = { -R[0][2], -R[1][2], -R[2][2] };

    if (push) {
        const int f0 = _imp->pushStart.lock()->getValue();
        const int f1 = _imp->pushEnd.lock()->getValue();
        const double span = (double)(f1 - f0);
        const double s = motionEase(span > 0.0 ? (time - f0) / span : (time >= f1 ? 1.0 : 0.0),
                                    _imp->pushEase.lock()->getValue());
        if (_imp->pushMode.lock()->getValue() == 0) {
            const double d = _imp->pushDistance.lock()->getValueAtTime(time) * s;
            tx += fwd[0] * d; ty += fwd[1] * d; tz += fwd[2] * d;
        } else {
            focal += _imp->pushFocalDelta.lock()->getValueAtTime(time) * s;
        }
    }

    if (handheld) {
        const double amount = _imp->handheldAmount.lock()->getValueAtTime(time);
        if (amount != 0.0) {
            double fps = 24.0;
            if (AppInstancePtr app = getApp()) {
                if (ProjectPtr proj = app->getProject()) {
                    const double f = proj->getProjectFrameRate();
                    if (f > 0.0) fps = f;
                }
            }
            const double freq = _imp->handheldFrequency.lock()->getValueAtTime(time);
            const double rough = std::max(0.0, std::min(1.0, _imp->handheldRoughness.lock()->getValueAtTime(time)));
            const double seed = (double)_imp->handheldSeed.lock()->getValue() * 13.37;
            const double t = time / fps * freq + seed;
            const double rotAmp = _imp->handheldRotation.lock()->getValueAtTime(time) * amount;
            const double trAmp  = _imp->handheldTranslation.lock()->getValueAtTime(time) * amount;
            const double roll   = _imp->handheldRoll.lock()->getValueAtTime(time);
            // Rotation: tilt (x), pan (y), roll (z, weighted).
            rx += rotAmp * motionFbm(t, 1.0, rough);
            ry += rotAmp * motionFbm(t, 2.0, rough);
            rz += rotAmp * roll * motionFbm(t, 3.0, rough);
            // Translation in camera space (sideways, up, forward) -> world.
            const double ox = trAmp * motionFbm(t, 4.0, rough);
            const double oy = trAmp * motionFbm(t, 5.0, rough);
            const double oz = trAmp * 0.5 * motionFbm(t, 6.0, rough);
            tx += R[0][0] * ox + R[0][1] * oy + R[0][2] * oz;
            ty += R[1][0] * ox + R[1][1] * oy + R[1][2] * oz;
            tz += R[2][0] * ox + R[2][1] * oy + R[2][2] * oz;
        }
    }
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
