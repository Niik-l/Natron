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
#include "ReadGeoImport.h"
#include "../DotUtils.h"

#include "../../NodeMetadata.h"
#include "../../NodeSerialization.h"
#include "../../KnobSerialization.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib> // strtol
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
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
    SubdivisionKnobs subdKnobs;   // Subdivision (Cycles) group

    KnobFileWPtr filePath;
    KnobChoiceWPtr objectPath;
    KnobButtonWPtr reloadBtn;
    KnobBoolWPtr reverseNormals;
    KnobStringWPtr info;

    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobDoubleWPtr uniformScale;

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

    // Static-frame controls for animated scene files (glTF/FBX; also applies
    // to .abc sampling) â€” "show static frame X":
    KnobBoolWPtr freezeAtFrame;
    KnobIntWPtr freezeFrame;
    // Single-part selection: bake the part's evaluated world matrix into the
    // vertices instead of carrying it in MeshData::transform.
    KnobBoolWPtr bakeTransforms;

    // --- Morph Weights (glTF shape keys / FBX blend shapes) ---
    // Per-target user weights. The morphW<i> knobs are created dynamically at
    // load (one per distinct morph-set target) and removed when the file
    // changes; morphNames serializes the ordered target list so
    // onKnobsAboutToBeLoaded can recreate the knobs BEFORE loadKnobs and the
    // saved values/keyframes restore by name. useFileMorphWeights off =
    // the knobs drive every set that has knobs (via rt.morphOverrideWeights).
    KnobPageWPtr filePage;
    KnobBoolWPtr useFileMorphWeights;
    KnobStringWPtr morphNames;
    std::vector<KnobDoubleWPtr> morphWeightKnobs;
    std::vector<int> morphKnobSetIds;     // parallel: knob i -> morph set id
    std::vector<int> morphKnobTargetIdx;  // parallel: knob i -> target index

    // Re-entrancy guard: prevents recursive knobChanged calls
    // when we programmatically update knobs during loading.
    bool isLoading = false;

    // Scene formats (glTF/FBX) read through the shared archive runtime â€” the
    // same parse + per-frame evaluation machinery ReadGeoArchive uses. Guarded
    // by _meshMutex (loadGeoFromFile and getMeshData both take it).
    GeoArchiveRuntime rt;
    bool sceneLoaded = false;
    std::vector<int> meshEntries;         // indices into rt.data.nodes (isMesh)
    std::vector<std::string> partLabels;  // Object dropdown labels

    // Built merged/single meshes, keyed by (frame key, Object selection,
    // Bake Transforms). Cleared on load and on freeze/bake knob changes.
    mutable std::map<std::tuple<int, int, bool>, MeshDataPtr> builtCache;

    // Cached Xform data for animated transforms
    bool hasAnimatedXform = false;
    size_t numXformSamples = 0;
    std::vector<float> xformMatrices; // numXformSamples * 16 floats (column-major)

    // Cached vertex samples for animated (deforming) meshes â€” populated by
    // loadAlembicGeo when the .abc has more than one PolyMesh / SubD sample
    // (e.g. a morphing terrain or animated character mesh). Pre-loaded into
    // memory at file-load time so per-frame fetches are a cheap memcpy.
    // Topology (faceIndices / faceCounts) is assumed constant across samples
    // â€” true for the typical exporter (Maya / Blender / Houdini Alembic).
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
       "Import geometry from .glb/.gltf (glTF), .fbx, .abc (Alembic) or .obj "
       "(Wavefront) files. The parser dispatches on the file extension.\n\n"
       "Alembic: PolyMesh, SubD, and Points objects. Object dropdown picks "
       "which entry from a multi-mesh archive to load.\n\n"
#else
       "Import geometry from .glb/.gltf (glTF), .fbx, or .obj (Wavefront) "
       "files. The parser dispatches on the file extension.\n\n"
       "Alembic (.abc) support is not built into this binary â€” rebuild with "
       "Alembic to enable it.\n\n"
