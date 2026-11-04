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

#include "Cryptomatte.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <QtCore/QMutex>
#include <QtCore/QPointF>

#include "CryptomatteHash.h"

#include "../../../Global/GLIncludes.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"

#ifdef NATRON_HAVE_OPENIMAGEIO
#include <OpenImageIO/imageio.h>
#endif

NATRON_NAMESPACE_ENTER

namespace {

enum OutputModeEnum
{
    eOutputMatteAlpha = 0,   // RGB from the Source, matte in alpha
    eOutputMatteRGBA,        // matte in every channel
    eOutputPreview           // keyable-surface colours in RGB, matte in alpha
};

// One Cryptomatte set on the input: the planes <prefix>00, <prefix>01, ... in rank order.
struct CryptoSet
{
    std::string prefix;
    std::vector<ImagePlaneDesc> planes;
};

// The names of one set, from the file's manifest.
struct Manifest
{
    std::string json;                               // as found in the header, "" if none
    std::string source;                             // where it came from, for the panel
    std::map<std::string, std::uint32_t> nameToHash; // name -> MurmurHash
    std::map<std::uint32_t, std::string> bitsToName; // layer ID bits -> name
};

// The bits an ID has inside a layer: the hash with the exponent nudge applied. Idempotent, so
// hex taken from a layer and hex taken from a manifest both resolve.
inline std::uint32_t
layerBits(std::uint32_t hash)
{
    return CryptoHash::floatToBits( CryptoHash::hashToFloat(hash) );
}

std::string
trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) {
        ++a;
    }
    while (b > a && std::isspace((unsigned char)s[b - 1])) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string
lower(const std::string& s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i) {
        r[i] = (char)std::tolower((unsigned char)r[i]);
    }
    return r;
}

// The Matte List split into its tokens: names and <hex> IDs separated by commas or new lines.
std::vector<std::string>
splitTokens(const std::string& list)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= list.size(); ++i) {
        const char c = i < list.size() ? list[i] : ',';
        if (c == ',' || c == '\n' || c == '\r' || c == ';') {
            const std::string t = trim(cur);
            if (!t.empty()) {
                out.push_back(t);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// The layer bits a token selects: "<2ce615c5>" is an ID, anything else is a name to hash.
bool
tokenToBits(const std::string& token, std::uint32_t* bits)
{
    if (token.size() >= 2 && token[0] == '<' && token[token.size() - 1] == '>') {
        std::uint32_t h;
        if (!CryptoHash::hexToHash(trim(token.substr(1, token.size() - 2)), &h)) {
            return false;
        }
        *bits = layerBits(h);
        return true;
    }
    *bits = layerBits( CryptoHash::hashName(token) );
    return true;
}

std::set<std::uint32_t>
selectedBits(const std::string& list)
{
    std::set<std::uint32_t> s;
    const std::vector<std::string> toks = splitTokens(list);
    for (size_t i = 0; i < toks.size(); ++i) {
        std::uint32_t b;
        if (tokenToBits(toks[i], &b)) {
            s.insert(b);
        }
    }
    return s;
}

// ---- a small JSON reader for the manifest: a flat object of string -> string.

void
appendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Reads the string starting at s[*pos] == '"'; leaves *pos after the closing quote.
bool
readJsonString(const std::string& s, size_t* pos, std::string* out)
{
    size_t i = *pos;
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    out->clear();
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') {
            *pos = i;
            return true;
        }
        if (c != '\\') {
            *out += c;
            continue;
        }
        if (i >= s.size()) {
            return false;
        }
        const char e = s[i++];
        switch (e) {
        case '"': *out += '"'; break;
        case '\\': *out += '\\'; break;
        case '/': *out += '/'; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'n': *out += '\n'; break;
        case 'r': *out += '\r'; break;
        case 't': *out += '\t'; break;
        case 'u': {
            if (i + 4 > s.size()) {
                return false;
            }
            unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), NULL, 16);
            i += 4;
            // a surrogate pair
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                const unsigned lo = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), NULL, 16);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            appendUtf8(*out, cp);
            break;
        }
        default:
            *out += e;
        }
    }
    return false;
}

