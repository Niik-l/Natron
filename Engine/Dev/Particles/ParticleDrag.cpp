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

#include "ParticleDrag.h"

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

struct ParticleDragPrivate
{
    KnobDoubleWPtr drag;
};

ParticleDrag::ParticleDrag(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleDragPrivate())
{
}

ParticleDrag::~ParticleDrag()
{
}

std::string
ParticleDrag::getPluginDescription() const
{
    return tr("Applies drag (velocity damping) to particles.\n\n"
              "Connect to a ParticleEmitter or another particle modifier.\n"
              "Drag reduces particle velocities each frame by a damping factor.").toStdString();
}

void
ParticleDrag::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Drag"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Drag"));
        k->setName("drag"); k->setDefaultValue(0.02); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Velocity damping per frame. 0 = no drag, 1 = full stop."));
        mainPage->addKnob(k); _imp->drag = k;
    }
}

void
ParticleDrag::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    float dragVal = (float)_imp->drag.lock()->getValueAtTime(time);
    if (dragVal <= 0.0f) return;

    float damping = 1.0f - dragVal;

    for (size_t i = 0; i < data->particles.size(); ++i) {
        Particle& p = data->particles[i];
        p.vx *= damping;
        p.vy *= damping;
        p.vz *= damping;
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleDrag.cpp"
