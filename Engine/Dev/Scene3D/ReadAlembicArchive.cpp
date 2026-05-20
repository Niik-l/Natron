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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <set>
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
    // Phase B reads sample 0 only (rest-pose); animated topology is deferred.
    MeshDataPtr meshData;

    ArchiveEntry() : parentIndex(-1), isMesh(false) {}
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
        // Read sample 0 of the mesh data (rest pose). The parent IXform's
        // animation drives positioning via the SceneGraph parent chain.
        IPolyMesh polyMesh(obj, kWrapExisting);
        ArchiveEntry e;
        e.fullPath = fullPath;
        e.name = obj.getName();
        e.parentIndex = parentEntryIndex;
        e.isMesh = true;
        e.meshData = readPolyMeshSample0(polyMesh);
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

    // Parsed archive
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
        return true;
    }
    if (isFilter) {
        // Re-apply the filter without re-reading the file.
        loadAlembicFile(_imp->loadedFilePath);
        return true;
    }
    return false;
}

void
ReadAlembicArchive::loadAlembicFile(const std::string& path)
{
#ifdef NATRON_HAVE_ALEMBIC
    using namespace Alembic::AbcGeom;
    using namespace Alembic::Abc;

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
    return (int)_imp->visible.size();
}

std::vector<ArchiveTreeEntry>
ReadAlembicArchive::getEntryTree() const
{
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
ReadAlembicArchive::getMeshDataAt(int idx) const
{
    if (idx < 0 || idx >= (int)_imp->visible.size()) return MeshDataPtr();
    const ArchiveEntry& e = _imp->entries[_imp->visible[idx]];
    if (!e.isMesh) return MeshDataPtr();
    return e.meshData;
}

bool
ReadAlembicArchive::getEntryWorldMatrix(int idx, double time, float outWorld[16]) const
{
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
            int sampleIdx = 0;
            if (timeMode == 1 && _imp->sourceFps > 0.0) {
                const double projectFps = getApp() ? getApp()->getProjectFrameRate() : 24.0;
                const double abcTimeWanted = (time - 1.0 - frameOffset) / projectFps;
                double bestDelta = 1e18;
                for (int i = 0; i < N; ++i) {
                    const double d = std::abs(e.sampleTimes[i] - abcTimeWanted);
                    if (d < bestDelta) { bestDelta = d; sampleIdx = i; }
                }
            } else {
                sampleIdx = (int)std::floor(time - 1.0 - frameOffset + 0.5);
                if (sampleIdx < 0) sampleIdx = 0;
                if (sampleIdx >= N) sampleIdx = N - 1;
            }
            const Imath::M44d& m = e.sampleMatrices[sampleIdx];
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    local[c * 4 + r] = (float)m[c][r];
                }
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
    EffectInstancePtr inp = getInput(0);
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}

MaterialProvider*
ReadAlembicArchive::getConnectedMaterial() const
{
    EffectInstancePtr inp = getInput(0);
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT

#include "moc_ReadAlembicArchive.cpp"
