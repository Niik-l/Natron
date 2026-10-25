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
#include "../../CreateNodeArgs.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../Project.h"
#include "../../ViewIdx.h"

#ifdef NATRON_HAVE_ALEMBIC
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreFactory/All.h>
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

/**
 * @brief Copy a mesh schema's UV param (PolyMesh/SubD) into out->uvs when
 * present (per u,v pair). Shares the raw-float policy of readMeshArrays.
 */
static void
readAlembicUvParam(const Alembic::AbcGeom::IV2fGeomParam& uvParam, MeshData* mesh)
{
    Alembic::AbcGeom::IV2fGeomParam::Sample uvSample;
    uvParam.getExpanded(uvSample);
    const Alembic::Abc::V2fArraySamplePtr& uvVals = uvSample.getVals();
    if (uvVals && uvVals->size() > 0) {
        size_t numUVs = uvVals->size();
        const float* rawUV = reinterpret_cast<const float*>(uvVals->get());
        mesh->uvs.resize(numUVs * 2);
        std::memcpy(mesh->uvs.data(), rawUV, numUVs * 2 * sizeof(float));
        mesh->hasUVs = true;
    }
}
#endif // NATRON_HAVE_ALEMBIC

// --- Object-hierarchy (multi-emit) helpers -----------------------------------
// Ported from ReadGeoArchive.cpp; the per-entry material ADAPTER type is
// deliberately distinct from ReadGeoArchive's EntryMaterialProvider (each
// node's TU owns its own adapter; consumers only ever hold MaterialProvider*).

namespace {

// One entry's FILE material snapshot exposed as a MaterialProvider (identical
// field mapping to ReadGeoArchive's EntryMaterialProvider).
struct RGEntryMaterialAdapter : public MaterialProvider
{
    std::shared_ptr<const GeoImportMaterial> mat;

    explicit RGEntryMaterialAdapter(const GeoImportMaterial& m)
        : mat(std::make_shared<const GeoImportMaterial>(m)) {}

    void getMaterialBaseColor(double, double& r, double& g, double& b) const OVERRIDE
    { r = mat->baseColor[0]; g = mat->baseColor[1]; b = mat->baseColor[2]; }
    double getMaterialRoughness(double) const OVERRIDE { return mat->roughness; }
    double getMaterialMetallic(double) const OVERRIDE { return mat->metallic; }
    double getMaterialSpecular(double) const OVERRIDE { return mat->specular; }
    void getMaterialEmission(double, double& r, double& g, double& b, double& strength) const OVERRIDE
    { r = mat->emission[0]; g = mat->emission[1]; b = mat->emission[2]; strength = mat->emissionStrength; }
    double getMaterialTransmission(double) const OVERRIDE { return mat->transmission; }
    double getMaterialIOR(double) const OVERRIDE { return mat->ior; }
    std::string getMaterialTextureFile() const OVERRIDE { return mat->diffuseMapPath; }
    std::string getMaterialRoughnessMapFile() const OVERRIDE { return mat->metallicRoughnessMapPath; }
    std::string getMaterialMetallicMapFile() const OVERRIDE
    { return mat->ormMetallicEnabled ? mat->metallicRoughnessMapPath : std::string(); }
    std::string getMaterialNormalMapFile() const OVERRIDE { return mat->normalMapPath; }
    std::string getMaterialOpacityMapFile() const OVERRIDE { return mat->opacityMapPath; }
    double getMaterialOpacity(double) const OVERRIDE { return mat->opacity; }
    bool getMaterialTextureUsesAlpha() const OVERRIDE { return mat->textureUsesAlpha; }
    std::string getMaterialDiffuseColorspace() const OVERRIDE { return "sRGB"; }
};

// Simple glob match: '*' matches any substring (mirror of ReadGeoArchive's
// arcGlobMatch — internal linkage in each TU's anonymous namespace).
bool
rgGlobMatch(const std::string& pattern, const std::string& text)
{
    if (pattern.empty()) return true;
    const int m = (int)pattern.size();
    const int n = (int)text.size();
    std::vector<std::vector<char>> dp(m + 1, std::vector<char>(n + 1, 0));
    dp[0][0] = 1;
    for (int i = 1; i <= m; ++i) {
        if (pattern[i - 1] == '*') dp[i][0] = dp[i - 1][0];
    }
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            if (pattern[i - 1] == '*') {
                dp[i][j] = (char)(dp[i - 1][j] || dp[i][j - 1]);
            } else {
                dp[i][j] = (char)(pattern[i - 1] == text[j - 1] && dp[i - 1][j - 1]);
            }
        }
    }
    return dp[m][n] != 0;
}

// Copy a file material's fields onto a fresh Material3D's knobs (ported from
// ReadGeoArchive: the knob script names are identical on both nodes).
// Includes the glTF PBR maps the file carries (metallicRoughness →
// roughness+metallic, normal).
void
setRebuildMaterialKnobs(NodePtr matNode, const GeoImportMaterial& m)
{
    if (KnobIPtr k = matNode->getKnobByName("baseColor")) {
        if (KnobColorPtr c = std::dynamic_pointer_cast<KnobColor>(k)) {
            c->setValue(m.baseColor[0], ViewSpec::all(), 0);
            c->setValue(m.baseColor[1], ViewSpec::all(), 1);
            c->setValue(m.baseColor[2], ViewSpec::all(), 2);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("roughness")) {
        if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k)) {
            d->setValue(m.roughness);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("metallic")) {
        if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k)) {
            d->setValue(m.metallic);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("specular")) {
        if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k)) {
            d->setValue(m.specular);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("emissionColor")) {
        if (KnobColorPtr c = std::dynamic_pointer_cast<KnobColor>(k)) {
            c->setValue(m.emission[0], ViewSpec::all(), 0);
            c->setValue(m.emission[1], ViewSpec::all(), 1);
            c->setValue(m.emission[2], ViewSpec::all(), 2);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("emissionStrength")) {
        if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k)) {
            d->setValue(m.emissionStrength);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("transmission")) {
        if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k)) {
            d->setValue(m.transmission);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("ior")) {
        if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k)) {
            d->setValue(m.ior);
        }
    }
    if (KnobIPtr k = matNode->getKnobByName("textureFile")) {
        if (KnobFilePtr f = std::dynamic_pointer_cast<KnobFile>(k)) {
            f->setValue(m.diffuseMapPath);
        }
    }
    auto setFileKnob = [&matNode](const char* name, const std::string& path) {
        if (path.empty() || !matNode) {
            return;
        }
        if (KnobIPtr k = matNode->getKnobByName(name)) {
            if (KnobFilePtr f = std::dynamic_pointer_cast<KnobFile>(k)) {
                f->setValue(path);
            }
        }
    };
    setFileKnob("roughnessMapFile", m.metallicRoughnessMapPath);
    setFileKnob("metallicMapFile",
                m.ormMetallicEnabled ? m.metallicRoughnessMapPath : std::string());
    setFileKnob("normalMapFile", m.normalMapPath);
    setFileKnob("opacityMapFile", m.opacityMapPath);
    // glTF BLEND/MASK materials: tick the albedo's own alpha so the rebuilt
    // Material3D keeps the cutout 1:1.
    if (m.textureUsesAlpha && matNode) {
        if (KnobIPtr k = matNode->getKnobByName("textureUsesAlpha")) {
            if (KnobBoolPtr b = std::dynamic_pointer_cast<KnobBool>(k)) {
                b->setValue(true);
            }
        }
    }
    // Texture colorspaces: the entry adapters hardcode "sRGB" for both (a
    // fresh Material3D's defaults are the first sRGB-ish entry of the OCIO
    // config, which Cycles decodes differently) — select the exact "sRGB"
    // entry when it exists.
    auto setSpace = [](const KnobIPtr& k) -> bool {
        KnobChoicePtr c = std::dynamic_pointer_cast<KnobChoice>(k);
        if (!c) {
            return false;
        }
        const std::vector<ChoiceOption> entries = c->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == "sRGB") {
                c->setValue((int)i);
                return true;
            }
        }
        return false;
    };
    if (KnobIPtr k = matNode->getKnobByName("diffuseColorspace")) {
        setSpace(k);
    }
    if (KnobIPtr k = matNode->getKnobByName("emissionColorspace")) {
        setSpace(k);
    }
}

} // anonymous namespace

#ifdef NATRON_HAVE_ALEMBIC
// One cached entry per IXform or IPolyMesh in the abc archive (the
// ReadAlembicArchive::ArchiveEntry model). A struct at namespace scope (not
// anonymous) so the header's private members can store containers of it.
struct RgAbcEntry
{
    std::string fullPath;            // "/Camera01Trackers/Tracker1"
    std::string name;                // leaf name, "Tracker1"
    int parentIndex = -1;            // index into the entries vector; -1 if archive-root
    bool isMesh = false;             // true => IPolyMesh, false => IXform (locator)

    // Sample data for xforms. Empty for meshes.
    std::vector<double> sampleTimes;
    std::vector<Imath::M44d> sampleMatrices;

    // Mesh geometry (rest/sample-0 payload); mesh->vertices holds the
    // CURRENT-frame positions mutated in place by rgAbcMeshAt.
    MeshDataPtr meshData;

    bool hasAnimatedVerts = false;
    std::vector<double> vertexSampleTimes;
    std::vector<std::vector<float>> vertexSamples;
};

// Per-frame parse result for an abc one-file-per-frame sequence (multi-emit):
// a standalone entry list (each frame file is complete) + its sampled fps.
struct RgAbcFrame
{
    std::vector<RgAbcEntry> entries;
    double sourceFps = 0.0;
};
#endif // NATRON_HAVE_ALEMBIC


struct ReadGeoPrivate
{
    SubdivisionKnobs subdKnobs;   // Subdivision (Cycles) group

    KnobFileWPtr filePath;
    KnobChoiceWPtr objectPath;
    KnobButtonWPtr reloadBtn;
    KnobBoolWPtr reverseNormals;
    KnobStringWPtr info;

    // --- Object-hierarchy mode ("Output Single Mesh" OFF) ---
    // The mode switch itself. ON (default) = the classic single-mesh ReadGeo;
    // OFF = multi-emit hierarchy through the ArchiveProvider accessors (the
    // ReadGeoArchive behavior). Both modes share File-page machinery below.
    KnobBoolWPtr outputSingleMesh;
    // Multi-only knobs (ported from ReadGeoArchive::initializeKnobs):
    KnobButtonWPtr rebuildMaterialsBtn;
    KnobStringWPtr objectFilter;
    KnobBoolWPtr includeMeshes;
    KnobBoolWPtr showBBox;
    KnobIntWPtr frameOffset;
    KnobDoubleWPtr fpsKnob;
    KnobStringWPtr excludedPaths;
    KnobChoiceWPtr rotationPivot;
    // Display page (multi-only; Viewport3D::drawTransformNode reads by name):
    KnobBoolWPtr showLocators;
    KnobDoubleWPtr locatorSize;
    // Per-frame parsed sequence runtimes (sequence mode). Guarded by
    // _meshMutex like rt.
    GeoSequenceLruCache<GeoArchiveRuntime> seqRt{72};
    // Entry indices exposed to SceneGraph (full-node indices into
    // rt.data.nodes), and the per-entry material adapters keyed by FULL node
    // index. Guarded by _meshMutex (loaded under it; read on render threads).
    std::vector<int> visible;
    mutable std::map<int, std::shared_ptr<RGEntryMaterialAdapter>> entryMaterials;

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

    // --- File sequence (one file per frame) ---
    // Driven by the file dialog's Sequence:/File: combo (Read-node practice):
    // Sequence: stores a '#' pattern (".../liquid_#####.abc") and each
    // timeline frame evaluates the file numbered like it; File: stores a
    // literal path and the file loads alone (static). Max Cached Frames
    // bounds the per-frame mesh LRU.
    KnobIntPtr cacheFramesKnob;
    // Sequence state (valid when isSequence). Frames keep the FILES' numbers
    // on the timeline (frame N -> the file numbered N; held at ends/gaps).
    GeoSequenceInfo seq;
    // Built meshes of the visited sequence frames, LRU-bounded. Consulted and
    // filled under _meshMutex (loads are GUI-thread, getMeshData serialises).
    mutable GeoSequenceLruCache<MeshData> frameCache{72};

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

    // Re-entrancy guard for the Re-build Materials button (a second trigger
    // while the batch is on the stack must not double-build).
    bool rebuildBusy = false;

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

