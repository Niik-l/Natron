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

#include "ReadAlembicCamera.h"

#include <cassert>
#include <cmath>
#include <sstream>
#include <vector>

#include "RotationConventions.h"

#include "../../AppInstance.h"
#include "../../ChoiceOption.h"
#include "../../Format.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../Project.h"
#include "../../ViewIdx.h"

#ifdef NATRON_HAVE_ALEMBIC
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

#ifdef NATRON_HAVE_ALEMBIC
// Recursively find camera objects in the Alembic hierarchy
static void
findCamerasRecursive(const Alembic::AbcGeom::IObject& obj,
                     const std::string& parentPath,
                     std::vector<std::string>& cameraPaths)
{
    using namespace Alembic::AbcGeom;

    std::string fullPath = parentPath.empty() ? obj.getName() : (parentPath + "/" + obj.getName());

    if (ICamera::matches(obj.getHeader())) {
        cameraPaths.push_back(fullPath);
    }

    for (size_t i = 0; i < obj.getNumChildren(); ++i) {
        findCamerasRecursive(obj.getChild(i), fullPath, cameraPaths);
    }
}
#endif


struct ReadAlembicCameraPrivate
{
    KnobFileWPtr filePath;
    KnobChoiceWPtr objectPath;
    KnobButtonWPtr reloadBtn;
    KnobIntWPtr frameOffset;
    KnobChoiceWPtr timeMode;

    // Output knobs (animated)
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobBoolWPtr lockTransform;
    KnobDoubleWPtr focalLength;
    KnobDoubleWPtr hAperture, vAperture;
    KnobDoubleWPtr nearClipKnob, farClipKnob;
    KnobStringWPtr info;

    // Sensor / project-format aspect info — discoverable mismatch + 1-click fix.
    KnobStringWPtr aspectInfo;
    KnobButtonWPtr matchAspectButton;

    // Cached camera paths
    std::vector<std::string> cameraPaths;
    std::string loadedFilePath;
};


