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
#include "../../KnobTypes.h"
#include "../../KnobFile.h"
#include "../../Node.h"
#include "../../Project.h"
#include "../../TimeLine.h"

#include "CyclesPassRender.h"
#include "CyclesRenderer.h"

NATRON_NAMESPACE_ENTER

struct CyclesRenderPassManagerPrivate
{
    // Pass list as a JSON string. Persisted in project files automatically
    // via Natron's knob serialization. Seed value covers the MVP 2-pass case
    // (one beauty + one data). User edits this directly in the panel for
    // MVP; custom widget lands in Phase 5+.
    KnobStringWPtr passesJson;
    KnobButtonWPtr resetToDefault;
    KnobButtonWPtr renderToDisk;

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

// Default seed JSON — two passes covering the MVP cases.
// Beauty: Combined only, default file path.
// Data:   Depth + Normal + UV (the comp utility bundle).
// Schema mirrors the data.js model from the UI demo, trimmed to fields
// the MVP submission actually consults.
static const char* kDefaultPassesJson =
    "[\n"
    "  {\n"
    "    \"id\": \"p1\",\n"
    "    \"name\": \"beauty_main\",\n"
    "    \"type\": \"bty\",\n"
    "    \"group\": \"beauty\",\n"
    "    \"enabled\": true,\n"
    "    \"solo\": false,\n"
    "    \"mute\": false,\n"
    "    \"output\": true,\n"
    "    \"aovs\": [\"Combined\"],\n"
    "    \"filePath\": \"$RENDER/$SHOT/$PASS/$SHOT_$PASS.####.exr\",\n"
    "    \"format\": \"EXR (Multilayer)\",\n"
    "    \"bitDepth\": \"16-bit Half\",\n"
    "    \"compression\": \"ZIP\",\n"
    "    \"samples\": 128\n"
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
    "    \"samples\": 128\n"
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

    // The pass list — JSON string, persisted in the project file. Multi-line
    // edit so users can edit directly in the panel until the custom widget
    // lands. Seed with a default 2-pass setup so a freshly-created node has
    // something usable.
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Passes (JSON)"));
        k->setName("passesJson");
        k->setAsMultiLine();
        k->setDefaultValue(kDefaultPassesJson);
        k->setHintToolTip(tr(
            "JSON-serialized list of render passes. Each entry is a pass spec "
            "with: id, name, type, group, enabled/solo/mute/output flags, "
            "aovs (array), filePath, format, bitDepth, compression, samples. "
            "Edit directly here for now; custom UI widget lands in a later phase. "
            "Use the Reset button to restore the default 2-pass seed."));
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
};