    // --- Alembic multi-emit (Output Single Mesh OFF, .abc file) ---
    // The rgAbc* engine state (see the RgAbcEntry model at parseObj's side):
    // parsed entry list + the visible filter. Guarded by _meshMutex like
    // rt / visible above.
    std::vector<RgAbcEntry> abcEntries;
    std::vector<int> abcVisible;
    double abcSourceFps = 0.0;
    bool abcLoaded = false;
    // Per-frame parsed engines for an abc one-file-per-frame sequence in
    // multi-emit mode (the seqRt rule: standalone frame files; LRU-bounded).
    mutable GeoSequenceLruCache<RgAbcFrame> abcSeqRt{72};
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
       "File sequences: pick the series with the file picker's Sequence: mode "
       "(like the image Reader) — a one-file-per-frame series "
       "(liquid_#####.abc) plays on the timeline: "
       "the file numbered like each frame provides its content and topology may "
       "change per frame (Realflow mesh caches). Source frame numbers map "
       "natively (frame N = file N); Max Cached Frames bounds the mesh cache."
        "OBJ: positions, texture coords, and n-gon faces are honored. Group/"
        "object directives populate the Object dropdown when present; "
        "otherwise the file loads as a single mesh.\n\n"
        "Output Single Mesh (Object page): ON (default) — the classic single "
        "merged mesh above. OFF — object-hierarchy mode: every mesh part "
        "emits as its own 3D object (the glTF/FBX/OBJ tree, filterable and "
        "[parentable]), and the Rebuild Materials button copies the file's "
        "materials into standalone Material3D nodes. glTF/FBX/OBJ support "
        "both modes; Alembic currently keeps the single-mesh mode only."
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
        fp->setAsInputImage(); // file dialog: the Sequence:/File: combo groups one-file-per-frame geo caches
        fp->setHintToolTip(tr("Path to a .glb/.gltf (glTF), .fbx, .abc (Alembic) or .obj (Wavefront) geometry file.\n\n"
                              "In the file picker, Sequence: mode gathers a one-file-per-frame "
                              "series (e.g. liquid_00002.abc, liquid_00003.abc, ...) — the node "
                              "then plays it on the timeline: frame N evaluates the file numbered "
                              "N, frames outside the range hold at the sequence ends, gaps hold "
                              "the nearest earlier frame. Every frame is parsed independently, "
                              "so topology may change per frame (Realflow mesh caches and "
                              "friends). File: mode loads the picked file alone (static). "
                              "Frame-1-based re-timing or FPS mapping does not apply: source "
                              "frame numbers are native to the timeline."));
        fp->setAnimationEnabled(false);
        page->addKnob(fp);
        _imp->filePath = fp;
    }

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Output Single Mesh"));
        k->setName("outputSingleMesh");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("ON (default): this node is the classic single-mesh import — one "
                             "merged or selected mesh on its output.\n\n"
                             "OFF: the file's full node hierarchy is exposed to the SceneGraph, "
                             "one entry per node/mesh with parent-child relationships preserved "
                             "(the archive/emitter behavior). glTF (.gltf/.glb), FBX, and OBJ "
                             "files support the hierarchy mode (OBJ: one entry per group, "
                             "flat); .abc files keep the single-mesh pipeline for now. Turning "
                             "the knob off reveals the archive "
                             "controls and hides the single-mesh ones (Object dropdown, Freeze "
                             "at Frame, Material/Texture pages)."));
        page->addKnob(k);
        _imp->outputSingleMesh = k;
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
        reload->setHintToolTip(tr("Re-read the file and refresh the hierarchy."));
        page->addKnob(reload);
        _imp->reloadBtn = reload;
    }

    // --- Object-hierarchy (multi-emit) controls — hidden while Output Single
    // Mesh is on (see applyModeVisibility) ---
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Re-build Materials"));
        k->setName("rebuildMaterials");
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Analyze the imported object's structure and build a standalone "
                             "material composition in the node editor beside this node: "
                             "per material-bearing part, one ReadGeo scoped to that part via "
                             "its Object dropdown and a Material3D wired into the ReadGeo's "
                             "Material input (diffuse map / PBR values carried over from the "
                             "file), all assembled under a Group3D. The rebuilt set "
                             "replicates this node's hierarchy output 100% — nothing "
                             "connects back to it."));
        page->addKnob(k);
        _imp->rebuildMaterialsBtn = k;
    }

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Object Filter"));
        k->setName("objectFilter");
        k->setDefaultValue(std::string());
        k->setHintToolTip(tr("Glob pattern matched against each entry's full path. "
                             "Use '*' for any substring. Empty = import everything."));
        page->addKnob(k);
        _imp->objectFilter = k;
    }

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Include Meshes"));
        k->setName("includeMeshes");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("If unchecked, mesh entries are skipped — useful for "
                             "locator/animation-only imports."));
        page->addKnob(k);
        _imp->includeMeshes = k;
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
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Cached Frames"));
        k->setName("cacheFrames");
        k->setDefaultValue(72);
        k->setMinimum(1);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("How many sequence frames to keep built in memory (least-recently-"
                             "used eviction). Sequence meshes can be large, so the cap bounds "
                             "memory over long sequences; a scrub back re-parses evicted "
                             "frames. Only used while a file sequence is loaded."));
        page->addKnob(k);
        _imp->cacheFramesKnob = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Bake Transforms"));
        k->setName("bakeTransforms");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Bake the part's file-authored transform into the vertices at "
                             "the evaluated frame. Only applies when a single part is picked "
                             "in the Object dropdown â€” merged mode always bakes. When off, "
                             "the part keeps its node-local vertices and the file's transform "
                             "animates through the node's transform slot (the user "
                             "Translate/Rotate/Scale composes on top of it). Default is on: "
                             "the ride path with a file's parent transform renders "
                             "misoriented in the viewport and empty in Cycles for the "
                             "standalone-part case the Re-build Materials set produces; "
                             "the baked path reproduces the archive look exactly."));
        page->addKnob(k);
        _imp->bakeTransforms = k;
    }

    // --- Object-hierarchy (multi-emit) controls, continued ---
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Show Bounding Box"));
        k->setName("showBBox");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("Draw a dashed box in the 3D viewport enclosing all "
                             "mesh entries (their union at the current frame). "
                             "Uncheck to hide it."));
        page->addKnob(k);
        _imp->showBBox = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Offset"));
        k->setName("frameOffset");
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Offset the file's animation relative to the Natron timeline. "
                             "Object-hierarchy mode only."));
        page->addKnob(k);
        _imp->frameOffset = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("FPS"));
        k->setName("fps");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(120.0);
        k->setHintToolTip(tr("Frames per second used to map the file's animation "
                             "time onto the Natron timeline. 0 = use the project "
                             "frame rate. glTF/FBX animation time is continuous "
                             "seconds; this sets how many timeline frames span one "
                             "second of source animation. Object-hierarchy mode only."));
        page->addKnob(k);
        _imp->fpsKnob = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Excluded Paths"));
        k->setName("excludedPaths");
        k->setAsMultiLine();
        k->setDefaultValue(std::string());
        k->setSecret(true);
        k->setHintToolTip(tr("Newline-separated entry paths to exclude from import. "
                             "Empty = nothing excluded."));
        page->addKnob(k);
        _imp->excludedPaths = k;
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
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Rotation Pivot"));
        k->setName("rotationPivot");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("Bounding-Box Center", "", ""));
        opts.push_back(ChoiceOption("Authored Origin", "", ""));
        opts.push_back(ChoiceOption("World Origin", "", ""));
        k->populateChoices(opts);
        k->setDefaultValue(0);  // Bounding-Box Center
        k->setHintToolTip(tr("Point the Translate/Rotate/Scale knobs pivot around. "
                             "Object-hierarchy mode only."));
        xformPage->addKnob(k);
        _imp->rotationPivot = k;
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

    // --- Display page: viewport-only controls for the archive's locator
    // gizmos (multi-emit mode; Viewport3D::drawTransformNode reads by name).
    // Ported from ReadGeoArchive::initializeKnobs.
    KnobPagePtr dispPage = AppManager::createKnob<KnobPage>(this, tr("Display"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Show Locators"));
        k->setName("showLocators");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("Draw a small locator at each transform node in the 3D "
                             "viewport. Turn off to declutter archives with many "
                             "transform nodes."));
        dispPage->addKnob(k);
        _imp->showLocators = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Locator Size"));
        k->setName("locatorSize");
        k->setDefaultValue(0.8);
        k->setMinimum(0.0);
        k->setDisplayMinimum(0.0);
        k->setDisplayMaximum(5.0);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("World-space size of the locator gizmos in the 3D viewport."));
        dispPage->addKnob(k);
        _imp->locatorSize = k;
    }

    // Initial per-mode knob visibility follows the default knob value
    // (single mesh ON).
    applyModeVisibility();
}

