/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ReadAlembicArchive.h"
#include "../DotUtils.h"

#include "../../NodeMetadata.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <mutex>
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
#include <Alembic/AbcCoreFactory/All.h>
#endif

NATRON_NAMESPACE_ENTER

#ifdef NATRON_HAVE_ALEMBIC

// One cached entry per IXform or IPolyMesh in the archive.
struct ArchiveEntry
{
    std::string fullPath;            // "/Camera01Trackers/Tracker1"
    std::string name;                // leaf name, "Tracker1"
    int parentIndex;                 // index into the entries vector; -1 if archive-root
    bool isMesh;                     // true => IPolyMesh, false => IXform (locator)

    // Sample data for xforms. Empty for meshes.
    std::vector<double> sampleTimes;
    std::vector<Imath::M44d> sampleMatrices;

    // Mesh geometry — only populated for entries where isMesh == true.
    // meshData->vertices holds the CURRENT-frame positions (mutated in place
    // by updateVerticesAtTime when hasAnimatedVerts is true). Topology
    // (faceIndices / faceCounts) stays constant across samples.
    MeshDataPtr meshData;

    // Vertex animation cache — populated by walkObjectRecursive when the
    // PolyMesh schema reports more than one sample. Same pattern as ReadGeo:
    // pre-load all samples into memory, then per-frame memcpy into
    // meshData->vertices. vertexSampleTimes mirror the source TimeSampling
    // so timeMode==1 (Source FPS) can pick the nearest sample correctly.
    bool hasAnimatedVerts;
    std::vector<double> vertexSampleTimes;
    std::vector<std::vector<float>> vertexSamples;

    ArchiveEntry() : parentIndex(-1), isMesh(false), hasAnimatedVerts(false) {}
};

// Read sample 0 of an IPolyMesh into a fresh MeshDataPtr.
// Returns nullptr if reading fails. Mesh vertices are in the mesh's local frame
// (the parent IXform carries the world placement, which our SceneGraph composes
// via parentIndex chaining).
static MeshDataPtr
readPolyMeshSample0(const Alembic::AbcGeom::IPolyMesh& polyMesh)
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

    const size_t numVerts = positions->size();
    const size_t numFaceIdx = faceIndices->size();
    const size_t numFaces = faceCounts->size();
    if (numVerts == 0 || numFaceIdx == 0 || numFaces == 0) return MeshDataPtr();

    MeshDataPtr mesh = std::make_shared<MeshData>();

    // Vertices (raw float access — same pattern as ReadGeo to avoid Imath::V3f
    // struct-layout issues across Imath builds).
    const float* rawPos = reinterpret_cast<const float*>(positions->get());
    mesh->vertices.resize(numVerts * 3);
    mesh->numVertices = numVerts;
    std::memcpy(mesh->vertices.data(), rawPos, numVerts * 3 * sizeof(float));

    // Face indices.
    const int32_t* rawIdx = reinterpret_cast<const int32_t*>(faceIndices->get());
    mesh->faceIndices.resize(numFaceIdx);
    for (size_t i = 0; i < numFaceIdx; ++i) mesh->faceIndices[i] = (int)rawIdx[i];

    // Face counts.
    const int32_t* rawCnt = reinterpret_cast<const int32_t*>(faceCounts->get());
    mesh->faceCounts.resize(numFaces);
    mesh->numFaces = numFaces;
    for (size_t i = 0; i < numFaces; ++i) mesh->faceCounts[i] = (int)rawCnt[i];

    // Edge indices for wireframe.
    mesh->edgeIndices.reserve(numFaceIdx * 2);
    size_t idxOffset = 0;
    for (size_t f = 0; f < numFaces; ++f) {
        const int count = (int)rawCnt[f];
        for (int v = 0; v < count; ++v) {
            mesh->edgeIndices.push_back((int)rawIdx[idxOffset + v]);
            mesh->edgeIndices.push_back((int)rawIdx[idxOffset + ((v + 1) % count)]);
        }
        idxOffset += count;
    }

    // UVs (optional).
    IV2fGeomParam uvParam = schema.getUVsParam();
    if (uvParam.valid()) {
        IV2fGeomParam::Sample uvSample;
        uvParam.getExpanded(uvSample);
        const Alembic::Abc::V2fArraySamplePtr& uvVals = uvSample.getVals();
        if (uvVals && uvVals->size() > 0) {
            const size_t numUVs = uvVals->size();
            const float* rawUV = reinterpret_cast<const float*>(uvVals->get());
            mesh->uvs.resize(numUVs * 2);
            std::memcpy(mesh->uvs.data(), rawUV, numUVs * 2 * sizeof(float));
            mesh->hasUVs = true;
        }
    }

    // Local transform stays identity — the parent IXform's transform is handled
    // by the SceneGraph parent chain.
    return mesh;
}

