/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "PointsToParticles.h"

#include <algorithm>
#include <cassert>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"
#include "../Deep/PointCloudData.h"
#include "../Deep/PointCloudProvider.h"
#include "../DotUtils.h"

NATRON_NAMESPACE_ENTER

struct PointsToParticlesPrivate
{
    KnobDoubleWPtr size;
    KnobStringWPtr info;
};

PointsToParticles::PointsToParticles(NodePtr node)
    : EffectInstance(node)
    , _imp(new PointsToParticlesPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

PointsToParticles::~PointsToParticles()
{
}

std::string
PointsToParticles::getPluginDescription() const
{
    return tr("Convert a point cloud (DeepToPoints, Blast, CameraTracker, "
              "PointCloudGenerator) into static particles.\n\n"
              "This unlocks the particle ecosystem for point clouds: "
              "ScanlineRender's particle modes (point/disc/sphere/sprite, "
              "additive/over), CyclesRender's native point rendering, and "
              "ParticleInstance (instance geometry onto the points).\n\n"
              "Classic use — 2.5D re-projection: DeepRecolor -> DeepToPoints "
              "(with a camera for world-space unprojection) -> PointsToParticles "
              "-> Scene3D -> ScanlineRender with a different camera.").toStdString();
}

void
PointsToParticles::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
PointsToParticles::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
PointsToParticles::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
PointsToParticles::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobDoublePtr size = AppManager::createKnob<KnobDouble>(this, tr("Particle Size"));
    size->setName("particleSize");
    size->setHintToolTip(tr("Size assigned to every particle (points carry no size of "
                            "their own). World units, like ParticleEmitter's Start Size."));
    size->setAnimationEnabled(true);
    size->setDefaultValue(0.05);
    size->setMinimum(0.0001);
    size->setDisplayMinimum(0.001);
    size->setDisplayMaximum(1.0);
    page->addKnob(size);
    _imp->size = size;

    KnobStringPtr info = AppManager::createKnob<KnobString>(this, tr("Info"));
    info->setName("info");
    info->setAnimationEnabled(false);
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    info->setDefaultValue("Connect a point-cloud node (DeepToPoints, Blast, ...).");
    page->addKnob(info);
    _imp->info = info;
}

void
PointsToParticles::onInputChanged(int inputNo)
{
    if (inputNo == 0) {
        std::lock_guard<std::mutex> lk(_computeMutex);
        _cachedData.reset();
        _cachedCloudKey = nullptr;
        if (getApp()) {
            getApp()->redrawAllViewers();
        }
    }
    EffectInstance::onInputChanged(inputNo);
}

StatusEnum
PointsToParticles::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                          ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProjectFormat = false;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProjectFormat);
}

ParticleDataPtr
PointsToParticles::getParticleData(double time)
{
    // Serialise against concurrent callers (viewport paint vs render workers)
    // and publish immutable snapshots — the P0 provider contract.
    std::lock_guard<std::mutex> lk(_computeMutex);

    EffectInstancePtr input = skipDots(getInput(0));
    if (!input) {
        _cachedData.reset();
        _cachedCloudKey = nullptr;
        return ParticleDataPtr();
    }
    PointCloudProvider* provider = dynamic_cast<PointCloudProvider*>(input.get());
    if (!provider) {
        _cachedData.reset();
        _cachedCloudKey = nullptr;
        return ParticleDataPtr();
    }

    PointCloudDataPtr cloud = provider->getPointCloud();
    if (!cloud || cloud->numPoints() == 0) {
        _cachedData.reset();
        _cachedCloudKey = nullptr;
        return ParticleDataPtr();
    }

    const double sizeVal = _imp->size.lock() ? _imp->size.lock()->getValue() : 0.05;

    // Cache on cloud identity + size: providers publish immutable snapshots,
    // so an unchanged pointer means unchanged points.
    if (_cachedData && _cachedCloudKey == (const void*)cloud.get()
        && _cachedSize == sizeVal) {
        return _cachedData;
    }

    ParticleDataPtr data = std::make_shared<ParticleData>();
    const std::size_t n = cloud->numPoints();
    const float* pts = cloud->data();
    const int stride = 6; // x,y,z,r,g,b
    data->particles.reserve(n);
    const bool hasIds = cloud->hasIds();
    for (std::size_t i = 0; i < n; ++i) {
        Particle p;
        p.px = pts[i * stride + 0];
        p.py = pts[i * stride + 1];
        p.pz = pts[i * stride + 2];
        p.prevPx = p.px;
        p.prevPy = p.py;
        p.prevPz = p.pz;
        p.vx = p.vy = p.vz = 0.0f;
        p.r = pts[i * stride + 3];
        p.g = pts[i * stride + 4];
        p.b = pts[i * stride + 5];
        p.a = 1.0f;
        p.size = (float)sizeVal;
        p.age = 0.0f;
        p.life = 1e9f;  // static — never expires
        p.mass = 1.0f;
        p.id = hasIds ? (uint32_t)cloud->idAt(i) : (uint32_t)i;
        data->particles.push_back(p);
    }

    _cachedData = data;
    _cachedCloudKey = (const void*)cloud.get();
    _cachedSize = sizeVal;

    KnobStringPtr infoKnob = _imp->info.lock();
    if (infoKnob) {
        infoKnob->setValue(std::to_string(n) + " points -> particles");
    }

    (void)time; // static conversion — time-independent
    return _cachedData;
}

StatusEnum
PointsToParticles::render(const RenderActionArgs& args)
{
    // Dummy 2D output (like ParticleInstance) — consumers use getParticleData.
    if (!args.outputPlanes.empty()) {
        ImagePtr outImg = args.outputPlanes.front().second;
        if (outImg) outImg->fillZero(args.roi);
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_PointsToParticles.cpp"
