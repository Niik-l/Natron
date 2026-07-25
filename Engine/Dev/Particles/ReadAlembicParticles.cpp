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

#include "ReadAlembicParticles.h"

#include "../../NodeMetadata.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

#include "../../AppInstance.h"
#include "../../Image.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../Project.h"

#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreFactory/All.h>

NATRON_NAMESPACE_ENTER

namespace {
using namespace Alembic::AbcGeom;
using Alembic::AbcCoreFactory::IFactory;

// One pre-loaded Alembic sample. The full archive is converted to a
// vector of these on file open, so the per-frame lookup at render time
// is just an index + copy.
struct LoadedSample
{
    std::vector<V3f>      positions;
    std::vector<uint64_t> ids;
    std::vector<V3f>      velocities;
    std::vector<C4f>      colors;     // arbGeomParam "Cd" if present
    std::vector<float>    sizes;      // arbGeomParam "size" if present
};

// Locate the first IPoints anywhere in the archive (depth-first).
IPoints findFirstPoints(IObject obj)
{
    for (size_t i = 0; i < obj.getNumChildren(); ++i) {
        IObject child = obj.getChild(i);
        if (IPoints::matches(child.getMetaData())) {
            return IPoints(obj, child.getName());
        }
        IPoints recur = findFirstPoints(child);
        if (recur.valid()) return recur;
    }
    return IPoints();
}
} // anonymous namespace

struct ReadAlembicParticlesPrivate
{
    KnobFileWPtr   filePath;
    KnobButtonWPtr reloadBtn;
    KnobStringWPtr infoLabel;

    // Pre-loaded archive state. Cleared on file change. Guarded by dataMutex:
    // loadAlembicFile (GUI thread, via knobChanged) rewrites these vectors
    // while getParticleData iterates them from render/viewport threads.
    mutable std::mutex        dataMutex;
    std::vector<LoadedSample> samples;
    TimeSamplingPtr           timeSampling;   // for time -> sample index
    bool                      animated = false; // > 1 sample
    std::string               loadedPath;
};

ReadAlembicParticles::ReadAlembicParticles(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadAlembicParticlesPrivate())
{
}

ReadAlembicParticles::~ReadAlembicParticles()
{
}

std::string
ReadAlembicParticles::getPluginDescription() const
{
    return tr("Read a baked particle stream from an Alembic (.abc) OPoints file.\n\n"
              "Companion to WriteAlembicParticles. Opens the archive, finds the first "
              "IPoints object, and pre-loads all samples on file open. Per-frame data "
              "(positions, IDs, velocities, Cd color, size) is then served from RAM.\n\n"
              "Closes the bake-once round-trip: ParticleSolver → WriteAlembicParticles "
              "→ (edit in Houdini/Blender) → ReadAlembicParticles → downstream.").toStdString();
}

void
ReadAlembicParticles::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
ReadAlembicParticles::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadAlembicParticles::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ReadAlembicParticles::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Read"));

    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("File"));
        k->setName("file");
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Alembic (.abc) point-cloud file."));
        page->addKnob(k); _imp->filePath = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reload"));
        k->setName("reload");
        k->setHintToolTip(tr("Re-read the Alembic file and refresh the sample cache."));
        page->addKnob(k); _imp->reloadBtn = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info");
        k->setDefaultValue("(no file loaded)");
        k->setAsLabel();
        k->setHintToolTip(tr("Number of samples + particle count range in the loaded file."));
        page->addKnob(k); _imp->infoLabel = k;
    }
}