bool
ReadGeo::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    // Re-entrancy guard: loadAlembicGeo modifies knobs which would trigger knobChanged again
    if (_imp->isLoading) {
        return false;
    }

    // --- Output Single Mesh: the universal node's mode switch -------------
    // Flip the knob visibility, drop BOTH modes' caches (a doorway from one
    // mode must never serve the other's data) and re-load the current file
    // under the new semantics.
    if (_imp->outputSingleMesh.lock().get() == k) {
        applyModeVisibility();
        {
            std::lock_guard<std::mutex> lk(_meshMutex);
            _lastMeshData.reset();
            _imp->sceneLoaded = false;
            _imp->builtCache.clear();
            _imp->frameCache.clear();
            _imp->seq = GeoSequenceInfo();
            _imp->visible.clear();
            _imp->entryMaterials.clear();
            _imp->seqRt.clear();
            _imp->rt.clear();
            _imp->abcEntries.clear();
            _imp->abcVisible.clear();
            _imp->abcSeqRt.clear();
            _imp->abcLoaded = false;
        }
        KnobFilePtr fp = _imp->filePath.lock();
        loadGeoFromFile(fp ? fp->getValue() : std::string());
        return true;
    }

    // --- Object-hierarchy (multi-emit) knobs -------------------------------
    // Filter knobs re-run the archive loader (cheap relative to a file read;
    // keeps applied state == knob state — ReadGeoArchive's isFilter pattern).
    const bool isMultiFilter =
        _imp->objectFilter.lock().get() == k ||
        _imp->includeMeshes.lock().get() == k ||
        _imp->excludedPaths.lock().get() == k;
    if (isMultiFilter) {
        KnobFilePtr fp = _imp->filePath.lock();
        loadGeoFromFile(fp ? fp->getValue() : std::string());
        return true;
    }
    // Re-build Materials (multi-emit only).
    if (_imp->rebuildMaterialsBtn.lock().get() == k) {
        if (isMultiEmit()) {
            rebuildMaterialsSet();
        }
        return true;
    }
    // Frame Offset / FPS: re-map the animation timing — the frame cache keys
    // shift meaning, so drop the caches (no re-parse needed).
    if (_imp->frameOffset.lock().get() == k || _imp->fpsKnob.lock().get() == k) {
        {
            std::lock_guard<std::mutex> lk(_meshMutex);
            _imp->builtCache.clear();
            _imp->frameCache.clear();
            _imp->rt.clearFrameCache();
            _imp->seqRt.clear();
        }
        return true;
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

    // Max Cached Frames: re-caps the sequence frame LRUs (the single-mode
    // mesh cache and the multi-mode per-frame runtime cache).
    if (_imp->cacheFramesKnob.get() == k) {
        _imp->frameCache.setCapacity(_imp->cacheFramesKnob->getValue());
        _imp->seqRt.setCapacity(_imp->cacheFramesKnob->getValue());
        return true;
    }

    // Bake Transforms: only re-keys the single-part build (no timing change),
    // so drop the built meshes but leave the metadata alone. In multi-emit
    // mode the flag IS the firing mechanism (ReadGeoArchive semantics):
    // flip the runtime flag and drop its frame evaluations + the per-frame
    // pre-baked runtimes (they bake at parse time).
    if (_imp->bakeTransforms.lock().get() == k) {
        {
            std::lock_guard<std::mutex> lk(_meshMutex);
            _imp->builtCache.clear();
            _imp->frameCache.clear();
            if (isMultiEmit()) {
                KnobBoolPtr bk = _imp->bakeTransforms.lock();
                _imp->rt.bakeMeshWorld = bk ? bk->getValue() : false;
                _imp->rt.clearFrameCache();
                _imp->seqRt.clear();
            }
        }
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

    // Drop the previous geometry FIRST — clearing the path or a failed load
    // must not keep serving the old mesh (stale-render class).
    _lastMeshData.reset();

    // Any non-scene load below clears the scene-format state; a scene load
    // re-populates it (loadSceneGeo). Clearing also covers the built-mesh
    // cache (keyed by frame/selection/bake — meaningless across files) and
    // the sequence frame cache + detection state. The multi-emit (archive)
    // state is cleared the same way so a mode or extension switch never
    // serves stale entries.
    _imp->sceneLoaded = false;
    _imp->builtCache.clear();
    _imp->seq = GeoSequenceInfo();
    _imp->frameCache.clear();
    _imp->visible.clear();
    _imp->entryMaterials.clear();
    _imp->seqRt.clear();
    _imp->rt.clear();
    _imp->abcEntries.clear();
    _imp->abcVisible.clear();
    _imp->abcSeqRt.clear();
    _imp->abcLoaded = false;

    if (path.empty()) {
        // No file — no morph targets either; drop any weight knobs from a
        // previously loaded file.
        std::vector<GeoMorphTargetRef> none;
        if (syncMorphWeightKnobs(none)) {
            recreateKnobs(true);
        }
        if (KnobStringPtr infoK = _imp->info.lock()) {
            infoK->setValue(std::string("No file selected."));
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

    // Detect the sequence from the stored value BEFORE loading. With the
    // Read-node dialog practice the value is either a '#' pattern (the
    // dialog's Sequence: mode) or a literal path (File: mode); detection
    // falls back to static single-file semantics when the pattern matches
    // nothing.
    std::string loadPath = path;
    _imp->seq = detectGeoSequence(path);
    if (_imp->seq.isSequence) {
        // The stored value is a '#' pattern — the per-frame files are the
        // sequence's frames: load the first frame file as the REFERENCE (its
        // dropdown, materials and rest metadata all come from it), while
        // getMeshData evaluates the sequence's per-frame files.
        std::map<int, std::string>::const_iterator f0 = _imp->seq.files.begin();
        if (f0 != _imp->seq.files.end()) {
            loadPath = f0->second;
        } else {
            _imp->seq = GeoSequenceInfo();
        }
    }

    const std::string e = extOf(loadPath);
    const bool multiEmit = isMultiEmit();
    if (multiEmit) {
        // Object-hierarchy mode: the formats the GeoArchiveRuntime parse
        // covers (phase 1) plus OBJ groups (phase 2). Alembic hierarchy
        // support lands in phase 3 — keep the message explicit.
        if (e == ".gltf" || e == ".glb" || e == ".fbx") {
            loadArchiveGeo(path);
        } else if (e == ".obj") {
            loadObjArchiveGeo(path);
        } else if (e == ".abc") {
            loadAbcArchiveGeo(path);
        }
    } else if (e == ".obj") {
        loadObjGeo(loadPath);
    } else if (e == ".abc") {
#ifdef NATRON_HAVE_ALEMBIC
        loadAlembicGeo(loadPath);
#else
        setPersistentMessage(eMessageTypeError,
            "Alembic support not built into this Natron binary. "
            "Use a .obj file, or rebuild Natron with Alembic.");
#endif
    } else if (e == ".gltf" || e == ".glb" || e == ".fbx") {
        loadSceneGeo(loadPath);
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

    // Sequence loaded: annotate the info line the loader just wrote. The
    // archive (multi-emit) loader composes its own info line, including seq.
    if (!multiEmit && _imp->seq.isSequence) {
        KnobStringPtr infoK = _imp->info.lock();
        if (infoK) {
            std::ostringstream seqInfo;
            seqInfo << infoK->getValue()
                    << " | seq frames " << _imp->seq.firstFrame << "-" << _imp->seq.lastFrame
                    << " (" << _imp->seq.files.size() << " files)";
            infoK->setValue(seqInfo.str());
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
    // A project saved with the knob toggled does not fire knobChanged on the
    // restore — re-derive the per-mode knob visibility from the restored
    // value before the load path runs.
    applyModeVisibility();
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

// ---- zero-allocation text scanning helpers --------------------------------
// The old core built a std::istringstream per line and a std::string (+up to
// three substr) per face-vertex token — on RealMesh/RealFlow exports that is
// ~2M allocations for a 127k-vertex frame and made OBJ sequence parsing
// ~5x slower than the binary FBX/Alembic paths (visible while scrubbing the
// 3D viewport). These helpers parse out of a whole-file buffer instead.

inline bool objIsWs(char c)
{
    return c == ' ' || c == '\t';
}

// Token-bounded float: copy into a stack buffer and use the C scanner.
// strtof is locale-bound exactly like the previous istringstream extraction
// was (the app forces LC_NUMERIC=C at startup), so no new dependency.
inline bool
objScanFloatTok(const char* b, const char* e, float* out)
{
    const size_t n = (size_t)(e - b);
    char tmp[64];
    if (n == 0 || n >= sizeof(tmp)) {
        return false;
    }
    std::memcpy(tmp, b, n);
    tmp[n] = '\0';
    char* pe = nullptr;
    const float v = std::strtof(tmp, &pe);
    if (pe == tmp) {
        return false;
    }
    *out = v;
    return true;
}

inline bool
objScanIntTok(const char* b, const char* e, int* out)
{
    const size_t n = (size_t)(e - b);
    char tmp[24];
    if (n == 0 || n >= sizeof(tmp)) {
        return false;
    }
    std::memcpy(tmp, b, n);
    tmp[n] = '\0';
    char* pe = nullptr;
    const long v = std::strtol(tmp, &pe, 10);
    if (pe == tmp) {
        return false;
    }
    *out = (int)v;
    return true;
}

// Skip whitespace then scan ONE float token; moves *c past the token.
inline bool
objNextFloat(const char** c, const char* le, float* out)
{
    const char* p = *c;
    while (p < le && objIsWs(*p)) ++p;
    const char* t = p;
    while (t < le && !objIsWs(*t)) ++t;
    *c = t;
    return objScanFloatTok(p, t, out);
}

struct ObjGroup
{
    std::string name;
    std::vector<int>   faceIndicesV;   // position index per face-vertex
    std::vector<int>   faceCounts;     // verts per face
    std::vector<float> uvs;            // u,v per face-vertex (0,0 if no vt)
    bool hasUVs = false;
};

// Line-parse an OBJ file into positions / texCoords / groups (the parse core
// of ReadGeo::loadObjGeo). Reads the whole file into one buffer and scans
// zero-copy: per-line memchr, fixed tag dispatch, stack-buffer float/int
// conversion, and no per-token strings in the face loop ("v 1.23"-style
// lines dominate these files). Returns false with *err
// filled when the file cannot be opened or holds no geometry. Used both by
// the single-file load and by the file-sequence frame builder
// (see buildObjSeqFrame).
bool
parseObjGroups(const std::string& path,
               std::vector<float>* positions,
               std::vector<float>* texCoords,
               std::vector<ObjGroup>* groups,
               std::string* err)
{
    // Whole-file read: single allocation beats the getline churn of the
    // previous io-streams implementation.
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) {
        *err = "Cannot open file: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streampos fsize = in.tellg();
    std::string buf;
    if (fsize > 0) {
        buf.resize((size_t)fsize);
        in.seekg(0, std::ios::beg);
        in.read(&buf[0], fsize);
        buf.resize((size_t)in.gcount());
    }

    groups->reserve(4);
    groups->push_back(ObjGroup{}); // implicit default group
    groups->back().name = "default";

    bool firstLine = true;
    const char* p = buf.data();
    const char* end = buf.data() + buf.size();

    while (p < end) {
        const char* nl = (const char*)std::memchr(p, '\n', (size_t)(end - p));
        const char* le = nl ? nl : end;
        if (le > p && le[-1] == '\r') --le;
        // Skip leading whitespace before the tag.
        const char* c = p;
        while (c < le && objIsWs(*c)) ++c;
        p = nl ? nl + 1 : end; // advance the line cursor up front

        // UTF-8 BOM of the first line.
        if (firstLine && le - c >= 3 &&
            (unsigned char)c[0] == 0xEF &&
            (unsigned char)c[1] == 0xBB &&
            (unsigned char)c[2] == 0xBF) {
            c += 3;
        }
        firstLine = false;

        // Tag token.
        const char* te = c;
        while (te < le && !objIsWs(*te)) ++te;
        const size_t tlen = (size_t)(te - c);

        if (tlen == 1 && c[0] == 'v') {
            float x = 0, y = 0, z = 0;
            const char* t = te;
            objNextFloat(&t, le, &x);
            objNextFloat(&t, le, &y);
            objNextFloat(&t, le, &z);
            positions->push_back(x);
            positions->push_back(y);
            positions->push_back(z);
        } else if (tlen == 2 && c[0] == 'v' && c[1] == 't') {
            float u = 0, v = 0;
            const char* t = te;
            objNextFloat(&t, le, &u);
            objNextFloat(&t, le, &v);
            texCoords->push_back(u);
            texCoords->push_back(v);
        } else if (tlen == 2 && c[0] == 'v' && c[1] == 'n') {
            // Normals: ignored (MeshData has no normals field).
        } else if (tlen == 1 && (c[0] == 'g' || c[0] == 'o')) {
            // Start a new group if the current one has already received faces.
            if (!groups->back().faceCounts.empty()) {
                groups->push_back(ObjGroup{});
            }
            const char* nb = te;
            while (nb < le && objIsWs(*nb)) ++nb;
            const char* ne = nb;
            while (ne < le && !objIsWs(*ne)) ++ne;
            const std::string name(nb, ne);
            groups->back().name = name.empty() ? "unnamed" : name;
        } else if (tlen == 1 && c[0] == 'f') {
            ObjGroup& g = groups->back();
            int faceVerts = 0;
            const int posCount = (int)(positions->size() / 3);
            const int vtCount  = (int)(texCoords->size() / 2);
            // Face-vertex tokens: "v", "v/vt", "v//vn", "v/vt/vn" — 1-based,
            // negative indices relative-from-end (same resolution as the old
            // parseObjFaceToken, without the per-token substr).
            const char* t0 = te;
            while (t0 < le) {
                while (t0 < le && objIsWs(*t0)) ++t0;
                if (t0 >= le) break;
                const char* t1 = t0;
                while (t1 < le && !objIsWs(*t1)) ++t1;

                int v = -1, vt = -1;
                const char* s1 = (const char*)std::memchr(t0, '/', (size_t)(t1 - t0));
                if (!s1) {
                    int iv;
                    if (objScanIntTok(t0, t1, &iv)) {
                        v = (iv == 0) ? -1 : (iv > 0
                                    ? ((iv <= posCount) ? (iv - 1) : -1)
                                    : ((-iv <= posCount) ? (posCount + iv) : -1));
                    }
                } else {
                    int iv;
                    if (objScanIntTok(t0, s1, &iv)) {
                        v = (iv == 0) ? -1 : (iv > 0
                                    ? ((iv <= posCount) ? (iv - 1) : -1)
                                    : ((-iv <= posCount) ? (posCount + iv) : -1));
                    }
                    const char* s2 = (const char*)std::memchr(s1 + 1, '/', (size_t)(t1 - (s1 + 1)));
                    if (!s2) {
                        if (t1 > s1 + 1 && objScanIntTok(s1 + 1, t1, &iv)) {
                            vt = (iv == 0) ? -1 : (iv > 0
                                        ? ((iv <= vtCount) ? (iv - 1) : -1)
                                        : ((-iv <= vtCount) ? (vtCount + iv) : -1));
                        }
                    } else {
                        if (s2 > s1 + 1 && objScanIntTok(s1 + 1, s2, &iv)) {
                            vt = (iv == 0) ? -1 : (iv > 0
                                        ? ((iv <= vtCount) ? (iv - 1) : -1)
                                        : ((-iv <= vtCount) ? (vtCount + iv) : -1));
                        }
                    }
                }

                if (v >= 0) {
                    g.faceIndicesV.push_back(v);
                    if (vt >= 0 && vt < vtCount) {
                        g.uvs.push_back((*texCoords)[vt * 2 + 0]);
                        g.uvs.push_back((*texCoords)[vt * 2 + 1]);
                        g.hasUVs = true;
                    } else {
                        g.uvs.push_back(0.0f);
                        g.uvs.push_back(0.0f);
                    }
                    ++faceVerts;
                }
                t0 = t1;
            }
            if (faceVerts >= 3) g.faceCounts.push_back(faceVerts);
        }
        // Other tags (s, mtllib, usemtl, ...) deliberately ignored.
    }

    // Drop empty groups (e.g. the implicit "default" if all faces went into a named group).
    groups->erase(std::remove_if(groups->begin(), groups->end(),
                                 [](const ObjGroup& g){ return g.faceCounts.empty(); }),
                  groups->end());

    if (groups->empty() || positions->empty()) {
        *err = "No geometry found in " + path;
        return false;
    }

    return true;
}

// Build the MeshData payload for one parsed group (the mesh core of
// ReadGeo::loadObjGeo): global vertex positions, the group's faces, per
// face-vertex uvs, the wireframe edge list and an identity transform (the
// node's own T/R/S knobs handle placement downstream).
void
buildObjGroupMeshData(const std::vector<float>& positions,
                      const ObjGroup& chosen,
                      MeshData* mesh)
{
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

    for (int i = 0; i < 16; ++i) mesh->transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

// OBJ multi-emit parser: one root-level mesh node per Wavefront group — the
// flat hierarchy an .obj carries (no transforms anywhere). Each piece gets
// the group's vertices PRUNED out of the file's global pool (the archive
// machinery indexes piece-local vertices: skinning/morph/bake code assumes
// indices into the entry's own payload). UVs are per face-corner, parallel
// to the face list — untouched by the vertex remap. Edges rebuild from the
// remapped indices so the wireframe stays correct.
bool
parseObjArchiveMulti(const std::string& path, GeoArchiveResult* out, std::string* err)
{
    std::vector<float> positions;
    std::vector<float> texCoords;
    std::vector<ObjGroup> groups;
    if (!parseObjGroups(path, &positions, &texCoords, &groups, err)) {
        return false;
    }
    if (groups.empty()) {
        *err = "No geometry found in " + path;
        return false;
    }

    *out = GeoArchiveResult();
    GeoArchiveResult& data = *out;
    data.meshes.resize(groups.size());
    data.nodes.resize(groups.size());

    for (std::size_t k = 0; k < groups.size(); ++k) {
        MeshDataPtr mesh(new MeshData);
        buildObjGroupMeshData(positions, groups[k], mesh.get());

        // Vertex prune: face-index walk marking used positions, then remap
        // the face + edge indices in place. Unused positions drop entirely.
        std::vector<int> remap((size_t)mesh->numVertices, -1);
        std::vector<float> pruned;
        for (size_t fi = 0; fi < mesh->faceIndices.size(); ++fi) {
            const int v = mesh->faceIndices[fi];
            if (v < 0 || v >= (int)remap.size()) continue;
            if (remap[(size_t)v] < 0) {
                remap[(size_t)v] = (int)(pruned.size() / 3);
                pruned.push_back(positions[(size_t)v * 3 + 0]);
                pruned.push_back(positions[(size_t)v * 3 + 1]);
                pruned.push_back(positions[(size_t)v * 3 + 2]);
            }
            mesh->faceIndices[fi] = remap[(size_t)v];
        }
        for (size_t ei = 0; ei < mesh->edgeIndices.size(); ++ei) {
            const int e = mesh->edgeIndices[ei];
            mesh->edgeIndices[ei] =
                (e >= 0 && e < (int)remap.size()) ? remap[(size_t)e] : 0;
        }
        mesh->vertices.swap(pruned);
        mesh->numVertices = (int)(mesh->vertices.size() / 3);

        GeoArchiveMeshData md;
        md.mesh = mesh;
        data.meshes[k] = md;

        GeoArchiveNodeData& nd = data.nodes[k];
        nd.name = groups[k].name;
        nd.fullPath = "/" + groups[k].name;
        nd.parentIndex = -1;   // flat: every group attaches to the archive root
        nd.isMesh = true;
        nd.meshIndex = (int)k;
        geoArchiveMatIdentity(nd.localMatrix); // hasTrs=false → serves this
    }

    data.ok = true;
    return true;
}

} // anon (obj helpers + obj archive parse)

// ================= Alembic multi-emit engine ================================
// (ReadGeo-local copy of the ReadAlembicArchive model — deliberately NOT
// shared code, same ODR rule as the RGEntryMaterialAdapter: the legacy node
// keeps its own implementation and names, so no header coupling and no
// CMake change.)

namespace {

// Read sample 0 of an IPolyMesh into a fresh MeshDataPtr (the
// ReadAlembicArchive::readPolyMeshSample0 logic).
MeshDataPtr
rgAbcReadPolyMeshSample0(const Alembic::AbcGeom::IPolyMesh& polyMesh)
{
    using namespace Alembic::AbcGeom;

    IPolyMeshSchema schema = polyMesh.getSchema();
    if (schema.getNumSamples() == 0) return MeshDataPtr();

    IPolyMeshSchema::Sample sample;
    schema.get(sample, ISampleSelector((Alembic::AbcCoreAbstract::index_t)0));

    Alembic::Abc::P3fArraySamplePtr positions = sample.getPositions();
    Alembic::Abc::Int32ArraySamplePtr faceIndices = sample.getFaceIndices();
    Alembic::Abc::Int32ArraySamplePtr faceCounts = sample.getFaceCounts();
    if (!positions || !faceIndices || !faceCounts) return MeshDataPtr();

    const std::size_t numVerts = positions->size();
    const std::size_t numFaceIdx = faceIndices->size();
    const std::size_t numFaces = faceCounts->size();
    if (numVerts == 0 || numFaceIdx == 0 || numFaces == 0) return MeshDataPtr();

    MeshDataPtr mesh = std::make_shared<MeshData>();

    const float* rawPos = reinterpret_cast<const float*>(positions->get());
    mesh->vertices.resize(numVerts * 3);
    mesh->numVertices = numVerts;
    std::memcpy(mesh->vertices.data(), rawPos, numVerts * 3 * sizeof(float));

    const int32_t* rawIdx = reinterpret_cast<const int32_t*>(faceIndices->get());
    mesh->faceIndices.resize(numFaceIdx);
    for (std::size_t i = 0; i < numFaceIdx; ++i) mesh->faceIndices[i] = (int)rawIdx[i];

    const int32_t* rawCnt = reinterpret_cast<const int32_t*>(faceCounts->get());
    mesh->faceCounts.resize(numFaces);
    mesh->numFaces = numFaces;
    for (std::size_t i = 0; i < numFaces; ++i) mesh->faceCounts[i] = (int)rawCnt[i];

    mesh->edgeIndices.reserve(numFaceIdx * 2);
    std::size_t idxOffset = 0;
    for (std::size_t f = 0; f < numFaces; ++f) {
        const int count = (int)rawCnt[f];
        for (int v = 0; v < count; ++v) {
            mesh->edgeIndices.push_back((int)rawIdx[idxOffset + v]);
            mesh->edgeIndices.push_back((int)rawIdx[idxOffset + ((v + 1) % count)]);
        }
        idxOffset += (std::size_t)count;
    }

    IV2fGeomParam uvParam = schema.getUVsParam();
    if (uvParam.valid()) {
        IV2fGeomParam::Sample uvSample;
        uvParam.getExpanded(uvSample);
        const Alembic::Abc::V2fArraySamplePtr& uvVals = uvSample.getVals();
        if (uvVals && uvVals->size() > 0) {
            const std::size_t numUVs = uvVals->size();
            const float* rawUV = reinterpret_cast<const float*>(uvVals->get());
            mesh->uvs.resize(numUVs * 2);
            std::memcpy(mesh->uvs.data(), rawUV, numUVs * 2 * sizeof(float));
            mesh->hasUVs = true;
        }
    }

    // Local transform stays identity — the parent IXform's transform is
    // handled by the SceneGraph parent chain.
    return mesh;
}

// Continuous sample index for one sampled entry list — the legacy
// time→sample mapping (timeMode 0: native frame indexing with edge clamp;
// timeMode 1 + known source fps: nearest abc-time bracket as a fractional
// index for sub-frame interpolation).
double
rgAbcSampleIndexContinuous(const std::vector<double>& sampleTimes, std::size_t N,
                           double time, int frameOffset, int timeMode,
                           double sourceFps, double projectFps)
{
    if (N == 0) return 0.0;
    double tCont = 0.0;
    if (timeMode == 1 && sourceFps > 0.0 && sampleTimes.size() == N) {
        const double abcTimeWanted = (time - 1.0 - (double)frameOffset) / projectFps;
        int idx0 = 0;
        for (int i = 0; i + 1 < (int)N; ++i) {
            if (sampleTimes[i] <= abcTimeWanted &&
                abcTimeWanted <= sampleTimes[i + 1]) {
                idx0 = i;
                break;
            }
            if (abcTimeWanted < sampleTimes[i]) break;
            idx0 = i;
        }
        const int idx1 = std::min(idx0 + 1, (int)N - 1);
        const double t0 = sampleTimes[idx0];
        const double t1 = sampleTimes[idx1];
        const double span = (t1 - t0);
        const double alpha = (span > 1e-12)
            ? std::max(0.0, std::min(1.0, (abcTimeWanted - t0) / span))
            : 0.0;
        tCont = (double)idx0 + alpha;
    } else {
        tCont = time - 1.0 - (double)frameOffset;
        if (tCont < 0.0) tCont = 0.0;
        if (tCont > (double)(N - 1)) tCont = (double)(N - 1);
    }
    return tCont;
}

// The entry's local matrix at Natron `time` (the legacy getSceneNodeAt
// entryLocalAt logic — nearest round sample in frame mode; interpolated
// bracket in time mode; Imath::M44d → column-major).
void
rgAbcEntryLocalAt(const RgAbcEntry& ej, double time, int frameOffset,
                  int timeMode, double sourceFps, double projectFps, float out[16])
{
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = out[5] = out[10] = out[15] = 1.0f;
    if (ej.sampleMatrices.empty()) return;

    const std::size_t N = ej.sampleMatrices.size();
    double tCont = 0.0;
    if (timeMode == 1 && sourceFps > 0.0 && ej.sampleTimes.size() == N) {
        const double abcTimeWanted = (time - 1.0 - (double)frameOffset) / projectFps;
        int idx0 = 0;
        for (int i = 0; i + 1 < (int)N; ++i) {
            if (ej.sampleTimes[i] <= abcTimeWanted &&
                abcTimeWanted <= ej.sampleTimes[i + 1]) {
                idx0 = i;
                break;
            }
            if (abcTimeWanted < ej.sampleTimes[i]) break;
            idx0 = i;
        }
        const int idx1 = std::min(idx0 + 1, (int)N - 1);
        const double t0 = ej.sampleTimes[idx0];
        const double t1 = ej.sampleTimes[idx1];
        const double span = (t1 - t0);
        const double alpha = (span > 1e-12)
            ? std::max(0.0, std::min(1.0, (abcTimeWanted - t0) / span))
            : 0.0;
        tCont = (double)idx0 + alpha;
    } else {
        tCont = time - 1.0 - (double)frameOffset;
        if (tCont < 0.0) tCont = 0.0;
        if (tCont > (double)(N - 1)) tCont = (double)(N - 1);
    }

    const int idx0 = (int)std::floor(tCont);
    const int idx1 = (int)std::min(idx0 + 1, (int)N - 1);
    const double alpha = tCont - (double)idx0;
    const Imath::M44d& m0 = ej.sampleMatrices[idx0];
    const Imath::M44d& m1 = ej.sampleMatrices[idx1];
    // Imath stores M_col with row-major memory: out[col*4 + row] = m[col][row].
    if (idx0 == idx1 || alpha < 1e-9) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                out[c * 4 + r] = (float)m0[c][r];
    } else {
        const double oneMinusA = 1.0 - alpha;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                out[c * 4 + r] = (float)(m0[c][r] * oneMinusA + m1[c][r] * alpha);
    }
}

// The mesh entry's payload at `time` (the legacy getMeshDataAt logic —
// in-place vertex mutation documented in ReadAlembicArchive; bracket-
// interpolated sub-frame for motion blur).
MeshDataPtr
rgAbcMeshAt(RgAbcEntry& e, double time, int frameOffset,
            int timeMode, double sourceFps, double projectFps)
{
    if (!e.isMesh) return MeshDataPtr();

    if (time >= 0.0 && e.hasAnimatedVerts && !e.vertexSamples.empty() && e.meshData) {
        const int N = (int)e.vertexSamples.size();
        const double tCont = rgAbcSampleIndexContinuous(e.vertexSampleTimes,
                                                        (std::size_t)N,
                                                        time, frameOffset, timeMode,
                                                        sourceFps, projectFps);
        const int idx0 = (int)std::floor(tCont);
        const int idx1 = std::min(idx0 + 1, N - 1);
        const double alpha = tCont - (double)idx0;
        const std::vector<float>& s0 = e.vertexSamples[idx0];
        const std::vector<float>& s1 = e.vertexSamples[idx1];

        if (!s0.empty() && s0.size() == e.meshData->vertices.size()) {
            if (idx0 == idx1 || alpha < 1e-9) {
                std::memcpy(e.meshData->vertices.data(), s0.data(),
                            s0.size() * sizeof(float));
            } else if (s1.size() == s0.size()) {
                const float a = (float)alpha;
                const float oneMinusA = 1.0f - a;
                float* dst = e.meshData->vertices.data();
                const float* src0 = s0.data();
                const float* src1 = s1.data();
                const std::size_t n = s0.size();
                for (std::size_t k = 0; k < n; ++k) {
                    dst[k] = src0[k] * oneMinusA + src1[k] * a;
                }
            } else {
                // Topology mismatch between brackets: fall back to the lower
                // sample.
                std::memcpy(e.meshData->vertices.data(), s0.data(),
                            s0.size() * sizeof(float));
            }
        }
    }

    return e.meshData;
}

// Root-down chain world matrix (the legacy getEntryWorldMatrix logic).
void
rgAbcEntryWorldAt(const std::vector<RgAbcEntry>& entries, int srcIdx, double time,
                  int frameOffset, int timeMode, double sourceFps, double projectFps,
                  float outWorld[16])
{
    for (int i = 0; i < 16; ++i) outWorld[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    if (srcIdx < 0 || srcIdx >= (int)entries.size()) return;

    std::vector<int> chain;
    int cur = srcIdx;
    while (cur >= 0 && cur < (int)entries.size()) {
        chain.push_back(cur);
        cur = entries[cur].parentIndex;
    }

    for (int ci = (int)chain.size() - 1; ci >= 0; --ci) {
        float local[16];
        rgAbcEntryLocalAt(entries[chain[ci]], time, frameOffset, timeMode,
                          sourceFps, projectFps, local);

        // outWorld = outWorld * local (column-major).
        float r[16];
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += outWorld[k * 4 + row] * local[col * 4 + k];
                }
                r[col * 4 + row] = sum;
            }
        }
        std::memcpy(outWorld, r, sizeof(r));
    }
}

// Recursive archive walker (the ReadAlembicArchive::walkObjectRecursive logic).
void
rgAbcWalkObject(const Alembic::AbcGeom::IObject& obj,
                const std::string& parentPath,
                int parentEntryIndex,
                std::vector<RgAbcEntry>& entries)
{
    using namespace Alembic::AbcGeom;

    const std::string fullPath = parentPath.empty()
        ? std::string("/") + obj.getName()
        : parentPath + "/" + obj.getName();

    int thisEntryIndex = -1;

    if (IXform::matches(obj.getHeader())) {
        IXform xf(obj, Alembic::Abc::kWrapExisting);
        IXformSchema& schema = xf.getSchema();
        const std::size_t numSamples = schema.getNumSamples();
        Alembic::AbcCoreAbstract::TimeSamplingPtr ts = schema.getTimeSampling();

        RgAbcEntry e;
        e.fullPath = fullPath;
        e.name = obj.getName();
        e.parentIndex = parentEntryIndex;
        e.isMesh = false;
        e.sampleTimes.reserve(numSamples);
        e.sampleMatrices.reserve(numSamples);
        for (std::size_t i = 0; i < numSamples; ++i) {
            const double t = ts ? ts->getSampleTime((Alembic::AbcCoreAbstract::index_t)i) : 0.0;
            XformSample xs;
            schema.get(xs, ISampleSelector((Alembic::AbcCoreAbstract::index_t)i));
            e.sampleTimes.push_back(t);
            e.sampleMatrices.push_back(xs.getMatrix());
        }
        thisEntryIndex = (int)entries.size();
        entries.push_back(e);
    } else if (IPolyMesh::matches(obj.getHeader())) {
        // Sample 0 as rest pose, then pre-load all remaining vertex samples
        // (topology assumed constant — the standard Maya/Blender/Houdini
        // export). The parent IXform's animation drives positioning via the
        // SceneGraph parent chain; vertex animation handled by the sampler.
        IPolyMesh polyMesh(obj, Alembic::Abc::kWrapExisting);
        RgAbcEntry e;
        e.fullPath = fullPath;
        e.name = obj.getName();
        e.parentIndex = parentEntryIndex;
        e.isMesh = true;
        e.meshData = rgAbcReadPolyMeshSample0(polyMesh);

        if (e.meshData && e.meshData->numVertices > 0) {
            IPolyMeshSchema schema = polyMesh.getSchema();
            const std::size_t numVertSamples = schema.getNumSamples();
            if (numVertSamples > 1) {
                Alembic::AbcCoreAbstract::TimeSamplingPtr ts = schema.getTimeSampling();
                e.hasAnimatedVerts = true;
                e.vertexSamples.assign(numVertSamples, std::vector<float>());
                e.vertexSampleTimes.reserve(numVertSamples);
                const std::size_t expectedFloats = (std::size_t)e.meshData->numVertices * 3;
                for (std::size_t s = 0; s < numVertSamples; ++s) {
                    const double t = ts ? ts->getSampleTime((Alembic::AbcCoreAbstract::index_t)s) : (double)s;
                    e.vertexSampleTimes.push_back(t);
                    Alembic::Abc::ISampleSelector ss((Alembic::AbcCoreAbstract::index_t)s);
                    IPolyMeshSchema::Sample animSample;
                    schema.get(animSample, ss);
                    Alembic::Abc::P3fArraySamplePtr positions = animSample.getPositions();
                    if (!positions || positions->size() != e.meshData->numVertices) continue;
                    const float* posPtr = reinterpret_cast<const float*>(positions->get());
                    e.vertexSamples[s].resize(expectedFloats);
                    std::memcpy(e.vertexSamples[s].data(), posPtr,
                                expectedFloats * sizeof(float));
                }
            }
        }

        thisEntryIndex = (int)entries.size();
        entries.push_back(e);
    }
    // Cameras / lights / other types: skipped — they have their own dedicated readers.

    // Recurse: pass thisEntryIndex when one was created so deeper entries
    // attach to the nearest enclosing xform/mesh.
    const int childParent = (thisEntryIndex >= 0) ? thisEntryIndex : parentEntryIndex;
    for (std::size_t i = 0; i < obj.getNumChildren(); ++i) {
        rgAbcWalkObject(obj.getChild(i), fullPath, childParent, entries);
    }
}

// Parse one abc file into the flat entry list; source fps from the first
// uniformly-sampled entry (the legacy loadAlembicFile logic, minus knobs).
bool
rgAbcParse(const std::string& path, bool reverseNormals,
           std::vector<RgAbcEntry>* entries, double* sourceFps, std::string* err)
{
    using Alembic::AbcCoreFactory::IFactory;

    entries->clear();
    *sourceFps = 0.0;

    try {
        IFactory factory;
        Alembic::Abc::IArchive archive = factory.getArchive(path);
        if (!archive.valid()) {
            *err = std::string("Failed to open Alembic file: ") + path;
            return false;
        }

        Alembic::Abc::IObject top = archive.getTop();
        for (std::size_t i = 0; i < top.getNumChildren(); ++i) {
            rgAbcWalkObject(top.getChild(i), std::string(), -1, *entries);
        }

        if (entries->empty()) {
            *err = "No IXform/IPolyMesh entries in " + path;
            return false;
        }

        // Reverse Normals: flip every mesh entry's winding (applied at load
        // time so a knob toggle just re-runs the load).
        if (reverseNormals) {
            for (RgAbcEntry& e : *entries) {
                if (e.isMesh && e.meshData) {
                    reverseMeshWinding(*e.meshData);
                }
            }
        }

        for (std::size_t i = 0; i < entries->size(); ++i) {
            if ((*entries)[i].sampleTimes.size() >= 2) {
                const double dt = (*entries)[i].sampleTimes[1] - (*entries)[i].sampleTimes[0];
                if (dt > 0.0) {
                    *sourceFps = 1.0 / dt;
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        *err = std::string("Error reading Alembic file: ") + e.what();
        return false;
    }
    
    return true;
}

} // anon

void
ReadGeo::loadObjGeo(const std::string& path)
{
    std::vector<float> positions;
    std::vector<float> texCoords;
    std::vector<ObjGroup> groups;
    std::string err;
    if (!parseObjGroups(path, &positions, &texCoords, &groups, &err)) {
        setPersistentMessage(eMessageTypeError, err);
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
    buildObjGroupMeshData(positions, chosen, mesh.get());

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
// File-sequence frame builders (one file per frame, '#' pattern)
// ---------------------------------------------------------------------------

bool
ReadGeo::buildObjSeqFrame(int frame, MeshData* out, std::string* err) const
{
    if (!out || !err) {
        return false;
    }
    std::map<int, std::string>::const_iterator pit = _imp->seq.files.find(frame);
    if (pit == _imp->seq.files.end()) {
        *err = "no file for frame " + std::to_string(frame);
        return false;
    }

    std::vector<float> positions;
    std::vector<float> texCoords;
    std::vector<ObjGroup> groups;
    if (!parseObjGroups(pit->second, &positions, &texCoords, &groups, err)) {
        return false;
    }

    // The frame file shares the reference's group layout: re-resolve the
    // dropdown's group by NAME and fall back to the first group on a
    // mismatch (renames between frames stay readable).
    KnobChoicePtr objKnob = _imp->objectPath.lock();
    int chosenIdx = objKnob ? objKnob->getValue() : 0;
    const std::string want = (chosenIdx >= 0 && chosenIdx < (int)_imp->geoPaths.size())
        ? _imp->geoPaths[chosenIdx] : std::string();
    int gi = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].name == want) {
            gi = (int)i;
            break;
        }
    }

    buildObjGroupMeshData(positions, groups[gi], out);
    if (_imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue()) {
        reverseMeshWinding(*out);
    }
    return true;
}

bool
ReadGeo::buildAbcSeqFrame(int frame, MeshData* out, std::string* err) const
{
#ifdef NATRON_HAVE_ALEMBIC
    if (!out || !err) {
        return false;
    }
    std::map<int, std::string>::const_iterator pit = _imp->seq.files.find(frame);
    if (pit == _imp->seq.files.end()) {
        *err = "no file for frame " + std::to_string(frame);
        return false;
    }
    const std::string& path = pit->second;

    try {
        FILE* testF = fopen(path.c_str(), "rb");
        if (!testF) {
            *err = "Cannot open file: " + path;
            return false;
        }
        fclose(testF);

        Alembic::AbcCoreOgawa::ReadArchive reader;
        Alembic::AbcGeom::IArchive archive(reader, path);
        Alembic::AbcGeom::IObject top = archive.getTop();

        // Same object layout as the reference file: re-resolve the dropdown's
        // selection path and fall back to the first geo entry when missing.
        KnobChoicePtr objKnob = _imp->objectPath.lock();
        int selectedIdx = objKnob ? objKnob->getValue() : 0;
        if (selectedIdx < 0 || selectedIdx >= (int)_imp->geoPaths.size()) {
            selectedIdx = 0;
        }
        std::string selectedPath = _imp->geoPaths.empty() ? std::string()
                                                          : _imp->geoPaths[selectedIdx];
        // Fallback: find any poly/subd object in the frame file.
        if (selectedPath.empty()) {
            std::vector<std::string> found;
            for (size_t i = 0; i < top.getNumChildren(); ++i) {
                findGeoRecursive(top.getChild(i), std::string(), found);
            }
            if (found.empty()) {
                *err = "No geometry found in " + path;
                return false;
            }
            selectedPath = found.front();
        }

        Alembic::AbcGeom::IObject obj;
        if (!navigateToObject(top, selectedPath, obj)) {
            *err = "Object not found in sequence frame: " + selectedPath;
            return false;
        }

        // One sample per frame file: read the FIRST sample (schema.get() with
        // no selector). Topology is whatever this frame carries — nothing
        // shared with the reference.
        using namespace Alembic::AbcGeom;
        if (IPolyMesh::matches(obj.getHeader())) {
            IPolyMesh polyMesh(obj);
            IPolyMeshSchema schema = polyMesh.getSchema();
            IPolyMeshSchema::Sample sample;
            schema.get(sample);
            readMeshArrays(sample.getPositions()->get(), sample.getPositions()->size(),
                           sample.getFaceIndices()->get(), sample.getFaceIndices()->size(),
                           sample.getFaceCounts()->get(), sample.getFaceCounts()->size(),
                           out);
            IV2fGeomParam uvParam = schema.getUVsParam();
            if (uvParam.valid()) {
                readAlembicUvParam(uvParam, out);
            }
        } else if (ISubD::matches(obj.getHeader())) {
            ISubD subdMesh(obj);
            ISubDSchema schema = subdMesh.getSchema();
            ISubDSchema::Sample sample;
            schema.get(sample);
            readMeshArrays(sample.getPositions()->get(), sample.getPositions()->size(),
                           sample.getFaceIndices()->get(), sample.getFaceIndices()->size(),
                           sample.getFaceCounts()->get(), sample.getFaceCounts()->size(),
                           out);
            IV2fGeomParam uvParam = schema.getUVsParam();
            if (uvParam.valid()) {
                readAlembicUvParam(uvParam, out);
            }
        } else {
            *err = "Object is not a supported mesh type: " + selectedPath;
            return false;
        }

        // Frame transform: parent IXform's first sample.
        IObject parent = obj.getParent();
        if (parent.valid() && IXform::matches(parent.getHeader())) {
            IXformSchema xSchema = IXform(parent).getSchema();
            if (xSchema.getNumSamples() > 0) {
                readXformMatrix(xSchema, 0, out->transform);
            }
        }

        if (_imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue()) {
            reverseMeshWinding(*out);
        }
        return true;

    } catch (const std::exception& e) {
        *err = std::string("Alembic error: ") + e.what();
        return false;
    } catch (...) {
        *err = "Unknown Alembic error.";
        return false;
    }
#else
    (void)frame;
    (void)out;
    if (err) {
        *err = "Alembic support not built into this Natron binary.";
    }
    return false;
#endif
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

// Core of ReadGeo::buildSceneMesh: build the merged / single-part mesh from
// an ALREADY-EVALUATED frame state. Shared verbatim by the single-file path
// (the node's own runtime + built cache) and the file-sequence path (a
// throwaway per-frame GeoArchiveRuntime evaluated at key 0). Caller provides
// the frame state; the caller is responsible for any cache insert.
MeshDataPtr
geoBuildSceneMeshCore(const GeoArchiveRuntime& rt,
                      const std::shared_ptr<const GeoArchiveFrameEval>& st,
                      const std::vector<int>& meshEntries,
                      const std::string& chosenLabel,
                      int chosenIdx,
                      bool bake,
                      double fps, double timeEff, int key)
{
    const GeoArchiveResult& data = rt.data;
    MeshDataPtr mesh(new MeshData);

    if (chosenIdx == 0) {
        // Merged: every mesh entry's evaluated vertices baked through its
        // world matrix, concatenated â€” the whole asset as one mesh. Each
        // part's file material color bakes onto its face corners (the merged
        // mesh has no per-part material slots; multi-material files keep
        // their per-part colors as face colors).
        for (std::size_t e = 0; e < meshEntries.size(); ++e) {
            const int nodeIdx = meshEntries[e];
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
            rt.worldMatrixAt(nodeIdx, timeEff, fps, 0, world);
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
    } else if (chosenIdx - 1 < (int)meshEntries.size()) {
        // Single part.
        const int nodeIdx = meshEntries[chosenIdx - 1];
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
                    rt.worldMatrixAt(nodeIdx, timeEff, fps, 0, world);
                    appendArchiveMesh(*src, world, mesh.get(), pc);
                } else {
                    // Non-baked: the part keeps its node-local vertices and
                    // the part's world matrix rides in MeshData::transform â€”
                    // the animation keeps flowing through it and the user
                    // T/R/S composes on top in every consumer.
                    float identity[16];
                    geoArchiveMatIdentity(identity);
                    appendArchiveMesh(*src, identity, mesh.get(), pc);
                    rt.worldMatrixAt(nodeIdx, timeEff, fps, 0, mesh->transform);
                }
            }
        }
    }
    // Edge indices for the viewport wireframe. Skipped for very dense meshes:
    // the array alone is ~2x the corner count (8.9M ints for a 1.5M-triangle
    // scan) and the viewport wire pass would push millions of immediate-mode
    // lines every paint, drowning the shaded fill in a uniform grey — the
    // archive path never builds edges, so big assets shaded clean there while
    // a rebuilt ReadGeo of the same file turned into a grey blob.
    if (mesh->faceIndices.size() <= 4000000) {
        importBuildEdgeIndices(mesh.get());
    }

    // Geometry-loader diagnostics gate (same pattern as NATRON_DEBUG_SC):
    // set env NATRON_DEBUG_GEOIO=1 to dump the raw built geometry (vertex
    // values, not just counts) so headless tests can verify what the loader
    // actually produced â€” a degenerate all-zero vertex array renders as an
    // empty scene and is invisible in the counts alone.
    if (std::getenv("NATRON_DEBUG_GEOIO")) {
        std::ostringstream dbg;
        dbg << "[GEOIO] fmt=" << (rt.isFbx ? "FBX" : "glTF")
            << " frame=" << (key + 1) << " sel=" << chosenLabel
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
        dbg << " uv0..u11=";
        for (std::size_t i = 0; i < mesh->uvs.size() && i < 12; ++i) {
            dbg << mesh->uvs[i] << ",";
        }
        if (mesh->uvs.size()) {
            float umn = mesh->uvs[0], umx = umn, vmn = mesh->uvs[1], vmx = vmn;
            for (std::size_t i = 0; i + 1 < mesh->uvs.size(); i += 2) {
                umn = std::min(umn, mesh->uvs[i]);
                umx = std::max(umx, mesh->uvs[i]);
                vmn = std::min(vmn, mesh->uvs[i + 1]);
                vmx = std::max(vmx, mesh->uvs[i + 1]);
            }
            dbg << " uvr=[u" << umn << ".." << umx << " v" << vmn << ".." << vmx << "]";
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

    return mesh;
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
    // granularity — the same snap the file's animated weights get through
    // ensureFrameState). Must run before ensureFrameState: the evaluators
    // read the store. Caller holds _meshMutex.
    applyMorphOverrideAt((double)(key + 1));

    const std::shared_ptr<const GeoArchiveFrameEval> st = imp->rt.ensureFrameState(fps, key);

    const std::string chosenLabel =
        (chosenIdx >= 0 && chosenIdx < (int)imp->partLabels.size())
            ? imp->partLabels[chosenIdx] : std::string();
    MeshDataPtr mesh = geoBuildSceneMeshCore(imp->rt, st, imp->meshEntries, chosenLabel,
                                             chosenIdx, bake, fps, timeEff, key);

    imp->builtCache.insert(std::make_pair(cacheKey, mesh));
    return mesh;
}

// One glTF/FBX sequence frame: a throwaway runtime parses the frame file and
// evaluates its frame-0 state; the shared scene-builder output is what gets
// cached as the frame's mesh. Materials already rode in with the reference
// load; sequence content strictly wins over the frame's own animated
// channels (only its frame-zero state is evaluated).
MeshDataPtr
ReadGeo::buildSceneSeqFrame(int frame, std::string* err) const
{
    if (!err) {
        return MeshDataPtr();
    }
    std::map<int, std::string>::const_iterator pit = _imp->seq.files.find(frame);
    if (pit == _imp->seq.files.end()) {
        *err = "no file for frame " + std::to_string(frame);
        return MeshDataPtr();
    }

    std::unique_ptr<GeoArchiveRuntime> frt(new GeoArchiveRuntime);
    const bool reverse = _imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue();
    if (!frt->loadFile(pit->second, reverse, *err)) {
        return MeshDataPtr();
    }
    const bool bake = _imp->bakeTransforms.lock() && _imp->bakeTransforms.lock()->getValue();
    frt->bakeMeshWorld = bake;
    const double fps = getApp() ? getApp()->getProjectFrameRate() : 24.0;
    const std::shared_ptr<const GeoArchiveFrameEval> st = frt->ensureFrameState(fps, 0);

    // Object selection rides the reference file's dropdown index (parts come
    // in the same order across frames; out-of-range indices build what the
    // frame file has).
    KnobChoicePtr objKnob = _imp->objectPath.lock();
    int chosenIdx = objKnob ? objKnob->getValue() : 0;
    if (chosenIdx < 0) {
        chosenIdx = 0;
    }
    const std::string chosenLabel =
        (chosenIdx >= 0 && chosenIdx < (int)_imp->partLabels.size())
            ? _imp->partLabels[chosenIdx] : std::string();
    return geoBuildSceneMeshCore(*frt, st, _imp->meshEntries, chosenLabel,
                                 chosenIdx, bake, fps, 1.0, 0);
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
// Object-hierarchy (multi-emit) mode — ported from ReadGeoArchive.cpp.
// Locking discipline: instead of the archive node's archiveMutex every
// accessor below takes _meshMutex (the same guard the single-mesh path
// uses), so a mode flip (which clears both sides) can never interleave with
// a render-thread walk of the other side's accessors.
// ---------------------------------------------------------------------------

bool
ReadGeo::isMultiEmit() const
{
    KnobBoolPtr k = _imp ? _imp->outputSingleMesh.lock() : KnobBoolPtr();
    // Knob missing (older project): the REMOVED ReadGeoArchive node aliased to
    // this class (same BuildEffect, old plugin ID) was hierarchy-only — its
    // serialized projects never carry outputSingleMesh, so they must load in
    // multi-emit. Plain ReadGeo projects default to single-mesh.
    if (!k) {
        return getPluginID() == PLUGINID_NATRON_READGEOARCHIVE;
    }
    // ON = single mesh (classic); OFF = hierarchy multi-emit.
    return !k->getValue();
}

double
ReadGeo::archiveFps() const
{
    KnobDoublePtr k = _imp->fpsKnob.lock();
    const double fpsKnob = k ? k->getValue() : 0.0;
    return fpsKnob > 0.0 ? fpsKnob : (getApp() ? getApp()->getProjectFrameRate() : 24.0);
}

void
ReadGeo::applyModeVisibility()
{
    if (!_imp) return;
    KnobBoolPtr mode = _imp->outputSingleMesh.lock();
    // ON = single mesh (classic); OFF = multi-emit hierarchy.
    const bool multi = mode ? !mode->getValue() : false;

    // --- Single-mesh-only knobs ---
    if (KnobChoicePtr k = _imp->objectPath.lock()) k->setSecret(multi);
    if (KnobBoolPtr k = _imp->freezeAtFrame.lock()) k->setSecret(multi);
    if (KnobIntPtr k = _imp->freezeFrame.lock()) k->setSecret(multi);
    if (KnobColorPtr k = _imp->baseColor.lock()) k->setSecret(multi);
    if (KnobDoublePtr k = _imp->roughness.lock()) k->setSecret(multi);
    if (KnobDoublePtr k = _imp->metallic.lock()) k->setSecret(multi);
    if (KnobDoublePtr k = _imp->specular.lock()) k->setSecret(multi);
    if (KnobColorPtr k = _imp->emissionColor.lock()) k->setSecret(multi);
    if (KnobDoublePtr k = _imp->emissionStrength.lock()) k->setSecret(multi);
    if (KnobDoublePtr k = _imp->transmission.lock()) k->setSecret(multi);
    if (KnobDoublePtr k = _imp->ior.lock()) k->setSecret(multi);
    if (KnobFilePtr k = _imp->textureFile.lock()) k->setSecret(multi);
    if (KnobChoicePtr k = _imp->diffuseColorspace.lock()) k->setSecret(multi);

    // --- Object-hierarchy-only knobs ---
    if (KnobButtonPtr k = _imp->rebuildMaterialsBtn.lock()) k->setSecret(!multi);
    if (KnobStringPtr k = _imp->objectFilter.lock()) k->setSecret(!multi);
    if (KnobBoolPtr k = _imp->includeMeshes.lock()) k->setSecret(!multi);
    if (KnobBoolPtr k = _imp->showBBox.lock()) k->setSecret(!multi);
    if (KnobIntPtr k = _imp->frameOffset.lock()) k->setSecret(!multi);
    if (KnobDoublePtr k = _imp->fpsKnob.lock()) k->setSecret(!multi);
    if (KnobStringPtr k = _imp->excludedPaths.lock()) k->setSecret(!multi);
    if (KnobChoicePtr k = _imp->rotationPivot.lock()) k->setSecret(!multi);
    if (KnobBoolPtr k = _imp->showLocators.lock()) k->setSecret(!multi);
    if (KnobDoublePtr k = _imp->locatorSize.lock()) k->setSecret(!multi);
}

void
ReadGeo::loadArchiveGeo(const std::string& path)
{
    // Caller (loadGeoFromFile) holds _meshMutex and the isLoading guard; the
    // multi state was already cleared there.

    if (path.empty()) {
        if (KnobStringPtr infoK = _imp->info.lock()) {
            infoK->setValue(std::string("No file selected."));
        }
        return;
    }

    // Reference file for the parse: the stored value itself, or the
    // sequence's first frame file when the knob holds a '#' pattern
    // (detectGeoSequence already stored the sequence in _imp->seq).
    std::string referencePath = path;
    if (_imp->seq.isSequence) {
        std::map<int, std::string>::const_iterator f0 = _imp->seq.files.begin();
        if (f0 == _imp->seq.files.end()) {
            _imp->seq = GeoSequenceInfo();
        } else {
            referencePath = f0->second;
        }
    }

    const bool reverse = _imp->reverseNormals.lock() &&
                         _imp->reverseNormals.lock()->getValue();
    std::string err;
    if (!_imp->rt.loadFile(referencePath, reverse, err)) {
        setPersistentMessage(eMessageTypeError, err);
        _imp->seq = GeoSequenceInfo();
        _imp->seqRt.clear();
        return;
    }
    _imp->rt.bakeMeshWorld = _imp->bakeTransforms.lock() &&
                             _imp->bakeTransforms.lock()->getValue();
    finishArchiveGeo(path);
}

void
ReadGeo::finishArchiveGeo(const std::string& path)
{
    // Caller holds _meshMutex; rt.data is parsed and OK.
    const GeoArchiveResult& data = _imp->rt.data;

    // Apply objectFilter + includeMeshes + excludedPaths with SUBTREE
    // semantics (the ReadGeoArchive rule): an entry is visible only if it
    // passes the filter AND its parent entry is visible — parents come
    // before children in data.nodes, so one pass suffices.
    const std::string pattern = _imp->objectFilter.lock() ?
                                    _imp->objectFilter.lock()->getValue() : std::string();
    const bool includeMeshes = _imp->includeMeshes.lock() ?
                                   _imp->includeMeshes.lock()->getValue() : true;
    const std::string excludedRaw = _imp->excludedPaths.lock() ?
                                        _imp->excludedPaths.lock()->getValue() : std::string();
    std::set<std::string> excluded;
    {
        std::size_t pos = 0;
        while (pos <= excludedRaw.size()) {
            std::size_t nl = excludedRaw.find('\n', pos);
            if (nl == std::string::npos) nl = excludedRaw.size();
            std::string line = excludedRaw.substr(pos, nl - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                     line.back() == '\t')) {
                line.pop_back();
            }
            std::size_t lead = 0;
            while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
            if (lead) line.erase(0, lead);
            if (!line.empty()) excluded.insert(line);
            if (nl == excludedRaw.size()) break;
            pos = nl + 1;
        }
    }

    std::vector<char> entryVisible(data.nodes.size(), 0);
    for (int i = 0; i < (int)data.nodes.size(); ++i) {
        const GeoArchiveNodeData& nd = data.nodes[i];
        bool pass = true;
        if (!includeMeshes && nd.isMesh) pass = false;
        if (pass && !rgGlobMatch(pattern, nd.fullPath)) pass = false;
        if (pass && excluded.count(nd.fullPath)) pass = false;
        if (pass && nd.parentIndex >= 0) pass = entryVisible[nd.parentIndex] != 0;
        entryVisible[i] = pass ? 1 : 0;
        if (pass) _imp->visible.push_back(i);
    }

    // Info string — compact single line (label-column display), the
    // ReadGeoArchive format including sequence coverage.
    std::size_t numXforms = 0, numMeshes = 0;
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        if (data.nodes[i].isMesh) ++numMeshes; else ++numXforms;
    }
    std::ostringstream ss;
    ss << data.nodes.size() << " entries (" << numXforms << " xform + " << numMeshes
       << " mesh), " << _imp->visible.size() << " shown";
    if (_imp->rt.isFbx) {
        ss << " | FBX";
    }
    {
        std::set<const GeoMorphSet*> seen;
        std::size_t numMorphs = 0;
        for (std::size_t i = 0; i < data.meshes.size(); ++i) {
            const GeoArchiveMeshData& md = data.meshes[i];
            if (md.morphs && seen.insert(md.morphs.get()).second) {
                numMorphs += md.morphs->targets.size();
            }
        }
        if (numMorphs > 0) {
            ss << " | " << numMorphs << " morphs";
        }
    }
    if (_imp->rt.hasAnimation) {
        const double dur = _imp->rt.animTimeEnd - _imp->rt.animTimeBegin;
        if (dur > 0.0) {
            const double fps = archiveFps();
            ss << " | ~" << (int)(dur * fps) << "f anim";
        } else {
            ss << " | anim";
        }
    }
    if (_imp->rt.bakeMeshWorld) {
        ss << " | baked";
    }
    if (_imp->seq.isSequence) {
        ss << " | seq frames " << _imp->seq.firstFrame << "-" << _imp->seq.lastFrame
           << " (" << _imp->seq.files.size() << " files)";
    }
    if (KnobStringPtr infoK = _imp->info.lock()) {
        infoK->setValue(ss.str());
    }

    _imp->loadedFilePath = path;
    _imp->sceneLoaded = true;
    clearPersistentMessage(false);

    // Morph Weight knobs: sync to the file's distinct morph sets (over ALL
    // meshes — collectMorphTargets); recreateKnobs can run under _meshMutex
    // (the loadSceneGeo precedent: the panel refresh does not re-enter it).
    {
        std::vector<GeoMorphTargetRef> targets;
        collectMorphTargets(&targets);
        if (syncMorphWeightKnobs(targets)) {
            recreateKnobs(true);
        }
    }
}

void
ReadGeo::loadObjArchiveGeo(const std::string& path)
{
    // Caller (loadGeoFromFile) holds _meshMutex and the isLoading guard; the
    // multi state was already cleared there.
    if (path.empty()) {
        if (KnobStringPtr infoK = _imp->info.lock()) {
            infoK->setValue(std::string("No file selected."));
        }
        return;
    }

    // Reference file for the parse: the stored value itself, or the
    // sequence's first frame file when the knob holds a '#' pattern (the
    // loadArchiveGeo resolution).
    std::string referencePath = path;
    if (_imp->seq.isSequence) {
        std::map<int, std::string>::const_iterator f0 = _imp->seq.files.begin();
        if (f0 == _imp->seq.files.end()) {
            _imp->seq = GeoSequenceInfo();
        } else {
            referencePath = f0->second;
        }
    }

    std::string err;
    GeoArchiveResult objData;
    if (!parseObjArchiveMulti(referencePath, &objData, &err)) {
        setPersistentMessage(eMessageTypeError, err);
        _imp->seq = GeoSequenceInfo();
        _imp->seqRt.clear();
        return;
    }
    // runtime is cleared (loadGeoFromFile); this parse never animates or
    // carries an FBX scene — assign the flat result straight in.
    _imp->rt.data = objData;
    finishArchiveGeo(path);
}

void
ReadGeo::loadAbcArchiveGeo(const std::string& path)
{
    // Caller (loadGeoFromFile) holds _meshMutex and the isLoading guard; the
    // multi state was already cleared there.
    if (path.empty()) {
        if (KnobStringPtr infoK = _imp->info.lock()) {
            infoK->setValue(std::string("No file selected."));
        }
        return;
    }

    // Reference file for the parse: the stored value itself, or the
    // sequence's first frame file when the knob holds a '#' pattern (the
    // loadArchiveGeo resolution).
    std::string referencePath = path;
    if (_imp->seq.isSequence) {
        std::map<int, std::string>::const_iterator f0 = _imp->seq.files.begin();
        if (f0 == _imp->seq.files.end()) {
            _imp->seq = GeoSequenceInfo();
        } else {
            referencePath = f0->second;
        }
    }

    const bool reverse = _imp->reverseNormals.lock() &&
                         _imp->reverseNormals.lock()->getValue();
    std::string err;
    if (!rgAbcParse(referencePath, reverse, &_imp->abcEntries,
                    &_imp->abcSourceFps, &err)) {
        setPersistentMessage(eMessageTypeError, err);
        _imp->abcEntries.clear();
        _imp->abcVisible.clear();
        _imp->abcSourceFps = 0.0;
        _imp->abcLoaded = false;
        _imp->seq = GeoSequenceInfo();
        _imp->seqRt.clear();
        return;
    }

    // Subtree-filtered visibility (the same rule as finishArchiveGeo / the
    // legacy node: filter glob + excludedPaths + includeMeshes, an entry
    // shows only when its parent entry does).
    const std::string pattern = _imp->objectFilter.lock() ?
                                    _imp->objectFilter.lock()->getValue() : std::string();
    const bool includeMeshes = _imp->includeMeshes.lock() ?
                                   _imp->includeMeshes.lock()->getValue() : true;
    const std::string excludedRaw = _imp->excludedPaths.lock() ?
                                        _imp->excludedPaths.lock()->getValue() : std::string();
    std::set<std::string> excluded;
    {
        std::size_t pos = 0;
        while (pos <= excludedRaw.size()) {
            std::size_t nl = excludedRaw.find('\n', pos);
            if (nl == std::string::npos) nl = excludedRaw.size();
            std::string line = excludedRaw.substr(pos, nl - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                     line.back() == '\t')) {
                line.pop_back();
            }
            std::size_t lead = 0;
            while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
            if (lead) line.erase(0, lead);
            if (!line.empty()) excluded.insert(line);
            if (nl == excludedRaw.size()) break;
            pos = nl + 1;
        }
    }
    std::vector<char> entryVisible(_imp->abcEntries.size(), 0);
    for (int i = 0; i < (int)_imp->abcEntries.size(); ++i) {
        const RgAbcEntry& e = _imp->abcEntries[i];
        bool pass = true;
        if (!includeMeshes && e.isMesh) pass = false;
        if (pass && !rgGlobMatch(pattern, e.fullPath)) pass = false;
        if (pass && excluded.count(e.fullPath)) pass = false;
        if (pass && e.parentIndex >= 0) pass = entryVisible[e.parentIndex] != 0;
        entryVisible[i] = pass ? 1 : 0;
        if (pass) _imp->abcVisible.push_back(i);
    }

    // Info string (the legacy node's format; the unified node has no
    // Time-mode knob — it evaluates frame-by-frame natively like mode 0).
    std::size_t numXforms = 0, numMeshes = 0;
    for (std::size_t i = 0; i < _imp->abcEntries.size(); ++i) {
        if (_imp->abcEntries[i].isMesh) ++numMeshes; else ++numXforms;
    }
    std::ostringstream ss;
    ss << "Total entries: " << _imp->abcEntries.size()
       << " (xforms: " << numXforms
       << ", meshes: " << numMeshes << ")"
       << " | Visible after filter: " << _imp->abcVisible.size();
    if (_imp->abcSourceFps > 0.0) {
        ss << " | Source FPS: " << _imp->abcSourceFps;
    }
    if (_imp->seq.isSequence) {
        ss << " | seq frames " << _imp->seq.firstFrame << "-" << _imp->seq.lastFrame
           << " (" << _imp->seq.files.size() << " files)";
    }
    if (KnobStringPtr infoK = _imp->info.lock()) {
        infoK->setValue(ss.str());
    }

    _imp->loadedFilePath = path;
    _imp->sceneLoaded = true;
    _imp->abcLoaded = true;
    clearPersistentMessage(false);
}

// One file per frame (multi-emit): each timeline frame samples its own
// throwaway parse kept in the bounded LRU (ReadGeoArchive policy — the
// caller holds _meshMutex).
std::shared_ptr<GeoArchiveRuntime>
ReadGeo::ensureSequenceFrameArch(double time, bool* outFallbackToReference) const
{
    *outFallbackToReference = true;
    if (time < 0.0 || _imp->seq.files.empty()) {
        return std::shared_ptr<GeoArchiveRuntime>();
    }
    // Sequence wins over the frameOffset knob: file frame numbers are NATIVE
    // to the timeline.
    const int frame = _imp->seq.clampFrame((int)std::floor(time));
    std::shared_ptr<GeoArchiveRuntime> frt = _imp->seqRt.get(frame);
    if (frt) {
        *outFallbackToReference = false;
        return frt;
    }

    std::map<int, std::string>::const_iterator it = _imp->seq.files.find(frame);
    if (it == _imp->seq.files.end()) {
        return std::shared_ptr<GeoArchiveRuntime>();
    }
    frt.reset(new GeoArchiveRuntime);
    const bool reverse = _imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue();
    std::string err;
    if (_imp->seq.extension == ".obj") {
        // OBJ sequence: parse the frame file into the flat group hierarchy
        // (loadObjArchiveMulti's rule) instead of the GeoArchiveRuntime
        // scene-format reader, which rejects .obj.
        if (!parseObjArchiveMulti(it->second, &frt->data, &err)) {
            NodePtr n(getNode());
            if (n) n->setPersistentMessage(eMessageTypeError,
                                           "Sequence frame " + std::to_string(frame) + ": " + err);
            return std::shared_ptr<GeoArchiveRuntime>();
        }
    } else if (!frt->loadFile(it->second, reverse, err)) {
        NodePtr n(getNode());
        if (n) n->setPersistentMessage(eMessageTypeError,
                                       "Sequence frame " + std::to_string(frame) + ": " + err);
        return std::shared_ptr<GeoArchiveRuntime>();
    }
    frt->bakeMeshWorld = _imp->bakeTransforms.lock() &&
                         _imp->bakeTransforms.lock()->getValue();

    // Hierarchy consistency: the frame file must carry the reference's node
    // layout; a mismatching frame cannot be sampled safely (hold the
    // reference frame's data instead).
    if (frt->data.nodes.size() != _imp->rt.data.nodes.size()) {
        NodePtr n(getNode());
        if (n) n->setPersistentMessage(eMessageTypeError,
                                       "Sequence frame " + std::to_string(frame) +
                                           ": hierarchy mismatch (" +
                                           std::to_string(frt->data.nodes.size()) +
                                           " nodes, reference has " +
                                           std::to_string(_imp->rt.data.nodes.size()) +
                                           ") — holding the reference frame's data.");
        return std::shared_ptr<GeoArchiveRuntime>();
    }

    _imp->seqRt.put(frame, frt);
    *outFallbackToReference = false;
    return frt;
}

// Alembic multi-emit sequence: parse the frame file into its own entry
// engine (per-frame files are standalone); the hierarchy check + LRU + the
// hold-the-reference rule mirror ensureSequenceFrameArch.
std::shared_ptr<RgAbcFrame>
ReadGeo::ensureAbcSequenceFrame(double time, bool* outFallbackToReference) const
{
    *outFallbackToReference = true;
    if (time < 0.0 || _imp->seq.files.empty()) {
        return std::shared_ptr<RgAbcFrame>();
    }
    // Sequence wins over the frameOffset knob: file frame numbers are NATIVE
    // to the timeline.
    const int frame = _imp->seq.clampFrame((int)std::floor(time));
    std::shared_ptr<RgAbcFrame> frt = _imp->abcSeqRt.get(frame);
    if (frt) {
        *outFallbackToReference = false;
        return frt;
    }

    std::map<int, std::string>::const_iterator it = _imp->seq.files.find(frame);
    if (it == _imp->seq.files.end()) {
        return std::shared_ptr<RgAbcFrame>();
    }
    frt.reset(new RgAbcFrame);
    const bool reverse = _imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue();
    std::string err;
    if (!rgAbcParse(it->second, reverse, &frt->entries, &frt->sourceFps, &err)) {
        NodePtr n(getNode());
        if (n) n->setPersistentMessage(eMessageTypeError,
                                       "Sequence frame " + std::to_string(frame) + ": " + err);
        return std::shared_ptr<RgAbcFrame>();
    }

    // Hierarchy consistency: the frame file must carry the reference's entry
    // layout (count only — the same rule as the runtime path).
    if (frt->entries.size() != _imp->abcEntries.size()) {
        NodePtr n(getNode());
        if (n) n->setPersistentMessage(eMessageTypeError,
                                       "Sequence frame " + std::to_string(frame) +
                                           ": hierarchy mismatch (" +
                                           std::to_string(frt->entries.size()) +
                                           " entries, reference has " +
                                           std::to_string(_imp->abcEntries.size()) +
                                           ") — holding the reference frame's data.");
        return std::shared_ptr<RgAbcFrame>();
    }

    _imp->abcSeqRt.put(frame, frt);
    *outFallbackToReference = false;
    return frt;
}

// --- ArchiveProvider interface (multi-emit; active when Output Single Mesh
// is OFF). Ported from ReadGeoArchive; the idx space is the VISIBLE entry
// index (the same space SceneNode::archiveEntryIdx stores).

int
ReadGeo::getSceneNodeCount() const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    // Alembic multi-emit: the engine's filtered entry list.
    if (_imp->abcLoaded) {
        return (int)_imp->abcVisible.size();
    }
    return (int)_imp->visible.size();
}

std::vector<ArchiveTreeEntry>
ReadGeo::getEntryTree() const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    // Alembic multi-emit: the engine entries (sampleCount = the xform
    // sample count; meshes report their vertex count).
    if (_imp->abcLoaded) {
        std::vector<ArchiveTreeEntry> out;
        out.reserve(_imp->abcEntries.size());
        for (std::size_t i = 0; i < _imp->abcEntries.size(); ++i) {
            const RgAbcEntry& e = _imp->abcEntries[i];
            ArchiveTreeEntry te;
            te.fullPath = e.fullPath;
            te.name = e.name;
            te.parentLocalIdx = e.parentIndex;
            te.isMesh = e.isMesh;
            te.sampleCount = (int)e.sampleMatrices.size();
            te.numVertices = (e.meshData) ? (int)e.meshData->numVertices : 0;
            out.push_back(te);
        }
        return out;
    }
    const GeoArchiveResult& data = _imp->rt.data;
    std::vector<ArchiveTreeEntry> out;
    out.reserve(data.nodes.size());
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        const GeoArchiveNodeData& nd = data.nodes[i];
        ArchiveTreeEntry te;
        te.fullPath = nd.fullPath;
        te.name = nd.name;
        te.parentLocalIdx = nd.parentIndex;
        te.isMesh = nd.isMesh;
        te.sampleCount = (_imp->seq.isSequence ||
                          nd.chanTr >= 0 || nd.chanRot >= 0 || nd.chanSc >= 0) ? 1 : 0;
        te.numVertices = (nd.isMesh && nd.meshIndex >= 0 &&
                          nd.meshIndex < (int)data.meshes.size() &&
                          data.meshes[nd.meshIndex].mesh)
                             ? (int)data.meshes[nd.meshIndex].mesh->numVertices : 0;
        out.push_back(te);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Morph Weights (glTF shape keys / FBX blend shapes)
// ---------------------------------------------------------------------------

void
ReadGeo::collectMorphTargets(std::vector<GeoMorphTargetRef>* out) const
{
    // Caller holds _meshMutex (guards rt.data). Distinct sets over ALL meshes
    // of the parsed file (both emission modes need this — the multi-emit
    // mode's filtered-out meshes must keep their shapes addressable, and in
    // single mode meshEntries lists the full isMesh set identically).
    if (!out) return;
    out->clear();
    if (!_imp->sceneLoaded) return;
    const GeoArchiveResult& data = _imp->rt.data;

    std::set<const GeoMorphSet*> seen;
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        const GeoArchiveNodeData& nd = data.nodes[i];
        if (!nd.isMesh) continue;
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

bool
ReadGeo::getSceneNodeAt(int idx, double time,
                        std::string& outName,
                        int& outParentLocalIndex,
                        bool& outIsMesh,
                        float outLocalMatrix[16]) const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    if (!_imp->sceneLoaded || idx < 0) {
        return false;
    }
    // Alembic multi-emit guards against the engine's own filtered list —
    // _imp->visible is empty for a plain .abc load.
    if (_imp->abcLoaded) {
        if (idx >= (int)_imp->abcVisible.size()) return false;
    } else if (idx >= (int)_imp->visible.size()) {
        return false;
    }

    // Alembic multi-emit: the engine's own walk (visible-ancestor attachment
    // + filtered-ancestor folding, ReadAlembicArchive::getSceneNodeAt).
    if (_imp->abcLoaded && idx < (int)_imp->abcVisible.size()) {
        const int srcIdx = _imp->abcVisible[idx];
        const RgAbcEntry& e = _imp->abcEntries[srcIdx];
        outName = e.fullPath;
        outIsMesh = e.isMesh;
        rgAbcEntryLocalAt(e, time, 0, 0, _imp->abcSourceFps,
                          archiveFps(), outLocalMatrix);

        outParentLocalIndex = -1;
        int p = e.parentIndex;
        while (p >= 0 && p < (int)_imp->abcEntries.size()) {
            int pv = -1;
            for (int v = 0; v < (int)_imp->abcVisible.size(); ++v) {
                if (_imp->abcVisible[v] == p) { pv = v; break; }
            }
            if (pv >= 0) {
                outParentLocalIndex = pv;
                break;
            }
            float pl[16];
            rgAbcEntryLocalAt(_imp->abcEntries[p], time, 0, 0,
                              _imp->abcSourceFps, archiveFps(), pl);
            float folded[16];
            geoArchiveMatMul(pl, outLocalMatrix, folded);
            std::memcpy(outLocalMatrix, folded, 16 * sizeof(float));
            p = _imp->abcEntries[p].parentIndex;
        }
        return true;
    }

    const int srcIdx = _imp->visible[idx];
    const GeoArchiveNodeData& nd = _imp->rt.data.nodes[srcIdx];

    outName = nd.fullPath;
    outIsMesh = nd.isMesh;

    // File-sequence mode: sample the frame file's runtime (frame-zero
    // evaluation; the frameOffset knob does not remap — ReadGeoArchive).
    GeoArchiveRuntime* actRt = &_imp->rt;
    std::shared_ptr<GeoArchiveRuntime> frameRt;
    if (_imp->seq.isSequence) {
        bool fallback = true;
        frameRt = ensureSequenceFrameArch(time, &fallback);
        if (frameRt && !fallback) {
            actRt = frameRt.get();
        } else {
            frameRt.reset();
        }
    }
    // frame-zero evaluation time (nodeLocalAt f = time - 1 - offset = 0)
    const double seqSampleTime = 1.0;

    // Baked mesh entries emit in world space attached to the archive root —
    // the world transform went into the vertices, so the chain must not
    // apply again.
    if (actRt->bakeMeshWorld && nd.isMesh) {
        outParentLocalIndex = -1;
        geoArchiveMatIdentity(outLocalMatrix);
        return true;
    }
    const double fps = archiveFps();
    const int frameOffset = _imp->frameOffset.lock() ? _imp->frameOffset.lock()->getValue() : 0;

    // Morph-weight override: fill the override store BEFORE nodeLocalAt
    // triggers ensureFrameState so a lazily-built frame embeds the user's
    // weights. Skipped in sequence mode: the per-frame files' own weights win.
    if (!frameRt) {
        const int key = (int)std::floor(time - 1.0 - (double)frameOffset);
        applyMorphOverrideAt((double)(key > 0 ? key : 0) + 1.0 + (double)frameOffset);
    }

    // The entry's own local matrix.
    if (frameRt) {
        actRt->nodeLocalAt(srcIdx, seqSampleTime, fps, 0, outLocalMatrix);
    } else {
        actRt->nodeLocalAt(srcIdx, time, fps, frameOffset, outLocalMatrix);
    }

    // Parent mapping + root normalization (ReadGeoArchive policy: fold
    // filtered-out intermediates into the entry's local and attach to the
    // nearest VISIBLE ancestor; root-level entries fold the format-level
    // root normalization so every chain composes it exactly once at the top).
    outParentLocalIndex = -1;
    bool attachRoot = (nd.parentIndex < 0);
    int p = nd.parentIndex;
    while (p >= 0 && p < (int)actRt->data.nodes.size()) {
        int pv = -1;
        for (int v = 0; v < (int)_imp->visible.size(); ++v) {
            if (_imp->visible[v] == p) { pv = v; break; }
        }
        if (pv >= 0) {
            outParentLocalIndex = pv;
            break;
        }
        float pl[16];
        if (frameRt) {
            actRt->nodeLocalAt(p, seqSampleTime, fps, 0, pl);
        } else {
            actRt->nodeLocalAt(p, time, fps, frameOffset, pl);
        }
        float folded[16];
        geoArchiveMatMul(pl, outLocalMatrix, folded);
        std::memcpy(outLocalMatrix, folded, 16 * sizeof(float));
        attachRoot = true;
        p = actRt->data.nodes[p].parentIndex;
    }
    if (attachRoot && actRt->data.rootMatrix.size() == 16) {
        float folded[16];
        geoArchiveMatMul(actRt->data.rootMatrix.data(), outLocalMatrix, folded);
        std::memcpy(outLocalMatrix, folded, 16 * sizeof(float));
    }
    return true;
}

MeshDataPtr
ReadGeo::getMeshDataAt(int idx, double time) const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    if (!_imp->sceneLoaded || idx < 0) {
        return MeshDataPtr();
    }
    if (_imp->abcLoaded) {
        if (idx >= (int)_imp->abcVisible.size()) return MeshDataPtr();
    } else if (idx >= (int)_imp->visible.size()) {
        return MeshDataPtr();
    }

    // Alembic multi-emit: the engine's per-time payload (in-place vertex
    // mutation rule; sequence frames come from their own parsed engine).
    if (_imp->abcLoaded && idx < (int)_imp->abcVisible.size()) {
        const int srcIdx = _imp->abcVisible[idx];
        if (_imp->seq.isSequence && time >= 0.0) {
            bool fallback = true;
            std::shared_ptr<RgAbcFrame> frameRt = ensureAbcSequenceFrame(time, &fallback);
            if (frameRt && !fallback) {
                return rgAbcMeshAt(frameRt->entries[srcIdx], 0.0, 0, 0,
                                   frameRt->sourceFps, archiveFps());
            }
            // frame unavailable — hold the reference frame's payload
            return rgAbcMeshAt(_imp->abcEntries[srcIdx], time, 0, 0,
                               _imp->abcSourceFps, archiveFps());
        }
        return rgAbcMeshAt(_imp->abcEntries[srcIdx], time, 0, 0,
                           _imp->abcSourceFps, archiveFps());
    }

    const int srcIdx = _imp->visible[idx];
    const GeoArchiveNodeData& nd = _imp->rt.data.nodes[srcIdx];
    if (!nd.isMesh || nd.meshIndex < 0 || nd.meshIndex >= (int)_imp->rt.data.meshes.size()) {
        return MeshDataPtr();
    }
    const GeoArchiveMeshData& md = _imp->rt.data.meshes[nd.meshIndex];
    if (!md.mesh) return MeshDataPtr();

    // File-sequence mode gates every entry through its frame file's parse
    // (topology/positions may change per frame); rest queries (time < 0)
    // serve the reference file's data.
    if (_imp->seq.isSequence && time < 0.0) {
        return md.mesh;
    }
    // Entries whose payload depends on the frame evaluation: skinned
    // (CPU skinning), baked (world bake), and morph-carrying RIGID meshes.
    const bool bakeOn = _imp->bakeTransforms.lock() &&
                        _imp->bakeTransforms.lock()->getValue();
    const bool needsFrame = (md.skinIndex >= 0) || bakeOn ||
                            (md.morphs && !md.morphs->targets.empty());
    if (!_imp->seq.isSequence && (!needsFrame || time < 0.0)) {
        return md.mesh;
    }
    if (_imp->seq.isSequence) {
        bool fallback = true;
        std::shared_ptr<GeoArchiveRuntime> frameRt = ensureSequenceFrameArch(time, &fallback);
        if (frameRt && !fallback) {
            std::shared_ptr<const GeoArchiveFrameEval> st =
                frameRt->ensureFrameState(archiveFps(), 0);
            if (nd.meshIndex < (int)st->deformed.size() && st->deformed[nd.meshIndex]) {
                return st->deformed[nd.meshIndex];
            }
            // Frame payload missing for this entry: serve the frame file's
            // rest mesh (same file, static content).
            if (nd.meshIndex >= 0 && nd.meshIndex < (int)frameRt->data.meshes.size() &&
                frameRt->data.meshes[nd.meshIndex].mesh) {
                return frameRt->data.meshes[nd.meshIndex].mesh;
            }
        }
        return md.mesh; // frame unavailable — hold the reference frame
    }

    // Skinned/morph/baked: per-frame evaluation via the frame cache.
    const int frameOffset = _imp->frameOffset.lock() ? _imp->frameOffset.lock()->getValue() : 0;
    int key = (int)std::floor(time - 1.0 - (double)frameOffset);
    if (key < 0) key = 0;
    applyMorphOverrideAt((double)(key + 1) + (double)frameOffset);
    std::shared_ptr<const GeoArchiveFrameEval> st =
        _imp->rt.ensureFrameState(archiveFps(), key);
    if (nd.meshIndex < (int)st->deformed.size() && st->deformed[nd.meshIndex]) {
        return st->deformed[nd.meshIndex];
    }
    return md.mesh;
}

bool
ReadGeo::getEntryWorldMatrix(int idx, double time, float outWorld[16]) const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    if (!_imp->sceneLoaded || idx < 0) {
        return false;
    }
    if (_imp->abcLoaded) {
        if (idx >= (int)_imp->abcVisible.size()) return false;
    } else if (idx >= (int)_imp->visible.size()) {
        return false;
    }

    // Alembic multi-emit: engine world matrix (chain composition; no bake
    // post-pass in abc mode — the legacy node has none).
    if (_imp->abcLoaded && idx < (int)_imp->abcVisible.size()) {
        const int srcIdx = _imp->abcVisible[idx];
        if (_imp->seq.isSequence && time >= 0.0) {
            bool fallback = true;
            std::shared_ptr<RgAbcFrame> frameRt = ensureAbcSequenceFrame(time, &fallback);
            if (frameRt && !fallback) {
                rgAbcEntryWorldAt(frameRt->entries, srcIdx, 1.0, 0, 0,
                                  frameRt->sourceFps, archiveFps(), outWorld);
                return true;
            }
            // frame unavailable — hold the reference file's state
        }
        rgAbcEntryWorldAt(_imp->abcEntries, srcIdx, time, 0, 0,
                          _imp->abcSourceFps, archiveFps(), outWorld);
        return true;
    }

    const int srcIdx = _imp->visible[idx];

    // File-sequence mode: the world matrix comes from the frame file's
    // runtime (its frame-zero state); the sequence owns the timing.
    if (_imp->seq.isSequence && time >= 0.0) {
        bool fallback = true;
        std::shared_ptr<GeoArchiveRuntime> frameRt = ensureSequenceFrameArch(time, &fallback);
        if (frameRt && !fallback) {
            const bool bakeF = _imp->bakeTransforms.lock() &&
                               _imp->bakeTransforms.lock()->getValue();
            if (bakeF && frameRt->data.nodes[srcIdx].isMesh) {
                geoArchiveMatIdentity(outWorld);
                return true;
            }
            frameRt->worldMatrixAt(srcIdx, 1.0, archiveFps(), 0, outWorld);
            return true;
        }
        // Frame unavailable: hold the reference file's state below.
    }

    // Baked mesh entries are world-space payloads parented to the root —
    // their entry world matrix is identity by construction.
    const bool bakeOn = _imp->bakeTransforms.lock() &&
                        _imp->bakeTransforms.lock()->getValue();
    if (bakeOn && _imp->rt.data.nodes[srcIdx].isMesh) {
        geoArchiveMatIdentity(outWorld);
        return true;
    }

    // Morph-weight override: fill the store before worldMatrixAt's potential
    // ensureFrameState (same key mapping as getSceneNodeAt).
    {
        const int frameOffset = _imp->frameOffset.lock() ? _imp->frameOffset.lock()->getValue() : 0;
        const int key = (int)std::floor(time - 1.0 - (double)frameOffset);
        applyMorphOverrideAt((double)(key > 0 ? key : 0) + 1.0 + (double)frameOffset);
    }

    const int frameOffset = _imp->frameOffset.lock() ? _imp->frameOffset.lock()->getValue() : 0;
    _imp->rt.worldMatrixAt(_imp->visible[idx], time, archiveFps(), frameOffset, outWorld);
    return true;
}

bool
ReadGeo::getRootMatrix(float outRoot[16]) const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    // Alembic multi-emit: no format-level root normalization (identity).
    if (_imp->abcLoaded) {
        geoArchiveMatIdentity(outRoot);
        return false;
    }
    if (!_imp->sceneLoaded || _imp->rt.data.rootMatrix.size() != 16) {
        geoArchiveMatIdentity(outRoot);
        return false;
    }
    std::memcpy(outRoot, _imp->rt.data.rootMatrix.data(), 16 * sizeof(float));
    return true;
}

MaterialProvider*
ReadGeo::getEntryMaterialProvider(int idx) const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    // idx is the VISIBLE entry index — remap to the full-archive node index
    // before touching rt.data (ReadGeoArchive: resolving the visible index
    // directly re-shuffled materials when subtrees were filtered out).
    // Alembic multi-emit: abc carries no entry materials (the legacy node
    // serves the connected input via the node's own getters) — no provider.
    if (_imp->abcLoaded) {
        return nullptr;
    }
    if (!_imp->sceneLoaded || idx < 0 || idx >= (int)_imp->visible.size()) return nullptr;
    const int srcIdx = _imp->visible[idx];
    if (srcIdx < 0 || srcIdx >= (int)_imp->rt.data.nodes.size()) return nullptr;
    const GeoArchiveNodeData& nd = _imp->rt.data.nodes[srcIdx];
    if (!nd.isMesh || nd.meshIndex < 0 || nd.meshIndex >= (int)_imp->rt.data.meshes.size()) {
        return nullptr;
    }

    // Cached adapter? Keyed by the FULL node index so the cache survives
    // visibility changes.
    std::map<int, std::shared_ptr<RGEntryMaterialAdapter>>::const_iterator it =
        _imp->entryMaterials.find(srcIdx);
    if (it != _imp->entryMaterials.end()) return it->second.get();

    const GeoArchiveMeshData& md = _imp->rt.data.meshes[nd.meshIndex];
    if (md.materialIndex < 0 ||
        md.materialIndex >= (int)_imp->rt.data.materials.size() ||
        !_imp->rt.data.materials[md.materialIndex].present) {
        return nullptr;
    }

    std::shared_ptr<RGEntryMaterialAdapter> adapter =
        std::make_shared<RGEntryMaterialAdapter>(_imp->rt.data.materials[md.materialIndex]);
    _imp->entryMaterials[srcIdx] = adapter;
    return adapter.get();
}