void
parseManifest(const std::string& json, Manifest* m)
{
    m->nameToHash.clear();
    m->bitsToName.clear();
    size_t i = json.find('{');
    if (i == std::string::npos) {
        return;
    }
    ++i;
    while (i < json.size()) {
        while (i < json.size() && (std::isspace((unsigned char)json[i]) || json[i] == ',')) {
            ++i;
        }
        if (i >= json.size() || json[i] == '}') {
            break;
        }
        std::string name, hex;
        if (!readJsonString(json, &i, &name)) {
            break;
        }
        while (i < json.size() && (std::isspace((unsigned char)json[i]) || json[i] == ':')) {
            ++i;
        }
        if (!readJsonString(json, &i, &hex)) {
            break;
        }
        std::uint32_t h;
        if (CryptoHash::hexToHash(trim(hex), &h)) {
            m->nameToHash[name] = h;
            m->bitsToName[layerBits(h)] = name;
        }
    }
}

// ---- the upstream file (same approach as LensDefocus's camera-from-header).

// The nearest upstream node with a "filename" knob, through first inputs.
NodePtr
upstreamRead(EffectInstance* self)
{
    NodePtr n = self->getNode() ? self->getNode()->getInput(0) : NodePtr();
    for (int hop = 0; n && hop < 32; ++hop) {
        if (std::dynamic_pointer_cast<KnobFile>(n->getKnobByName("filename"))) {
            return n;
        }
        n = n->getInput(0);
    }
    return NodePtr();
}

// The file a Read reads at `frame`, held inside its frame range.
std::string
readFileAt(EffectInstance* self, const NodePtr& rd, int frame)
{
    KnobFilePtr fk = std::dynamic_pointer_cast<KnobFile>(rd->getKnobByName("filename"));
    if (!fk) {
        return std::string();
    }
    KnobIntPtr first = std::dynamic_pointer_cast<KnobInt>(rd->getKnobByName("firstFrame"));
    KnobIntPtr last = std::dynamic_pointer_cast<KnobInt>(rd->getKnobByName("lastFrame"));
    if (first && last && first->getValue() <= last->getValue()) {
        frame = std::max(first->getValue(), std::min(last->getValue(), frame));
    }
    std::string f = fk->getFileName(frame, ViewIdx(0));
    if (!f.empty() && self->getApp() && self->getApp()->getProject()) {
        self->getApp()->getProject()->canonicalizePath(f);
    }
    return f;
}

// Reads the manifest of the set `prefix` from the EXR header of `file` (or its sidecar).
void
manifestFromFile(const std::string& file, const std::string& prefix, Manifest* m)
{
    m->json.clear();
    m->source.clear();
#ifdef NATRON_HAVE_OPENIMAGEIO
    std::unique_ptr<OIIO::ImageInput> in = OIIO::ImageInput::open(file);
    if (!in) {
        (void)OIIO::geterror();
        m->source = "file could not be opened";
        return;
    }
    const OIIO::ImageSpec& sp = in->spec();
    // Blender's layer prefix carries the view layer ("ViewLayer.CryptoObject"); Natron's plane is
    // named the same. Other renderers name the set after the channel prefix too.
    std::string base;
    for (size_t i = 0; i < sp.extra_attribs.size(); ++i) {
        const OIIO::ParamValue& p = sp.extra_attribs[i];
        const std::string name = p.name().string();
        const std::string head = "cryptomatte/";
        const std::string tail = "/name";
        if (name.size() <= head.size() + tail.size() || name.compare(0, head.size(), head) != 0 ||
            name.compare(name.size() - tail.size(), tail.size(), tail) != 0) {
            continue;
        }
        const std::string value = sp.get_string_attribute(name);
        if (value == prefix || value + "." == prefix || value == prefix + ".") {
            base = name.substr(0, name.size() - tail.size() + 1);   // "cryptomatte/<id>/"
            break;
        }
    }
    if (base.empty()) {
        in->close();
        m->source = "no manifest for this layer in the EXR header";
        return;
    }
    m->json = sp.get_string_attribute(base + "manifest");
    if (!m->json.empty()) {
        m->source = "EXR header";
    } else {
        const std::string side = sp.get_string_attribute(base + "manif_file");
        if (!side.empty()) {
            std::string dir = file;
            const size_t slash = dir.find_last_of("/\\");
            dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
            std::ifstream f((dir + side).c_str(), std::ios::binary);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                m->json = ss.str();
                m->source = "sidecar " + side;
            } else {
                m->source = "sidecar " + side + " not found";
            }
        } else {
            m->source = "manifest entry empty";
        }
    }
    in->close();