bool
ReadAlembicParticles::loadAlembicFile(const std::string& path)
{
    // Hold the data lock for the whole (re)load — render threads iterating
    // the old samples must not see them freed mid-walk.
    std::lock_guard<std::mutex> lk(_imp->dataMutex);

    _imp->samples.clear();
    _imp->timeSampling.reset();
    _imp->animated = false;
    _imp->loadedPath.clear();

    if (path.empty()) return false;

    try {
        IFactory factory;
        IFactory::CoreType coreType;
        IArchive archive = factory.getArchive(path, coreType);
        if (!archive.valid()) {
            std::cerr << "[ReadAlembicParticles] Could not open archive: " << path << "\n";
            return false;
        }

        IPoints points = findFirstPoints(archive.getTop());
        if (!points.valid()) {
            std::cerr << "[ReadAlembicParticles] No IPoints object found in: " << path << "\n";
            return false;
        }

        IPointsSchema& schema = points.getSchema();
        const size_t numSamples = schema.getNumSamples();
        if (numSamples == 0) {
            std::cerr << "[ReadAlembicParticles] Archive has zero samples: " << path << "\n";
            return false;
        }

        _imp->timeSampling = schema.getTimeSampling();
        _imp->animated     = (numSamples > 1);

        // Optional arbGeomParams — Cd (color) and size.
        IC4fGeomParam   colorParam;
        IFloatGeomParam sizeParam;
        ICompoundProperty arbGeoms = schema.getArbGeomParams();
        if (arbGeoms.valid()) {
            if (arbGeoms.getPropertyHeader("Cd") != nullptr
                && IC4fGeomParam::matches(*arbGeoms.getPropertyHeader("Cd"))) {
                colorParam = IC4fGeomParam(arbGeoms, "Cd");
            }
            if (arbGeoms.getPropertyHeader("size") != nullptr
                && IFloatGeomParam::matches(*arbGeoms.getPropertyHeader("size"))) {
                sizeParam = IFloatGeomParam(arbGeoms, "size");
            }
        }

        _imp->samples.resize(numSamples);
        size_t minN = std::numeric_limits<size_t>::max();
        size_t maxN = 0;

        for (size_t s = 0; s < numSamples; ++s) {
            IPointsSchema::Sample sample;
            schema.get(sample, ISampleSelector((index_t)s));
            LoadedSample& dst = _imp->samples[s];

            P3fArraySamplePtr pos = sample.getPositions();
            UInt64ArraySamplePtr id = sample.getIds();
            V3fArraySamplePtr vel = sample.getVelocities();

            const size_t n = pos ? pos->size() : 0;
            if (n > 0) {
                dst.positions.assign(pos->get(), pos->get() + n);
            } else {
                dst.positions.clear();
            }
            if (id && id->size() == n) {
                dst.ids.assign(id->get(), id->get() + n);
            } else {
                dst.ids.assign(n, 0);
            }
            if (vel && vel->size() == n) {
                dst.velocities.assign(vel->get(), vel->get() + n);
            } else {
                dst.velocities.assign(n, V3f(0, 0, 0));
            }
            // arbGeomParams (optional)
            if (colorParam.valid()) {
                IC4fGeomParam::Sample cs;
                colorParam.getExpanded(cs, ISampleSelector((index_t)s));
                C4fArraySamplePtr cvals = cs.getVals();
                if (cvals && cvals->size() == n) {
                    dst.colors.assign(cvals->get(), cvals->get() + n);
                } else {
                    dst.colors.assign(n, C4f(1, 1, 1, 1));
                }
            } else {
                dst.colors.assign(n, C4f(1, 1, 1, 1));
            }
            if (sizeParam.valid()) {
                IFloatGeomParam::Sample ss;
                sizeParam.getExpanded(ss, ISampleSelector((index_t)s));
                FloatArraySamplePtr svals = ss.getVals();
                if (svals && svals->size() == n) {
                    dst.sizes.assign(svals->get(), svals->get() + n);
                } else {
                    dst.sizes.assign(n, 0.1f);
                }
            } else {
                dst.sizes.assign(n, 0.1f);
            }

            minN = std::min(minN, n);
            maxN = std::max(maxN, n);
        }

        _imp->loadedPath = path;

        KnobStringPtr infoK = _imp->infoLabel.lock();
        if (infoK) {
            std::ostringstream os;
            os << numSamples << " samples, " << minN;
            if (minN != maxN) os << "-" << maxN;
            os << " particles";
            infoK->setValue(os.str());
        }
        std::cout << "[ReadAlembicParticles] Loaded " << numSamples << " samples ("
                  << minN << "-" << maxN << " particles) from " << path << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ReadAlembicParticles] Read failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[ReadAlembicParticles] Read failed: unknown error\n";
    }
    _imp->samples.clear();
    _imp->timeSampling.reset();
    _imp->animated = false;
    _imp->loadedPath.clear();
    return false;
}

