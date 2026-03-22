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

#include "ParticleEmitter.h"

#include <cmath>
#include <random>
#include <algorithm>
#include <vector>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../NodeMetadata.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../TimeLine.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct ParticleEmitterPrivate
{
    // Emission
    KnobIntWPtr rate;
    KnobDoubleWPtr lifetime;
    KnobDoubleWPtr lifetimeVariance;
    KnobDoubleWPtr velocity;
    KnobDoubleWPtr velocityVariance;
    KnobDoubleWPtr spread;
    KnobDoubleWPtr startSize;
    KnobDoubleWPtr sizeVariance;
    KnobIntWPtr seed;

    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr emitDirX, emitDirY, emitDirZ;

    // Color
    KnobDoubleWPtr startColorR, startColorG, startColorB, startColorA;

    // Built-in forces
    KnobDoubleWPtr gravityX, gravityY, gravityZ;
    KnobDoubleWPtr drag;
    KnobDoubleWPtr turbulenceStrength;
    KnobDoubleWPtr turbulenceScale;
    KnobDoubleWPtr turbulenceSpeed;

    // Age-based appearance
    KnobDoubleWPtr endSize;
    KnobDoubleWPtr endColorR, endColorG, endColorB;
    KnobDoubleWPtr fadeIn, fadeOut;
};


ParticleEmitter::ParticleEmitter(NodePtr node)
    : EffectInstance(node)
    , _imp(new ParticleEmitterPrivate())
    , _lastSimFrame(-1e9)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleEmitter::~ParticleEmitter()
{
}

std::string
ParticleEmitter::getPluginDescription() const
{
    return tr("Particle emitter — spawns particles each frame and simulates them forward.\n\n"
              "No inputs — this is the source node for a particle system.\n"
              "Spawns particles with randomized velocity within an emission cone.\n"
              "Connect downstream to ParticleForce, ParticleMerge, or ParticleRender nodes.\n\n"
              "Equivalent to Nuke's ParticleEmitter node.").toStdString();
}

std::string
ParticleEmitter::getInputLabel(int /*inputNb*/) const
{
    return "";
}