// --- Re-build Materials (multi-emit) -----------------------------------------
// Standalone ReadGeo + Material3D node set under Group3D, replicating this
// node's archive-mode output; the port of ReadGeoArchive::rebuildMaterialsSet
// (same layout, naming and semantics — only the mutex differs).

void
ReadGeo::rebuildMaterialsSet()
{
    AppInstancePtr app = getApp();
    NodePtr thisNode = getNode();
    if (!app || !thisNode) {
        return;
    }
    if (_imp->rebuildBusy) {
        return;
    }

    KnobFilePtr fp = _imp->filePath.lock();
    const std::string filePath = fp ? fp->getValue() : std::string();

    // Snapshot the visible mesh entries + their file materials under the
    // lock. objectPathIdx = the part's 1-based rank among ALL mesh entries of
    // the file (ReadGeo's Object dropdown counts the unfiltered file).
    struct RebuildPart
    {
        std::string path;
        int objectPathIdx;
        bool hasMaterial;
        GeoImportMaterial material; // read only when hasMaterial
    };
    std::vector<RebuildPart> parts;
    {
        std::lock_guard<std::mutex> lk(_meshMutex);
        if (_imp->loadedFilePath.empty() || filePath.empty() || _imp->rt.data.nodes.empty()) {
            setPersistentMessage(eMessageTypeError,
                "Re-build Materials: no file loaded on the node.");
            return;
        }
        std::vector<char> visFlag(_imp->rt.data.nodes.size(), 0);
        for (std::size_t v = 0; v < _imp->visible.size(); ++v) {
            if (_imp->visible[v] >= 0 && _imp->visible[v] < (int)visFlag.size()) {
                visFlag[_imp->visible[v]] = 1;
            }
        }
        int meshRank = 0;
        for (std::size_t i = 0; i < _imp->rt.data.nodes.size(); ++i) {
            const GeoArchiveNodeData& nd = _imp->rt.data.nodes[i];
            if (!nd.isMesh) continue;
            ++meshRank;
            if (!visFlag[i]) continue;
            RebuildPart p;
            p.path = nd.fullPath;
            p.objectPathIdx = meshRank;
            p.hasMaterial = false;
            if (nd.meshIndex >= 0 && nd.meshIndex < (int)_imp->rt.data.meshes.size()) {
                const GeoArchiveMeshData& md = _imp->rt.data.meshes[nd.meshIndex];
                if (md.materialIndex >= 0 &&
                    md.materialIndex < (int)_imp->rt.data.materials.size() &&
                    _imp->rt.data.materials[md.materialIndex].present) {
                    p.hasMaterial = true;
                    p.material = _imp->rt.data.materials[md.materialIndex];
                }
            }
            parts.push_back(p);
        }
    }
    if (parts.empty()) {
        setPersistentMessage(eMessageTypeError,
            "Re-build Materials: no mesh entries are currently visible "
            "(check Object Filter / Excluded Paths).");
        return;
    }

    // Mirror selected archive-wide knobs onto the copies (bake is forced
    // instead — see the ReadGeo creation below).
    const bool archiveReverse = _imp->reverseNormals.lock() &&
                             _imp->reverseNormals.lock()->getValue();
    const bool archiveUseFileMorphs = _imp->useFileMorphWeights.lock() &&
                                      _imp->useFileMorphWeights.lock()->getValue();

    NodeCollectionPtr grp = thisNode->getGroup();
    _imp->rebuildBusy = true;
    struct BusyReset
    {
        bool* flag;
        explicit BusyReset(bool* f) : flag(f) {}
        ~BusyReset() { *flag = false; }
    } busyReset(&_imp->rebuildBusy);

    // Root group: assembles the per-part pairs (8 inputs each — more parts
    // nest additional Group3Ds at the root's spare slots). Name:
    // Group3d_RebuiltMaterials_NN — NN counts previous rebuilds in the
    // project (scan existing nodes for the highest suffix).
    std::string rootName = "Group3d_RebuiltMaterials_1";
    {
        static const std::string prefix = "Group3d_RebuiltMaterials";
        int maxN = 0;
        NodesList nodes = app->getProject()->getNodes();
        for (const NodePtr& n : nodes) {
            const std::string lbl = n->getLabel();
            if (lbl.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string rem = lbl.substr(prefix.size());
            if (!rem.empty() && rem[0] == '_') {
                const int v = std::atoi(rem.c_str() + 1);
                if (v > maxN && v < 100000) maxN = v;
            }
        }
        rootName = prefix + "_" + std::to_string(maxN + 1);
    }
    CreateNodeArgs gArgs(PLUGINID_NATRON_GROUP3D, grp);
    gArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    gArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
    gArgs.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    gArgs.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, rootName);
    NodePtr rootGroup = app->createNode(gArgs);
    if (!rootGroup) {
        setPersistentMessage(eMessageTypeError,
            "Re-build Materials: failed to create the Group3D node.");
        return;
    }
    rootGroup->setLabel(rootName);

    // Mirror this node's user TRS onto the group (Uniform Scale folds into
    // the per-axis scales; Group3D has no uniform-scale knob).
    {
        KnobBoolPtr enable = std::dynamic_pointer_cast<KnobBool>(
            rootGroup->getKnobByName("enableTransform"));
        static const char* trsNames[9] = { "translateX", "translateY", "translateZ",
                                           "rotateX", "rotateY", "rotateZ",
                                           "scaleX", "scaleY", "scaleZ" };
        double trsVals[9] = { 0, 0, 0, 0, 0, 0, 1, 1, 1 };
        double uniformScale = 1.0;
        bool anyNonDefault = false;
        for (int i = 0; i < 9; ++i) {
            KnobIPtr k = thisNode->getKnobByName(trsNames[i]);
            KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k);
            if (d) {
                trsVals[i] = d->getValue();
                if (i < 6 && trsVals[i] != 0.0) anyNonDefault = true;
            }
        }
        {
            KnobIPtr k = thisNode->getKnobByName("uniformScale");
            KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k);
            if (d) {
                uniformScale = d->getValue();
                if (uniformScale != 1.0) anyNonDefault = true;
            }
        }
        if (enable) {
            enable->setValue(anyNonDefault);
        }
        if (anyNonDefault) {
            for (int i = 0; i < 9; ++i) {
                KnobIPtr k = rootGroup->getKnobByName(trsNames[i]);
                KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k);
                if (d) {
                    d->setValue(trsVals[i]);
                }
            }
            for (int i = 6; i < 9; ++i) {
                KnobIPtr k = rootGroup->getKnobByName(trsNames[i]);
                KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k);
                if (d) {
                    d->setValue(trsVals[i] * uniformScale);
                }
            }
        }
    }

    // Layout anchor: this node's graph position. Data reads left to right —
    // Material3D -> part ReadGeo -> sub Group3D -> root Group3D.
    double px = 0, py = 0;
    thisNode->getPosition(&px, &py);
    const double rowHeight = 150.0;

    const int kGroupInputs = 8;
    NodePtr curGroup = rootGroup;
    int curSlot = 0;
    int rootSlot = 0;

    for (std::size_t i = 0; i < parts.size(); ++i) {
        const RebuildPart& part = parts[i];
        if (curSlot >= kGroupInputs) {
            CreateNodeArgs sArgs(PLUGINID_NATRON_GROUP3D, grp);
            sArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            sArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
            sArgs.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
            NodePtr sub = app->createNode(sArgs);
            if (!sub) {
                setPersistentMessage(eMessageTypeError,
                    "Re-build Materials: failed to create a sub-Group3D.");
                return;
            }
            sub->setLabel(rootName);
            // Only the root group draws the bounding box (parity with the
            // archive's single box); nested sub-groups stay unboxed.
            if (KnobIPtr sbk = sub->getKnobByName("showBBox")) {
                if (KnobBoolPtr sb = std::dynamic_pointer_cast<KnobBool>(sbk)) {
                    sb->setValue(false);
                }
            }
            rootGroup->connectInput(sub, rootSlot++);
            sub->setPosition(px + 780, py + rowHeight * (double)(i + 1));
            curGroup = sub;
            curSlot = 0;
        }

        // Per-part ReadGeo: file set first (fires the synchronous parse +
        // dropdown build), then the Object dropdown scopes it to the part.
        CreateNodeArgs rArgs(PLUGINID_NATRON_READGEO, grp);
        rArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        rArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
        rArgs.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        NodePtr geo = app->createNode(rArgs);
        if (!geo) {
            setPersistentMessage(eMessageTypeError,
                "Re-build Materials: failed to create the ReadGeo node for '" + part.path + "'.");
            return;
        }
        geo->setLabel(part.path.empty()
                          ? ("part_" + std::to_string(i + 1))
                          : part.path);
        if (KnobIPtr fk = geo->getKnobByName("filename")) {
            if (KnobFilePtr ff = std::dynamic_pointer_cast<KnobFile>(fk)) {
                ff->setValue(filePath);
            }
        }
        if (KnobIPtr ok = geo->getKnobByName("objectPath")) {
            if (KnobChoicePtr oc = std::dynamic_pointer_cast<KnobChoice>(ok)) {
                oc->setValue(part.objectPathIdx);
            }
        }
        if (KnobIPtr rk = geo->getKnobByName("reverseNormals")) {
            if (KnobBoolPtr rb = std::dynamic_pointer_cast<KnobBool>(rk)) {
                rb->setValue(archiveReverse);
            }
        }
        if (KnobIPtr mk = geo->getKnobByName("useFileMorphWeights")) {
            if (KnobBoolPtr mb = std::dynamic_pointer_cast<KnobBool>(mk)) {
                mb->setValue(archiveUseFileMorphs);
            }
        }

        // Material3D carrying the part's file material.
        NodePtr mat;
        if (part.hasMaterial) {
            CreateNodeArgs mArgs(PLUGINID_NATRON_MATERIAL3D, grp);
            mArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            mArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
            mArgs.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
            mat = app->createNode(mArgs);
            if (!mat) {
                setPersistentMessage(eMessageTypeError,
                    "Re-build Materials: failed to create the Material3D node for '" + part.path + "'.");
                return;
            }
            mat->setLabel(part.path.empty()
                              ? ("part_" + std::to_string(i + 1) + "_mat")
                              : (part.path + "_mat"));
            setRebuildMaterialKnobs(mat, part.material);
        }

        // Wire: material feeds the part's Material input; part feeds group.
        if (mat) {
            geo->connectInput(mat, 0);
        }
        curGroup->connectInput(geo, curSlot++);

        // Bake Transforms is forced ON: the baked path is the consumer-verified
        // one (probe-verified pixel-identical to the archive output).
        if (KnobIPtr bk = geo->getKnobByName("bakeTransforms")) {
            if (KnobBoolPtr bb = std::dynamic_pointer_cast<KnobBool>(bk)) {
                bb->setValue(true);
            }
        }
        geo->setPosition(px + 620, py + rowHeight * (double)(i + 1));
        if (mat) {
            mat->setPosition(px + 460, py + rowHeight * (double)(i + 1));
        }
    }

    rootGroup->setPosition(px + 940, py);
    // No info PersistentMessage here: in the GUI an info PersistentMessage
    // pops a modal OK dialog after every rebuild (wedges scripted/bridge
    // sessions). Error paths above still report via setPersistentMessage.
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

