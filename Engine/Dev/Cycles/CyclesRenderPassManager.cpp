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

#include "CyclesRenderPassManager.h"

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QStringList>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Format.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../KnobFile.h"
#include "../../Node.h"
#include "../../Project.h"
#include "../../TimeLine.h"

#include "CyclesPassRender.h"
#include "CyclesRenderer.h"
#include "CyclesRenderSettings.h"
#include "KnobPassTable.h"
#include "../Scene3D/Material3D.h"

NATRON_NAMESPACE_ENTER

struct CyclesRenderPassManagerPrivate
{
    // Pass list as a JSON string. Persisted in project files automatically
    // via Natron's knob serialization. Seed value covers the MVP 2-pass case
    // (one beauty + one data). Edited via the custom spreadsheet table widget
    // (KnobPassTable → KnobGuiPassTable → PassTableWidget); the backing store
    // is still the same JSON string the renderer reads.
    KnobPassTableWPtr passesJson;
    KnobButtonWPtr resetToDefault;
    KnobButtonWPtr renderToDisk;

    // Which pass to preview live in the viewer (-1 = none → transparent black).
    // Driven by the pass-table widget's "Preview Selected" button; secret
    // because the GUI owns it.
    KnobIntWPtr    previewPassIndex;

    // Frame range
    KnobChoiceWPtr frameMode;
    KnobIntWPtr    frameStart;
    KnobIntWPtr    frameEnd;
    KnobIntWPtr    frameIncrement;

    CyclesRenderPassManagerPrivate()
    {}
};

// Frame mode choice (top-level frame iteration policy on Render to Disk).
enum FrameMode {
    eFrameModeCurrent      = 0,  // Render the current timeline frame only
    eFrameModeRange        = 1,  // Render [start, end] step inc
    eFrameModeRangeNoReRender = 2, // Same as Range, but skip frames whose ALL output files already exist
};

// Default seed JSON — two passes covering the common production cases.
// Beauty: Combined + all light groups (via @all_light_groups token) so a
//         comp artist can relight by mixing layers. Light-group AOVs
//         are auto-expanded from any Light3D in the scene whose Light
//         Group knob is set.
// Data:   Depth + Normal + UV (the comp utility bundle), 32-bit float.
// Schema mirrors the data.js model from the UI demo.
static const char* kDefaultPassesJson =
    "[\n"
    "  {\n"
    "    \"_comment\": \"Beauty + every light group as layers. Set a Light3D's Light Group knob (e.g. 'keyLight') to add it to the bundle. @all_light_groups expands at render time.\",\n"
    "    \"id\": \"p1\",\n"
    "    \"name\": \"beauty_main\",\n"
    "    \"type\": \"bty\",\n"
    "    \"group\": \"beauty\",\n"
    "    \"enabled\": true,\n"
    "    \"solo\": false,\n"
    "    \"mute\": false,\n"
    "    \"output\": true,\n"
    "    \"aovs\": [\"Combined\", \"@all_light_groups\"],\n"
    "    \"filePath\": \"$RENDER/$SHOT/$PASS/$SHOT_$PASS.####.exr\",\n"
    "    \"format\": \"EXR (Multilayer)\",\n"
    "    \"bitDepth\": \"16-bit Half\",\n"
    "    \"compression\": \"ZIP\",\n"
    "    \"samples\": 128,\n"
    "    \"candidateLights\": \"*\",\n"
    "    \"excludeLights\": \"\",\n"
    "    \"soloLight\": \"\",\n"
    "    \"candidateObjects\": \"*\",\n"
    "    \"excludeObjects\": \"\",\n"
    "    \"soloObject\": \"\",\n"
    "    \"cameraOverride\": \"\",\n"
    "    \"materialOverride\": \"\",\n"
    "    \"shadowCatcherObjects\": \"\",\n"
    "    \"holdoutObjects\": \"\",\n"
    "    \"traceObjects\": \"\"\n"
    "  },\n"
    "  {\n"
    "    \"id\": \"p2\",\n"
    "    \"name\": \"data_utility\",\n"
    "    \"type\": \"data\",\n"
    "    \"group\": \"data\",\n"
    "    \"enabled\": true,\n"
    "    \"solo\": false,\n"
    "    \"mute\": false,\n"
    "    \"output\": true,\n"
    "    \"aovs\": [\"Depth\", \"Normal\", \"UV\"],\n"
    "    \"filePath\": \"$RENDER/$SHOT/data/$SHOT_data.####.exr\",\n"
    "    \"format\": \"EXR (Multilayer)\",\n"
    "    \"bitDepth\": \"32-bit Full\",\n"
    "    \"compression\": \"ZIP\",\n"
    "    \"samples\": 128,\n"
    "    \"candidateObjects\": \"*\",\n"
    "    \"excludeObjects\": \"\",\n"
    "    \"soloObject\": \"\",\n"
    "    \"cameraOverride\": \"\",\n"
    "    \"materialOverride\": \"\",\n"
    "    \"shadowCatcherObjects\": \"\",\n"
    "    \"holdoutObjects\": \"\",\n"
    "    \"traceObjects\": \"\"\n"
    "  }\n"
    "]\n";

