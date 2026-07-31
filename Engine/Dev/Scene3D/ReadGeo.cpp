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
#include "../DotUtils.h"

#include "../../NodeMetadata.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib> // strtol
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
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
/**
 * @brief Recursively find all PolyMesh, SubD, and Points objects in the Alembic hierarchy.
 */
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

/**
 * @brief Navigate from the top object to a child by slash-separated path.
 * Returns true if the object was found.
 */
static bool
navigateToObject(const Alembic::AbcGeom::IObject& top,
                 const std::string& path,
                 Alembic::AbcGeom::IObject& result)
{
    result = top;
    std::istringstream iss(path);
    std::string token;
    while (std::getline(iss, token, '/')) {
        if (token.empty()) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < result.getNumChildren(); ++i) {
            if (result.getChild(i).getName() == token) {
                result = result.getChild(i);
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Read mesh data from positions/faceIndices/faceCounts using raw float/int access.
 *
 * The critical fix: we treat position data as a raw float array rather than
 * accessing Imath::V3f members (x, y, z). This avoids crashes caused by
 * struct layout mismatches between Natron's Imath and Alembic's Imath.
 */
static void
readMeshArrays(const void* positionsRaw, size_t numVerts,
               const void* faceIndicesRaw, size_t numFaceIndices,
               const void* faceCountsRaw, size_t numFaces,
               MeshData* mesh)
{
    // -- Vertices: raw float access, 3 floats per vertex --
    const float* rawPos = reinterpret_cast<const float*>(positionsRaw);
    mesh->vertices.resize(numVerts * 3);
    mesh->numVertices = numVerts;
    std::memcpy(mesh->vertices.data(), rawPos, numVerts * 3 * sizeof(float));

    // -- Face indices: raw int32_t access --
    const int32_t* rawIdx = reinterpret_cast<const int32_t*>(faceIndicesRaw);
    mesh->faceIndices.resize(numFaceIndices);
    for (size_t i = 0; i < numFaceIndices; ++i) {
        mesh->faceIndices[i] = static_cast<int>(rawIdx[i]);
    }

    // -- Face counts: raw int32_t access --
    const int32_t* rawCnt = reinterpret_cast<const int32_t*>(faceCountsRaw);
    mesh->faceCounts.resize(numFaces);
    mesh->numFaces = numFaces;
    for (size_t i = 0; i < numFaces; ++i) {
        mesh->faceCounts[i] = static_cast<int>(rawCnt[i]);
    }

    // -- Build edge indices for wireframe rendering --
    mesh->edgeIndices.clear();
    mesh->edgeIndices.reserve(numFaceIndices * 2); // upper bound
    size_t idxOffset = 0;
    for (size_t f = 0; f < numFaces; ++f) {
        int count = static_cast<int>(rawCnt[f]);
        for (int v = 0; v < count; ++v) {
            mesh->edgeIndices.push_back(static_cast<int>(rawIdx[idxOffset + v]));
            mesh->edgeIndices.push_back(static_cast<int>(rawIdx[idxOffset + ((v + 1) % count)]));
        }
        idxOffset += count;
    }
}

/**
 * @brief Read Xform matrix from a sample, storing as column-major float[16] for OpenGL.
 * Uses raw double access to avoid Imath M44d struct layout issues.
 */
static void
readXformMatrix(const Alembic::AbcGeom::IXformSchema& xSchema,
                size_t sampleIndex,
                float outMatrix[16])
{
    using namespace Alembic::AbcGeom;
    ISampleSelector sel(static_cast<Alembic::AbcCoreAbstract::index_t>(sampleIndex));
    XformSample xSample;
    xSchema.get(xSample, sel);

    // getMatrix() returns Imath::M44d -- 16 doubles in row-major order.
    // Access the raw doubles to avoid struct layout dependency.
    Imath::M44d matrix = xSample.getMatrix();
    const double* rawMat = reinterpret_cast<const double*>(&matrix);

    // Transpose from Imath row-major to OpenGL column-major:
    // GL[col*4+row] = Imath[row*4+col]
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            outMatrix[c * 4 + r] = static_cast<float>(rawMat[r * 4 + c]);
        }
    }
}
#endif // NATRON_HAVE_ALEMBIC


struct ReadGeoPrivate
{
    KnobFileWPtr filePath;
    KnobChoiceWPtr objectPath;
    KnobButtonWPtr reloadBtn;
    KnobBoolWPtr reverseNormals;
    KnobStringWPtr info;

    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    std::vector<std::string> geoPaths;
    std::string loadedFilePath;
    std::string loadedObjectPath;

    // Material
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    KnobFileWPtr textureFile;
    KnobChoiceWPtr diffuseColorspace;

    // Re-entrancy guard: prevents recursive knobChanged calls
    // when we programmatically update knobs during loading.
    bool isLoading = false;

    // Cached Xform data for animated transforms
    bool hasAnimatedXform = false;
    size_t numXformSamples = 0;
    std::vector<float> xformMatrices; // numXformSamples * 16 floats (column-major)

    // Cached vertex samples for animated (deforming) meshes — populated by
    // loadAlembicGeo when the .abc has more than one PolyMesh / SubD sample
    // (e.g. a morphing terrain or animated character mesh). Pre-loaded into
    // memory at file-load time so per-frame fetches are a cheap memcpy.
    // Topology (faceIndices / faceCounts) is assumed constant across samples
    // — true for the typical exporter (Maya / Blender / Houdini Alembic).
    bool hasAnimatedVerts = false;
    size_t numVertexSamples = 0;
    std::vector<std::vector<float>> vertexSamples; // each: numVerts * 3 floats
};


// ---------------------------------------------------------------------------
// Construction / Description
// ---------------------------------------------------------------------------

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
    return tr(
#ifdef NATRON_HAVE_ALEMBIC
       "Import geometry from .abc (Alembic) or .obj (Wavefront) files. "
       "The parser dispatches on the file extension.\n\n"
       "Alembic: PolyMesh, SubD, and Points objects. Object dropdown picks "
       "which entry from a multi-mesh archive to load.\n\n"
#else
       "Import geometry from .obj (Wavefront) files. "
       "Alembic (.abc) support is not built into this binary — rebuild with "
       "Alembic to enable it.\n\n"
#endif
       "OBJ: positions, texture coords, and n-gon faces are honored. Group/"
       "object directives populate the Object dropdown when present; "
       "otherwise the file loads as a single mesh."
    ).toStdString();
}

