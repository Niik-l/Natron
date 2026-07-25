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

#include <cmath>

#include "../../AppManager.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../DotUtils.h"
#include "../Scene3D/Cube3D.h"
#include "../Scene3D/RotationConventions.h"

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
              "Particles matching the kill condition are removed at end of frame.\n\n"
              "Bounds input (optional): connect a Cube3D — its full transform "
              "(translate/rotate/scale, rotated boxes supported) defines the kill "
              "volume, visible and movable with the gizmo in the 3D viewport. "
              "Without it, the Min/Max + Center knobs define an axis-aligned box.").toStdString();
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

// Read a Cube3D bounds input into an OBB (center, half-extents, rotation
// columns = local axes in world space). Mirrors Blast's getBlastOBBInternal /
// SceneGraph::buildTRS so the kill volume matches the Cube3D's wireframe in
// the 3D viewport exactly. Also honors Cube3D's Uniform Scale and Size knobs.
static bool
killBoxOBBFromCube(EffectInstancePtr boundsInput, double time,
                   float center[3], float extent[3], float rot[3][3])
{
    Cube3D* cube = boundsInput ? dynamic_cast<Cube3D*>(boundsInput.get()) : nullptr;
    if (!cube) return false;

    auto readD = [&](const char* name, double def) -> double {
        KnobIPtr k = boundsInput->getKnobByName(name);
        KnobDouble* kd = k ? dynamic_cast<KnobDouble*>(k.get()) : nullptr;
        return kd ? kd->getValueAtTime(time) : def;
    };
    const double tx = readD("translateX", 0), ty = readD("translateY", 0), tz = readD("translateZ", 0);
    const double rx = readD("rotateX", 0),    ry = readD("rotateY", 0),    rz = readD("rotateZ", 0);
    const double us = readD("uniformScale", 1.0);
    const double sz3 = readD("size", 1.0);
    // Cube3D's mesh spans ±(size * 0.5) in local space (generateCubeMesh),
    // scaled by scaleXYZ * uniformScale in the world transform — so the OBB
    // half-extent must carry the 0.5 too, or the volume is 2x the wireframe.
    const double sx = readD("scaleX", 1) * us * sz3 * 0.5;
    const double sy = readD("scaleY", 1) * us * sz3 * 0.5;
    const double szz = readD("scaleZ", 1) * us * sz3 * 0.5;

    center[0] = (float)tx; center[1] = (float)ty; center[2] = (float)tz;
    extent[0] = (float)std::abs(sx);
    extent[1] = (float)std::abs(sy);
    extent[2] = (float)std::abs(szz);

    double mRot[3][3];
    RotationConventions::compose(rx, ry, rz, mRot);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rot[i][j] = (float)mRot[i][j];
    return true;
}

static bool
killBoxPointInOBB(float px, float py, float pz,
                  const float center[3], const float extent[3], const float rot[3][3])
{
    const float vx = px - center[0];
    const float vy = py - center[1];
    const float vz = pz - center[2];
    // Local coords via R^T * v (dot with each column of R)
    const float lx = rot[0][0] * vx + rot[1][0] * vy + rot[2][0] * vz;
    const float ly = rot[0][1] * vx + rot[1][1] * vy + rot[2][1] * vz;
    const float lz = rot[0][2] * vx + rot[1][2] * vy + rot[2][2] * vz;
    return std::abs(lx) <= extent[0] &&
           std::abs(ly) <= extent[1] &&
           std::abs(lz) <= extent[2];
}

void
ParticleKillBox::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    // Cube3D bounds input (input 1): the cube's full transform defines the
    // kill volume — rotated boxes work, and the volume is visible/movable in
    // the 3D viewport. Falls back to the Min/Max + Center knobs otherwise.
    float obbCenter[3], obbExtent[3], obbRot[3][3];
    if (killBoxOBBFromCube(skipDots(getInput(1)), time, obbCenter, obbExtent, obbRot)) {
        int modeVal = _imp->mode.lock() ? _imp->mode.lock()->getValueAtTime(time) : 0;
        const bool killInside = (modeVal == 0);
        for (size_t i = 0; i < data->particles.size(); ++i) {
            Particle& p = data->particles[i];
            const bool inside = killBoxPointInOBB(p.px, p.py, p.pz,
                                                  obbCenter, obbExtent, obbRot);
            if ((killInside && inside) || (!killInside && !inside)) {
                p.life = p.age; // Mark for removal by removeExpired()
            }
        }
        return;
    }

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
