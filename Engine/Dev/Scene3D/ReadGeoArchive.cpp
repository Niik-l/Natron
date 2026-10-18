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
#include "../../NodeSerialization.h"
#include "../../KnobSerialization.h"

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

// Per-entry FILE material exposed as a MaterialProvider: one immutable
// GeoImportMaterial snapshot per adapter (shared_ptr — the archive data can be
// freed by a reload while a render thread still holds the adapter).
struct EntryMaterialProvider : public MaterialProvider
{
    std::shared_ptr<const GeoImportMaterial> mat;

    explicit EntryMaterialProvider(const GeoImportMaterial& m)
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
    std::string getMaterialDiffuseColorspace() const OVERRIDE { return "sRGB"; }
};

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

    // --- Morph Weights (glTF shape keys / FBX blend shapes) ---
    // Per-target user weights; same scheme as ReadGeo (see ReadGeo.cpp). The
    // morphW<i> knobs are created dynamically at load and removed when the
    // file changes; morphNames serializes the ordered target list so
    // onKnobsAboutToBeLoaded can recreate the knobs BEFORE loadKnobs and the
    // saved values/keyframes restore by name. useFileMorphWeights off = the
    // knobs drive every set that has knobs (via rt.morphOverrideWeights).
    KnobPageWPtr filePage;
    KnobBoolWPtr useFileMorphWeights;
    KnobStringWPtr morphNames;
    std::vector<KnobDoubleWPtr> morphWeightKnobs;
    std::vector<int> morphKnobSetIds;     // parallel: knob i -> morph set id
    std::vector<int> morphKnobTargetIdx;  // parallel: knob i -> target index

    // Parsed archive + frame cache. rt (and the visible list) are guarded by
    // archiveMutex: loadArchiveFile (GUI thread) clears and refills while the
    // SceneGraph accessors read from render workers and the viewport paint.
    mutable std::mutex archiveMutex;
    GeoArchiveRuntime rt;
    std::vector<int> visible;                // entry indices exposed to SceneGraph
    std::string loadedFilePath;

    // Per-entry file-material adapters (ArchiveProvider::getEntryMaterialProvider).
    // key = entry index into data.nodes; reset on load. Guarded by archiveMutex
    // (built lazily under it by the renderer's query).
    mutable std::map<int, std::shared_ptr<EntryMaterialProvider>> entryMaterials;

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
        "Morph targets / blend shapes: file-animated weights play through; "
        "turning Use File Morph Weights off exposes a keyframable weight knob "
        "per target under the File page.\n\n"
        "Per-entry material assignment from the file is supported "
        "(ArchiveProvider::getEntryMaterialProvider) — Cycles and the viewport "
        "consume the file's per-part materials; wire a Material3D into input 0 "
        "to override the whole archive.").toStdString();
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
    _imp->filePage = page;

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
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Use File Morph Weights"));
        k->setName("useFileMorphWeights");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("When on, morph target (shape key) weights come from the "
                             "file's animation or rest values. When off, the per-target "
                             "Morph Weight knobs on this page drive every target instead "
                             "— keyframable, so you can animate your own blends. The "
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

    // Morph-weight edits (Use File Morph Weights off): the frame cache embeds
    // the weights, so drop it — same pattern as the Bake Transforms branch.
    bool isMorphKnob = (_imp->useFileMorphWeights.lock().get() == k);
    if (!isMorphKnob) {
        for (std::size_t i = 0; i < _imp->morphWeightKnobs.size() && !isMorphKnob; ++i) {
            isMorphKnob = (_imp->morphWeightKnobs[i].lock().get() == k);
        }
    }
    if (isMorphKnob) {
        {
            std::lock_guard<std::mutex> lk(_imp->archiveMutex);
            KnobBoolPtr uf = _imp->useFileMorphWeights.lock();
            _imp->rt.morphOverrideEnabled = uf ? !uf->getValue() : false;
            if (!_imp->rt.morphOverrideEnabled) {
                _imp->rt.morphOverrideWeights.clear();
            }
            _imp->rt.clearFrameCache();
        }
        // The toggle changes frame-varying semantics (animated user weights);
        // a plain weight edit does not.
        if (_imp->useFileMorphWeights.lock().get() == k) {
            refreshMetadata_public(true);
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

// --- Morph Weights (glTF shape keys / FBX blend shapes) -----------------------

void
ReadGeoArchive::collectMorphTargets(std::vector<GeoMorphTargetRef>* out) const
{
    // Caller holds archiveMutex (guards rt.data). Distinct sets over ALL
    // meshes (not just the visible ones — the info census counts the same
    // way), so a filtered-out mesh's shapes stay addressable when it is
    // re-enabled.
    if (!out) return;
    out->clear();
    const GeoArchiveResult& data = _imp->rt.data;

    std::set<const GeoMorphSet*> seen;
    for (std::size_t i = 0; i < data.meshes.size(); ++i) {
        const GeoArchiveMeshData& md = data.meshes[i];
        if (!md.morphs || md.morphs->targets.empty()) continue;
        if (!seen.insert(md.morphs.get()).second) continue;
        const GeoMorphSet& ms = *md.morphs;
        // The label carries the owning node's path when one exists (glTF
        // sets are per-primitive; FBX sets are shared by the pieces of one
        // node mesh — the first owning node wins for the label).
        std::string part;
        for (std::size_t n = 0; n < data.nodes.size() && part.empty(); ++n) {
            const GeoArchiveNodeData& nd = data.nodes[n];
            if (nd.isMesh && nd.meshIndex >= 0 &&
                nd.meshIndex < (int)data.meshes.size() &&
                data.meshes[nd.meshIndex].morphs.get() == md.morphs.get()) {
                part = nd.fullPath.empty() ? nd.name : nd.fullPath;
            }
        }
        for (std::size_t t = 0; t < ms.targets.size(); ++t) {
            GeoMorphTargetRef ref;
            ref.setId = ms.id;
            ref.targetIdx = (int)t;
            ref.restWeight = (t < ms.restWeights.size()) ? ms.restWeights[t] : 0.0f;
            ref.label = ms.targets[t].name.empty()
                ? (std::string("target ") + std::to_string(t + 1))
                : ms.targets[t].name;
            if (!part.empty()) {
                ref.label = part + ": " + ref.label;
            }
            out->push_back(ref);
        }
    }
}

bool
ReadGeoArchive::syncMorphWeightKnobs(const std::vector<GeoMorphTargetRef>& targets)
{
    // Caller holds archiveMutex; runs on the GUI thread (knob add/delete
    // assert it). Index-stable morphW<i> names: identical lists keep knob
    // values (project-load flow), a changed list resets them to the new rest
    // weights (file-change flow). The caller calls recreateKnobs(true) when
    // this returns true — AFTER releasing archiveMutex.
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
            k->setValue(targets[i].restWeight);
        }
    }

    if (mn && listChanged) {
        mn->setValue(newList);
    }

    return structureChanged;
}

void
ReadGeoArchive::applyMorphOverrideAt(double time) const
{
    // Caller holds archiveMutex. Fill the runtime's override store from the
    // weight knobs; the evaluators read it inside ensureFrameState. When Use
    // File Morph Weights is on (the default) this is a no-op and the file's
    // animated/rest weights apply.
    KnobBoolPtr uf = _imp->useFileMorphWeights.lock();
    const bool overrideOn = uf && !uf->getValue();
    _imp->rt.morphOverrideEnabled = overrideOn;
    if (!overrideOn) {
        _imp->rt.morphOverrideWeights.clear();
        return;
    }
    _imp->rt.morphOverrideWeights.clear();
    for (std::size_t i = 0; i < _imp->morphWeightKnobs.size(); ++i) {
        KnobDoublePtr k = _imp->morphWeightKnobs[i].lock();
        if (!k || i >= _imp->morphKnobSetIds.size() || i >= _imp->morphKnobTargetIdx.size()) {
            continue;
        }
        const int setId = _imp->morphKnobSetIds[i];
        const int tIdx = _imp->morphKnobTargetIdx[i];
        if (setId < 0 || tIdx < 0) continue;
        std::vector<float>& v = _imp->rt.morphOverrideWeights[setId];
        if ((std::size_t)tIdx >= v.size()) {
            v.resize((std::size_t)tIdx + 1, 0.0f);
        }
        v[(std::size_t)tIdx] = (float)k->getValueAtTime(time);
    }
}

void
ReadGeoArchive::onKnobsAboutToBeLoaded(const NodeSerializationPtr& serialization)
{
    // Project load: dynamic morphW<i> knobs must exist BEFORE loadKnobs so
    // the saved values + keyframes match by name. Same scheme as ReadGeo —
    // see ReadGeo::onKnobsAboutToBeLoaded.
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

// --- loading -----------------------------------------------------------------

void
ReadGeoArchive::loadArchiveFile(const std::string& path)
{
    // Hold the archive lock for the whole (re)load — render threads holding
    // entries / frame states must not see them freed mid-walk.
    std::unique_lock<std::mutex> lk(_imp->archiveMutex);

    // Drop the parsed archive + frame cache first.
    _imp->visible.clear();
    _imp->entryMaterials.clear();
    _imp->rt.clear();

    if (path.empty()) {
        // No file — no morph targets either; drop any weight knobs from a
        // previously loaded file (they are inert after rt.clear(), but the
        // panel rows would dangle).
        std::vector<GeoMorphTargetRef> none;
        if (syncMorphWeightKnobs(none)) {
            lk.unlock();
            recreateKnobs(true);
            Q_EMIT archiveReloaded();
            return;
        }
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
    // Morph targets (shape keys): targets over DISTINCT morph sets (the
    // material pieces of one FBX node mesh share one set).
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

    // Morph Weight knobs: sync to the file's distinct morph sets (no-op when
    // the file has none — the knobs drop). Knob surgery happens under the
    // archive lock; the panel refresh (recreateKnobs) must NOT — it can
    // re-enter node accessors, so it runs after lk.unlock() below.
    bool morphKnobsChanged = false;
    {
        std::vector<GeoMorphTargetRef> targets;
        collectMorphTargets(&targets);
        morphKnobsChanged = syncMorphWeightKnobs(targets);
    }

    // Release BEFORE the emit: settings-panel slots run synchronously off
    // archiveReloaded and re-enter getEntryTree(), which takes this same
    // non-recursive mutex (self-deadlock guard, same as ReadAlembicArchive).
    lk.unlock();
    if (morphKnobsChanged) {
        recreateKnobs(true);
    }
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

    // Morph-weight override: the node-local walk below triggers
    // ensureFrameState through nodeLocalAt — fill the override store first
    // (frame time the key maps to) so a lazily-built frame embeds the user's
    // weights, not the file's. Caller holds archiveMutex.
    {
        const int key = (int)std::floor(time - 1.0 - (double)frameOffset);
        applyMorphOverrideAt((double)(key > 0 ? key : 0) + 1.0 + (double)frameOffset);
    }

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

    // Entries whose payload depends on the frame evaluation: skinned meshes
    // (CPU skinning), baked meshes (world bake), and — since the morph work —
    // morph-carrying RIGID meshes too. The latter used to early-return the
    // rest mesh, silently dropping morph targets (shape keys / blend shapes)
    // on the most common rig (no skeleton); the evaluators always build the
    // morph-applied payload, so route through the frame cache like the
    // skinned ones.
    const bool needsFrame = (md.skinIndex >= 0) || _imp->rt.bakeMeshWorld ||
                            (md.morphs && !md.morphs->targets.empty());
    if (!needsFrame || time < 0.0) {
        return md.mesh;
    }

    // Skinned: per-frame CPU skinning via the frame cache. Snaps per frame
    // (the sub-frame motion-blur interpolation the Alembic machinery does on
    // vertex samples is not applied to skinning yet — documented limitation).
    const int frameOffset = _imp->frameOffsetKnobValue();
    int key = (int)std::floor(time - 1.0 - (double)frameOffset);
    if (key < 0) key = 0;
    // Morph-weight override: fill the runtime store from the user's weight
    // knobs at the frame time the key maps to, BEFORE ensureFrameState (the
    // evaluators read it). Caller holds archiveMutex.
    applyMorphOverrideAt((double)(key + 1) + (double)frameOffset);
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

    // Morph-weight override: worldMatrixAt -> nodeLocalAt may trigger
    // ensureFrameState; fill the override store first (same key mapping as
    // getSceneNodeAt). Caller holds archiveMutex.
    {
        const int frameOffset = _imp->frameOffsetKnobValue();
        const int key = (int)std::floor(time - 1.0 - (double)frameOffset);
        applyMorphOverrideAt((double)(key > 0 ? key : 0) + 1.0 + (double)frameOffset);
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

MaterialProvider*
ReadGeoArchive::getEntryMaterialProvider(int idx) const
{
    std::lock_guard<std::mutex> lk(_imp->archiveMutex);
    if (idx < 0 || idx >= (int)_imp->rt.data.nodes.size()) return nullptr;
    const GeoArchiveNodeData& nd = _imp->rt.data.nodes[idx];
    if (!nd.isMesh || nd.meshIndex < 0 || nd.meshIndex >= (int)_imp->rt.data.meshes.size()) {
        return nullptr;
    }

    // Cached adapter?
    std::map<int, std::shared_ptr<EntryMaterialProvider>>::const_iterator it =
        _imp->entryMaterials.find(idx);
    if (it != _imp->entryMaterials.end()) return it->second.get();

    const GeoArchiveMeshData& md = _imp->rt.data.meshes[nd.meshIndex];
    if (md.materialIndex < 0 || md.materialIndex >= (int)_imp->rt.data.materials.size() ||
        !_imp->rt.data.materials[md.materialIndex].present) {
        return nullptr;
    }

    std::shared_ptr<EntryMaterialProvider> adapter =
        std::make_shared<EntryMaterialProvider>(_imp->rt.data.materials[md.materialIndex]);
    _imp->entryMaterials[idx] = adapter;
    return adapter.get();
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
    // Animated user morph weights (Use File Morph Weights off) also make the
    // node time-varying — the file-animation flags don't see knob animation.
    if (_imp) {
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
