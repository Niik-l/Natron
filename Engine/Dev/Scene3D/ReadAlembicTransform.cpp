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

#include "ReadAlembicTransform.h"

#include <cassert>
#include <cmath>
#include <sstream>
#include <vector>

#include "RotationConventions.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../ChoiceOption.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
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
// Recursively find all IXform nodes in the Alembic hierarchy
static void
findXformsRecursive(const Alembic::AbcGeom::IObject& obj,
                    const std::string& parentPath,
                    std::vector<std::string>& xformPaths)
{
    using namespace Alembic::AbcGeom;

    std::string fullPath = parentPath.empty() ? obj.getName() : (parentPath + "/" + obj.getName());

    if (IXform::matches(obj.getHeader())) {
        xformPaths.push_back(fullPath);
    }

    for (size_t i = 0; i < obj.getNumChildren(); ++i) {
        findXformsRecursive(obj.getChild(i), fullPath, xformPaths);
    }
}
#endif


struct ReadAlembicTransformPrivate
{
    KnobFileWPtr filePath;
    KnobChoiceWPtr objectPath;
    KnobButtonWPtr reloadBtn;
    KnobIntWPtr frameOffset;
    KnobDoubleWPtr fpsKnob;
    KnobChoiceWPtr timeMode;

    // Output knobs (animated)
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobStringWPtr info;

    // Cached paths
    std::vector<std::string> xformPaths;
    std::string loadedFilePath;
};


