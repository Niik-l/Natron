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

#include "ZDefocus.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

namespace {

// Phase 1 ScanlineRender writes depth as ImagePlaneDesc("depth", "depth", "Z", {Z}).
ImagePlaneDesc depthPlaneDesc()
{
    std::vector<std::string> channels;
    channels.push_back("Z");
    return ImagePlaneDesc("depth", "depth", "Z", channels);
}

enum MathModeEnum
{
    eMathModeDepth = 0,   // Z = camera-space distance (Nuke / RenderMan default)
    eMathModeDirect = 1,  // Z value directly multiplies size (Z=0.5 → blur=size*0.5)
};

enum FilterTypeEnum
{
    eFilterTypeDisc = 0,
    eFilterTypeBladed = 1,
};

// Returns 1.0 if (dx, dy) is inside an iris polygon of `radius` with `blades` sides,
// rotated by `rotationRad`, with corner roundness `roundness` in [0,1].
// 0.0 outside. For disc: blades is ignored and the test is a pure radial cut.
inline float irisMask(float dx, float dy, float radius,
                      int blades, float roundness, float rotationRad,
                      bool isDisc, float aspect)
{
    // Apply anamorphic aspect (squeeze x or y).
    float ax = (aspect >= 1.0f) ? dx / aspect : dx;
    float ay = (aspect <  1.0f) ? dy * aspect : dy;

    const float r2 = ax * ax + ay * ay;
    const float r  = std::sqrt(r2);
    if (r > radius) return 0.0f;

    if (isDisc) return 1.0f;

    // Bladed: distance to nearest blade edge.
    // The iris is a regular polygon with N=blades sides circumscribed in `radius`.
    // For each point, find its angular position relative to nearest blade edge,
    // then compute distance to that edge.
    if (blades < 3) return 1.0f;
    float angle = std::atan2(ay, ax) + rotationRad;
    const float bladeAngle = 2.0f * (float)M_PI / (float)blades;
    // Wrap angle to nearest blade sector.
    float sector = angle - std::floor(angle / bladeAngle + 0.5f) * bladeAngle;
    // Distance from center to polygon edge along this direction.
    // For a regular polygon, edge distance = radius * cos(bladeAngle/2) / cos(sector).
    const float halfBlade = bladeAngle * 0.5f;
    const float polyEdge = radius * std::cos(halfBlade) / std::max(1e-4f, std::cos(sector));
    // Mix between disc radius (roundness=1) and polygon edge (roundness=0).
    const float edge = radius * roundness + polyEdge * (1.0f - roundness);
    return r <= edge ? 1.0f : 0.0f;
}

} // anonymous namespace

struct ZDefocusPrivate
{
    KnobChoiceWPtr depthPlane;    // user-selectable dropdown for which input plane carries depth
    std::vector<ImagePlaneDesc> cachedLayers;  // planes discovered from input 0; refreshed on input/metadata change
    KnobChoiceWPtr math;
    KnobDoubleWPtr center;
    KnobDoubleWPtr dof;
    KnobBoolWPtr   blurDof;
    KnobDoubleWPtr size;
    KnobDoubleWPtr maxSize;
    KnobChoiceWPtr filterType;
    KnobDoubleWPtr aspect;
    KnobIntWPtr    blades;
    KnobDoubleWPtr roundness;
    KnobDoubleWPtr rotation;
    KnobDoubleWPtr mix;
};

// Resolves the user-selected plane to an ImagePlaneDesc. Falls back to the
// hardcoded depth plane if cachedLayers don't include the selection.
static ImagePlaneDesc resolveDepthPlane(const KnobChoiceWPtr& depthPlaneKnob,
                                         const std::vector<ImagePlaneDesc>& cached)
{
    KnobChoicePtr k = depthPlaneKnob.lock();
    if (!k) return depthPlaneDesc();
    const std::string id = k->getActiveEntry().id;
    for (size_t i = 0; i < cached.size(); ++i) {
        if (cached[i].getPlaneID() == id) return cached[i];
    }
    return depthPlaneDesc();
}

