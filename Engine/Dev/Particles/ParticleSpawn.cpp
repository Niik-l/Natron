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

#include "ParticleSpawn.h"
#include "../DotUtils.h"

#include <random>
#include <cmath>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../NodeMetadata.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../TimeLine.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct ParticleSpawnPrivate
{
    KnobChoiceWPtr trigger;
    KnobDoubleWPtr burstAge;
    KnobDoubleWPtr burstAgeVariance;
    KnobIntWPtr rate;
    KnobDoubleWPtr inheritVelocity;
    KnobDoubleWPtr extraSpeed;
    KnobDoubleWPtr spread;
    KnobDoubleWPtr childLifetime;
    KnobDoubleWPtr childSize;
    KnobDoubleWPtr childColorR;
    KnobDoubleWPtr childColorG;
    KnobDoubleWPtr childColorB;
    KnobDoubleWPtr probability;
};

ParticleSpawn::ParticleSpawn(NodePtr node)
    : EffectInstance(node)
    , _imp(new ParticleSpawnPrivate())
    , _cachedFrame(-1e9)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleSpawn::~ParticleSpawn()
{
}

std::string
ParticleSpawn::getPluginDescription() const
{
    return tr("Spawns secondary particles from a parent particle stream.\n\n"
              "Outputs ONLY the spawned child particles. Use ParticleMerge to combine\n"
              "with the parent stream. This allows independent force chains for children.\n\n"
              "Workflow: Emitter -> Gravity -> Merge -> Viewer\n"
              "             \\-> Spawn -> Wind -> Gravity -/").toStdString();
}

std::string
ParticleSpawn::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "parents";
    return "";
}

void
ParticleSpawn::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleSpawn::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleSpawn::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

StatusEnum
ParticleSpawn::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setIsFrameVarying(true);
    return eStatusOK;
}