CyclesRenderPassManager::CyclesRenderPassManager(NodePtr node)
    : EffectInstance(node)
    , _imp(new CyclesRenderPassManagerPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

CyclesRenderPassManager::~CyclesRenderPassManager()
{
}

std::string
CyclesRenderPassManager::getPluginDescription() const
{
    return tr("Manage and submit multiple Cycles render passes to disk.\n\n"
              "Sink node — owns a list of pass specs, each with its own camera, "
              "object / light visibility, AOV selection, and output settings. "
              "The 'Render to Disk' button batches passes by shared scene state "
              "and submits one Cycles session per batch.\n\n"
              "Input layout matches CyclesRender (bg / obj / cam).").toStdString();
}

std::string
CyclesRenderPassManager::getInputLabel(int inputNb) const
{
    switch (inputNb) {
    case 0: return "bg";
    case 1: return "obj";
    case 2: return "cam";
    case 3: return "settings";
    default: return "?";
    }
}

bool
CyclesRenderPassManager::isInputOptional(int /*inputNb*/) const
{
    // Same convention as the other multi-input Natron nodes — all inputs
    // optional, runtime validation in render() / submit handler.
    return true;
}

void
CyclesRenderPassManager::addAcceptedComponents(int /*inputNb*/,
                                                std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
}

void
CyclesRenderPassManager::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
CyclesRenderPassManager::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
CyclesRenderPassManager::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Pass Manager"));

    // The pass list — JSON string backing a custom spreadsheet table widget
    // (KnobPassTable). Persisted in the project file like any string knob.
    // Seed with a default 2-pass setup so a freshly-created node has something
    // usable.
    {
        KnobPassTablePtr k = AppManager::createKnob<KnobPassTable>(this, tr("Passes"));
        k->setName("passesJson");
        k->setDefaultValue(kDefaultPassesJson);
        k->setHintToolTip(tr(
            "JSON-serialized list of render passes. Each entry: id, name, "
            "type, group, enabled/solo/mute/output flags, aovs (array), "
            "filePath, format, bitDepth, compression, samples, candidateLights, "
            "excludeLights, soloLight, candidateObjects, excludeObjects, "
            "soloObject, cameraOverride, materialOverride, shadowCatcherObjects, "
            "holdoutObjects, traceObjects."
            "\n\nLight handling — two mechanisms:"
            "\n  1. Light-group AOVs (standard workflow): the beauty file "
            "carries every light group as separate layers so comp can do "
            "the relight. Set a Light3D node's Light Group knob (e.g. "
            "'keyLight'), then either request \"Combined_keyLight\" "
            "explicitly in aovs, OR use the magic token "
            "\"@all_light_groups\" which auto-expands to one Combined_<group> "
            "per unique Light Group in the scene."
            "\n  2. Light scoping (rare — shadow/matte/bespoke passes): set "
            "candidateLights / excludeLights / soloLight to Light3D script "
            "names. The scene is physically rebuilt with only those lights, "
            "forcing a separate Cycles session per scoping config."
            "\n\nObject scoping (rare — matte/element/clean passes): "
            "candidateObjects / excludeObjects / soloObject use the same "
            "semicolon-separated script-name semantics. Visible objects are "
            "rendered with full ray visibility; everything else is excluded "
            "from the scene. \"*\" or empty candidates = all objects. An "
            "exclude list without any candidates means \"all objects EXCEPT\". "
            "Passes with the same scoping (and samples + light scoping) batch "
            "into one Cycles session."
            "\n\nCamera override: cameraOverride takes the fully-qualified "
            "script name of any CameraProvider node in the project (Camera3D, "
            "ReadAlembicCamera, …). Empty → fall back to the manager's input "
            "slot 2 → renderer default. An unresolvable name fails that "
            "batch loudly rather than silently rendering the wrong angle. "
            "Different camera names force separate batches."
            "\n\nMaterial override: materialOverride takes the fully-qualified "
            "script name of a Material3D-like MaterialProvider node anywhere "
            "in the project. When set, every mesh in the batch renders with "
            "this shader instead of its own — the Blender-style clay-render / "
            "shadow-pass pattern. Particles and volumes keep their own "
            "shaders. Unresolvable name aborts the batch loudly."
            "\n\nVisibility promotions (shadow / matte / element passes): "
            "shadowCatcherObjects / holdoutObjects / traceObjects are "
            "semicolon-separated script names of scene-geo objects that "
            "should be promoted from default-visible to a specific "
            "visibility profile."
            "\n  shadowCatcherObjects → catches shadows from other objects "
            "but is itself invisible in beauty. Pair with materialOverride "
            "(a plain diffuse) for the canonical shadow pass."
            "\n  holdoutObjects → punches the alpha (clean-plate workflows)."
            "\n  traceObjects → phantom / trace-only; contributes to "
            "reflections/refractions but is invisible to camera."
            "\nPriority when an object appears in multiple lists: "
            "shadow catcher > holdout > trace > default visible. Excluded "
            "(from object scoping) wins over all promotions. Different "
            "promotion sets force separate batches."
            "\n\nOutput format is selected by the extension on filePath:"
            "\n  .exr            → multi-layer EXR (default). bitDepth = "
            "\"16-bit Half\" or \"32-bit Full\"; compression = ZIP / ZIPS / "
            "PIZ / DWAA / DWAB / RLE / PXR24 / B44 / B44A / None."
            "\n  .png            → bitDepth \"8\" or \"16\"; deflate compression (implicit)."
            "\n  .tif / .tiff    → bitDepth \"8\", \"16\", or \"32\"; "
            "compression = ZIP / LZW / None."
            "\n  .jpg / .jpeg    → 8-bit RGB only; compression field is JPEG quality 1-100."
            "\nMulti-AOV passes in non-EXR formats produce one file per AOV "
            "with _<AOVName> injected before the extension (e.g. data.0001_Normal.png)."
            "\n\nRender to Disk prints the scene's Light3D + non-light script "
            "names AND every CameraProvider + MaterialProvider node in the "
            "project so you can match them. Edit directly here for now; "
            "custom UI widget lands in a later phase. Use Reset to Default "
            "to restore the seed."));
        page->addKnob(k);
        _imp->passesJson = k;
    }

    // Reset button — easy way to recover from a JSON edit gone wrong.
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reset to Default"));
        k->setName("resetPassesToDefault");
        k->setHintToolTip(tr("Restore the default 2-pass JSON (beauty + data)."));
        page->addKnob(k);
        _imp->resetToDefault = k;
    }

    // Frame range controls — mirrors the data.js toolbar layout.
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Frame Mode"));
        k->setName("frameMode");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Render Current Frame", "",
            "Render only the timeline's current frame on each button press."));
        entries.push_back(ChoiceOption("Render Frame Range", "",
            "Render every frame in [Start, End] stepping by Increment. Re-renders existing files."));
        entries.push_back(ChoiceOption("Render Frame Range (No Re-render)", "",
            "Same as Range, but skip any frame whose output EXR(s) already exist on disk."));
        k->populateChoices(entries);
        k->setDefaultValue((int)eFrameModeCurrent);
        page->addKnob(k);
        _imp->frameMode = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Start"));
        k->setName("frameStart");
        k->setDefaultValue(1);
        k->setHintToolTip(tr("First frame to render in Range modes."));
        page->addKnob(k);
        _imp->frameStart = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("End"));
        k->setName("frameEnd");
        k->setDefaultValue(100);
        k->setHintToolTip(tr("Last frame to render (inclusive) in Range modes."));
        page->addKnob(k);
        _imp->frameEnd = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Increment"));
        k->setName("frameIncrement");
        k->setDefaultValue(1);
        k->setMinimum(1);
        k->setHintToolTip(tr("Frame step in Range modes. 1 = every frame, 2 = every other, etc."));
        page->addKnob(k);
        _imp->frameIncrement = k;
    }

    // The submit button. MVP step 3 wires it to parse + dump only —
    // actual disk write lands in step 5. Verifies the JSON schema is
    // intact and the active-pass filter behaves correctly before we
    // touch the renderer.
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Render to Disk"));
        k->setName("renderToDisk");
        k->setHintToolTip(tr(
            "Submit all active passes to disk. Active = enabled && output && "
            "!mute && (no solo set OR this pass is solo'd). "
            "MVP step 3: prints the active list to stderr without rendering."));
        page->addKnob(k);
        _imp->renderToDisk = k;
    }

    // Secret: which pass index the viewer previews. The pass-table widget's
    // "Preview Selected" button sets this; render() reads it. Persisted so a
    // reopened project shows the same preview.
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Preview Pass Index"));
        k->setName("previewPassIndex");
        k->setDefaultValue(-1);
        k->setSecretByDefault(true);
        page->addKnob(k);
        _imp->previewPassIndex = k;
    }
}

// Token resolver — substitutes dollar tokens + frame padding in pass
// filePaths. Mirrors the data.js convention plus Natron's existing
// frame-pattern handling from CyclesRenderer::resolveTextureFrame.
//
// Tokens supported:
//   $PASS   — pass name (passed in as context)
//   $SHOT   — env var SHOT, falls back to "shot"
//   $RENDER — env var RENDER, falls back to "/tmp" (or current dir on Windows;
//             user is expected to set this for real pipelines)
//
// Frame patterns supported (in priority order):
//   #### / # / ##... — zero-padded substring of #s
//   %0Nd / %d        — printf-style numeric
//   trailing digit group before .ext — guessed padding from match width
static std::string
getEnvOrDefault(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    if (v && *v) return std::string(v);
    return std::string(fallback);
}

static std::string
substituteDollarTokens(const std::string& path, const std::string& passName)
{
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    std::string out = path;
    replaceAll(out, "$PASS",   passName);
    replaceAll(out, "$SHOT",   getEnvOrDefault("SHOT",   "shot"));
    replaceAll(out, "$RENDER", getEnvOrDefault("RENDER", "/tmp"));
    return out;
}

// Lifted from CyclesRenderer::resolveTextureFrame — same #### / %04d /
// trailing-digit logic. Local copy so this file doesn't have to include
// CyclesRenderer.h (which pulls in cycles headers).
static std::string
resolveFramePadding(const std::string& path, int frame)
{
    if (path.empty()) return path;
    std::string result = path;

    // Replace #### with zero-padded frame number
    size_t hashStart = result.find('#');
    if (hashStart != std::string::npos) {
        size_t hashEnd = hashStart;
        while (hashEnd < result.size() && result[hashEnd] == '#') ++hashEnd;
        int padding = (int)(hashEnd - hashStart);
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(padding) << frame;
        result.replace(hashStart, hashEnd - hashStart, ss.str());
        return result;
    }

    // Replace %04d-style patterns
    char buf[1024];
    snprintf(buf, sizeof(buf), result.c_str(), frame);
    if (std::string(buf) != result) return std::string(buf);

    // Trailing digit group before extension
    size_t dotPos = result.rfind('.');
    if (dotPos != std::string::npos && dotPos > 0) {
        size_t numEnd = dotPos;
        size_t numStart = numEnd;
        while (numStart > 0 && result[numStart - 1] >= '0' && result[numStart - 1] <= '9') --numStart;
        if (numStart < numEnd) {
            int padding = (int)(numEnd - numStart);
            std::ostringstream ss;
            ss << std::setfill('0') << std::setw(padding) << frame;
            result.replace(numStart, numEnd - numStart, ss.str());
            return result;
        }
    }
    return result;
}

// Compose the two resolvers. Order: substitute $TOKENs first (some
// might contain frame patterns), then resolve frame padding.
static std::string
expandPassPath(const std::string& template_, const std::string& passName, int frame)
{
    std::string after = substituteDollarTokens(template_, passName);
    return resolveFramePadding(after, frame);
}

// Per-spec info captured from the JSON during the per-pass diagnostic
// dump, then consumed in the batching phase. Path stays as the raw
// template ($PASS / $SHOT / $RENDER / #### unresolved) so the frame
// loop can re-resolve it per frame.
struct ActiveSpec {
    int                       index;        // original index in the JSON array
    std::string               name;
    int                       samples = 0;
    std::vector<std::string>  aovs;
    std::string               rawPath;      // template with tokens unresolved
    // Output format spec (per-pass) — empty strings → defaults.
    std::string               format;       // "EXR (Multilayer)", "PNG (16-bit)", etc. MVP: EXR only.
    std::string               bitDepth;     // "32-bit Full" (default), "16-bit Half"
    std::string               compression;  // ZIP / ZIPS / PIZ / DWAA / DWAB / RLE / PXR24 / B44 / B44A / None
    // Light scoping (per-pass). Semicolon-separated light names — glob
    // pattern resolution deferred. Resolved at render time into the
    // CyclesPassRequest::activeLights filter set.
    std::string               candidateLights;  // "" or "*" = all; otherwise "lightA; lightB"
    std::string               excludeLights;    // never include these (subtracted from candidates)
    std::string               soloLight;        // if set: this overrides everything and is the only light
    // Object scoping (per-pass). Same semicolon-separated semantics as
    // lights. Resolved at render time into a CyclesPassRequest::visMap
    // covering EVERY scene object (visible → ALL_VISIBILITY, scoped-out
    // → isExcluded=true). Unscoped specs leave visMap null so the
    // renderer skips the per-object visibility code path entirely.
    std::string               candidateObjects;
    std::string               excludeObjects;
    std::string               soloObject;
    // Per-pass camera override. Script name of a CameraProvider-derived
    // node anywhere in the project (Camera3D, ReadAlembicCamera, …).
    // Empty → fall back to the manager's input slot 2. Resolved at render
    // time; an unresolvable name aborts the batch with a clear error.
    std::string               cameraOverride;

