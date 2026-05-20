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

#include "ParticleInstance.h"

#include <cmath>

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
    KnobBoolWPtr orientToVelocity;
    KnobDoubleWPtr scaleMultiplier;
    KnobIntWPtr maxInstances;
};

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
              "Input 0: particles (from ParticleSolver or emitter chain)\n"
              "Input 1-4: geometry sources A-D (Cube3D, Sphere3D, etc.)\n\n"
              "Each particle gets assigned a geometry source based on the distribution mode. "
              "The renderer reads the instance list and draws each geo at the particle's "
              "position, rotation, and scale.\n\n"
              "Particle color tints the instance. Particle size controls instance scale.").toStdString();
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
        k->setHintToolTip(tr("Rotate instances to face their velocity direction."));
        page->addKnob(k); _imp->orientToVelocity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Multiplier"));
        k->setName("scaleMultiplier"); k->setDefaultValue(1.0);
        k->setMinimum(0.001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Global scale multiplier on all instances."));
        page->addKnob(k); _imp->scaleMultiplier = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Instances"));
        k->setName("maxInstances"); k->setDefaultValue(10000);
        k->setMinimum(1); k->setDisplayMinimum(100); k->setDisplayMaximum(50000);
        k->setHintToolTip(tr("Maximum number of instances to render. Limits performance impact."));
        page->addKnob(k); _imp->maxInstances = k;
    }
}

ParticleDataPtr
ParticleInstance::getParticleData(double time)
{
    // Pass through particle data for downstream nodes
    EffectInstancePtr input = getInput(0);
    if (!input) return ParticleDataPtr();
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input.get());
    if (!provider) return ParticleDataPtr();
    return provider->getParticleData(time);
}

void
ParticleInstance::getInstances(double time, std::vector<GeoInstance>& outInstances)
{
    outInstances.clear();

    // Get particle data
    EffectInstancePtr input0 = getInput(0);
    if (!input0) return;
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input0.get());
    if (!provider) return;
    ParticleDataPtr pdata = provider->getParticleData(time);
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

    int distMode = _imp->distribution.lock() ? _imp->distribution.lock()->getValue() : 1;
    bool orientVel = _imp->orientToVelocity.lock() ? _imp->orientToVelocity.lock()->getValue() : false;
    float scaleMult = _imp->scaleMultiplier.lock() ? (float)_imp->scaleMultiplier.lock()->getValueAtTime(time) : 1.0f;
    int maxInst = _imp->maxInstances.lock() ? _imp->maxInstances.lock()->getValue() : 10000;

    int numGeos = (int)connectedGeos.size();
    int count = std::min(pdata->numParticles(), maxInst);

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
            // Random (deterministic by particle ID)
            geoIdx = (int)(p.id * 2654435761u) % numGeos; // Knuth hash
            if (geoIdx < 0) geoIdx += numGeos;
        } else {
            // By ID
            geoIdx = (int)(p.id % (uint32_t)numGeos);
        }
        inst.geoSourceIndex = connectedGeos[geoIdx];

        // Position
        inst.px = p.px;
        inst.py = p.py;
        inst.pz = p.pz;

        // Rotation from velocity (extrinsic XYZ composition: M = Rz*Ry*Rx col-vec).
        // Align prototype's +Z forward axis with the velocity direction (dx, dy, dz).
        // Solving M * (0,0,1) = (dx, dy, dz) with rz=0:
        //   dx =  sin(ry) * cos(rx)
        //   dy = -sin(rx)
        //   dz =  cos(ry) * cos(rx)
        //   -> rx = -asin(dy)
        //   -> ry = atan2(dx, dz)
        if (orientVel) {
            float speed = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
            if (speed > 0.001f) {
                float dx = p.vx / speed, dy = p.vy / speed, dz = p.vz / speed;
                inst.ry = std::atan2(dx, dz) * 180.0f / (float)M_PI;
                inst.rx = -std::asin(std::max(-1.0f, std::min(1.0f, dy))) * 180.0f / (float)M_PI;
                inst.rz = 0;
            } else {
                inst.rx = inst.ry = inst.rz = 0;
            }
        } else {
            inst.rx = inst.ry = inst.rz = 0;
        }

        // Scale from particle size
        float s = p.size * scaleMult;
        inst.sx = s;
        inst.sy = s;
        inst.sz = s;

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