ParticleDataPtr
ReadAlembicParticles::getParticleData(double time)
{
    // Guard against a concurrent reload rewriting the sample vectors
    std::lock_guard<std::mutex> lk(_imp->dataMutex);

    if (_imp->samples.empty()) return ParticleDataPtr();

    // Time -> Alembic seconds via project FPS. Files written by
    // WriteAlembicParticles at the same project FPS round-trip exactly.
    double fps = 24.0;
    if (getApp() && getApp()->getProject()) {
        double f = getApp()->getProject()->getProjectFrameRate();
        if (f > 0) fps = f;
    }
    const chrono_t alembicTime = (chrono_t)(time / fps);

    size_t sampleIdx = 0;
    if (_imp->timeSampling) {
        // Local name "nearest" — `near` is a legacy DOS macro on Windows
        // (defined as empty in windef.h via minwindef.h), so `near.first`
        // expands to garbage and the compile fails.
        std::pair<index_t, chrono_t> nearest =
            _imp->timeSampling->getNearIndex(alembicTime, _imp->samples.size());
        if (nearest.first >= 0 && (size_t)nearest.first < _imp->samples.size()) {
            sampleIdx = (size_t)nearest.first;
        }
    } else {
        // Fallback: floor(time) - 1 as a frame index, clamped.
        long idx = (long)std::floor(time) - 1;
        if (idx < 0) idx = 0;
        if ((size_t)idx >= _imp->samples.size()) idx = (long)_imp->samples.size() - 1;
        sampleIdx = (size_t)idx;
    }

    const LoadedSample& s = _imp->samples[sampleIdx];

    ParticleDataPtr out = std::make_shared<ParticleData>();
    out->particles.reserve(s.positions.size());
    for (size_t i = 0; i < s.positions.size(); ++i) {
        Particle p;
        p.px = s.positions[i].x;
        p.py = s.positions[i].y;
        p.pz = s.positions[i].z;
        p.prevPx = p.px;
        p.prevPy = p.py;
        p.prevPz = p.pz;
        if (i < s.velocities.size()) {
            p.vx = s.velocities[i].x;
            p.vy = s.velocities[i].y;
            p.vz = s.velocities[i].z;
        }
        if (i < s.colors.size()) {
            p.r = s.colors[i].r;
            p.g = s.colors[i].g;
            p.b = s.colors[i].b;
            p.a = s.colors[i].a;
        }
        if (i < s.sizes.size()) {
            p.size = s.sizes[i];
        }
        if (i < s.ids.size()) {
            p.id = (uint32_t)s.ids[i];
        }
        out->particles.push_back(p);
    }
    return out;
}

StatusEnum
ReadAlembicParticles::getPreferredMetadata(NodeMetadata& metadata)
{
    StatusEnum st = EffectInstance::getPreferredMetadata(metadata);
    if (st != eStatusOK && st != eStatusReplyDefault) return st;
    // Mark frame-varying when the loaded archive carries multiple samples,
    // so downstream consumers (Scene3D, ScanlineRender) don't cache one
    // frame across the whole timeline.
    metadata.setIsFrameVarying(_imp->animated);
    return eStatusOK;
}

bool
ReadAlembicParticles::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                                  ViewSpec /*view*/, double /*time*/,
                                  bool /*originatedFromMainThread*/)
{
    if (!k) return false;
    KnobIPtr fpKnob     = _imp->filePath.lock();
    KnobIPtr reloadKnob = _imp->reloadBtn.lock();

    const bool isFileChange = (fpKnob     && k == fpKnob.get());
    const bool isReload     = (reloadKnob && k == reloadKnob.get());

    if (isFileChange || isReload) {
        if (fpKnob) {
            KnobFile* fp = dynamic_cast<KnobFile*>(fpKnob.get());
            if (fp) loadAlembicFile(fp->getValue());
        }
        refreshMetadata_public(true);
        return true;
    }
    return false;
}

StatusEnum
ReadAlembicParticles::getRegionOfDefinition(U64 /*hash*/, double /*time*/,
                                            const RenderScale& /*scale*/, ViewIdx /*view*/,
                                            RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadAlembicParticles::render(const RenderActionArgs& /*args*/)
{
    // No image output — this is a particle source. Downstream consumers
    // pull via getParticleData(); render() is just a viewport hook.
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