#else
    (void)file;
    (void)prefix;
    m->source = "this build has no OpenImageIO: names cannot be listed";
#endif
    parseManifest(m->json, m);
}

// Set prefixes in a helpful order: object, material, asset, then the rest alphabetically.
int
prefixRank(const std::string& prefix)
{
    const std::string l = lower(prefix);
    if (l.find("object") != std::string::npos) {
        return 0;
    }
    if (l.find("material") != std::string::npos) {
        return 1;
    }
    if (l.find("asset") != std::string::npos) {
        return 2;
    }
    return 3;
}

bool
setLess(const CryptoSet& a, const CryptoSet& b)
{
    const int ra = prefixRank(a.prefix), rb = prefixRank(b.prefix);
    return ra != rb ? ra < rb : a.prefix < b.prefix;
}

} // anonymous namespace

struct CryptomattePrivate
{
    KnobChoiceWPtr layer;
    KnobStringWPtr matteList;
    KnobBoolWPtr pick;
    KnobChoiceWPtr output;
    KnobStringWPtr manifestInfo;
    KnobStringWPtr manifestNames;
    KnobButtonWPtr reload;

    mutable QMutex setsMutex;
    std::vector<CryptoSet> sets;

    mutable QMutex manifestMutex;
    Manifest manifest;
    std::string manifestKey;   // file + "|" + prefix the manifest was read for

    bool dragging;
    QPointF dragFrom, dragTo;
    int outputBeforePick;

    CryptomattePrivate()
        : setsMutex()
        , sets()
        , manifestMutex()
        , manifest()
        , manifestKey()
        , dragging(false)
        , dragFrom()
        , dragTo()
        , outputBeforePick(-1)
    {
    }

    // The set the Layer menu points at (the first one when the stored prefix is gone).
    bool currentSet(CryptoSet* out) const
    {
        KnobChoicePtr k = layer.lock();
        const std::string want = k ? k->getActiveEntry().id : std::string();
        QMutexLocker l(&setsMutex);
        if (sets.empty()) {
            return false;
        }
        for (size_t i = 0; i < sets.size(); ++i) {
            if (sets[i].prefix == want) {
                *out = sets[i];
                return true;
            }
        }
        *out = sets.front();
        return true;
    }
};