// One built sequence frame (caller holds _meshMutex): the timeline frame maps
// to the sequence's FILE frame numbers — frozen evaluation substitutes the
// frozen frame, out-of-range/gap times hold at the nearest present file.
// Built meshes are served from the bounded LRU; misses parse + build the
// frame file through the format dispatch.
MeshDataPtr
ReadGeo::getMeshDataAtSeqFrame(double time) const
{
    double tEff = time;
    if (_imp->freezeAtFrame.lock() && _imp->freezeAtFrame.lock()->getValue()) {
        tEff = (double)(_imp->freezeFrame.lock()
                            ? _imp->freezeFrame.lock()->getValue() : 1);
    }
    int frame = (int)std::floor(tEff < 0.0 ? 0.0 : tEff);
    frame = _imp->seq.clampFrame(frame);

    std::shared_ptr<MeshData> cached = _imp->frameCache.get(frame);
    if (cached) {
        return MeshDataPtr(new MeshData(*cached));   // G1 copy-on-read
    }

    std::string err;
    MeshDataPtr mesh;
    if (_imp->seq.extension == ".obj") {
        mesh = MeshDataPtr(new MeshData);
        if (!buildObjSeqFrame(frame, mesh.get(), &err)) {
            mesh.reset();
        }
    } else if (_imp->seq.extension == ".abc") {
        mesh = MeshDataPtr(new MeshData);
        if (!buildAbcSeqFrame(frame, mesh.get(), &err)) {
            mesh.reset();
        }
    } else if (_imp->seq.extension == ".gltf" || _imp->seq.extension == ".glb" ||
               _imp->seq.extension == ".fbx") {
        mesh = buildSceneSeqFrame(frame, &err);
    } else {
        err = "format not handled: " + _imp->seq.extension;
    }

    if (!mesh) {
        NodePtr n(getNode());
        if (n) n->setPersistentMessage(eMessageTypeError,
                                       "Sequence frame " + std::to_string(frame) + ": " + err);
        return MeshDataPtr();
    }
    if (std::getenv("GSEQ_DEBUG")) {
        std::fprintf(stderr, "[GSEQ] built frame %d ext=%s verts=%d faces=%d\n",
                     frame, _imp->seq.extension.c_str(), mesh->numVertices,
                     mesh->numFaces);
        std::fflush(stderr);
    }
    _imp->frameCache.put(frame, mesh);
    return MeshDataPtr(new MeshData(*mesh));
}