    // Per-pass material override. Script name of a MaterialProvider node
    // (typically a Material3D) anywhere in the project. When set, every
    // mesh in the batch renders with this shader instead of its own —
    // the standard clay-render / shadow-pass pattern. Unresolvable name
    // aborts the batch with a clear error.
    std::string               materialOverride;

    // Per-pass visibility promotions. Semicolon-separated script names of
    // scene-geo objects that should be promoted from default-visible to
    // a specific visibility profile. Priority when an object appears in
    // multiple lists: shadow catcher > holdout > trace. Excluded (from
    // object scoping) wins over all promotions.
    std::string               shadowCatcherObjects;  // is_shadow_catcher=true
    std::string               holdoutObjects;        // is_holdout=true
    std::string               traceObjects;          // rayVisibility = ALL & ~CAMERA (phantom)
};

// Split a semicolon-separated name list into a set of trimmed names.
// "*" and the empty string both mean "no explicit list" → empty set.
// Reused for both light and object scoping.
static std::set<std::string>
parseNameList(const std::string& s)
{
    std::set<std::string> out;
    if (s.empty()) return out;
    std::string trimmed = s;
    // strip leading + trailing whitespace
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))  trimmed.pop_back();
    if (trimmed == "*") return out;  // wildcard = all lights

    size_t start = 0;
    while (start < trimmed.size()) {
        size_t end = trimmed.find(';', start);
        if (end == std::string::npos) end = trimmed.size();
        std::string tok = trimmed.substr(start, end - start);
        // trim per-token
        while (!tok.empty() && std::isspace((unsigned char)tok.front())) tok.erase(tok.begin());
        while (!tok.empty() && std::isspace((unsigned char)tok.back()))  tok.pop_back();
        if (!tok.empty()) out.insert(tok);
        start = end + 1;
    }
    return out;
}

// Resolve a spec's per-pass light-scoping fields into a final active set.
//   solo non-empty       → {solo}
//   candidates non-empty → candidates - excludes
//   otherwise            → empty set (= all lights pass through)
//
// Returns true if the result is "scoped" (non-empty set or explicit
// no-lights), false if it means "render with all lights" (empty
// candidates and no solo). The caller uses this to decide whether to
// pass a non-null pointer into CyclesPassRequest::activeLights.
static bool
resolveActiveLights(const ActiveSpec& spec, std::set<std::string>& out)
{
    out.clear();
    if (!spec.soloLight.empty()) {
        // Trim and use as the only active light.
        std::string s = spec.soloLight;
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        if (!s.empty()) out.insert(s);
        return true;
    }
    const std::set<std::string> candidates = parseNameList(spec.candidateLights);
    const std::set<std::string> excludes   = parseNameList(spec.excludeLights);
    if (candidates.empty()) {
        // No candidates declared → "all lights" semantics, even if there
        // are excludes. Excludes-without-candidates is ambiguous in the
        // empty-set-means-all model; deferred until we enumerate the
        // scene light tree (V2).
        return false;
    }
    for (const auto& c : candidates) {
        if (excludes.find(c) == excludes.end()) out.insert(c);
    }
    return true;
}

// Resolve a spec's per-pass object-scoping fields into a final visible set.
//   solo non-empty       → {solo}
//   candidates non-empty → candidates - excludes
//   candidates empty, excludes non-empty → allSceneGeo - excludes
//   otherwise            → unscoped (caller passes nullptr visMap)
//
// Returns true if scoped (visMap must be built), false if unscoped.
// Unlike lights, we *can* honor excludes-without-candidates here because
// we have the enumerated geo list to enumerate-and-subtract from.
static bool
resolveActiveObjects(const ActiveSpec&                spec,
                     const std::vector<SceneGeoInfo>& allSceneGeo,
                     std::set<std::string>&           outVisible)
{
    outVisible.clear();
    if (!spec.soloObject.empty()) {
        std::string s = spec.soloObject;
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        if (!s.empty()) outVisible.insert(s);
        return true;
    }
    const std::set<std::string> candidates = parseNameList(spec.candidateObjects);
    const std::set<std::string> excludes   = parseNameList(spec.excludeObjects);
    if (candidates.empty()) {
        if (excludes.empty()) return false;  // truly unscoped
        for (const auto& g : allSceneGeo) {
            if (excludes.find(g.scriptName) == excludes.end()) {
                outVisible.insert(g.scriptName);
            }
        }
        return true;
    }
    for (const auto& c : candidates) {
        if (excludes.find(c) == excludes.end()) outVisible.insert(c);
    }
    return true;
}

// Trim leading/trailing whitespace in place. Local helper because we
// already do this inline in a few spots; refactoring those out is
// cosmetic and not in scope here.
static std::string
trimCopy(const std::string& s)
{
    std::string t = s;
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin());
    while (!t.empty() && std::isspace((unsigned char)t.back()))  t.pop_back();
    return t;
}

// Walk the project's full node tree (sub-groups included) and collect
// every node whose effect implements CameraProvider. Used for the
// diagnostic dump and to surface the camera names the user can plug
// into JSON cameraOverride fields.
struct ProjectCameraInfo {
    std::string fullName;   // suitable for getNodeByFullySpecifiedName
};

static void
enumerateProjectCameras(EffectInstance* callerEffect,
                        std::vector<ProjectCameraInfo>& out)
{
    out.clear();
    if (!callerEffect) return;
    AppInstancePtr app = callerEffect->getApp();
    if (!app || !app->getProject()) return;
    NodesList nodes;
    app->getProject()->getNodes_recursive(nodes, /*onlyActive*/ false);
    for (const NodePtr& n : nodes) {
        if (!n) continue;
        EffectInstancePtr fx = n->getEffectInstance();
        if (!fx) continue;
        if (!dynamic_cast<CameraProvider*>(fx.get())) continue;
        ProjectCameraInfo info;
        info.fullName = n->getFullyQualifiedName();
        out.push_back(std::move(info));
    }
}

// Same shape as enumerateProjectCameras but for MaterialProvider-derived
// nodes. Filters out per-geo nodes (Sphere3D, Card3D, etc. all implement
// MaterialProvider) by requiring `hasMaterialInput()` == false — that's
// true only for actual material-source nodes like Material3D, which is
// what users should plug into a `materialOverride` field.
struct ProjectMaterialInfo {
    std::string fullName;
};

static void
enumerateProjectMaterials(EffectInstance* callerEffect,
                          std::vector<ProjectMaterialInfo>& out)
{
    out.clear();
    if (!callerEffect) return;
    AppInstancePtr app = callerEffect->getApp();
    if (!app || !app->getProject()) return;
    NodesList nodes;
    app->getProject()->getNodes_recursive(nodes, /*onlyActive*/ false);
    for (const NodePtr& n : nodes) {
        if (!n) continue;
        EffectInstancePtr fx = n->getEffectInstance();
        if (!fx) continue;
        // Filter to genuine material-source nodes. hasMaterialInput() is
        // a runtime-state query ("is something wired") not a structural
        // one, so a fresh geo node with no material connected would slip
        // through. dynamic_cast<Material3D> only matches actual material
        // sources.
        if (!dynamic_cast<Material3D*>(fx.get())) continue;
        ProjectMaterialInfo info;
        info.fullName = n->getFullyQualifiedName();
        out.push_back(std::move(info));
    }
}

// Build a string key that two specs MUST match on to share a Cycles
// session. Components: samples, sorted active-lights set, sorted visible-
// objects set, camera override, sorted shadow-catcher / holdout / phantom
// sets, material override. Specs with the same key render with the same
// Cycles scene state, so we can batch them into a single submission.
static std::string
batchKeyFor(const ActiveSpec&                spec,
            const std::vector<SceneGeoInfo>& allSceneGeo)
{
    std::set<std::string> activeL;
    const bool lightScoped = resolveActiveLights(spec, activeL);
    std::set<std::string> activeO;
    const bool objScoped = resolveActiveObjects(spec, allSceneGeo, activeO);

    auto joinSet = [](const std::set<std::string>& s) {
        std::string out;
        bool first = true;
        for (const auto& n : s) {
            if (!first) out += ",";
            out += n;
            first = false;
        }
        return out;
    };

    std::string key = std::to_string(spec.samples);
    key += "|lights=";
    key += lightScoped ? joinSet(activeL) : std::string("*");
    key += "|objects=";
    key += objScoped ? joinSet(activeO) : std::string("*");
    key += "|cam=";
    const std::string camTrim = trimCopy(spec.cameraOverride);
    key += camTrim.empty() ? std::string("<input2>") : camTrim;
    key += "|sc=";
    key += joinSet(parseNameList(spec.shadowCatcherObjects));
    key += "|ho=";
    key += joinSet(parseNameList(spec.holdoutObjects));
    key += "|tr=";
    key += joinSet(parseNameList(spec.traceObjects));
    key += "|mat=";
    const std::string matTrim = trimCopy(spec.materialOverride);
    key += matTrim.empty() ? std::string("<none>") : matTrim;
    return key;
}

// Render one frame: re-resolve paths for `frame`, partition into batches,
// run one Cycles session per batch, demux per-pass and save EXR.
// Returns the number of batches actually rendered (0 if skipped or
// nothing to do; -1 if any render failed).
static int
renderFrameForBatches(EffectInstance*                  effect,
                       const std::vector<ActiveSpec>&   activeSpecs,
                       const std::vector<SceneGeoInfo>& allSceneGeo,
                       const CyclesRenderSettings*    settings,
                       int                              frame,
                       FrameMode                        mode,
                       int                              width,
                       int                              height);