ReadAlembicTransform::ReadAlembicTransform(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadAlembicTransformPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ReadAlembicTransform::~ReadAlembicTransform()
{
}

std::string
ReadAlembicTransform::getPluginDescription() const
{
    return
#ifdef NATRON_HAVE_ALEMBIC
    tr("Import animated transforms (nulls/locators) from Alembic (.abc) files.\n\n"
       "Reads any IXform node — typically a null, locator, or group exported from "
       "Maya, Houdini, or Blender. Translate, rotate, and scale are exposed as "
       "animated knob values.\n\n"
       "Use case: parent a locator to a point on your geometry in Maya (e.g. jet exhaust), "
       "export it as Alembic, then expression-link ParticleEmitter position to these outputs.\n\n"
       "Supports Y-up (Maya/Blender default) and Z-up scenes.").toStdString();
#else
    tr("ReadAlembicTransform requires the Alembic library. Rebuild Natron with Alembic support.").toStdString();
#endif
}

void
ReadAlembicTransform::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ReadAlembicTransform::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadAlembicTransform::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ReadAlembicTransform::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("File"));

    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("File"));
        k->setName("filename");
        k->setHintToolTip(tr("Path to the Alembic (.abc) file."));
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->filePath = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Transform"));
        k->setName("objectPath");
        k->setHintToolTip(tr("Select which transform/null to read from the Alembic file."));
        page->addKnob(k);
        _imp->objectPath = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reload"));
        k->setName("reload");
        k->setHintToolTip(tr("Re-read the Alembic file."));
        page->addKnob(k);
        _imp->reloadBtn = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Offset"));
        k->setName("frameOffset");
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Offset the Alembic animation relative to the Natron timeline."));
        page->addKnob(k);
        _imp->frameOffset = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("FPS"));
        k->setName("fps");
        k->setDefaultValue(24.0);
        k->setMinimum(1.0);
        k->setDisplayMinimum(1.0); k->setDisplayMaximum(120.0);
        k->setHintToolTip(tr("Frames per second used for Time-based time mode. Ignored in Frame-by-frame mode."));
        page->addKnob(k);
        _imp->fpsKnob = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Time Mode"));
        k->setName("timeMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("frame_by_frame", "Frame-by-frame",
            "Each Alembic sample becomes one keyframe at consecutive integer Natron frames (sample 0 -> frame 1, sample 1 -> frame 2, ...). Use this for frame-accurate matching with Maya/Blender/Houdini regardless of fps."));
        opts.push_back(ChoiceOption("time_based", "Time-based",
            "Map Alembic sample times to Natron timeline using the FPS knob above. Preserves real-world timing but may interpolate between samples when source and project fps differ."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("How Alembic sample times are mapped onto the Natron timeline."));
        page->addKnob(k);
        _imp->timeMode = k;
    }

    // Transform output page
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        xformPage->addKnob(k); _imp->translateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        xformPage->addKnob(k); _imp->translateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        xformPage->addKnob(k); _imp->translateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        k->setName("rotateX"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        xformPage->addKnob(k); _imp->rotateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        k->setName("rotateY"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        xformPage->addKnob(k); _imp->rotateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        k->setName("rotateZ"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        xformPage->addKnob(k); _imp->rotateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale X"));
        k->setName("scaleX"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        k->setDefaultValue(1.0);
        xformPage->addKnob(k); _imp->scaleX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Y"));
        k->setName("scaleY"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        k->setDefaultValue(1.0);
        xformPage->addKnob(k); _imp->scaleY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
        k->setName("scaleZ"); k->setAnimationEnabled(true); k->setEvaluateOnChange(false);
        k->setDefaultValue(1.0);
        xformPage->addKnob(k); _imp->scaleZ = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info"); k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false); k->setIsPersistent(false);
        k->setDefaultValue("Set file path to an .abc file.");
        xformPage->addKnob(k); _imp->info = k;
    }
}

bool
ReadAlembicTransform::knobChanged(KnobI* k,
                                  ValueChangedReasonEnum /*reason*/,
                                  ViewSpec /*view*/,
                                  double /*time*/,
                                  bool /*originatedFromMainThread*/)
{
    if (_imp->filePath.lock().get() == k || _imp->reloadBtn.lock().get() == k) {
        std::string path = _imp->filePath.lock()->getValue();
        if (!path.empty()) {
            loadAlembicFile(path);
        }
        return true;
    }
    if (_imp->objectPath.lock().get() == k) {
        // User changed the selected transform — reload with new selection
        std::string path = _imp->filePath.lock()->getValue();
        if (!path.empty()) {
            loadAlembicFile(path);
        }
        return true;
    }
    return false;
}

void
ReadAlembicTransform::onKnobsLoaded()
{
    // After a project load the file-path knob is restored but no knobChanged
    // fires, so the transform is never read — the user used to have to hit
    // "Reload". Re-read it here so the saved scene's transform appears immediately.
    KnobFilePtr fp = _imp->filePath.lock();
    if (fp) {
        const std::string path = fp->getValue();
        if (!path.empty()) {
            loadAlembicFile(path);
        }
    }
}

void
ReadAlembicTransform::loadAlembicFile(const std::string& path)
{
#ifdef NATRON_HAVE_ALEMBIC
    using namespace Alembic::AbcGeom;

    try {
        IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        IObject top = archive.getTop();

        // Find all xform nodes
        std::vector<std::string> newPaths;
        findXformsRecursive(top, "", newPaths);

        if (newPaths.empty()) {
            setPersistentMessage(eMessageTypeError, "No transforms found in " + path);
            return;
        }

        // Update dropdown if file changed
        if (path != _imp->loadedFilePath || _imp->xformPaths != newPaths) {
            _imp->xformPaths = newPaths;
            KnobChoicePtr objKnob = _imp->objectPath.lock();
            std::vector<ChoiceOption> entries;
            for (const std::string& xp : _imp->xformPaths) {
                entries.push_back(ChoiceOption(xp, "", ""));
            }
            objKnob->populateChoices(entries);
        }

        // Get selected transform index
        int selectedIdx = _imp->objectPath.lock()->getValue();
        if (selectedIdx < 0 || selectedIdx >= (int)_imp->xformPaths.size()) {
            selectedIdx = 0;
        }
        std::string selectedPath = _imp->xformPaths[selectedIdx];

        // Navigate to the selected object
        IObject obj = top;
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

        if (!IXform::matches(obj.getHeader())) {
            setPersistentMessage(eMessageTypeError, "Selected object is not a transform: " + selectedPath);
            return;
        }

        IXform xform(obj);
        IXformSchema xSchema = xform.getSchema();
        size_t numSamples = xSchema.getNumSamples();
        const int frameOffset = _imp->frameOffset.lock()->getValue();
        double fps = _imp->fpsKnob.lock()->getValue();
        if (fps < 1.0) fps = 24.0;
        const int timeMode = _imp->timeMode.lock()->getValue(); // 0=frame-by-frame, 1=time-based

        Alembic::AbcCoreAbstract::TimeSamplingPtr timeSampling = xSchema.getTimeSampling();

        // Detect source fps from timeSampling (uniform case only; for Info field).
        double sourceFps = 0.0;
        if (timeSampling) {
            const auto& tst = timeSampling->getTimeSamplingType();
            if (tst.isUniform() && tst.getTimePerCycle() > 0.0) {
                sourceFps = 1.0 / tst.getTimePerCycle();
            }
        }

        // Clear existing keyframes
        _imp->translateX.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->translateY.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->translateZ.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->rotateX.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->rotateY.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->rotateZ.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->scaleX.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->scaleY.lock()->removeAnimation(ViewSpec::all(), 0);
        _imp->scaleZ.lock()->removeAnimation(ViewSpec::all(), 0);

        for (size_t i = 0; i < numSamples; ++i) {
            double abcTime = timeSampling->getSampleTime(i);
            double natronFrame = (timeMode == 1)
                ? abcTime * fps + frameOffset + 1.0
                : (double)i + frameOffset + 1.0;

            ISampleSelector sel((Alembic::AbcCoreAbstract::index_t)i);
            XformSample xSample;
            xSchema.get(xSample, sel);

            Imath::M44d matrix = xSample.getMatrix();

            // Extract translation (row 3 of the matrix)
            double tx = matrix[3][0];
            double ty = matrix[3][1];
            double tz = matrix[3][2];

            // Extract scale (length of each basis vector)
            double sx = std::sqrt(matrix[0][0]*matrix[0][0] + matrix[0][1]*matrix[0][1] + matrix[0][2]*matrix[0][2]);
            double sy = std::sqrt(matrix[1][0]*matrix[1][0] + matrix[1][1]*matrix[1][1] + matrix[1][2]*matrix[1][2]);
            double sz = std::sqrt(matrix[2][0]*matrix[2][0] + matrix[2][1]*matrix[2][1] + matrix[2][2]*matrix[2][2]);

            // Extract rotation in Natron's standard extrinsic XYZ convention
            // (Maya/Blender/Houdini default). Imath stores M_col with row-major
            // memory (translation at matrix[3][i], confirming row-vector storage):
            //   M_col[i][j] = matrix[j][i]
            // Each column-vector basis (column j) carries scale s_j, so the
            // normalized column-vector entries are matrix[j][i] / s_j.
            const double sArr[3] = { sx, sy, sz };
            double mCol[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    mCol[i][j] = matrix[j][i] / sArr[j];
                }
            }
            double rxDeg = 0, ryDeg = 0, rzDeg = 0;
            RotationConventions::decompose(mCol, rxDeg, ryDeg, rzDeg);

            if (numSamples == 1) {
                // Static transform — set as default values
                _imp->translateX.lock()->setValue(tx);
                _imp->translateY.lock()->setValue(ty);
                _imp->translateZ.lock()->setValue(tz);
                _imp->rotateX.lock()->setValue(rxDeg);
                _imp->rotateY.lock()->setValue(ryDeg);
                _imp->rotateZ.lock()->setValue(rzDeg);
                _imp->scaleX.lock()->setValue(sx);
                _imp->scaleY.lock()->setValue(sy);
                _imp->scaleZ.lock()->setValue(sz);
            } else {
                _imp->translateX.lock()->setValueAtTime(natronFrame, tx, ViewSpec::all(), 0);
                _imp->translateY.lock()->setValueAtTime(natronFrame, ty, ViewSpec::all(), 0);
                _imp->translateZ.lock()->setValueAtTime(natronFrame, tz, ViewSpec::all(), 0);
                _imp->rotateX.lock()->setValueAtTime(natronFrame, rxDeg, ViewSpec::all(), 0);
                _imp->rotateY.lock()->setValueAtTime(natronFrame, ryDeg, ViewSpec::all(), 0);
                _imp->rotateZ.lock()->setValueAtTime(natronFrame, rzDeg, ViewSpec::all(), 0);
                _imp->scaleX.lock()->setValueAtTime(natronFrame, sx, ViewSpec::all(), 0);
                _imp->scaleY.lock()->setValueAtTime(natronFrame, sy, ViewSpec::all(), 0);
                _imp->scaleZ.lock()->setValueAtTime(natronFrame, sz, ViewSpec::all(), 0);
            }
        }

        // Update info
        std::ostringstream ss;
        ss << "Transform: " << selectedPath
           << " | Samples: " << numSamples
           << " | Transforms found: " << _imp->xformPaths.size();
        if (sourceFps > 0.0) {
            ss << " | Source FPS: " << sourceFps;
        }
        ss << " | Time mode: " << (timeMode == 1 ? "Time-based" : "Frame-by-frame");
        _imp->info.lock()->setValue(ss.str());
        _imp->loadedFilePath = path;

        clearPersistentMessage(false);

    } catch (const std::exception& e) {
        setPersistentMessage(eMessageTypeError, std::string("Error reading Alembic file: ") + e.what());
    }
#else
    Q_UNUSED(path);
    setPersistentMessage(eMessageTypeError, "Alembic support not available. Rebuild with Alembic library.");
#endif
}

StatusEnum
ReadAlembicTransform::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                            ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadAlembicTransform::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ReadAlembicTransform.cpp"
