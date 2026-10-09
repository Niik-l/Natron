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

#include "Zap3D.h"
#include "Light3D.h"
#include "../DotUtils.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../Project.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

// ---------------------------------------------------------------------------
// small math (self-contained so the bolt code matches the 2D Zap exactly)
// ---------------------------------------------------------------------------

namespace {

struct V3
{
    double x, y, z;
};
inline V3 v3(double x, double y, double z) { V3 r = {x, y, z}; return r; }
inline V3 operator+(const V3& a, const V3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(const V3& a, const V3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(const V3& a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(const V3& a) { const double l = length(a); return l > 1e-12 ? a * (1. / l) : v3(0, 0, 1); }
inline V3 cross(const V3& a, const V3& b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

// a stable perpendicular basis (e1, e2) for direction d, right-handed with d
inline void basis(const V3& d, V3& e1, V3& e2)
{
    const V3 helper = std::fabs(d.y) < 0.9 ? v3(0, 1, 0) : v3(1, 0, 0);
    e1 = normalize(cross(helper, d));
    e2 = cross(d, e1);
}

// Rodrigues rotation of v about unit axis k by angle a
inline V3 rotate(const V3& v, const V3& k, double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1. - c));
}

struct Rng
{
    unsigned int s;
    explicit Rng(unsigned int seed) : s(seed * 2654435761u + 0x9E3779B9u) { next(); next(); }
    unsigned int next() { s = s * 1664525u + 1013904223u; return s; }
    double frand() { return (next() >> 8) * (1. / 16777216.); }
    double srand() { return frand() * 2. - 1.; }
};

inline double hashU(unsigned int x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return (x & 0xffffffU) * (1. / 16777216.);
}

// smooth 1-D value noise in [-1,1]
inline double noise1(double t, unsigned int stream)
{
    const double f = std::floor(t);
    double u = t - f;
    u = u * u * (3. - 2. * u);
    const unsigned int i = (unsigned int)(long long)f;
    const double a = hashU(i * 0x9E3779B1u + stream * 0x85EBCA77u);
    const double b = hashU((i + 1) * 0x9E3779B1u + stream * 0x85EBCA77u);
    return (a + (b - a) * u) * 2. - 1.;
}

struct Poly
{
    std::vector<V3> pts;
    std::vector<double> u;    // fraction along the MAIN bolt at each point (Progress)
    double wscale;            // radius scale (branches get thinner)
};

struct BoltParams
{
    double wrinkle;
    int detail;
    double branchiness, branchAngle, branchLength, branchScale;
    double wiggleT;
};

// recursive midpoint displacement in 3D with branches; same structure as the 2D Zap
void genBolt(V3 a, V3 b, const BoltParams& bp, Rng& rng, int depthLeft, double wscale,
             double u0, double u1, std::vector<Poly>& out)
{
    const double len0 = length(b - a);
    if (len0 < 1e-6) {
        return;
    }
    // levels from the bolt length relative to its own size: ~6 kinks per unit of
    // length is the 2D node's "6 px" rule scaled to world space
    int levels = (int)std::ceil(std::log(std::max(2., len0 / (len0 * 0.03))) / std::log(2.)) + bp.detail;
    levels = std::max(2, std::min(10, levels));

    std::vector<V3> pts;
    pts.push_back(a);
    pts.push_back(b);
    for (int lv = 0; lv < levels; ++lv) {
        std::vector<V3> np;
        np.reserve(pts.size() * 2);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const V3 p = pts[i], q = pts[i + 1];
            np.push_back(p);
            const V3 d = q - p;
            const double sl = length(d);
            if (sl > 1e-9) {
                V3 e1, e2;
                basis(d * (1. / sl), e1, e2);
                const double az = rng.frand() * 2. * M_PI;
                const V3 side = e1 * std::cos(az) + e2 * std::sin(az);
                const unsigned int stream = rng.next();
                const double amp = bp.wrinkle * sl * 0.35;
                const double disp = noise1(bp.wiggleT + rng.frand() * 100., stream) * amp;
                np.push_back((p + q) * 0.5 + side * disp);
            } else {
                np.push_back((p + q) * 0.5);
            }
        }
        np.push_back(pts.back());
        pts.swap(np);

        if (depthLeft > 0 && bp.branchiness > 0. && lv == std::min(2, levels - 1)) {
            for (size_t i = 1; i + 1 < pts.size(); ++i) {
                if (rng.frand() < bp.branchiness * 0.6) {
                    const V3 p = pts[i];
                    const V3 dir = normalize(pts[i + 1] - pts[i - 1]);
                    V3 e1, e2;
                    basis(dir, e1, e2);
                    const double az = rng.frand() * 2. * M_PI;
                    const V3 axis = e1 * std::cos(az) + e2 * std::sin(az);   // random perpendicular
                    const double ang = (0.3 + 0.7 * rng.frand()) * bp.branchAngle * M_PI / 180.;
                    const V3 bd = rotate(dir, axis, ang);
                    const double frac = (double)i / (double)(pts.size() - 1);
                    const double remain = len0 * (1. - frac);
                    const double bl = bp.branchLength * remain * (0.3 + 0.7 * rng.frand());
                    if (bl < len0 * 0.01) {
                        continue;
                    }
                    const double ub = u0 + (u1 - u0) * frac;
                    genBolt(p, p + bd * bl, bp, rng, depthLeft - 1, wscale * bp.branchScale,
                            ub, ub + (u1 - u0) * 0.5 * bp.branchLength, out);
                }
            }
        }
    }
    Poly poly;
    poly.pts = pts;
    poly.wscale = wscale;
    const size_t n = pts.size() - 1;
    poly.u.resize(pts.size());
    for (size_t i = 0; i <= n; ++i) {
        poly.u[i] = u0 + (u1 - u0) * (double)i / (double)n;
    }
    out.push_back(poly);
}

// cut a polyline at Progress: keep points with u <= progress, plus the exact crossing
void trimPoly(Poly& p, double progress)
{
    if (progress >= 1.) {
        return;
    }
    size_t keep = 0;
    while (keep < p.pts.size() && p.u[keep] <= progress) {
        ++keep;
    }
    if (keep == 0) {
        p.pts.clear();
        p.u.clear();
        return;
    }
    if (keep < p.pts.size()) {
        const double u0 = p.u[keep - 1], u1 = p.u[keep];
        const double t = u1 > u0 ? (progress - u0) / (u1 - u0) : 0.;
        const V3 cut = p.pts[keep - 1] + (p.pts[keep] - p.pts[keep - 1]) * t;
        p.pts.resize(keep);
        p.u.resize(keep);
        p.pts.push_back(cut);
        p.u.push_back(progress);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------

struct Zap3D::Snapshot
{
    MeshDataPtr mesh;
    std::vector<Zap3D::LightSample> samples;   // all segment midpoints, weights sum to 1
};

struct Zap3DPrivate
{
    // Zap page
    KnobDoubleWPtr start, end;
    KnobIntWPtr bolts;
    KnobDoubleWPtr varyEnd, wrinkle;
    KnobIntWPtr detail;
    KnobDoubleWPtr branchiness, branchAngle, branchLength;
    KnobIntWPtr branchDepth;
    KnobDoubleWPtr progress, wiggle;
    KnobIntWPtr restrike;
    KnobDoubleWPtr flicker, boltVar, tipFade;
    KnobIntWPtr seed;
    // Shape page
    KnobDoubleWPtr radius, branchScale;
    KnobIntWPtr sides;
    // Transform page
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobDoubleWPtr uniformScale;
    // Material page
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    // Lights page (FastVolumeRender)
    KnobBoolWPtr asLights;
    KnobIntWPtr lightMax;
    KnobDoubleWPtr lightIntensity, lightRange;
};

Zap3D::Zap3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Zap3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Zap3D::~Zap3D()
{
}

std::string
Zap3D::getPluginDescription() const
{
    return tr("Procedural lightning bolt as 3D geometry.\n\n"
              "The same midpoint-displacement bolt as the Nik Kit Zap node (branching, smooth Wiggle, "
              "Restrike, Flicker, Progress, tip taper), built between the Start and End points - or "
              "between the 'from' and 'to' inputs (any node with Translate knobs, or a Light3D) - and "
              "turned into a thin emissive tube.\n\n"
              "Connect it to a Scene3D like any geometry. In Cycles the emission makes it a real light: "
              "it casts shadows, lights volumes and shows in reflections. FastVolumeRender places a "
              "chain of point lights along it (Lights page). The 3D viewport and ScanlineRender show "
              "the tube (grey / white). Add the 2D Zap or Glare in comp for the glow.").toStdString();
}

std::string
Zap3D::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "from";
    if (inputNb == 1) return "to";
    if (inputNb == 2) return "mat";
    return "";
}

void
Zap3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Zap3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Zap3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

StatusEnum
Zap3D::getPreferredMetadata(NodeMetadata& metadata)
{
    // the bolt wiggles / restrikes / flickers with time: downstream hashes must move per frame
    metadata.setIsFrameVarying(true);
    return eStatusOK;
}

// ==================== Knobs ====================

namespace {

KnobDoublePtr makeDouble(KnobHolder* holder, const KnobPagePtr& page, const QString& label, const char* name,
                         double def, double dmin, double dmax, const QString& hint, bool animates = true,
                         bool hasMin = false, double minv = 0., bool hasMax = false, double maxv = 0.)
{
    KnobDoublePtr k = AppManager::createKnob<KnobDouble>(holder, label);
    k->setName(name);
    k->setDefaultValue(def);
    k->setDisplayMinimum(dmin);
    k->setDisplayMaximum(dmax);
    if (hasMin) k->setMinimum(minv);
    if (hasMax) k->setMaximum(maxv);
    k->setAnimationEnabled(animates);
    if (!hint.isEmpty()) k->setHintToolTip(hint);
    page->addKnob(k);
    return k;
}

KnobIntPtr makeInt(KnobHolder* holder, const KnobPagePtr& page, const QString& label, const char* name,
                   int def, int minv, int maxv, int dmax, const QString& hint, bool animates = true)
{
    KnobIntPtr k = AppManager::createKnob<KnobInt>(holder, label);
    k->setName(name);
    k->setDefaultValue(def);
    k->setMinimum(minv);
    k->setMaximum(maxv);
    k->setDisplayMinimum(minv);
    k->setDisplayMaximum(dmax);
    k->setAnimationEnabled(animates);
    if (!hint.isEmpty()) k->setHintToolTip(hint);
    page->addKnob(k);
    return k;
}

} // namespace

void
Zap3D::initializeKnobs()
{
    // ---- Zap page
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Zap"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start"), 3);
        k->setName("start");
        k->setDefaultValue(0.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(0.0, 2);
        k->setDisplayMinimum(-10.0, 0); k->setDisplayMaximum(10.0, 0);
        k->setDisplayMinimum(-10.0, 1); k->setDisplayMaximum(10.0, 1);
        k->setDisplayMinimum(-10.0, 2); k->setDisplayMaximum(10.0, 2);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Where the bolts begin, in the node's local space. Overridden by the 'from' input when connected."));
        page->addKnob(k); _imp->start = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End"), 3);
        k->setName("end");
        k->setDefaultValue(0.0, 0); k->setDefaultValue(0.0, 1); k->setDefaultValue(0.0, 2);
        k->setDisplayMinimum(-10.0, 0); k->setDisplayMaximum(10.0, 0);
        k->setDisplayMinimum(-10.0, 1); k->setDisplayMaximum(10.0, 1);
        k->setDisplayMinimum(-10.0, 2); k->setDisplayMaximum(10.0, 2);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Where the bolts aim. Overridden by the 'to' input when connected."));
        page->addKnob(k); _imp->end = k;
    }
    _imp->bolts = makeInt(this, page, tr("Bolts"), "bolts", 1, 0, 200, 20, tr("Number of bolts."));
    _imp->varyEnd = makeDouble(this, page, tr("Vary Endpoint"), "varyEndpoint", 0.0, 0.0, 1.0,
                               tr("Scatters the far end of each bolt around End, as a fraction of the bolt length."), true, true, 0.0);
    _imp->wrinkle = makeDouble(this, page, tr("Wrinkle"), "wrinkle", 0.6, 0.0, 2.0,
                               tr("Kink amplitude: 0 is a straight line, 1 the classic lightning jaggedness."), true, true, 0.0);
    _imp->detail = makeInt(this, page, tr("Detail"), "detail", 0, -4, 4, 3, tr("Extra subdivision levels (finer kinks); negative is coarser."), false);
    _imp->branchiness = makeDouble(this, page, tr("Branchiness"), "branchiness", 0.5, 0.0, 1.0,
                                   tr("How many side branches grow off the main bolt (0 = none)."), true, true, 0.0, true, 1.0);
    _imp->branchAngle = makeDouble(this, page, tr("Branch Angle"), "branchAngle", 45.0, 0.0, 90.0,
                                   tr("Maximum angle of a branch from its parent, in degrees."), true, true, 0.0, true, 180.0);
    _imp->branchLength = makeDouble(this, page, tr("Branch Length"), "branchLength", 0.5, 0.0, 1.0,
                                    tr("Branch length relative to the remaining length of its parent."), true, true, 0.0);
    _imp->branchDepth = makeInt(this, page, tr("Branch Depth"), "branchDepth", 2, 0, 5, 4, tr("Generations of branches (branches of branches)."), false);
    _imp->progress = makeDouble(this, page, tr("Progress"), "progress", 1.0, 0.0, 1.0,
                                tr("Grows the bolt from Start towards End: animate 0 to 1 for a strike."), true, true, 0.0, true, 1.0);
    _imp->wiggle = makeDouble(this, page, tr("Wiggle Speed"), "wiggleSpeed", 2.0, 0.0, 10.0,
                              tr("How fast every kink drifts, per second at the project frame rate. 0 freezes the shape."), true, true, 0.0);
    _imp->restrike = makeInt(this, page, tr("Restrike"), "restrike", 0, 0, 1000, 24,
                             tr("Re-roll the whole bolt structure every N frames (0 = never). 1 gives the frantic electric look."), false);
    _imp->flicker = makeDouble(this, page, tr("Flicker"), "flicker", 0.3, 0.0, 1.0,
                               tr("Random per-frame change of the emission strength (and the FastVolume lights)."), true, true, 0.0, true, 1.0);
    _imp->boltVar = makeDouble(this, page, tr("Bolt Variation"), "boltVariation", 0.5, 0.0, 1.0,
                               tr("Random thickness difference between bolts, so some strikes dominate."), true, true, 0.0, true, 1.0);
    _imp->tipFade = makeDouble(this, page, tr("Tip Fade"), "tipFade", 0.6, 0.0, 1.0,
                               tr("Tapers the last part of every bolt and branch towards its tip."), true, true, 0.0, true, 1.0);
    _imp->seed = makeInt(this, page, tr("Seed"), "seed", 1, 0, 100000, 100, tr("Random seed for the bolt shapes."), false);

    // ---- Shape page
    KnobPagePtr shapePage = AppManager::createKnob<KnobPage>(this, tr("Shape"));
    _imp->radius = makeDouble(this, shapePage, tr("Radius"), "radius", 0.02, 0.001, 0.5,
                              tr("Tube radius of the main bolt, in world units."), true, true, 0.0001);
    _imp->sides = makeInt(this, shapePage, tr("Sides"), "sides", 6, 3, 32, 16, tr("Sides of the tube cross-section."), false);
    _imp->branchScale = makeDouble(this, shapePage, tr("Branch Scale"), "branchScale", 0.6, 0.2, 1.0,
                                   tr("Radius of each branch generation relative to its parent."), true, true, 0.05, true, 1.0);

    // ---- Transform page (read by name by the SceneGraph / renderers)
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));
    _imp->translateX = makeDouble(this, xformPage, tr("Translate X"), "translateX", 0.0, -100.0, 100.0, QString());
    _imp->translateY = makeDouble(this, xformPage, tr("Translate Y"), "translateY", 0.0, -100.0, 100.0, QString());
    _imp->translateZ = makeDouble(this, xformPage, tr("Translate Z"), "translateZ", 0.0, -100.0, 100.0, QString());
    _imp->rotateX = makeDouble(this, xformPage, tr("Rotate X"), "rotateX", 0.0, -180.0, 180.0, QString());
    _imp->rotateY = makeDouble(this, xformPage, tr("Rotate Y"), "rotateY", 0.0, -180.0, 180.0, QString());
    _imp->rotateZ = makeDouble(this, xformPage, tr("Rotate Z"), "rotateZ", 0.0, -180.0, 180.0, QString());
    _imp->scaleX = makeDouble(this, xformPage, tr("Scale X"), "scaleX", 1.0, 0.01, 10.0, QString());
    _imp->scaleY = makeDouble(this, xformPage, tr("Scale Y"), "scaleY", 1.0, 0.01, 10.0, QString());
    _imp->scaleZ = makeDouble(this, xformPage, tr("Scale Z"), "scaleZ", 1.0, 0.01, 10.0, QString());
    _imp->uniformScale = makeDouble(this, xformPage, tr("Uniform Scale"), "uniformScale", 1.0, 0.001, 10.0,
                                    tr("Multiplies all three axes together."));

    // ---- Material page
    KnobPagePtr matPage = AppManager::createKnob<KnobPage>(this, tr("Material"));
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Base Color"), 3);
        k->setName("baseColor");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(1.0, 2);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->baseColor = k;
    }
    _imp->roughness = makeDouble(this, matPage, tr("Roughness"), "roughness", 0.5, 0.0, 1.0, QString(), true, true, 0.0, true, 1.0);
    _imp->metallic = makeDouble(this, matPage, tr("Metallic"), "metallic", 0.0, 0.0, 1.0, QString(), true, true, 0.0, true, 1.0);
    _imp->specular = makeDouble(this, matPage, tr("Specular"), "specular", 0.0, 0.0, 1.0, QString(), true, true, 0.0, true, 1.0);
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Emission Color"), 3);
        k->setName("emissionColor");
        k->setDefaultValue(0.6, 0); k->setDefaultValue(0.75, 1); k->setDefaultValue(1.0, 2);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->emissionColor = k;
    }
    _imp->emissionStrength = makeDouble(this, matPage, tr("Emission Strength"), "emissionStrength", 50.0, 0.0, 500.0,
                                        tr("Brightness of the bolt. In Cycles this is what lights the scene: a thin tube needs a high value (tens to hundreds)."),
                                        true, true, 0.0);
    _imp->transmission = makeDouble(this, matPage, tr("Transmission"), "transmission", 0.0, 0.0, 1.0, QString(), true, true, 0.0, true, 1.0);
    _imp->ior = makeDouble(this, matPage, tr("IOR"), "ior", 1.45, 1.0, 2.5, QString(), true, true, 1.0);

    // ---- Lights page (FastVolumeRender)
    KnobPagePtr lightPage = AppManager::createKnob<KnobPage>(this, tr("Lights"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("As Lights (FastVolume)"));
        k->setName("asLights"); k->setDefaultValue(true);
        k->setHintToolTip(tr("FastVolumeRender places a chain of point lights along the bolt so it lights the volume "
                             "(Cycles needs nothing: the emission already lights the scene)."));
        lightPage->addKnob(k); _imp->asLights = k;
    }
    _imp->lightMax = makeInt(this, lightPage, tr("Max Lights"), "lightMax", 32, 1, 128, 128,
                             tr("How many point lights along the bolts (FastVolumeRender has 128 in total)."), false);
    _imp->lightIntensity = makeDouble(this, lightPage, tr("Light Intensity"), "lightIntensity", 1.0, 0.0, 10.0,
                                      tr("Total light of the chain: 1 equals one Light3D point light at intensity 1, split along the bolt."),
                                      true, true, 0.0);
    _imp->lightRange = makeDouble(this, lightPage, tr("Light Range"), "lightRange", 0.5, 0.0, 5.0,
                                  tr("Half-intensity distance of each point light (0 = the volume's diagonal)."), true, true, 0.0);
}