ReadAlembicCamera::ReadAlembicCamera(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadAlembicCameraPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ReadAlembicCamera::~ReadAlembicCamera()
{
}

std::string
ReadAlembicCamera::getPluginDescription() const
{
    return
#ifdef NATRON_HAVE_ALEMBIC
    tr("Import animated cameras from Alembic (.abc) files.\n\n"
       "Set the file path to an .abc file and select the camera from the dropdown. "
       "Camera transform (translate/rotate) and intrinsics (focal length, aperture) "
       "are exposed as animated knob values.\n\n"
       "Use expression links to connect these values to other nodes.\n"
       "The camera frustum is visualized in the 3D viewport when this node is selected.\n\n"
       "Supports exports from Maya, Houdini, 3DEqualizer, and Blender.").toStdString();
#else
    tr("ReadAlembicCamera requires the Alembic library. Rebuild Natron with Alembic support.").toStdString();
#endif
}

void
ReadAlembicCamera::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ReadAlembicCamera::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadAlembicCamera::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ReadAlembicCamera::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("File"));

    KnobFilePtr fp = AppManager::createKnob<KnobFile>(this, tr("File"));
    fp->setName("filename");
    fp->setHintToolTip(tr("Path to the Alembic (.abc) file."));
    fp->setAnimationEnabled(false);
    page->addKnob(fp);
    _imp->filePath = fp;

    KnobChoicePtr obj = AppManager::createKnob<KnobChoice>(this, tr("Camera"));
    obj->setName("objectPath");
    obj->setHintToolTip(tr("Select which camera to use from the Alembic file."));
    page->addKnob(obj);
    _imp->objectPath = obj;

    KnobButtonPtr reload = AppManager::createKnob<KnobButton>(this, tr("Reload"));
    reload->setName("reload");
    reload->setHintToolTip(tr("Re-read the Alembic file."));
    page->addKnob(reload);
    _imp->reloadBtn = reload;

    KnobIntPtr foff = AppManager::createKnob<KnobInt>(this, tr("Frame Offset"));
    foff->setName("frameOffset");
    foff->setDefaultValue(0);
    foff->setHintToolTip(tr("Offset the Alembic animation relative to the Natron timeline."));
    page->addKnob(foff);
    _imp->frameOffset = foff;

    KnobChoicePtr tm = AppManager::createKnob<KnobChoice>(this, tr("Time Mode"));
    tm->setName("timeMode");
    {
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("frame_by_frame", "Frame-by-frame",
            "Each Alembic sample becomes one keyframe at consecutive integer Natron frames (sample 0 -> frame 1, sample 1 -> frame 2, ...). Use this for frame-accurate matching with Maya/Blender/Houdini regardless of fps."));
        opts.push_back(ChoiceOption("time_based", "Time-based",
            "Map Alembic sample times to Natron timeline using the project frame rate. Preserves real-world timing but may interpolate between samples when source and project fps differ."));
        tm->populateChoices(opts);
    }
    tm->setDefaultValue(1); // Default to Time-based — matches Maya/Houdini behaviour.
    tm->setHintToolTip(tr("How Alembic sample times are mapped onto the Natron timeline."));
    page->addKnob(tm);
    _imp->timeMode = tm;

    // Camera output page
    KnobPagePtr camPage = AppManager::createKnob<KnobPage>(this, tr("Camera"));

    KnobDoublePtr tx = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
    tx->setName("translateX"); tx->setAnimationEnabled(true); tx->setEvaluateOnChange(false);
    camPage->addKnob(tx); _imp->translateX = tx;

    KnobDoublePtr ty = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
    ty->setName("translateY"); ty->setAnimationEnabled(true); ty->setEvaluateOnChange(false);
    camPage->addKnob(ty); _imp->translateY = ty;

    KnobDoublePtr tz = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
    tz->setName("translateZ"); tz->setAnimationEnabled(true); tz->setEvaluateOnChange(false);
    camPage->addKnob(tz); _imp->translateZ = tz;

    KnobDoublePtr rx = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
    rx->setName("rotateX"); rx->setAnimationEnabled(true); rx->setEvaluateOnChange(false);
    camPage->addKnob(rx); _imp->rotateX = rx;

    KnobDoublePtr ry = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
    ry->setName("rotateY"); ry->setAnimationEnabled(true); ry->setEvaluateOnChange(false);
    camPage->addKnob(ry); _imp->rotateY = ry;

    KnobDoublePtr rz = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
    rz->setName("rotateZ"); rz->setAnimationEnabled(true); rz->setEvaluateOnChange(false);
    camPage->addKnob(rz); _imp->rotateZ = rz;

    KnobBoolPtr lock = AppManager::createKnob<KnobBool>(this, tr("Lock Transform"));
    lock->setName("lockTransform");
    lock->setDefaultValue(true);
    lock->setAnimationEnabled(false);
    lock->setEvaluateOnChange(false);
    lock->setHintToolTip(tr("Lock the imported Translate/Rotate keyframes against accidental "
                            "edits (panel + viewport). On by default — the animation comes "
                            "from the Alembic cache and is rebaked on Reload. Untick to "
                            "deliberately offset the camera by hand."));
    camPage->addKnob(lock); _imp->lockTransform = lock;

    KnobDoublePtr fl = AppManager::createKnob<KnobDouble>(this, tr("Focal Length"));
    fl->setName("focalLength"); fl->setAnimationEnabled(true); fl->setEvaluateOnChange(false);
    fl->setDefaultValue(50.0);
    camPage->addKnob(fl); _imp->focalLength = fl;

    KnobDoublePtr ha = AppManager::createKnob<KnobDouble>(this, tr("H Aperture (mm)"));
    ha->setName("hAperture"); ha->setAnimationEnabled(true); ha->setEvaluateOnChange(false);
    ha->setDefaultValue(36.0);
    camPage->addKnob(ha); _imp->hAperture = ha;

    KnobDoublePtr va = AppManager::createKnob<KnobDouble>(this, tr("V Aperture (mm)"));
    va->setName("vAperture"); va->setAnimationEnabled(true); va->setEvaluateOnChange(false);
    va->setDefaultValue(24.0);
    camPage->addKnob(va); _imp->vAperture = va;

    KnobDoublePtr nc = AppManager::createKnob<KnobDouble>(this, tr("Near Clip"));
    nc->setName("nearClip"); nc->setAnimationEnabled(true); nc->setEvaluateOnChange(false);
    nc->setDefaultValue(0.1);
    camPage->addKnob(nc); _imp->nearClipKnob = nc;

    KnobDoublePtr fc = AppManager::createKnob<KnobDouble>(this, tr("Far Clip"));
    fc->setName("farClip"); fc->setAnimationEnabled(true); fc->setEvaluateOnChange(false);
    fc->setDefaultValue(10000.0);
    camPage->addKnob(fc); _imp->farClipKnob = fc;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info"); info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false); info->setIsPersistent(false);
    info->setDefaultValue("Set file path to an .abc file.");
    camPage->addKnob(info); _imp->info = info;

    // --- Aspect info (informational, refreshed live) ---
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Aspect Info"));
        k->setName("aspectInfo"); k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false); k->setIsPersistent(false);
        k->setAsMultiLine();
        k->setHintToolTip(tr("Sensor aspect (H/V aperture) vs the current project format aspect. "
                             "If they differ, the image will be stretched or cropped — click "
                             "\"Match Project Aspect\" to set V Aperture so it matches."));
        camPage->addKnob(k); _imp->aspectInfo = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Match Project Aspect"));
        k->setName("matchAspect");
        k->setHintToolTip(tr("Set V Aperture = H Aperture x (project height / project width). "
                             "This adjusts the imported camera's vertical aperture so its sensor "
                             "aspect matches the current project format."));
        camPage->addKnob(k); _imp->matchAspectButton = k;
    }
    refreshAspectInfo();
    applyTransformLock();
}