Cryptomatte::Cryptomatte(NodePtr node)
    : EffectInstance(node)
    , _imp(new CryptomattePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

Cryptomatte::~Cryptomatte()
{
}

std::string
Cryptomatte::getPluginDescription() const
{
    return "Decodes Cryptomatte ID mattes written by a renderer (Blender/Cycles, Arnold, Redshift, "
           "Houdini...).\n\n"
           "Read a multi-layer EXR and connect it: the Layer menu lists every Cryptomatte set found "
           "(object, material, asset). Type object names in the Matte List, separated by commas, or "
           "turn on Pick in viewer and click objects in the viewer (click again to remove, drag a box "
           "to add everything inside). Names come from the manifest in the EXR header; an object "
           "without a name can be given as its hex ID in angle brackets, <2ce615c5>.\n\n"
           "Output puts the matte in the alpha channel (RGB from the Source), in every channel, or "
           "shows the preview colours of all objects with the matte in alpha. Other planes pass "
           "through unchanged.\n\n"
           "Keep the Cryptomatte layers untouched between the Read and this node: the IDs are raw "
           "bit patterns, and nodes that filter, premultiply or colour-convert them break the matte.";
}

void
Cryptomatte::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
Cryptomatte::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Cryptomatte::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Cryptomatte::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Cryptomatte"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Layer"));
        k->setName("cryptoLayer");
        k->setHintToolTip(tr("The Cryptomatte set to decode. Each set is a run of planes named <prefix>00, "
                             "<prefix>01... on the Source (object, material and asset IDs are separate sets)."));
        page->addKnob(k);
        _imp->layer = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Matte List"));
        k->setName("matteList");
        k->setAsMultiLine();
        k->setHintToolTip(tr("The objects in the matte: names separated by commas or new lines, as the "
                             "renderer wrote them (see Manifest Names below). An object can also be given "
                             "as its ID in angle brackets, <2ce615c5>. Pick in viewer adds and removes "
                             "entries here."));
        page->addKnob(k);
        _imp->matteList = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Pick in viewer"));
        k->setName("pick");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("While on, a left click in the viewer adds the object under the cursor to the "
                             "Matte List, or removes it if it is already there; a left drag adds every object "
                             "inside the box (or removes them all if all were in). Switches the Output to "
                             "the preview colours while on."));
        page->addKnob(k);
        _imp->pick = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output"));
        k->setName("output");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("matteAlpha", "Matte in Alpha", "RGB from the Source's colour, the matte in alpha."));
        o.push_back(ChoiceOption("matteRGBA", "Matte in RGBA", "The matte in every channel."));
        o.push_back(ChoiceOption("preview", "Preview Colours", "Every object in its own colour (brighter when it is in the matte), the matte in alpha. The look other packages call keyable surface."));
        k->populateChoices(o);
        k->setDefaultValue(eOutputMatteAlpha);
        k->setHintToolTip(tr("What goes in the Color plane."));
        page->addKnob(k);
        _imp->output = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reload Manifest"));
        k->setName("reloadManifest");
        k->setHintToolTip(tr("Re-read the object names from the upstream file's header."));
        page->addKnob(k);
        _imp->reload = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Manifest"));
        k->setName("manifestInfo");
        k->setAsLabel();
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        page->addKnob(k);
        _imp->manifestInfo = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Manifest Names"));
        k->setName("manifestNames");
        k->setAsMultiLine();
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("Every object the renderer listed for this layer, one per line, for copying into "
                             "the Matte List. Read-only: edits here do nothing."));
        page->addKnob(k);
        _imp->manifestNames = k;
    }
}

StatusEnum
Cryptomatte::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale, ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) {
        return eStatusFailed;
    }
    bool isProject;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
}

void
Cryptomatte::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                            EffectInstance::ComponentsNeededMap* comps,
                                            double* passThroughTime,
                                            int* passThroughView,
                                            int* passThroughInput)
{
    (*comps)[0].push_back( ImagePlaneDesc::getRGBAComponents() );
    CryptoSet set;
    if (_imp->currentSet(&set)) {
        for (size_t i = 0; i < set.planes.size(); ++i) {
            (*comps)[0].push_back(set.planes[i]);
        }
    }
    (*comps)[-1].push_back( ImagePlaneDesc::getRGBAComponents() );
    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0;
}

void
Cryptomatte::onInputChanged(int inputNo)
{
    if (inputNo == 0) {
        refreshLayerChoices();
    }
}

void
Cryptomatte::onKnobsLoaded()
{
    refreshLayerChoices();
}

void
Cryptomatte::onMetadataRefreshed(const NodeMetadata& /*metadata*/)
{
    refreshLayerChoices();
}