// ==================== RoD / Render (dummy image) ====================

StatusEnum
Zap3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                             ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Zap3D::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;
    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.0f; }
        }
    }
    return eStatusOK;
}

// ==================== Endpoints ====================

namespace {

// position of an upstream node: a Light3D's light position, else its translate knobs
bool nodePosition(const EffectInstancePtr& e, double time, double* p)
{
    if (!e) {
        return false;
    }
    if (Light3D* L = dynamic_cast<Light3D*>(e.get())) {
        double r, g, b, inten, expo;
        L->getLightParams(time, p[0], p[1], p[2], r, g, b, inten, expo);
        return true;
    }
    const char* names[3] = {"translateX", "translateY", "translateZ"};
    for (int i = 0; i < 3; ++i) {
        KnobIPtr k = e->getKnobByName(names[i]);
        KnobDouble* kd = k ? dynamic_cast<KnobDouble*>(k.get()) : NULL;
        if (!kd) {
            return false;
        }
        p[i] = kd->getValueAtTime(time);
    }
    return true;
}

} // namespace

void
Zap3D::endpoints(double time, double* a, double* b) const
{
    KnobDoublePtr s = _imp->start.lock(), e = _imp->end.lock();
    for (int i = 0; i < 3; ++i) {
        a[i] = s ? s->getValueAtTime(time, i) : 0.;
        b[i] = e ? e->getValueAtTime(time, i) : 0.;
    }
    Zap3D* self = const_cast<Zap3D*>(this);
    double p[3];
    if (nodePosition(skipDots(self->getInput(0)), time, p)) {
        a[0] = p[0]; a[1] = p[1]; a[2] = p[2];
    }
    if (nodePosition(skipDots(self->getInput(1)), time, p)) {
        b[0] = p[0]; b[1] = p[1]; b[2] = p[2];
    }
}