ZDefocus::ZDefocus(NodePtr node)
    : EffectInstance(node)
    , _imp(new ZDefocusPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

ZDefocus::~ZDefocus()
{
}

std::string
ZDefocus::getPluginDescription() const
{
    return "Depth-of-field / defocus blur driven by a depth pass.\n\n"
           "Consumes the depth.Z AOV produced by ScanlineRender (Phase 1) and applies a "
           "per-pixel variable-radius blur whose radius (circle of confusion) is computed "
           "from depth, focal plane, and aperture controls.\n\n"
           "v1 uses single-pass per-pixel disc/iris gather. Modeled on Nuke's ZDefocus but "
           "simplified — see RECIPE_ZDEFOCUS.md for the full Nuke knob set we plan to add "
           "(layer-stack algorithm, filter image, bloom, catadioptric etc. are deferred to v2).\n\n"
           "Wire ScanlineRender (with depth.Z AOV on) directly to the Source input.";
}

void
ZDefocus::addAcceptedComponents(int /*inputNb*/,
                                 std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( depthPlaneDesc() );
}

void
ZDefocus::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ZDefocus::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ZDefocus::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                          EffectInstance::ComponentsNeededMap* comps,
                                          double* passThroughTime,
                                          int* passThroughView,
                                          int* passThroughInput)
{
    (*comps)[0].push_back( ImagePlaneDesc::getRGBAComponents() );
    (*comps)[0].push_back( resolveDepthPlane(_imp->depthPlane, _imp->cachedLayers) );
    (*comps)[-1].push_back( ImagePlaneDesc::getRGBAComponents() );

    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0;
}

void
ZDefocus::onInputChanged(int inputNo)
{
    if (inputNo == 0) refreshLayerChoices();
}

void
ZDefocus::onMetadataRefreshed(const NodeMetadata& /*metadata*/)
{
    refreshLayerChoices();
}

void
ZDefocus::refreshLayerChoices()
{
    _imp->cachedLayers.clear();

    std::list<ImagePlaneDesc> availableLayers;
    EffectInstancePtr input = getInput(0);
    if (input) {
        double time = 0;
        if (getApp() && getApp()->getTimeLine()) {
            time = getApp()->getTimeLine()->currentFrame();
        }
        input->getAvailableLayers(time, ViewIdx(0), -1, &availableLayers);
    }

    std::vector<ChoiceOption> entries;
    entries.push_back(ChoiceOption("depth",
                                   "(auto) depth",
                                   "Use the depth plane (Phase 1 AOV). Default — works out of the box for ScanlineRender."));

    for (std::list<ImagePlaneDesc>::const_iterator it = availableLayers.begin();
         it != availableLayers.end(); ++it) {
        const ImagePlaneDesc& layer = *it;
        // Skip Color/Alpha — they're never depth data.
        const std::string id = layer.getPlaneID();
        if (id == "Color" || id == "Alpha") continue;

        std::string label = layer.getPlaneLabel();
        std::string chans;
        const std::vector<std::string>& channels = layer.getChannels();
        for (size_t c = 0; c < channels.size(); ++c) chans += channels[c];
        std::string displayName = (label.empty() ? id : label) + "." + chans;

        entries.push_back(ChoiceOption(id, displayName, "Use plane: " + displayName));
        _imp->cachedLayers.push_back(layer);
    }

    KnobChoicePtr k = _imp->depthPlane.lock();
    if (k) k->populateChoices(entries);
}