MeshDataPtr
ReadGeo::getMeshData(double time) const
{
    // Serialise against a concurrent reload swapping/clearing _lastMeshData.
    std::lock_guard<std::mutex> lk(_meshMutex);

    // Object-hierarchy mode: the geometry lives in the ArchiveProvider
    // accessors; the consumers route there (their MeshProvider branches skip
    // this node when isMultiEmit() is true).
    if (_imp && isMultiEmit()) {
        return MeshDataPtr();
    }

    // File-sequence mode: every frame comes from its own
    // file — nothing can ride the single-file machinery below.
    if (_imp && _imp->seq.isSequence) {
        return getMeshDataAtSeqFrame(time);
    }

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
    // Object-hierarchy mode: freeze knobs are not visible there — the mode is
    // frame-varying whenever the archive has animation or a sequence is
    // loaded (the ReadGeoArchive metadata rule).
    if (isMultiEmit()) {
        if (_imp->seq.isSequence || (_imp->sceneLoaded && _imp->rt.hasAnimation)) {
            metadata.setIsFrameVarying(true);
            return eStatusOK;
        }
        // Alembic multi-emit: animated content = multi-sample xform OR
        // animated verts (the ReadAlembicArchive metadata rule).
        if (_imp->abcLoaded) {
            for (std::size_t i = 0; i < _imp->abcEntries.size(); ++i) {
                const RgAbcEntry& e = _imp->abcEntries[i];
                if (e.isMesh ? e.hasAnimatedVerts : (e.sampleMatrices.size() > 1)) {
                    metadata.setIsFrameVarying(true);
                    return eStatusOK;
                }
            }
        }
        // Animated user morph weights still make the node time-varying.
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
    const bool frozen = _imp->freezeAtFrame.lock() &&
                        _imp->freezeAtFrame.lock()->getValue();
    if (frozen) {
        return eStatusOK;
    }
    // A loaded file sequence is frame-varying by construction (the frame
    // files are independent), regardless of the file format or its own
    // animation flags.
    if (_imp->seq.isSequence) {
        metadata.setIsFrameVarying(true);
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
    if (isMultiEmit()) {
        if (MaterialProvider* mat = getConnectedMaterial()) {
            mat->getMaterialBaseColor(time, r, g, b);
            return;
        }
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            r = _imp->rt.data.materials[0].baseColor[0];
            g = _imp->rt.data.materials[0].baseColor[1];
            b = _imp->rt.data.materials[0].baseColor[2];
            return;
        }
        r = 0.7; g = 0.7; b = 0.7;
        return;
    }
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}

double ReadGeo::getMaterialRoughness(double time) const
{
    if (isMultiEmit()) {
        if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialRoughness(time);
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            return _imp->rt.data.materials[0].roughness;
        }
        return 0.5;
    }
    KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5;
}