double
Zap3D::flickerFactor(double time) const
{
    KnobDoublePtr f = _imp->flicker.lock();
    const double fl = f ? std::min(1., std::max(0., f->getValueAtTime(time))) : 0.;
    if (fl <= 0.) {
        return 1.;
    }
    KnobIntPtr sk = _imp->seed.lock();
    const unsigned int seed = sk ? (unsigned int)sk->getValue() : 1u;
    return std::max(0., 1. + fl * (hashU((unsigned int)(time * 31.) * 2654435761u + seed * 97u) - 0.5) * 2.);
}

// ==================== Snapshot ====================

unsigned long long
Zap3D::paramHash(double time) const
{
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&h](double v) {
        unsigned char b[sizeof(double)];
        std::memcpy(b, &v, sizeof b);
        for (size_t k = 0; k < sizeof b; ++k) { h ^= b[k]; h *= 1099511628211ULL; }
    };
    mix(time);
    double a[3], b[3];
    endpoints(time, a, b);
    for (int i = 0; i < 3; ++i) { mix(a[i]); mix(b[i]); }
    const KnobDoubleWPtr dk[] = {_imp->varyEnd, _imp->wrinkle, _imp->branchiness, _imp->branchAngle, _imp->branchLength,
                                 _imp->progress, _imp->wiggle, _imp->boltVar, _imp->tipFade, _imp->radius, _imp->branchScale};
    for (size_t i = 0; i < sizeof(dk) / sizeof(dk[0]); ++i) {
        KnobDoublePtr k = dk[i].lock();
        mix(k ? k->getValueAtTime(time) : 0.);
    }
    const KnobIntWPtr ik[] = {_imp->bolts, _imp->detail, _imp->branchDepth, _imp->restrike, _imp->seed, _imp->sides};
    for (size_t i = 0; i < sizeof(ik) / sizeof(ik[0]); ++i) {
        KnobIntPtr k = ik[i].lock();
        mix(k ? (double)k->getValueAtTime(time) : 0.);
    }
    return h;
}