void
ReadAlembicCamera::applyTransformLock()
{
    KnobBoolPtr lk = _imp->lockTransform.lock();
    const bool locked = lk && lk->getValue();
    // Only the GUI edit paths are disabled — loadAlembicFile's setValueAtTime
    // bake still writes (enabled state is a GUI-level gate).
    KnobDoubleWPtr knobs[6] = { _imp->translateX, _imp->translateY, _imp->translateZ,
                                _imp->rotateX, _imp->rotateY, _imp->rotateZ };
    for (int i = 0; i < 6; ++i) {
        if (KnobDoublePtr kk = knobs[i].lock()) {
            kk->setAllDimensionsEnabled(!locked);
        }
    }
}

bool
ReadAlembicCamera::knobChanged(KnobI* k,
                               ValueChangedReasonEnum /*reason*/,
                               ViewSpec /*view*/,
                               double /*time*/,
                               bool /*originatedFromMainThread*/)
{
    if (KnobBoolPtr lockK = _imp->lockTransform.lock()) {
        if (k == lockK.get()) {
            applyTransformLock();
            return true;
        }
    }

    if (_imp->filePath.lock().get() == k || _imp->reloadBtn.lock().get() == k) {
        std::string path = _imp->filePath.lock()->getValue();
        if (!path.empty()) {
            loadAlembicFile(path);
        }
        return true;
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
ReadAlembicCamera::refreshAspectInfo()
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

void
ReadAlembicCamera::onKnobsLoaded()
{
    // After a project load the file-path knob is restored but no knobChanged
    // fires, so the camera is never read — the user used to have to hit "Reload".
    // Re-read it here so the saved scene's camera appears immediately.
    KnobFilePtr fp = _imp->filePath.lock();
    if (fp) {
        const std::string path = fp->getValue();
        if (!path.empty()) {
            loadAlembicFile(path);
        }
    }
    // Re-apply the saved lock state to the knob enabled flags.
    applyTransformLock();
}

void
ReadAlembicCamera::loadAlembicFile(const std::string& path)
{
#ifdef NATRON_HAVE_ALEMBIC
    using namespace Alembic::AbcGeom;

    try {
        IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        IObject top = archive.getTop();

        _imp->cameraPaths.clear();
        findCamerasRecursive(top, "", _imp->cameraPaths);

        if (_imp->cameraPaths.empty()) {
            setPersistentMessage(eMessageTypeError, "No cameras found in " + path);
            return;
        }

        // Populate camera choice dropdown
        KnobChoicePtr objKnob = _imp->objectPath.lock();
        std::vector<ChoiceOption> entries;
        for (const std::string& cp : _imp->cameraPaths) {
            entries.push_back(ChoiceOption(cp, "", ""));
        }
        objKnob->populateChoices(entries);
        objKnob->setDefaultValue(0);

        // Load the first camera's data
        std::string selectedPath = _imp->cameraPaths[0];

        // Navigate to the camera object
        IObject obj = top;
        // Split path and navigate
        std::istringstream iss(selectedPath);
        std::string token;
        while (std::getline(iss, token, '/')) {
            if (token.empty()) continue;
            for (size_t i = 0; i < obj.getNumChildren(); ++i) {
                if (obj.getChild(i).getName() == token) {
                    obj = obj.getChild(i);
                    break;
                }
            }
        }

        if (!ICamera::matches(obj.getHeader())) {
            setPersistentMessage(eMessageTypeError, "Selected object is not a camera: " + selectedPath);
            return;
        }

        ICamera cam(obj);
        ICameraSchema camSchema = cam.getSchema();
        const int frameOffset = _imp->frameOffset.lock()->getValue();
        const int timeMode = _imp->timeMode.lock()->getValue(); // 0=frame-by-frame, 1=time-based
        const double projectFps = getApp() ? getApp()->getProjectFrameRate() : 24.0;

        // Read camera intrinsics (may have fewer samples than Xform)
        size_t numCamSamples = camSchema.getNumSamples();
        Alembic::AbcCoreAbstract::TimeSamplingPtr camTimeSampling = camSchema.getTimeSampling();

        // Detect source fps from the timeSampling (uniform case only; reported in Info field).
        double sourceFps = 0.0;
        if (camTimeSampling) {
            const auto& tst = camTimeSampling->getTimeSamplingType();
            if (tst.isUniform() && tst.getTimePerCycle() > 0.0) {
                sourceFps = 1.0 / tst.getTimePerCycle();
            }
        }

        auto sampleIndexToNatronFrame = [&](size_t i, double abcTime) -> double {
            if (timeMode == 1) {
                // Time-based: abcTime is already absolute time; multiply by project fps
                // for the absolute Natron frame. No +1 — Maya stores frame N at time N/fps,
                // so abcTime*fps already equals N. Adding +1 would shift the whole track
                // by one frame and miss the camera's actual range.
                return abcTime * projectFps + frameOffset;
            }
            // Frame-by-frame: one keyframe per sample at consecutive integer frames,
            // ignoring abcTime. Sample 0 lands on frame frameOffset+1.
            return (double)i + frameOffset + 1.0;
        };

        for (size_t i = 0; i < numCamSamples; ++i) {
            double abcTime = camTimeSampling->getSampleTime(i);
            double natronFrame = sampleIndexToNatronFrame(i, abcTime);

            ISampleSelector sel((Alembic::AbcCoreAbstract::index_t)i);
            CameraSample sample;
            camSchema.get(sample, sel);

            double fl = sample.getFocalLength();
            double hAp = sample.getHorizontalAperture() * 10.0; // cm → mm
            double vAp = sample.getVerticalAperture() * 10.0;   // cm → mm
            double nearC = sample.getNearClippingPlane();
            double farC = sample.getFarClippingPlane();

            if (numCamSamples == 1) {
                // Static camera intrinsics — set as default values (no keyframes)
                _imp->focalLength.lock()->setValue(fl);
                _imp->hAperture.lock()->setValue(hAp);
                _imp->vAperture.lock()->setValue(vAp);
                _imp->nearClipKnob.lock()->setValue(nearC);
                _imp->farClipKnob.lock()->setValue(farC);
            } else {
                _imp->focalLength.lock()->setValueAtTime(natronFrame, fl, ViewSpec::all(), 0);
                _imp->hAperture.lock()->setValueAtTime(natronFrame, hAp, ViewSpec::all(), 0);
                _imp->vAperture.lock()->setValueAtTime(natronFrame, vAp, ViewSpec::all(), 0);
                _imp->nearClipKnob.lock()->setValueAtTime(natronFrame, nearC, ViewSpec::all(), 0);
                _imp->farClipKnob.lock()->setValueAtTime(natronFrame, farC, ViewSpec::all(), 0);
            }
        }

        // Read camera transform from parent Xform (typically has more samples)
        IObject parent = obj.getParent();
        size_t numXformSamples = 0;

        if (IXform::matches(parent.getHeader())) {
            IXform xform(parent);
            IXformSchema xSchema = xform.getSchema();
            numXformSamples = xSchema.getNumSamples();
            Alembic::AbcCoreAbstract::TimeSamplingPtr xformTimeSampling = xSchema.getTimeSampling();

            // If the xform's timeSampling differs from cam's, prefer xform's for fps detection.
            if (xformTimeSampling) {
                const auto& tst = xformTimeSampling->getTimeSamplingType();
                if (tst.isUniform() && tst.getTimePerCycle() > 0.0) {
                    sourceFps = 1.0 / tst.getTimePerCycle();
                }
            }
            for (size_t i = 0; i < numXformSamples; ++i) {
                double abcTime = xformTimeSampling->getSampleTime(i);
                double natronFrame = sampleIndexToNatronFrame(i, abcTime);

                ISampleSelector sel((Alembic::AbcCoreAbstract::index_t)i);
                XformSample xSample;
                xSchema.get(xSample, sel);

                // Get the 4x4 matrix
                Imath::M44d matrix = xSample.getMatrix();

                // Extract translation
                _imp->translateX.lock()->setValueAtTime(natronFrame, matrix[3][0], ViewSpec::all(), 0);
                _imp->translateY.lock()->setValueAtTime(natronFrame, matrix[3][1], ViewSpec::all(), 0);
                _imp->translateZ.lock()->setValueAtTime(natronFrame, matrix[3][2], ViewSpec::all(), 0);

                // Extract XYZ Euler angles from the rotation matrix using Natron's
                // standard extrinsic XYZ convention (Maya/Blender/Houdini default).
                // Imath stores M_col with row-major memory, which means
                // m_imath[i][j] = M_col[j][i] — so transpose into our column-vector
                // 3x3 form, then decompose.
                //
                // The xform may carry scale (Maya exports bake unit-conversion
                // scale onto the camera — 78.5x on real assets). Scale is
                // meaningless for a camera's view but it corrupts the Euler
                // decompose (the asin term clamps at ±1 → bogus ±90° Y), so
                // divide each basis column by its length, as
                // ReadAlembicTransform does.
                double sLen[3];
                for (int j = 0; j < 3; ++j) {
                    sLen[j] = std::sqrt(matrix[j][0]*matrix[j][0] + matrix[j][1]*matrix[j][1] + matrix[j][2]*matrix[j][2]);
                    if (sLen[j] < 1e-12) {
                        sLen[j] = 1.0;
                    }
                }
                double mCol[3][3];
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        mCol[i][j] = matrix[j][i] / sLen[j];
                    }
                }
                double rxDeg = 0, ryDeg = 0, rzDeg = 0;
                RotationConventions::decompose(mCol, rxDeg, ryDeg, rzDeg);

                _imp->rotateX.lock()->setValueAtTime(natronFrame, rxDeg, ViewSpec::all(), 0);
                _imp->rotateY.lock()->setValueAtTime(natronFrame, ryDeg, ViewSpec::all(), 0);
                _imp->rotateZ.lock()->setValueAtTime(natronFrame, rzDeg, ViewSpec::all(), 0);
            }
        }

        // Update info
        std::ostringstream ss;
        ss << "Camera: " << selectedPath
           << " | Xform samples: " << numXformSamples
           << " | Camera samples: " << numCamSamples
           << " | Cameras found: " << _imp->cameraPaths.size();
        if (sourceFps > 0.0) {
            ss << " | Source FPS: " << sourceFps;
        }
        ss << " | Project FPS: " << projectFps;
        ss << " | Time mode: " << (timeMode == 1 ? "Time-based" : "Frame-by-frame");
        _imp->info.lock()->setValue(ss.str());
        _imp->loadedFilePath = path;

        refreshAspectInfo();
        clearPersistentMessage(false);

    } catch (const std::exception& e) {
        setPersistentMessage(eMessageTypeError, std::string("Error reading Alembic file: ") + e.what());
    }
#else
    Q_UNUSED(path);
    setPersistentMessage(eMessageTypeError, "Alembic support not available. Rebuild with Alembic library.");
#endif
}

void
ReadAlembicCamera::getCameraTransform(double time, double& tx, double& ty, double& tz,
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
ReadAlembicCamera::getFocalLength(double time) const
{
    return _imp->focalLength.lock()->getValueAtTime(time);
}

double
ReadAlembicCamera::getHAperture(double time) const
{
    return _imp->hAperture.lock()->getValueAtTime(time);
}

double
ReadAlembicCamera::getVAperture(double time) const
{
    return _imp->vAperture.lock()->getValueAtTime(time);
}

StatusEnum
ReadAlembicCamera::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                         ViewIdx /*view*/, RectD* rod)
{
    // Generator node — produce a 1x1 "dummy" output
    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = 1;
    rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadAlembicCamera::render(const RenderActionArgs& /*args*/)
{
    // No image output — this node only produces knob values
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ReadAlembicCamera.cpp"
