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

#include "ParticleKillBox.h"

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

NATRON_NAMESPACE_ENTER

struct ParticleKillBoxPrivate
{
    KnobDoubleWPtr minX;
    KnobDoubleWPtr minY;
    KnobDoubleWPtr minZ;
    KnobDoubleWPtr maxX;
    KnobDoubleWPtr maxY;
    KnobDoubleWPtr maxZ;
    KnobChoiceWPtr mode;
    KnobDoubleWPtr centerX;
    KnobDoubleWPtr centerY;
    KnobDoubleWPtr centerZ;
};

ParticleKillBox::ParticleKillBox(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleKillBoxPrivate())
{
}

ParticleKillBox::~ParticleKillBox()
{
}

std::string
ParticleKillBox::getPluginDescription() const
{
    return tr("Kills particles that enter or leave a bounding box region.\n\n"
              "Connect to a ParticleEmitter or another particle modifier.\n"
              "Particles matching the kill condition are removed at end of frame.").toStdString();
}

void
ParticleKillBox::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Kill Box"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Min X"));
        k->setName("minX"); k->setDefaultValue(-5.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Minimum X bound of the kill box."));
        mainPage->addKnob(k); _imp->minX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Min Y"));
        k->setName("minY"); k->setDefaultValue(-5.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Minimum Y bound of the kill box."));
        mainPage->addKnob(k); _imp->minY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Min Z"));
        k->setName("minZ"); k->setDefaultValue(-5.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Minimum Z bound of the kill box."));
        mainPage->addKnob(k); _imp->minZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Max X"));
        k->setName("maxX"); k->setDefaultValue(5.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Maximum X bound of the kill box."));
        mainPage->addKnob(k); _imp->maxX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Max Y"));
        k->setName("maxY"); k->setDefaultValue(5.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Maximum Y bound of the kill box."));
        mainPage->addKnob(k); _imp->maxY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Max Z"));
        k->setName("maxZ"); k->setDefaultValue(5.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Maximum Z bound of the kill box."));
        mainPage->addKnob(k); _imp->maxZ = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
        k->setName("mode"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Kill Inside", "", "Particles inside the box are killed"));
        entries.push_back(ChoiceOption("Kill Outside", "", "Particles outside the box are killed"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Kill Inside: particles inside the box are killed.\n"
                             "Kill Outside: particles outside the box are killed."));
        mainPage->addKnob(k); _imp->mode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center X"));
        k->setName("centerX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("X offset of the box center."));
        mainPage->addKnob(k); _imp->centerX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center Y"));
        k->setName("centerY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Y offset of the box center."));
        mainPage->addKnob(k); _imp->centerY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Center Z"));
        k->setName("centerZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Z offset of the box center."));
        mainPage->addKnob(k); _imp->centerZ = k;
    }
}

void
ParticleKillBox::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    // Guarded .lock() reads — this runs on render threads; a null knob must
    // not deref (matches the ternary pattern used by the other force nodes).
    auto dval = [&](const KnobDoubleWPtr& w, double def) -> float {
        KnobDoublePtr k = w.lock();
        return k ? (float)k->getValueAtTime(time) : (float)def;
    };
    float bMinX = dval(_imp->minX, -1.0);
    float bMinY = dval(_imp->minY, -1.0);
    float bMinZ = dval(_imp->minZ, -1.0);
    float bMaxX = dval(_imp->maxX, 1.0);
    float bMaxY = dval(_imp->maxY, 1.0);
    float bMaxZ = dval(_imp->maxZ, 1.0);
    int modeVal = _imp->mode.lock() ? _imp->mode.lock()->getValueAtTime(time) : 0;
    float cx = dval(_imp->centerX, 0.0);
    float cy = dval(_imp->centerY, 0.0);
    float cz = dval(_imp->centerZ, 0.0);

    // Offset bounds by center
    bMinX += cx; bMaxX += cx;
    bMinY += cy; bMaxY += cy;
    bMinZ += cz; bMaxZ += cz;

    bool killInside = (modeVal == 0);

    for (size_t i = 0; i < data->particles.size(); ++i) {
        Particle& p = data->particles[i];

        bool inside = (p.px >= bMinX && p.px <= bMaxX &&
                       p.py >= bMinY && p.py <= bMaxY &&
                       p.pz >= bMinZ && p.pz <= bMaxZ);

        if ((killInside && inside) || (!killInside && !inside)) {
            p.life = p.age; // Mark for removal by removeExpired()
        }
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ParticleKillBox.cpp"