// Resolve output dimensions: prefer the project's default format, fall
// back to 1920×1080 if the project hasn't set one (or no app instance).
static void
resolveOutputDimensions(EffectInstance* effect, int& w, int& h)
{
    w = 1920;
    h = 1080;
    if (!effect) return;
    AppInstancePtr app = effect->getApp();
    if (!app || !app->getProject()) return;
    Format fmt;
    app->getProject()->getProjectDefaultFormat(&fmt);
    const int pw = fmt.width();
    const int ph = fmt.height();
    if (pw > 0 && ph > 0) {
        w = pw;
        h = ph;
    }
}

// Parse the passes JSON, filter to the active set, expand token-bearing
// paths, then iterate the frame range (or just the current frame), and
// for each frame group active passes into batches (one render per shared
// scene-state config) and demux each batch's rendered AOVs into per-pass
// EXR files. Returns false on JSON parse failure so the caller can
// surface a UI message.
static bool
parseAndDumpActivePasses(EffectInstance*    callerEffect,
                          double             renderTime,
                          const std::string& jsonStr,
                          FrameMode          mode,
                          int                frameStart,
                          int                frameEnd,
                          int                frameInc)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(jsonStr), &err);
    if (err.error != QJsonParseError::NoError) {
        fprintf(stderr, "[PassManager] JSON parse error at offset %d: %s\n",
                err.offset, err.errorString().toUtf8().constData());
        fflush(stderr);
        return false;
    }
    if (!doc.isArray()) {
        fprintf(stderr, "[PassManager] JSON root is not an array\n");
        fflush(stderr);
        return false;
    }

    const QJsonArray arr = doc.array();

    // First pass: detect whether any pass has solo set. Solo bypasses
    // mute-of-non-solo'd peers — matches the data.js / app.jsx semantics
    // (`enabled && output && !mute && (!soloActive || solo)`).
    bool soloActive = false;
    for (const QJsonValue& v : arr) {
        if (v.isObject() && v.toObject().value(QStringLiteral("solo")).toBool()) {
            soloActive = true;
            break;
        }
    }

    int total    = arr.size();
    int active   = 0;
    int disabled = 0;
    int muted    = 0;
    int hidden   = 0; // soloActive && !solo

    // Frame for path resolution + the renderer's args.time. Step 6 will
    // iterate a real frame range; for now we render the timeline's
    // current frame on every button press.
    const int dbgFrame = (int)renderTime;

    std::vector<ActiveSpec> activeSpecs;
    activeSpecs.reserve(arr.size());

    fprintf(stderr, "[PassManager] Render to Disk — parsing %d pass(es)%s\n",
            total, soloActive ? " (SOLO active)" : "");
    fprintf(stderr, "[PassManager]   tokens: $PASS=<per-pass>  $SHOT='%s'  $RENDER='%s'  frame=%d\n",
            getEnvOrDefault("SHOT",   "shot").c_str(),
            getEnvOrDefault("RENDER", "/tmp").c_str(),
            dbgFrame);

    // Enumerate scene lights + geo up front. Three consumers: the per-frame
    // diagnostic dump below, the @all_light_groups AOV expansion in the
    // per-pass loop, and the per-batch ObjectVisibility map built in
    // renderFrameForBatches. Cheap — pure dynamic_cast walk, no SceneGraph
    // rebuild.
    std::vector<SceneLightInfo> sceneLights;
    enumerateSceneLights(callerEffect, renderTime, sceneLights);

    std::vector<SceneGeoInfo> sceneGeo;
    enumerateSceneGeo(callerEffect, renderTime, sceneGeo);

    // Collect unique non-empty light-group names — drives @all_light_groups.
    std::set<std::string> sceneLightGroupSet;
    for (const auto& li : sceneLights) {
        if (!li.lightGroup.empty()) sceneLightGroupSet.insert(li.lightGroup);
    }
    for (int i = 0; i < arr.size(); ++i) {
        if (!arr[i].isObject()) {
            fprintf(stderr, "[PassManager]   #%d : entry is not an object (skipping)\n", i);
            continue;
        }
        const QJsonObject p = arr[i].toObject();
        const QString name    = p.value(QStringLiteral("name")).toString();
        const QString type    = p.value(QStringLiteral("type")).toString();
        const bool enabled    = p.value(QStringLiteral("enabled")).toBool();
        const bool solo       = p.value(QStringLiteral("solo")).toBool();
        const bool mute       = p.value(QStringLiteral("mute")).toBool();
        const bool output     = p.value(QStringLiteral("output")).toBool();

        const bool wouldRender = enabled && output && !mute &&
                                 (!soloActive || solo);

        // Build the AOV list, expanding any magic tokens. Currently:
        //   @all_light_groups → Combined_<group> for every unique
        //                       non-empty lightGroup in the scene
        // Other tokens (e.g. @cryptomatte) can be added the same way.
        // The resulting list is deduplicated while preserving the user's
        // ordering for non-token entries.
        QStringList aovList;
        std::set<QString> seenAovs;
        const QJsonArray aovs = p.value(QStringLiteral("aovs")).toArray();
        for (const QJsonValue& a : aovs) {
            const QString s = a.toString();
            if (s == QStringLiteral("@all_light_groups")) {
                for (const auto& g : sceneLightGroupSet) {
                    const QString expanded =
                        QStringLiteral("Combined_") + QString::fromStdString(g);
                    if (seenAovs.insert(expanded).second) aovList << expanded;
                }
            } else if (!s.isEmpty()) {
                if (seenAovs.insert(s).second) aovList << s;
            }
        }

        const QString filePath = p.value(QStringLiteral("filePath")).toString();
        const int samples = p.value(QStringLiteral("samples")).toInt();

        const char* statusTag =
            !enabled        ? "DISABLED" :
            mute            ? "MUTED"    :
            (soloActive && !solo) ? "HIDDEN"   :
            !output         ? "NOOUT"   :
                              "ACTIVE";

        if (wouldRender) ++active;
        else if (!enabled) ++disabled;
        else if (mute) ++muted;
        else if (soloActive && !solo) ++hidden;

        // MVP step 4: expand the path through both substitutors. Step 5C
        // then writes a synthetic EXR to that path for every ACTIVE pass.
        const std::string nameStd = name.toStdString();
        const std::string rawPath = filePath.toStdString();
        const std::string resolved = expandPassPath(rawPath, nameStd, dbgFrame);

        fprintf(stderr, "[PassManager]   #%d %-8s name='%s' type='%s' samples=%d aovs=[%s]\n",
                i, statusTag,
                name.toUtf8().constData(),
                type.toUtf8().constData(),
                samples,
                aovList.join(QLatin1Char(',')).toUtf8().constData());
        fprintf(stderr, "[PassManager]       raw      = %s\n", rawPath.c_str());
        fprintf(stderr, "[PassManager]       resolved = %s\n", resolved.c_str());

        if (wouldRender) {
            ActiveSpec spec;
            spec.index        = i;
            spec.name         = nameStd;
            spec.samples      = samples;
            spec.rawPath      = rawPath;
            spec.format       = p.value(QStringLiteral("format")).toString().toStdString();
            spec.bitDepth     = p.value(QStringLiteral("bitDepth")).toString().toStdString();
            spec.compression  = p.value(QStringLiteral("compression")).toString().toStdString();
            spec.candidateLights  = p.value(QStringLiteral("candidateLights")).toString().toStdString();
            spec.excludeLights    = p.value(QStringLiteral("excludeLights")).toString().toStdString();
            spec.soloLight        = p.value(QStringLiteral("soloLight")).toString().toStdString();
            spec.candidateObjects     = p.value(QStringLiteral("candidateObjects")).toString().toStdString();
            spec.excludeObjects       = p.value(QStringLiteral("excludeObjects")).toString().toStdString();
            spec.soloObject           = p.value(QStringLiteral("soloObject")).toString().toStdString();
            spec.cameraOverride       = p.value(QStringLiteral("cameraOverride")).toString().toStdString();
            spec.materialOverride     = p.value(QStringLiteral("materialOverride")).toString().toStdString();
            spec.shadowCatcherObjects = p.value(QStringLiteral("shadowCatcherObjects")).toString().toStdString();
            spec.holdoutObjects       = p.value(QStringLiteral("holdoutObjects")).toString().toStdString();
            spec.traceObjects         = p.value(QStringLiteral("traceObjects")).toString().toStdString();
            spec.aovs.reserve(aovList.size());
            for (const QString& a : aovList) spec.aovs.push_back(a.toStdString());
            activeSpecs.push_back(std::move(spec));
        }
    }

    fprintf(stderr, "[PassManager] Summary: %d active / %d disabled / %d muted / %d hidden of %d total\n",
            active, disabled, muted, hidden, total);

    if (activeSpecs.empty()) {
        fprintf(stderr, "[PassManager] Nothing to render.\n");
        fflush(stderr);
        return true;
    }

    // Scene-light diagnostic — reuses the enumeration we already did above.
    if (sceneLights.empty()) {
        fprintf(stderr, "[PassManager] Scene lights: <none found>\n");
    } else {
        fprintf(stderr, "[PassManager] Scene lights (%d):\n", (int)sceneLights.size());
        for (const auto& li : sceneLights) {
            fprintf(stderr,
                    "[PassManager]   scriptName='%s'  lightGroup='%s' → produces AOV '%s'\n",
                    li.scriptName.c_str(),
                    li.lightGroup.empty() ? "(none)" : li.lightGroup.c_str(),
                    li.lightGroup.empty()
                        ? "(no Combined_<group>)"
                        : (std::string("Combined_") + li.lightGroup).c_str());
        }
    }

    // Scene-geo diagnostic — same role for candidateObjects/excludeObjects/soloObject.
    if (sceneGeo.empty()) {
        fprintf(stderr, "[PassManager] Scene objects: <none found>\n");
    } else {
        fprintf(stderr, "[PassManager] Scene objects (%d):\n", (int)sceneGeo.size());
        for (const auto& g : sceneGeo) {
            fprintf(stderr, "[PassManager]   scriptName='%s'\n", g.scriptName.c_str());
        }
    }

    // Project-camera diagnostic — every CameraProvider-derived node in the
    // project tree, whether wired up or not. Use these names verbatim in
    // a pass's cameraOverride field.
    {
        std::vector<ProjectCameraInfo> projCams;
        enumerateProjectCameras(callerEffect, projCams);
        if (projCams.empty()) {
            fprintf(stderr, "[PassManager] Project cameras: <none found>\n");
        } else {
            fprintf(stderr, "[PassManager] Project cameras (%d):\n", (int)projCams.size());
            for (const auto& c : projCams) {
                fprintf(stderr, "[PassManager]   fullName='%s'\n", c.fullName.c_str());
            }
        }
    }

    // Project-material diagnostic — every MaterialProvider node in the
    // project tree that isn't a geo wrapper (i.e. genuine Material3D-like
    // sources). Use these names verbatim in a pass's materialOverride.
    {
        std::vector<ProjectMaterialInfo> projMats;
        enumerateProjectMaterials(callerEffect, projMats);
        if (projMats.empty()) {
            fprintf(stderr, "[PassManager] Project materials: <none found>\n");
        } else {
            fprintf(stderr, "[PassManager] Project materials (%d):\n", (int)projMats.size());
            for (const auto& m : projMats) {
                fprintf(stderr, "[PassManager]   fullName='%s'\n", m.fullName.c_str());
            }
        }
    }

    // ---- Frame iteration ----
    int firstFrame = dbgFrame;
    int lastFrame  = dbgFrame;
    int frameStep  = 1;
    if (mode == eFrameModeRange || mode == eFrameModeRangeNoReRender) {
        firstFrame = frameStart;
        lastFrame  = frameEnd;
        frameStep  = (frameInc > 0) ? frameInc : 1;
        if (firstFrame > lastFrame) std::swap(firstFrame, lastFrame);
    }

    const char* modeLabel =
        mode == eFrameModeCurrent          ? "Current"        :
        mode == eFrameModeRange            ? "Range"          :
                                              "Range NoReRender";
    int outW = 1920, outH = 1080;
    resolveOutputDimensions(callerEffect, outW, outH);
    fprintf(stderr, "[PassManager] Frame mode: %s, range=[%d..%d step %d], output=%dx%d\n",
            modeLabel, firstFrame, lastFrame, frameStep, outW, outH);

    // Settings provider (input 3, optional) — applies uniformly to every
    // batch in every frame. Per-pass JSON `samples` still overrides the
    // provider's getSamples() for that pass; everything else (DOF, MB,
    // integrator) comes from the provider when connected.
    const CyclesRenderSettings* settings = nullptr;
    {
        EffectInstancePtr settingsEffect = callerEffect ? callerEffect->getInput(3) : EffectInstancePtr();
        if (settingsEffect) {
            settings = dynamic_cast<const CyclesRenderSettings*>(settingsEffect.get());
        }
    }
    fprintf(stderr, "[PassManager] Settings input: %s\n",
            settings ? "connected (provider drives DOF/MB/integrator)"
                     : "not connected (renderer defaults)");

    int framesRendered = 0;
    int framesSkipped  = 0;
    for (int f = firstFrame; f <= lastFrame; f += frameStep) {
        const int rc = renderFrameForBatches(callerEffect, activeSpecs, sceneGeo,
                                              settings, f, mode, outW, outH);
        if (rc < 0) {
            fprintf(stderr, "[PassManager] Frame %d aborted due to render failure.\n", f);
        } else if (rc == 0 && mode == eFrameModeRangeNoReRender) {
            ++framesSkipped;
        } else {
            ++framesRendered;
        }
    }
    if (firstFrame != lastFrame) {
        fprintf(stderr, "[PassManager] Range done: %d frame(s) rendered, %d skipped (existing files).\n",
                framesRendered, framesSkipped);
    }
    fflush(stderr);
    return true;
}