void
Cryptomatte::refreshLayerChoices()
{
    std::list<ImagePlaneDesc> layers;
    double time = 0;
    if (getApp() && getApp()->getTimeLine()) {
        time = getApp()->getTimeLine()->currentFrame();
    }
    EffectInstancePtr input = getInput(0);
    if (input) {
        input->getAvailableLayers(time, ViewIdx(0), -1, &layers);
    }

    // Group the planes <prefix><two digits> by prefix, in rank order; keep the sets that start at 00.
    std::map<std::string, std::map<int, ImagePlaneDesc> > groups;
    for (std::list<ImagePlaneDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        const std::string id = it->getPlaneID();
        if (id.size() < 3 || it->getNumComponents() < 4) {
            continue;
        }
        const char a = id[id.size() - 2], b = id[id.size() - 1];
        if (!std::isdigit((unsigned char)a) || !std::isdigit((unsigned char)b)) {
            continue;
        }
        const std::string prefix = id.substr(0, id.size() - 2);
        if (lower(prefix).find("crypto") == std::string::npos) {
            continue;   // "Layer01"-style names are not ID layers
        }
        groups[prefix][(a - '0') * 10 + (b - '0')] = *it;
    }
    std::vector<CryptoSet> sets;
    for (std::map<std::string, std::map<int, ImagePlaneDesc> >::const_iterator g = groups.begin(); g != groups.end(); ++g) {
        if (g->second.find(0) == g->second.end()) {
            continue;
        }
        CryptoSet s;
        s.prefix = g->first;
        for (int rank = 0; ; ++rank) {
            std::map<int, ImagePlaneDesc>::const_iterator p = g->second.find(rank);
            if (p == g->second.end()) {
                break;
            }
            s.planes.push_back(p->second);
        }
        sets.push_back(s);
    }
    std::sort(sets.begin(), sets.end(), setLess);

    std::vector<ChoiceOption> entries;
    for (size_t i = 0; i < sets.size(); ++i) {
        std::ostringstream tip;
        tip << sets[i].planes.size() << " plane" << (sets[i].planes.size() == 1 ? "" : "s") << ", "
            << sets[i].planes.size() * 2 << " ID levels";
        entries.push_back(ChoiceOption(sets[i].prefix, sets[i].prefix, tip.str()));
    }
    {
        QMutexLocker l(&_imp->setsMutex);
        _imp->sets.swap(sets);
    }
    KnobChoicePtr k = _imp->layer.lock();
    if (k) {
        k->populateChoices(entries);
    }
    refreshManifest();
}

void
Cryptomatte::refreshManifest()
{
    CryptoSet set;
    const bool haveSet = _imp->currentSet(&set);
    std::string file;
    NodePtr rd = haveSet ? upstreamRead(this) : NodePtr();
    if (rd) {
        double time = 0;
        if (getApp() && getApp()->getTimeLine()) {
            time = getApp()->getTimeLine()->currentFrame();
        }
        file = readFileAt(this, rd, (int)std::floor(time + 0.5));
    }
    const std::string key = file + "|" + set.prefix;
    {
        QMutexLocker l(&_imp->manifestMutex);
        if (key == _imp->manifestKey) {
            return;
        }
        _imp->manifestKey = key;
    }

    Manifest m;
    if (!haveSet) {
        m.source = "no Cryptomatte layers on the Source";
    } else if (file.empty()) {
        m.source = "no Read upstream: type names, or pick in the viewer (IDs are written as <hex>)";
    } else {
        manifestFromFile(file, set.prefix, &m);
    }

    std::ostringstream info, names;
    if (m.nameToHash.empty()) {
        info << "No names (" << m.source << ")";
    } else {
        info << m.nameToHash.size() << " name" << (m.nameToHash.size() == 1 ? "" : "s") << " from the " << m.source;
        int n = 0;
        for (std::map<std::string, std::uint32_t>::const_iterator it = m.nameToHash.begin(); it != m.nameToHash.end(); ++it) {
            if (++n > 5000) {
                names << "... (" << m.nameToHash.size() - 5000 << " more)\n";
                break;
            }
            names << it->first << "\n";
        }
    }
    {
        QMutexLocker l(&_imp->manifestMutex);
        _imp->manifest = m;
    }
    KnobStringPtr ik = _imp->manifestInfo.lock();
    if (ik) {
        ik->setValue(info.str());
    }
    KnobStringPtr nk = _imp->manifestNames.lock();
    if (nk) {
        nk->setValue(names.str());
    }
}

