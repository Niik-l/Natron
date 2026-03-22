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

#include "ReadGeo.h"

#include <cassert>
#include <cmath>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
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

NATRON_NAMESPACE_ENTER

#ifdef NATRON_HAVE_ALEMBIC
// Find geometry objects in the hierarchy
static void
findGeoRecursive(const Alembic::AbcGeom::IObject& obj,
                 const std::string& parentPath,
                 std::vector<std::string>& geoPaths)
{
    using namespace Alembic::AbcGeom;

    std::string fullPath = parentPath.empty() ? obj.getName() : (parentPath + "/" + obj.getName());

    if (IPolyMesh::matches(obj.getHeader()) ||
        ISubD::matches(obj.getHeader()) ||
        IPoints::matches(obj.getHeader())) {
        geoPaths.push_back(fullPath);
    }

    for (size_t i = 0; i < obj.getNumChildren(); ++i) {
        findGeoRecursive(obj.getChild(i), fullPath, geoPaths);
    }
}
#endif


struct ReadGeoPrivate
{
    KnobFileWPtr filePath;
    KnobChoiceWPtr objectPath;
    KnobButtonWPtr reloadBtn;
    KnobStringWPtr info;

    std::vector<std::string> geoPaths;
    std::string loadedFilePath;
    std::string loadedObjectPath;

    // Cached Xform data for animation
    bool hasAnimatedXform = false;
    size_t numXformSamples = 0;
    std::vector<float> xformMatrices; // numXformSamples * 16 floats (row-major from Imath)
};


ReadGeo::ReadGeo(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadGeoPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ReadGeo::~ReadGeo()
{
}

std::string
ReadGeo::getPluginDescription() const
{
    return
#ifdef NATRON_HAVE_ALEMBIC
    tr("Import geometry from Alembic (.abc) files.\n\n"
       "Supports PolyMesh, SubD, and Points objects. "
       "Meshes are displayed as wireframe in the 3D viewport.\n\n"
       "Set the file path and select the geometry object from the dropdown.").toStdString();
#else
    tr("ReadGeo requires the Alembic library. Rebuild Natron with Alembic support.").toStdString();
#endif
}

void
ReadGeo::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ReadGeo::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadGeo::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ReadGeo::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("File"));

    KnobFilePtr fp = AppManager::createKnob<KnobFile>(this, tr("File"));
    fp->setName("filename");
    fp->setHintToolTip(tr("Path to the Alembic (.abc) file."));
    fp->setAnimationEnabled(false);
    page->addKnob(fp);
    _imp->filePath = fp;

    KnobChoicePtr obj = AppManager::createKnob<KnobChoice>(this, tr("Object"));
    obj->setName("objectPath");
    obj->setHintToolTip(tr("Select which geometry object to load."));
    page->addKnob(obj);
    _imp->objectPath = obj;

    KnobButtonPtr reload = AppManager::createKnob<KnobButton>(this, tr("Reload"));
    reload->setName("reload");
    page->addKnob(reload);
    _imp->reloadBtn = reload;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info"); info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false); info->setIsPersistent(false);
    info->setDefaultValue("Set file path to an .abc file.");
    page->addKnob(info); _imp->info = info;
}

bool
ReadGeo::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    if (_imp->filePath.lock().get() == k || _imp->reloadBtn.lock().get() == k) {
        std::string path = _imp->filePath.lock()->getValue();
        if (!path.empty()) {
            loadAlembicGeo(path);
        }
        return true;
    }
    return false;
}