// Simple glob match: '*' matches any substring (incl. empty), no other special chars.
// Empty pattern matches everything.
static bool
globMatch(const std::string& pattern, const std::string& text)
{
    if (pattern.empty()) return true;
    // dp[i][j] = does pattern[0..i) match text[0..j)?
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

// Recursive archive walker.
static void
walkObjectRecursive(const Alembic::AbcGeom::IObject& obj,
                    const std::string& parentPath,
                    int parentEntryIndex,
                    std::vector<ArchiveEntry>& entries)
{
    using namespace Alembic::AbcGeom;

    const std::string fullPath = parentPath.empty() ? std::string("/") + obj.getName()
                                                    : parentPath + "/" + obj.getName();

    int thisEntryIndex = -1;

    if (IXform::matches(obj.getHeader())) {
        IXform xf(obj, kWrapExisting);
        IXformSchema& schema = xf.getSchema();
        const size_t numSamples = schema.getNumSamples();
        Alembic::AbcCoreAbstract::TimeSamplingPtr ts = schema.getTimeSampling();

        ArchiveEntry e;
        e.fullPath = fullPath;
        e.name = obj.getName();
        e.parentIndex = parentEntryIndex;
        e.isMesh = false;
        e.sampleTimes.reserve(numSamples);
        e.sampleMatrices.reserve(numSamples);
        for (size_t i = 0; i < numSamples; ++i) {
            const double t = ts ? ts->getSampleTime((Alembic::AbcCoreAbstract::index_t)i) : 0.0;
            XformSample xs;
            schema.get(xs, ISampleSelector((Alembic::AbcCoreAbstract::index_t)i));
            e.sampleTimes.push_back(t);
            e.sampleMatrices.push_back(xs.getMatrix());
        }
        thisEntryIndex = (int)entries.size();
        entries.push_back(e);
    } else if (IPolyMesh::matches(obj.getHeader())) {
        // Read sample 0 as rest pose, then pre-load all remaining vertex
        // samples for deforming meshes. Topology is assumed constant across
        // samples (the standard Maya / Blender / Houdini export). The parent
        // IXform's animation drives positioning via the SceneGraph parent
        // chain; vertex animation handled here.
        IPolyMesh polyMesh(obj, kWrapExisting);
        ArchiveEntry e;
        e.fullPath = fullPath;
        e.name = obj.getName();
        e.parentIndex = parentEntryIndex;
        e.isMesh = true;
        e.meshData = readPolyMeshSample0(polyMesh);

        if (e.meshData && e.meshData->numVertices > 0) {
            IPolyMeshSchema schema = polyMesh.getSchema();
            const size_t numVertSamples = schema.getNumSamples();
            if (numVertSamples > 1) {
                Alembic::AbcCoreAbstract::TimeSamplingPtr ts = schema.getTimeSampling();
                e.hasAnimatedVerts = true;
                e.vertexSamples.assign(numVertSamples, std::vector<float>());
                e.vertexSampleTimes.reserve(numVertSamples);
                const size_t expectedFloats = e.meshData->numVertices * 3;
                for (size_t s = 0; s < numVertSamples; ++s) {
                    const double t = ts ? ts->getSampleTime((Alembic::AbcCoreAbstract::index_t)s) : (double)s;
                    e.vertexSampleTimes.push_back(t);
                    Alembic::Abc::ISampleSelector ss((Alembic::AbcCoreAbstract::index_t)s);
                    IPolyMeshSchema::Sample animSample;
                    schema.get(animSample, ss);
                    Alembic::Abc::P3fArraySamplePtr positions = animSample.getPositions();
                    if (!positions || positions->size() != e.meshData->numVertices) continue;
                    const float* posPtr = reinterpret_cast<const float*>(positions->get());
                    e.vertexSamples[s].resize(expectedFloats);
                    std::memcpy(e.vertexSamples[s].data(), posPtr, expectedFloats * sizeof(float));
                }
            }
        }

        thisEntryIndex = (int)entries.size();
        entries.push_back(e);
    }
    // Cameras / lights / other types: skipped — they have their own dedicated readers.

    // Recurse. Use thisEntryIndex if we created one, otherwise pass through parentEntryIndex
    // so deeper entries still attach to the nearest enclosing xform/mesh.
    const int childParent = (thisEntryIndex >= 0) ? thisEntryIndex : parentEntryIndex;
    for (size_t i = 0; i < obj.getNumChildren(); ++i) {
        walkObjectRecursive(obj.getChild(i), fullPath, childParent, entries);
    }
}

#endif // NATRON_HAVE_ALEMBIC


struct ReadAlembicArchivePrivate
{
    KnobFileWPtr filePath;
    KnobButtonWPtr reloadBtn;
    KnobStringWPtr objectFilter;
    KnobBoolWPtr includeMeshes;
    KnobIntWPtr frameOffset;
    KnobChoiceWPtr timeMode;
    KnobStringWPtr excludedPaths;  // one excluded archive path per line (driven by AlembicTreeWidget)
    KnobStringWPtr info;

    // Parsed archive. Guarded by archiveMutex: loadAlembicFile (GUI thread via
    // knobChanged/onKnobsLoaded) clears and refills these while the SceneGraph
    // accessors (getMeshDataAt / getEntryWorldMatrix / getSceneNodeAt / ...)
    // read them from render workers and the 3D-viewport paint.
    mutable std::mutex archiveMutex;
    std::vector<ArchiveEntry> entries;
    // Filtered indices into `entries` — what we currently expose to SceneGraph.
    std::vector<int> visible;
    // Source fps detected from the file (0 if unknown/non-uniform).
    double sourceFps;
    std::string loadedFilePath;

    ReadAlembicArchivePrivate() : sourceFps(0.0) {}
};


ReadAlembicArchive::ReadAlembicArchive(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadAlembicArchivePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ReadAlembicArchive::~ReadAlembicArchive()
{
}

std::string
ReadAlembicArchive::getPluginDescription() const
{
    return
#ifdef NATRON_HAVE_ALEMBIC
    tr("One-node import of an entire Alembic archive.\n\n"
       "Walks the .abc file's full hierarchy and exposes every IXform / IPolyMesh "
       "as an entry in Natron's SceneGraph, preserving parent-child relationships "
       "and per-piece animation. Drop this node, point it at a .abc, and the whole "
       "asset (car, character rig, SynthEyes solve, set props) appears in the 3D "
       "pipeline — no manual per-piece wiring.\n\n"
       "Phase A (current build): all entries render as locators (transform gizmos). "
       "Mesh geometry is recognized and entered in the hierarchy but not yet drawn — "
       "Phase B will wire mesh data through.\n\n"
       "Cameras and lights inside the archive are NOT auto-imported by this node — "
       "use ReadAlembicCamera / ReadAlembicLight for those.").toStdString();
#else
    tr("ReadAlembicArchive requires the Alembic library. Rebuild Natron with Alembic support.").toStdString();
#endif
}

void
ReadAlembicArchive::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ReadAlembicArchive::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadAlembicArchive::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ReadAlembicArchive::initializeKnobs()
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
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reload"));
        k->setName("reload");
        k->setHintToolTip(tr("Re-read the Alembic file and refresh the hierarchy."));
        page->addKnob(k);
        _imp->reloadBtn = k;
    }

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Object Filter"));
        k->setName("objectFilter");
        k->setDefaultValue(std::string());
        k->setHintToolTip(tr("Glob pattern matched against each entry's full path. Use '*' for any substring. "
                             "Empty = import everything. Example: '*Trackers*' to import only tracker locators."));
        page->addKnob(k);
        _imp->objectFilter = k;
    }

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Include Meshes"));
        k->setName("includeMeshes");
        k->setDefaultValue(true);
        k->setHintToolTip(tr("If unchecked, mesh entries (IPolyMesh) are skipped — useful for tracker-only imports."));
        page->addKnob(k);
        _imp->includeMeshes = k;
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
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Time Mode"));
        k->setName("timeMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("frame_by_frame", "Frame-by-frame",
            "Each Alembic sample becomes one keyframe at consecutive integer Natron frames (sample 0 -> frame 1, sample 1 -> frame 2, ...). Matches Maya/Blender/Houdini frame-for-frame regardless of fps."));
        opts.push_back(ChoiceOption("time_based", "Time-based",
            "Map Alembic sample times to Natron timeline using the project frame rate. Preserves real-world timing but may interpolate between samples when source and project fps differ."));
        k->populateChoices(opts);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("How Alembic sample times are mapped onto the Natron timeline."));
        page->addKnob(k);
        _imp->timeMode = k;
    }

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Excluded Paths"));
        k->setName("excludedPaths");
        k->setAsMultiLine();
        k->setDefaultValue(std::string());
        k->setHintToolTip(tr("Newline-separated archive paths to exclude from import. "
                             "Normally driven by the tree-view widget below; users can also "
                             "type or paste paths here directly. Empty = nothing excluded."));
        // Hidden from the regular knob layout — the tree widget below is its UI.
        k->setSecret(true);
        page->addKnob(k);
        _imp->excludedPaths = k;
    }

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info");
        k->setAsMultiLine();
        k->setAsLabel();
        k->setHintToolTip(tr("Summary of the loaded archive."));
        page->addKnob(k);
        _imp->info = k;
    }

    // --- Transform: user T/R/S applied to the WHOLE archive ----------------
    // Read in SceneGraph::rebuild and placed on the archive's root SceneNode, so
    // it propagates to every entry via the worldMatrix chain — handy when an
    // archive's geo comes in at the wrong scale. Animatable; composes with any
    // parent Group3D. Read by name (translateX/.../scaleZ + uniformScale).
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X")); k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y")); k->setName("translateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z")); k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate X")); k->setName("rotateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y")); k->setName("rotateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z")); k->setName("rotateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale X")); k->setName("scaleX"); k->setDefaultValue(1.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Y")); k->setName("scaleY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Z")); k->setName("scaleZ"); k->setDefaultValue(1.0); k->setAnimationEnabled(true); xformPage->addKnob(k);
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Uniform Scale"));
        k->setName("uniformScale"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Multiplies all three scale axes — quick way to scale the whole archive up or down."));
        xformPage->addKnob(k);
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
        k->setHintToolTip(tr(
            "Point the Translate/Rotate/Scale knobs pivot around:\n"
            "• Bounding-Box Center — the centre of the archive's geometry, so the gizmo "
            "sits on the object and it rotates in place. Default — works for baked / "
            "world-space archives where the authored top transform is at the origin.\n"
            "• Authored Origin — the archive's own top transform origin (rotates the "
            "archive the way it was authored). Useful when the archive has a meaningful "
            "root transform (e.g. a rigged character).\n"
            "• World Origin — legacy behaviour: pivot around (0,0,0)."));
        xformPage->addKnob(k);
    }

    // --- Display: viewport-only controls for the per-xform locator gizmos ------
    // An archive can carry many xform (null) entries; each draws a small axis +
    // diamond locator in the 3D viewport. These let you hide them or resize them.
    // Read by name in Viewport3D::drawTransformNode (showLocators / locatorSize);
    // viewport-only (setEvaluateOnChange(false)) so changing them never re-renders.
    KnobPagePtr dispPage = AppManager::createKnob<KnobPage>(this, tr("Display"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Show Locators"));
        k->setName("showLocators");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("Draw a small axis + diamond locator at each Alembic xform "
                             "(null) in the 3D viewport. Turn off to declutter archives "
                             "with many transform nodes."));
        dispPage->addKnob(k);
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
        k->setHintToolTip(tr("World-space size of the xform locator gizmos in the 3D "
                             "viewport — a constant on-screen size regardless of any unit "
                             "scale baked into the archive (e.g. an FBX→Alembic cm→m import)."));
        dispPage->addKnob(k);
    }
}