double ReadGeo::getMaterialMetallic(double time) const
{
    if (isMultiEmit()) {
        if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialMetallic(time);
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            return _imp->rt.data.materials[0].metallic;
        }
        return 0.0;
    }
    KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0;
}

double ReadGeo::getMaterialSpecular(double time) const
{
    if (isMultiEmit()) {
        if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialSpecular(time);
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            return _imp->rt.data.materials[0].specular;
        }
        return 0.5;
    }
    KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5;
}

void
ReadGeo::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    if (isMultiEmit()) {
        if (MaterialProvider* mat = getConnectedMaterial()) {
            mat->getMaterialEmission(time, r, g, b, strength);
            return;
        }
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            r = _imp->rt.data.materials[0].emission[0];
            g = _imp->rt.data.materials[0].emission[1];
            b = _imp->rt.data.materials[0].emission[2];
            strength = _imp->rt.data.materials[0].emissionStrength;
            return;
        }
        r = 1.0; g = 1.0; b = 1.0; strength = 0.0;
        return;
    }
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}

double ReadGeo::getMaterialTransmission(double time) const
{
    if (isMultiEmit()) {
        if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialTransmission(time);
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            return _imp->rt.data.materials[0].transmission;
        }
        return 0.0;
    }
    KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0;
}

