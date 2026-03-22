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

#include "ParticleGravity.h"

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "ParticleEmitter.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct ParticleGravityPrivate
{
    KnobDoubleWPtr gravityX, gravityY, gravityZ;
    KnobDoubleWPtr strength;
};

ParticleGravity::ParticleGravity(NodePtr node)
    : EffectInstance(node)
    , _imp(new ParticleGravityPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ParticleGravity::~ParticleGravity()
{
}

std::string
ParticleGravity::getPluginDescription() const
{
    return tr("Applies gravity force to particles.\n\n"
              "Connect to a ParticleEmitter or another particle node.\n"
              "Gravity modifies particle velocities each frame during simulation.").toStdString();
}

std::string
ParticleGravity::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "particles";
    return "";
}

void
ParticleGravity::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ParticleGravity::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ParticleGravity::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ParticleGravity::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Gravity"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gravity X"));
        k->setName("gravityX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-20.0); k->setDisplayMaximum(20.0);
        mainPage->addKnob(k); _imp->gravityX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gravity Y"));
        k->setName("gravityY"); k->setDefaultValue(-9.8); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-20.0); k->setDisplayMaximum(20.0);
        mainPage->addKnob(k); _imp->gravityY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gravity Z"));
        k->setName("gravityZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-20.0); k->setDisplayMaximum(20.0);
        mainPage->addKnob(k); _imp->gravityZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Strength"));
        k->setName("strength"); k->setDefaultValue(0.01); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        mainPage->addKnob(k); _imp->strength = k;
    }
}

ParticleDataPtr
ParticleGravity::getParticleData(double time)
{
    // Get particles from input
    EffectInstancePtr input = getInput(0);
    if (!input) return ParticleDataPtr();

    // Try ParticleEmitter
    ParticleEmitter* emitter = dynamic_cast<ParticleEmitter*>(input.get());
    if (emitter) {
        ParticleDataPtr data = emitter->getParticleData(time);
        if (!data) return data;

        // Apply gravity to all particles
        float gx = (float)_imp->gravityX.lock()->getValueAtTime(time);
        float gy = (float)_imp->gravityY.lock()->getValueAtTime(time);
        float gz = (float)_imp->gravityZ.lock()->getValueAtTime(time);
        float str = (float)_imp->strength.lock()->getValueAtTime(time);

        for (size_t i = 0; i < data->particles.size(); ++i) {
            Particle& p = data->particles[i];
            float invMass = (p.mass > 0.001f) ? (1.0f / p.mass) : 1.0f;
            p.vx += gx * str * invMass;
            p.vy += gy * str * invMass;
            p.vz += gz * str * invMass;
        }

        return data;
    }

    // Try another ParticleGravity in the chain
    ParticleGravity* prevGrav = dynamic_cast<ParticleGravity*>(input.get());
    if (prevGrav) {
        ParticleDataPtr data = prevGrav->getParticleData(time);
        if (!data) return data;

        float gx = (float)_imp->gravityX.lock()->getValueAtTime(time);
        float gy = (float)_imp->gravityY.lock()->getValueAtTime(time);
        float gz = (float)_imp->gravityZ.lock()->getValueAtTime(time);
        float str = (float)_imp->strength.lock()->getValueAtTime(time);

        for (size_t i = 0; i < data->particles.size(); ++i) {
            Particle& p = data->particles[i];
            float invMass = (p.mass > 0.001f) ? (1.0f / p.mass) : 1.0f;
            p.vx += gx * str * invMass;
            p.vy += gy * str * invMass;
            p.vz += gz * str * invMass;
        }

        return data;
    }

    return ParticleDataPtr();
}

StatusEnum
ParticleGravity::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                       ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ParticleGravity::render(const RenderActionArgs& args)
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

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleGravity.cpp"
