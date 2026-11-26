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

#include "MaskToGeo.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <QCoreApplication>
#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Global/Enums.h"

#include "Dev/Mask/VectorDocument.h"
#include "Dev/Scene3D/MaskMeshBuilder.h"
#include "Dev/Scene3D/MeshData.h"
#include "Dev/Scene3D/VectorShapeProvider.h"

NATRON_NAMESPACE_ENTER

struct MaskToGeoPrivate
{
    // Controls
    KnobDoubleWPtr worldScale;
    KnobDoubleWPtr extrudeDepth;
    KnobStringWPtr info;

    // True while programmatically setting knobs (info line): knobChanged must
    // ignore those, or the info setValue re-enters knobChanged forever
    // (stack overflow, seen while editing roto curves).
    bool syncing = false;

    // Transform page (read by name by every consumer)
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobDoubleWPtr uniformScale;

    // Material (same knobs as GeoBuilder)
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    KnobFileWPtr textureFile;
    KnobChoiceWPtr diffuseColorspace;
};

namespace {

static KnobDoublePtr
makeDouble(MaskToGeo* self, KnobPagePtr page, const QString& label, const char* name,
           double def, double dispMin, double dispMax, double hardMin = -1e30, double step = -1.0)
{
    KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, label);
    k->setName(name);
    k->setDefaultValue(def);
    k->setAnimationEnabled(true);
    if (hardMin > -1e29) k->setMinimum(hardMin);
    k->setDisplayMinimum(dispMin);
    k->setDisplayMaximum(dispMax);
    if (step > 0.0) k->setIncrement(step);
    // Geometry-group knobs pass a null page (they are added to the group by
    // the caller, not to a page): adding to a null KnobPagePtr crashed the
    // node on creation.
    if (page) {
        page->addKnob(k);
    }
    return k;
}

// ---------------------------------------------------------------------------
// Bezier flattening: the roto-context side of the shared MaskMeshBuilder
// (the tessellation/extrusion itself lives in MaskMeshBuilder.cpp).
// ---------------------------------------------------------------------------

/**
 * @brief Flatten one curve at time into a closed mask-space contour (50 points
 * per segment, the Roto house value; open curves are closed implicitly - the
 * Roto fill closes them too) and drop sub-pixel duplicates + the closure
 * point. Returns false for degenerate results.
 */
static bool
flattenCurveContour(const BezierPtr& curve, double time,
                    std::vector<std::pair<double, double> >* contour)
{
    std::list<ParametricPoint> polygon;
    RectD bbox;
    curve->evaluateAtTime_DeCasteljau(false, time, 0,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                      50,
#else
                                      1.0,
#endif
                                      &polygon, &bbox);
    if (polygon.size() < 3) {
        return false;
    }
    contour->reserve(polygon.size());
    for (std::list<ParametricPoint>::const_iterator p = polygon.begin();
         p != polygon.end(); ++p) {
        contour->push_back(std::make_pair(p->x, p->y));
    }
    if (contour->front() != contour->back()) {
        contour->push_back(contour->front());
    }
    return MaskMesh::cleanContour(contour);
}

} // anonymous namespace

MaskToGeo::MaskToGeo(NodePtr node)
    : EffectInstance(node)
    , _imp(new MaskToGeoPrivate())
{
}

MaskToGeo::~MaskToGeo()
{
}

std::string
MaskToGeo::getPluginDescription() const
{
    return tr(
        "Turns the vector shapes of a Roto / RotoPaint / ReadMask node into 3D "
        "geometry: every active shape at the requested time is flattened and "
        "tessellated into a flat mesh in the XY plane, centered on the project "
        "format, scaled so the format height equals World Scale (1.0 world unit "
        "by default). Extrude Depth > 0 adds side walls and an offset cap.\n\n"
        "The mesh follows the mask shapes at render time, so animated masks "
        "(ReadMask file sequences, baked keyframes, hand-animated Roto) become "
        "animated geo. Connect to Scene3D / ScanlineRender / CyclesRender input "
        "1 like any geometry node."
    ).toStdString();
}

std::string
MaskToGeo::getInputLabel(int inputNb) const
{
    return inputNb == 0 ? "Mask" : std::string();
}

bool
MaskToGeo::isInputOptional(int inputNb) const
{
    // Without a mask source there is simply no geometry.
    Q_UNUSED(inputNb);
    return true;
}

bool
MaskToGeo::carriesOverlayPath(int inputNb) const
{
    // The mask source is pure data: its 2D roto overlay must not be drawn over
    // the image produced by the 3D comp this node feeds. The mask node still
    // shows its overlay when viewed directly.
    Q_UNUSED(inputNb);
    return false;
}