std::string
ReadGeo::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "Material";
    if (inputNb == 1) return "Image";
    return std::string();
}

// ---------------------------------------------------------------------------
// EffectInstance overrides: formats, bit depths
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

void
ReadGeo::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("File"));

    {
        KnobFilePtr fp = AppManager::createKnob<KnobFile>(this, tr("File"));
        fp->setName("filename");
        fp->setHintToolTip(tr("Path to a .abc (Alembic) or .obj (Wavefront) geometry file."));
        fp->setAnimationEnabled(false);
        page->addKnob(fp);
        _imp->filePath = fp;
    }

    {
        KnobChoicePtr obj = AppManager::createKnob<KnobChoice>(this, tr("Object"));
        obj->setName("objectPath");
        obj->setHintToolTip(tr("Select which geometry object to load."));
        page->addKnob(obj);
        _imp->objectPath = obj;
    }

    {
        KnobButtonPtr reload = AppManager::createKnob<KnobButton>(this, tr("Reload"));
        reload->setName("reload");
        page->addKnob(reload);
        _imp->reloadBtn = reload;
    }

    {
        KnobBoolPtr rn = AppManager::createKnob<KnobBool>(this, tr("Reverse Normals"));
        rn->setName("reverseNormals");
        rn->setDefaultValue(false);
        rn->setHintToolTip(tr("Flip the face winding so normals point the other way. "
                              "Use the 3D viewport's Face Orientation shading mode to check: "
                              "blue = facing you, red = facing away."));
        page->addKnob(rn);
        _imp->reverseNormals = rn;
    }

    {
        KnobStringPtr infoKnob = AppManager::createKnob<KnobString>(this, tr("Info"));
        infoKnob->setName("info");
        infoKnob->setAnimationEnabled(false);
        infoKnob->setEvaluateOnChange(false);
        infoKnob->setIsPersistent(false);
        infoKnob->setDefaultValue("Set file path to a .abc or .obj file.");
        page->addKnob(infoKnob);
        _imp->info = infoKnob;
    }

    // Transform page
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
        k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
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
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale X"));
        k->setName("scaleX"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Y"));
        k->setName("scaleY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
        k->setName("scaleZ"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleZ = k;
    }

    // Material page
    KnobPagePtr matPage = AppManager::createKnob<KnobPage>(this, tr("Material"));
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Base Color"), 3);
        k->setName("baseColor");
        k->setDefaultValue(0.8, 0); k->setDefaultValue(0.8, 1); k->setDefaultValue(0.8, 2);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->baseColor = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Roughness"));
        k->setName("roughness"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        matPage->addKnob(k); _imp->roughness = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Metallic"));
        k->setName("metallic"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        matPage->addKnob(k); _imp->metallic = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Specular"));
        k->setName("specular"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        matPage->addKnob(k); _imp->specular = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Emission Color"), 3);
        k->setName("emissionColor");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(1.0, 2);
        matPage->addKnob(k); _imp->emissionColor = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emission Strength"));
        k->setName("emissionStrength"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        matPage->addKnob(k); _imp->emissionStrength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Transmission"));
        k->setName("transmission"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("0 = opaque, 1 = fully transparent (glass). Use with IOR."));
        matPage->addKnob(k); _imp->transmission = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("IOR"));
        k->setName("ior"); k->setDefaultValue(1.45);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(2.5);
        k->setHintToolTip(tr("Index of refraction. Glass=1.5, Water=1.33, Diamond=2.42"));
        matPage->addKnob(k); _imp->ior = k;
    }
    // Texture Maps page — order: Diffuse, Metallic, Roughness, Emission, Normal
    KnobPagePtr texPage = AppManager::createKnob<KnobPage>(this, tr("Texture Maps"));
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Diffuse Map"));
        k->setName("textureFile");
        k->setHintToolTip(tr("Base color / albedo texture. Supports .exr, .hdr, .png, .jpg"));
        texPage->addKnob(k); _imp->textureFile = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Diffuse Colorspace"));
        k->setName("diffuseColorspace");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("sRGB", "", "sRGB gamma-encoded (PNG, JPEG)"));
        entries.push_back(ChoiceOption("Linear", "", "Linear / scene-referred (EXR, HDR)"));
        entries.push_back(ChoiceOption("ACEScg", "", "ACEScg (AP1 linear, ACES pipeline)"));
        entries.push_back(ChoiceOption("Raw", "", "Raw data, no conversion"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Color space of the diffuse texture file."));
        texPage->addKnob(k); _imp->diffuseColorspace = k;
    }
}

bool
ReadGeo::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    // Re-entrancy guard: loadAlembicGeo modifies knobs which would trigger knobChanged again
    if (_imp->isLoading) {
        return false;
    }

    if (_imp->filePath.lock().get() == k || _imp->reloadBtn.lock().get() == k) {
        loadGeoFromFile(_imp->filePath.lock()->getValue());
        return true;
    }

    // Object dropdown changed: reload the selected object from the already-open file
    if (_imp->objectPath.lock().get() == k) {
        loadGeoFromFile(_imp->filePath.lock()->getValue());
        return true;
    }

    // Reverse Normals: re-run the loader so the flip is applied to freshly
    // built data (keeps applied state == knob state, no toggle bookkeeping).
    if (_imp->reverseNormals.lock().get() == k) {
        loadGeoFromFile(_imp->filePath.lock()->getValue());
        return true;
    }

    return false;
}

void
ReadGeo::loadGeoFromFile(const std::string& path)
{
    // Serialise (re)loads against getMeshData readers on render threads.
    std::lock_guard<std::mutex> lk(_meshMutex);

    // Drop the previous geometry FIRST — clearing the path or a failed load
    // must not keep serving the old mesh (stale-render class).
    _lastMeshData.reset();

    if (path.empty()) {
        refreshMetadata_public(true);
        return;
    }

    // Helper: lowercase extension of `path` ("foo.OBJ" → ".obj").
    auto extOf = [](const std::string& p) -> std::string {
        const size_t dot = p.find_last_of('.');
        if (dot == std::string::npos) return std::string();
        std::string e = p.substr(dot);
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return e;
    };

    // Re-entrancy guard: loading modifies knobs which would re-trigger knobChanged.
    _imp->isLoading = true;

    const std::string e = extOf(path);
    if (e == ".obj") {
        loadObjGeo(path);
    } else if (e == ".abc") {
#ifdef NATRON_HAVE_ALEMBIC
        loadAlembicGeo(path);
#else
        setPersistentMessage(eMessageTypeError,
            "Alembic support not built into this Natron binary. "
            "Use a .obj file, or rebuild Natron with Alembic.");
#endif
    } else {
        setPersistentMessage(eMessageTypeError,
            "Unsupported file extension. Use .abc (Alembic) or .obj (Wavefront).");
    }

    _imp->isLoading = false;
    // Re-read metadata so getPreferredMetadata picks up the new
    // hasAnimatedXform / hasAnimatedVerts flags (Scene3D + ScanlineRender
    // cache need to see frame-varying when the new file has animation).
    refreshMetadata_public(true);
}

void
ReadGeo::onKnobsLoaded()
{
    // After a project load the file-path knob is restored, but no knobChanged
    // fires for it, so the geometry is never read into memory — the user used to
    // have to hit "Reload" manually. Load it here so the saved scene appears
    // immediately. (loadGeoFromFile is a no-op for an empty path.)
    KnobFilePtr fileKnob = _imp->filePath.lock();
    if (fileKnob) {
        loadGeoFromFile( fileKnob->getValue() );
    }
}

// ---------------------------------------------------------------------------
// Alembic loading
// ---------------------------------------------------------------------------

void
ReadGeo::loadAlembicGeo(const std::string& path)
{
#ifdef NATRON_HAVE_ALEMBIC
    using namespace Alembic::AbcGeom;

    try {
        // Verify file is readable before handing to Alembic
        {
            FILE* testF = fopen(path.c_str(), "rb");
            if (!testF) {
                setPersistentMessage(eMessageTypeError, "Cannot open file: " + path);
                return;
            }
            fclose(testF);
        }

        // Open archive with Ogawa reader
        Alembic::AbcCoreOgawa::ReadArchive reader;
        IArchive archive(reader, path);
        IObject top = archive.getTop();

        // --- Discover geometry objects ---
        // Start from top's children (not top itself) so paths don't include
        // the root "ABC" name. navigateToObject starts FROM top, so paths
        // must be relative to top's children.
        _imp->geoPaths.clear();
        for (size_t i = 0; i < top.getNumChildren(); ++i) {
            findGeoRecursive(top.getChild(i), "", _imp->geoPaths);
        }

        if (_imp->geoPaths.empty()) {
            setPersistentMessage(eMessageTypeError, "No geometry found in " + path);
            return;
        }

        // --- Populate the Object dropdown ---
        KnobChoicePtr objKnob = _imp->objectPath.lock();
        if (objKnob) {
            std::vector<ChoiceOption> entries;
            for (const std::string& gp : _imp->geoPaths) {
                entries.push_back(ChoiceOption(gp, "", ""));
            }
            objKnob->populateChoices(entries);
            // Keep current selection if still valid, otherwise default to 0
            int curIdx = objKnob->getValue();
            if (curIdx < 0 || curIdx >= (int)_imp->geoPaths.size()) {
                objKnob->setDefaultValue(0);
            }
        }

        // --- Determine which object is selected ---
        int selectedIdx = 0;
        if (objKnob) {
            selectedIdx = objKnob->getValue();
            if (selectedIdx < 0 || selectedIdx >= (int)_imp->geoPaths.size()) {
                selectedIdx = 0;
            }
        }
        std::string selectedPath = _imp->geoPaths[selectedIdx];

        // --- Navigate to the selected object ---
        IObject obj;
        if (!navigateToObject(top, selectedPath, obj)) {
            setPersistentMessage(eMessageTypeError, "Object not found: " + selectedPath);
            return;
        }

        // --- Allocate MeshData FIRST, before touching any Alembic sample data ---
        MeshDataPtr mesh = std::make_shared<MeshData>();

        // Reset animation caches in case the user is reloading a different file.
        _imp->hasAnimatedVerts = false;
        _imp->numVertexSamples = 0;
        _imp->vertexSamples.clear();

        bool isPoly = IPolyMesh::matches(obj.getHeader());
        bool isSubD = ISubD::matches(obj.getHeader());

        if (isPoly) {
            IPolyMesh polyMesh(obj);
            IPolyMeshSchema schema = polyMesh.getSchema();
            IPolyMeshSchema::Sample sample;
            schema.get(sample);

            // Raw pointer access -- avoids Imath::V3f struct layout dependency
            const void* posPtr     = sample.getPositions()->get();
            size_t      numVerts   = sample.getPositions()->size();
            const void* idxPtr     = sample.getFaceIndices()->get();
            size_t      numIdx     = sample.getFaceIndices()->size();
            const void* cntPtr     = sample.getFaceCounts()->get();
            size_t      numFaces   = sample.getFaceCounts()->size();

            readMeshArrays(posPtr, numVerts, idxPtr, numIdx, cntPtr, numFaces, mesh.get());

            // Pre-load all vertex samples if the mesh is animated. Topology
            // (faceIndices/faceCounts) is assumed constant — only positions
            // change. Stored in _imp->vertexSamples for per-frame memcpy via
            // updateVerticesAtTime. See ReadGeoPrivate::hasAnimatedVerts.
            const size_t numVertSamples = schema.getNumSamples();
            if (numVertSamples > 1) {
                _imp->hasAnimatedVerts = true;
                _imp->numVertexSamples = numVertSamples;
                _imp->vertexSamples.assign(numVertSamples, std::vector<float>());
                for (size_t s = 0; s < numVertSamples; ++s) {
                    Alembic::Abc::ISampleSelector ss((Alembic::Abc::index_t)s);
                    IPolyMeshSchema::Sample animSample;
                    schema.get(animSample, ss);
                    const Alembic::Abc::P3fArraySamplePtr& positions = animSample.getPositions();
                    if (!positions || positions->size() != numVerts) continue;
                    const float* posPtrAnim = reinterpret_cast<const float*>(positions->get());
                    _imp->vertexSamples[s].resize(numVerts * 3);
                    std::memcpy(_imp->vertexSamples[s].data(), posPtrAnim,
                                numVerts * 3 * sizeof(float));
                }
            }

            // Read UVs — try common Alembic UV param names
            IV2fGeomParam uvParam = schema.getUVsParam();
            if (uvParam.valid()) {
                IV2fGeomParam::Sample uvSample;
                uvParam.getExpanded(uvSample);
                const Alembic::Abc::V2fArraySamplePtr& uvVals = uvSample.getVals();
                if (uvVals && uvVals->size() > 0) {
                    size_t numUVs = uvVals->size();
                    const float* rawUV = reinterpret_cast<const float*>(uvVals->get());
                    mesh->uvs.resize(numUVs * 2);
                    std::memcpy(mesh->uvs.data(), rawUV, numUVs * 2 * sizeof(float));
                    mesh->hasUVs = true;
                    (void)0; // UVs loaded
                }
            }

        } else if (isSubD) {
            ISubD subdMesh(obj);
            ISubDSchema schema = subdMesh.getSchema();
            ISubDSchema::Sample sample;
            schema.get(sample);

            const void* posPtr     = sample.getPositions()->get();
            size_t      numVerts   = sample.getPositions()->size();
            const void* idxPtr     = sample.getFaceIndices()->get();
            size_t      numIdx     = sample.getFaceIndices()->size();
            const void* cntPtr     = sample.getFaceCounts()->get();
            size_t      numFaces   = sample.getFaceCounts()->size();

            readMeshArrays(posPtr, numVerts, idxPtr, numIdx, cntPtr, numFaces, mesh.get());

            // Pre-load all vertex samples (same logic as the PolyMesh branch).
            const size_t numVertSamples = schema.getNumSamples();
            if (numVertSamples > 1) {
                _imp->hasAnimatedVerts = true;
                _imp->numVertexSamples = numVertSamples;
                _imp->vertexSamples.assign(numVertSamples, std::vector<float>());
                for (size_t s = 0; s < numVertSamples; ++s) {
                    Alembic::Abc::ISampleSelector ss((Alembic::Abc::index_t)s);
                    ISubDSchema::Sample animSample;
                    schema.get(animSample, ss);
                    const Alembic::Abc::P3fArraySamplePtr& positions = animSample.getPositions();
                    if (!positions || positions->size() != numVerts) continue;
                    const float* posPtrAnim = reinterpret_cast<const float*>(positions->get());
                    _imp->vertexSamples[s].resize(numVerts * 3);
                    std::memcpy(_imp->vertexSamples[s].data(), posPtrAnim,
                                numVerts * 3 * sizeof(float));
                }
            }

            // Read UVs from SubD
            IV2fGeomParam uvParam = schema.getUVsParam();
            if (uvParam.valid()) {
                IV2fGeomParam::Sample uvSample;
                uvParam.getExpanded(uvSample);
                const Alembic::Abc::V2fArraySamplePtr& uvVals = uvSample.getVals();
                if (uvVals && uvVals->size() > 0) {
                    size_t numUVs = uvVals->size();
                    const float* rawUV = reinterpret_cast<const float*>(uvVals->get());
                    mesh->uvs.resize(numUVs * 2);
                    std::memcpy(mesh->uvs.data(), rawUV, numUVs * 2 * sizeof(float));
                    mesh->hasUVs = true;
                    (void)0; // UVs loaded
                }
            }

        } else {
            setPersistentMessage(eMessageTypeError, "Object is not a supported mesh type: " + selectedPath);
            return;
        }

        // --- Read Xform from parent if available ---
        _imp->hasAnimatedXform = false;
        _imp->numXformSamples = 0;
        _imp->xformMatrices.clear();

        IObject parent = obj.getParent();
        if (parent.valid() && IXform::matches(parent.getHeader())) {
            IXform xform(parent);
            IXformSchema xSchema = xform.getSchema();
            size_t numSamples = xSchema.getNumSamples();

            if (numSamples > 0) {
                _imp->numXformSamples = numSamples;
                _imp->hasAnimatedXform = (numSamples > 1);
                _imp->xformMatrices.resize(numSamples * 16);

                for (size_t s = 0; s < numSamples; ++s) {
                    readXformMatrix(xSchema, s, &_imp->xformMatrices[s * 16]);
                }

                // Set frame 0 transform on the mesh
                std::memcpy(mesh->transform, _imp->xformMatrices.data(), 16 * sizeof(float));
            }
        }

        if (_imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue()) {
            reverseMeshWinding(*mesh);
        }

        // --- Store result ---
        _lastMeshData = mesh;
        _imp->loadedFilePath = path;
        _imp->loadedObjectPath = selectedPath;

        // --- Update info string ---
        std::ostringstream ss;
        ss << "Object: " << selectedPath
           << " | Vertices: " << mesh->numVertices
           << " | Faces: " << mesh->numFaces;
        _imp->info.lock()->setValue(ss.str());

        clearPersistentMessage(false);

    } catch (const std::exception& e) {
        setPersistentMessage(eMessageTypeError, std::string("Alembic error: ") + e.what());
    } catch (...) {
        setPersistentMessage(eMessageTypeError, "Unknown error loading Alembic file.");
    }
#else
    Q_UNUSED(path);
    setPersistentMessage(eMessageTypeError, "Alembic support not available.");
#endif
}

// ---------------------------------------------------------------------------
// OBJ (Wavefront) loading
// ---------------------------------------------------------------------------

namespace {

// Parse one face-vertex token: "v", "v/vt", "v//vn", or "v/vt/vn".
// OBJ uses 1-based indices; negative indices are relative-from-end. We resolve
// to 0-based positive indices using the supplied counts. Returns true on
// parse success.
bool
parseObjFaceToken(const std::string& tok,
                  int posCount, int vtCount, int vnCount,
                  int& outV, int& outVt, int& outVn)
{
    outV = outVt = outVn = -1;
    const size_t slash1 = tok.find('/');
    auto resolve = [](int idx, int count) -> int {
        if (idx == 0) return -1;
        if (idx > 0)  return (idx <= count) ? (idx - 1) : -1;
        // negative: -1 = last
        const int abs = -idx;
        return (abs <= count) ? (count - abs) : -1;
    };

    if (slash1 == std::string::npos) {
        outV = resolve(std::atoi(tok.c_str()), posCount);
        return outV >= 0;
    }

    outV = resolve(std::atoi(tok.substr(0, slash1).c_str()), posCount);
    const size_t slash2 = tok.find('/', slash1 + 1);
    if (slash2 == std::string::npos) {
        outVt = resolve(std::atoi(tok.substr(slash1 + 1).c_str()), vtCount);
    } else {
        if (slash2 > slash1 + 1) {
            outVt = resolve(std::atoi(tok.substr(slash1 + 1, slash2 - slash1 - 1).c_str()),
                            vtCount);
        }
        outVn = resolve(std::atoi(tok.substr(slash2 + 1).c_str()), vnCount);
    }
    return outV >= 0;
}

// Strip leading UTF-8 BOM (\xEF\xBB\xBF) if present, plus trailing CR.
void
stripBomAndCR(std::string& line, bool& firstLine)
{
    if (firstLine && line.size() >= 3 &&
        (unsigned char)line[0] == 0xEF &&
        (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
        line.erase(0, 3);
    }
    firstLine = false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
}

struct ObjGroup
{
    std::string name;
    std::vector<int>   faceIndicesV;   // position index per face-vertex
    std::vector<int>   faceCounts;     // verts per face
    std::vector<float> uvs;            // u,v per face-vertex (0,0 if no vt)
    bool hasUVs = false;
};

} // anon

void
ReadGeo::loadObjGeo(const std::string& path)
{
    std::ifstream in(path.c_str());
    if (!in.is_open()) {
        setPersistentMessage(eMessageTypeError, "Cannot open file: " + path);
        return;
    }

    std::vector<float> positions;  // x,y,z
    std::vector<float> texCoords;  // u,v

    std::vector<ObjGroup> groups;
    groups.reserve(4);
    groups.push_back(ObjGroup{}); // implicit default group
    groups[0].name = "default";

    bool firstLine = true;
    std::string line;
    line.reserve(256);

    while (std::getline(in, line)) {
        stripBomAndCR(line, firstLine);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag.empty()) continue;

        if (tag == "v") {
            float x = 0, y = 0, z = 0;
            ls >> x >> y >> z;
            positions.push_back(x);
            positions.push_back(y);
            positions.push_back(z);
        } else if (tag == "vt") {
            float u = 0, v = 0;
            ls >> u >> v;
            texCoords.push_back(u);
            texCoords.push_back(v);
        } else if (tag == "vn") {
            // Normals: ignored in v1 (MeshData has no normals field).
        } else if (tag == "g" || tag == "o") {
            std::string name;
            ls >> name;
            // Start a new group if the current one has already received faces.
            if (!groups.back().faceCounts.empty()) {
                groups.push_back(ObjGroup{});
            }
            groups.back().name = name.empty() ? "unnamed" : name;
        } else if (tag == "f") {
            ObjGroup& g = groups.back();
            int faceVerts = 0;
            std::string tok;
            const int posCount = (int)(positions.size() / 3);
            const int vtCount  = (int)(texCoords.size() / 2);
            while (ls >> tok) {
                int v = -1, vt = -1, vn = -1;
                if (!parseObjFaceToken(tok, posCount, vtCount, /*vnCount*/0, v, vt, vn)) continue;
                g.faceIndicesV.push_back(v);
                if (vt >= 0 && vt < vtCount) {
                    g.uvs.push_back(texCoords[vt * 2 + 0]);
                    g.uvs.push_back(texCoords[vt * 2 + 1]);
                    g.hasUVs = true;
                } else {
                    g.uvs.push_back(0.0f);
                    g.uvs.push_back(0.0f);
                }
                ++faceVerts;
            }
            if (faceVerts >= 3) g.faceCounts.push_back(faceVerts);
        }
        // Other tags (s, mtllib, usemtl, ...) deliberately ignored in v1.
    }

    // Drop empty groups (e.g. the implicit "default" if all faces went into a named group).
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const ObjGroup& g){ return g.faceCounts.empty(); }),
                 groups.end());

    if (groups.empty() || positions.empty()) {
        setPersistentMessage(eMessageTypeError, "No geometry found in " + path);
        return;
    }

    // --- Populate the Object dropdown ---
    _imp->geoPaths.clear();
    for (const ObjGroup& g : groups) _imp->geoPaths.push_back(g.name);

    KnobChoicePtr objKnob = _imp->objectPath.lock();
    int chosenIdx = 0;
    if (objKnob) {
        std::vector<ChoiceOption> entries;
        for (const std::string& n : _imp->geoPaths) {
            entries.push_back(ChoiceOption(n, "", ""));
        }
        objKnob->populateChoices(entries);
        chosenIdx = objKnob->getValue();
        if (chosenIdx < 0 || chosenIdx >= (int)groups.size()) {
            chosenIdx = 0;
            objKnob->setValue(0);
        }
    }

    const ObjGroup& chosen = groups[chosenIdx];

    // --- Build MeshData for the chosen group ---
    MeshDataPtr mesh(new MeshData);
    mesh->vertices = positions; // global vertex positions
    mesh->numVertices = positions.size() / 3;
    mesh->faceIndices = chosen.faceIndicesV;
    mesh->faceCounts  = chosen.faceCounts;
    mesh->numFaces    = chosen.faceCounts.size();
    if (chosen.hasUVs) {
        mesh->uvs = chosen.uvs;
        mesh->hasUVs = true;
        mesh->texCoordComponents = 2;
    }

    // --- Edge list for wireframe drawing ---
    // For each face emit boundary edges (vi, vi+1) and closing (last, first).
    {
        size_t off = 0;
        const int nv = (int)mesh->numVertices;
        for (size_t f = 0; f < mesh->faceCounts.size(); ++f) {
            const int c = mesh->faceCounts[f];
            if (c < 2 || off + (size_t)c > mesh->faceIndices.size()) {
                off += (size_t)std::max(0, c);
                continue;
            }
            for (int i = 0; i < c; ++i) {
                const int a = mesh->faceIndices[off + i];
                const int b = mesh->faceIndices[off + ((i + 1) % c)];
                if (a >= 0 && a < nv && b >= 0 && b < nv) {
                    mesh->edgeIndices.push_back(a);
                    mesh->edgeIndices.push_back(b);
                }
            }
            off += (size_t)c;
        }
    }

    // Identity transform — the node's own Translate/Rotate/Scale knobs handle
    // user-controlled placement downstream.
    for (int i = 0; i < 16; ++i) mesh->transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    if (_imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue()) {
        reverseMeshWinding(*mesh);
    }

    // --- Store result ---
    _lastMeshData = mesh;
    _imp->loadedFilePath = path;
    _imp->loadedObjectPath = chosen.name;

    // --- Update info string ---
    std::ostringstream ss;
    ss << "OBJ: " << chosen.name
       << " | Vertices: " << mesh->numVertices
       << " | Faces: " << mesh->numFaces
       << " | Groups: " << groups.size()
       << " | UVs: " << (mesh->hasUVs ? "yes" : "no");
    _imp->info.lock()->setValue(ss.str());

    clearPersistentMessage(false);
}

