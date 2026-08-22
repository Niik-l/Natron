/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "FastVolumeRender.h"

#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include <QtCore/QMutex>

#include "../../AppManager.h"
#include "../../AppInstance.h"
#include "../../Image.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../Project.h"
#include "../DotUtils.h"
#include "../Scene3D/CameraProvider.h"
#include "../Scene3D/Light3D.h"
#include "../Scene3D/ReadVDB.h"
#include "../Scene3D/SceneGraph.h"
#include "../Scene3D/Volume3D.h"

#include "FastVolumeCore.h"

NATRON_NAMESPACE_ENTER

// Camera/orientation conventions, verified against this fork:
//   1. Natron cameras look down local -Z (extrinsic XYZ, R = Rz*Ry*Rx),
//      identical to FastVolumeCore (fwdW = -R[:,2]). CyclesRenderer negates
//      this column to reach Cycles' +Z; we do not. No flip needed.
//   2. Natron Image pixel space is bottom-up (row 0 = bottom); the core
//      writes row 0 = top, so the plane-write loop flips Y (srcY below).
//   3. getPreferredMetadata sets IsFrameVarying so frame scrubbing through an
//      animated VDB sequence forces a fresh render() each frame.

// Identifies the uploaded volume: anything here changing means the compressed
// GPU bricks are stale and upload() must run again. Knob-only changes (look,
// camera) leave this untouched, so they reuse the resident upload via draw().
struct VolumeKey {
    double time = -1e30;
    std::string path, bindD, bindF, bindT;
    int frameOff = 0, gridIdx = -1;
    float vol[16] = {0};
    uint64_t v3hash = 0;            // procedural Volume3D shape hash (0 for ReadVDB)
    bool valid = false;
    bool operator==(const VolumeKey& o) const {
        return valid && o.valid && time == o.time && frameOff == o.frameOff
            && gridIdx == o.gridIdx && path == o.path && bindD == o.bindD
            && bindF == o.bindF && bindT == o.bindT && v3hash == o.v3hash
            && memcmp(vol, o.vol, sizeof vol) == 0;
    }
};

struct FastVolumeRenderPrivate {
    QMutex rendererLock;
    std::unique_ptr<FastVolume::Renderer> renderer;   // lazy, lives with node
    VolumeKey uploaded;                               // what render last upload()ed

    // Sun comes from the scene's Distant Light3D and ambient fill from a Dome
    // Light3D — neither has knobs. The rest are pure look controls.
    KnobDoubleWPtr sigma, albedo, hgG, fireK, fireMax, fireLight;
    KnobIntWPtr steps, lightFactor;
    // AOV enable toggles (AOVs page). Beauty is always produced.
    KnobBoolWPtr aovEmission, aovAmbient, aovDepth, aovTemperature, aovShadow, aovLightGroups;
};

FastVolumeRender::FastVolumeRender(NodePtr node)
    : EffectInstance(node)
    , _imp(new FastVolumeRenderPrivate)
{
}

FastVolumeRender::~FastVolumeRender() {}

std::string
FastVolumeRender::getPluginDescription() const
{
    return "Real-time GPU volume renderer for VDB grids. Wire a scene (a "
           "ReadVDB volume plus a Distant Light3D, optionally under a Group3D) "
           "into the Scene input; the sun direction, color and intensity are "
           "taken from the Distant light. Renders smoke/fire in milliseconds "
           "via a compressed brick ray marcher (single scattering + sun shadow "
           "grid + blackbody-style emission). Outputs linear premultiplied RGBA "
           "plus AOV planes (Emission, SunScatter, AmbientScatter, Depth, "
           "Temperature, Shadow). Interactive stand-in for CyclesRender volumes; "
           "swap to Cycles for final-quality frames.";
}

std::string
FastVolumeRender::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "Scene";
        case 1: return "Camera";
    }
    return "";
}

bool
FastVolumeRender::isInputOptional(int inputNb) const
{
    return inputNb == 1;   // camera optional -> auto-frame
}

void
FastVolumeRender::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
}