void
MaskToGeo::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
MaskToGeo::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
MaskToGeo::initializeKnobs()
{
    // ---------------- Controls ----------------
    // World Scale / Extrude Depth live in a Geometry group like TraceMask's:
    // the group header is the panel's first row, keeping the sliders clear of
    // the page tab bar (a wrapped tab row used to clip the first slider).
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));
    {
        KnobGroupPtr geoGroup = AppManager::createKnob<KnobGroup>(this, tr("Geometry"));
        geoGroup->setName("geometryGroup");
        geoGroup->setDefaultValue(true);
        page->addKnob(geoGroup);
        KnobDoublePtr k = makeDouble(this, KnobPagePtr(), tr("World Scale"), "worldScale", 1.0, 0.01, 10.0, 0.0, 0.01);
        k->setHintToolTip(tr("World size of the project format height. The mask pixel "
                             "coordinates are normalized so the format height equals this "
                             "value (1.0 = Nuke-style normalized space), then centered on "
                             "the format rect."));
        geoGroup->addKnob(k);
        _imp->worldScale = k;
        k = makeDouble(this, KnobPagePtr(), tr("Extrude Depth"), "extrudeDepth", 0.0, 0.0, 5.0, 0.0, 0.01);
        k->setHintToolTip(tr("0 (default): a pure flat shape mesh in the XY plane at z=0. "
                             "Greater than 0: side walls between the contour at z=0 and an "
                             "offset cap at z=depth, plus both caps."));
        geoGroup->addKnob(k);
        _imp->extrudeDepth = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info"); k->setAsLabel(); k->setIsPersistent(false); k->setEvaluateOnChange(false);
        page->addKnob(k); _imp->info = k;
    }

    // ---------------- Transform page (whole model) ----------------
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));
    _imp->translateX = makeDouble(this, xformPage, tr("Translate X"), "translateX", 0.0, -100.0, 100.0);
    _imp->translateY = makeDouble(this, xformPage, tr("Translate Y"), "translateY", 0.0, -100.0, 100.0);
    _imp->translateZ = makeDouble(this, xformPage, tr("Translate Z"), "translateZ", 0.0, -100.0, 100.0);
    _imp->rotateX = makeDouble(this, xformPage, tr("Rotate X"), "rotateX", 0.0, -180.0, 180.0);
    _imp->rotateY = makeDouble(this, xformPage, tr("Rotate Y"), "rotateY", 0.0, -180.0, 180.0);
    _imp->rotateZ = makeDouble(this, xformPage, tr("Rotate Z"), "rotateZ", 0.0, -180.0, 180.0);
    _imp->scaleX = makeDouble(this, xformPage, tr("Scale X"), "scaleX", 1.0, 0.1, 10.0, 0.0, 0.01);
    _imp->scaleY = makeDouble(this, xformPage, tr("Scale Y"), "scaleY", 1.0, 0.1, 10.0, 0.0, 0.01);
    _imp->scaleZ = makeDouble(this, xformPage, tr("Scale Z"), "scaleZ", 1.0, 0.1, 10.0, 0.0, 0.01);
    _imp->uniformScale = makeDouble(this, xformPage, tr("Uniform Scale"), "uniformScale", 1.0, 0.001, 10.0, 0.0, 0.0001);
    _imp->uniformScale.lock()->setHintToolTip(tr("Multiplies Scale X/Y/Z, so the whole model resizes from one knob."));

    // ---------------- Material page (same as GeoBuilder / ReadGeo) ----------------
    KnobPagePtr matPage = AppManager::createKnob<KnobPage>(this, tr("Material"));
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Base Color"), 3);
        k->setName("baseColor");
        k->setDefaultValue(0.8, 0); k->setDefaultValue(0.8, 1); k->setDefaultValue(0.8, 2);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->baseColor = k;
    }
    _imp->roughness = makeDouble(this, matPage, tr("Roughness"), "roughness", 0.5, 0.0, 1.0, 0.0);
    _imp->metallic = makeDouble(this, matPage, tr("Metallic"), "metallic", 0.0, 0.0, 1.0, 0.0);
    _imp->specular = makeDouble(this, matPage, tr("Specular"), "specular", 0.5, 0.0, 1.0, 0.0);
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Emission Color"), 3);
        k->setName("emissionColor");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(1.0, 2);
        matPage->addKnob(k); _imp->emissionColor = k;
    }
    _imp->emissionStrength = makeDouble(this, matPage, tr("Emission Strength"), "emissionStrength", 0.0, 0.0, 10.0, 0.0);
    _imp->transmission = makeDouble(this, matPage, tr("Transmission"), "transmission", 0.0, 0.0, 1.0, 0.0);
    _imp->ior = makeDouble(this, matPage, tr("IOR"), "ior", 1.45, 1.0, 2.5, 1.0);

    KnobPagePtr texPage = AppManager::createKnob<KnobPage>(this, tr("Texture Maps"));
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Diffuse Map"));
        k->setName("textureFile");
        k->setHintToolTip(tr("Base color / albedo texture. Supports .exr, .hdr, .png, .jpg. "
                             "The mesh UVs are the mask pixel coordinates normalized to the "
                             "format (0..1), so a plate projected onto the mask maps 1:1."));
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
        texPage->addKnob(k); _imp->diffuseColorspace = k;
    }
}