// ---------------------------------------------------------------------------
// Mesh data access
// ---------------------------------------------------------------------------

MeshDataPtr
ReadGeo::getMeshData(double time) const
{
    // Serialise against a concurrent reload swapping/clearing _lastMeshData.
    // NOTE: the per-time vertex/transform update below still mutates the
    // shared mesh in place (wrong-render across concurrent consumers at
    // different times) — that is the G1 copy-on-read fix, tracked separately.
    std::lock_guard<std::mutex> lk(_meshMutex);

    if (_lastMeshData && time >= 0 && _imp) {
        if (_imp->hasAnimatedXform && _imp->numXformSamples > 0 &&
            !_imp->xformMatrices.empty()) {
            updateTransformAtTime(_lastMeshData.get(), time);
        }
        if (_imp->hasAnimatedVerts && _imp->numVertexSamples > 0 &&
            !_imp->vertexSamples.empty()) {
            updateVerticesAtTime(_lastMeshData.get(), time);
        }
    }
    return _lastMeshData;
}

void
ReadGeo::updateTransformAtTime(MeshData* mesh, double time) const
{
    if (!mesh || !_imp->hasAnimatedXform || _imp->numXformSamples == 0) {
        return;
    }

    // Continuous sample index (frame 1 = sample 0). Bracket with floor/ceil
    // and linearly interpolate matrix components — without this, motion blur
    // sub-frame sampling returns snapped data and animated xforms don't blur.
    // Linear matrix interp isn't strictly correct for large rotations (would
    // need slerp on the rotation block) but at sub-frame deltas the error is
    // negligible.
    const int N = static_cast<int>(_imp->numXformSamples);
    double tCont = time - 1.0;
    if (tCont < 0.0) tCont = 0.0;
    if (tCont > (double)(N - 1)) tCont = (double)(N - 1);

    const int idx0 = static_cast<int>(std::floor(tCont));
    const int idx1 = std::min(idx0 + 1, N - 1);
    const double alpha = tCont - (double)idx0;

    const float* m0 = &_imp->xformMatrices[idx0 * 16];
    const float* m1 = &_imp->xformMatrices[idx1 * 16];
    if (idx0 == idx1 || alpha < 1e-9) {
        std::memcpy(mesh->transform, m0, 16 * sizeof(float));
    } else {
        const float a = static_cast<float>(alpha);
        const float oneMinusA = 1.0f - a;
        for (int i = 0; i < 16; ++i) {
            mesh->transform[i] = m0[i] * oneMinusA + m1[i] * a;
        }
    }
}