void
ParticleEmitter::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleEmitter::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleEmitter::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
ParticleEmitter::initializeKnobs()
{
    // Emission page
    KnobPagePtr emissionPage = AppManager::createKnob<KnobPage>(this, tr("Emission"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Rate"));
        k->setName("rate"); k->setDefaultValue(100);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(10000);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->rate = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Lifetime"));
        k->setName("lifetime"); k->setDefaultValue(50.0);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(500.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->lifetime = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Lifetime Variance"));
        k->setName("lifetimeVariance"); k->setDefaultValue(10.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->lifetimeVariance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Velocity"));
        k->setName("velocity"); k->setDefaultValue(2.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(50.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->velocity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Velocity Variance"));
        k->setName("velocityVariance"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->velocityVariance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Spread (degrees)"));
        k->setName("spread"); k->setDefaultValue(30.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(180.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->spread = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Size"));
        k->setName("startSize"); k->setDefaultValue(0.1);
        k->setMinimum(0.001); k->setDisplayMinimum(0.001); k->setDisplayMaximum(10.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->startSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size Variance"));
        k->setName("sizeVariance"); k->setDefaultValue(0.02);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        emissionPage->addKnob(k); _imp->sizeVariance = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("seed"); k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(10000);
        k->setAnimationEnabled(false);
        emissionPage->addKnob(k); _imp->seed = k;
    }

    // Transform page
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emit Direction X"));
        k->setName("emitDirX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        xformPage->addKnob(k); _imp->emitDirX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emit Direction Y"));
        k->setName("emitDirY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        xformPage->addKnob(k); _imp->emitDirY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Emit Direction Z"));
        k->setName("emitDirZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        xformPage->addKnob(k); _imp->emitDirZ = k;
    }

    // Color page
    KnobPagePtr colorPage = AppManager::createKnob<KnobPage>(this, tr("Color"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color R"));
        k->setName("startColorR"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color G"));
        k->setName("startColorG"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color B"));
        k->setName("startColorB"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorB = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Start Color A"));
        k->setName("startColorA"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        colorPage->addKnob(k); _imp->startColorA = k;
    }

    // Forces page
    KnobPagePtr forcesPage = AppManager::createKnob<KnobPage>(this, tr("Forces"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gravity X"));
        k->setName("gravityX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-20.0); k->setDisplayMaximum(20.0);
        forcesPage->addKnob(k); _imp->gravityX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gravity Y"));
        k->setName("gravityY"); k->setDefaultValue(-2.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-20.0); k->setDisplayMaximum(20.0);
        forcesPage->addKnob(k); _imp->gravityY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gravity Z"));
        k->setName("gravityZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-20.0); k->setDisplayMaximum(20.0);
        forcesPage->addKnob(k); _imp->gravityZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Drag"));
        k->setName("drag"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(QString::fromUtf8("Velocity damping per frame. 0 = no drag, 1 = full stop."));
        forcesPage->addKnob(k); _imp->drag = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Turbulence"));
        k->setName("turbulenceStrength"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(5.0);
        k->setHintToolTip(QString::fromUtf8("Noise-based displacement strength."));
        forcesPage->addKnob(k); _imp->turbulenceStrength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Turb Scale"));
        k->setName("turbulenceScale"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        k->setHintToolTip(QString::fromUtf8("Spatial scale of turbulence noise."));
        forcesPage->addKnob(k); _imp->turbulenceScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Turb Speed"));
        k->setName("turbulenceSpeed"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(5.0);
        k->setHintToolTip(QString::fromUtf8("Time evolution speed of turbulence."));
        forcesPage->addKnob(k); _imp->turbulenceSpeed = k;
    }

    // Over Life page (age-based appearance)
    KnobPagePtr lifePage = AppManager::createKnob<KnobPage>(this, tr("Over Life"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Size"));
        k->setName("endSize"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        lifePage->addKnob(k); _imp->endSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Color R"));
        k->setName("endColorR"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lifePage->addKnob(k); _imp->endColorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Color G"));
        k->setName("endColorG"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lifePage->addKnob(k); _imp->endColorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("End Color B"));
        k->setName("endColorB"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        lifePage->addKnob(k); _imp->endColorB = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Fade In"));
        k->setName("fadeIn"); k->setDefaultValue(0.1); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(QString::fromUtf8("Fraction of life for fade-in (0-1). 0.1 = first 10% of life."));
        lifePage->addKnob(k); _imp->fadeIn = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Fade Out"));
        k->setName("fadeOut"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(QString::fromUtf8("Fraction of life for fade-out (0-1). 0.3 = last 30% of life."));
        lifePage->addKnob(k); _imp->fadeOut = k;
    }
}

// ==================== Particle Simulation ====================

/**
 * @brief Build an orthonormal basis from a direction vector.
 *
 * Given a normalized direction `dir`, produces two perpendicular vectors `tangent` and `bitangent`
 * such that (tangent, bitangent, dir) forms a right-handed orthonormal basis.
 */
static void
buildBasisFromDirection(float dx, float dy, float dz,
                        float& tx, float& ty, float& tz,
                        float& bx, float& by, float& bz)
{
    // Pick a vector not parallel to dir
    float ax = 0.0f, ay = 0.0f, az = 1.0f;
    if (std::fabs(dz) > 0.9f) {
        ax = 1.0f; ay = 0.0f; az = 0.0f;
    }

    // tangent = cross(dir, a)
    tx = dy * az - dz * ay;
    ty = dz * ax - dx * az;
    tz = dx * ay - dy * ax;
    float tLen = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (tLen > 1e-8f) { tx /= tLen; ty /= tLen; tz /= tLen; }

    // bitangent = cross(dir, tangent)
    bx = dy * tz - dz * ty;
    by = dz * tx - dx * tz;
    bz = dx * ty - dy * tx;
    float bLen = std::sqrt(bx * bx + by * by + bz * bz);
    if (bLen > 1e-8f) { bx /= bLen; by /= bLen; bz /= bLen; }
}

StatusEnum
ParticleEmitter::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setIsFrameVarying(true);
    return eStatusOK;
}

ParticleDataPtr
ParticleEmitter::getParticleData(double time)
{
    // Return cached result if already simulated to this frame
    if (_lastParticleData && time == _lastSimFrame) {
        return _lastParticleData;
    }

    // Incremental simulation: if we have cached data and moving forward,
    // continue from cached state instead of re-simulating from frame 1
    int startFrame = 1;
    ParticleDataPtr data;
    if (_lastParticleData && _lastSimFrame > 0 && time > _lastSimFrame) {
        startFrame = (int)_lastSimFrame + 1;
        data = std::make_shared<ParticleData>();
        data->particles = _lastParticleData->particles; // copy current state
    }

    // Read knob values at current time (most are constant but support animation)
    int rateVal           = _imp->rate.lock()->getValueAtTime(time);
    double lifetimeVal    = _imp->lifetime.lock()->getValueAtTime(time);
    double lifeVarVal     = _imp->lifetimeVariance.lock()->getValueAtTime(time);
    double velocityVal    = _imp->velocity.lock()->getValueAtTime(time);
    double velVarVal      = _imp->velocityVariance.lock()->getValueAtTime(time);
    double spreadVal      = _imp->spread.lock()->getValueAtTime(time);
    double startSizeVal   = _imp->startSize.lock()->getValueAtTime(time);
    double sizeVarVal     = _imp->sizeVariance.lock()->getValueAtTime(time);
    int seedVal           = _imp->seed.lock()->getValueAtTime(time);

    double emPosX = _imp->translateX.lock()->getValueAtTime(time);
    double emPosY = _imp->translateY.lock()->getValueAtTime(time);
    double emPosZ = _imp->translateZ.lock()->getValueAtTime(time);

    double edx = _imp->emitDirX.lock()->getValueAtTime(time);
    double edy = _imp->emitDirY.lock()->getValueAtTime(time);
    double edz = _imp->emitDirZ.lock()->getValueAtTime(time);

    float colR = (float)_imp->startColorR.lock()->getValueAtTime(time);
    float colG = (float)_imp->startColorG.lock()->getValueAtTime(time);
    float colB = (float)_imp->startColorB.lock()->getValueAtTime(time);
    float colA = (float)_imp->startColorA.lock()->getValueAtTime(time);

    // Normalize emit direction
    double edLen = std::sqrt(edx * edx + edy * edy + edz * edz);
    if (edLen < 1e-8) { edx = 0; edy = 1; edz = 0; }
    else { edx /= edLen; edy /= edLen; edz /= edLen; }

    float spreadRad = (float)(spreadVal * M_PI / 180.0);

    // Build orthonormal basis from emit direction
    float fdx = (float)edx, fdy = (float)edy, fdz = (float)edz;
    float tangentX, tangentY, tangentZ;
    float bitangentX, bitangentY, bitangentZ;
    buildBasisFromDirection(fdx, fdy, fdz,
                            tangentX, tangentY, tangentZ,
                            bitangentX, bitangentY, bitangentZ);

    // Seeded RNG — use frame-based seed for deterministic results
    // even with incremental simulation
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    if (!data) {
        data = std::make_shared<ParticleData>();
    }

    int endFrame = (int)std::floor(time);
    if (endFrame < 1) endFrame = 1;

    // If going backward or to a different timeline position, reset
    if (startFrame > endFrame + 1) {
        startFrame = 1;
        data->particles.clear();
    }

    // Simulate from startFrame to endFrame (incremental when going forward)
    for (int frame = startFrame; frame <= endFrame; ++frame) {

        // Per-frame deterministic RNG (same results regardless of start frame)
        std::mt19937 rng((unsigned int)(seedVal * 73856093 + frame * 19349663));

        // Spawn new particles
        for (int i = 0; i < rateVal; ++i) {
            Particle p;

            // Position at emitter location
            p.px = (float)emPosX;
            p.py = (float)emPosY;
            p.pz = (float)emPosZ;

            // Random direction within cone of `spread` degrees around emitDir
            float theta = spreadRad * std::sqrt(dist01(rng)); // uniform on disk
            float phi = 2.0f * (float)M_PI * dist01(rng);

            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            // Local direction in cone: sinTheta*cos(phi)*tangent + sinTheta*sin(phi)*bitangent + cosTheta*dir
            float localX = sinTheta * std::cos(phi) * tangentX + sinTheta * std::sin(phi) * bitangentX + cosTheta * fdx;
            float localY = sinTheta * std::cos(phi) * tangentY + sinTheta * std::sin(phi) * bitangentY + cosTheta * fdy;
            float localZ = sinTheta * std::cos(phi) * tangentZ + sinTheta * std::sin(phi) * bitangentZ + cosTheta * fdz;

            // Normalize the direction
            float dirLen = std::sqrt(localX * localX + localY * localY + localZ * localZ);
            if (dirLen > 1e-8f) { localX /= dirLen; localY /= dirLen; localZ /= dirLen; }

            // Randomize speed
            float speed = (float)velocityVal + (dist01(rng) * 2.0f - 1.0f) * (float)velVarVal;
            if (speed < 0.0f) speed = 0.0f;

            p.vx = localX * speed;
            p.vy = localY * speed;
            p.vz = localZ * speed;

            // Randomize lifetime
            p.life = (float)lifetimeVal + (dist01(rng) * 2.0f - 1.0f) * (float)lifeVarVal;
            if (p.life < 1.0f) p.life = 1.0f;

            // Randomize size
            p.size = (float)startSizeVal + (dist01(rng) * 2.0f - 1.0f) * (float)sizeVarVal;
            if (p.size < 0.001f) p.size = 0.001f;

            // Color
            p.r = colR;
            p.g = colG;
            p.b = colB;
            p.a = colA;

            p.age = 0.0f;
            p.mass = 1.0f;

            data->particles.push_back(p);
        }

        // Read all force/appearance knobs ONCE per frame (outside particle loop)
        float dt = 1.0f;
        float gx = (float)_imp->gravityX.lock()->getValueAtTime(frame);
        float gy = (float)_imp->gravityY.lock()->getValueAtTime(frame);
        float gz = (float)_imp->gravityZ.lock()->getValueAtTime(frame);
        float dragAmt = (float)_imp->drag.lock()->getValueAtTime(frame);
        float turbStr = (float)_imp->turbulenceStrength.lock()->getValueAtTime(frame);
        float turbScale = (float)_imp->turbulenceScale.lock()->getValueAtTime(frame);
        float turbSpeed = (float)_imp->turbulenceSpeed.lock()->getValueAtTime(frame);
        float turbTime = frame * turbSpeed * 0.01f;

        // Over-life knobs (read once per frame, NOT per particle)
        float startSz = (float)_imp->startSize.lock()->getValueAtTime(frame);
        float endSz = (float)_imp->endSize.lock()->getValueAtTime(frame);
        float sr = (float)_imp->startColorR.lock()->getValueAtTime(frame);
        float sg = (float)_imp->startColorG.lock()->getValueAtTime(frame);
        float sb = (float)_imp->startColorB.lock()->getValueAtTime(frame);
        float er = (float)_imp->endColorR.lock()->getValueAtTime(frame);
        float eg = (float)_imp->endColorG.lock()->getValueAtTime(frame);
        float eb = (float)_imp->endColorB.lock()->getValueAtTime(frame);
        float fadeInFrac = (float)_imp->fadeIn.lock()->getValueAtTime(frame);
        float fadeOutFrac = (float)_imp->fadeOut.lock()->getValueAtTime(frame);

        for (size_t j = 0; j < data->particles.size(); ++j) {
            Particle& p = data->particles[j];

            // Gravity
            p.vx += gx * dt * 0.01f;
            p.vy += gy * dt * 0.01f;
            p.vz += gz * dt * 0.01f;

            // Drag (velocity damping)
            if (dragAmt > 0.0f) {
                float damping = 1.0f - dragAmt;
                p.vx *= damping;
                p.vy *= damping;
                p.vz *= damping;
            }

            // Turbulence (simple hash-based noise)
            if (turbStr > 0.0f) {
                float invScale = 1.0f / std::max(0.01f, turbScale);
                // Hash-based pseudo noise using position
                float nx = p.px * invScale + turbTime;
                float ny = p.py * invScale + turbTime * 0.7f;
                float nz = p.pz * invScale + turbTime * 1.3f;

                // Simple noise using sin combinations
                float noiseX = sinf(nx * 12.9898f + ny * 78.233f) * 43758.5453f;
                noiseX = noiseX - floorf(noiseX);
                float noiseY = sinf(ny * 12.9898f + nz * 78.233f) * 43758.5453f;
                noiseY = noiseY - floorf(noiseY);
                float noiseZ = sinf(nz * 12.9898f + nx * 78.233f) * 43758.5453f;
                noiseZ = noiseZ - floorf(noiseZ);

                p.vx += (noiseX - 0.5f) * turbStr * 0.1f;
                p.vy += (noiseY - 0.5f) * turbStr * 0.1f;
                p.vz += (noiseZ - 0.5f) * turbStr * 0.1f;
            }

            // Update position
            p.px += p.vx * dt;
            p.py += p.vy * dt;
            p.pz += p.vz * dt;
            p.age += 1.0f;

            // Age-based appearance (interpolate start→end over life)
            float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
            ageFrac = std::max(0.0f, std::min(1.0f, ageFrac));

            // Size: lerp start→end
            p.size = startSz + (endSz - startSz) * ageFrac;

            // Color: lerp start→end
            p.r = sr + (er - sr) * ageFrac;
            p.g = sg + (eg - sg) * ageFrac;
            p.b = sb + (eb - sb) * ageFrac;

            // Alpha: fade in/out
            float alpha = 1.0f;
            if (fadeInFrac > 0.0f && ageFrac < fadeInFrac) {
                alpha = ageFrac / fadeInFrac;
            }
            if (fadeOutFrac > 0.0f && ageFrac > (1.0f - fadeOutFrac)) {
                alpha = (1.0f - ageFrac) / fadeOutFrac;
            }
            p.a = std::max(0.0f, std::min(1.0f, alpha));
        }

        // Remove expired particles (age >= life)
        data->removeExpired();
    }

    // Cache
    _lastParticleData = data;
    _lastSimFrame = time;

    return data;
}

// ==================== RoD / Render ====================

StatusEnum
ParticleEmitter::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                       ViewIdx /*view*/, RectD* rod)
{
    // Data node — 1x1 dummy output
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ParticleEmitter::render(const RenderActionArgs& args)
{
    // Data node — no meaningful image output, fill black
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

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleEmitter.cpp"
