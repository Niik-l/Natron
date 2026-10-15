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

#include "ParticleInstance.h"
#include "../DotUtils.h"
#include "../Scene3D/SceneGraph.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct ParticleInstancePrivate
{
    KnobChoiceWPtr distribution;
    KnobBoolWPtr   orientToVelocity;
    KnobBoolWPtr   alignToNormal;
    KnobChoiceWPtr upAxis;
    KnobDoubleWPtr randomRotation;   // 3 dims, degrees
    KnobDoubleWPtr scaleMultiplier;
    KnobDoubleWPtr randomScale;      // 2 dims: min, max
    KnobIntWPtr    seed;
    KnobBoolWPtr   applyPrototypeTransform;
    KnobIntWPtr    maxInstances;
};

namespace {

// Small integer hash -> [0,1). Per particle ID so an instance keeps its
// rotation and scale from frame to frame, and across renderers.
inline uint32_t hashU32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline float rnd01(uint32_t id, uint32_t seed, uint32_t k)
{
    return (hashU32(id * 0x9E3779B1u + seed * 0x85EBCA77u + k * 0xC2B2AE3Du + 0x27d4eb2fu) & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

// Rotation taking unit `from` onto unit `to` (Rodrigues), column-major.
void rotationBetween(const float from[3], const float to[3], float out[16])
{
    geoMat4Identity(out);
    const float d = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    float ax = from[1] * to[2] - from[2] * to[1];
    float ay = from[2] * to[0] - from[0] * to[2];
    float az = from[0] * to[1] - from[1] * to[0];
    float al = std::sqrt(ax * ax + ay * ay + az * az);
    if (al < 1e-6f) {
        if (d > 0.f) {
            return; // already aligned
        }
        // Opposite: turn 180 degrees about any axis perpendicular to `from`.
        if (std::fabs(from[0]) < 0.9f) { ax = 0.f; ay = -from[2]; az = from[1]; }
        else                            { ax = -from[2]; ay = 0.f; az = from[0]; }
        al = std::sqrt(ax * ax + ay * ay + az * az);
        ax /= al; ay /= al; az /= al;
        const float c = -1.f, s = 0.f, t = 1.f - c;
        out[0] = c + ax * ax * t;      out[1] = ay * ax * t + az * s; out[2]  = az * ax * t - ay * s;
        out[4] = ax * ay * t - az * s; out[5] = c + ay * ay * t;      out[6]  = az * ay * t + ax * s;
        out[8] = ax * az * t + ay * s; out[9] = ay * az * t - ax * s; out[10] = c + az * az * t;
        return;
    }
    ax /= al; ay /= al; az /= al;
    const float c = std::max(-1.f, std::min(1.f, d));
    const float s = al; // |from x to| = sin(angle) for unit vectors
    const float t = 1.f - c;
    out[0] = c + ax * ax * t;      out[1] = ay * ax * t + az * s; out[2]  = az * ax * t - ay * s;
    out[4] = ax * ay * t - az * s; out[5] = c + ay * ay * t;      out[6]  = az * ay * t + ax * s;
    out[8] = ax * az * t + ay * s; out[9] = ay * az * t - ax * s; out[10] = c + az * az * t;
}

} // namespace

ParticleInstance::ParticleInstance(NodePtr node)
    : EffectInstance(node)
    , _imp(new ParticleInstancePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleInstance::~ParticleInstance()
{
}

std::string
ParticleInstance::getPluginDescription() const
{
    return tr("Instance geometry at particle positions.\n\n"
              "Input 0: particles (ParticleSolver, an emitter chain, Scatter3D, PointsToParticles)\n"
              "Input 1-4: geometry sources A-D. Any geo the 3D system renders: Cube3D, Sphere3D, "
              "Card3D, Cylinder3D, ReadGeo (a Megascans asset with its Material3D), GeoBuilder, "
              "ReadAlembicArchive.\n\n"
              "Each particle gets one geometry source (Distribution) and is drawn with that "
              "prototype's material. Particle size times Scale Multiplier is the instance scale; "
              "Random Scale and Random Rotation vary each instance by its particle ID, so the "
              "variation is stable over time. Particles that carry a surface normal (Scatter3D) "
              "stand up along it when Align to Normal is on.\n\n"
              "Particle color tints the instance in the preview renderers. CyclesRender shares one "
              "mesh between all instances of a prototype (native instancing), so tens of thousands "
              "of instances cost little more than one.").toStdString();
}

std::string
ParticleInstance::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "particles";
    if (inputNb == 1) return "geo A";
    if (inputNb == 2) return "geo B";
    if (inputNb == 3) return "geo C";
    if (inputNb == 4) return "geo D";
    return "";
}

void
ParticleInstance::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleInstance::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleInstance::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ParticleInstance::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Instance"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Distribution"));
        k->setName("distribution"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Sequential", "", "Cycle through connected geos: A, B, C, D, A, B..."));
        entries.push_back(ChoiceOption("Random", "", "Random geo per particle (deterministic by particle ID)"));
        entries.push_back(ChoiceOption("By ID", "", "particle ID modulo number of connected geos"));
        k->populateChoices(entries);
        k->setDefaultValue(1); // Random default
        page->addKnob(k); _imp->distribution = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Orient to Velocity"));
        k->setName("orientToVelocity"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Rotate instances to face their velocity direction (the prototype's +Z). "
                             "Takes precedence over Align to Normal while a particle is moving."));
        page->addKnob(k); _imp->orientToVelocity = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Align to Normal"));
        k->setName("alignToNormal"); k->setDefaultValue(true);
        k->setHintToolTip(tr("When a particle carries a surface normal (Scatter3D), turn the prototype's "
                             "Up Axis onto it, so debris sits flat on a slope and plants grow out of it. "
                             "Particles without a normal keep the prototype upright."));
        page->addKnob(k); _imp->alignToNormal = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Up Axis"));
        k->setName("upAxis"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Y", "", "The prototype was modelled Y-up (Natron, Maya, Megascans FBX/OBJ)"));
        entries.push_back(ChoiceOption("Z", "", "The prototype was modelled Z-up (Blender, Houdini exports)"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Which prototype axis is 'up' for Align to Normal and for Random Rotation's yaw."));
        page->addKnob(k); _imp->upAxis = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Random Rotation"), 3);
        k->setName("randomRotation");
        k->setDimensionName(0, "x"); k->setDimensionName(1, "y"); k->setDimensionName(2, "z");
        for (int d = 0; d < 3; ++d) {
            k->setDefaultValue(0.0, d);
            k->setMinimum(0.0, d); k->setMaximum(360.0, d);
            k->setDisplayMinimum(0.0, d); k->setDisplayMaximum(360.0, d);
        }
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Each instance is turned by a random angle in [-range, +range] degrees about "
                             "each of its own axes (applied before Align to Normal, so the up axis stays on "
                             "the surface). Set the up axis to 360 for a free spin, the other two to a few "
                             "degrees for a natural lean."));
        page->addKnob(k); _imp->randomRotation = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Multiplier"));
        k->setName("scaleMultiplier"); k->setDefaultValue(1.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Global scale multiplier on all instances (times the particle size)."));
        page->addKnob(k); _imp->scaleMultiplier = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Random Scale"), 2);
        k->setName("randomScale");
        k->setDimensionName(0, "min"); k->setDimensionName(1, "max");
        for (int d = 0; d < 2; ++d) {
            k->setDefaultValue(1.0, d);
            k->setMinimum(0.0, d); k->setMaximum(1000.0, d);
            k->setDisplayMinimum(0.0, d); k->setDisplayMaximum(3.0, d);
        }
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Each instance picks a uniform scale factor between min and max (by particle ID)."));
        page->addKnob(k); _imp->randomScale = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("seed"); k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(1000);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Reshuffles Random Rotation, Random Scale and the Random distribution."));
        page->addKnob(k); _imp->seed = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Apply Prototype Transform"));
        k->setName("applyPrototypeTransform"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Include each prototype node's own translate/rotate/scale knobs in every instance. "
                             "Leave on so a ReadGeo scaled from centimetres to metres instances at the right "
                             "size; turn off to instance the raw mesh and ignore where the prototype sits in "
                             "the scene."));
        page->addKnob(k); _imp->applyPrototypeTransform = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Instances"));
        k->setName("maxInstances"); k->setDefaultValue(10000);
        k->setMinimum(1); k->setDisplayMinimum(100); k->setDisplayMaximum(200000);
        k->setHintToolTip(tr("Maximum number of instances to render. Limits performance impact."));
        page->addKnob(k); _imp->maxInstances = k;
    }
}