void
ReadGeo::updateVerticesAtTime(MeshData* mesh, double time) const
{
    if (!mesh || !_imp->hasAnimatedVerts || _imp->numVertexSamples == 0) {
        return;
    }

    // Continuous sample index (frame 1 = sample 0), then lerp between the
    // two bracket samples. Without sub-frame interpolation, motion blur
    // sampling at non-integer times returns snapped vertices and animated
    // meshes don't blur.
    // For 24fps source + project we still get 1:1 at integer frames; for
    // mismatched rates the playback will be off until we add proper
    // sourceFps + projectFps + Frame Offset mapping (mirrors ReadAlembicArchive's
    // timeMode 1 path).
    const int N = static_cast<int>(_imp->numVertexSamples);
    double tCont = time - 1.0;
    if (tCont < 0.0) tCont = 0.0;
    if (tCont > (double)(N - 1)) tCont = (double)(N - 1);

    const int idx0 = static_cast<int>(std::floor(tCont));
    const int idx1 = std::min(idx0 + 1, N - 1);
    const double alpha = tCont - (double)idx0;

    const std::vector<float>& s0 = _imp->vertexSamples[idx0];
    if (s0.empty() || s0.size() != mesh->vertices.size()) {
        // Sample missing or mismatched topology — skip (mesh keeps its current
        // verts, no flicker).
        return;
    }

    if (idx0 == idx1 || alpha < 1e-9) {
        std::memcpy(mesh->vertices.data(), s0.data(), s0.size() * sizeof(float));
        return;
    }

    const std::vector<float>& s1 = _imp->vertexSamples[idx1];
    if (s1.size() != s0.size()) {
        // Topology mismatch between brackets — fall back to lower sample.
        std::memcpy(mesh->vertices.data(), s0.data(), s0.size() * sizeof(float));
        return;
    }

    const float a = static_cast<float>(alpha);
    const float oneMinusA = 1.0f - a;
    float* dst = mesh->vertices.data();
    const float* src0 = s0.data();
    const float* src1 = s1.data();
    const size_t n = s0.size();
    for (size_t k = 0; k < n; ++k) {
        dst[k] = src0[k] * oneMinusA + src1[k] * a;
    }
}