void
ParticleSpawn::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Spawn"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Trigger"));
        k->setName("trigger"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Continuous", "", "Emit from every living parent particle every frame"));
        entries.push_back(ChoiceOption("On Birth", "", "Emit only from newly born parent particles"));
        entries.push_back(ChoiceOption("On Death", "", "Emit only from dying parent particles"));
        entries.push_back(ChoiceOption("On Collision", "", "Emit from particles that just collided (requires ParticleSolver upstream)"));
        entries.push_back(ChoiceOption("At Age (burst)", "", "Emit once when each parent reaches its burst age — mid-air firework bursts, crackle, secondary pops. Burst Age Variance staggers the bursts per particle."));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        mainPage->addKnob(k); _imp->trigger = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Burst Age"));
        k->setName("burstAge"); k->setDefaultValue(15.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("At Age trigger: parent age (frames) at which the burst fires. Each parent bursts exactly once."));
        mainPage->addKnob(k); _imp->burstAge = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Variance"));
        k->setName("burstAgeVariance"); k->setDefaultValue(5.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(50.0);
        k->setHintToolTip(tr("Per-particle random spread (+/-) on Burst Age, stable per parent ID — staggers the bursts so they don't all pop on the same frame."));
        mainPage->addKnob(k); _imp->burstAgeVariance = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Rate"));
        k->setName("rate"); k->setDefaultValue(2); k->setAnimationEnabled(true);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(50);
        mainPage->addKnob(k); _imp->rate = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Inherit Velocity"));
        k->setName("inheritVelocity"); k->setDefaultValue(0.5); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        mainPage->addKnob(k); _imp->inheritVelocity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Extra Speed"));
        k->setName("extraSpeed"); k->setDefaultValue(0.5); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        mainPage->addKnob(k); _imp->extraSpeed = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Spread"));
        k->setName("spread"); k->setDefaultValue(45.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(180.0);
        mainPage->addKnob(k); _imp->spread = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Child Lifetime"));
        k->setName("childLifetime"); k->setDefaultValue(20.0); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(200.0);
        mainPage->addKnob(k); _imp->childLifetime = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Child Size"));
        k->setName("childSize"); k->setDefaultValue(0.05); k->setAnimationEnabled(true);
        k->setMinimum(0.001); k->setDisplayMinimum(0.001); k->setDisplayMaximum(5.0);
        mainPage->addKnob(k); _imp->childSize = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Child Color R"));
        k->setName("childColorR"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        mainPage->addKnob(k); _imp->childColorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Child Color G"));
        k->setName("childColorG"); k->setDefaultValue(0.5); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        mainPage->addKnob(k); _imp->childColorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Child Color B"));
        k->setName("childColorB"); k->setDefaultValue(0.1); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        mainPage->addKnob(k); _imp->childColorB = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Probability"));
        k->setName("probability"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        mainPage->addKnob(k); _imp->probability = k;
    }
}

// ==================== Simulation ====================

ParticleDataPtr
ParticleSpawn::getParticleData(double time)
{
    // Serialise against concurrent callers (3D viewport paint on the GUI
    // thread vs render workers). The published snapshot is never mutated
    // after storage — all sim work happens on a private copy that replaces
    // the snapshot on publish — so the cache hit below can hand out the
    // shared pointer directly.
    std::lock_guard<std::mutex> computeLk(_computeMutex);

    // Return cached result if already simulated to this frame
    if (_cachedData && time == _cachedFrame) {
        return _cachedData;
    }

    // Get parent particle provider from input
    EffectInstancePtr input = skipDots(getInput(0));
    if (!input) return ParticleDataPtr();
    ParticleProvider* parentProvider = dynamic_cast<ParticleProvider*>(input.get());
    if (!parentProvider) return ParticleDataPtr();

    int endFrame = (int)std::floor(time);
    if (endFrame < 1) endFrame = 1;

    // Determine start frame for incremental sim, working on a private copy of
    // the cached children (never mutate the published snapshot in place).
    int startFrame = 1;
    ParticleDataPtr data = std::make_shared<ParticleData>();
    if (_cachedData && _cachedFrame > 0 && time > _cachedFrame) {
        startFrame = (int)_cachedFrame + 1;
        data->particles = _cachedData->particles;
    }

    // Read knob values
    int triggerMode = _imp->trigger.lock()->getValueAtTime(time);
    int rateVal = _imp->rate.lock()->getValueAtTime(time);
    float inheritVel = (float)_imp->inheritVelocity.lock()->getValueAtTime(time);
    float extraSpeed = (float)_imp->extraSpeed.lock()->getValueAtTime(time);
    float childLife = (float)_imp->childLifetime.lock()->getValueAtTime(time);
    float childSz = (float)_imp->childSize.lock()->getValueAtTime(time);
    float childR = (float)_imp->childColorR.lock()->getValueAtTime(time);
    float childG = (float)_imp->childColorG.lock()->getValueAtTime(time);
    float childB = (float)_imp->childColorB.lock()->getValueAtTime(time);
    float prob = (float)_imp->probability.lock()->getValueAtTime(time);
    float burstAgeVal = _imp->burstAge.lock() ? (float)_imp->burstAge.lock()->getValueAtTime(time) : 15.0f;
    float burstVarVal = _imp->burstAgeVariance.lock() ? (float)_imp->burstAgeVariance.lock()->getValueAtTime(time) : 5.0f;

    if (rateVal <= 0) {
        _cachedData = data;
        _cachedFrame = time;
        return _cachedData;
    }

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    // Frame-by-frame: scan parent particles, spawn children, age + integrate children
    for (int frame = startFrame; frame <= endFrame; ++frame) {

        // Get parent particles at this frame
        ParticleDataPtr parentData = parentProvider->getParticleData((double)frame);
        if (!parentData) continue;

        // Deterministic RNG per frame
        std::mt19937 rng((unsigned int)(frame * 54321 + 98765));

        // Spawn children from qualifying parent particles
        for (size_t i = 0; i < parentData->particles.size(); ++i) {
            const Particle& parent = parentData->particles[i];

            // Check trigger condition
            bool shouldSpawn = false;
            if (triggerMode == 0) {
                shouldSpawn = true;  // Continuous
            } else if (triggerMode == 1) {
                shouldSpawn = (parent.age <= 1.0f);  // On Birth
            } else if (triggerMode == 2) {
                shouldSpawn = (parent.age >= parent.life - 1.0f);  // On Death
            } else if (triggerMode == 3) {
                shouldSpawn = parent.collided;  // On Collision
            } else if (triggerMode == 4) {
                // At Age (burst): fire in the one-frame window where the
                // parent crosses its per-ID randomized target age. Stable
                // hash keeps the target identical across frames/re-sims.
                uint32_t h = parent.id * 2654435761u + 0x9E3779B9u;
                h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
                const float u = (float)(h & 0x00FFFFFFu) / 16777216.0f;
                const float target = burstAgeVal + (u * 2.0f - 1.0f) * burstVarVal;
                shouldSpawn = (parent.age >= target && parent.age < target + 1.0f);
            }

            if (!shouldSpawn) continue;
            if (dist01(rng) > prob) continue;

            for (int j = 0; j < rateVal; ++j) {
                Particle child;
                child.px = parent.px;
                child.py = parent.py;
                child.pz = parent.pz;

                // Inherit parent velocity
                child.vx = parent.vx * inheritVel;
                child.vy = parent.vy * inheritVel;
                child.vz = parent.vz * inheritVel;

                // Add random extra velocity in sphere
                float speed = extraSpeed * (0.5f + dist01(rng) * 0.5f);
                float rx = dist01(rng) * 2.0f - 1.0f;
                float ry = dist01(rng) * 2.0f - 1.0f;
                float rz = dist01(rng) * 2.0f - 1.0f;
                float rLen = std::sqrt(rx * rx + ry * ry + rz * rz);
                if (rLen > 0.001f) { rx /= rLen; ry /= rLen; rz /= rLen; }

                child.vx += rx * speed;
                child.vy += ry * speed;
                child.vz += rz * speed;

                child.life = childLife;
                child.size = childSz;
                child.r = childR;
                child.g = childG;
                child.b = childB;
                child.a = 1.0f;
                child.age = 0.0f;
                child.mass = 1.0f;
                child.id = (uint32_t)(frame * 200000 + (int)i * 1000 + j + 99991);

                data->particles.push_back(child);
            }
        }

        // Advance ALL children (existing + newly spawned this frame)
        float dt = 1.0f;
        for (size_t j = 0; j < data->particles.size(); ++j) {
            Particle& p = data->particles[j];
            p.px += p.vx * dt;
            p.py += p.vy * dt;
            p.pz += p.vz * dt;
            p.age += 1.0f;
        }

        // Remove expired children
        data->removeExpired();
    }

    // Publish the new immutable snapshot
    _cachedData = data;
    _cachedFrame = time;
    return _cachedData;
}

// ==================== RoD / Render ====================

StatusEnum
ParticleSpawn::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                     ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ParticleSpawn::render(const RenderActionArgs& args)
{
    // Fill dummy output with black to avoid NaN warnings
    std::list< std::pair<ImagePlaneDesc, ImagePtr> > outputPlanes = args.outputPlanes;
    for (auto& plane : outputPlanes) {
        if (plane.second) {
            plane.second->fillZero(args.roi);
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleSpawn.cpp"