bool
ParticleInstance::getApplyPrototypeTransform() const
{
    KnobBoolPtr k = _imp->applyPrototypeTransform.lock();
    return k ? k->getValue() : true;
}

bool
ParticleInstance::getPrototypeMesh(int geoIdx, double time, TriMesh& out) const
{
    if (geoIdx < 0 || geoIdx > 3) {
        return false;
    }
    EffectInstancePtr geo = getInput(geoIdx + 1);
    if (!geo) {
        return false;
    }
    if (!extractTriangleMesh(geo, time, out)) {
        return false;
    }
    if (!getApplyPrototypeTransform()) {
        geoMat4Identity(out.localMatrix);
    }
    return true;
}

ParticleDataPtr
ParticleInstance::getParticleData(double time)
{
    // Pass through particle data for downstream nodes
    EffectInstancePtr input = skipDots(getInput(0));
    if (!input) return ParticleDataPtr();
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input.get());
    if (!provider) return ParticleDataPtr();
    return provider->getParticleData(time);
}

void
ParticleInstance::getInstances(double time, std::vector<GeoInstance>& outInstances)
{
    outInstances.clear();

    // Get particle data from the upstream provider, then delegate
    EffectInstancePtr input0 = skipDots(getInput(0));
    if (!input0) return;
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input0.get());
    if (!provider) return;
    getInstancesFromData(provider->getParticleData(time), time, outInstances);
}

