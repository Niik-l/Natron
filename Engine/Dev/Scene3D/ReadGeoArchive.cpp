/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

// ReadGeoArchive node implementation. Structure mirrors ReadAlembicArchive
// (same knobs, same ArchiveProvider multi-emit contract); the per-frame data
// comes from the ReadGeoImport archive contract shared with ReadGeo:
//
//   load     : GeoArchiveRuntime::loadFile (file -> parsed archive + flags)
//   per frame: GeoArchiveRuntime::ensureFrameState
//              -> GeoArchiveFrameEval (node local matrices + deformed meshes)
//
// Frame cache: keyed by the integer Natron frame offset from frame 1
// (k = floor(time) - 1 - frameOffset; sourceTime = k / fps). Filled lazily
// on first request per frame, kept for the session (the pre-load spirit of
// ReadGeo's Alembic sample cache, without paying for unvisited frames).
// Sub-frame sampling (motion blur): node matrices interpolate linearly
// between the cached bracket frames — the same matrix-lerp policy
// ReadAlembicArchive uses; skinned vertices snap per frame (G1 in-place
// caveat of the Alembic machinery applies; tracked separately).
// The runtime + locking discipline live in ReadGeoImport.h (GeoArchiveRuntime).

#include "ReadGeoArchive.h"
#include "../DotUtils.h"

#include "../../NodeMetadata.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
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

NATRON_NAMESPACE_ENTER

namespace {

// Simple glob match: '*' matches any substring (same as ReadAlembicArchive's).
bool
arcGlobMatch(const std::string& pattern, const std::string& text)
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

} // anonymous namespace

struct ReadGeoArchivePrivate
{
    KnobFileWPtr filePath;
    KnobButtonWPtr reloadBtn;
    KnobStringWPtr objectFilter;
    KnobBoolWPtr includeMeshes;
    KnobBoolWPtr reverseNormals;
    KnobBoolWPtr bakeTransforms;
    KnobIntWPtr frameOffset;
    KnobDoubleWPtr fps;
    KnobStringWPtr excludedPaths;
    KnobStringWPtr info;

    // Parsed archive + frame cache. rt (and the visible list) are guarded by
    // archiveMutex: loadArchiveFile (GUI thread) clears and refills while the
    // SceneGraph accessors read from render workers and the viewport paint.
    mutable std::mutex archiveMutex;
    GeoArchiveRuntime rt;
    std::vector<int> visible;                // entry indices exposed to SceneGraph
    std::string loadedFilePath;

    int frameOffsetKnobValue() const
    {
        return frameOffset.lock() ? frameOffset.lock()->getValue() : 0;
    }
};

// --- construction / description ---------------------------------------------

// FPS used for time mapping: the knob, else the project frame rate.
double
ReadGeoArchive::archiveFps() const
{
    const double fpsKnob = _imp->fps.lock() ? _imp->fps.lock()->getValue() : 0.0;
    return fpsKnob > 0.0 ? fpsKnob : (getApp() ? getApp()->getProjectFrameRate() : 24.0);
}