bool
ReadAlembicArchive::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                                ViewSpec /*view*/, double /*time*/,
                                bool /*originatedFromMainThread*/)
{
    if (!k) return false;
    KnobIPtr fpKnob = _imp->filePath.lock();
    KnobIPtr reloadKnob = _imp->reloadBtn.lock();
    KnobIPtr filterKnob = _imp->objectFilter.lock();
    KnobIPtr includeKnob = _imp->includeMeshes.lock();

    KnobIPtr excludedKnob = _imp->excludedPaths.lock();

    const bool isFileChange = (fpKnob && k == fpKnob.get());
    const bool isReload     = (reloadKnob && k == reloadKnob.get());
    const bool isFilter     = (filterKnob && k == filterKnob.get())
                            || (includeKnob && k == includeKnob.get())
                            || (excludedKnob && k == excludedKnob.get());

    if (isFileChange || isReload) {
        if (fpKnob) {
            KnobFile* fp = dynamic_cast<KnobFile*>(fpKnob.get());
            if (fp) {
                loadAlembicFile(fp->getValue());
            }
        }
        // Re-read metadata so getPreferredMetadata picks up the new entries'
        // animation flags (without this, ScanlineRender's cache freezes the
        // render on one frame even when the archive carries animated meshes).
        refreshMetadata_public(true);
        return true;
    }
    if (isFilter) {
        // Re-apply the filter without re-reading the file.
        loadAlembicFile(_imp->loadedFilePath);
        refreshMetadata_public(true);
        return true;
    }
    return false;
}