void
FastVolumeRender::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
FastVolumeRender::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Density Scale"));
        k->setName("sigma");
        k->setDefaultValue(0.55);
        k->setMinimum(0.0);
        k->setDisplayMinimum(0.0);
        k->setDisplayMaximum(5.0);
        k->setHintToolTip(tr("Extinction multiplier on the density grid. Higher = thicker smoke."));
        k->setAnimationEnabled(true);
        page->addKnob(k);
        _imp->sigma = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Albedo"));
        k->setName("albedo");
        k->setDefaultValue(0.72);
        k->setMinimum(0.0);
        k->setMaximum(1.0);
        k->setHintToolTip(tr("Scattering albedo. 1 = white smoke, lower = sootier."));
        page->addKnob(k);
        _imp->albedo = k;
    }
    // (Sun comes from the scene's Distant Light3D and ambient fill from a Dome
    // Light3D — see render(). No manual sun/ambient knobs.)
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Anisotropy"));
        k->setName("hgG");
        k->setDefaultValue(0.5);
        k->setMinimum(-0.9);
        k->setMaximum(0.99);
        k->setHintToolTip(tr("Henyey-Greenstein phase anisotropy. >0 = forward scattering (silver lining)."));
        page->addKnob(k);
        _imp->hgG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Fire Intensity"));
        k->setName("fireK");
        k->setDefaultValue(2.2);
        k->setMinimum(0.0);
        k->setDisplayMaximum(20.0);
        k->setAnimationEnabled(true);
        page->addKnob(k);
        _imp->fireK = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Fire Max"));
        k->setName("fireMax");
        k->setDefaultValue(3.0);
        k->setMinimum(0.01);
        k->setHintToolTip(tr("Flames-grid value mapped to the top of the fire color ramp."));
        page->addKnob(k);
        _imp->fireMax = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Fire Light"));
        k->setName("fireLight");
        k->setDefaultValue(1.0);
        k->setMinimum(0.0);
        k->setDisplayMaximum(5.0);
        k->setHintToolTip(tr("How much the fire illuminates the surrounding smoke "
                             "(orange glow). 0 = fire only emits to camera, doesn't light smoke."));
        k->setAnimationEnabled(true);
        page->addKnob(k);
        _imp->fireLight = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Steps"));
        k->setName("steps");
        k->setDefaultValue(1024);
        k->setMinimum(64);
        k->setMaximum(4096);
        k->setHintToolTip(tr("Ray march steps across the volume. Draft mode uses a quarter."));
        page->addKnob(k);
        _imp->steps = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Shadow Grid Detail"));
        k->setName("lightFactor");
        k->setDefaultValue(4);
        k->setMinimum(2);
        k->setMaximum(16);
        k->setHintToolTip(tr("Sun shadow grid cell size in voxels. Smaller = sharper self-shadowing, slower."));
        page->addKnob(k);
        _imp->lightFactor = k;
    }

    // ---- AOVs page: tickbox per output pass (like CyclesRenderPass). Beauty
    // is always produced; everything else is opt-out here.
    KnobPagePtr aovs = AppManager::createKnob<KnobPage>(this, tr("AOVs"));
    auto aovToggle = [&](const char* name, const QString& label,
                         const QString& hint, KnobBoolWPtr& slot) {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, label);
        k->setName(name);
        k->setDefaultValue(true);
        k->setHintToolTip(hint);
        k->setAnimationEnabled(false);
        aovs->addKnob(k);
        slot = k;
    };
    aovToggle("aovEmission", tr("Emission"),
              tr("Fire emission pass (RGB)."), _imp->aovEmission);
    aovToggle("aovAmbient", tr("Ambient Scatter"),
              tr("Ambient/indirect + fire-light scatter pass (RGB)."), _imp->aovAmbient);
    aovToggle("aovDepth", tr("Depth"),
              tr("Mean world-space depth pass."), _imp->aovDepth);
    aovToggle("aovTemperature", tr("Temperature"),
              tr("Mean flames (temperature) value pass."), _imp->aovTemperature);
    aovToggle("aovShadow", tr("Shadow"),
              tr("Key-light visibility / shadow matte pass."), _imp->aovShadow);
    aovToggle("aovLightGroups", tr("Light Group AOVs"),
              tr("One RGBA scatter pass per light group (named by each Light3D's "
                 "Light Group; ungrouped lights -> 'main'). Each viewed group AOV "
                 "costs one extra render pass."), _imp->aovLightGroups);
}