#endif
       "glTF/FBX: the Object dropdown lists \"All parts (merged)\" (the whole "
       "asset, each part's file transform baked in) plus one entry per part. "
       "Animated glTF/FBX files play on the timeline: node transforms and "
       "skinned vertices are evaluated per frame (rigid transforms interpolate "
       "for motion blur). Freeze at Frame pins the evaluation to a fixed frame "
       "â€” the static-snapshot mode. The file's PBR material (base color, "
       "roughness, metallic, emission, transmission, IOR, diffuse texture) is "
       "applied to the node's Material and Texture Maps pages.\n\n"
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
    _imp->filePage = page;

    {
        KnobFilePtr fp = AppManager::createKnob<KnobFile>(this, tr("File"));
        fp->setName("filename");
        fp->setHintToolTip(tr("Path to a .glb/.gltf (glTF), .fbx, .abc (Alembic) or .obj (Wavefront) geometry file."));
        fp->setAnimationEnabled(false);
        page->addKnob(fp);
        _imp->filePath = fp;
    }

    {
        KnobChoicePtr obj = AppManager::createKnob<KnobChoice>(this, tr("Object"));
        obj->setName("objectPath");
        obj->setHintToolTip(tr("Select which geometry object to load. "
                               "\"All parts (merged)\" loads the whole asset with each "
                               "part's file transform baked in at the evaluated frame; "
                               "the individual entries load a single part."));
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
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Freeze at Frame"));
        k->setName("freezeAtFrame");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Evaluate the file at a fixed frame instead of following the "
                             "timeline. Turns an animated glTF/FBX (or .abc) into a static "
                             "snapshot â€” the built mesh never changes with the playhead."));
        page->addKnob(k);
        _imp->freezeAtFrame = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Freeze Frame"));
        k->setName("freezeFrame");
        k->setDefaultValue(1);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("The frame the file is evaluated at while Freeze at Frame is on. "
                             "Changing it re-evaluates the snapshot immediately."));
        page->addKnob(k);
        _imp->freezeFrame = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Bake Transforms"));
        k->setName("bakeTransforms");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Bake the part's file-authored transform into the vertices at "
                             "the evaluated frame. Only applies when a single part is picked "
                             "in the Object dropdown â€” merged mode always bakes. When off, "
                             "the part keeps its node-local vertices and the file's transform "
                             "animates through the node's transform slot (the user "
                             "Translate/Rotate/Scale composes on top of it)."));
        page->addKnob(k);
        _imp->bakeTransforms = k;
    }

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Use File Morph Weights"));
        k->setName("useFileMorphWeights");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("When on, morph target (shape key) weights come from the "
                             "file's animation or rest values. When off, the per-target "
                             "Morph Weight knobs on this page drive every target instead "
                             "â€” keyframable, so you can animate your own blends. The "
                             "knobs appear after a file with morph targets is loaded."));
        page->addKnob(k);
        _imp->useFileMorphWeights = k;
    }
    {
        // Serialization container for the morph-weight knobs: one line per
        // target, "<setId>|<targetIdx>|<label>" (see geoArchiveEncodeMorphList).
        // Hidden from the GUI; the knob list itself is rebuilt from it on
        // project load (onKnobsAboutToBeLoaded) so values + keyframes restore.
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Morph Target List"));
        k->setName("morphNames");
        k->setAsMultiLine();
        k->setDefaultValue(std::string());
        k->setSecret(true);
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->morphNames = k;
    }

    {
        // Same compact read-only style as ReadGeoArchive: single-line LABEL
        // mode renders the value as plain text in the row's label column.
        KnobStringPtr infoKnob = AppManager::createKnob<KnobString>(this, tr("Info"));
        infoKnob->setName("info");
        infoKnob->setAsLabel();
        infoKnob->setAnimationEnabled(false);
        infoKnob->setEvaluateOnChange(false);
        infoKnob->setIsPersistent(false);
        infoKnob->setDefaultValue("Set file path to a .glb/.gltf, .fbx, .abc or .obj file.");
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
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Uniform Scale"));
        k->setName("uniformScale"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0001);
        k->setDisplayMinimum(0.001); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Multiplies Scale X/Y/Z, so the whole object resizes from one "
                             "knob. The display range goes below 0.01 on purpose: imported "
                             "assets are often authored in centimetres or millimetres, and "
                             "0.01 (cm to m) or 0.001 (mm to m) is the usual correction."));
        xformPage->addKnob(k); _imp->uniformScale = k;
    }

    // Material page
    createSubdivisionKnobs(this, page, _imp->subdKnobs);

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
    // Texture Maps page â€” order: Diffuse, Metallic, Roughness, Emission, Normal
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

    // Freeze at Frame / Freeze Frame: the built-mesh cache is keyed by the
    // source frame key, so a freeze change remaps what a key evaluates to â€”
    // drop the built meshes. Freezing also toggles frame-varying metadata
    // (a frozen file is static), so refresh it.
    if (_imp->freezeAtFrame.lock().get() == k || _imp->freezeFrame.lock().get() == k) {
        {
            std::lock_guard<std::mutex> lk(_meshMutex);
            _imp->builtCache.clear();
        }
        refreshMetadata_public(true);
        return true;
    }

    // Bake Transforms: only re-keys the single-part build (no timing change),
    // so drop the built meshes but leave the metadata alone.
    if (_imp->bakeTransforms.lock().get() == k) {
        std::lock_guard<std::mutex> lk(_meshMutex);
        _imp->builtCache.clear();
        return true;
    }

    // Morph-weight edits (Use File Morph Weights off): the runtime's frame
    // cache and the built-mesh cache both embed the weights, so drop both â€”
    // same pattern as the Bake Transforms branch (ReadGeoArchive mirrors it).
    bool isMorphKnob = (_imp->useFileMorphWeights.lock().get() == k);
    if (!isMorphKnob) {
        for (std::size_t i = 0; i < _imp->morphWeightKnobs.size() && !isMorphKnob; ++i) {
            isMorphKnob = (_imp->morphWeightKnobs[i].lock().get() == k);
        }
    }
    if (isMorphKnob) {
        {
            std::lock_guard<std::mutex> lk(_meshMutex);
            KnobBoolPtr uf = _imp->useFileMorphWeights.lock();
            _imp->rt.morphOverrideEnabled = uf ? !uf->getValue() : false;
            if (!_imp->rt.morphOverrideEnabled) {
                _imp->rt.morphOverrideWeights.clear();
            }
            _imp->rt.clearFrameCache();
            _imp->builtCache.clear();
        }
        // The toggle changes frame-varying semantics (animated user weights);
        // a plain weight edit does not.
        if (_imp->useFileMorphWeights.lock().get() == k) {
            refreshMetadata_public(true);
        }
        return true;
    }

    return false;
}