void
ReadAlembicArchive::onKnobsLoaded()
{
    // After a project load the file-path knob is restored but no knobChanged
    // fires, so the archive is never parsed — the user used to have to hit
    // "Reload". Re-read it here so the saved scene appears immediately.
    KnobFilePtr fp = _imp->filePath.lock();
    if (fp) {
        const std::string path = fp->getValue();
        if (!path.empty()) {
            loadAlembicFile(path);
            refreshMetadata_public(true);
        }
    }
}

void
ReadAlembicArchive::loadAlembicFile(const std::string& path)
{
#ifdef NATRON_HAVE_ALEMBIC
    using namespace Alembic::AbcGeom;
    using namespace Alembic::Abc;

    // Hold the archive lock for the whole (re)load — render threads holding
    // ArchiveEntry references or indexing `visible` must not see the vectors
    // freed/refilled mid-walk.
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);

    _imp->entries.clear();
    _imp->visible.clear();
    _imp->sourceFps = 0.0;

    if (path.empty()) {
        if (_imp->info.lock()) {
            _imp->info.lock()->setValue(std::string("No file selected."));
        }
        return;
    }

    try {
        Alembic::AbcCoreFactory::IFactory factory;
        IArchive archive = factory.getArchive(path);
        if (!archive.valid()) {
            setPersistentMessage(eMessageTypeError, std::string("Failed to open Alembic file: ") + path);
            return;
        }

        IObject top = archive.getTop();
        for (size_t i = 0; i < top.getNumChildren(); ++i) {
            walkObjectRecursive(top.getChild(i), std::string(), -1, _imp->entries);
        }

        // Detect source fps from the first entry that has uniform time sampling.
        for (size_t i = 0; i < _imp->entries.size(); ++i) {
            if (_imp->entries[i].sampleTimes.size() >= 2) {
                const double dt = _imp->entries[i].sampleTimes[1] - _imp->entries[i].sampleTimes[0];
                if (dt > 0.0) {
                    _imp->sourceFps = 1.0 / dt;
                    break;
                }
            }
        }

        // Apply objectFilter + includeMeshes + excludedPaths.
        const std::string pattern = _imp->objectFilter.lock() ? _imp->objectFilter.lock()->getValue() : std::string();
        const bool includeMeshes = _imp->includeMeshes.lock() ? _imp->includeMeshes.lock()->getValue() : true;
        const std::string excludedRaw = _imp->excludedPaths.lock() ? _imp->excludedPaths.lock()->getValue() : std::string();
        // Parse excludedPaths into a set for O(1) lookup. One path per line; ignore blanks.
        std::set<std::string> excluded;
        {
            size_t pos = 0;
            while (pos <= excludedRaw.size()) {
                size_t nl = excludedRaw.find('\n', pos);
                if (nl == std::string::npos) nl = excludedRaw.size();
                std::string line = excludedRaw.substr(pos, nl - pos);
                // Trim trailing CR (Windows line endings) and surrounding whitespace.
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                size_t lead = 0;
                while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
                if (lead) line.erase(0, lead);
                if (!line.empty()) excluded.insert(line);
                if (nl == excludedRaw.size()) break;
                pos = nl + 1;
            }
        }

        for (int i = 0; i < (int)_imp->entries.size(); ++i) {
            const ArchiveEntry& e = _imp->entries[i];
            if (!includeMeshes && e.isMesh) continue;
            if (!globMatch(pattern, e.fullPath)) continue;
            if (excluded.count(e.fullPath)) continue;
            _imp->visible.push_back(i);
        }

        // Build info string.
        size_t numXforms = 0, numMeshes = 0;
        for (size_t i = 0; i < _imp->entries.size(); ++i) {
            if (_imp->entries[i].isMesh) ++numMeshes; else ++numXforms;
        }

        std::ostringstream ss;
        ss << "Total entries: " << _imp->entries.size()
           << " (xforms: " << numXforms
           << ", meshes: " << numMeshes << ")"
           << " | Visible after filter: " << _imp->visible.size();
        if (_imp->sourceFps > 0.0) {
            ss << " | Source FPS: " << _imp->sourceFps;
        }
        const int tm = _imp->timeMode.lock() ? _imp->timeMode.lock()->getValue() : 0;
        ss << " | Time mode: " << (tm == 1 ? "Time-based" : "Frame-by-frame");
        if (_imp->info.lock()) _imp->info.lock()->setValue(ss.str());

        _imp->loadedFilePath = path;
        clearPersistentMessage(false);

    } catch (const std::exception& e) {
        setPersistentMessage(eMessageTypeError, std::string("Error reading Alembic file: ") + e.what());
    }