ReadGeoArchive::ReadGeoArchive(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadGeoArchivePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ReadGeoArchive::~ReadGeoArchive()
{
}

std::string
ReadGeoArchive::getPluginDescription() const
{
    return tr(
        "One-node import of a whole scene-format file: .glb/.gltf (glTF 2.0) and "
        ".fbx (binary + ASCII).\n\n"
        "Walks the file's full hierarchy and exposes every node / mesh as an "
        "entry in Natron's SceneGraph, preserving parent-child relationships. "
        "Node transforms and skinned vertices animate on the timeline (frame "
        "cache; rigid transforms interpolate for motion blur, skinned vertices "
        "snap per frame). The file's first PBR material is offered through the "
        "material interface; wire a Material3D into input 0 to override it for "
        "the whole archive.\n\n"
        "Animation knobs: Frame Offset + FPS (0 = project frame rate), and "
        "Bake Transforms — bakes every mesh entry's evaluated world transform "
        "into its vertices so the parts emit in world space (the assembled "
        "look, independent of the node chain).\n\n"
        "The settings panel shows a checkable tree of the archive's entries; "
        "unchecked entries are excluded from the scene.\n\n"
        "Morph targets / blend shapes and per-entry material assignment from "
        "the file are not supported yet.").toStdString();
}

void
ReadGeoArchive::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ReadGeoArchive::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadGeoArchive::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// --- knobs -------------------------------------------------------------------

void
ReadGeoArchive::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("File"));

    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("File"));
        k->setName("filename");
        k->setHintToolTip(tr("Path to a .glb/.gltf (glTF) or .fbx scene file."));
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->filePath = k;
    }

    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reload"));
        k->setName("reload");
        k->setHintToolTip(tr("Re-read the file and refresh the hierarchy."));
        page->addKnob(k);
        _imp->reloadBtn = k;
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
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Reverse Normals"));
        k->setName("reverseNormals");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Flip the face winding of every mesh in the archive so "
                             "normals point the other way. Use the 3D viewport's Face "
                             "Orientation shading mode to check: blue = facing you, "
                             "red = facing away."));
        page->addKnob(k);
        _imp->reverseNormals = k;
    }

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Bake Transforms"));
        k->setName("bakeTransforms");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Bake every mesh entry's evaluated world transform into its "
                             "vertices, so each part emits in world space attached to the "
                             "archive root — the assembled look, independent of the node "
                             "chain. Use this when a file's hierarchy does not compose "
                             "correctly, or when you just want the object as authored. "
                             "The transform/locator entries still show in the tree and "
                             "viewport, the file's animation is baked per frame, and the "
                             "node's own Transform page still applies on top. Rigid "
                             "sub-frame motion blur snaps per frame while baked."));
        page->addKnob(k);
        _imp->bakeTransforms = k;
    }

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Offset"));
        k->setName("frameOffset");
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Offset the file's animation relative to the Natron timeline."));
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
                             "second of source animation."));
        page->addKnob(k);
        _imp->fps = k;
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
        // Compact read-only display: single-line LABEL mode renders the value
        // as plain text in the row's label column — no field box, no reserved
        // vertical space (multi-line mode is an expanding QTextEdit and ate
        // the panel).
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info");
        k->setAsLabel();
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setIsPersistent(false);
        k->setHintToolTip(tr("Summary of the loaded file."));
        page->addKnob(k);
        _imp->info = k;
    }

    // --- Transform: user T/R/S applied to the WHOLE archive ----------------
    // Read in SceneGraph::rebuild and placed on the archive's root SceneNode,
    // so it propagates to every entry via the worldMatrix chain (same as
    // ReadAlembicArchive; read by name).
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
        k->setHintToolTip(tr("Point the Translate/Rotate/Scale knobs pivot around."));
        xformPage->addKnob(k);
    }

    // --- Display: viewport-only controls for locator gizmos ----------------
    // Read by name in Viewport3D::drawTransformNode (showLocators / locatorSize).
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
    }
}

