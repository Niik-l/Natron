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

#include "ParticleAttract.h"

#include <cmath>

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

struct ParticleAttractPrivate
{
    KnobDoubleWPtr pointX, pointY, pointZ;
    KnobDoubleWPtr strength;
    KnobChoiceWPtr falloff;
    KnobDoubleWPtr maxDistance;
};

ParticleAttract::ParticleAttract(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleAttractPrivate())
{
}

ParticleAttract::~ParticleAttract()
{
}

std::string
ParticleAttract::getPluginDescription() const
{
    return tr("Attracts or repels particles toward/from a point in 3D space.\n\n"
              "Connect to a ParticleEmitter or another particle modifier.\n"
              "Positive strength attracts, negative strength repels.\n"
              "Supports no falloff, linear falloff, or inverse-square falloff.").toStdString();
}

void
ParticleAttract::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Attract"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Point X"));
        k->setName("pointX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("X coordinate of the attraction point."));
        mainPage->addKnob(k); _imp->pointX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Point Y"));
        k->setName("pointY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Y coordinate of the attraction point."));
        mainPage->addKnob(k); _imp->pointY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Point Z"));
        k->setName("pointZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Z coordinate of the attraction point."));
        mainPage->addKnob(k); _imp->pointZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Strength"));
        k->setName("strength"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(-10.0); k->setDisplayMinimum(-10.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Force strength. Positive = attract, negative = repel."));
        mainPage->addKnob(k); _imp->strength = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Falloff"));
        k->setName("falloff");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("None", "", "No falloff — constant force regardless of distance"));
        entries.push_back(ChoiceOption("Linear", "", "Force decreases linearly with distance"));
        entries.push_back(ChoiceOption("InverseSquare", "", "Force decreases with the square of distance"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("How the force diminishes with distance from the attraction point."));
        mainPage->addKnob(k); _imp->falloff = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Max Distance"));
        k->setName("maxDistance"); k->setDefaultValue(10.0); k->setAnimationEnabled(true);
        k->setMinimum(0.1); k->setDisplayMinimum(0.1); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Particles beyond this distance from the point are unaffected."));
        mainPage->addKnob(k); _imp->maxDistance = k;
    }
}

void
ParticleAttract::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    float ptX = (float)_imp->pointX.lock()->getValueAtTime(time);
    float ptY = (float)_imp->pointY.lock()->getValueAtTime(time);
    float ptZ = (float)_imp->pointZ.lock()->getValueAtTime(time);
    float strength = (float)_imp->strength.lock()->getValueAtTime(time);
    int falloffType = _imp->falloff.lock()->getValue();
    float maxDist = (float)_imp->maxDistance.lock()->getValueAtTime(time);

    if (strength == 0.0f) return;

    for (size_t i = 0; i < data->particles.size(); ++i) {
        Particle& p = data->particles[i];

        float dx = ptX - p.px;
        float dy = ptY - p.py;
        float dz = ptZ - p.pz;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (dist > maxDist) continue;
        if (dist < 0.001f) continue; // avoid divide by zero

        // Normalize direction
        dx /= dist;
        dy /= dist;
        dz /= dist;

        // Apply falloff
        float force = strength;
        if (falloffType == 1) {
            force *= (1.0f - dist / maxDist); // Linear
        } else if (falloffType == 2) {
            force *= 1.0f / (dist * dist); // Inverse square
        }

        p.vx += dx * force * 0.01f;
        p.vy += dy * force * 0.01f;
        p.vz += dz * force * 0.01f;
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleAttract.cpp"
