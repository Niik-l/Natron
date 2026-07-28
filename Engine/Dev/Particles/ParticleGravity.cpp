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

#include "ParticleParallel.h"

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

struct ParticleGravityPrivate
{
    KnobDoubleWPtr gravityX, gravityY, gravityZ;
    KnobDoubleWPtr strength;
};

ParticleGravity::ParticleGravity(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleGravityPrivate())
{
}

ParticleGravity::~ParticleGravity()
{
}

std::string
ParticleGravity::getPluginDescription() const
{
    return tr("Applies gravity force to particles.\n\n"
              "Connect to a ParticleEmitter or another particle modifier.\n"
              "Gravity modifies particle velocities each frame during simulation.").toStdString();
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

void
ParticleGravity::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    float gx = (float)_imp->gravityX.lock()->getValueAtTime(time);
    float gy = (float)_imp->gravityY.lock()->getValueAtTime(time);
    float gz = (float)_imp->gravityZ.lock()->getValueAtTime(time);
    float str = (float)_imp->strength.lock()->getValueAtTime(time);

    forEachParticleParallel(data->particles, [&](Particle& p) {
        float invMass = (p.mass > 0.001f) ? (1.0f / p.mass) : 1.0f;
        p.vx += gx * str * invMass;
        p.vy += gy * str * invMass;
        p.vz += gz * str * invMass;
    });
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleGravity.cpp"
