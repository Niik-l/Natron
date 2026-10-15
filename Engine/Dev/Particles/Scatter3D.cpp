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

#include "Scatter3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <random>
#include <unordered_map>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"
#include "../DotUtils.h"
#include "../Scene3D/GeoExtract.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

enum ScatterNormalEnum
{
    eScatterNormalSurface = 0,
    eScatterNormalSurfaceFlipped = 1,
    eScatterNormalUp = 2,
};

struct Scatter3DPrivate
{
    KnobIntWPtr    count;
    KnobIntWPtr    seed;
    KnobDoubleWPtr size;          // 2 dims: min, max
    KnobDoubleWPtr slopeLimit;
    KnobDoubleWPtr minDistance;
    KnobChoiceWPtr normalMode;
    KnobBoolWPtr   invertDensity;
    KnobDoubleWPtr densityGamma;
    KnobDoubleWPtr displaySize;   // viewport-only point scale (read by Viewport3D by knob name)
    KnobStringWPtr info;
};

Scatter3D::Scatter3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Scatter3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Scatter3D::~Scatter3D()
{
}

std::string
Scatter3D::getPluginDescription() const
{
    return tr("Scatter points over the surface of a geo node, for instancing.\n\n"
              "Input surface: any geo the 3D system renders (ReadGeo, GeoBuilder, ReadAlembicArchive, "
              "Card3D, Sphere3D...). Input density: an optional mask looked up through the surface's "
              "UVs; white = full density, black = none, so a painted map decides where the debris or "
              "the foliage goes.\n\n"
              "The points are area-weighted (as many per square metre on a big triangle as on a small "
              "one), carry the surface normal, and come out as static particles. Feed them to "
              "ParticleInstance with Megascans assets on the geo inputs: Align to Normal stands them "
              "on the slope, Random Rotation and Random Scale break up the repetition.\n\n"
              "Slope Limit drops points on faces steeper than the angle (no grass on cliffs), Min "
              "Distance keeps points apart (fewer than Count may fit), Seed reshuffles.").toStdString();
}

void
Scatter3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
Scatter3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Scatter3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Scatter3D::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Count"));
        k->setName("count");
        k->setDefaultValue(1000);
        k->setMinimum(0); k->setMaximum(5000000);
        k->setDisplayMinimum(0); k->setDisplayMaximum(100000);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("How many points to place. Slope Limit, Min Distance and the density mask "
                             "reject candidates, so fewer may come out; the Info line shows the result."));
        page->addKnob(k); _imp->count = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Seed"));
        k->setName("seed");
        k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(1000);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Reshuffles the point positions."));
        page->addKnob(k); _imp->seed = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"), 2);
        k->setName("size");
        k->setDimensionName(0, "min"); k->setDimensionName(1, "max");
        for (int d = 0; d < 2; ++d) {
            k->setDefaultValue(1.0, d);
            k->setMinimum(0.0, d); k->setMaximum(10000.0, d);
            k->setDisplayMinimum(0.0, d); k->setDisplayMaximum(5.0, d);
        }
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Particle size, picked uniformly between min and max per point. ParticleInstance "
                             "multiplies the prototype by it (1 = the prototype as modelled)."));
        page->addKnob(k); _imp->size = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Slope Limit"));
        k->setName("slopeLimit");
        k->setDefaultValue(90.0);
        k->setMinimum(0.0); k->setMaximum(180.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(90.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Steepest face, in degrees from horizontal, that still receives points. "
                             "90 = everything up to vertical walls, 30 = gentle slopes only, 180 = also faces pointing down."));
        page->addKnob(k); _imp->slopeLimit = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Min Distance"));
        k->setName("minDistance");
        k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(10000.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(5.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Minimum spacing between points in world units (0 = off). Candidates closer "
                             "than this to an accepted point are dropped, which gives an even, "
                             "non-clumping spread at the cost of reaching Count."));
        page->addKnob(k); _imp->minDistance = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Normal"));
        k->setName("normalMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("surface", "Surface", "The face normal from the winding, so instances stand on the slope."));
        opts.push_back(ChoiceOption("flipped", "Surface (Flipped)", "The face normal reversed, for meshes wound inside-out."));
        opts.push_back(ChoiceOption("up", "World Up", "Always +Y, so instances stay upright whatever the slope."));
        k->populateChoices(opts);
        k->setDefaultValue(eScatterNormalSurface);
        k->setAnimationEnabled(false);
        k->setHintToolTip(tr("The normal each point carries; ParticleInstance's Align to Normal uses it."));
        page->addKnob(k); _imp->normalMode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Density Gamma"));
        k->setName("densityGamma");
        k->setDefaultValue(1.0);
        k->setMinimum(0.05); k->setMaximum(20.0);
        k->setDisplayMinimum(0.2); k->setDisplayMaximum(5.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Contrast of the density mask: above 1 thins the grey areas, below 1 fills them."));
        page->addKnob(k); _imp->densityGamma = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert Density"));
        k->setName("invertDensity");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Scatter where the density mask is black instead of white."));
        page->addKnob(k); _imp->invertDensity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Display Size"));
        k->setName("displaySize");
        k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1000.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);   // viewport only: no re-scatter, no render invalidation
        k->setHintToolTip(tr("Multiplier on the point size drawn in the 3D viewport only. "
                             "Does not affect renders or the instances made by ParticleInstance. "
                             "Turn it down when small Size values make the preview points look huge."));
        page->addKnob(k); _imp->displaySize = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info");
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setIsPersistent(false);
        k->setDefaultValue("Connect a geo to 'surface'.");
        page->addKnob(k); _imp->info = k;
    }
}