bool
ReadGeoArchive::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                            ViewSpec /*view*/, double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    if (!k) return false;
    KnobIPtr fpKnob = _imp->filePath.lock();
    KnobIPtr reloadKnob = _imp->reloadBtn.lock();
    KnobIPtr filterKnob = _imp->objectFilter.lock();
    KnobIPtr includeKnob = _imp->includeMeshes.lock();
    KnobIPtr excludedKnob = _imp->excludedPaths.lock();
    KnobIPtr revNormKnob = _imp->reverseNormals.lock();
    KnobIPtr bakeKnob = _imp->bakeTransforms.lock();

    const bool isFileChange = (fpKnob && k == fpKnob.get());
    const bool isReload     = (reloadKnob && k == reloadKnob.get());
    const bool isFilter     = (filterKnob && k == filterKnob.get())
                            || (includeKnob && k == includeKnob.get())
                            || (excludedKnob && k == excludedKnob.get())
                            || (revNormKnob && k == revNormKnob.get());

    // Bake Transforms: flip the runtime flag and drop the cached frame
    // evaluations (no re-parse — the baked payloads are derived from them).
    if (bakeKnob && k == bakeKnob.get()) {
        {
            std::lock_guard<std::mutex> lk(_imp->archiveMutex);
            KnobBoolPtr bk = _imp->bakeTransforms.lock();
            _imp->rt.bakeMeshWorld = bk ? bk->getValue() : false;
            _imp->rt.clearFrameCache();
        }
        return true;
    }

    if (isFileChange || isReload) {
        if (fpKnob) {
            KnobFile* fp = dynamic_cast<KnobFile*>(fpKnob.get());
            if (fp) {
                loadArchiveFile(fp->getValue());
            }
        }
        // Re-read metadata so getPreferredMetadata picks up the new entries'
        // animation flags (ScanlineRender's cache would otherwise freeze).
        refreshMetadata_public(true);
        return true;
    }
    if (isFilter) {
        // Re-apply the filter: re-parse from the loaded path (cheap relative
        // to a file read; keeps applied state == knob state).
        loadArchiveFile(_imp->loadedFilePath);
        refreshMetadata_public(true);
        return true;
    }
    return false;
}

void
ReadGeoArchive::onKnobsLoaded()
{
    KnobFilePtr fp = _imp->filePath.lock();
    if (fp) {
        const std::string path = fp->getValue();
        if (!path.empty()) {
            loadArchiveFile(path);
            refreshMetadata_public(true);
        }
    }
}

// --- loading -----------------------------------------------------------------

void
ReadGeoArchive::loadArchiveFile(const std::string& path)
{
    // Hold the archive lock for the whole (re)load — render threads holding
    // entries / frame states must not see them freed mid-walk.
    std::unique_lock<std::mutex> lk(_imp->archiveMutex);

    // Drop the parsed archive + frame cache first.
    _imp->visible.clear();
    _imp->rt.clear();

    if (path.empty()) {
        if (_imp->info.lock()) {
            _imp->info.lock()->setValue(std::string("No file selected."));
        }
        return;
    }

    // Parse. The runtime reports unsupported extensions generically; keep the
    // archive-specific hint for .abc/.obj files here.
    {
        std::string ext;
        {
            const std::size_t dot = path.find_last_of('.');
            if (dot != std::string::npos) {
                ext = path.substr(dot);
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c){ return (char)std::tolower(c); });
            }
        }
        if (ext == ".abc" || ext == ".obj") {
            setPersistentMessage(eMessageTypeError,
                ext == ".abc"
                    ? "Use ReadAlembicArchive for .abc files."
                    : "Use ReadGeo for .obj files.");
            return;
        }
    }

    const bool reverse = _imp->reverseNormals.lock() && _imp->reverseNormals.lock()->getValue();
    std::string err;
    if (!_imp->rt.loadFile(path, reverse, err)) {
        setPersistentMessage(eMessageTypeError, err);
        return;
    }
    _imp->rt.bakeMeshWorld = _imp->bakeTransforms.lock() &&
                             _imp->bakeTransforms.lock()->getValue();
    const GeoArchiveResult& data = _imp->rt.data;

    // Apply objectFilter + includeMeshes + excludedPaths (same rules as
    // ReadAlembicArchive), with SUBTREE semantics: an entry is visible only
    // if it passes the filter AND its parent entry is visible — unchecking a
    // transform in the tree (or filtering it out) hides its whole subtree,
    // instead of leaving children to detach (they used to re-attach to the
    // archive root and render at file-unit scale — off-screen or giant).
    // Entries are parents-before-children, so one pass suffices.
    const std::string pattern = _imp->objectFilter.lock() ? _imp->objectFilter.lock()->getValue() : std::string();
    const bool includeMeshes = _imp->includeMeshes.lock() ? _imp->includeMeshes.lock()->getValue() : true;
    const std::string excludedRaw = _imp->excludedPaths.lock() ? _imp->excludedPaths.lock()->getValue() : std::string();
    std::set<std::string> excluded;
    {
        std::size_t pos = 0;
        while (pos <= excludedRaw.size()) {
            std::size_t nl = excludedRaw.find('\n', pos);
            if (nl == std::string::npos) nl = excludedRaw.size();
            std::string line = excludedRaw.substr(pos, nl - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
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
        if (pass && !arcGlobMatch(pattern, nd.fullPath)) pass = false;
        if (pass && excluded.count(nd.fullPath)) pass = false;
        if (pass && nd.parentIndex >= 0) pass = entryVisible[nd.parentIndex] != 0;
        entryVisible[i] = pass ? 1 : 0;
        if (pass) _imp->visible.push_back(i);
    }

    // Info string — compact single line (label-column display).
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
    if (_imp->rt.hasAnimation) {
        const double dur = _imp->rt.animTimeEnd - _imp->rt.animTimeBegin;
        if (dur > 0.0) {
            const double fpsKnob = _imp->fps.lock() ? _imp->fps.lock()->getValue() : 0.0;
            const double fps = fpsKnob > 0.0 ? fpsKnob
                                             : (getApp() ? getApp()->getProjectFrameRate() : 24.0);
            ss << " | ~" << (int)(dur * fps) << "f anim";
        } else {
            ss << " | anim";
        }
    }
    if (_imp->rt.bakeMeshWorld) {
        ss << " | baked";
    }
    if (_imp->info.lock()) _imp->info.lock()->setValue(ss.str());

    _imp->loadedFilePath = path;
    clearPersistentMessage(false);

    // Release BEFORE the emit: settings-panel slots run synchronously off
    // archiveReloaded and re-enter getEntryTree(), which takes this same
    // non-recursive mutex (self-deadlock guard, same as ReadAlembicArchive).
    lk.unlock();
    Q_EMIT archiveReloaded();
}

// --- ArchiveProvider interface -----------------------------------------------

int
ReadGeoArchive::getSceneNodeCount() const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    return (int)_imp->visible.size();
}