void
ReadGeo::loadAlembicGeo(const std::string& path)
{
#ifdef NATRON_HAVE_ALEMBIC
    using namespace Alembic::AbcGeom;

    try {
        IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        IObject top = archive.getTop();

        _imp->geoPaths.clear();
        findGeoRecursive(top, "", _imp->geoPaths);

        if (_imp->geoPaths.empty()) {
            setPersistentMessage(eMessageTypeError, "No geometry found in " + path);
            return;
        }

        // Populate choice dropdown
        KnobChoicePtr objKnob = _imp->objectPath.lock();
        std::vector<ChoiceOption> entries;
        for (const std::string& gp : _imp->geoPaths) {
            entries.push_back(ChoiceOption(gp, "", ""));
        }
        objKnob->populateChoices(entries);
        objKnob->setDefaultValue(0);

        // Load the first geometry
        std::string selectedPath = _imp->geoPaths[0];

        // Navigate to the object
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

        MeshDataPtr mesh = std::make_shared<MeshData>();

        if (IPolyMesh::matches(obj.getHeader()) || ISubD::matches(obj.getHeader())) {
            // Read poly mesh
            IPolyMesh polyMesh(obj);
            IPolyMeshSchema schema = polyMesh.getSchema();
            IPolyMeshSchema::Sample sample;
            schema.get(sample);

            // Vertices
            const Imath::V3f* positions = sample.getPositions()->get();
            size_t numVerts = sample.getPositions()->size();
            mesh->vertices.resize(numVerts * 3);
            for (size_t i = 0; i < numVerts; ++i) {
                mesh->vertices[i * 3 + 0] = positions[i].x;
                mesh->vertices[i * 3 + 1] = positions[i].y;
                mesh->vertices[i * 3 + 2] = positions[i].z;
            }
            mesh->numVertices = numVerts;

            // Face indices and counts
            const int32_t* faceIdxs = sample.getFaceIndices()->get();
            size_t numFaceIdxs = sample.getFaceIndices()->size();
            const int32_t* faceCnts = sample.getFaceCounts()->get();
            size_t numFaces = sample.getFaceCounts()->size();
            mesh->numFaces = numFaces;

            mesh->faceIndices.assign(faceIdxs, faceIdxs + numFaceIdxs);
            mesh->faceCounts.assign(faceCnts, faceCnts + numFaces);

            // Build edge indices for wireframe rendering
            size_t idxOffset = 0;
            for (size_t f = 0; f < numFaces; ++f) {
                int count = faceCnts[f];
                for (int v = 0; v < count; ++v) {
                    mesh->edgeIndices.push_back(faceIdxs[idxOffset + v]);
                    mesh->edgeIndices.push_back(faceIdxs[idxOffset + ((v + 1) % count)]);
                }
                idxOffset += count;
            }

            // Cache all Xform samples for animation
            _imp->hasAnimatedXform = false;
            _imp->numXformSamples = 0;
            _imp->xformMatrices.clear();

            IObject parent = obj.getParent();
            if (IXform::matches(parent.getHeader())) {
                IXform xform(parent);
                IXformSchema xSchema = xform.getSchema();
                size_t numSamples = xSchema.getNumSamples();
                _imp->numXformSamples = numSamples;
                _imp->hasAnimatedXform = (numSamples > 1);
                _imp->xformMatrices.resize(numSamples * 16);

                for (size_t s = 0; s < numSamples; ++s) {
                    ISampleSelector sel((Alembic::AbcCoreAbstract::index_t)s);
                    XformSample xSample;
                    xSchema.get(xSample, sel);
                    Imath::M44d matrix = xSample.getMatrix();

                    // Imath M44d is row-major: matrix[row][col]
                    // OpenGL glMultMatrixf expects column-major
                    // Transpose: GL[col*4+row] = Imath[row][col]
                    for (int r = 0; r < 4; ++r) {
                        for (int c = 0; c < 4; ++c) {
                            _imp->xformMatrices[s * 16 + c * 4 + r] = (float)matrix[r][c];
                        }
                    }
                }

                // Set frame 0 transform on the mesh
                for (int i = 0; i < 16; ++i) {
                    mesh->transform[i] = _imp->xformMatrices[i];
                }

            }

        }

        _lastMeshData = mesh;
        _imp->loadedFilePath = path;
        _imp->loadedObjectPath = selectedPath;

        std::ostringstream ss;
        ss << "Object: " << selectedPath
           << " | Vertices: " << mesh->numVertices
           << " | Faces: " << mesh->numFaces;
        _imp->info.lock()->setValue(ss.str());

        clearPersistentMessage(false);

    } catch (const std::exception& e) {
        setPersistentMessage(eMessageTypeError, std::string("Error: ") + e.what());
    }
#else
    Q_UNUSED(path);
    setPersistentMessage(eMessageTypeError, "Alembic support not available.");
#endif
}

MeshDataPtr
ReadGeo::getMeshData(double time) const
{
    if (_lastMeshData && time >= 0 && _imp && _imp->hasAnimatedXform &&
        _imp->numXformSamples > 0 && !_imp->xformMatrices.empty()) {
        updateTransformAtTime(_lastMeshData.get(), time);
    }
    return _lastMeshData;
}

void
ReadGeo::updateTransformAtTime(MeshData* mesh, double time) const
{
    if (!mesh || !_imp->hasAnimatedXform || _imp->numXformSamples == 0) return;

    // Map Natron frame (1-based) to Alembic sample index
    // Assume 24fps, frame 1 = sample 0
    int sampleIdx = (int)(time - 1.0);
    if (sampleIdx < 0) sampleIdx = 0;
    if (sampleIdx >= (int)_imp->numXformSamples) sampleIdx = (int)_imp->numXformSamples - 1;

    // Copy the cached matrix
    const float* mat = &_imp->xformMatrices[sampleIdx * 16];
    for (int i = 0; i < 16; ++i) {
        mesh->transform[i] = mat[i];
    }
}

StatusEnum
ReadGeo::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                               ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0; rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadGeo::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ReadGeo.cpp"
