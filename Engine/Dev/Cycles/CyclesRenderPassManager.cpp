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

#include <OpenImageIO/imageio.h>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../KnobTypes.h"
#include "../../KnobFile.h"
#include "../../Node.h"

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

    // MVP step 4 will add: token-resolver helper context.
    // MVP step 5+: per-pass output state, batching engine.

    CyclesRenderPassManagerPrivate()
    {}
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

// MVP step 5C — stub disk writer.
//
// Per AOV in the pass, look up the channel set the same way
// CyclesRenderer::saveMultiLayerEXR does (so the layer names look real
// when the EXR is opened in a Read node). Fill each channel set with a
// recognizable synthetic pattern so we can spot which file is which.
//
// Proves: token resolver → directory creation → OIIO write → readable EXR.
// Real Cycles render lands in step 5A after the refactor.
struct AovChannels {
    std::string prefix;       // EXR layer prefix; empty = "beauty" mode (R/G/B/A unprefixed)
    std::vector<std::string> chans;
};

static AovChannels
aovChannelDef(const std::string& aovName)
{
    if (aovName == "Combined")    return {"",                {"R", "G", "B", "A"}};
    if (aovName == "DiffDir")     return {"DiffuseDirect",   {"R", "G", "B"}};
    if (aovName == "DiffInd")     return {"DiffuseIndirect", {"R", "G", "B"}};
    if (aovName == "DiffCol")     return {"DiffuseColor",    {"R", "G", "B"}};
    if (aovName == "GlossDir")    return {"GlossyDirect",    {"R", "G", "B"}};
    if (aovName == "GlossInd")    return {"GlossyIndirect",  {"R", "G", "B"}};
    if (aovName == "GlossCol")    return {"GlossyColor",     {"R", "G", "B"}};
    if (aovName == "Emit")        return {"Emission",        {"R", "G", "B"}};
    if (aovName == "Env")         return {"Environment",     {"R", "G", "B"}};
    if (aovName == "AO")          return {"AO",              {"A"}};
    if (aovName == "Normal")      return {"Normal",          {"X", "Y", "Z"}};
    if (aovName == "Depth")       return {"depth",           {"Z"}};
    if (aovName == "UV")          return {"UV",              {"U", "V", "W"}};
    if (aovName == "Mist")        return {"Mist",            {"A"}};
    // Unknown AOV — best-effort 3-channel.
    return {aovName, {"R", "G", "B"}};
}

// 32-bit name hash used to colorize each AOV's synthetic pattern.
static uint32_t
hashName(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (char c : s) { h ^= (uint8_t)c; h *= 16777619u; }
    return h;
}

static bool
writeStubExr(const std::string& filepath,
             const std::vector<std::string>& aovs,
             std::string& errOut)
{
    if (aovs.empty()) {
        errOut = "no AOVs in pass";
        return false;
    }

    // Make the parent directory if needed.
    const QString qpath = QString::fromStdString(filepath);
    const QString parent = QFileInfo(qpath).absolutePath();
    if (!parent.isEmpty()) {
        QDir dir;
        if (!dir.mkpath(parent)) {
            errOut = "failed to create directory '" + parent.toStdString() + "'";
            return false;
        }
    }

    // Assemble channel list + total count.
    const int W = 64;
    const int H = 64;
    std::vector<std::string> chanNames;
    std::vector<AovChannels> defs;
    defs.reserve(aovs.size());
    for (const auto& a : aovs) {
        const AovChannels d = aovChannelDef(a);
        defs.push_back(d);
        for (const auto& c : d.chans) {
            chanNames.push_back(d.prefix.empty() ? c : (d.prefix + "." + c));
        }
    }
    const int totalCh = (int)chanNames.size();
    if (totalCh == 0) {
        errOut = "no channels resolved from AOV list";
        return false;
    }

    // Build the synthetic interleaved buffer. Each AOV's channels get a
    // distinct mid-tone color tinted by hashName(aov) so different passes
    // are visually distinguishable when opened in a Read node.
    std::vector<float> pixels((size_t)W * H * totalCh, 0.0f);
    int offset = 0;
    for (size_t a = 0; a < aovs.size(); ++a) {
        const AovChannels& d = defs[a];
        const uint32_t h = hashName(aovs[a]);
        const float tintR = 0.3f + ((h >>  0) & 0xFF) / 510.0f; // [0.3, 0.8]
        const float tintG = 0.3f + ((h >>  8) & 0xFF) / 510.0f;
        const float tintB = 0.3f + ((h >> 16) & 0xFF) / 510.0f;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t base = ((size_t)y * W + x) * totalCh + offset;
                for (size_t c = 0; c < d.chans.size(); ++c) {
                    // For 3-channel chunks: spread the tint across R/G/B.
                    // For 1-channel: use averaged tint.
                    // For 4-channel (Combined): R/G/B from tint, A = 1.
                    float v;
                    if (d.chans.size() >= 3) {
                        v = (c == 0) ? tintR : (c == 1) ? tintG : (c == 2) ? tintB : 1.0f;
                    } else {
                        v = (tintR + tintG + tintB) / 3.0f;
                    }
                    pixels[base + c] = v;
                }
            }
        }
        offset += (int)d.chans.size();
    }

    // Write through OIIO. Float32 multi-channel EXR with channel names.
    OIIO::ImageSpec spec(W, H, totalCh, OIIO::TypeDesc::FLOAT);
    spec.channelnames = chanNames;
    auto out = OIIO::ImageOutput::create(filepath);
    if (!out) {
        errOut = "OIIO::ImageOutput::create returned null for '" + filepath + "'";
        return false;
    }
    if (!out->open(filepath, spec)) {
        errOut = "ImageOutput::open failed: " + out->geterror();
        return false;
    }
    if (!out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) {
        errOut = "write_image failed: " + out->geterror();
        out->close();
        return false;
    }
    out->close();
    return true;
}

// MVP step 3: parse the passes JSON, filter the active set, dump to
// stderr. No rendering. Returns false on parse failure so the caller
// can surface a UI message later.
static bool
parseAndDumpActivePasses(const std::string& jsonStr)
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

    // MVP step 4: frame stub. Real submission (step 5+) iterates over a
    // frame range; for the parse-dump path we just expand at frame 1001
    // so the user can see token resolution working.
    const int dbgFrame = 1001;

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

        // Step 5C: stub write for actives. Real Cycles render replaces
        // this in step 5A.
        if (wouldRender) {
            std::vector<std::string> aovsStd;
            aovsStd.reserve(aovList.size());
            for (const QString& a : aovList) aovsStd.push_back(a.toStdString());

            std::string err;
            if (writeStubExr(resolved, aovsStd, err)) {
                fprintf(stderr, "[PassManager]       WROTE STUB EXR %s\n", resolved.c_str());
            } else {
                fprintf(stderr, "[PassManager]       WRITE FAILED   %s :: %s\n",
                        resolved.c_str(), err.c_str());
            }
        }
    }

    fprintf(stderr, "[PassManager] Summary: %d active / %d disabled / %d muted / %d hidden of %d total\n",
            active, disabled, muted, hidden, total);
    fflush(stderr);
    return true;
}

bool
CyclesRenderPassManager::knobChanged(KnobI* k,
                                     ValueChangedReasonEnum /*reason*/,
                                     ViewSpec /*view*/,
                                     double /*time*/,
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
        parseAndDumpActivePasses(passes->getValue());
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