#else
    Q_UNUSED(path);
    if (_imp->info.lock()) {
        _imp->info.lock()->setValue(std::string("Alembic support not available — rebuild with Alembic library."));
    }
    setPersistentMessage(eMessageTypeError, "Alembic support not available. Rebuild with Alembic library.");
#endif
    // Notify the tree widget (and anyone else interested) that the archive's
    // parsed state has changed.
    Q_EMIT archiveReloaded();
}

int
ReadAlembicArchive::getSceneNodeCount() const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    return (int)_imp->visible.size();
}

std::vector<ArchiveTreeEntry>
ReadAlembicArchive::getEntryTree() const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    std::vector<ArchiveTreeEntry> out;
    out.reserve(_imp->entries.size());
    for (size_t i = 0; i < _imp->entries.size(); ++i) {
        const ArchiveEntry& e = _imp->entries[i];
        ArchiveTreeEntry te;
        te.fullPath = e.fullPath;
        te.name = e.name;
        te.parentLocalIdx = e.parentIndex; // entries[] uses raw indices; tree caller uses same space
        te.isMesh = e.isMesh;
        te.sampleCount = (int)e.sampleMatrices.size();
        te.numVertices = (e.meshData) ? (int)e.meshData->numVertices : 0;
        out.push_back(te);
    }
    return out;
}