std::string
Cryptomatte::getManifestJSON() const
{
    QMutexLocker l(&_imp->manifestMutex);
    return _imp->manifest.json;
}

bool
Cryptomatte::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    if (k == _imp->layer.lock().get()) {
        refreshManifest();
        return true;
    }
    if (k == _imp->reload.lock().get()) {
        {
            QMutexLocker l(&_imp->manifestMutex);
            _imp->manifestKey.clear();
        }
        refreshManifest();
        return true;
    }
    if (k == _imp->pick.lock().get()) {
        // Picking wants to see the objects: show the preview colours while it is on.
        KnobChoicePtr out = _imp->output.lock();
        if (out) {
            if (_imp->pick.lock()->getValue()) {
                _imp->outputBeforePick = out->getValue();
                out->setValue(eOutputPreview);
            } else if (_imp->outputBeforePick >= 0) {
                out->setValue(_imp->outputBeforePick);
                _imp->outputBeforePick = -1;
            }
        }
        redrawOverlayInteract();
        return true;
    }
    if (k == _imp->manifestNames.lock().get()) {
        return true;   // read-only list: nothing to render
    }
    return false;
}

StatusEnum
Cryptomatte::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) {
        return eStatusFailed;
    }
    ImagePtr outImg;
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        if (it->second && (it->first.isColorPlane() || it->first.getNumComponents() >= 1)) {
            outImg = it->second;
            break;
        }
    }
    if (!outImg) {
        return eStatusFailed;
    }
    const RectI outBounds = outImg->getBounds();
    const int outNComp = outImg->getComponents().getNumComponents();
    const int mode = _imp->output.lock()->getValue();

    CryptoSet set;
    const bool haveSet = _imp->currentSet(&set);
    if (!haveSet) {
        setPersistentMessage(eMessageTypeWarning, "No Cryptomatte layers on the Source: it needs planes named "
                                                  "<prefix>00, <prefix>01... (CryptoObject00, crypto_material00 ...).");
    } else {
        clearPersistentMessage(false);
    }

    // The Source colour (for RGB pass-through) and the ID planes.
    ImagePtr srcImg;
    if (mode == eOutputMatteAlpha || !haveSet) {
        srcImg = getImage(0, args.time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, NULL);
    }
    std::vector<ImagePtr> planeImgs;
    if (haveSet) {
        for (size_t i = 0; i < set.planes.size(); ++i) {
            ImagePtr im = getImage(0, args.time, args.mappedScale, args.view, NULL, &set.planes[i], false, false,
                                   eStorageModeRAM, 0, NULL);
            if (im && im->getComponents().getNumComponents() >= 4) {
                planeImgs.push_back(im);
            }
        }
    }

    const std::set<std::uint32_t> selected = selectedBits( _imp->matteList.lock()->getValue() );

    std::vector<std::unique_ptr<Image::ReadAccess> > planeRa;
    std::vector<RectI> planeBounds;
    for (size_t i = 0; i < planeImgs.size(); ++i) {
        planeRa.emplace_back(new Image::ReadAccess(planeImgs[i].get()));
        planeBounds.push_back(planeImgs[i]->getBounds());
    }
    std::unique_ptr<Image::ReadAccess> srcRa;
    RectI srcBounds;
    int srcNComp = 0;
    if (srcImg) {
        srcRa.reset(new Image::ReadAccess(srcImg.get()));
        srcBounds = srcImg->getBounds();
        srcNComp = srcImg->getComponents().getNumComponents();
    }
    Image::WriteAccess wa(outImg.get());

    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        if (aborted()) {
            return eStatusOK;
        }
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) {
                continue;
            }
            float matte = 0.f, pr = 0.f, pg = 0.f, pb = 0.f;
            for (size_t p = 0; p < planeRa.size(); ++p) {
                if (!planeBounds[p].contains(x, y)) {
                    continue;
                }
                const float* px = (const float*)planeRa[p]->pixelAt(x, y);
                if (!px) {
                    continue;
                }
                for (int pair = 0; pair < 2; ++pair) {
                    const float id = px[pair * 2], cov = px[pair * 2 + 1];
                    if (cov <= 0.f) {
                        continue;
                    }
                    const std::uint32_t bits = CryptoHash::floatToBits(id);
                    if (selected.count(bits)) {
                        matte += cov;
                    }
                    if (mode == eOutputPreview) {
                        float r, g, b;
                        CryptoHash::idToPreviewColor(bits, &r, &g, &b);
                        pr += cov * r;
                        pg += cov * g;
                        pb += cov * b;
                    }
                }
            }
            matte = std::min(1.f, std::max(0.f, matte));

            float out[4] = {0.f, 0.f, 0.f, matte};
            if (mode == eOutputMatteRGBA) {
                out[0] = out[1] = out[2] = matte;
            } else if (mode == eOutputPreview) {
                // Objects in the matte are blended half-way to white so the pick reads at a glance.
                out[0] = pr + (1.f - pr) * 0.5f * matte;
                out[1] = pg + (1.f - pg) * 0.5f * matte;
                out[2] = pb + (1.f - pb) * 0.5f * matte;
            } else if (srcRa && srcBounds.contains(x, y)) {
                const float* s = (const float*)srcRa->pixelAt(x, y);
                if (s) {
                    if (srcNComp >= 3) {
                        out[0] = s[0];
                        out[1] = s[1];
                        out[2] = s[2];
                    } else {
                        out[0] = out[1] = out[2] = s[0];
                    }
                    if (!haveSet) {
                        out[3] = srcNComp == 4 ? s[3] : (srcNComp == 1 ? s[0] : 1.f);
                    }
                }
            }
            if (outNComp == 1) {
                dst[0] = out[3];
            } else {
                for (int c = 0; c < outNComp && c < 4; ++c) {
                    dst[c] = out[c];
                }
            }
        }
    }
    return eStatusOK;
}