void
ReadGeo::loadGeoFromFile(const std::string& path)
{
    // Serialise (re)loads against getMeshData readers on render threads.
    std::lock_guard<std::mutex> lk(_meshMutex);

    // Drop the previous geometry FIRST â€” clearing the path or a failed load
    // must not keep serving the old mesh (stale-render class).
    _lastMeshData.reset();

    // Any non-scene load below clears the scene-format state; a scene load
    // re-populates it (loadSceneGeo). Clearing also covers the built-mesh
    // cache (keyed by frame/selection/bake â€” meaningless across files).
    _imp->sceneLoaded = false;
    _imp->builtCache.clear();

    if (path.empty()) {
        // No file â€” no morph targets either; drop any weight knobs from a
        // previously loaded file.
        std::vector<GeoMorphTargetRef> none;
        if (syncMorphWeightKnobs(none)) {
            recreateKnobs(true);
        }
        refreshMetadata_public(true);
        return;
    }

    // Helper: lowercase extension of `path` ("foo.OBJ" â†’ ".obj").
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
    } else if (e == ".gltf" || e == ".glb" || e == ".fbx") {
        loadSceneGeo(path);
    } else {
        setPersistentMessage(eMessageTypeError,
            "Unsupported file extension. Use .glb/.gltf, .fbx, .abc (Alembic) or .obj (Wavefront).");
    }

    // Non-scene formats carry no morph targets: drop any weight knobs left
    // over from a previous scene-format file (the runtime is not touched
    // here â€” loadSceneGeo manages it for the scene formats).
    if (!(e == ".gltf" || e == ".glb" || e == ".fbx")) {
        std::vector<GeoMorphTargetRef> none;
        if (syncMorphWeightKnobs(none)) {
            recreateKnobs(true);
        }
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
    // fires for it, so the geometry is never read into memory â€” the user used to
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
            // (faceIndices/faceCounts) is assumed constant â€” only positions
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

            // Read UVs â€” try common Alembic UV param names
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

    // Identity transform â€” the node's own Translate/Rotate/Scale knobs handle
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
// glTF / FBX loading through the shared archive runtime
// ---------------------------------------------------------------------------

namespace {

// Append `src` to `dst`, baking the entry's world matrix `world` into the
// appended vertices (identity for skinned glTF entries â€” their deformed
// vertices are already in world space) and re-basing vertex indices. UVs are
// per face-vertex, parallel to faceIndices: parts without UVs contribute
// zero-padded slots so the interleaving stays aligned. `partColor` (when
// non-null) is baked as per face-corner colors â€” a merged multi-material
// asset keeps each part's file material color on its faces (single MeshData =
// no per-part material slots; consumers shade the face colors). Call
// importBuildEdgeIndices() once after all parts are appended.
void
appendArchiveMesh(const MeshData& src, const float world[16], MeshData* dst,
                  const float partColor[3] = nullptr)
{
    const std::size_t vertexOffset = dst->numVertices;
    const std::size_t faceIdxBegin = dst->faceIndices.size();

    // Vertices: copy, then bake `world` over the appended slice only.
    dst->vertices.reserve(dst->vertices.size() + src.vertices.size());
    dst->vertices.insert(dst->vertices.end(), src.vertices.begin(), src.vertices.end());
    if (world) {
        const std::size_t end = dst->vertices.size();
        for (std::size_t i = vertexOffset * 3; i + 2 < end; i += 3) {
            const float x = dst->vertices[i];
            const float y = dst->vertices[i + 1];
            const float z = dst->vertices[i + 2];
            dst->vertices[i]     = world[0]  * x + world[4]  * y + world[8]  * z + world[12];
            dst->vertices[i + 1] = world[1]  * x + world[5]  * y + world[9]  * z + world[13];
            dst->vertices[i + 2] = world[2]  * x + world[6]  * y + world[10] * z + world[14];
        }
    }
    dst->numVertices += src.numVertices;

    for (std::size_t i = 0; i < src.faceIndices.size(); ++i) {
        dst->faceIndices.push_back(src.faceIndices[i] + (int)vertexOffset);
    }
    dst->faceCounts.insert(dst->faceCounts.end(),
                           src.faceCounts.begin(), src.faceCounts.end());
    dst->numFaces += src.faceCounts.size();

    if (partColor) {
        dst->vertexColors.resize(faceIdxBegin * 3, 0.0f);
        for (std::size_t i = 0; i < src.faceIndices.size(); ++i) {
            dst->vertexColors.push_back(partColor[0]);
            dst->vertexColors.push_back(partColor[1]);
            dst->vertexColors.push_back(partColor[2]);
        }
        dst->hasVertexColors = true;
    }

    const bool srcUVs = src.hasUVs && src.texCoordComponents == 2;
    if (srcUVs) {
        dst->hasUVs = true;
        dst->texCoordComponents = 2;
    }
    if (dst->hasUVs && dst->texCoordComponents == 2) {
        // Zero-pad any earlier parts that had no UVs so the slots stay aligned.
        dst->uvs.resize(faceIdxBegin * 2, 0.0f);
        if (srcUVs) {
            dst->uvs.insert(dst->uvs.end(), src.uvs.begin(), src.uvs.end());
        } else {
            dst->uvs.resize(faceIdxBegin * 2 + src.faceIndices.size() * 2, 0.0f);
        }
    }
}

} // anon

void
ReadGeo::loadSceneGeo(const std::string& path)
{
    // Caller (loadGeoFromFile) holds _meshMutex and the isLoading guard.

    // Label by the file's actual container: .glb vs .gltf (XML/JSON) vs .fbx.
    const std::size_t dot = path.find_last_of('.');
    const bool isFbx = (dot != std::string::npos) &&
        _stricmp(path.c_str() + dot, ".fbx") == 0;
    const bool isGlb = (dot != std::string::npos) &&
        _stricmp(path.c_str() + dot, ".glb") == 0;
    const char* fmtName = isFbx ? "FBX" : (isGlb ? "GLB" : "glTF");

    // Reset the scene-format state: the runtime re-parses from scratch and
    // the built-mesh cache is invalidated with it (freezes/bakes/selections
    // all re-evaluate from the new file).
    _imp->sceneLoaded = false;
    _imp->meshEntries.clear();
    _imp->partLabels.clear();
    _imp->builtCache.clear();
    _imp->rt.clear();

    const bool reverse = _imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue();
    std::string err;
    if (!_imp->rt.loadFile(path, reverse, err)) {
        setPersistentMessage(eMessageTypeError, err);
        return;
    }
    const GeoArchiveResult& data = _imp->rt.data;

    // Mesh entries are the renderable parts (glTF primitives / FBX
    // mesh x material); transform-only nodes are skipped.
    for (int i = 0; i < (int)data.nodes.size(); ++i) {
        if (data.nodes[i].isMesh) _imp->meshEntries.push_back(i);
    }
    if (_imp->meshEntries.empty()) {
        setPersistentMessage(eMessageTypeError,
            std::string(fmtName) + ": no triangle geometry found in the file");
        return;
    }

    // Object dropdown: entry 0 is the merged asset, then one entry per part
    // (scene paths like "/Car/Body").
    _imp->partLabels.push_back(std::string("All parts (merged)"));
    for (std::size_t i = 0; i < _imp->meshEntries.size(); ++i) {
        _imp->partLabels.push_back(data.nodes[_imp->meshEntries[i]].fullPath);
    }
    KnobChoicePtr objKnob = _imp->objectPath.lock();
    int chosenIdx = 0;
    if (objKnob) {
        std::vector<ChoiceOption> entries;
        for (std::size_t i = 0; i < _imp->partLabels.size(); ++i) {
            entries.push_back(ChoiceOption(_imp->partLabels[i], "", ""));
        }
        objKnob->populateChoices(entries);
        chosenIdx = objKnob->getValue();
        if (chosenIdx < 0 || (std::size_t)chosenIdx >= _imp->partLabels.size()) {
            chosenIdx = 0;
            objKnob->setValue(0);
        }
    }

    _imp->sceneLoaded = true;

    // Publish the initial mesh (frame 1). Per-frame fetches go through
    // getMeshData -> buildSceneMesh from here on.
    MeshDataPtr mesh = buildSceneMesh(1.0);

    // Material: the selected part's (single mode), else the first part that
    // carries one in the file. With no material in the file the knobs are
    // left alone (user's manual settings stay).
    const GeoImportMaterial* mat = NULL;
    if (chosenIdx > 0) {
        const GeoArchiveNodeData& nd = data.nodes[_imp->meshEntries[chosenIdx - 1]];
        const GeoArchiveMeshData& md = data.meshes[nd.meshIndex];
        if (md.materialIndex >= 0 && md.materialIndex < (int)data.materials.size() &&
            data.materials[md.materialIndex].present) {
            mat = &data.materials[md.materialIndex];
        }
    } else {
        for (std::size_t i = 0; i < _imp->meshEntries.size() && !mat; ++i) {
            const GeoArchiveNodeData& nd = data.nodes[_imp->meshEntries[i]];
            const GeoArchiveMeshData& md = data.meshes[nd.meshIndex];
            if (md.materialIndex >= 0 && md.materialIndex < (int)data.materials.size() &&
                data.materials[md.materialIndex].present) {
                mat = &data.materials[md.materialIndex];
            }
        }
    }
    if (mat) {
        applyImportedMaterialKnobs(*mat);
    }

    _imp->loadedFilePath = path;
    _imp->loadedObjectPath = _imp->partLabels[chosenIdx];

    // Compact single line, same style as ReadGeoArchive's info.
    std::ostringstream ss;
    ss << _imp->meshEntries.size() << " parts, "
       << (mesh ? mesh->numVertices : 0) << " verts, "
       << (mesh ? mesh->numFaces : 0) << " faces";
    if (_imp->rt.isFbx) {
        ss << " | FBX";
    } else {
        ss << " | glTF";
    }
    if (mesh && mesh->hasUVs) {
        ss << " | UVs";
    }
    // Morph targets (shape keys): count targets over DISTINCT morph sets â€”
    // the material pieces of one FBX node mesh share one set.
    {
        std::set<const GeoMorphSet*> seen;
        std::size_t numMorphs = 0;
        for (std::size_t i = 0; i < _imp->meshEntries.size(); ++i) {
            const GeoArchiveMeshData& md = data.meshes[data.nodes[_imp->meshEntries[i]].meshIndex];
            if (md.morphs && seen.insert(md.morphs.get()).second) {
                numMorphs += md.morphs->targets.size();
            }
        }
        if (numMorphs > 0) {
            ss << " | " << numMorphs << " morphs";
        }
    }
    if (_imp->rt.hasAnimation) {
        ss << " | anim";
        if (_imp->freezeAtFrame.lock() && _imp->freezeAtFrame.lock()->getValue()) {
            ss << " (frozen@" << (_imp->freezeFrame.lock() ? _imp->freezeFrame.lock()->getValue() : 1) << ")";
        }
    }
    if (!mat) {
        ss << " | no material";
    }
    _imp->info.lock()->setValue(ss.str());

    // Morph Weight knobs: sync to the file's distinct morph sets (no-op when
    // the file has none â€” the knobs drop). Caller (loadGeoFromFile) holds
    // _meshMutex; the panel refresh inside recreateKnobs never re-enters it,
    // so the call can run under the lock.
    {
        std::vector<GeoMorphTargetRef> targets;
        collectMorphTargets(&targets);
        if (syncMorphWeightKnobs(targets)) {
            recreateKnobs(true);
        }
    }

    clearPersistentMessage(false);
}

MeshDataPtr
ReadGeo::buildSceneMesh(double time) const
{
    // Caller holds _meshMutex (getMeshData / loadGeoFromFile serialise on it,
    // which also guards the archive data and the built-mesh cache below).
    ReadGeoPrivate* imp = _imp.get();

    // Frozen evaluation: frame N maps to source key N-1 (frame 1 = key 0).
    const bool frozen = imp->freezeAtFrame.lock() && imp->freezeAtFrame.lock()->getValue();
    const int frozenFrame = imp->freezeFrame.lock() ? imp->freezeFrame.lock()->getValue() : 1;
    int key;
    if (frozen) {
        key = frozenFrame > 1 ? frozenFrame - 1 : 0;
    } else {
        const double f = time - 1.0;
        key = (int)std::floor(f);
        if (key < 0) key = 0;
    }

    const KnobChoicePtr objKnob = imp->objectPath.lock();
    int chosenIdx = objKnob ? objKnob->getValue() : 0;
    if (chosenIdx < 0 || (std::size_t)chosenIdx >= imp->partLabels.size()) {
        chosenIdx = 0;
    }

    const bool bake = imp->bakeTransforms.lock() && imp->bakeTransforms.lock()->getValue();

    const std::tuple<int, int, bool> cacheKey(key, chosenIdx, bake);
    {
        std::map<std::tuple<int, int, bool>, MeshDataPtr>::const_iterator it =
            imp->builtCache.find(cacheKey);
        if (it != imp->builtCache.end()) return it->second;
    }

    const double fps = getApp() ? getApp()->getProjectFrameRate() : 24.0;
    // Non-frozen sub-frame time (motion blur) lerps rigid transforms via
    // worldMatrixAt; the frozen evaluation maps to the exact frame.
    const double timeEff = frozen ? (double)(key + 1) : time;

    // Morph-weight override: fill the runtime store from the user's weight
    // knobs at the frame time the key maps to (matching the frame-cache
    // granularity â€” the same snap the file's animated weights get through
    // ensureFrameState). Must run before ensureFrameState: the evaluators
    // read the store. Caller holds _meshMutex.
    applyMorphOverrideAt((double)(key + 1));

    const std::shared_ptr<const GeoArchiveFrameEval> st = imp->rt.ensureFrameState(fps, key);

    MeshDataPtr mesh(new MeshData);
    const GeoArchiveResult& data = imp->rt.data;

    if (chosenIdx == 0) {
        // Merged: every mesh entry's evaluated vertices baked through its
        // world matrix, concatenated â€” the whole asset as one mesh. Each
        // part's file material color bakes onto its face corners (the merged
        // mesh has no per-part material slots; multi-material files keep
        // their per-part colors as face colors).
        for (std::size_t e = 0; e < imp->meshEntries.size(); ++e) {
            const int nodeIdx = imp->meshEntries[e];
            if (nodeIdx >= (int)data.nodes.size()) continue;
            const GeoArchiveNodeData& nd = data.nodes[nodeIdx];
            if (nd.meshIndex < 0 || nd.meshIndex >= (int)st->deformed.size() ||
                !st->deformed[nd.meshIndex]) {
                continue;
            }
            float partColor[3] = { -1.f, 0.f, 0.f };
            const GeoArchiveMeshData& md = data.meshes[nd.meshIndex];
            if (md.materialIndex >= 0 && md.materialIndex < (int)data.materials.size() &&
                data.materials[md.materialIndex].present) {
                const GeoImportMaterial& m = data.materials[md.materialIndex];
                partColor[0] = (float)m.baseColor[0];
                partColor[1] = (float)m.baseColor[1];
                partColor[2] = (float)m.baseColor[2];
            }
            float world[16];
            imp->rt.worldMatrixAt(nodeIdx, timeEff, fps, 0, world);
            if (std::getenv("NATRON_DEBUG_GEOIO")) {
                std::fprintf(stderr,
                             "[GEOIO] merged part %d node %d worldDiag=(%.4f,%.4f,%.4f) tr=(%.4f,%.4f,%.4f)\n",
                             (int)e, nodeIdx, world[0], world[5], world[10],
                             world[12], world[13], world[14]);
                const MeshData& srcM = *st->deformed[nd.meshIndex];
                if (srcM.vertices.size() >= 3) {
                    std::fprintf(stderr,
                                 "[GEOIO]   src v0=(%.4f,%.4f,%.4f) srcNumVerts=%d\n",
                                 srcM.vertices[0], srcM.vertices[1],
                                 srcM.vertices[2], (int)srcM.numVertices);
                }
            }
            appendArchiveMesh(*st->deformed[nd.meshIndex], world, mesh.get(),
                              partColor[0] >= 0.f ? partColor : nullptr);
            if (std::getenv("NATRON_DEBUG_GEOIO") && mesh->vertices.size() >= 3) {
                std::fprintf(stderr,
                             "[GEOIO]   baked v0=(%.4f,%.4f,%.4f)\n",
                             mesh->vertices[0], mesh->vertices[1],
                             mesh->vertices[2]);
            }
        }
    } else if (chosenIdx - 1 < (int)imp->meshEntries.size()) {
        // Single part.
        const int nodeIdx = imp->meshEntries[chosenIdx - 1];
        if (nodeIdx < (int)data.nodes.size()) {
            const GeoArchiveNodeData& nd = data.nodes[nodeIdx];
            MeshDataPtr src;
            if (nd.meshIndex >= 0 && nd.meshIndex < (int)st->deformed.size()) {
                src = st->deformed[nd.meshIndex];
            }
            if (src) {
                // Single part: bake the part's material color on its faces
                // (the merged-style face-color consumption applies 1:1).
                float partColor[3] = { -1.f, 0.f, 0.f };
                const GeoArchiveMeshData& md = data.meshes[nd.meshIndex];
                if (md.materialIndex >= 0 && md.materialIndex < (int)data.materials.size() &&
                    data.materials[md.materialIndex].present) {
                    const GeoImportMaterial& m = data.materials[md.materialIndex];
                    partColor[0] = (float)m.baseColor[0];
                    partColor[1] = (float)m.baseColor[1];
                    partColor[2] = (float)m.baseColor[2];
                }
                const float* pc = partColor[0] >= 0.f ? partColor : nullptr;
                if (bake) {
                    // Baked: the part's evaluated world matrix goes into the
                    // vertices; MeshData::transform stays identity.
                    float world[16];
                    imp->rt.worldMatrixAt(nodeIdx, timeEff, fps, 0, world);
                    appendArchiveMesh(*src, world, mesh.get(), pc);
                } else {
                    // Non-baked: the part keeps its node-local vertices and
                    // the part's world matrix rides in MeshData::transform â€”
                    // the animation keeps flowing through it and the user
                    // T/R/S composes on top in every consumer.
                    float identity[16];
                    geoArchiveMatIdentity(identity);
                    appendArchiveMesh(*src, identity, mesh.get(), pc);
                    imp->rt.worldMatrixAt(nodeIdx, timeEff, fps, 0, mesh->transform);
                }
            }
        }
    }
    importBuildEdgeIndices(mesh.get());

    // Geometry-loader diagnostics gate (same pattern as NATRON_DEBUG_SC):
    // set env NATRON_DEBUG_GEOIO=1 to dump the raw built geometry (vertex
    // values, not just counts) so headless tests can verify what the loader
    // actually produced â€” a degenerate all-zero vertex array renders as an
    // empty scene and is invisible in the counts alone.
    if (std::getenv("NATRON_DEBUG_GEOIO")) {
        std::ostringstream dbg;
        dbg << "[GEOIO] fmt=" << (imp->rt.isFbx ? "FBX" : "glTF")
            << " frame=" << (key + 1) << " sel=" << imp->partLabels[chosenIdx]
            << " bake=" << (bake ? 1 : 0)
            << " verts=" << mesh->numVertices
            << " faces=" << mesh->numFaces
            << " uvslots=" << (mesh->uvs.size() / 2)
            << " vcolors=" << (mesh->hasVertexColors ? (int)(mesh->vertexColors.size() / 3) : -1)
            << " c0..c5=";
        for (std::size_t i = 0; i < mesh->vertexColors.size() && i < 6; ++i) {
            dbg << mesh->vertexColors[i] << ",";
        }
        dbg << " v0..v5=";
        for (std::size_t i = 0; i < mesh->vertices.size() && i < 6; ++i) {
            dbg << mesh->vertices[i] << ",";
        }
        dbg << " faceCounts0..3=";
        for (std::size_t i = 0; i < mesh->faceCounts.size() && i < 4; ++i) {
            dbg << mesh->faceCounts[i] << ",";
        }
        dbg << " faceIdx0..5=";
        for (std::size_t i = 0; i < mesh->faceIndices.size() && i < 6; ++i) {
            dbg << mesh->faceIndices[i] << ",";
        }
        dbg << " tr=";
        for (int i = 0; i < 16; ++i) {
            dbg << mesh->transform[i] << ",";
        }
        // Vertex bbox: direct deformation evidence (morph weights move the
        // bbox even when v0..v5 happen to be unmoved corners).
        if (!mesh->vertices.empty()) {
            float bmn[3] = { mesh->vertices[0], mesh->vertices[1], mesh->vertices[2] };
            float bmx[3] = { bmn[0], bmn[1], bmn[2] };
            for (std::size_t i = 0; i + 2 < mesh->vertices.size(); i += 3) {
                for (int a = 0; a < 3; ++a) {
                    bmn[a] = std::min(bmn[a], mesh->vertices[i + a]);
                    bmx[a] = std::max(bmx[a], mesh->vertices[i + a]);
                }
            }
            dbg << " bbox=[" << bmn[0] << "," << bmn[1] << "," << bmn[2]
                << " .. " << bmx[0] << "," << bmx[1] << "," << bmx[2] << "]";
        }
        fprintf(stderr, "%s\n", dbg.str().c_str());
    }

    imp->builtCache.insert(std::make_pair(cacheKey, mesh));
    return mesh;
}

void
ReadGeo::applyImportedMaterialKnobs(const GeoImportMaterial& m)
{
    if (!m.present) return;

    if (KnobColorPtr c = _imp->baseColor.lock()) {
        c->setValue(m.baseColor[0], ViewSpec::all(), 0);
        c->setValue(m.baseColor[1], ViewSpec::all(), 1);
        c->setValue(m.baseColor[2], ViewSpec::all(), 2);
    }
    if (KnobDoublePtr k = _imp->roughness.lock()) k->setValue(m.roughness);
    if (KnobDoublePtr k = _imp->metallic.lock()) k->setValue(m.metallic);
    if (KnobDoublePtr k = _imp->specular.lock()) k->setValue(m.specular);
    if (KnobColorPtr c = _imp->emissionColor.lock()) {
        c->setValue(m.emission[0], ViewSpec::all(), 0);
        c->setValue(m.emission[1], ViewSpec::all(), 1);
        c->setValue(m.emission[2], ViewSpec::all(), 2);
    }
    if (KnobDoublePtr k = _imp->emissionStrength.lock()) k->setValue(m.emissionStrength);
    if (KnobDoublePtr k = _imp->transmission.lock()) k->setValue(m.transmission);
    if (KnobDoublePtr k = _imp->ior.lock()) k->setValue(m.ior);
    // Diffuse map: write the resolved path, including clearing it when the
    // file's material has no texture â€” knob state must match file state
    // (a stale texture from a previously loaded file is worse than a blank).
    if (KnobFilePtr k = _imp->textureFile.lock()) k->setValue(m.diffuseMapPath);
}

// ---------------------------------------------------------------------------
// Morph Weights (glTF shape keys / FBX blend shapes)
// ---------------------------------------------------------------------------

void
ReadGeo::collectMorphTargets(std::vector<GeoMorphTargetRef>* out) const
{
    // Caller holds _meshMutex (guards rt.data).
    if (!out) return;
    out->clear();
    if (!_imp->sceneLoaded) return;
    const GeoArchiveResult& data = _imp->rt.data;

    // Distinct sets only: the material pieces of one FBX node mesh share one
    // GeoMorphSet (same pointer); glTF sets are per-primitive. The target
    // order here defines the morphW<i> knob indices â€” keep it stable.
    std::set<const GeoMorphSet*> seen;
    for (std::size_t i = 0; i < _imp->meshEntries.size(); ++i) {
        const int nodeIdx = _imp->meshEntries[i];
        if (nodeIdx < 0 || nodeIdx >= (int)data.nodes.size()) continue;
        const GeoArchiveNodeData& nd = data.nodes[nodeIdx];
        if (nd.meshIndex < 0 || nd.meshIndex >= (int)data.meshes.size()) continue;
        const GeoArchiveMeshData& md = data.meshes[nd.meshIndex];
        if (!md.morphs || md.morphs->targets.empty()) continue;
        if (!seen.insert(md.morphs.get()).second) continue;
        const GeoMorphSet& ms = *md.morphs;
        for (std::size_t t = 0; t < ms.targets.size(); ++t) {
            GeoMorphTargetRef ref;
            ref.setId = ms.id;
            ref.targetIdx = (int)t;
            ref.restWeight = (t < ms.restWeights.size()) ? ms.restWeights[t] : 0.0f;
            ref.label = ms.targets[t].name.empty()
                ? (std::string("target ") + std::to_string(t + 1))
                : ms.targets[t].name;
            // Prefix with the owning part: multi-set files repeat target
            // names ("target 1" fallbacks), and knob labels must stay
            // distinguishable in the panel.
            const std::string& part = nd.fullPath.empty() ? nd.name : nd.fullPath;
            if (!part.empty()) {
                ref.label = part + ": " + ref.label;
            }
            out->push_back(ref);
        }
    }
}

bool
ReadGeo::syncMorphWeightKnobs(const std::vector<GeoMorphTargetRef>& targets)
{
    // Caller holds _meshMutex; runs on the GUI thread (knob add/delete assert
    // it). The morphW<i> script names are index-stable: identical lists keep
    // knob values (project-load flow), a changed list resets them to the new
    // rest weights (file-change flow).
    const std::string newList = geoArchiveEncodeMorphList(targets);
    KnobStringPtr mn = _imp->morphNames.lock();
    const std::string oldList = mn ? mn->getValue() : std::string();
    const bool listChanged = (newList != oldList);

    KnobPagePtr page = _imp->filePage.lock();
    bool structureChanged = false;

    // Shrink from the end so the remaining indices stay stable.
    while (_imp->morphWeightKnobs.size() > targets.size()) {
        KnobDoublePtr k = _imp->morphWeightKnobs.back().lock();
        _imp->morphWeightKnobs.pop_back();
        _imp->morphKnobSetIds.pop_back();
        _imp->morphKnobTargetIdx.pop_back();
        if (k) {
            if (page) page->removeKnob(k.get());
            deleteKnob(k.get(), false);
        }
        structureChanged = true;
    }

    for (std::size_t i = 0; i < targets.size(); ++i) {
        KnobDoublePtr k;
        if (i < _imp->morphWeightKnobs.size()) {
            k = _imp->morphWeightKnobs[i].lock();
        }
        if (!k) {
            const std::string label = targets[i].label.empty()
                ? ("Morph " + std::to_string(i + 1)) : targets[i].label;
            k = AppManager::createKnob<KnobDouble>(this, tr(label.c_str()));
            k->setName("morphW" + std::to_string(i));
            k->setDefaultValue(targets[i].restWeight);
            k->setAnimationEnabled(true);
            k->setMinimum(0.0);
            k->setMaximum(1.0);
            k->setDisplayMinimum(0.0);
            k->setDisplayMaximum(1.0);
            k->setHintToolTip(tr("Weight of this morph target (shape key). 0 = rest "
                                 "pose, 1 = fully applied. Only used when Use File "
                                 "Morph Weights is off."));
            if (page) page->addKnob(k);
            _imp->morphWeightKnobs.push_back(k);
            structureChanged = true;
        }
        k->setLabel(targets[i].label);
        if (i >= _imp->morphKnobSetIds.size()) {
            _imp->morphKnobSetIds.push_back(targets[i].setId);
            _imp->morphKnobTargetIdx.push_back(targets[i].targetIdx);
        } else {
            _imp->morphKnobSetIds[i] = targets[i].setId;
            _imp->morphKnobTargetIdx[i] = targets[i].targetIdx;
        }
        _imp->morphWeightKnobs[i] = k;
        if (listChanged) {
            // Reset to the new file's rest weight when the target list
            // changed (a saved value from another file would apply to a
            // different shape key). During a project load this is a no-op:
            // the list matches the saved one and loadKnobs already restored
            // the values.
            k->setValue(targets[i].restWeight);
        }
    }

    if (mn && listChanged) {
        mn->setValue(newList);
    }

    // NOTE: the caller calls recreateKnobs(true) when this returns true â€”
    // ReadGeoArchive must do that AFTER releasing its archive mutex (the
    // panel refresh must not run under the lock).
    return structureChanged;
}

void
ReadGeo::applyMorphOverrideAt(double time) const
{
    // Caller holds _meshMutex. Fill the runtime's override store from the
    // weight knobs; the evaluators read it inside ensureFrameState. When Use
    // File Morph Weights is on (the default) this is a no-op and the file's
    // animated/rest weights apply.
    ReadGeoPrivate* imp = _imp.get();
    KnobBoolPtr uf = imp->useFileMorphWeights.lock();
    const bool overrideOn = uf && !uf->getValue();
    imp->rt.morphOverrideEnabled = overrideOn;
    if (!overrideOn) {
        imp->rt.morphOverrideWeights.clear();
        return;
    }
    imp->rt.morphOverrideWeights.clear();
    for (std::size_t i = 0; i < imp->morphWeightKnobs.size(); ++i) {
        KnobDoublePtr k = imp->morphWeightKnobs[i].lock();
        if (!k || i >= imp->morphKnobSetIds.size() || i >= imp->morphKnobTargetIdx.size()) {
            continue;
        }
        const int setId = imp->morphKnobSetIds[i];
        const int tIdx = imp->morphKnobTargetIdx[i];
        if (setId < 0 || tIdx < 0) continue;
        std::vector<float>& v = imp->rt.morphOverrideWeights[setId];
        if ((std::size_t)tIdx >= v.size()) {
            v.resize((std::size_t)tIdx + 1, 0.0f);
        }
        v[(std::size_t)tIdx] = (float)k->getValueAtTime(time);
    }
}

void
ReadGeo::onKnobsAboutToBeLoaded(const NodeSerializationPtr& serialization)
{
    // Project load: dynamic morphW<i> knobs must exist BEFORE loadKnobs so
    // the saved values + keyframes match by name. The saved `morphNames`
    // string (a persistent knob created in initializeKnobs, so its own value
    // restores normally) carries the ordered target list; rebuild the knobs
    // from it without touching values (loadKnobs restores them right after).
    if (!serialization) {
        return;
    }
    std::string saved;
    const std::list<KnobSerializationPtr>& serializations = serialization->getKnobsValues();
    for (std::list<KnobSerializationPtr>::const_iterator it = serializations.begin();
         it != serializations.end(); ++it) {
        if ((*it)->getKnob()->getName() == "morphNames") {
            KnobStringBase* isString = dynamic_cast<KnobStringBase*>((*it)->getKnob().get());
            if (isString) {
                saved = isString->getValue();
            }
            break;
        }
    }
    if (saved.empty()) {
        return;
    }
    std::vector<GeoMorphTargetRef> targets;
    if (!geoArchiveDecodeMorphList(saved, &targets) || targets.empty()) {
        return;
    }

    // Create the knobs from the saved list only (no runtime needed). The
    // parallel vectors must be filled too â€” buildSceneMesh maps knob i ->
    // (setId, targetIdx) through them.
    KnobPagePtr page = _imp->filePage.lock();
    _imp->morphWeightKnobs.clear();
    _imp->morphKnobSetIds.clear();
    _imp->morphKnobTargetIdx.clear();
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const std::string label = targets[i].label.empty()
            ? ("Morph " + std::to_string(i + 1)) : targets[i].label;
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(label.c_str()));
        k->setName("morphW" + std::to_string(i));
        k->setDefaultValue(targets[i].restWeight);
        k->setAnimationEnabled(true);
        k->setMinimum(0.0);
        k->setMaximum(1.0);
        k->setDisplayMinimum(0.0);
        k->setDisplayMaximum(1.0);
        if (page) page->addKnob(k);
        _imp->morphWeightKnobs.push_back(k);
        _imp->morphKnobSetIds.push_back(targets[i].setId);
        _imp->morphKnobTargetIdx.push_back(targets[i].targetIdx);
    }
    // No recreateKnobs here: on a project load the panel builds after the
    // values are in (or refreshes in onKnobsLoaded's load path).
}