std::vector<ArchiveTreeEntry>
ReadGeoArchive::getEntryTree() const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    const GeoArchiveResult& data = _imp->rt.data;
    std::vector<ArchiveTreeEntry> out;
    out.reserve(_imp->rt.data.nodes.size());
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        const GeoArchiveNodeData& nd = data.nodes[i];
        ArchiveTreeEntry te;
        te.fullPath = nd.fullPath;
        te.name = nd.name;
        te.parentLocalIdx = nd.parentIndex;
        te.isMesh = nd.isMesh;
        te.sampleCount = (nd.chanTr >= 0 || nd.chanRot >= 0 || nd.chanSc >= 0) ? 1 : 0;
        te.numVertices = (nd.isMesh && nd.meshIndex >= 0 &&
                          nd.meshIndex < (int)data.meshes.size() &&
                          data.meshes[nd.meshIndex].mesh)
                             ? (int)data.meshes[nd.meshIndex].mesh->numVertices : 0;
        out.push_back(te);
    }
    return out;
}

bool
ReadGeoArchive::getSceneNodeAt(int idx, double time,
                               std::string& outName,
                               int& outParentLocalIndex,
                               bool& outIsMesh,
                               float outLocalMatrix[16]) const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    if (idx < 0 || idx >= (int)_imp->visible.size()) return false;
    const int srcIdx = _imp->visible[idx];
    const GeoArchiveNodeData& nd = _imp->rt.data.nodes[srcIdx];

    outName = nd.fullPath;
    outIsMesh = nd.isMesh;

    // Baked mesh entries emit in world space attached to the archive root —
    // the world transform went into the vertices (the baked payload), so the
    // chain must not apply again.
    if (_imp->rt.bakeMeshWorld && nd.isMesh) {
        outParentLocalIndex = -1;
        geoArchiveMatIdentity(outLocalMatrix);
        return true;
    }
    const double fps = archiveFps();
    const int frameOffset = _imp->frameOffsetKnobValue();

    // The entry's own local matrix.
    _imp->rt.nodeLocalAt(srcIdx, time, fps, frameOffset, outLocalMatrix);

    // Parent mapping + root normalization. The result must satisfy:
    // SceneGraph compose (root SceneNode identity × this local) ==
    // rootMatrix × (full parent chain × own local).
    // - The entry's own local is file-space; a root-level entry folds the
    //   format-level root normalization (the ufbx unit/axes matrix) into its
    //   local so every chain composes it exactly once at the top — matching
    //   worldMatrixAt / the bake post-pass, which compose it at their chain
    //   tops. Deep entries stay file-space; the (true column-major)
    //   SceneGraph product converts their translations through the folded
    //   parent chain.
    // - When the direct parent is filtered out (glob / excludedPaths; the
    //   tree's subtree rule already hides such children), attach to the
    //   nearest VISIBLE ancestor and fold every filtered intermediate's
    //   local into this entry's local. If the walk reaches the archive root,
    //   the root normalization composes here too (the visible ancestors'
    //   chain no longer carries it for this entry).
    outParentLocalIndex = -1;
    bool attachRoot = (nd.parentIndex < 0);
    int p = nd.parentIndex;
    while (p >= 0 && p < (int)_imp->rt.data.nodes.size()) {
        int pv = -1;
        for (int v = 0; v < (int)_imp->visible.size(); ++v) {
            if (_imp->visible[v] == p) { pv = v; break; }
        }
        if (pv >= 0) {
            outParentLocalIndex = pv;
            break;
        }
        float pl[16];
        _imp->rt.nodeLocalAt(p, time, fps, frameOffset, pl);
        float folded[16];
        geoArchiveMatMul(pl, outLocalMatrix, folded);
        std::memcpy(outLocalMatrix, folded, 16 * sizeof(float));
        attachRoot = true;
        p = _imp->rt.data.nodes[p].parentIndex;
    }
    if (attachRoot && _imp->rt.data.rootMatrix.size() == 16) {
        float folded[16];
        geoArchiveMatMul(_imp->rt.data.rootMatrix.data(), outLocalMatrix, folded);
        std::memcpy(outLocalMatrix, folded, 16 * sizeof(float));
    }
    return true;
}