double ReadGeo::getMaterialIOR(double time) const
{
    if (isMultiEmit()) {
        if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialIOR(time);
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            return _imp->rt.data.materials[0].ior;
        }
        return 1.45;
    }
    KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45;
}

std::string ReadGeo::getMaterialDiffuseColorspace() const
{
    if (isMultiEmit()) {
        // Multi-emit: the entry adapters pin file textures to sRGB (the
        // ReadGeoArchive contract); node-level diffuse coloring is unused.
        if (MaterialProvider* mat = getConnectedMaterial()) {
            return mat->getMaterialDiffuseColorspace();
        }
        return "sRGB";
    }
    KnobChoicePtr k = _imp->diffuseColorspace.lock();
    int idx = k ? k->getValue() : 0;
    const char* names[] = {"sRGB", "Linear", "ACEScg", "Raw"};
    return (idx >= 0 && idx < 4) ? names[idx] : "sRGB";
}

std::string ReadGeo::getMaterialTextureFile() const
{
    if (isMultiEmit()) {
        // The first material-bearing part's diffuse map — single-mesh
        // texture-albedo semantics on the archive-mode node level.
        if (MaterialProvider* mat = getConnectedMaterial()) {
            return mat->getMaterialTextureFile();
        }
        if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
            return _imp->rt.data.materials[0].diffuseMapPath;
        }
        return std::string();
    }
    KnobFilePtr k = _imp->textureFile.lock(); return k ? k->getValue() : std::string();
}

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