MeshDataPtr
ReadAlembicArchive::getMeshDataAt(int idx, double time) const
{
    // Guards against a concurrent reload freeing `entries` under us. NOTE: the
    // in-place vertex mutation documented below is still the G1 copy-on-read
    // issue (wrong-render across concurrent consumers at different times) —
    // this lock only removes the crash class.
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);

    if (idx < 0 || idx >= (int)_imp->visible.size()) return MeshDataPtr();
    ArchiveEntry& e = _imp->entries[_imp->visible[idx]];
    if (!e.isMesh) return MeshDataPtr();

    // Per-frame vertex update for deforming meshes. Mutates e.meshData->vertices
    // in place (same shared_ptr returned across calls — matches ReadGeo's pattern).
    // The time-to-sample mapping mirrors getEntryWorldMatrix below: timeMode 0 is
    // simple per-frame indexing with Frame Offset, timeMode 1 uses the source
    // TimeSampling values to find the nearest sample (handles fps mismatches).
    //
    // Sub-frame interpolation: bracket the requested time between idx0 (floor)
    // and idx1 (ceil), then linearly blend vertices. Without this, motion blur
    // sampling at non-integer times returns snapped data and animated meshes
    // don't blur.
    if (time >= 0.0 && e.hasAnimatedVerts && !e.vertexSamples.empty() && e.meshData) {
        const int N = (int)e.vertexSamples.size();
        const int frameOffset = _imp->frameOffset.lock() ? _imp->frameOffset.lock()->getValue() : 0;
        const int timeMode    = _imp->timeMode.lock()    ? _imp->timeMode.lock()->getValue()    : 0;

        double tCont = 0.0; // continuous sample index (e.g. 49.25)
        if (timeMode == 1 && _imp->sourceFps > 0.0 && (int)e.vertexSampleTimes.size() == N) {
            // Find the bracket [idx0, idx1] whose sample times sandwich abcTimeWanted,
            // then express the wanted time as a fractional index between them.
            const double projectFps = getApp() ? getApp()->getProjectFrameRate() : 24.0;
            const double abcTimeWanted = (time - 1.0 - frameOffset) / projectFps;
            int idx0 = 0;
            for (int i = 0; i + 1 < N; ++i) {
                if (e.vertexSampleTimes[i] <= abcTimeWanted &&
                    abcTimeWanted <= e.vertexSampleTimes[i + 1]) {
                    idx0 = i; break;
                }
                if (abcTimeWanted < e.vertexSampleTimes[i]) break;
                idx0 = i;
            }
            const int idx1 = std::min(idx0 + 1, N - 1);
            const double t0 = e.vertexSampleTimes[idx0];
            const double t1 = e.vertexSampleTimes[idx1];
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

        const int    idx0  = (int)std::floor(tCont);
        const int    idx1  = std::min(idx0 + 1, N - 1);
        const double alpha = tCont - (double)idx0;
        const std::vector<float>& s0 = e.vertexSamples[idx0];
        const std::vector<float>& s1 = e.vertexSamples[idx1];

        if (!s0.empty() && s0.size() == e.meshData->vertices.size()) {
            if (idx0 == idx1 || alpha < 1e-9) {
                // Exact-sample fast path (and the only correct path if the
                // upper bracket sample size mismatches the lower).
                std::memcpy(e.meshData->vertices.data(), s0.data(),
                            s0.size() * sizeof(float));
            } else if (s1.size() == s0.size()) {
                const float a = (float)alpha;
                const float oneMinusA = 1.0f - a;
                float* dst = e.meshData->vertices.data();
                const float* src0 = s0.data();
                const float* src1 = s1.data();
                const size_t n = s0.size();
                for (size_t k = 0; k < n; ++k) {
                    dst[k] = src0[k] * oneMinusA + src1[k] * a;
                }
            } else {
                // Topology mismatch between brackets (shouldn't happen for
                // standard exports). Fall back to lower sample.
                std::memcpy(e.meshData->vertices.data(), s0.data(),
                            s0.size() * sizeof(float));
            }
        }
    }

    return e.meshData;
}