Zap3D::SnapshotPtr
Zap3D::snapshot(double time) const
{
    const unsigned long long key = paramHash(time);
    {
        std::lock_guard<std::mutex> lk(_snapMutex);
        if (_snap && _snapKey == key) {
            return _snap;
        }
    }
    SnapshotPtr built = buildSnapshot(time);
    std::lock_guard<std::mutex> lk(_snapMutex);
    _snap = built;
    _snapKey = key;
    return _snap;
}

Zap3D::SnapshotPtr
Zap3D::buildSnapshot(double time) const
{
    std::shared_ptr<Snapshot> snap = std::make_shared<Snapshot>();
    MeshDataPtr mesh = std::make_shared<MeshData>();
    snap->mesh = mesh;

    auto dval = [time](const KnobDoubleWPtr& w, double def) { KnobDoublePtr k = w.lock(); return k ? k->getValueAtTime(time) : def; };
    auto ival = [time](const KnobIntWPtr& w, int def) { KnobIntPtr k = w.lock(); return k ? k->getValueAtTime(time) : def; };

    double a[3], b[3];
    endpoints(time, a, b);
    const V3 A = v3(a[0], a[1], a[2]), B = v3(b[0], b[1], b[2]);
    const double len = length(B - A);
    const int nBolts = std::max(0, ival(_imp->bolts, 1));
    if (len < 1e-6 || nBolts == 0) {
        return snap;
    }

    BoltParams bp;
    bp.wrinkle = std::max(0., dval(_imp->wrinkle, 0.6));
    bp.detail = ival(_imp->detail, 0);
    bp.branchiness = std::min(1., std::max(0., dval(_imp->branchiness, 0.5)));
    bp.branchAngle = dval(_imp->branchAngle, 45.);
    bp.branchLength = std::max(0., dval(_imp->branchLength, 0.5));
    bp.branchScale = std::min(1., std::max(0.05, dval(_imp->branchScale, 0.6)));
    double fps = 24.;
    if (getApp() && getApp()->getProject()) {
        const double pf = getApp()->getProject()->getProjectFrameRate();
        if (pf > 0.) fps = pf;
    }
    bp.wiggleT = time / fps * dval(_imp->wiggle, 2.);
    const int restrike = ival(_imp->restrike, 0);
    const int seed = ival(_imp->seed, 1);
    const unsigned int strike = restrike > 0 ? (unsigned int)std::floor(time / restrike) : 0u;
    const double varyEnd = std::max(0., dval(_imp->varyEnd, 0.));
    const double progress = std::min(1., std::max(0., dval(_imp->progress, 1.)));
    const double boltVar = std::min(1., std::max(0., dval(_imp->boltVar, 0.5)));
    const double tipFade = std::min(1., std::max(0., dval(_imp->tipFade, 0.6)));
    const double radius = std::max(1e-5, dval(_imp->radius, 0.02));
    const int sides = std::max(3, std::min(64, ival(_imp->sides, 6)));
    const int branchDepth = std::max(0, ival(_imp->branchDepth, 2));

    V3 e1, e2;
    basis(normalize(B - A), e1, e2);

    struct Tube { Poly poly; double gain; };
    std::vector<Tube> tubes;
    for (int i = 0; i < nBolts; ++i) {
        Rng r((unsigned int)(seed * 7919 + i * 131 + 17) ^ (strike * 2654435761u));
        // vary endpoint: scatter the far end in the plane perpendicular to the bolt
        const double az = r.frand() * 2. * M_PI, d = varyEnd * len * 0.5 * r.frand();
        const V3 end = B + (e1 * std::cos(az) + e2 * std::sin(az)) * d;
        const double gain = 1. - boltVar * r.frand();
        Rng rb((unsigned int)(seed * 104729 + i * 7877 + 3) ^ (strike * 2246822519u));
        std::vector<Poly> polys;
        genBolt(A, end, bp, rb, branchDepth, 1., 0., 1., polys);
        for (size_t k = 0; k < polys.size(); ++k) {
            trimPoly(polys[k], progress);
            if (polys[k].pts.size() < 2) {
                continue;
            }
            Tube t;
            t.poly = polys[k];
            t.gain = gain;
            tubes.push_back(t);
        }
    }

    // ---- tube mesh (quads, CCW from outside) + light samples
    double wsum = 0.;
    for (size_t ti = 0; ti < tubes.size(); ++ti) {
        const Poly& p = tubes[ti].poly;
        const size_t n = p.pts.size();
        const double rbase = radius * p.wscale * (0.4 + 0.6 * tubes[ti].gain);
        // per-point tangent + parallel-transported frame
        std::vector<V3> tan(n), f1(n), f2(n);
        for (size_t i = 0; i < n; ++i) {
            const V3 prev = i > 0 ? p.pts[i - 1] : p.pts[i];
            const V3 next = i + 1 < n ? p.pts[i + 1] : p.pts[i];
            tan[i] = normalize(next - prev);
        }
        basis(tan[0], f1[0], f2[0]);
        for (size_t i = 1; i < n; ++i) {
            // project the previous frame onto the new tangent plane
            V3 u = f1[i - 1] - tan[i] * dot(f1[i - 1], tan[i]);
            if (length(u) < 1e-6) {
                basis(tan[i], f1[i], f2[i]);
            } else {
                f1[i] = normalize(u);
                f2[i] = cross(tan[i], f1[i]);
            }
        }
        const int base = (int)(mesh->vertices.size() / 3);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / (double)(n - 1);
            const double tt = (t - 0.55) / 0.45;
            const double fade = tt <= 0. ? 0. : (tt >= 1. ? 1. : tt * tt * (3. - 2. * tt));
            const double rr = rbase * std::max(0.08, 1. - tipFade * fade);
            for (int s = 0; s < sides; ++s) {
                const double th = 2. * M_PI * s / sides;
                const V3 v = p.pts[i] + (f1[i] * std::cos(th) + f2[i] * std::sin(th)) * rr;
                mesh->vertices.push_back((float)v.x);
                mesh->vertices.push_back((float)v.y);
                mesh->vertices.push_back((float)v.z);
            }
        }
        for (size_t i = 0; i + 1 < n; ++i) {
            const int r0 = base + (int)i * sides, r1 = r0 + sides;
            const float u0 = (float)i / (float)(n - 1), u1 = (float)(i + 1) / (float)(n - 1);
            for (int s = 0; s < sides; ++s) {
                const int s1 = (s + 1) % sides;
                const float v0 = (float)s / (float)sides, v1 = (float)(s + 1) / (float)sides;
                // (i,s) (i,s+1) (i+1,s+1) (i+1,s): outward with the right-handed (f1, f2, tan) frame
                mesh->faceIndices.push_back(r0 + s);
                mesh->faceIndices.push_back(r0 + s1);
                mesh->faceIndices.push_back(r1 + s1);
                mesh->faceIndices.push_back(r1 + s);
                mesh->faceCounts.push_back(4);
                mesh->uvs.push_back(u0); mesh->uvs.push_back(v0);
                mesh->uvs.push_back(u0); mesh->uvs.push_back(v1);
                mesh->uvs.push_back(u1); mesh->uvs.push_back(v1);
                mesh->uvs.push_back(u1); mesh->uvs.push_back(v0);
                mesh->edgeIndices.push_back(r0 + s);
                mesh->edgeIndices.push_back(r1 + s);
            }
            // light sample at the segment midpoint
            LightSample ls;
            const V3 m = (p.pts[i] + p.pts[i + 1]) * 0.5;
            ls.pos[0] = (float)m.x; ls.pos[1] = (float)m.y; ls.pos[2] = (float)m.z;
            ls.weight = (float)(length(p.pts[i + 1] - p.pts[i]) * p.wscale * tubes[ti].gain);
            wsum += ls.weight;
            snap->samples.push_back(ls);
        }
        // ring edges on the first and last ring, so the wireframe shows the tube ends
        for (int s = 0; s < sides; ++s) {
            mesh->edgeIndices.push_back(base + s);
            mesh->edgeIndices.push_back(base + (s + 1) % sides);
            const int last = base + (int)(n - 1) * sides;
            mesh->edgeIndices.push_back(last + s);
            mesh->edgeIndices.push_back(last + (s + 1) % sides);
        }
    }
    if (wsum > 0.) {
        for (size_t i = 0; i < snap->samples.size(); ++i) {
            snap->samples[i].weight = (float)(snap->samples[i].weight / wsum);
        }
    }
    mesh->numVertices = mesh->vertices.size() / 3;
    mesh->numFaces = mesh->faceCounts.size();
    mesh->texCoordComponents = 2;
    mesh->hasUVs = !mesh->uvs.empty();
    return snap;
}