// ---------------------------------------------------------------------------
// Mesh build
// ---------------------------------------------------------------------------

unsigned long long
MaskToGeo::currentInputsHash(double time) const
{
    // Input hash + input Roto age (curve edits bump the age without changing
    // hashes) + own hash (knob values incl. animation at time).
    auto mix = [](unsigned long long seed, unsigned long long val) -> unsigned long long {
        return seed ^ (val * 0x9e3779b97f4a7c15ULL + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    unsigned long long h = 0;
    NodePtr input = getNode()->getInput(0);
    h = mix(h, (input && input->getEffectInstance()) ? (unsigned long long)input->getEffectInstance()->getHash() : 0ULL);
    h = mix(h, (input && input->getRotoContext()) ? (unsigned long long)input->getRotoContext()->getAge() : 0ULL);
    h = mix(h, (unsigned long long)getNode()->getEffectInstance()->getHash() ^ (unsigned long long)(time * 1000.0));
    return h;
}

MeshDataPtr
MaskToGeo::buildMeshSnapshot(double time) const
{
    NodePtr input = getNode()->getInput(0);

    // Collect closed contours in canonical (mask/format) space, then hand
    // them to the shared MaskMeshBuilder (union fill / extrusion).
    std::vector<MaskMesh::Contour> contours;

    // Prefer a VectorShapeProvider (TraceMask & friends): its shapes arrive
    // already in canonical space, with per-shape hole operators. The roto
    // context is the legacy path.
    VectorShapeProvider* provider = (input && input->getEffectInstance())
        ? dynamic_cast<VectorShapeProvider*>(input->getEffectInstance().get())
        : (VectorShapeProvider*)0;
    if (provider) {
        const VectorMask::VectorDocument doc = provider->getShapeDocument(time);
        contours.reserve(doc.shapes.size());
        for (std::size_t i = 0; i < doc.shapes.size(); ++i) {
            std::vector<std::pair<double, double> > poly;
            if (VectorMask::flattenShape(doc.shapes[i], &poly)) {
                MaskMesh::Contour c;
                c.points.swap(poly);
                c.op = doc.shapes[i].compositingOperator;
                contours.push_back(c);
            }
        }
    } else {
        RotoContextPtr ctx = input ? input->getRotoContext() : RotoContextPtr();
        if (ctx) {
            // Collect every active Bezier in the tree, recursing into layers
            // (ReadMask groups imported shapes under a file layer + per-group
            // sub-layers). A layer can be reachable twice (base layer also
            // lists the file layer's items), so guard both layers and curves
            // by identity: emitting the same curve twice stacks two
            // coincident shells in the mesh.
            std::vector<BezierPtr> curves;
            std::set<const RotoLayer*> visitedLayers;
            std::set<const Bezier*> visitedCurves;
            std::function<void(const RotoLayerPtr&)> walkLayer = [&](const RotoLayerPtr& layer) {
                if (!layer || !visitedLayers.insert(layer.get()).second) {
                    return;
                }
                const std::list<RotoItemPtr> items = layer->getItems_mt_safe();
                for (std::list<RotoItemPtr>::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
                    if (RotoLayerPtr child = std::dynamic_pointer_cast<RotoLayer>(*it2)) {
                        walkLayer(child);
                        continue;
                    }
                    const BezierPtr curve = std::dynamic_pointer_cast<Bezier>(*it2);
                    if (!curve || !curve->isActivated(time)) {
                        continue;
                    }
                    if (!visitedCurves.insert(curve.get()).second) {
                        continue;
                    }
                    curves.push_back(curve);
                }
            };

            const std::list<RotoLayerPtr> layers = ctx->getLayers_mt_safe();
            for (std::list<RotoLayerPtr>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
                walkLayer(*it);
            }

            contours.reserve(curves.size());
            for (std::size_t i = 0; i < curves.size(); ++i) {
                std::vector<std::pair<double, double> > poly;
                if (flattenCurveContour(curves[i], time, &poly)) {
                    MaskMesh::Contour c;
                    c.points.swap(poly);
                    c.op = curves[i]->getCompositingOperator();
                    contours.push_back(c);
                }
            }
        }
    }

    // Scale: world units per mask pixel, format height -> World Scale.
    Format fmt;
    getApp()->getProject()->getProjectDefaultFormat(&fmt);
    const double formatW = fmt.x2 - fmt.x1;
    const double formatH = fmt.y2 - fmt.y1;
    const KnobDoublePtr worldScaleKnob = _imp->worldScale.lock();
    const double worldScale = worldScaleKnob ? worldScaleKnob->getValueAtTime(time) : 1.0;
    const double s = (formatH > 0.0) ? worldScale / formatH : 1.0;
    const double cx = fmt.x1 + formatW / 2.0;
    const double cy = fmt.y1 + formatH / 2.0;
    const KnobDoublePtr depthKnob = _imp->extrudeDepth.lock();
    const double depth = depthKnob ? std::max(0.0, depthKnob->getValueAtTime(time)) : 0.0;

    return MaskMesh::buildMesh(contours, s, cx, cy, formatW, formatH, depth, 0.0);
}

MeshDataPtr
MaskToGeo::getMeshData(double time) const
{
    // Animated masks: the mesh must follow the shapes per frame. The snapshot
    // is keyed by (inputs hash, time); hashes cover knob edits and curve edits
    // (the Roto context age), time covers frame-varying curve animation.
    const unsigned long long h = currentInputsHash(time);
    std::lock_guard<std::mutex> lk(_meshMutex);
    if (!_mesh || h != _meshInputsHash || time != _meshInputsTime) {
        _mesh = buildMeshSnapshot(time);
        _meshInputsHash = h;
        _meshInputsTime = time;
    }
    return _mesh;
}

bool
MaskToGeo::knobChanged(KnobI* /*k*/, ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    // Main-thread knob edit: refresh the published snapshot + the info line
    // (GeoBuilder's rebuildMesh pattern). Returns false: no knob consumed.
    if (_imp->syncing) {
        return false;
    }
    const double time = getApp() ? getApp()->getTimeLine()->currentFrame() : 0.0;
    const unsigned long long h = currentInputsHash(time);
    MeshDataPtr built = buildMeshSnapshot(time);
    {
        std::lock_guard<std::mutex> lk(_meshMutex);
        _mesh = built;
        _meshInputsHash = h;
        _meshInputsTime = time;
    }
    if (KnobStringPtr info = _imp->info.lock()) {
        std::ostringstream ss;
        ss << built->numVertices << " vertices, " << built->numFaces << " faces";
        _imp->syncing = true;
        info->setValue(ss.str());
        _imp->syncing = false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// RoD / render (this node produces no image)
// ---------------------------------------------------------------------------

StatusEnum
MaskToGeo::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                 ViewIdx /*view*/, RectD* rod)
{
    // Token RoD like the other 3D sources (GeoBuilder without a plate).
    rod->x1 = 0; rod->y1 = 0; rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
MaskToGeo::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

// ---------------------------------------------------------------------------
// MaterialProvider
// ---------------------------------------------------------------------------

void
MaskToGeo::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}
double MaskToGeo::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }
double MaskToGeo::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }
double MaskToGeo::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5; }
void
MaskToGeo::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}
double MaskToGeo::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }
double MaskToGeo::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }
std::string MaskToGeo::getMaterialTextureFile() const
{ KnobFilePtr k = _imp->textureFile.lock(); return k ? k->getValue() : std::string(); }
std::string
MaskToGeo::getMaterialDiffuseColorspace() const
{
    KnobChoicePtr k = _imp->diffuseColorspace.lock();
    int idx = k ? k->getValue() : 0;
    const char* names[] = {"sRGB", "Linear", "ACEScg", "Raw"};
    return (idx >= 0 && idx < 4) ? names[idx] : "sRGB";
}
bool MaskToGeo::hasMaterialInput() const
{
    return false; // the single input is the mask source, not a material
}
MaterialProvider* MaskToGeo::getConnectedMaterial() const
{
    return nullptr;
}

NATRON_NAMESPACE_EXIT

#include "moc_MaskToGeo.cpp"