StatusEnum
FastVolumeRender::getPreferredMetadata(NodeMetadata& metadata)
{
    // A volume render always depends on the current frame (the VDB sequence
    // and any animated camera/look knobs), so force time into the ImageKey —
    // otherwise scrubbing would show a stale cached frame. Mirrors
    // CyclesRender::getPreferredMetadata.
    metadata.setIsFrameVarying(true);
    metadata.setNComps(-1, 4);                      // RGBA
    metadata.setBitDepth(-1, eImageBitDepthFloat);

    // Output canvas = project format (no bg input feeds the resolution).
    Format f;
    getApp()->getProject()->getProjectDefaultFormat(&f);
    RectI fmt;
    fmt.x1 = f.x1; fmt.y1 = f.y1;
    fmt.x2 = f.x2; fmt.y2 = f.y2;
    metadata.setOutputFormat(fmt);
    return eStatusOK;
}

StatusEnum
FastVolumeRender::getRegionOfDefinition(U64 /*hash*/, double /*time*/,
                                        const RenderScale& /*scale*/,
                                        ViewIdx /*view*/, RectD* rod)
{
    // RoD is in canonical (full-resolution) coordinates — not scaled by the
    // render scale (CyclesRender::getRegionOfDefinition ignores it likewise).
    // Output covers the project format, as there is no bg input to size from.
    Format f;
    getApp()->getProject()->getProjectDefaultFormat(&f);
    rod->x1 = f.x1;
    rod->y1 = f.y1;
    rod->x2 = f.x2;
    rod->y2 = f.y2;
    return eStatusOK;
}

namespace {

ImagePlaneDesc aovPlane(const std::string& id, const std::string& label)
{
    static const char* rgb3[] = {"R", "G", "B"};
    return ImagePlaneDesc(id, label, "", rgb3, 3);
}

ImagePlaneDesc aovPlaneRGBA(const std::string& id, const std::string& label)
{
    static const char* rgba4[] = {"R", "G", "B", "A"};
    return ImagePlaneDesc(id, label, "", rgba4, 4);
}

// Fixed AOV plane IDs (everything NOT in this set produced by us is a light
// group AOV). Used to tell group planes apart in render().
bool isFixedAovId(const std::string& id)
{
    return id == "Emission" || id == "AmbientScatter" || id == "Depth"
        || id == "Temperature" || id == "Shadow";
}

// Light group name for a light: its Light Group knob, or "main" if unset.
std::string lightGroupName(Light3D* L)
{
    std::string g = L->getLightGroup();
    return g.empty() ? std::string("main") : g;
}

// Ordered unique light-group names across the project's renderable non-dome
// lights (cap MAX_LIGHTS). Drives which group AOV planes are produced.
std::vector<std::string> collectLightGroups(EffectInstance* self)
{
    std::vector<std::string> groups;
    std::set<std::string> seen;
    AppInstancePtr app = self->getApp();
    if (!app) return groups;
    ProjectPtr proj = app->getProject();
    if (!proj) return groups;
    NodesList all;
    proj->getNodes_recursive(all, true);
    for (const NodePtr& n : all) {
        if (!n || n->isNodeDisabled()) continue;
        Light3D* L = dynamic_cast<Light3D*>(n->getEffectInstance().get());
        if (!L || !L->isRenderable() || L->getLightType() == Light3D::eLightDome) continue;
        const std::string g = lightGroupName(L);
        if (seen.insert(g).second && (int)groups.size() < FastVolume::MAX_LIGHTS)
            groups.push_back(g);
    }
    return groups;
}

#ifdef NATRON_HAVE_OPENVDB
// Build an OpenVDB density grid from a procedural Volume3D. The Volume3D emits a
// dense res^3 field in local [-1,1]^3; we wrap it in a sparse FloatGrid whose
// index->world transform maps voxels into [-1,1]. The SceneNode world matrix
// (buildTRS of the node's centre/rotation/scale) is applied on top by upload(),
// so placement matches how Cycles renders the same Volume3D.
openvdb::FloatGrid::Ptr buildVolume3DGrid(Volume3D* v3, double time)
{
    std::vector<float> data;
    int res = 0;
    v3->generateVolumeData(time, data, res);
    if (res <= 0 || (int)data.size() < res * res * res) return openvdb::FloatGrid::Ptr();

    const Volume3D::VolumeParams vp = v3->getVolumeParams(time);
    const float ds = vp.density;
    const double vs = 2.0 / (double)res;            // voxel size in [-1,1] space
    const double t = -1.0 + vs * 0.5;               // first voxel centre

    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.0f);
    openvdb::math::Mat4d m(vs,  0.0, 0.0, 0.0,
                           0.0, vs,  0.0, 0.0,
                           0.0, 0.0, vs,  0.0,
                           t,   t,   t,   1.0);       // index -> local [-1,1]
    grid->setTransform(openvdb::math::Transform::createLinearTransform(m));