MeshDataPtr
ReadGeoArchive::getMeshDataAt(int idx, double time) const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    if (idx < 0 || idx >= (int)_imp->visible.size()) return MeshDataPtr();
    const int srcIdx = _imp->visible[idx];
    const GeoArchiveNodeData& nd = _imp->rt.data.nodes[srcIdx];
    if (!nd.isMesh || nd.meshIndex < 0 || nd.meshIndex >= (int)_imp->rt.data.meshes.size()) {
        return MeshDataPtr();
    }
    const GeoArchiveMeshData& md = _imp->rt.data.meshes[nd.meshIndex];
    if (!md.mesh) return MeshDataPtr();

    // Rigid meshes: the node's own transform animates; vertices are static —
    // except when baking, when the frame state holds the world-baked payload.
    if (!md.mesh || (md.skinIndex < 0 && !_imp->rt.bakeMeshWorld) || time < 0.0) {
        return md.mesh;
    }

    // Skinned: per-frame CPU skinning via the frame cache. Snaps per frame
    // (the sub-frame motion-blur interpolation the Alembic machinery does on
    // vertex samples is not applied to skinning yet — documented limitation).
    const int frameOffset = _imp->frameOffsetKnobValue();
    int key = (int)std::floor(time - 1.0 - (double)frameOffset);
    if (key < 0) key = 0;
    std::shared_ptr<const GeoArchiveFrameEval> st =
        _imp->rt.ensureFrameState(archiveFps(), key);
    if (nd.meshIndex < (int)st->deformed.size() && st->deformed[nd.meshIndex]) {
        return st->deformed[nd.meshIndex];
    }
    return md.mesh;
}

