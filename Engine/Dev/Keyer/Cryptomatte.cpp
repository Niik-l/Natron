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

#include <QtCore/QMetaObject>
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
#ifdef NATRON_CYCLES
#include "../Cycles/CyclesRender.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

enum ManifestSourceEnum
{
    eManifestAuto = 0,   // the upstream file's EXR header (or the sidecar it names)
    eManifestSidecar,    // a manifest JSON file chosen by the user
    eManifestNone
};

enum MatteChannelEnum
{
    eMatteChannelAlpha = 0,
    eMatteChannelRed,
    eMatteChannelGreen,
    eMatteChannelBlue
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

// The nearest upstream node with a "filename" knob, through first inputs. Stops early at a
// Cycles render node (which carries its own manifests), returning it in *cyclesNode.
NodePtr
upstreamRead(EffectInstance* self, NodePtr* cyclesNode)
{
    NodePtr n = self->getNode() ? self->getNode()->getInput(0) : NodePtr();
    for (int hop = 0; n && hop < 32; ++hop) {
#ifdef NATRON_CYCLES
        if (dynamic_cast<CyclesRender*>(n->getEffectInstance().get())) {
            if (cyclesNode) {
                *cyclesNode = n;
            }
            return NodePtr();
        }
#endif
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

// Reads a whole file into `out`; false if it cannot be opened.
bool
slurp(const std::string& path, std::string* out)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
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
            if (slurp(dir + side, &m->json)) {
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
    KnobChoiceWPtr manifestSource;
    KnobFileWPtr manifestFile;
    KnobBoolWPtr previewMattes;
    KnobBoolWPtr pickerAdd;
    KnobBoolWPtr pickerRemove;
    KnobStringWPtr matteList;
    KnobButtonWPtr clear;
    KnobBoolWPtr matteOutput;
    KnobChoiceWPtr matteChannel;
    KnobBoolWPtr unpremultiply;
    KnobBoolWPtr removeChannels;
    KnobStringWPtr manifestInfo;
    KnobStringWPtr manifestNames;
    KnobButtonWPtr reload;

    mutable QMutex setsMutex;
    std::vector<CryptoSet> sets;

    mutable QMutex manifestMutex;
    Manifest manifest;
    std::string manifestKey;   // source mode + file + prefix the manifest was read for

    bool dragging;
    QPointF dragFrom, dragTo;

    CryptomattePrivate()
        : setsMutex()
        , sets()
        , manifestMutex()
        , manifest()
        , manifestKey()
        , dragging(false)
        , dragFrom()
        , dragTo()
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

    // 0 = off, 1 = add, 2 = remove.
    int pickerMode() const
    {
        KnobBoolPtr a = pickerAdd.lock(), r = pickerRemove.lock();
        if (a && a->getValue()) {
            return 1;
        }
        if (r && r->getValue()) {
            return 2;
        }
        return 0;
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
           "Read a multi-layer EXR and connect it: Layer Selection lists every Cryptomatte set found "
           "(object, material, asset). Type object names in the Matte List, separated by commas, or "
           "turn on Picker Add and click objects in the viewer (drag a box to add everything inside; "
           "Picker Remove takes them out again). Names come from the manifest in the EXR header; an "
           "object without a name can be given as its hex ID in angle brackets, <2ce615c5>.\n\n"
           "Preview Mattes shows every object in its own colour, the ones in the matte lifted towards "
           "white. Matte Output writes the matte into the chosen channel (alpha by default); "
           "Unpremultiply divides the RGB by the incoming alpha first so a later Premult by the matte "
           "gives clean edges; Remove Channels drops every other plane from the output.\n\n"
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

EffectInstance::PassThroughEnum
Cryptomatte::isPassThroughForNonRenderedPlanes() const
{
    KnobBoolPtr k = _imp->removeChannels.lock();
    return (k && k->getValue()) ? ePassThroughBlockNonRenderedPlanes : ePassThroughPassThroughNonRenderedPlanes;
}

void
Cryptomatte::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Cryptomatte"));
    auto separator = [this, &page]() {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    };

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Layer Selection"));
        k->setName("cryptoLayer");
        k->setHintToolTip(tr("The Cryptomatte set to decode. Each set is a run of planes named <prefix>00, "
                             "<prefix>01... on the Source (object, material and asset IDs are separate sets)."));
        page->addKnob(k);
        _imp->layer = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Manifest Source"));
        k->setName("manifestSource");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("auto", "Metadata Auto", "The manifest in the upstream file's EXR header (or the sidecar file it names)."));
        o.push_back(ChoiceOption("sidecar", "Sidecar File", "A manifest JSON file you choose."));
        o.push_back(ChoiceOption("none", "None", "No names: the Matte List takes names (hashed) and <hex> IDs only."));
        k->populateChoices(o);
        k->setDefaultValue(eManifestAuto);
        k->setAnimationEnabled(false);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("Where the object names come from."));
        page->addKnob(k);
        _imp->manifestSource = k;
    }
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr(""));
        k->setName("manifestFile");
        k->setHintToolTip(tr("The manifest JSON file, when Manifest Source is Sidecar File."));
        page->addKnob(k);
        _imp->manifestFile = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Manifest"));
        k->setName("manifestInfo");
        k->setAsLabel();
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        page->addKnob(k);
        _imp->manifestInfo = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reload"));
        k->setName("reloadManifest");
        k->setHintToolTip(tr("Re-read the object names."));
        page->addKnob(k);
        _imp->reload = k;
    }
    {
        KnobGroupPtr grp = AppManager::createKnob<KnobGroup>(this, tr("Manifest Names"));
        grp->setName("manifestNamesGroup");
        grp->setDefaultValue(false);
        grp->setAsTab();
        page->addKnob(grp);
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr(""));
        k->setName("manifestNames");
        k->setAsMultiLine();
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("Every object the manifest lists for this layer, one per line, for copying into "
                             "the Matte List. Read-only: edits here do nothing."));
        grp->addKnob(k);
        _imp->manifestNames = k;
    }
    separator();
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Preview Mattes"));
        k->setName("previewMattes");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Show every object in its own colour in RGB, the objects in the matte lifted towards "
                             "white (the keyable-surface look). Off: RGB comes from the Source."));
        page->addKnob(k);
        _imp->previewMattes = k;
    }
    separator();
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Picker Add"));
        k->setName("pickerAdd");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("While on, a left click in the viewer adds the object under the cursor to the Matte "
                             "List; a left drag adds every object inside the box."));
        page->addKnob(k);
        _imp->pickerAdd = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Picker Remove"));
        k->setName("pickerRemove");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("While on, a left click in the viewer removes the object under the cursor from the "
                             "Matte List; a left drag removes every object inside the box."));
        page->addKnob(k);
        _imp->pickerRemove = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Matte List"));
        k->setName("matteList");
        k->setAsMultiLine();
        k->setHintToolTip(tr("The objects in the matte: names separated by commas or new lines, as the "
                             "renderer wrote them (see Manifest Names). An object can also be given as its ID "
                             "in angle brackets, <2ce615c5>. The pickers add and remove entries here."));
        page->addKnob(k);
        _imp->matteList = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Clear"));
        k->setName("clear");
        k->setHintToolTip(tr("Empty the Matte List."));
        page->addKnob(k);
        _imp->clear = k;
    }
    separator();
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Matte Output"));
        k->setName("matteOutput");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("Write the matte into the channel on the right. Off: the Source's channels are kept "
                             "(with the preview colours in RGB if Preview Mattes is on)."));
        page->addKnob(k);
        _imp->matteOutput = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr(""));
        k->setName("matteChannel");
        std::vector<ChoiceOption> o;
        o.push_back(ChoiceOption("alpha", "rgba.alpha", ""));
        o.push_back(ChoiceOption("red", "rgba.red", ""));
        o.push_back(ChoiceOption("green", "rgba.green", ""));
        o.push_back(ChoiceOption("blue", "rgba.blue", ""));
        k->populateChoices(o);
        k->setDefaultValue(eMatteChannelAlpha);
        k->setAnimationEnabled(false);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("The channel the matte goes into."));
        page->addKnob(k);
        _imp->matteChannel = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Unpremultiply"));
        k->setName("unpremultiply");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Divide the Source RGB by the Source alpha before the matte replaces it, so that "
                             "premultiplying by the matte afterwards gives clean edges. Not applied to the "
                             "preview colours."));
        page->addKnob(k);
        _imp->unpremultiply = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Remove Channels"));
        k->setName("removeChannels");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Output only the Color plane: the Cryptomatte layers and every other plane of the "
                             "Source are dropped."));
        page->addKnob(k);
        _imp->removeChannels = k;
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
    KnobChoicePtr src = _imp->manifestSource.lock();
    KnobFilePtr f = _imp->manifestFile.lock();
    if (src && f) {
        f->setSecret(src->getValue() != eManifestSidecar);
    }
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
    KnobChoicePtr srcKnob = _imp->manifestSource.lock();
    const int source = srcKnob ? srcKnob->getValue() : (int)eManifestAuto;
    double time = 0;
    if (getApp() && getApp()->getTimeLine()) {
        time = getApp()->getTimeLine()->currentFrame();
    }
    std::string file;
    NodePtr cyclesNode;
    std::string cyclesJson;
    if (source == eManifestAuto && haveSet) {
        NodePtr rd = upstreamRead(this, &cyclesNode);
        if (rd) {
            file = readFileAt(this, rd, (int)std::floor(time + 0.5));
        } else if (cyclesNode) {
#ifdef NATRON_CYCLES
            // The render node's manifests change with every render: key on their content size.
            CyclesRender* cr = dynamic_cast<CyclesRender*>(cyclesNode->getEffectInstance().get());
            cyclesJson = cr ? cr->getCryptomatteManifest(set.prefix) : std::string();
#endif
            file = "cycles:" + cyclesNode->getScriptName_mt_safe() + ":" + std::to_string(cyclesJson.size());
        }
    } else if (source == eManifestSidecar) {
        KnobFilePtr f = _imp->manifestFile.lock();
        if (f) {
            file = f->getFileName((int)std::floor(time + 0.5), ViewIdx(0));
            if (!file.empty() && getApp() && getApp()->getProject()) {
                getApp()->getProject()->canonicalizePath(file);
            }
        }
    }
    std::ostringstream keyStream;
    keyStream << source << "|" << file << "|" << set.prefix;
    const std::string key = keyStream.str();
    {
        QMutexLocker l(&_imp->manifestMutex);
        if (key == _imp->manifestKey) {
            return;
        }
        _imp->manifestKey = key;
    }

    Manifest m;
    if (source == eManifestNone) {
        m.source = "Manifest Source is None";
    } else if (source == eManifestSidecar) {
        if (file.empty()) {
            m.source = "no sidecar file chosen";
        } else if (slurp(file, &m.json)) {
            m.source = "sidecar file";
            parseManifest(m.json, &m);
        } else {
            m.source = "sidecar file not found";
        }
    } else if (!haveSet) {
        m.source = "no Cryptomatte layers on the Source";
    } else if (cyclesNode) {
        m.json = cyclesJson;
        m.source = m.json.empty() ? "Cycles render has no manifest for this layer yet (render first)" : "Cycles render";
        parseManifest(m.json, &m);
    } else if (file.empty()) {
        m.source = "no Read upstream: type names, or pick in the viewer (IDs are written as <hex>)";
    } else {
        manifestFromFile(file, set.prefix, &m);
    }

    std::ostringstream info, names;
    if (m.nameToHash.empty()) {
        info << "no names (" << m.source << ")";
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
    if (k == _imp->layer.lock().get() || k == _imp->manifestFile.lock().get()) {
        refreshManifest();
        return true;
    }
    if (k == _imp->manifestSource.lock().get()) {
        KnobFilePtr f = _imp->manifestFile.lock();
        if (f) {
            f->setSecret(_imp->manifestSource.lock()->getValue() != eManifestSidecar);
        }
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
    if (k == _imp->clear.lock().get()) {
        KnobStringPtr lk = _imp->matteList.lock();
        if (lk) {
            lk->setValue(std::string());
        }
        return true;
    }
    // The two pickers are modes: turning one on turns the other off.
    if (k == _imp->pickerAdd.lock().get() && _imp->pickerAdd.lock()->getValue()) {
        KnobBoolPtr other = _imp->pickerRemove.lock();
        if (other && other->getValue()) {
            other->setValue(false);
        }
        return true;
    }
    if (k == _imp->pickerRemove.lock().get() && _imp->pickerRemove.lock()->getValue()) {
        KnobBoolPtr other = _imp->pickerAdd.lock();
        if (other && other->getValue()) {
            other->setValue(false);
        }
        return true;
    }
    if (k == _imp->pickerAdd.lock().get() || k == _imp->pickerRemove.lock().get() ||
        k == _imp->manifestNames.lock().get()) {
        return true;   // nothing to render
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
        if (it->second) {
            outImg = it->second;
            break;
        }
    }
    if (!outImg) {
        return eStatusFailed;
    }
    const RectI outBounds = outImg->getBounds();
    const int outNComp = outImg->getComponents().getNumComponents();

    const bool preview = _imp->previewMattes.lock()->getValue();
    const bool matteOn = _imp->matteOutput.lock()->getValue();
    const bool unpremult = _imp->unpremultiply.lock()->getValue();
    int matteIdx = 3;
    switch (_imp->matteChannel.lock()->getValue()) {
    case eMatteChannelRed: matteIdx = 0; break;
    case eMatteChannelGreen: matteIdx = 1; break;
    case eMatteChannelBlue: matteIdx = 2; break;
    default: matteIdx = 3; break;
    }

    CryptoSet set;
    const bool haveSet = _imp->currentSet(&set);
    if (!haveSet) {
        setPersistentMessage(eMessageTypeWarning, "No Cryptomatte layers on the Source: it needs planes named "
                                                  "<prefix>00, <prefix>01... (CryptoObject00, crypto_material00 ...).");
    } else {
        clearPersistentMessage(false);
        // An upstream Cycles render only has manifests once it has rendered, which is now: ask
        // the main thread to look again (cheap when nothing changed).
        bool noNames;
        {
            QMutexLocker l(&_imp->manifestMutex);
            noNames = _imp->manifest.nameToHash.empty();
        }
        if (noNames) {
            QMetaObject::invokeMethod(this, [this]() { refreshManifest(); }, Qt::QueuedConnection);
        }
    }

    // The Source colour and the ID planes.
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, NULL);
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
            // The Source pixel (RGB broadcast from a single channel, alpha 1 when it has none).
            float out[4] = {0.f, 0.f, 0.f, 1.f};
            if (srcRa && srcBounds.contains(x, y)) {
                const float* s = (const float*)srcRa->pixelAt(x, y);
                if (s) {
                    if (srcNComp >= 3) {
                        out[0] = s[0];
                        out[1] = s[1];
                        out[2] = s[2];
                        out[3] = srcNComp == 4 ? s[3] : 1.f;
                    } else {
                        out[0] = out[1] = out[2] = out[3] = s[0];
                    }
                }
            }
            if (!haveSet) {
                for (int c = 0; c < outNComp && c < 4; ++c) {
                    dst[c] = outNComp == 1 ? out[3] : out[c];
                }
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
                    if (preview) {
                        float r, g, b;
                        CryptoHash::idToPreviewColor(bits, &r, &g, &b);
                        pr += cov * r;
                        pg += cov * g;
                        pb += cov * b;
                    }
                }
            }
            matte = std::min(1.f, std::max(0.f, matte));

            if (preview) {
                // Objects in the matte are blended half-way to white so the pick reads at a glance.
                out[0] = pr + (1.f - pr) * 0.5f * matte;
                out[1] = pg + (1.f - pg) * 0.5f * matte;
                out[2] = pb + (1.f - pb) * 0.5f * matte;
            } else if (unpremult && out[3] > 0.f && out[3] < 1.f) {
                out[0] /= out[3];
                out[1] /= out[3];
                out[2] /= out[3];
            }
            if (matteOn) {
                out[matteIdx] = matte;
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
    const int mode = _imp->pickerMode();
    if (!mode || !_imp->dragging) {
        return;
    }
    GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
    if (mode == 1) {
        glColor4f(0.3f, 1.f, 0.3f, 0.9f);
    } else {
        glColor4f(1.f, 0.35f, 0.3f, 0.9f);
    }
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
    if (!_imp->pickerMode() || pen != ePenTypeLMB) {
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
    const int mode = _imp->pickerMode();
    CryptoSet set;
    if (!mode || !_imp->currentSet(&set) || set.planes.empty()) {
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
    const std::vector<std::string> tokens = splitTokens( lk->getValue() );
    std::vector<std::string> result;
    if (mode == 2) {
        // Remove every token that resolves to one of the picked IDs.
        for (size_t i = 0; i < tokens.size(); ++i) {
            std::uint32_t b;
            if (tokenToBits(tokens[i], &b) && found.count(b)) {
                continue;
            }
            result.push_back(tokens[i]);
        }
    } else {
        std::set<std::uint32_t> present;
        for (size_t i = 0; i < tokens.size(); ++i) {
            std::uint32_t b;
            if (tokenToBits(tokens[i], &b)) {
                present.insert(b);
            }
        }
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