// ---- viewer picking

void
Cryptomatte::drawOverlay(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/)
{
    KnobBoolPtr pk = _imp->pick.lock();
    if (!pk || !pk->getValue() || !_imp->dragging) {
        return;
    }
    GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
    glColor4f(1.f, 0.8f, 0.1f, 0.9f);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    glVertex2d(_imp->dragFrom.x(), _imp->dragFrom.y());
    glVertex2d(_imp->dragTo.x(), _imp->dragFrom.y());
    glVertex2d(_imp->dragTo.x(), _imp->dragTo.y());
    glVertex2d(_imp->dragFrom.x(), _imp->dragTo.y());
    glEnd();
}

bool
Cryptomatte::onOverlayPenDown(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                              const QPointF& /*viewportPos*/, const QPointF& pos, double /*pressure*/,
                              double /*timestamp*/, PenType pen)
{
    KnobBoolPtr pk = _imp->pick.lock();
    if (!pk || !pk->getValue() || pen != ePenTypeLMB) {
        return false;
    }
    _imp->dragging = true;
    _imp->dragFrom = _imp->dragTo = pos;
    return true;
}

bool
Cryptomatte::onOverlayPenMotion(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                                const QPointF& /*viewportPos*/, const QPointF& pos, double /*pressure*/,
                                double /*timestamp*/)
{
    if (!_imp->dragging) {
        return false;
    }
    _imp->dragTo = pos;
    redrawOverlayInteract();
    return true;
}

bool
Cryptomatte::onOverlayPenUp(double time, const RenderScale& /*renderScale*/, ViewIdx view,
                            const QPointF& /*viewportPos*/, const QPointF& pos, double /*pressure*/,
                            double /*timestamp*/)
{
    if (!_imp->dragging) {
        return false;
    }
    _imp->dragging = false;
    _imp->dragTo = pos;
    pickAt(time, view, _imp->dragFrom, _imp->dragTo);
    redrawOverlayInteract();
    return true;
}