MeshDataPtr
Zap3D::getMeshData(double time) const
{
    if (time < 0.) {
        time = 0.;
    }
    return snapshot(time)->mesh;
}

// ==================== Lights (FastVolumeRender) ====================

bool
Zap3D::getAsLights() const
{
    KnobBoolPtr k = _imp->asLights.lock();
    return k ? k->getValue() : false;
}

double
Zap3D::getLightIntensity(double time) const
{
    KnobDoublePtr k = _imp->lightIntensity.lock();
    return (k ? k->getValueAtTime(time) : 1.) * flickerFactor(time);
}

double
Zap3D::getLightRange(double time) const
{
    KnobDoublePtr k = _imp->lightRange.lock();
    return k ? k->getValueAtTime(time) : 0.5;
}

void
Zap3D::getLightSamples(double time, int maxN, std::vector<LightSample>& out) const
{
    out.clear();
    if (!getAsLights() || maxN <= 0) {
        return;
    }
    KnobIntPtr km = _imp->lightMax.lock();
    maxN = std::min(maxN, km ? std::max(1, km->getValue()) : 32);
    const std::vector<LightSample>& all = snapshot(time)->samples;
    if ((int)all.size() <= maxN) {
        out = all;
        return;
    }
    // stride along the channels, folding the skipped neighbours' weight into the kept light
    const double stride = (double)all.size() / maxN;
    for (int i = 0; i < maxN; ++i) {
        const size_t s0 = (size_t)(i * stride), s1 = std::min(all.size(), (size_t)((i + 1) * stride));
        LightSample ls = all[s0];
        float w = 0.f;
        for (size_t j = s0; j < s1; ++j) w += all[j].weight;
        ls.weight = w;
        out.push_back(ls);
    }
}

// ==================== MaterialProvider ====================

void
Zap3D::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = g = b = 1.0; }
}
double Zap3D::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }
double Zap3D::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }
double Zap3D::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.0; }
void
Zap3D::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.6; g = 0.75; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = (s ? s->getValueAtTime(time) : 50.0) * flickerFactor(time);
}
double Zap3D::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }
double Zap3D::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }
std::string Zap3D::getMaterialTextureFile() const
{ return std::string(); }
bool Zap3D::hasMaterialInput() const
{
    EffectInstancePtr inp = skipDots(const_cast<Zap3D*>(this)->getInput(2));
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}
MaterialProvider* Zap3D::getConnectedMaterial() const
{
    EffectInstancePtr inp = skipDots(const_cast<Zap3D*>(this)->getInput(2));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_Zap3D.cpp"