bool
ReadGeoArchive::getEntryWorldMatrix(int idx, double time, float outWorld[16]) const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    if (idx < 0 || idx >= (int)_imp->visible.size()) return false;

    // Baked mesh entries are world-space payloads parented to the root —
    // their entry world matrix is identity by construction.
    if (_imp->rt.bakeMeshWorld && _imp->rt.data.nodes[_imp->visible[idx]].isMesh) {
        geoArchiveMatIdentity(outWorld);
        return true;
    }

    _imp->rt.worldMatrixAt(_imp->visible[idx], time, archiveFps(),
                           _imp->frameOffsetKnobValue(), outWorld);
    return true;
}

bool
ReadGeoArchive::getRootMatrix(float outRoot[16]) const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    if (_imp->rt.data.rootMatrix.size() != 16) {
        geoArchiveMatIdentity(outRoot);
        return false;
    }
    std::memcpy(outRoot, _imp->rt.data.rootMatrix.data(), 16 * sizeof(float));
    return true;
}

// --- EffectInstance overrides ------------------------------------------------

StatusEnum
ReadGeoArchive::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
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
ReadGeoArchive::getPreferredMetadata(NodeMetadata& metadata)
{
    // Declare frame-varying when any entry carries animation (node channels,
    // skinned payload, or an FBX animation stack). Required so Scene3D +
    // ScanlineRender hashes change per frame — same reason as ReadGeo /
    // ReadAlembicArchive.
    if (_imp && _imp->rt.hasAnimation) {
        metadata.setIsFrameVarying(true);
    }
    return eStatusOK;
}

StatusEnum
ReadGeoArchive::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

// --- Inputs / MaterialProvider -------------------------------------------------

std::string
ReadGeoArchive::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "Material";
    if (inputNb == 1) return "Image";
    return std::string();
}

// The archive forwards every material query to the Material3D wired into
// input 0 (if any). When none is connected, the FIRST material authored in
// the file is offered — so an imported GLB/FBX renders with its authored
// look without extra wiring. Per-entry materials need SceneNode-level
// material plumbing (GeoMaterialOverride covers it today).

void
ReadGeoArchive::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
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
}

double
ReadGeoArchive::getMaterialRoughness(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialRoughness(time);
    if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
        return _imp->rt.data.materials[0].roughness;
    }
    return 0.5;
}

double
ReadGeoArchive::getMaterialMetallic(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialMetallic(time);
    if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
        return _imp->rt.data.materials[0].metallic;
    }
    return 0.0;
}

double
ReadGeoArchive::getMaterialSpecular(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialSpecular(time);
    if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
        return _imp->rt.data.materials[0].specular;
    }
    return 0.5;
}

void
ReadGeoArchive::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
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
}

double
ReadGeoArchive::getMaterialTransmission(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialTransmission(time);
    if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
        return _imp->rt.data.materials[0].transmission;
    }
    return 0.0;
}

double
ReadGeoArchive::getMaterialIOR(double time) const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialIOR(time);
    if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
        return _imp->rt.data.materials[0].ior;
    }
    return 1.45;
}

std::string
ReadGeoArchive::getMaterialTextureFile() const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialTextureFile();
    if (_imp && !_imp->rt.data.materials.empty() && _imp->rt.data.materials[0].present) {
        return _imp->rt.data.materials[0].diffuseMapPath;
    }
    return std::string();
}

std::string
ReadGeoArchive::getMaterialDiffuseColorspace() const
{
    if (MaterialProvider* mat = getConnectedMaterial()) return mat->getMaterialDiffuseColorspace();
    return "sRGB";
}

bool
ReadGeoArchive::hasMaterialInput() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}

MaterialProvider*
ReadGeoArchive::getConnectedMaterial() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT

#include "moc_ReadGeoArchive.cpp"