// ---------------------------------------------------------------------------
// Mesh data access
// ---------------------------------------------------------------------------

MeshDataPtr
ReadGeo::getMeshData(double time) const
{
    // Serialise against a concurrent reload swapping/clearing _lastMeshData.
    std::lock_guard<std::mutex> lk(_meshMutex);

    // Scene formats (glTF/FBX) always go through the per-frame builder, which
    // caches its builds - frozen files evaluate at the frozen frame, animated
    // ones at the requested time.
    if (_imp && _imp->sceneLoaded) {
        MeshDataPtr built = buildSceneMesh(time);
        if (!built) return built;
        // G1 copy-on-read (2026-10-17): hand out an immutable snapshot instead
        // of the cached build. Consumers (CyclesRender's worker-thread sync,
        // the GL viewport, downstream MeshProvider readers) previously shared
        // the cached object while a concurrent getMeshData at another time
        // mutated it in place - heap tearing, wrong-render across concurrent
        // consumers, and access violations deep in the readers (observed as
        // the recurring template-load crash inside syncSceneWithCamera).
        // Cost: one MeshData copy per call, taken under _meshMutex so the
        // snapshot is always consistent.
        return MeshDataPtr(new MeshData(*built));
    }

    // Frozen evaluation for .abc: substitute the frozen frame for the
    // timeline (the same freeze semantics the scene formats get).
    double effTime = time;
    if (_imp && _imp->freezeAtFrame.lock() && _imp->freezeAtFrame.lock()->getValue()) {
        effTime = (double)(_imp->freezeFrame.lock()
                               ? _imp->freezeFrame.lock()->getValue() : 1);
    }

    if (_lastMeshData && effTime >= 0 && _imp) {
        if (_imp->hasAnimatedXform && _imp->numXformSamples > 0 &&
            !_imp->xformMatrices.empty()) {
            updateTransformAtTime(_lastMeshData.get(), effTime);
        }
        if (_imp->hasAnimatedVerts && _imp->numVertexSamples > 0 &&
            !_imp->vertexSamples.empty()) {
            updateVerticesAtTime(_lastMeshData.get(), effTime);
        }
    }

    // G1 copy-on-read: the same immutable-snapshot contract as the scene
    // path above. The in-place updates stay (they own the mutation under
    // _meshMutex); consumers receive a copy of the result, never the shared
    // object itself.
    if (!_lastMeshData) return _lastMeshData;
    return MeshDataPtr(new MeshData(*_lastMeshData));
}