static int
renderFrameForBatches(EffectInstance*                  effect,
                       const std::vector<ActiveSpec>&   activeSpecs,
                       const std::vector<SceneGeoInfo>& allSceneGeo,
                       const CyclesRenderSettings*    settings,
                       int                              frame,
                       FrameMode                        mode,
                       int                              width,
                       int                              height)
{
    // Resolve all paths for this frame up front. Used by both the
    // existence-check (No Re-render mode) and the per-pass save step.
    std::vector<std::string> resolvedPaths(activeSpecs.size());
    for (size_t i = 0; i < activeSpecs.size(); ++i) {
        resolvedPaths[i] = expandPassPath(activeSpecs[i].rawPath,
                                           activeSpecs[i].name, frame);
    }

    // No Re-render: if every output file already exists, skip the whole
    // frame. Per-batch granularity is an optimization for later.
    if (mode == eFrameModeRangeNoReRender) {
        bool allExist = true;
        for (const auto& p : resolvedPaths) {
            if (!QFileInfo::exists(QString::fromStdString(p))) {
                allExist = false;
                break;
            }
        }
        if (allExist) {
            fprintf(stderr, "[PassManager] Frame %d — all %d output(s) exist, skipping (No Re-render).\n",
                    frame, (int)resolvedPaths.size());
            return 0;
        }
    }

    // ---- Batching ----
    // Two passes share a Cycles session iff they share the same batch
    // key — currently samples + active-lights set + visible-objects set +
    // camera override. Future per-pass overrides (shader override, ...)
    // extend the key by being concatenated into batchKeyFor() above.
    // Within a batch the Cycles session renders the UNION of all batched
    // passes' AOV lists; per-pass output files are then demuxed via
    // filtered buffer maps before saveMultiLayerEXR runs.
    std::map<std::string, std::vector<size_t>> batches;
    for (size_t i = 0; i < activeSpecs.size(); ++i) {
        batches[batchKeyFor(activeSpecs[i], allSceneGeo)].push_back(i);
    }
    fprintf(stderr, "[PassManager] Frame %d — batching: %d active pass(es) → %d batch(es) (key=samples+lights+objects+camera+material+sc+ho+tr)\n",
            frame, (int)activeSpecs.size(), (int)batches.size());

    int batchesRendered = 0;
    bool anyFailure = false;
    int batchIdx = 0;
    for (const auto& kv : batches) {
        ++batchIdx;
        const std::string& batchKey = kv.first;
        const std::vector<size_t>& specIdxs = kv.second;
        // All specs in this batch share key, so any one is representative.
        const ActiveSpec& batchHead = activeSpecs[specIdxs[0]];
        const int batchSamples = batchHead.samples;
        std::set<std::string> batchActiveLights;
        const bool batchScoped = resolveActiveLights(batchHead, batchActiveLights);
        std::set<std::string> batchVisibleObjects;
        const bool batchObjScoped = resolveActiveObjects(batchHead, allSceneGeo, batchVisibleObjects);
        const std::string batchCamName = trimCopy(batchHead.cameraOverride);
        const std::string batchMatName = trimCopy(batchHead.materialOverride);

        // Resolve the per-batch camera override (if any). An empty string
        // means "use the manager's input slot 2"; the helper handles that
        // case itself. A non-empty string MUST resolve to a CameraProvider
        // — if it doesn't, skip this batch with a clear error rather than
        // silently falling back (silent fallback would produce the wrong
        // angle with no indication to the user).
        const CameraProvider* batchCamOverride = nullptr;
        if (!batchCamName.empty()) {
            AppInstancePtr app = effect ? effect->getApp() : AppInstancePtr();
            NodePtr camNode;
            if (app) camNode = app->getNodeByFullySpecifiedName(batchCamName);
            EffectInstancePtr camFx = camNode ? camNode->getEffectInstance() : EffectInstancePtr();
            if (camFx) batchCamOverride = dynamic_cast<const CameraProvider*>(camFx.get());
            if (!batchCamOverride) {
                fprintf(stderr,
                        "[PassManager]   SKIP batch %d :: cameraOverride='%s' did not resolve to a CameraProvider node\n",
                        batchIdx, batchCamName.c_str());
                anyFailure = true;
                continue;
            }
        }

        // Union of AOVs across all specs in this batch.
        std::set<std::string> unionSet;
        for (size_t idx : specIdxs) {
            for (const auto& a : activeSpecs[idx].aovs) unionSet.insert(a);
        }
        std::vector<std::string> unionAovs(unionSet.begin(), unionSet.end());

        // Diagnostic header for this batch.
        std::string memberList;
        for (size_t idx : specIdxs) {
            if (!memberList.empty()) memberList += ", ";
            memberList += activeSpecs[idx].name;
        }
        std::string unionList;
        for (const auto& a : unionAovs) {
            if (!unionList.empty()) unionList += ",";
            unionList += a;
        }
        std::string lightSummary;
        if (!batchScoped) {
            lightSummary = "*";
        } else {
            for (const auto& n : batchActiveLights) {
                if (!lightSummary.empty()) lightSummary += ",";
                lightSummary += n;
            }
            if (lightSummary.empty()) lightSummary = "<none>";
        }
        std::string objectSummary;
        if (!batchObjScoped) {
            objectSummary = "*";
        } else {
            for (const auto& n : batchVisibleObjects) {
                if (!objectSummary.empty()) objectSummary += ",";
                objectSummary += n;
            }
            if (objectSummary.empty()) objectSummary = "<none>";
        }
        // Promotion summaries — built from the head spec's lists since
        // every member shares them by batch-key construction.
        auto joinList = [](const std::string& s) {
            std::string out;
            const std::set<std::string> set = parseNameList(s);
            for (const auto& n : set) {
                if (!out.empty()) out += ",";
                out += n;
            }
            return out.empty() ? std::string("<none>") : out;
        };
        const std::string scSummary  = joinList(batchHead.shadowCatcherObjects);
        const std::string hoSummary  = joinList(batchHead.holdoutObjects);
        const std::string trSummary  = joinList(batchHead.traceObjects);
        const std::string matSummary = batchMatName.empty() ? std::string("<none>") : batchMatName;
        fprintf(stderr, "[PassManager]   Batch %d/%d: samples=%d, lights=[%s], objects=[%s], cam=%s, mat=%s, sc=[%s], ho=[%s], tr=[%s], %d pass(es): [%s], union AOVs=[%s]\n",
                batchIdx, (int)batches.size(),
                batchSamples, lightSummary.c_str(), objectSummary.c_str(),
                batchCamName.empty() ? "<input2>" : batchCamName.c_str(),
                matSummary.c_str(),
                scSummary.c_str(), hoSummary.c_str(), trSummary.c_str(),
                (int)specIdxs.size(),
                memberList.c_str(), unionList.c_str());

        // Pre-create every output's parent directory.
        for (size_t idx : specIdxs) {
            const QString qpath = QString::fromStdString(resolvedPaths[idx]);
            const QString parent = QFileInfo(qpath).absolutePath();
            if (!parent.isEmpty()) QDir().mkpath(parent);
        }

        // Object scoping + visibility promotions: when ANY of object
        // scoping / shadow-catchers / holdouts / phantom (trace-only)
        // is configured, we must build a full ObjectVisibility map
        // covering EVERY scene object. The renderer treats "object
        // present in a non-null map" as a visibility flag carrier;
        // "object missing from a non-null map" is implicit
        // visibility(0). An unscoped batch passes nullptr.
        //
        // Promotion priority (per object):
        //   excluded (from object scoping) > shadow catcher > holdout >
        //   trace > default visible.
        //
        // 0x7FF mirrors RenderPass.cpp's PATH_RAY_ALL_VISIBILITY; 0x7FE
        // is the same with PATH_RAY_CAMERA (bit 0) cleared, matching
        // the phantom / trace-only convention.
        const std::set<std::string> scSet = parseNameList(batchHead.shadowCatcherObjects);
        const std::set<std::string> hoSet = parseNameList(batchHead.holdoutObjects);
        const std::set<std::string> trSet = parseNameList(batchHead.traceObjects);
        const bool batchHasVisMap =
            batchObjScoped || !scSet.empty() || !hoSet.empty() || !trSet.empty();

        std::map<std::string, ObjectVisibility> batchVisMap;
        if (batchHasVisMap) {
            for (const auto& g : allSceneGeo) {
                ObjectVisibility v;
                const bool inScope =
                    !batchObjScoped || batchVisibleObjects.count(g.scriptName) > 0;
                if (!inScope) {
                    v.rayVisibility   = 0;
                    v.isExcluded      = true;
                } else if (scSet.count(g.scriptName)) {
                    v.rayVisibility   = 0x7FF;
                    v.isShadowCatcher = true;
                } else if (hoSet.count(g.scriptName)) {
                    v.rayVisibility   = 0x7FF;
                    v.isHoldout       = true;
                } else if (trSet.count(g.scriptName)) {
                    v.rayVisibility   = 0x7FE; // ALL & ~PATH_RAY_CAMERA
                } else {
                    v.rayVisibility   = 0x7FF;
                }
                batchVisMap[g.scriptName] = v;
            }
        }

        // Resolve the per-batch material override (if any). Same hard-fail
        // semantics as cameraOverride — an unresolvable name aborts the
        // batch with a clear stderr line rather than silently rendering
        // with original materials. (batchMatName was hoisted up so the
        // per-batch diagnostic line could include it.)
        MaterialProvider* batchMatOverride = nullptr;
        if (!batchMatName.empty()) {
            AppInstancePtr app = effect ? effect->getApp() : AppInstancePtr();
            NodePtr matNode;
            if (app) matNode = app->getNodeByFullySpecifiedName(batchMatName);
            EffectInstancePtr matFx = matNode ? matNode->getEffectInstance() : EffectInstancePtr();
            if (matFx) {
                MaterialProvider* mp = dynamic_cast<MaterialProvider*>(matFx.get());
                if (mp && !mp->hasMaterialInput()) {
                    batchMatOverride = mp;
                }
            }
            if (!batchMatOverride) {
                fprintf(stderr,
                        "[PassManager]   SKIP batch %d :: materialOverride='%s' did not resolve to a MaterialProvider node (e.g. Material3D)\n",
                        batchIdx, batchMatName.c_str());
                anyFailure = true;
                continue;
            }
        }

        // Settings provider → DOF / MB / Integrator params for this batch.
        // The provider is global (applies to every batch in every frame);
        // per-pass JSON `samples` already wins for samples since that's
        // baked into batchSamples above. apertureSize for DOF is derived
        // from the camera's focal length + F-Stop, so when DOF is enabled
        // we resolve the active camera (override OR input 2) and compute it.
        CyclesRenderer::DOFParams        batchDof;
        CyclesRenderer::MotionBlurParams batchMb;
        CyclesRenderer::IntegratorParams batchInteg;
        if (settings) {
            if (settings->getDOFEnabled((double)frame)) {
                batchDof.enabled       = true;
                batchDof.focusDistance = (float)settings->getFocusDistance((double)frame);
                batchDof.blades        = settings->getBokehBlades((double)frame);
                batchDof.bladeRotation = (float)(settings->getBladeRotation((double)frame) * 3.14159265358979323846 / 180.0);
                const CameraProvider* dofCam = batchCamOverride;
                if (!dofCam) {
                    EffectInstancePtr camEffect = effect ? effect->getInput(2) : EffectInstancePtr();
                    if (camEffect) dofCam = dynamic_cast<const CameraProvider*>(camEffect.get());
                }
                if (dofCam) {
                    double fl    = dofCam->getCameraFocalLength((double)frame);
                    double fstop = dofCam->getCameraFStop((double)frame);
                    if (fstop < 0.1) fstop = 0.1;
                    batchDof.apertureSize = (float)(fl / (2.0 * fstop) / 1000.0); // mm to meters
                } else {
                    batchDof.enabled = false; // no camera → DOF disabled
                }
            }
            if (settings->getMotionBlurEnabled((double)frame)) {
                batchMb.enabled         = true;
                batchMb.shutterTime     = (float)settings->getShutterTime((double)frame);
                batchMb.shutterPosition = settings->getShutterPosition((double)frame);
            }
            batchInteg.maxBounces          = settings->getMaxBounces((double)frame);
            batchInteg.diffuseBounces      = settings->getDiffuseBounces((double)frame);
            batchInteg.glossyBounces       = settings->getGlossyBounces((double)frame);
            batchInteg.transmissionBounces = settings->getTransmissionBounces((double)frame);
        }

        // Render once for the whole batch with the union AOV list.
        CyclesPassRequest reqBatch;
        reqBatch.time            = (double)frame;
        reqBatch.view            = ViewIdx(0);
        reqBatch.width           = width;
        reqBatch.height          = height;
        reqBatch.samples         = batchSamples;
        reqBatch.requestedPasses = unionAovs;
        reqBatch.transparentBg   = false;
        // Light scoping: pass the resolved set in only when the batch is
        // actually scoped. An empty pointer means "all lights" — which is
        // distinct from an empty set (= "no lights at all"). The renderer
        // skips the filter check entirely when activeLights is nullptr.
        if (batchScoped)      reqBatch.activeLights     = &batchActiveLights;
        if (batchHasVisMap)   reqBatch.visMap           = &batchVisMap;
        if (batchCamOverride) reqBatch.cameraOverride   = batchCamOverride;
        if (batchMatOverride) reqBatch.materialOverride = batchMatOverride;
        if (settings) {
            if (batchDof.enabled) reqBatch.dof = &batchDof;
            if (batchMb.enabled)  reqBatch.mb  = &batchMb;
            reqBatch.integrator = &batchInteg;
        }

        std::map<std::string, std::vector<float>> passBuffers;
        std::string err;
        const bool rendered = renderCyclesPassesForEffect(
            effect, reqBatch, passBuffers, err);
        if (!rendered) {
            fprintf(stderr, "[PassManager]   RENDER FAILED batch %d :: %s\n",
                    batchIdx, err.c_str());
            anyFailure = true;
            continue;
        }
        ++batchesRendered;

        // Demux: for each pass in this batch, dispatch by file extension.
        //   .exr → multi-layer EXR with the pass's AOV list as layers.
        //   .png / .tif{f} / .jpg{,eg} → single-image-per-AOV. When the
        //     pass has more than one AOV, _<AOV> is injected before the
        //     extension so the AOVs don't overwrite each other.
        // Falls through to multi-layer EXR when the extension is unknown
        // (legacy behavior). Per-pass bit-depth / compression strings are
        // passed through verbatim; non-EXR formats reinterpret them.
        for (size_t idx : specIdxs) {
            const ActiveSpec& spec = activeSpecs[idx];
            CyclesRenderer::ExrOutputOptions opts;
            opts.bitDepth    = spec.bitDepth;
            opts.compression = spec.compression;

            const std::string& outPath = resolvedPaths[idx];
            const std::string ext = [&]() {
                size_t dot = outPath.rfind('.');
                if (dot == std::string::npos) return std::string();
                std::string e = outPath.substr(dot + 1);
                for (char& ch : e) ch = (char)std::tolower((unsigned char)ch);
                return e;
            }();
            const bool isSingleFormat = (ext == "png" || ext == "tif" ||
                                          ext == "tiff" || ext == "jpg" ||
                                          ext == "jpeg");

            if (!isSingleFormat) {
                std::map<std::string, std::vector<float>> filtered;
                for (const auto& aov : spec.aovs) {
                    auto it = passBuffers.find(aov);
                    if (it != passBuffers.end()) filtered[aov] = it->second;
                }
                const bool saved = CyclesRenderer::saveMultiLayerEXR(
                    outPath, filtered, reqBatch.width, reqBatch.height, opts);
                if (saved) {
                    fprintf(stderr, "[PassManager]   WROTE EXR  %s\n", outPath.c_str());
                } else {
                    fprintf(stderr, "[PassManager]   SAVE FAILED %s\n", outPath.c_str());
                }
                continue;
            }

            // PNG/TIFF/JPEG: write one image per AOV. The path gets
            // "_<AOV>" injected before the extension when the pass owns
            // more than one AOV; single-AOV passes write to the path as-is.
            const bool multiAov = spec.aovs.size() > 1;
            for (const auto& aov : spec.aovs) {
                auto it = passBuffers.find(aov);
                if (it == passBuffers.end()) continue;
                std::string perAovPath = outPath;
                if (multiAov) {
                    size_t dot = perAovPath.rfind('.');
                    const std::string suffix = std::string("_") + aov;
                    if (dot == std::string::npos) perAovPath += suffix;
                    else perAovPath.insert(dot, suffix);
                }
                const bool isCombined = (aov == "Combined");
                const bool saved = CyclesRenderer::saveSingleImage(
                    perAovPath, it->second, reqBatch.width, reqBatch.height,
                    isCombined, opts);
                if (saved) {
                    fprintf(stderr, "[PassManager]   WROTE %s  %s\n",
                            ext.c_str(), perAovPath.c_str());
                } else {
                    fprintf(stderr, "[PassManager]   SAVE FAILED %s\n",
                            perAovPath.c_str());
                }
            }
        }
    }

    return anyFailure ? -1 : batchesRendered;
}