// ---------------------------------------------------------------------------
// Render (outputs a 1x1 transparent image, same as Light3D / Scene3D)
// ---------------------------------------------------------------------------

StatusEnum
ReadGeo::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                               ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = 1;
    rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadGeo::getPreferredMetadata(NodeMetadata& metadata)
{
    // Declare this node as frame-varying when the loaded .abc carries
    // animated content (transform samples OR vertex samples). Required so
    // downstream Scene3D + ScanlineRender hashes change per frame, otherwise
    // the image cache freezes the render output on a single frame even though
    // the geometry actually animates. The 3D viewport bypasses this cache so
    // it always shows live motion; CyclesRender sets frame-varying on its own
    // node which is why Cycles worked while ScanlineRender did not.
    //
    // Static .obj / single-sample .abc imports stay non-frame-varying so the
    // cache does its job (one render reused across all frames).
    if (_imp && (_imp->hasAnimatedXform || _imp->hasAnimatedVerts)) {
        metadata.setIsFrameVarying(true);
    }
    return eStatusOK;
}

StatusEnum
ReadGeo::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

// ---------------------------------------------------------------------------
// MaterialProvider
// ---------------------------------------------------------------------------

void
ReadGeo::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}

double ReadGeo::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }

double ReadGeo::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double ReadGeo::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5; }

void
ReadGeo::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}

double ReadGeo::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }

double ReadGeo::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }

std::string ReadGeo::getMaterialDiffuseColorspace() const
{
    KnobChoicePtr k = _imp->diffuseColorspace.lock();
    int idx = k ? k->getValue() : 0;
    const char* names[] = {"sRGB", "Linear", "ACEScg", "Raw"};
    return (idx >= 0 && idx < 4) ? names[idx] : "sRGB";
}

std::string ReadGeo::getMaterialTextureFile() const
{ KnobFilePtr k = _imp->textureFile.lock(); return k ? k->getValue() : std::string(); }

bool ReadGeo::hasMaterialInput() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}

MaterialProvider* ReadGeo::getConnectedMaterial() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ReadGeo.cpp"