bool
ReadAlembicArchive::getEntryWorldMatrix(int idx, double time, float outWorld[16]) const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    if (idx < 0 || idx >= (int)_imp->visible.size()) return false;

    // Identity to start.
    for (int i = 0; i < 16; ++i) outWorld[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    // Walk parent chain from entry → root, collect entries in order
    // (root → ... → this), then compose: world = root.local * ... * this.local.
    std::vector<int> chain;
    int cur = _imp->visible[idx];
    while (cur >= 0) {
        chain.push_back(cur);
        cur = _imp->entries[cur].parentIndex;
    }

    // Compose root-down. Initial outWorld is identity.
    for (int ci = (int)chain.size() - 1; ci >= 0; --ci) {
        const int entryIdx = chain[ci];

        // Reuse the same time → sample-index logic as getSceneNodeAt by faking
        // a fresh getSceneNodeAt call against THIS entry. But this entry may
        // not be in `visible` (parents that got filtered out). Compute the
        // local matrix directly.
        float local[16];
        for (int i = 0; i < 16; ++i) local[i] = (i % 5 == 0) ? 1.0f : 0.0f;

        const ArchiveEntry& e = _imp->entries[entryIdx];
        if (!e.sampleMatrices.empty()) {
            const int frameOffset = _imp->frameOffset.lock() ? _imp->frameOffset.lock()->getValue() : 0;
            const int timeMode    = _imp->timeMode.lock()    ? _imp->timeMode.lock()->getValue()    : 0;
            const int N = (int)e.sampleMatrices.size();

            // Sub-frame interpolation: continuous sample index, then lerp
            // matrix components between brackets. Linear matrix interp isn't
            // strictly correct for large rotations (would need slerp on the
            // rotation block), but for the small sub-frame deltas motion blur
            // uses (~1/N of one frame), the error is negligible — and the
            // alternative is animated xforms not blurring at all.
            double tCont = 0.0;
            if (timeMode == 1 && _imp->sourceFps > 0.0 && (int)e.sampleTimes.size() == N) {
                const double projectFps = getApp() ? getApp()->getProjectFrameRate() : 24.0;
                const double abcTimeWanted = (time - 1.0 - frameOffset) / projectFps;
                int idx0 = 0;
                for (int i = 0; i + 1 < N; ++i) {
                    if (e.sampleTimes[i] <= abcTimeWanted &&
                        abcTimeWanted <= e.sampleTimes[i + 1]) {
                        idx0 = i; break;
                    }
                    if (abcTimeWanted < e.sampleTimes[i]) break;
                    idx0 = i;
                }
                const int idx1 = std::min(idx0 + 1, N - 1);
                const double t0 = e.sampleTimes[idx0];
                const double t1 = e.sampleTimes[idx1];
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

            const int    idx0  = (int)std::floor(tCont);
            const int    idx1  = std::min(idx0 + 1, N - 1);
            const double alpha = tCont - (double)idx0;
            const Imath::M44d& m0 = e.sampleMatrices[idx0];
            const Imath::M44d& m1 = e.sampleMatrices[idx1];
            if (idx0 == idx1 || alpha < 1e-9) {
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        local[c * 4 + r] = (float)m0[c][r];
            } else {
                const double oneMinusA = 1.0 - alpha;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        local[c * 4 + r] = (float)(m0[c][r] * oneMinusA + m1[c][r] * alpha);
            }
        }

        // outWorld = outWorld * local  (column-major matrix multiplication)
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
    return true;
}

bool
ReadAlembicArchive::getSceneNodeAt(int idx, double time,
                                   std::string& outName,
                                   int& outParentLocalIndex,
                                   bool& outIsMesh,
                                   float outLocalMatrix[16]) const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    if (idx < 0 || idx >= (int)_imp->visible.size()) return false;
    const int srcIdx = _imp->visible[idx];
    const ArchiveEntry& e = _imp->entries[srcIdx];

    outName = e.fullPath;
    outIsMesh = e.isMesh;

    // Translate parentIndex from "index into entries[]" to "index into visible[]".
    outParentLocalIndex = -1;
    if (e.parentIndex >= 0) {
        for (int v = 0; v < (int)_imp->visible.size(); ++v) {
            if (_imp->visible[v] == e.parentIndex) { outParentLocalIndex = v; break; }
        }
        // If parent was filtered out, this entry attaches to the archive root (-1).
    }

    // Identity as a safe default.
    std::memset(outLocalMatrix, 0, 16 * sizeof(float));
    outLocalMatrix[0] = outLocalMatrix[5] = outLocalMatrix[10] = outLocalMatrix[15] = 1.0f;

    if (e.sampleMatrices.empty()) {
        return true;
    }

    // Map Natron time to sample index per the active time mode.
    const int frameOffset = _imp->frameOffset.lock() ? _imp->frameOffset.lock()->getValue() : 0;
    const int timeMode    = _imp->timeMode.lock()    ? _imp->timeMode.lock()->getValue()    : 0;
    const int N = (int)e.sampleMatrices.size();

    int sampleIdx = 0;
    if (timeMode == 1 && _imp->sourceFps > 0.0) {
        // Time-based: locate the sample whose abcTime is closest to (Natron seconds at this time).
        const double projectFps = getApp() ? getApp()->getProjectFrameRate() : 24.0;
        const double abcTimeWanted = (time - 1.0 - frameOffset) / projectFps;
        // Linear search — N is typically small (samples per archive entry).
        double bestDelta = 1e18;
        for (int i = 0; i < N; ++i) {
            const double d = std::abs(e.sampleTimes[i] - abcTimeWanted);
            if (d < bestDelta) { bestDelta = d; sampleIdx = i; }
        }
    } else {
        // Frame-by-frame: sample 0 lives at frame 1+frameOffset.
        sampleIdx = (int)std::floor(time - 1.0 - frameOffset + 0.5);
        if (sampleIdx < 0) sampleIdx = 0;
        if (sampleIdx >= N) sampleIdx = N - 1;
    }

    // Convert Imath::M44d → column-major float[16].
    // Imath stores M_col with row-major memory, where m_imath[i][j] = M_col[j][i].
    // OpenGL column-major: out[col*4 + row] = M_col[row][col] = m_imath[col][row].
    const Imath::M44d& m = e.sampleMatrices[sampleIdx];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            outLocalMatrix[c * 4 + r] = (float)m[c][r];
        }
    }
    return true;
}