// Render one frame: re-resolve paths for `frame`, partition into batches,
// run one Cycles session per batch, demux per-pass and save EXR.
// Returns the number of batches actually rendered (0 if skipped or
// nothing to do; -1 if any render failed).
static int
renderFrameForBatches(EffectInstance*                effect,
                       const std::vector<ActiveSpec>& activeSpecs,
                       int                            frame,
                       FrameMode                      mode,
                       int                            width,
                       int                            height);

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

        // Build a short AOV list summary for the line.
        QStringList aovList;
        const QJsonArray aovs = p.value(QStringLiteral("aovs")).toArray();
        for (const QJsonValue& a : aovs) aovList << a.toString();

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

    int framesRendered = 0;
    int framesSkipped  = 0;
    for (int f = firstFrame; f <= lastFrame; f += frameStep) {
        const int rc = renderFrameForBatches(callerEffect, activeSpecs, f, mode, outW, outH);
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
renderFrameForBatches(EffectInstance*                effect,
                       const std::vector<ActiveSpec>& activeSpecs,
                       int                            frame,
                       FrameMode                      mode,
                       int                            width,
                       int                            height)
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
    // MVP rule: two passes share a Cycles session iff their `samples`
    // value matches. (Future per-pass overrides — camera, visibility
    // map, shader override, light sets — extend the key by being
    // concatenated into the bucket identifier here.) Within a batch,
    // the Cycles session renders the UNION of all batched passes' AOV
    // lists; per-pass output files are then demuxed via filtered
    // buffer maps before saveMultiLayerEXR runs.
    std::map<int, std::vector<size_t>> batches; // samples -> indices into activeSpecs
    for (size_t i = 0; i < activeSpecs.size(); ++i) {
        batches[activeSpecs[i].samples].push_back(i);
    }
    fprintf(stderr, "[PassManager] Frame %d — batching: %d active pass(es) → %d batch(es) (key=samples)\n",
            frame, (int)activeSpecs.size(), (int)batches.size());

    int batchesRendered = 0;
    bool anyFailure = false;
    int batchIdx = 0;
    for (const auto& kv : batches) {
        ++batchIdx;
        const int batchSamples = kv.first;
        const std::vector<size_t>& specIdxs = kv.second;

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
        fprintf(stderr, "[PassManager]   Batch %d/%d: samples=%d, %d pass(es): [%s], union AOVs=[%s]\n",
                batchIdx, (int)batches.size(),
                batchSamples, (int)specIdxs.size(),
                memberList.c_str(), unionList.c_str());

        // Pre-create every output's parent directory.
        for (size_t idx : specIdxs) {
            const QString qpath = QString::fromStdString(resolvedPaths[idx]);
            const QString parent = QFileInfo(qpath).absolutePath();
            if (!parent.isEmpty()) QDir().mkpath(parent);
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

        // Demux: for each pass in this batch, build a filtered map
        // containing only that pass's AOVs and hand it to
        // saveMultiLayerEXR with the pass's own bit-depth / compression.
        for (size_t idx : specIdxs) {
            const ActiveSpec& spec = activeSpecs[idx];
            std::map<std::string, std::vector<float>> filtered;
            for (const auto& aov : spec.aovs) {
                auto it = passBuffers.find(aov);
                if (it != passBuffers.end()) filtered[aov] = it->second;
            }
            CyclesRenderer::ExrOutputOptions opts;
            opts.bitDepth    = spec.bitDepth;
            opts.compression = spec.compression;
            const bool saved = CyclesRenderer::saveMultiLayerEXR(
                resolvedPaths[idx], filtered, reqBatch.width, reqBatch.height, opts);
            if (saved) {
                fprintf(stderr, "[PassManager]   WROTE EXR  %s\n",
                        resolvedPaths[idx].c_str());
            } else {
                fprintf(stderr, "[PassManager]   SAVE FAILED %s\n",
                        resolvedPaths[idx].c_str());
            }
        }
    }

    return anyFailure ? -1 : batchesRendered;
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
        KnobStringPtr passes = _imp->passesJson.lock();
        if (passes) {
            passes->setValue(kDefaultPassesJson);
        }
        return true;
    }

    KnobButtonPtr submit = _imp->renderToDisk.lock();
    if (submit && submit.get() == k) {
        KnobStringPtr passes = _imp->passesJson.lock();
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

StatusEnum
CyclesRenderPassManager::getRegionOfDefinition(U64 /*hash*/,
                                                double /*time*/,
                                                const RenderScale& /*scale*/,
                                                ViewIdx /*view*/,
                                                RectD* rod)
{
    // Sink node — render() never produces image data. Project default RoD
    // so the engine doesn't cache a failure (per
    // feedback-natron-input-optional-required, an early failure here
    // poisons the cache for later knob changes).
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1920; rod->y2 = 1080;
    return eStatusOK;
}

StatusEnum
CyclesRenderPassManager::render(const RenderActionArgs& args)
{
    // MVP step 1: write transparent black so any downstream viewer wired by
    // mistake gets a defined image instead of garbage. Real work lives in
    // the "Render to Disk" button handler (MVP step 3+), not the live
    // render path.
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
    int nComp = outImg->getComponents().getNumComponents();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;
            for (int c = 0; c < nComp; ++c) dst[c] = 0.0f;
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_CyclesRenderPassManager.cpp"