void
ParticleInstance::getInstancesFromData(const ParticleDataPtr& pdata, double time,
                                       std::vector<GeoInstance>& outInstances)
{
    outInstances.clear();
    if (!pdata || pdata->numParticles() == 0) return;

    // Count connected geo inputs
    std::vector<int> connectedGeos;
    for (int g = 1; g <= 4; ++g) {
        EffectInstancePtr geoInput = getInput(g);
        if (geoInput) {
            connectedGeos.push_back(g - 1); // 0-based index
        }
    }
    if (connectedGeos.empty()) return;

    const int   distMode  = _imp->distribution.lock() ? _imp->distribution.lock()->getValue() : 1;
    const bool  orientVel = _imp->orientToVelocity.lock() ? _imp->orientToVelocity.lock()->getValue() : false;
    const bool  alignN    = _imp->alignToNormal.lock() ? _imp->alignToNormal.lock()->getValue() : true;
    const int   upAxis    = _imp->upAxis.lock() ? _imp->upAxis.lock()->getValue() : 0;
    const float scaleMult = _imp->scaleMultiplier.lock() ? (float)_imp->scaleMultiplier.lock()->getValueAtTime(time) : 1.0f;
    const int   maxInst   = _imp->maxInstances.lock() ? _imp->maxInstances.lock()->getValue() : 10000;
    const uint32_t seed   = _imp->seed.lock() ? (uint32_t)std::max(0, _imp->seed.lock()->getValue()) : 0u;
    float rrange[3] = {0.f, 0.f, 0.f};
    if (KnobDoublePtr k = _imp->randomRotation.lock()) {
        for (int d = 0; d < 3; ++d) rrange[d] = (float)k->getValueAtTime(time, d);
    }
    float rsMin = 1.f, rsMax = 1.f;
    if (KnobDoublePtr k = _imp->randomScale.lock()) {
        rsMin = (float)k->getValueAtTime(time, 0);
        rsMax = (float)k->getValueAtTime(time, 1);
        if (rsMax < rsMin) std::swap(rsMin, rsMax);
    }
    const bool anyRandomRot = rrange[0] != 0.f || rrange[1] != 0.f || rrange[2] != 0.f;
    const float up[3] = { 0.f, upAxis == 1 ? 0.f : 1.f, upAxis == 1 ? 1.f : 0.f };

    const int numGeos = (int)connectedGeos.size();
    const int count = std::min(pdata->numParticles(), maxInst);

    outInstances.reserve(count);

    for (int i = 0; i < count; ++i) {
        const Particle& p = pdata->particles[i];

        GeoInstance inst;

        // Assign geo source
        int geoIdx = 0;
        if (distMode == 0) {
            // Sequential
            geoIdx = i % numGeos;
        } else if (distMode == 1) {
            // Random (deterministic by particle ID + seed)
            geoIdx = (int)(hashU32(p.id * 2654435761u + seed * 0x68E31DA4u) % (uint32_t)numGeos);
        } else {
            // By ID
            geoIdx = (int)(p.id % (uint32_t)numGeos);
        }
        inst.geoSourceIndex = connectedGeos[geoIdx];

        // Position
        inst.px = p.px;
        inst.py = p.py;
        inst.pz = p.pz;

        // Base orientation. Velocity first (extrinsic XYZ Euler, M = Rz*Ry*Rx
        // col-vec, aligning the prototype's +Z with the velocity:
        //   dx = sin(ry) cos(rx), dy = -sin(rx), dz = cos(ry) cos(rx)
        //   -> rx = -asin(dy), ry = atan2(dx, dz)), else the surface normal.
        inst.rx = inst.ry = inst.rz = 0.f;
        float Rbase[16];
        geoMat4Identity(Rbase);
        bool haveBase = false;
        if (orientVel) {
            const float speed = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
            if (speed > 0.001f) {
                const float dx = p.vx / speed, dy = p.vy / speed, dz = p.vz / speed;
                inst.ry = std::atan2(dx, dz) * 180.0f / (float)M_PI;
                inst.rx = -std::asin(std::max(-1.0f, std::min(1.0f, dy))) * 180.0f / (float)M_PI;
                inst.rz = 0;
                SceneGraph::buildTRS(0, 0, 0, inst.rx, inst.ry, inst.rz, 1, 1, 1, Rbase);
                haveBase = true;
            }
        }
        if (!haveBase && alignN) {
            const float nl = std::sqrt(p.nx * p.nx + p.ny * p.ny + p.nz * p.nz);
            if (nl > 0.5f) {
                const float n[3] = { p.nx / nl, p.ny / nl, p.nz / nl };
                rotationBetween(up, n, Rbase);
                haveBase = true;
            }
        }

        // Per-ID random rotation, in the prototype's own frame (before the
        // base rotation, so a yaw about the up axis stays a spin on the surface).
        float R[16];
        if (anyRandomRot) {
            float Rr[16];
            const float rx = (rnd01(p.id, seed, 1) * 2.f - 1.f) * rrange[0];
            const float ry = (rnd01(p.id, seed, 2) * 2.f - 1.f) * rrange[1];
            const float rz = (rnd01(p.id, seed, 3) * 2.f - 1.f) * rrange[2];
            SceneGraph::buildTRS(0, 0, 0, rx, ry, rz, 1, 1, 1, Rr);
            geoMat4Mul(R, Rbase, Rr);
            if (!haveBase) {
                inst.rx = rx; inst.ry = ry; inst.rz = rz; // legacy Euler consumers
            }
        } else {
            std::memcpy(R, Rbase, sizeof(R));
        }

        // Scale from particle size, global multiplier and the per-ID random factor
        float s = p.size * scaleMult;
        if (rsMin != 1.f || rsMax != 1.f) {
            s *= rsMin + (rsMax - rsMin) * rnd01(p.id, seed, 4);
        }
        inst.sx = s;
        inst.sy = s;
        inst.sz = s;

        // m = T * R * S
        for (int c = 0; c < 3; ++c) {
            inst.m[c * 4 + 0] = R[c * 4 + 0] * s;
            inst.m[c * 4 + 1] = R[c * 4 + 1] * s;
            inst.m[c * 4 + 2] = R[c * 4 + 2] * s;
            inst.m[c * 4 + 3] = 0.f;
        }
        inst.m[12] = p.px; inst.m[13] = p.py; inst.m[14] = p.pz; inst.m[15] = 1.f;

        // Color
        inst.r = p.r;
        inst.g = p.g;
        inst.b = p.b;
        inst.a = p.a;

        // Velocity (for renderer motion blur)
        inst.vx = p.vx;
        inst.vy = p.vy;
        inst.vz = p.vz;

        outInstances.push_back(inst);
    }
}

StatusEnum
ParticleInstance::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                        ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ParticleInstance::render(const RenderActionArgs& args)
{
    // Fill dummy output
    if (!args.outputPlanes.empty()) {
        ImagePtr outImg = args.outputPlanes.front().second;
        if (outImg) outImg->fillZero(args.roi);
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleInstance.cpp"