void
Scatter3D::onInputChanged(int inputNo)
{
    {
        std::lock_guard<std::mutex> lk(_computeMutex);
        _cachedData.reset();
        _cachedHash = 0;
    }
    if (getApp()) {
        getApp()->redrawAllViewers();
    }
    EffectInstance::onInputChanged(inputNo);
}

bool
Scatter3D::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                       double /*time*/, bool /*originatedFromMainThread*/)
{
    // Display Size is viewport-only (no evaluate-on-change), so repaint by hand.
    if (k && k == _imp->displaySize.lock().get()) {
        if (getApp()) getApp()->redrawAllViewers();
        return true;
    }
    return false;
}

StatusEnum
Scatter3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                 ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

ParticleDataPtr
Scatter3D::getParticleData(double time)
{
    // One snapshot per (node hash, time); the hash covers our knobs and the
    // upstream chain, so a reshuffled seed or a moved surface recomputes.
    std::lock_guard<std::mutex> lk(_computeMutex);
    const U64 h = getHash();
    if (_cachedData && _cachedHash == h && _cachedTime == time) {
        return _cachedData;
    }
    _cachedData = compute(time);
    _cachedHash = h;
    _cachedTime = time;
    return _cachedData;
}

ParticleDataPtr
Scatter3D::compute(double time)
{
    KnobStringPtr infoKnob = _imp->info.lock();
    auto setInfo = [&](const std::string& s) {
        if (infoKnob) infoKnob->setValue(s);
    };

    EffectInstancePtr surface = skipDots(getInput(0));
    if (!surface) {
        setInfo("Connect a geo to 'surface'.");
        return ParticleDataPtr();
    }
    TriMesh mesh;
    if (!extractTriangleMesh(surface, time, mesh)) {
        setInfo("'surface' is not a geo this node understands (ReadGeo, GeoBuilder, archive, primitive).");
        return ParticleDataPtr();
    }

    const int    count    = std::max(0, _imp->count.lock()->getValueAtTime(time));
    const uint32_t seed   = (uint32_t)std::max(0, _imp->seed.lock()->getValue());
    double sizeMin = std::max(0.0, _imp->size.lock()->getValueAtTime(time, 0));
    double sizeMax = std::max(0.0, _imp->size.lock()->getValueAtTime(time, 1));
    if (sizeMax < sizeMin) std::swap(sizeMin, sizeMax);
    const double slopeLimit = _imp->slopeLimit.lock()->getValueAtTime(time);
    const double minDist    = std::max(0.0, _imp->minDistance.lock()->getValueAtTime(time));
    const int    normalMode = _imp->normalMode.lock()->getValue();
    const bool   invertDens = _imp->invertDensity.lock()->getValue();
    const double densGamma  = std::max(0.05, _imp->densityGamma.lock()->getValueAtTime(time));
    const float  cosLimit   = (float)std::cos(std::min(180.0, std::max(0.0, slopeLimit)) * M_PI / 180.0);

    ParticleDataPtr data = std::make_shared<ParticleData>();
    if (count == 0) {
        setInfo("0 points");
        return data;
    }

    // World-space triangles (the node's own transform applied), with areas.
    const size_t nt = mesh.numTris();
    std::vector<float> wv(mesh.verts.size());
    for (size_t v = 0; v < mesh.numVerts(); ++v) {
        geoMat4MulPoint(mesh.localMatrix, &mesh.verts[v * 3], &wv[v * 3]);
    }
    std::vector<double> cdf(nt, 0.0);
    std::vector<float>  faceN(nt * 3, 0.f);
    double totalArea = 0.0;
    for (size_t t = 0; t < nt; ++t) {
        const float* a = &wv[(size_t)mesh.tris[t * 3] * 3];
        const float* b = &wv[(size_t)mesh.tris[t * 3 + 1] * 3];
        const float* c = &wv[(size_t)mesh.tris[t * 3 + 2] * 3];
        const double ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
        const double fx = c[0] - a[0], fy = c[1] - a[1], fz = c[2] - a[2];
        const double nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        totalArea += 0.5 * len;
        cdf[t] = totalArea;
        if (len > 1e-20) {
            faceN[t * 3 + 0] = (float)(nx / len);
            faceN[t * 3 + 1] = (float)(ny / len);
            faceN[t * 3 + 2] = (float)(nz / len);
        } else {
            faceN[t * 3 + 1] = 1.f;
        }
    }
    if (totalArea <= 0.0) {
        setInfo("surface has no area");
        return data;
    }

    // Density mask, read through the surface UVs (nearest pixel; Natron images
    // are bottom-up like UV v).
    ImagePtr maskImg;
    RectI maskBounds;
    int maskNComp = 0;
    if (getInput(1) && mesh.hasUVs()) {
        RectI roi;
        maskImg = getImage(1, time, RenderScale(), ViewIdx(0), NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        if (maskImg) {
            maskBounds = maskImg->getBounds();
            maskNComp = maskImg->getComponents().getNumComponents();
            if (maskBounds.width() <= 0 || maskBounds.height() <= 0 || maskNComp <= 0) {
                maskImg.reset();
            }
        }
    }
    std::unique_ptr<Image::ReadAccess> maskRa;
    if (maskImg) {
        maskRa.reset(new Image::ReadAccess(maskImg.get()));
    }
    auto densityAt = [&](float u, float v) -> float {
        if (!maskRa) return 1.f;
        u -= std::floor(u); v -= std::floor(v); // tile like a repeating texture
        const int x = maskBounds.x1 + std::min(maskBounds.width() - 1, std::max(0, (int)(u * maskBounds.width())));
        const int y = maskBounds.y1 + std::min(maskBounds.height() - 1, std::max(0, (int)(v * maskBounds.height())));
        const float* pix = (const float*)maskRa->pixelAt(x, y);
        if (!pix) return 0.f;
        float d = pix[maskNComp - 1]; // mask inputs are delivered as 1-channel alpha; RGBA falls back to alpha too
        if (maskNComp == 3) d = 0.2126f * pix[0] + 0.7152f * pix[1] + 0.0722f * pix[2];
        d = std::max(0.f, std::min(1.f, d));
        if (invertDens) d = 1.f - d;
        if (densGamma != 1.0) d = (float)std::pow((double)d, densGamma);
        return d;
    };

    // Min-distance rejection on a hash grid of cell = minDist.
    struct CellKey { long long x, y, z; bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; } };
    struct CellHash { size_t operator()(const CellKey& k) const { return (size_t)(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL); } };
    std::unordered_map<CellKey, std::vector<int>, CellHash> grid;
    const double cell = minDist > 0.0 ? minDist : 1.0;
    auto cellOf = [&](const float p[3]) -> CellKey {
        CellKey k; k.x = (long long)std::floor(p[0] / cell); k.y = (long long)std::floor(p[1] / cell); k.z = (long long)std::floor(p[2] / cell);
        return k;
    };
    auto tooClose = [&](const float p[3]) -> bool {
        if (minDist <= 0.0) return false;
        const CellKey c = cellOf(p);
        const double d2min = minDist * minDist;
        for (long long dz = -1; dz <= 1; ++dz) for (long long dy = -1; dy <= 1; ++dy) for (long long dx = -1; dx <= 1; ++dx) {
            CellKey k; k.x = c.x + dx; k.y = c.y + dy; k.z = c.z + dz;
            auto it = grid.find(k);
            if (it == grid.end()) continue;
            for (size_t i = 0; i < it->second.size(); ++i) {
                const Particle& q = data->particles[(size_t)it->second[i]];
                const double ddx = q.px - p[0], ddy = q.py - p[1], ddz = q.pz - p[2];
                if (ddx * ddx + ddy * ddy + ddz * ddz < d2min) return true;
            }
        }
        return false;
    };

    std::mt19937 rng(seed * 2654435761u + 12345u);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const long long maxAttempts = (long long)count * 30 + 1000;
    long long attempts = 0;
    long long rejSlope = 0, rejDensity = 0, rejDist = 0;
    data->particles.reserve((size_t)count);

    while ((int)data->particles.size() < count && attempts < maxAttempts) {
        ++attempts;
        // Triangle by area, then a uniform barycentric sample.
        const double r = uni(rng) * totalArea;
        size_t t = (size_t)(std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
        if (t >= nt) t = nt - 1;
        double bu = uni(rng), bv = uni(rng);
        if (bu + bv > 1.0) { bu = 1.0 - bu; bv = 1.0 - bv; }
        const double bw = 1.0 - bu - bv;
        const int ia = mesh.tris[t * 3], ib = mesh.tris[t * 3 + 1], ic = mesh.tris[t * 3 + 2];
        const float* a = &wv[(size_t)ia * 3];
        const float* b = &wv[(size_t)ib * 3];
        const float* c = &wv[(size_t)ic * 3];
        float p[3] = {
            (float)(bw * a[0] + bu * b[0] + bv * c[0]),
            (float)(bw * a[1] + bu * b[1] + bv * c[1]),
            (float)(bw * a[2] + bu * b[2] + bv * c[2]) };
        float n[3] = { faceN[t * 3], faceN[t * 3 + 1], faceN[t * 3 + 2] };
        if (normalMode == eScatterNormalSurfaceFlipped) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }

        // Slope: angle between the (possibly flipped) normal and world up.
        // Small tolerance so an exactly vertical face (n.y == 0) still passes the
        // default 90 degree limit instead of losing to cos(90) == 6e-17.
        if (slopeLimit < 180.0 && n[1] < cosLimit - 1e-4f) { ++rejSlope; continue; }
        if (normalMode == eScatterNormalUp) { n[0] = 0.f; n[1] = 1.f; n[2] = 0.f; }

        // Density through the UVs
        if (maskRa) {
            const float* ua = &mesh.uvs[(t * 3) * 2];
            const float* ub = &mesh.uvs[(t * 3 + 1) * 2];
            const float* uc = &mesh.uvs[(t * 3 + 2) * 2];
            const float u = (float)(bw * ua[0] + bu * ub[0] + bv * uc[0]);
            const float v = (float)(bw * ua[1] + bu * ub[1] + bv * uc[1]);
            const float d = densityAt(u, v);
            if (d <= 0.f || uni(rng) > d) { ++rejDensity; continue; }
        }

        if (tooClose(p)) { ++rejDist; continue; }

        Particle q;
        q.px = p[0]; q.py = p[1]; q.pz = p[2];
        q.prevPx = q.px; q.prevPy = q.py; q.prevPz = q.pz;
        q.vx = q.vy = q.vz = 0.f;
        q.nx = n[0]; q.ny = n[1]; q.nz = n[2];
        q.r = q.g = q.b = q.a = 1.f;
        q.size = (float)(sizeMin + (sizeMax - sizeMin) * uni(rng));
        q.age = 0.f;
        q.life = 1e9f; // static, never expires
        q.mass = 1.f;
        q.id = (uint32_t)data->particles.size();
        if (minDist > 0.0) {
            grid[cellOf(p)].push_back((int)data->particles.size());
        }
        data->particles.push_back(q);
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%d points on %d tris (%.3g m2)%s", (int)data->particles.size(), (int)nt, totalArea,
                 maskRa ? ", density mask on" : (getInput(1) ? ", density ignored: surface has no UVs" : ""));
        std::string s = buf;
        if ((int)data->particles.size() < count) {
            snprintf(buf, sizeof(buf), " - short of Count: slope %lld, density %lld, distance %lld rejected", rejSlope, rejDensity, rejDist);
            s += buf;
        }
        setInfo(s);
    }
    return data;
}

StatusEnum
Scatter3D::render(const RenderActionArgs& args)
{
    // Dummy 2D output (like ParticleInstance): consumers use getParticleData.
    if (!args.outputPlanes.empty()) {
        ImagePtr outImg = args.outputPlanes.front().second;
        if (outImg) outImg->fillZero(args.roi);
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Scatter3D.cpp"