void
ZDefocus::initializeKnobs()
{
    KnobPagePtr focusPage = AppManager::createKnob<KnobPage>(this, tr("Focus"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Depth Plane"));
        k->setName("depthPlane");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("depth",
                                       "(auto) depth",
                                       "Use the depth plane (Phase 1 AOV). Default — works out of the box for ScanlineRender."));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip(tr("Which input plane to read depth from. "
                              "Refreshes when the input is wired or upstream metadata updates. "
                              "Default 'depth' matches the ScanlineRender Phase 1 AOV. "
                              "Pick any other single-channel plane for renderers using different names "
                              "(e.g. 'Z', 'mantra.depth', 'world_position' if you want to defocus by distance)."));
        focusPage->addKnob(k); _imp->depthPlane = k;
    }

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Math"));
        k->setName("math");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("depth",  "Depth",  "Z = camera-space distance. Default — matches Nuke/RenderMan/our ScanlineRender depth.Z."));
        opts.push_back(ChoiceOption("direct", "Direct", "Z value directly multiplies Size — useful with hand-authored gradient maps."));
        k->populateChoices(opts);
        k->setDefaultValue(eMathModeDepth);
        k->setHintToolTip(tr("How to interpret the depth value."));
        focusPage->addKnob(k); _imp->math = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Focal Plane"));
        k->setName("center"); k->setDefaultValue(5.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Camera-space distance to the in-focus plane."));
        focusPage->addKnob(k); _imp->center = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Depth of Field"));
        k->setName("dof"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.001); k->setDisplayMinimum(0.001); k->setDisplayMaximum(20.0);
        k->setHintToolTip(tr("Depth slice around the focal plane that stays sharp. "
                              "Smaller = shallower DoF (cinema look)."));
        focusPage->addKnob(k); _imp->dof = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Blur Inside DoF"));
        k->setName("blurDof"); k->setDefaultValue(true);
        k->setHintToolTip(tr("Apply a tiny blur to in-focus regions for smoother transition "
                              "to the defocused area. Recommended ON."));
        focusPage->addKnob(k); _imp->blurDof = k;
    }

    KnobPagePtr blurPage = AppManager::createKnob<KnobPage>(this, tr("Blur"));

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"));
        k->setName("size"); k->setDefaultValue(15.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(50.0);
        k->setHintToolTip(tr("Blur radius gain (pixels). Larger = more dramatic out-of-focus blur."));
        blurPage->addKnob(k); _imp->size = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Max Size"));
        k->setName("maxSize"); k->setDefaultValue(25.0); k->setAnimationEnabled(true);
        k->setMinimum(1.0); k->setDisplayMinimum(1.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Hard cap on blur radius. Defends against runaway perf when depth values blow up. "
                              "v1 algorithm is O(radius²) per pixel; keep < 50 unless you have time."));
        blurPage->addKnob(k); _imp->maxSize = k;
    }

    KnobPagePtr bokehPage = AppManager::createKnob<KnobPage>(this, tr("Bokeh"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Filter Type"));
        k->setName("filterType");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("disc",   "Disc",   "Smooth circular bokeh — fastest."));
        opts.push_back(ChoiceOption("bladed", "Bladed", "Polygonal iris bokeh (5-blade default → pentagonal)."));
        k->populateChoices(opts);
        k->setDefaultValue(eFilterTypeDisc);
        k->setHintToolTip(tr("Bokeh kernel shape."));
        bokehPage->addKnob(k); _imp->filterType = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Aspect Ratio"));
        k->setName("aspect"); k->setDefaultValue(1.0);
        k->setMinimum(0.1); k->setDisplayMinimum(0.1); k->setDisplayMaximum(3.0);
        k->setHintToolTip(tr("Anamorphic squeeze. <1 squeezes X, >1 squeezes Y."));
        bokehPage->addKnob(k); _imp->aspect = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Blades"));
        k->setName("blades"); k->setDefaultValue(5);
        k->setMinimum(3); k->setDisplayMinimum(3); k->setDisplayMaximum(12);
        k->setHintToolTip(tr("Iris blade count (only used when Filter Type = Bladed). "
                              "3 = triangle, 5 = pentagon, 8 = octagon, etc."));
        bokehPage->addKnob(k); _imp->blades = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Roundness"));
        k->setName("roundness"); k->setDefaultValue(0.2);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Polygon corner rounding (Bladed only). 0 = sharp polygon, 1 = circular."));
        bokehPage->addKnob(k); _imp->roundness = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotation"));
        k->setName("rotation"); k->setDefaultValue(0.0);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        k->setHintToolTip(tr("Iris rotation in degrees (Bladed only). Positive = counter-clockwise."));
        bokehPage->addKnob(k); _imp->rotation = k;
    }

    KnobPagePtr outPage = AppManager::createKnob<KnobPage>(this, tr("Output"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Blend between original (0) and full defocus (1)."));
        outPage->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
ZDefocus::getRegionOfDefinition(U64 /*hash*/,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    StatusEnum st = input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
    if (st != eStatusOK && st != eStatusReplyDefault) return st;

    // Expand by maxSize so out-of-focus highlights at the bbox edge don't get clipped.
    KnobDoublePtr maxK = _imp->maxSize.lock();
    if (maxK) {
        double maxRadius = maxK->getValueAtTime(time);
        const OfxPointD sp = scale.toOfxPointD();
        double padX = maxRadius / std::max(1e-6, sp.x);
        double padY = maxRadius / std::max(1e-6, sp.y);
        rod->x1 -= padX; rod->x2 += padX;
        rod->y1 -= padY; rod->y2 += padY;
    }
    return eStatusOK;
}

StatusEnum
ZDefocus::render(const RenderActionArgs& args)
{
    // Source (Color) plane.
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;

    // User-selected depth plane (resolves to hardcoded "depth" if absent).
    RectI depthRoi;
    ImagePlaneDesc dDesc = resolveDepthPlane(_imp->depthPlane, _imp->cachedLayers);
    ImagePtr depthImg = getImage(0, args.time, args.mappedScale, args.view,
                                 NULL, &dDesc, false, false,
                                 eStorageModeRAM, 0, &depthRoi);
    const bool haveDepth = (depthImg != nullptr);

    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const int   mathMode   = _imp->math.lock()->getValue();
    const float center     = (float)_imp->center.lock()->getValueAtTime(args.time);
    const float dof        = (float)std::max(0.001, _imp->dof.lock()->getValueAtTime(args.time));
    const bool  blurDof    = _imp->blurDof.lock()->getValueAtTime(args.time) != 0;
    const float size       = (float)_imp->size.lock()->getValueAtTime(args.time);
    const float maxSize    = (float)_imp->maxSize.lock()->getValueAtTime(args.time);
    const int   filterIdx  = _imp->filterType.lock()->getValue();
    const float aspect     = (float)_imp->aspect.lock()->getValueAtTime(args.time);
    const int   blades     = std::max(3, _imp->blades.lock()->getValue());
    const float roundness  = (float)_imp->roundness.lock()->getValueAtTime(args.time);
    const float rotationRad = (float)(_imp->rotation.lock()->getValueAtTime(args.time) * M_PI / 180.0);
    const float mixVal     = (float)_imp->mix.lock()->getValueAtTime(args.time);
    const float invMix     = 1.0f - mixVal;
    const bool  isDisc     = (filterIdx == eFilterTypeDisc);

    RectI srcBounds   = srcImg->getBounds();
    RectI outBounds   = outImg->getBounds();
    RectI depthBounds = haveDepth ? depthImg->getBounds() : RectI();

    int srcNComp = srcImg->getComponents().getNumComponents();
    int outNComp = std::min(srcNComp, 4);

    Image::ReadAccess  srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());
    std::unique_ptr<Image::ReadAccess> depthRa;
    if (haveDepth) depthRa.reset(new Image::ReadAccess(depthImg.get()));

    // Passthrough if no depth.
    if (!haveDepth) {
        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const float* src = (x >= srcBounds.x1 && x < srcBounds.x2 &&
                                    y >= srcBounds.y1 && y < srcBounds.y2)
                                 ? (const float*)srcRa.pixelAt(x, y) : nullptr;
                if (!src) { for (int c = 0; c < outNComp; ++c) dst[c] = 0.0f; continue; }
                for (int c = 0; c < outNComp; ++c) dst[c] = src[c];
            }
        }
        return eStatusOK;
    }

    // Per-pixel variable-radius gather. For each output pixel:
    //   1. Read depth z; compute CoC radius from |z - center| - dof, scaled by size,
    //      clamped to maxSize (Direct mode: radius = z * size, clamped).
    //   2. If radius < 0.5 (in focus): copy source pixel.
    //   3. Else: walk a square (2r+1)^2 neighbourhood, accumulate samples weighted by
    //      iris mask. Output = lerp(src, acc/wsum, mix).
    #ifdef _OPENMP
    #pragma omp parallel for
    #endif
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            const float* srcP = (x >= srcBounds.x1 && x < srcBounds.x2 &&
                                 y >= srcBounds.y1 && y < srcBounds.y2)
                              ? (const float*)srcRa.pixelAt(x, y) : nullptr;
            if (!srcP) { for (int c = 0; c < outNComp; ++c) dst[c] = 0.0f; continue; }

            // Depth at this pixel.
            float z = 0.0f;
            if (x >= depthBounds.x1 && x < depthBounds.x2 &&
                y >= depthBounds.y1 && y < depthBounds.y2) {
                const float* dp = (const float*)depthRa->pixelAt(x, y);
                if (dp) z = dp[0];
            }

            // CoC computation.
            float radius;
            if (mathMode == eMathModeDirect) {
                radius = std::abs(z) * size;
            } else {
                // depth mode: distance from focal plane, falling outside the DoF slice.
                const float d = std::abs(z - center);
                if (d <= dof) {
                    radius = blurDof ? 0.5f : 0.0f;
                } else {
                    radius = (d - dof) * size / dof;
                }
            }
            radius = std::min(radius, maxSize);
            if (radius < 0.5f) {
                for (int c = 0; c < outNComp; ++c) dst[c] = srcP[c];
                continue;
            }

            // Square neighbourhood scan with iris-mask cutout.
            const int ir = (int)std::ceil(radius);
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float wSum = 0.0f;

            for (int sy = -ir; sy <= ir; ++sy) {
                const int yy = y + sy;
                if (yy < srcBounds.y1 || yy >= srcBounds.y2) continue;
                for (int sx = -ir; sx <= ir; ++sx) {
                    const int xx = x + sx;
                    if (xx < srcBounds.x1 || xx >= srcBounds.x2) continue;

                    const float w = irisMask((float)sx, (float)sy, radius,
                                             blades, roundness, rotationRad,
                                             isDisc, aspect);
                    if (w <= 0.0f) continue;

                    const float* sp = (const float*)srcRa.pixelAt(xx, yy);
                    if (!sp) continue;
                    for (int c = 0; c < outNComp; ++c) acc[c] += sp[c] * w;
                    wSum += w;
                }
            }

            const float denom = wSum > 0.0f ? wSum : 1.0f;
            for (int c = 0; c < outNComp; ++c) {
                const float blurred = acc[c] / denom;
                dst[c] = mixVal < 1.0f ? blurred * mixVal + srcP[c] * invMix : blurred;
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
