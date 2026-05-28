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

#include "WriteAlembicParticles.h"

#include <cmath>
#include <iostream>
#include <vector>

#include "../../AppInstance.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../Project.h"

#include "ParticleData.h"

#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>

NATRON_NAMESPACE_ENTER

namespace {
// Brevity for the Alembic write side. Scoped to this TU only.
using namespace Alembic::AbcGeom;
using Alembic::AbcCoreOgawa::WriteArchive;
}

struct WriteAlembicParticlesPrivate
{
    KnobOutputFileWPtr filename;
    KnobButtonWPtr     bake;
};

WriteAlembicParticles::WriteAlembicParticles(NodePtr node)
    : ParticleModifier(node)
    , _imp(new WriteAlembicParticlesPrivate())
{
}

WriteAlembicParticles::~WriteAlembicParticles()
{
}

std::string
WriteAlembicParticles::getPluginDescription() const
{
    return tr("Bake a particle stream to an Alembic .abc point-cloud file.\n\n"
              "Connect to the output of a ParticleSolver (or any particle stream). "
              "Set the output file, then click Bake. The node iterates the project's "
              "frame range and writes one point-cloud sample per frame containing "
              "positions, IDs, velocities, RGBA color, and per-particle size.\n\n"
              "Particles flow through the node unchanged, so downstream nodes still "
              "see the live simulation while baking. The bake is synchronous — the "
              "UI will be unresponsive while it runs.").toStdString();
}

void
WriteAlembicParticles::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Write"));

    {
        KnobOutputFilePtr k = AppManager::createKnob<KnobOutputFile>(this, tr("File"));
        k->setName("file");
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Output Alembic file (.abc). Existing file will be overwritten."));
        page->addKnob(k); _imp->filename = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Bake"));
        k->setName("bake");
        k->setHintToolTip(tr("Iterate the project's frame range and write the particle "
                             "stream to the file above. Synchronous — UI freezes until "
                             "complete. Watch the console for per-frame progress."));
        page->addKnob(k); _imp->bake = k;
    }
}

bool
WriteAlembicParticles::knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec /*view*/,
                                   double /*time*/, bool /*originatedFromMainThread*/)
{
    if (k == _imp->bake.lock().get() && reason == eValueChangedReasonUserEdited) {
        bake();
        return true;
    }
    return false;
}

void
WriteAlembicParticles::bake()
{
    // ----- Resolve inputs + parameters -----
    KnobOutputFilePtr fileKnob = _imp->filename.lock();
    std::string path = fileKnob ? fileKnob->getValue() : std::string();
    if (path.empty()) {
        std::cerr << "[WriteAlembicParticles] No output file set.\n";
        return;
    }
    EffectInstancePtr input0 = getInput(0);
    if (!input0) {
        std::cerr << "[WriteAlembicParticles] No particle input connected.\n";
        return;
    }
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input0.get());
    if (!provider) {
        std::cerr << "[WriteAlembicParticles] Input 0 is not a particle stream.\n";
        return;
    }

    // ----- Frame range + FPS from the project -----
    double firstD = 1.0, lastD = 1.0;
    getApp()->getProject()->getFrameRange(&firstD, &lastD);
    int firstFrame = (int)std::floor(firstD);
    int lastFrame  = (int)std::floor(lastD);
    if (lastFrame < firstFrame) {
        std::cerr << "[WriteAlembicParticles] Empty frame range.\n";
        return;
    }
    double fps = getApp()->getProject()->getProjectFrameRate();
    if (fps <= 0) fps = 24.0;

    std::cout << "[WriteAlembicParticles] Baking frames " << firstFrame
              << ".." << lastFrame << " at " << fps << " fps to " << path << "\n";

    try {
        // ----- Open archive + create OPoints schema -----
        OArchive archive(WriteArchive(), path);
        OObject  topObj(archive, kTop);

        // Time sampling — uniform per-frame, starting at firstFrame's time.
        // Alembic stores sample times as seconds, not frames.
        const chrono_t dt = 1.0 / fps;
        const chrono_t startTime = (chrono_t)firstFrame * dt;
        TimeSamplingPtr ts(new TimeSampling(dt, startTime));
        uint32_t tsIdx = archive.addTimeSampling(*ts);

        OPoints points(topObj, "particles", tsIdx);
        OPointsSchema& schema = points.getSchema();

        // Custom geom params: color (C4f) and size (float) live in
        // .arbGeomParams so consumers like Houdini/Blender pick them up
        // as standard varying-per-point attributes.
        OC4fGeomParam colorParam(schema.getArbGeomParams(), "Cd",
                                 false, kVaryingScope, 1, tsIdx);
        OFloatGeomParam sizeParam(schema.getArbGeomParams(), "size",
                                  false, kVaryingScope, 1, tsIdx);

        // ----- Per-frame sample loop -----
        for (int frame = firstFrame; frame <= lastFrame; ++frame) {
            ParticleDataPtr data = provider->getParticleData((double)frame);
            const size_t n = data ? data->particles.size() : 0;

            std::vector<V3f>      positions;  positions.reserve(n);
            std::vector<uint64_t> ids;        ids.reserve(n);
            std::vector<V3f>      velocities; velocities.reserve(n);
            std::vector<C4f>      colors;     colors.reserve(n);
            std::vector<float>    sizes;      sizes.reserve(n);

            if (data) {
                for (size_t i = 0; i < n; ++i) {
                    const Particle& p = data->particles[i];
                    positions.emplace_back(p.px, p.py, p.pz);
                    ids.push_back((uint64_t)p.id);
                    velocities.emplace_back(p.vx, p.vy, p.vz);
                    colors.emplace_back(p.r, p.g, p.b, p.a);
                    sizes.push_back(p.size);
                }
            }

            OPointsSchema::Sample sample;
            sample.setPositions(V3fArraySample(positions));
            sample.setIds(UInt64ArraySample(ids));
            sample.setVelocities(V3fArraySample(velocities));
            schema.set(sample);

            colorParam.set(OC4fGeomParam::Sample(C4fArraySample(colors),    kVaryingScope));
            sizeParam .set(OFloatGeomParam::Sample(FloatArraySample(sizes), kVaryingScope));

            if (frame == firstFrame || frame == lastFrame || (frame % 10) == 0) {
                std::cout << "[WriteAlembicParticles] frame " << frame
                          << " — " << n << " particles\n";
            }
        }

        std::cout << "[WriteAlembicParticles] Bake complete: " << path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[WriteAlembicParticles] Alembic write failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[WriteAlembicParticles] Alembic write failed: unknown error\n";
    }
}

NATRON_NAMESPACE_EXIT