void
CyclesRenderPassManager::discoverSceneObjects(std::vector<std::string>& outGeo,
                                              std::vector<std::string>& outLights) const
{
    outGeo.clear();
    outLights.clear();

    double t = 0.;
    if (getApp() && getApp()->getTimeLine()) {
        t = getApp()->getTimeLine()->currentFrame();
    }

    // Reuse the same dynamic_cast walk the renderer uses, so the picker names
    // match exactly what the ObjectVisibility map keys on at render time.
    std::vector<SceneGeoInfo>   geo;
    std::vector<SceneLightInfo> lights;
    enumerateSceneGeo(const_cast<CyclesRenderPassManager*>(this), t, geo);
    enumerateSceneLights(const_cast<CyclesRenderPassManager*>(this), t, lights);

    for (size_t i = 0; i < geo.size(); ++i)    outGeo.push_back(geo[i].scriptName);
    for (size_t i = 0; i < lights.size(); ++i) outLights.push_back(lights[i].scriptName);
}

bool
CyclesRenderPassManager::knobChanged(KnobI* k,
                                     ValueChangedReasonEnum /*reason*/,
                                     ViewSpec /*view*/,
                                     double time,
                                     bool /*originatedFromMainThread*/)
{
    KnobButtonPtr reset = _imp->resetToDefault.lock();
    if (reset && reset.get() == k) {
        KnobPassTablePtr passes = _imp->passesJson.lock();
        if (passes) {
            passes->setValue(kDefaultPassesJson);
        }
        return true;
    }

    KnobButtonPtr submit = _imp->renderToDisk.lock();
    if (submit && submit.get() == k) {
        KnobPassTablePtr passes = _imp->passesJson.lock();
        if (!passes) return true;
        // Resolve the render time. Prefer the current timeline frame so
        // a button press doesn't render whatever stale frame value Natron
        // passed into knobChanged.
        double renderTime = time;
        if (getApp() && getApp()->getTimeLine()) {
            renderTime = getApp()->getTimeLine()->currentFrame();
        }
        // Read frame range knobs.
        FrameMode mode = eFrameModeCurrent;
        int frameStart = (int)renderTime;
        int frameEnd   = (int)renderTime;
        int frameInc   = 1;
        if (KnobChoicePtr fm = _imp->frameMode.lock())     mode       = (FrameMode)fm->getValue();
        if (KnobIntPtr    fs = _imp->frameStart.lock())     frameStart = fs->getValue();
        if (KnobIntPtr    fe = _imp->frameEnd.lock())       frameEnd   = fe->getValue();
        if (KnobIntPtr    fi = _imp->frameIncrement.lock()) frameInc   = fi->getValue();
        parseAndDumpActivePasses(this, renderTime, passes->getValue(),
                                  mode, frameStart, frameEnd, frameInc);
        return true;
    }

    return false;
}