StatusEnum
ReadAlembicArchive::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                          ViewIdx /*view*/, RectD* rod)
{
    // Generator node — 1x1 dummy RoD; the real output goes through SceneGraph.
    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = 1;
    rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadAlembicArchive::getPreferredMetadata(NodeMetadata& metadata)
{
    // Declare frame-varying when any archive entry carries animated content
    // (multi-sample xform OR multi-sample mesh verts). Without this, Scene3D
    // doesn't see the time-varying upstream and ScanlineRender's cache pins
    // its output to one frame even though the geometry actually animates.
    // CyclesRender sets frame-varying on its own node so it never had this
    // problem; the 3D viewport reads getMeshDataAt(idx, time) directly each
    // paintGL so it bypasses the cache entirely.
    bool anyAnimated = false;
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    for (const ArchiveEntry& e : _imp->entries) {
        if (e.isMesh) {
            if (e.hasAnimatedVerts) { anyAnimated = true; break; }
        } else {
            if (e.sampleMatrices.size() > 1) { anyAnimated = true; break; }
        }
    }
    if (anyAnimated) {
        metadata.setIsFrameVarying(true);
    }
    return eStatusOK;
}

StatusEnum
ReadAlembicArchive::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

// --- Inputs ---

std::string
ReadAlembicArchive::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "Material";
    if (inputNb == 1) return "Image";
    return std::string();
}

// --- MaterialProvider interface ---
//
// The archive forwards every material query to the Material3D wired into
// input 0 (if any). When no material is connected, returns Principled-BSDF
// defaults so Cycles' default shader path produces a sensible flat-gray render.
// Every emitted archive mesh inherits this same material; per-mesh assignment
// is a separate Plan B/C ticket.

void
ReadAlembicArchive::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) {
        mat->getMaterialBaseColor(time, r, g, b);
        return;
    }
    r = 0.7; g = 0.7; b = 0.7;
}

double
ReadAlembicArchive::getMaterialRoughness(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialRoughness(time);
    return 0.5;
}

double
ReadAlembicArchive::getMaterialMetallic(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialMetallic(time);
    return 0.0;
}

double
ReadAlembicArchive::getMaterialSpecular(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialSpecular(time);
    return 0.5;
}

void
ReadAlembicArchive::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) {
        mat->getMaterialEmission(time, r, g, b, strength);
        return;
    }
    r = 1.0; g = 1.0; b = 1.0; strength = 0.0;
}

double
ReadAlembicArchive::getMaterialTransmission(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialTransmission(time);
    return 0.0;
}

double
ReadAlembicArchive::getMaterialIOR(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialIOR(time);
    return 1.45;
}

std::string
ReadAlembicArchive::getMaterialTextureFile() const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialTextureFile();
    return std::string();
}

std::string
ReadAlembicArchive::getMaterialDiffuseColorspace() const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialDiffuseColorspace();
    return "sRGB";
}

bool
ReadAlembicArchive::hasMaterialInput() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}

MaterialProvider*
ReadAlembicArchive::getConnectedMaterial() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT

#include "moc_ReadAlembicArchive.cpp"
