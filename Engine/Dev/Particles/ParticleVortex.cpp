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

#include "ParticleVortex.h"

#include <cmath>

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

struct ParticleVortexPrivate
{
    KnobDoubleWPtr axisX, axisY, axisZ;
    KnobDoubleWPtr centerX, centerY, centerZ;
    KnobDoubleWPtr strength;
    KnobDoubleWPtr inwardPull;
};

ParticleVortex::ParticleVortex(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleVortexPrivate())
{
}

ParticleVortex::~ParticleVortex()
{
}

std::string
ParticleVortex::getPluginDescription() const
{
    return tr("Creates a spiral/vortex force around an axis, spinning particles in a whirlpool pattern.\n\n"
              "Connect to a ParticleEmitter or another particle modifier.\n"
              "Positive strength spins counter-clockwise, negative spins clockwise.\n"
              "Inward Pull controls whether particles spiral inward or outward.").toStdString();
}

void
ParticleVortex::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Vortex"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Axis X"));
        k->setName("axisX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("X component of the vortex rotation axis."));
        mainPage->addKnob(k); _imp->axisX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Axis Y"));
        k->setName("axisY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Y component of the vortex rotation axis. Default is Y-up."));
        mainPage->addKnob(k); _imp->axisY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Axis Z"));
        k->setName("axisZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Z component of the vortex rotation axis."));
        mainPage->addKnob(k); _imp->axisZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center X"));
        k->setName("centerX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("X coordinate of the vortex center."));
        mainPage->addKnob(k); _imp->centerX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center Y"));
        k->setName("centerY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Y coordinate of the vortex center."));
        mainPage->addKnob(k); _imp->centerY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center Z"));
        k->setName("centerZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Z coordinate of the vortex center."));
        mainPage->addKnob(k); _imp->centerZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Strength"));
        k->setName("strength"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(-10.0); k->setDisplayMinimum(-10.0); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Spin force strength. Positive = counter-clockwise, negative = clockwise."));
        mainPage->addKnob(k); _imp->strength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Inward Pull"));
        k->setName("inwardPull"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(-5.0); k->setDisplayMinimum(-5.0); k->setDisplayMaximum(5.0);
        k->setHintToolTip(tr("Pulls particles toward the axis (positive) or pushes away (negative)."));
        mainPage->addKnob(k); _imp->inwardPull = k;
    }
}

void
ParticleVortex::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    float ax = (float)_imp->axisX.lock()->getValueAtTime(time);
    float ay = (float)_imp->axisY.lock()->getValueAtTime(time);
    float az = (float)_imp->axisZ.lock()->getValueAtTime(time);
    float ctrX = (float)_imp->centerX.lock()->getValueAtTime(time);
    float ctrY = (float)_imp->centerY.lock()->getValueAtTime(time);
    float ctrZ = (float)_imp->centerZ.lock()->getValueAtTime(time);
    float strength = (float)_imp->strength.lock()->getValueAtTime(time);
    float inwardPull = (float)_imp->inwardPull.lock()->getValueAtTime(time);

    if (strength == 0.0f && inwardPull == 0.0f) return;

    // Normalize axis
    float axLen = std::sqrt(ax * ax + ay * ay + az * az);
    if (axLen < 0.001f) return;
    ax /= axLen;
    ay /= axLen;
    az /= axLen;

    for (size_t i = 0; i < data->particles.size(); ++i) {
        Particle& p = data->particles[i];

        // Vector from center to particle
        float rx = p.px - ctrX;
        float ry = p.py - ctrY;
        float rz = p.pz - ctrZ;

        // Project onto axis plane (remove component along axis)
        float dot = rx * ax + ry * ay + rz * az;
        rx -= dot * ax;
        ry -= dot * ay;
        rz -= dot * az;

        float dist = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (dist < 0.001f) continue;

        // Tangent direction = cross(axis, r) -- this is the spin direction
        float tx = ay * rz - az * ry;
        float ty = az * rx - ax * rz;
        float tz = ax * ry - ay * rx;
        float tLen = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (tLen > 0.001f) {
            tx /= tLen;
            ty /= tLen;
            tz /= tLen;
        }

        // Apply tangential force (spin)
        p.vx += tx * strength * 0.01f;
        p.vy += ty * strength * 0.01f;
        p.vz += tz * strength * 0.01f;

        // Apply inward pull (toward axis)
        if (inwardPull != 0.0f) {
            float nrx = rx / dist;
            float nry = ry / dist;
            float nrz = rz / dist;
            p.vx -= nrx * inwardPull * 0.01f;
            p.vy -= nry * inwardPull * 0.01f;
            p.vz -= nrz * inwardPull * 0.01f;
        }
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleVortex.cpp"