void
ReadGeo::updateTransformAtTime(MeshData* mesh, double time) const
{
    if (!mesh || !_imp->hasAnimatedXform || _imp->numXformSamples == 0) {
        return;
    }

    // Continuous sample index (frame 1 = sample 0). Bracket with floor/ceil
    // and linearly interpolate matrix components â€” without this, motion blur
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
        // Sample missing or mismatched topology â€” skip (mesh keeps its current
        // verts, no flicker).
        return;
    }
    bumpMeshVersion(*mesh);   // vertices rewritten in place below: GPU caches must re-upload

    if (idx0 == idx1 || alpha < 1e-9) {
        std::memcpy(mesh->vertices.data(), s0.data(), s0.size() * sizeof(float));
        return;
    }

    const std::vector<float>& s1 = _imp->vertexSamples[idx1];
    if (s1.size() != s0.size()) {
        // Topology mismatch between brackets â€” fall back to lower sample.
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
    // Declare this node as frame-varying when the loaded file carries
    // animated content: scene formats via the archive runtime's animation
    // flags (node channels / skins / FBX animation stack), .abc via the
    // transform/vertex sample counts. Required so downstream Scene3D +
    // ScanlineRender hashes change per frame, otherwise the image cache
    // freezes the render output on a single frame even though the geometry
    // actually animates. The 3D viewport bypasses this cache so it always
    // shows live motion; CyclesRender sets frame-varying on its own node
    // which is why Cycles worked while ScanlineRender did not.
    //
    // A frozen file is static by definition â€” never frame-varying, so the
    // cache does its job (one render reused across all frames).
    if (!_imp) {
        return eStatusOK;
    }
    const bool frozen = _imp->freezeAtFrame.lock() &&
                        _imp->freezeAtFrame.lock()->getValue();
    if (frozen) {
        return eStatusOK;
    }
    if (_imp->sceneLoaded) {
        if (_imp->rt.hasAnimation) {
            metadata.setIsFrameVarying(true);
        }
        // Animated user morph weights (Use File Morph Weights off) also make
        // the node time-varying â€” the file-animation checks above don't see
        // knob animation.
        KnobBoolPtr uf = _imp->useFileMorphWeights.lock();
        if (uf && !uf->getValue()) {
            for (std::size_t i = 0; i < _imp->morphWeightKnobs.size(); ++i) {
                KnobDoublePtr k = _imp->morphWeightKnobs[i].lock();
                if (k && k->isAnimated(0, ViewSpec(0))) {
                    metadata.setIsFrameVarying(true);
                    break;
                }
            }
        }
        return eStatusOK;
    }
    if (_imp->hasAnimatedXform || _imp->hasAnimatedVerts) {
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

bool
ReadGeo::getSubdivisionParams(double time, SubdivisionParams* out) const
{
    return readSubdivisionKnobs(_imp->subdKnobs, time, out);
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ReadGeo.cpp"
