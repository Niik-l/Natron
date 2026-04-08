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

#include "ParticleWind.h"

#include <cmath>

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

namespace {

// Deterministic hash for per-particle gustiness
inline float hashFloat(unsigned int seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed = seed + (seed << 3u);
    seed = seed ^ (seed >> 4u);
    seed = seed * 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return (float)(seed & 0x7FFFFFFFu) / (float)0x7FFFFFFFu;
}

} // anonymous namespace

struct ParticleWindPrivate
{
    KnobDoubleWPtr dirX;
    KnobDoubleWPtr dirY;
    KnobDoubleWPtr dirZ;
    KnobDoubleWPtr strength;
    KnobDoubleWPtr gustiness;
};

ParticleWind::ParticleWind(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleWindPrivate())
{
}

ParticleWind::~ParticleWind()
{
}

std::string
ParticleWind::getPluginDescription() const
{
    return tr("Applies directional wind force to particles.\n\n"
              "Connect to a ParticleEmitter or another particle modifier.\n"
              "Pushes particles along a specified direction with optional gustiness.").toStdString();
}

void
ParticleWind::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Wind"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Direction X"));
        k->setName("directionX"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(-10.0); k->setDisplayMinimum(-10.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("X component of the wind direction."));
        mainPage->addKnob(k); _imp->dirX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Direction Y"));
        k->setName("directionY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(-10.0); k->setDisplayMinimum(-10.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Y component of the wind direction."));
        mainPage->addKnob(k); _imp->dirY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Direction Z"));
        k->setName("directionZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(-10.0); k->setDisplayMinimum(-10.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Z component of the wind direction."));
        mainPage->addKnob(k); _imp->dirZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Strength"));
        k->setName("strength"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Overall strength of the wind force."));
        mainPage->addKnob(k); _imp->strength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Gustiness"));
        k->setName("gustiness"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Random variation in wind strength per particle. 0 = uniform, 1 = highly variable."));
        mainPage->addKnob(k); _imp->gustiness = k;
    }
}

void
ParticleWind::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    float dx = (float)_imp->dirX.lock()->getValueAtTime(time);
    float dy = (float)_imp->dirY.lock()->getValueAtTime(time);
    float dz = (float)_imp->dirZ.lock()->getValueAtTime(time);
    float strengthVal = (float)_imp->strength.lock()->getValueAtTime(time);
    float gustiness = (float)_imp->gustiness.lock()->getValueAtTime(time);

    if (strengthVal <= 0.0f) return;

    // Normalize direction
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) return;
    float invLen = 1.0f / len;
    dx *= invLen;
    dy *= invLen;
    dz *= invLen;

    unsigned int frame = (unsigned int)(int)time;

    for (size_t i = 0; i < data->particles.size(); ++i) {
        Particle& p = data->particles[i];

        float gust = 1.0f;
        if (gustiness > 0.0f) {
            gust = 1.0f + (hashFloat((unsigned int)i + frame * 7919u) - 0.5f) * gustiness * 2.0f;
        }

        p.vx += dx * strengthVal * gust * 0.01f;
        p.vy += dy * strengthVal * gust * 0.01f;
        p.vz += dz * strengthVal * gust * 0.01f;
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleWind.cpp"