    openvdb::FloatGrid::Accessor acc = grid->getAccessor();
    for (int z = 0; z < res; ++z)
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x) {
                const float d = data[((size_t)z * res + y) * res + x] * ds;
                if (d > 1e-4f) acc.setValue(openvdb::Coord(x, y, z), d);
            }
    return grid;
}
#endif

}  // namespace

void
FastVolumeRender::getComponentsNeededAndProduced(double time, ViewIdx view,
                                                 EffectInstance::ComponentsNeededMap* comps,
                                                 double* passThroughTime,
                                                 int* passThroughView,
                                                 int* passThroughInput)
{
    std::list<ImagePlaneDesc> produced;
    produced.push_back(ImagePlaneDesc::getRGBAComponents());     // Beauty (always)
    auto on = [](const KnobBoolWPtr& w) { KnobBoolPtr k = w.lock(); return !k || k->getValue(); };
    if (on(_imp->aovEmission))    produced.push_back(aovPlane("Emission", "Emission"));
    if (on(_imp->aovAmbient))     produced.push_back(aovPlane("AmbientScatter", "Ambient Scatter"));
    if (on(_imp->aovDepth))       produced.push_back(aovPlane("Depth", "Depth"));
    if (on(_imp->aovTemperature)) produced.push_back(aovPlane("Temperature", "Temperature"));
    if (on(_imp->aovShadow))      produced.push_back(aovPlane("Shadow", "Shadow"));
    if (on(_imp->aovLightGroups)) {
        // One RGBA scatter pass per light group found in the scene.
        for (const std::string& g : collectLightGroups(this))
            produced.push_back(aovPlaneRGBA(g, g));
    }
    (*comps)[-1] = produced;

    // Pass-through for planes we don't render (mirrors CyclesRender). These
    // outputs must be initialized or the caller reads garbage.
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInput = 0;          // VDB input
}

