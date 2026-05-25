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

#include "PMask.h"

#include <algorithm>
#include <cmath>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

namespace {

enum ModeEnum
{
    eModeSphere = 0,
    eModeAabb = 1,
};

enum OutputModeEnum
{
    eOutputModeMatte = 0,   // output = matte in all channels (alpha + grayscale RGB)
    eOutputModePremult = 1, // output = source * matte
    eOutputModeOverlay = 2, // output = source tinted with overlay color where matte > 0
};

// Smoothstep helper (Hermite interpolation). Returns 0 at edge0, 1 at edge1.
inline float smoothstep01(float edge0, float edge1, float x)
{
    if (edge1 <= edge0) return x >= edge0 ? 1.0f : 0.0f;
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

} // anonymous namespace

struct PMaskPrivate
{
    KnobChoiceWPtr mode;
    KnobColorWPtr  center;        // 3-dim RGB knob — gives us Natron's built-in viewport eyedropper.
                                   // User opens the picker, clicks on a Position-pass pixel, and
                                   // its RGB (which IS the XYZ world position) lands in Centre.
    KnobDoubleWPtr radius;        // sphere only
    KnobDoubleWPtr size;          // 3-dim: X, Y, Z half-extents (AABB)
    KnobDoubleWPtr falloff;       // soft edge thickness (world units)
    KnobBoolWPtr   invert;
    KnobChoiceWPtr outputMode;
    KnobColorWPtr  overlayColor;  // 3-dim RGB for overlay mode tint
    KnobDoubleWPtr overlayMix;    // strength of tint when in overlay mode
    KnobDoubleWPtr mix;           // final blend with source
};

PMask::PMask(NodePtr node)
    : EffectInstance(node)
    , _imp(new PMaskPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

PMask::~PMask()
{
}

std::string
PMask::getPluginDescription() const
{
    return "3D position-based matte. Isolates pixels whose XYZ position falls inside a sphere "
           "or axis-aligned bounding box in world space.\n\n"
           "Modeled on Nuke's classic P_matte expression-node pattern.\n\n"
           "Input 0 (Source): the beauty image to be matted.\n"
           "Input 1 (Position): a 3-channel image where R, G, B hold X, Y, Z position values. "
           "Wire via a Shuffle from world_position.xyz, Pref.xyz, or any custom XYZ field.\n\n"
           "Output modes:\n"
           "  Matte    — outputs the matte value in all channels (alpha + grayscale RGB)\n"
           "  Premult  — multiplies the source by the matte\n"
           "  Overlay  — tints the source with overlayColor where matte > 0 (preview)";
}

void
PMask::addAcceptedComponents(int /*inputNb*/,
                               std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
PMask::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
PMask::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
PMask::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
        k->setName("mode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("sphere", "Sphere",
                                    "Matte is 1 inside a sphere of given radius around Centre."));
        opts.push_back(ChoiceOption("aabb",   "AABB",
                                    "Matte is 1 inside an axis-aligned box of given half-Size around Centre."));
        k->populateChoices(opts);
        k->setDefaultValue(eModeSphere);
        k->setHintToolTip(tr("Region shape used for the matte."));
        page->addKnob(k); _imp->mode = k;
    }

    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Centre"), 3);
        k->setName("center");
        k->setDefaultValue(0.0, 0);
        k->setDefaultValue(0.0, 1);
        k->setDefaultValue(0.0, 2);
        k->setDimensionName(0, "x");
        k->setDimensionName(1, "y");
        k->setDimensionName(2, "z");
        // World positions are not bounded to [0,1] — allow large + negative ranges.
        k->setDisplayMinimum(-50.0, 0); k->setDisplayMaximum(50.0, 0);
        k->setDisplayMinimum(-50.0, 1); k->setDisplayMaximum(50.0, 1);
        k->setDisplayMinimum(-50.0, 2); k->setDisplayMaximum(50.0, 2);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("World-space XYZ centre of the matte region. "
                              "Use the colour-picker eyedropper: switch the viewer to your Position "
                              "pass, click the picker, then click on the pixel you want to centre on. "
                              "The picked RGB IS the XYZ world position."));
        page->addKnob(k); _imp->center = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Radius"));
        k->setName("radius"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(20.0);
        k->setHintToolTip(tr("Sphere mode: matte is 1 inside a sphere of this radius around Centre, "
                              "fading to 0 over Falloff. World units."));
        page->addKnob(k); _imp->radius = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Size"), 3);
        k->setName("size");
        k->setDefaultValue(1.0, 0);
        k->setDefaultValue(1.0, 1);
        k->setDefaultValue(1.0, 2);
        k->setDimensionName(0, "x");
        k->setDimensionName(1, "y");
        k->setDimensionName(2, "z");
        k->setMinimum(0.0, 0); k->setMinimum(0.0, 1); k->setMinimum(0.0, 2);
        k->setDisplayMinimum(0.0, 0); k->setDisplayMaximum(20.0, 0);
        k->setDisplayMinimum(0.0, 1); k->setDisplayMaximum(20.0, 1);
        k->setDisplayMinimum(0.0, 2); k->setDisplayMaximum(20.0, 2);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("AABB mode: half-extents of the bounding box along each axis. "
                              "Matte is 1 inside the box, fades to 0 over Falloff. World units."));
        page->addKnob(k); _imp->size = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Falloff"));
        k->setName("falloff"); k->setDefaultValue(0.2); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(5.0);
        k->setHintToolTip(tr("Soft-edge thickness in world units. Smoothstep falloff from full matte "
                              "inside the region to 0 at (Radius/Size + Falloff). Set to 0 for hard edge."));
        page->addKnob(k); _imp->falloff = k;
    }

    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert"));
        k->setName("invert"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Invert the matte (1 outside the region, 0 inside)."));
        page->addKnob(k); _imp->invert = k;
    }

    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr(""));
        page->addKnob(sep);
    }

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output"));
        k->setName("outputMode");
        std::vector<ChoiceOption> opts;
        opts.push_back(ChoiceOption("matte",   "Matte",
                                    "Output the matte value in all channels (alpha + grayscale RGB). For piping into downstream nodes."));
        opts.push_back(ChoiceOption("premult", "Premult",
                                    "Multiply the source by the matte (premultiplied)."));
        opts.push_back(ChoiceOption("overlay", "Overlay",
                                    "Tint the source with Overlay Colour where matte > 0. For visually placing the matte."));
        k->populateChoices(opts);
        k->setDefaultValue(eOutputModeOverlay);
        k->setHintToolTip(tr("How to use the computed matte. Overlay is the typical 'pick the region' "
                              "workflow; switch to Matte or Premult when you're ready to use it downstream."));
        page->addKnob(k); _imp->outputMode = k;
    }

    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Overlay Colour"), 3);
        k->setName("overlayColor");
        k->setDefaultValue(0.0, 0);
        k->setDefaultValue(1.0, 1);
        k->setDefaultValue(0.0, 2);
        k->setHintToolTip(tr("Tint colour used in Overlay output mode. Default is bright green for "
                              "obvious visual contrast against typical scene colours."));
        page->addKnob(k); _imp->overlayColor = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Overlay Strength"));
        k->setName("overlayMix"); k->setDefaultValue(0.7);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("How strongly the Overlay Colour tints the source in Overlay mode. "
                              "1.0 = full colour replace inside matte region, 0.0 = source unchanged."));
        page->addKnob(k); _imp->overlayMix = k;
    }

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Mix"));
        k->setName("mix"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Blend between source (0) and the chosen Output (1)."));
        page->addKnob(k); _imp->mix = k;
    }
}