// Map a Cycles pass name to its viewer plane. Mirrors CyclesRender's
// passNameToPlane so the preview's layer names match the beauty renderer.
static ImagePlaneDesc
ppmPassNameToPlane(const std::string& name)
{
    if (name == "Combined") return ImagePlaneDesc::getRGBAComponents();
    static const char* rgb3[] = {"R", "G", "B"};
    static const char* rgba4[] = {"R", "G", "B", "A"};
    if (name == "DiffDir")  return ImagePlaneDesc("DiffuseDirect",   "Diffuse Direct",   "", rgb3, 3);
    if (name == "DiffInd")  return ImagePlaneDesc("DiffuseIndirect", "Diffuse Indirect", "", rgb3, 3);
    if (name == "DiffCol")  return ImagePlaneDesc("DiffuseColor",    "Diffuse Color",    "", rgb3, 3);
    if (name == "GlossDir") return ImagePlaneDesc("GlossyDirect",    "Glossy Direct",    "", rgb3, 3);
    if (name == "GlossInd") return ImagePlaneDesc("GlossyIndirect",  "Glossy Indirect",  "", rgb3, 3);
    if (name == "GlossCol") return ImagePlaneDesc("GlossyColor",     "Glossy Color",     "", rgb3, 3);
    if (name == "TransDir") return ImagePlaneDesc("TransmissionDirect",   "Transmission Direct",   "", rgb3, 3);
    if (name == "TransInd") return ImagePlaneDesc("TransmissionIndirect", "Transmission Indirect", "", rgb3, 3);
    if (name == "TransCol") return ImagePlaneDesc("TransmissionColor",    "Transmission Color",    "", rgb3, 3);
    if (name == "Emit")     return ImagePlaneDesc("Emission",        "Emission",         "", rgb3, 3);
    if (name == "Env")      return ImagePlaneDesc("Environment",     "Environment",      "", rgb3, 3);
    if (name == "Normal")   return ImagePlaneDesc("Normal",          "Normal",           "", rgb3, 3);
    if (name == "UV")       return ImagePlaneDesc("UV",              "UV",               "", rgb3, 3);
    if (name == "AO")       return ImagePlaneDesc("AO",              "Ambient Occlusion","", rgb3, 3);
    if (name == "Depth")    return ImagePlaneDesc("Depth",           "Depth",            "", rgb3, 3);
    if (name == "Mist")     return ImagePlaneDesc("Mist",            "Mist",             "", rgb3, 3);
    if (name == "ShadowCatcherMatte")
        return ImagePlaneDesc("ShadowCatcherMatte", "Shadow Catcher Matte", "", rgba4, 4);
    if (name == "ShadowCatcher")
        return ImagePlaneDesc("ShadowCatcher", "Shadow Catcher", "", rgb3, 3);
    if (name == "ShadowCatcherSampleCount")
        return ImagePlaneDesc("ShadowCatcherSampleCount", "SC Sample Count", "", rgb3, 3);
    if (name.substr(0, 9) == "Combined_") {
        const std::string grp = name.substr(9);
        return ImagePlaneDesc("LightGroup_" + grp, "LightGroup " + grp, "", rgb3, 3);
    }
    return ImagePlaneDesc::getRGBAComponents();
}