StatusEnum
FastVolumeRender::render(const RenderActionArgs& args)
{
    // ---- the Scene input must be connected: it is the dependency edge that
    // makes Natron re-render when the upstream volume or light changes.
    if (!getInput(0)) {
        setPersistentMessage(eMessageTypeError,
            "FastVolumeRender: connect a scene (a ReadVDB volume + a Distant "
            "Light3D, optionally under a Group3D) to the Scene input.");
        return eStatusFailed;
    }

#ifdef NATRON_HAVE_OPENVDB
    // ---- walk the scene like CyclesRender: find the volume (first ReadVDB)
    // and the sun (first renderable Distant Light3D), each with a world matrix
    // that already includes any parent Group3D transform.
    SceneGraph scene;
    {
        NodesList allNodes;
        getApp()->getProject()->getNodes_recursive(allNodes, true);
        scene.rebuild(allNodes, args.time);
    }
    // Only consider nodes actually wired upstream of the Scene input — walk the
    // input graph from input 0 and collect the reachable Nodes. This makes
    // discovery follow the wiring: disconnecting a light (or its Group) removes
    // it, and the SceneGraph (built from ALL project nodes) is filtered to it.
    std::set<const Node*> upstream;
    {
        std::vector<EffectInstancePtr> todo;
        todo.push_back(getInput(0));
        while (!todo.empty()) {
            EffectInstancePtr e = todo.back();
            todo.pop_back();
            if (!e) continue;
            NodePtr en = e->getNode();
            if (!en || !upstream.insert(en.get()).second) continue;  // already seen
            const int ni = e->getNInputs();
            for (int i = 0; i < ni; ++i) todo.push_back(e->getInput(i));
        }
    }

    ReadVDB* readVdb = NULL;
    Volume3D* vol3d = NULL;                         // procedural volume (alt source)
    float volMatrix[16];
    Light3D* dome = NULL;                          // first Dome -> ambient fill
    int nKey = 0;                                  // key lights (point/spot/distant/area)
    Light3D* keyLights[FastVolume::MAX_LIGHTS];
    float keyWorld[FastVolume::MAX_LIGHTS][16];
    std::string keyGroup[FastVolume::MAX_LIGHTS];  // light group name per key light
    for (const SceneNode& sn : scene.nodes()) {
        NodePtr n = sn.sourceNode.lock();
        if (!n) continue;
        if (!upstream.count(n.get())) continue;    // not wired into this node
        if (n->isNodeDisabled()) continue;         // node muted (D) -> skip
        EffectInstance* eff = n->getEffectInstance().get();
        if (!readVdb && !vol3d && sn.type == eSceneNodeVolume && sn.visible) {
            if (ReadVDB* rv = dynamic_cast<ReadVDB*>(eff)) {
                readVdb = rv;
                memcpy(volMatrix, sn.worldMatrix, sizeof volMatrix);
            } else if (Volume3D* v3 = dynamic_cast<Volume3D*>(eff)) {
                vol3d = v3;
                memcpy(volMatrix, sn.worldMatrix, sizeof volMatrix);
            }
        } else if (sn.type == eSceneNodeLight) {
            if (Light3D* L = dynamic_cast<Light3D*>(eff)) {
                if (!L->isRenderable()) continue;  // light's own Renderable toggle
                if (L->getLightType() == Light3D::eLightDome) {
                    if (!dome) dome = L;
                } else if (nKey < FastVolume::MAX_LIGHTS) {
                    keyLights[nKey] = L;
                    memcpy(keyWorld[nKey], sn.worldMatrix, sizeof keyWorld[nKey]);
                    keyGroup[nKey] = lightGroupName(L);
                    ++nKey;
                }
            }
        }
    }
    if (!readVdb && !vol3d) {
        setPersistentMessage(eMessageTypeError,
            "FastVolumeRender: no volume in the connected scene (add a ReadVDB or Volume3D).");
        return eStatusFailed;
    }
    // No lights is fine: the volume renders unlit (a fire sim still shows its
    // emission + fire self-light; plain smoke renders dark until a light is added).

    // ---- upload cache key: everything that would change the compressed volume,
    // read from the source's knobs (no disk / no generation). A knob-only
    // re-render leaves this unchanged, so it reuses the resident GPU upload.
    VolumeKey key;
    key.valid = true;
    memcpy(key.vol, volMatrix, sizeof key.vol);
    if (readVdb) {
        key.time = args.time;   // VDB data is per-frame; Volume3D keys on v3hash only
        auto strKnob = [&](const char* n) -> std::string {
            KnobIPtr k = readVdb->getKnobByName(n);
            if (KnobStringBase* s = dynamic_cast<KnobStringBase*>(k.get()))
                return s->getValue();
            return std::string();
        };
        auto intKnob = [&](const char* n) -> int {
            KnobIPtr k = readVdb->getKnobByName(n);
            if (KnobIntBase* s = dynamic_cast<KnobIntBase*>(k.get()))
                return (int)s->getValueAtTime(args.time);
            return 0;
        };
        key.path = strKnob("filePath");
        key.bindD = strKnob("bindDensity");
        key.bindF = strKnob("bindFlame");
        key.bindT = strKnob("bindTemperature");
        key.frameOff = intKnob("frameOffset");
        key.gridIdx = intKnob("gridName");
    } else {  // Volume3D: one hash of all shape params (re-upload when it changes)
        key.v3hash = vol3d->getShapeHash(args.time);
    }

    // ---- camera input
    FastVolume::CameraParams cam;
    {
        EffectInstancePtr camEffect = skipDots(getInput(1));
        CameraProvider* cp = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;
        if (cp) {
            cp->getCameraPosition(args.time, cam.tx, cam.ty, cam.tz,
                                  cam.rx, cam.ry, cam.rz);
            cam.focal = cp->getCameraFocalLength(args.time);
            cam.hAperture = cp->getCameraHAperture(args.time);
            cam.vAperture = cp->getCameraVAperture(args.time);
            cam.valid = true;
        }
    }

    // ---- look from knobs
    FastVolume::LookParams look;
    look.sigma = (float)_imp->sigma.lock()->getValueAtTime(args.time);
    look.albedo = (float)_imp->albedo.lock()->getValueAtTime(args.time);
    // Lights from the scene (found above). Each light's world transform gives
    // position + axis; getLightParams gives color and intensity*2^exposure.
    // kSunScale maps Light3D intensity into the renderer's extinction scale
    // (the old hand-tuned default sun was ~18). Point/spot additionally fall
    // off with distance (scene-relative; tune intensity in the viewer).
    static const float kSunScale = 18.0f;
    look.numLights = nKey;
    for (int i = 0; i < nKey; ++i) {
        Light3D* L = keyLights[i];
        const float* wm = keyWorld[i];
        FastVolume::LightDesc& ld = look.lights[i];
        const Light3D::LightType lt = L->getLightType();
        ld.type = (lt == Light3D::eLightDistant) ? FastVolume::LIGHT_DISTANT
                : (lt == Light3D::eLightSpot)    ? FastVolume::LIGHT_SPOT
                : (lt == Light3D::eLightArea)    ? FastVolume::LIGHT_AREA
                                                 : FastVolume::LIGHT_POINT;
        ld.pos[0] = wm[12]; ld.pos[1] = wm[13]; ld.pos[2] = wm[14];
        // world Z column: distant/area use +Z (TO the light); spot uses -Z (shine axis)
        const float zx = wm[8], zy = wm[9], zz = wm[10];
        const float s = (ld.type == FastVolume::LIGHT_SPOT) ? -1.0f : 1.0f;
        ld.dir[0] = s * zx; ld.dir[1] = s * zy; ld.dir[2] = s * zz;
        double ltx, lty, ltz, lr, lg, lb, lint, lexp;
        L->getLightParams(args.time, ltx, lty, ltz, lr, lg, lb, lint, lexp);
        const float k = (float)(lint * std::pow(2.0, lexp)) * kSunScale;
        ld.color[0] = (float)lr * k; ld.color[1] = (float)lg * k; ld.color[2] = (float)lb * k;
        if (ld.type == FastVolume::LIGHT_SPOT) {
            const double outer = (L->getSpotAngle(args.time) * 0.5) * M_PI / 180.0;
            const double smooth = L->getSpotSmooth(args.time);
            ld.cosOuter = (float)std::cos(outer);
            ld.cosInner = (float)std::cos(outer * (1.0 - 0.9 * smooth));
        } else if (ld.type == FastVolume::LIGHT_AREA) {
            // Rectangular beam: Width/Height -> cross-section. World X/Y columns
            // are the beam axes; sizeU/sizeV are full Width/Height. Long+thin =
            // a laser/light shaft.
            ld.uax[0] = wm[0]; ld.uax[1] = wm[1]; ld.uax[2] = wm[2];   // local X
            ld.vax[0] = wm[4]; ld.vax[1] = wm[5]; ld.vax[2] = wm[6];   // local Y
            ld.halfU = 0.5f * (float)L->getAreaSizeU(args.time);
            ld.halfV = 0.5f * (float)L->getAreaSizeV(args.time);
        }
    }
    // Ambient fill comes solely from a Dome (Environment) Light3D: solid
    // lightColor * intensity*2^exposure. No dome -> no ambient (pure sun
    // lighting). HDRI env maps are NOT sampled by this real-time model, only
    // the dome's solid color.
    look.ambient[0] = look.ambient[1] = look.ambient[2] = 0.0f;
    if (dome) {
        static const float kDomeScale = 0.25f;   // dome units -> ambient fill (tune in viewer)
        double dtx, dty, dtz, dr, dg, db, dint, dexp;
        dome->getLightParams(args.time, dtx, dty, dtz, dr, dg, db, dint, dexp);
        const float k = (float)(dint * std::pow(2.0, dexp)) * kDomeScale;
        look.ambient[0] = (float)dr * k;
        look.ambient[1] = (float)dg * k;
        look.ambient[2] = (float)db * k;
    }
    look.hgG = (float)_imp->hgG.lock()->getValueAtTime(args.time);
    look.fireK = (float)_imp->fireK.lock()->getValueAtTime(args.time);
    look.fireMax = (float)_imp->fireMax.lock()->getValueAtTime(args.time);
    look.fireLight = (float)_imp->fireLight.lock()->getValueAtTime(args.time);
    look.steps = _imp->steps.lock()->getValueAtTime(args.time);
    if (args.draftMode) look.steps = std::max(64, look.steps / 4);
    look.lightFactor = _imp->lightFactor.lock()->getValueAtTime(args.time);

    // ---- render: upload() only when the volume key changed (new frame, file,
    // or transform); draw() every time. Knob-only changes skip the disk read
    // in getVDBDirect AND the recompression, reusing the resident GPU bricks.
    const int W = args.roi.width();
    const int H = args.roi.height();
    FastVolume::RenderOutput rout;
    std::map<std::string, FastVolume::RenderOutput> groupOuts;   // light-group AOVs
    const std::string beautyId = ImagePlaneDesc::getRGBAComponents().getPlaneID();
    bool emptyVolume = false;   // volume carved to nothing -> render transparent
    {
        QMutexLocker lock(&_imp->rendererLock);
        if (!_imp->renderer)
            _imp->renderer.reset(new FastVolume::Renderer);
        if (!_imp->renderer->valid()) {
            setPersistentMessage(eMessageTypeError,
                ("FastVolumeRender: GPU init failed: " + _imp->renderer->lastError()).c_str());
            return eStatusFailed;
        }

        if (!(key == _imp->uploaded) || !_imp->renderer->hasVolume()) {
            // Volume changed — (re)build the grids and upload.
            openvdb::FloatGrid::ConstPtr density, flames;
            if (readVdb) {
                ReadVDB::VDBDirectData vdbData;
                if (!readVdb->getVDBDirect(args.time, vdbData)) {
                    // Name the file and the OpenVDB error. The bare "failed to
                    // load VDB grids" gave the user nothing to act on — the
                    // usual cause is a resolved frame that doesn't exist, and
                    // the path says so at a glance.
                    const std::string why = readVdb->getLastLoadError();
                    std::string msg = "FastVolumeRender: failed to load VDB grids.";
                    if ( !why.empty() ) {
                        msg = "FastVolumeRender: failed to load VDB grids — " + why;
                    }
                    setPersistentMessage( eMessageTypeError, msg.c_str() );
                    return eStatusFailed;
                }
                // grid selection honors ReadVDB's binding knobs, with fallbacks
                auto findGrid = [&vdbData](const std::string& primary,
                                           const char* fb1, const char* fb2)
                    -> openvdb::FloatGrid::ConstPtr {
                    for (const auto& gi : vdbData.grids)
                        if (!primary.empty() && gi.name == primary)
                            return openvdb::gridConstPtrCast<openvdb::FloatGrid>(gi.grid);
                    for (const auto& gi : vdbData.grids)
                        if (gi.name == fb1 || (fb2 && gi.name == fb2))
                            return openvdb::gridConstPtrCast<openvdb::FloatGrid>(gi.grid);
                    return nullptr;
                };
                density = findGrid(vdbData.bindDensity, "density", NULL);
                flames  = findGrid(vdbData.bindFlame.empty() ? vdbData.bindTemperature : vdbData.bindFlame,
                                   "flames", "flame");
            } else {
                // Procedural Volume3D: build a density grid (no fire channel).
                density = buildVolume3DGrid(vol3d, args.time);
            }
            // A null grid means a genuine load problem (e.g. a ReadVDB density
            // grid name not found). An *empty* grid (no active voxels) is not an
            // error: a Volume3D carved to nothing by Coverage, or an empty VDB
            // frame, should render fully transparent — handled below.
            if (!density) {
                setPersistentMessage(eMessageTypeError, "FastVolumeRender: no density grid.");
                return eStatusFailed;
            }
            if (density->empty()) {
                emptyVolume = true;
                _imp->uploaded = VolumeKey();   // re-upload once it fills again
            } else if (!_imp->renderer->upload(density, flames, volMatrix)) {
                setPersistentMessage(eMessageTypeError,
                    ("FastVolumeRender: " + _imp->renderer->lastError()).c_str());
                _imp->uploaded = VolumeKey();   // invalidate
                return eStatusFailed;
            } else {
                _imp->uploaded = key;
            }
        }

        if (!emptyVolume && !_imp->renderer->draw(cam, look, W, H, rout)) {
            setPersistentMessage(eMessageTypeError,
                ("FastVolumeRender: " + _imp->renderer->lastError()).c_str());
            return eStatusFailed;
        }

        // Per-light-group AOVs: one extra draw per requested group plane, using
        // only that group's lights with emission/ambient/fire off, so the plane
        // holds that group's pure scatter. Reuses the cached upload.
        for (auto& pp : args.outputPlanes) {
            if (emptyVolume) break;   // nothing to scatter -> all planes transparent
            const std::string id = pp.first.getPlaneID();
            if (id == beautyId || isFixedAovId(id) || groupOuts.count(id)) continue;
            FastVolume::LookParams glook = look;
            glook.fireK = 0.0f; glook.fireLight = 0.0f;
            glook.ambient[0] = glook.ambient[1] = glook.ambient[2] = 0.0f;
            int gc = 0;
            for (int i = 0; i < nKey; ++i)
                if (keyGroup[i] == id) glook.lights[gc++] = look.lights[i];
            glook.numLights = gc;
            FastVolume::RenderOutput gout;
            if (gc > 0) _imp->renderer->draw(cam, glook, W, H, gout);
            groupOuts[id] = gout;   // gc == 0 -> empty (written as black below)
        }
    }
    clearPersistentMessage(false);

    // ---- write planes (core row 0 = image top; Natron y1 = image bottom)
    for (auto& planePair : args.outputPlanes) {
        const ImagePlaneDesc& desc = planePair.first;
        ImagePtr img = planePair.second;
        const RectI bounds = img->getBounds();
        const int nc = desc.getNumComponents();
        const std::string id = desc.getPlaneID();

        const std::vector<float>* src = &rout.beauty;   // beauty (color plane)
        int srcN = 4;                     // RGB count within the layer vec4
        bool fromAlpha = false;
        bool blackPlane = emptyVolume;    // empty volume -> every plane transparent
        if (emptyVolume) { /* leave blackPlane true; skip source selection */ }
        else if (id == "Emission") { src = &rout.emission; srcN = 3; }
        else if (id == "AmbientScatter") { src = &rout.ambScatter; srcN = 3; }
        else if (id == "Depth") { src = &rout.emission; fromAlpha = true; }
        else if (id == "Temperature") { src = &rout.sunScatter; fromAlpha = true; }
        else if (id == "Shadow") { src = &rout.ambScatter; fromAlpha = true; }
        else if (id != beautyId) {        // a light-group AOV (RGBA scatter)
            std::map<std::string, FastVolume::RenderOutput>::const_iterator it = groupOuts.find(id);
            if (it != groupOuts.end() && !it->second.beauty.empty()) src = &it->second.beauty;
            else blackPlane = true;       // group with no wired lights -> black
        }

        Image::WriteAccess wa(img.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            const int srcY = (H - 1) - (y - args.roi.y1);
            if (srcY < 0 || srcY >= H) continue;
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                const int srcX = x - args.roi.x1;
                if (srcX < 0 || srcX >= W) continue;
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                if (blackPlane) { for (int c = 0; c < nc; ++c) dst[c] = 0.0f; continue; }
                const float* s = src->data() + ((size_t)srcY * W + srcX) * 4;
                if (fromAlpha) {
                    for (int c = 0; c < nc; ++c) dst[c] = s[3];
                } else {
                    for (int c = 0; c < nc; ++c) dst[c] = c < srcN ? s[c] : s[3];
                }
            }
        }
    }
    return eStatusOK;
#else
    setPersistentMessage(eMessageTypeError, "FastVolumeRender requires OpenVDB support.");
    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_FastVolumeRender.cpp"