StatusEnum
PMask::getRegionOfDefinition(U64 /*hash*/,
                               double time,
                               const RenderScale& scale,
                               ViewIdx view,
                               RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
}

StatusEnum
PMask::render(const RenderActionArgs& args)
{
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &srcRoi);
    if (!srcImg) return eStatusFailed;

    // Position input is optional (so the node can pass through if not wired yet).
    RectI posRoi;
    ImagePtr posImg = getInput(1)
                    ? getImage(1, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false,
                               eStorageModeRAM, 0, &posRoi)
                    : ImagePtr();
    const bool havePos = (posImg != nullptr);

    if (args.outputPlanes.empty()) return eStatusFailed;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    const int   modeIdx    = _imp->mode.lock()->getValue();
    const float cx         = (float)_imp->center.lock()->getValueAtTime(args.time, 0);
    const float cy         = (float)_imp->center.lock()->getValueAtTime(args.time, 1);
    const float cz         = (float)_imp->center.lock()->getValueAtTime(args.time, 2);
    const float radius     = (float)_imp->radius.lock()->getValueAtTime(args.time);
    const float sx         = (float)_imp->size.lock()->getValueAtTime(args.time, 0);
    const float sy         = (float)_imp->size.lock()->getValueAtTime(args.time, 1);
    const float sz         = (float)_imp->size.lock()->getValueAtTime(args.time, 2);
    const float falloff    = (float)std::max(0.0, _imp->falloff.lock()->getValueAtTime(args.time));
    const bool  invert     = _imp->invert.lock()->getValueAtTime(args.time) != 0;
    const int   outMode    = _imp->outputMode.lock()->getValue();
    const float ovR        = (float)_imp->overlayColor.lock()->getValueAtTime(args.time, 0);
    const float ovG        = (float)_imp->overlayColor.lock()->getValueAtTime(args.time, 1);
    const float ovB        = (float)_imp->overlayColor.lock()->getValueAtTime(args.time, 2);
    const float ovMix      = (float)_imp->overlayMix.lock()->getValueAtTime(args.time);
    const float mixVal     = (float)_imp->mix.lock()->getValueAtTime(args.time);
    const float invMix     = 1.0f - mixVal;

    RectI srcBounds = srcImg->getBounds();
    RectI outBounds = outImg->getBounds();
    RectI posBounds = havePos ? posImg->getBounds() : RectI();

    int srcNComp = srcImg->getComponents().getNumComponents();
    int outNComp = std::min(srcNComp, 4);
    int posNComp = havePos ? posImg->getComponents().getNumComponents() : 0;

    Image::ReadAccess  srcRa(srcImg.get());
    Image::WriteAccess wa(outImg.get());
    std::unique_ptr<Image::ReadAccess> posRa;
    if (havePos) posRa.reset(new Image::ReadAccess(posImg.get()));

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

            // Read XYZ position (or 0 if Position input not wired / out of bounds).
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            bool posValid = false;
            if (havePos && x >= posBounds.x1 && x < posBounds.x2 &&
                           y >= posBounds.y1 && y < posBounds.y2) {
                const float* pp = (const float*)posRa->pixelAt(x, y);
                if (pp && posNComp >= 3) {
                    px = pp[0];
                    py = pp[1];
                    pz = pp[2];
                    posValid = true;
                }
            }

            // Compute matte value m in [0, 1].
            float m;
            if (!posValid) {
                m = 0.0f;  // No position data → no matte
            } else if (modeIdx == eModeSphere) {
                const float dx = px - cx;
                const float dy = py - cy;
                const float dz = pz - cz;
                const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                // 1 when dist <= radius; 0 when dist >= radius + falloff; smoothstep between.
                if (falloff <= 0.0f) {
                    m = dist <= radius ? 1.0f : 0.0f;
                } else {
                    m = 1.0f - smoothstep01(radius, radius + falloff, dist);
                }
            } else {
                // AABB: per-axis matte then product. Each axis: 1 inside [c - s, c + s],
                // fading to 0 by (s + falloff) on each side.
                const float dx = std::fabs(px - cx);
                const float dy = std::fabs(py - cy);
                const float dz = std::fabs(pz - cz);
                float mx, my, mz;
                if (falloff <= 0.0f) {
                    mx = dx <= sx ? 1.0f : 0.0f;
                    my = dy <= sy ? 1.0f : 0.0f;
                    mz = dz <= sz ? 1.0f : 0.0f;
                } else {
                    mx = 1.0f - smoothstep01(sx, sx + falloff, dx);
                    my = 1.0f - smoothstep01(sy, sy + falloff, dy);
                    mz = 1.0f - smoothstep01(sz, sz + falloff, dz);
                }
                m = mx * my * mz;
            }

            if (invert) m = 1.0f - m;

            // Resolve output per mode.
            float outR, outG, outB, outA;
            if (outMode == eOutputModeMatte) {
                outR = outG = outB = m;
                outA = m;
            } else if (outMode == eOutputModePremult) {
                outR = (srcP ? srcP[0] : 0.0f) * m;
                outG = (srcP ? srcP[1] : 0.0f) * m;
                outB = (srcP ? srcP[2] : 0.0f) * m;
                outA = (srcP && outNComp >= 4 ? srcP[3] : 1.0f) * m;
            } else {
                // Overlay: lerp source toward overlayColor by (m * overlayMix).
                const float t = m * ovMix;
                const float invT = 1.0f - t;
                const float sR = srcP ? srcP[0] : 0.0f;
                const float sG = srcP ? srcP[1] : 0.0f;
                const float sB = srcP ? srcP[2] : 0.0f;
                outR = sR * invT + ovR * t;
                outG = sG * invT + ovG * t;
                outB = sB * invT + ovB * t;
                outA = srcP && outNComp >= 4 ? srcP[3] : 1.0f;
            }

            // Final mix with source.
            if (mixVal < 1.0f && srcP) {
                outR = outR * mixVal + srcP[0] * invMix;
                outG = outG * mixVal + srcP[1] * invMix;
                outB = outB * mixVal + srcP[2] * invMix;
                if (outNComp >= 4) outA = outA * mixVal + srcP[3] * invMix;
            }

            if (outNComp >= 1) dst[0] = outR;
            if (outNComp >= 2) dst[1] = outG;
            if (outNComp >= 3) dst[2] = outB;
            if (outNComp >= 4) dst[3] = outA;
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