// Read the currently-previewed pass's AOV names (skips tokens; ensures
// "Combined" is present). Returns false if no valid preview pass.
static bool
ppmPreviewAovNames(const KnobIntWPtr& previewIdxKnob,
                   const KnobPassTableWPtr& passesKnob,
                   std::vector<std::string>& outAovs)
{
    outAovs.clear();
    int idx = -1;
    if (KnobIntPtr pk = previewIdxKnob.lock()) idx = pk->getValue();
    if (idx < 0) return false;
    KnobPassTablePtr pj = passesKnob.lock();
    if (!pj) return false;

    const QJsonDocument doc =
        QJsonDocument::fromJson(QString::fromStdString(pj->getValue()).toUtf8());
    if (!doc.isArray()) return false;
    const QJsonArray arr = doc.array();
    if (idx >= arr.size() || !arr[idx].isObject()) return false;

    bool haveCombined = false;
    const QJsonArray aovs = arr[idx].toObject().value(QStringLiteral("aovs")).toArray();
    for (const QJsonValue& v : aovs) {
        const std::string nm = v.toString().toStdString();
        if (nm.empty() || nm[0] == '@') continue;   // skip magic tokens
        if (nm == "Combined") haveCombined = true;
        outAovs.push_back(nm);
    }
    if (!haveCombined) outAovs.insert(outAovs.begin(), std::string("Combined"));
    return true;
}

void
CyclesRenderPassManager::getComponentsNeededAndProduced(double /*time*/,
                                                        ViewIdx /*view*/,
                                                        EffectInstance::ComponentsNeededMap* comps,
                                                        double* passThroughTime,
                                                        int* passThroughView,
                                                        int* passThroughInput)
{
    std::list<ImagePlaneDesc> produced;
    produced.push_back(ImagePlaneDesc::getRGBAComponents());   // Color/Combined always

    std::vector<std::string> aovs;
    if (ppmPreviewAovNames(_imp->previewPassIndex, _imp->passesJson, aovs)) {
        for (size_t i = 0; i < aovs.size(); ++i) {
            if (aovs[i] == "Combined") continue;               // already added as Color
            produced.push_back(ppmPassNameToPlane(aovs[i]));
        }
    }

    (*comps)[-1] = produced;
    *passThroughTime  = 0;
    *passThroughView  = 0;
    *passThroughInput = 0;
}

StatusEnum
CyclesRenderPassManager::getRegionOfDefinition(U64 /*hash*/,
                                                double /*time*/,
                                                const RenderScale& /*scale*/,
                                                ViewIdx /*view*/,
                                                RectD* rod)
{
    // Match the project/render format so a previewed pass shows at the right
    // resolution. Falls back to 1920x1080 if the project has no default format.
    int w = 1920, h = 1080;
    resolveOutputDimensions(this, w, h);
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = w; rod->y2 = h;
    return eStatusOK;
}

StatusEnum
CyclesRenderPassManager::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;

    // --- Live preview of the selected pass, MULTI-PLANE (Step B) ---
    // Render the selected pass once with ALL its AOVs, then fill each output
    // plane from its matching pass buffer so the Viewer's layer dropdown lists
    // them. previewPassIndex < 0 → no preview → every plane transparent black.
    std::vector<std::string> aovs;
    const bool hasPreview = ppmPreviewAovNames(_imp->previewPassIndex, _imp->passesJson, aovs);

    std::map<std::string, std::vector<float> > buffers;
    int srcW = 0, srcH = 0;

    if (hasPreview) {
        int previewIdx = -1;
        if (KnobIntPtr pk = _imp->previewPassIndex.lock()) previewIdx = pk->getValue();
        KnobPassTablePtr pj = _imp->passesJson.lock();
        QJsonObject pass;
        bool gotPass = false;
        if (previewIdx >= 0 && pj) {
            const QJsonDocument doc =
                QJsonDocument::fromJson(QString::fromStdString(pj->getValue()).toUtf8());
            if (doc.isArray()) {
                const QJsonArray arr = doc.array();
                if (previewIdx < arr.size() && arr[previewIdx].isObject()) {
                    pass = arr[previewIdx].toObject();
                    gotPass = true;
                }
            }
        }
        if (gotPass) {
            int w = 1920, h = 1080;
            resolveOutputDimensions(this, w, h);
            const int samples = pass.value(QStringLiteral("samples")).toInt(128);

            // Per-pass scoping → ObjectVisibility map (mirror of the disk
            // path's renderFrameForBatches construction).
            auto strOf = [&](const char* key) {
                return trimCopy(pass.value(QLatin1String(key)).toString().toStdString());
            };
            const std::set<std::string> scSet = parseNameList(strOf("shadowCatcherObjects"));
            const std::set<std::string> hoSet = parseNameList(strOf("holdoutObjects"));
            const std::set<std::string> trSet = parseNameList(strOf("traceObjects"));
            const std::set<std::string> cand  = parseNameList(strOf("candidateObjects"));
            const std::set<std::string> excl  = parseNameList(strOf("excludeObjects"));
            const std::string soloObj = strOf("soloObject");

            std::vector<SceneGeoInfo> geo;
            enumerateSceneGeo(this, args.time, geo);

            std::set<std::string> visibleSet;
            bool objScoped = false;
            if (!soloObj.empty()) {
                objScoped = true; visibleSet.insert(soloObj);
            } else if (!cand.empty()) {
                objScoped = true;
                for (const auto& c : cand) if (!excl.count(c)) visibleSet.insert(c);
            } else if (!excl.empty()) {
                objScoped = true;
                for (const auto& g : geo) if (!excl.count(g.scriptName)) visibleSet.insert(g.scriptName);
            }

            std::map<std::string, ObjectVisibility> visMap;
            const bool hasVisMap =
                objScoped || !scSet.empty() || !hoSet.empty() || !trSet.empty();
            if (hasVisMap) {
                for (const auto& g : geo) {
                    ObjectVisibility v;
                    const bool inScope = !objScoped || visibleSet.count(g.scriptName) > 0;
                    if (!inScope) {
                        v.rayVisibility = 0; v.isExcluded = true;
                    } else if (scSet.count(g.scriptName)) {
                        v.rayVisibility = 0x7FF; v.isShadowCatcher = true;
                    } else if (hoSet.count(g.scriptName)) {
                        v.rayVisibility = 0x7FF; v.isHoldout = true;
                    } else if (trSet.count(g.scriptName)) {
                        v.rayVisibility = 0x7FE; // ALL & ~CAMERA (phantom)
                    } else {
                        v.rayVisibility = 0x7FF;
                    }
                    visMap[g.scriptName] = v;
                }
            }

            CyclesPassRequest req;
            req.time            = args.time;
            req.view            = args.view;
            req.width           = w;
            req.height          = h;
            req.samples         = samples;
            req.requestedPasses = aovs;        // render every AOV the pass declares
            req.transparentBg   = false;
            if (!visMap.empty()) req.visMap = &visMap;

            std::string err;
            if (renderCyclesPassesForEffect(this, req, buffers, err)) {
                srcW = w; srcH = h;
            } else {
                fprintf(stderr, "[PassManager] preview render failed: %s\n", err.c_str());
            }
        }
    }

    // Fill each output plane from its matching pass buffer (mirror CyclesRender:
    // no Y-flip, nearest-sample scale; Depth/Mist/AO broadcast to grayscale).
    for (const auto& planePair : args.outputPlanes) {
        const ImagePlaneDesc& planeDesc = planePair.first;
        ImagePtr planeImg = planePair.second;
        if (!planeImg) continue;

        std::string passName;
        for (size_t a = 0; a < aovs.size(); ++a) {
            if (ppmPassNameToPlane(aovs[a]).getPlaneID() == planeDesc.getPlaneID()) {
                passName = aovs[a];
                break;
            }
        }
        if (passName.empty() && planeDesc.getNumComponents() == 4) passName = "Combined";

        const RectI bounds = planeImg->getBounds();
        const int nComp = planeDesc.getNumComponents();
        Image::WriteAccess wa(planeImg.get());

        std::map<std::string, std::vector<float> >::const_iterator it =
            passName.empty() ? buffers.end() : buffers.find(passName);

        if (srcW <= 0 || srcH <= 0 || it == buffers.end() || it->second.empty()) {
            for (int y = bounds.y1; y < bounds.y2; ++y) {
                for (int x = bounds.x1; x < bounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
                }
            }
            continue;
        }

        const std::vector<float>& src = it->second;
        const int dstW = bounds.width();
        const int dstH = bounds.height();
        const bool gray = (passName == "Mist" || passName == "AO");
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const int fbX = x - bounds.x1;
                const int fbY = y - bounds.y1;
                int sx = (srcW == dstW) ? fbX : (fbX * srcW / dstW);
                int sy = (srcH == dstH) ? fbY : (fbY * srcH / dstH);
                if (sx >= srcW) sx = srcW - 1;
                if (sy >= srcH) sy = srcH - 1;
                const int sidx = (sy * srcW + sx) * 4;
                float r = src[sidx + 0];
                float g = src[sidx + 1];
                float b = src[sidx + 2];
                float a = src[sidx + 3];
                if (passName == "Depth")      { if (r >= 1e9f) r = 0.0f; g = b = r; a = 1.0f; }
                else if (gray)                { g = b = r; a = 1.0f; }
                dst[0] = r;
                if (nComp > 1) dst[1] = g;
                if (nComp > 2) dst[2] = b;
                if (nComp > 3) dst[3] = a;
            }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_CyclesRenderPassManager.cpp"