void
Cryptomatte::pickAt(double time, ViewIdx view, const QPointF& from, const QPointF& to)
{
    CryptoSet set;
    if (!_imp->currentSet(&set) || set.planes.empty()) {
        return;
    }
    refreshManifest();

    // A click within a couple of pixels is a point pick: the top-ranked object at that pixel. A
    // longer drag is a box: every object with coverage inside it.
    const bool box = std::fabs(to.x() - from.x()) > 2. || std::fabs(to.y() - from.y()) > 2.;
    const size_t nPlanes = box ? set.planes.size() : 1;

    std::set<std::uint32_t> found;
    for (size_t p = 0; p < nPlanes; ++p) {
        RectI roi;
        ImagePtr im = getImage(0, time, RenderScale(), view, NULL, &set.planes[p], false, true,
                               eStorageModeRAM, 0, &roi);
        if (!im || im->getComponents().getNumComponents() < 4) {
            continue;
        }
        const double par = im->getPixelAspectRatio() > 0 ? im->getPixelAspectRatio() : 1.;
        const RectI b = im->getBounds();
        int x1 = (int)std::floor(std::min(from.x(), to.x()) / par), x2 = (int)std::floor(std::max(from.x(), to.x()) / par);
        int y1 = (int)std::floor(std::min(from.y(), to.y())), y2 = (int)std::floor(std::max(from.y(), to.y()));
        x1 = std::max(x1, b.x1);
        y1 = std::max(y1, b.y1);
        x2 = std::min(x2, b.x2 - 1);
        y2 = std::min(y2, b.y2 - 1);
        Image::ReadAccess ra(im.get());
        for (int y = y1; y <= y2; ++y) {
            for (int x = x1; x <= x2; ++x) {
                const float* px = (const float*)ra.pixelAt(x, y);
                if (!px) {
                    continue;
                }
                if (!box) {
                    if (px[1] > 0.f) {
                        found.insert( CryptoHash::floatToBits(px[0]) );
                    }
                    continue;
                }
                for (int pair = 0; pair < 2; ++pair) {
                    if (px[pair * 2 + 1] > 0.f) {
                        found.insert( CryptoHash::floatToBits(px[pair * 2]) );
                    }
                }
            }
        }
    }
    if (found.empty()) {
        return;
    }

    KnobStringPtr lk = _imp->matteList.lock();
    if (!lk) {
        return;
    }
    std::vector<std::string> tokens = splitTokens( lk->getValue() );
    std::set<std::uint32_t> present;
    for (size_t i = 0; i < tokens.size(); ++i) {
        std::uint32_t b;
        if (tokenToBits(tokens[i], &b)) {
            present.insert(b);
        }
    }
    bool allPresent = true;
    for (std::set<std::uint32_t>::const_iterator it = found.begin(); it != found.end(); ++it) {
        if (!present.count(*it)) {
            allPresent = false;
            break;
        }
    }

    std::vector<std::string> result;
    if (allPresent) {
        // Remove every token that resolves to one of the picked IDs.
        for (size_t i = 0; i < tokens.size(); ++i) {
            std::uint32_t b;
            if (tokenToBits(tokens[i], &b) && found.count(b)) {
                continue;
            }
            result.push_back(tokens[i]);
        }
    } else {
        result = tokens;
        QMutexLocker l(&_imp->manifestMutex);
        for (std::set<std::uint32_t>::const_iterator it = found.begin(); it != found.end(); ++it) {
            if (present.count(*it)) {
                continue;
            }
            std::map<std::uint32_t, std::string>::const_iterator n = _imp->manifest.bitsToName.find(*it);
            result.push_back(n != _imp->manifest.bitsToName.end() ? n->second : "<" + CryptoHash::hashToHex(*it) + ">");
        }
    }
    std::string joined;
    for (size_t i = 0; i < result.size(); ++i) {
        if (i) {
            joined += ", ";
        }
        joined += result[i];
    }
    lk->setValue(joined);
}

NATRON_NAMESPACE_EXIT
