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

#include "ParticleAttribute.h"
#include "../DotUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include "../../AppManager.h"
#include "../../ChoiceOption.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

#include "KnobGradient.h"
#include "ParticleData.h"

NATRON_NAMESPACE_ENTER

// ----------------------------------------------------------------------------
// Source attribute enum + reader (shared by all sections)
// ----------------------------------------------------------------------------

namespace {
enum SourceAttr {
    eSrcAge = 0,
    eSrcAgeOverLife,
    eSrcSpeed,
    eSrcVelocityX,
    eSrcVelocityY,
    eSrcVelocityZ,
    eSrcPositionX,
    eSrcPositionY,
    eSrcPositionZ,
    eSrcBounceCount,
    eSrcSpawnIndex,
    eSrcRandomID,       // stable hash of particle ID -> [0,1)
    eSrcFlicker,        // smooth value noise over (ID, time) -> [0,1)
};

// Stable per-particle hash -> [0,1). NEVER rand(): identical across frames,
// scrubs, and re-sims, so renders are deterministic.
inline float
hash01(uint32_t a, uint32_t b)
{
    uint32_t h = a * 2654435761u + b * 2246822519u;
    h ^= h >> 15; h *= 2246822519u;
    h ^= h >> 13; h *= 3266489917u;
    h ^= h >> 16;
    return (float)(h & 0x00FFFFFFu) / 16777216.0f;
}

// Smooth per-particle value noise over time (flicker). `speed` = cycles per
// frame. Piecewise-smoothstep between per-integer-step hash values.
inline float
flicker01(uint32_t id, uint32_t seed, double time, double speed)
{
    const double tt = time * speed;
    const double t0 = std::floor(tt);
    float f = (float)(tt - t0);
    f = f * f * (3.0f - 2.0f * f); // smoothstep
    const uint32_t k0 = (uint32_t)(int64_t)t0;
    const float v0 = hash01(id ^ (k0 * 0x9E3779B9u), seed);
    const float v1 = hash01(id ^ ((k0 + 1u) * 0x9E3779B9u), seed);
    return v0 + (v1 - v0) * f;
}

// Per-evaluation context for the stochastic sources.
struct SourceCtx
{
    uint32_t seed = 1;
    double time = 0.0;
    double flickerSpeed = 0.35;
};

inline float
readSource(const Particle& p, int source, const SourceCtx& ctx)
{
    switch (source) {
    case eSrcAge:           return p.age;
    case eSrcAgeOverLife:   return (p.life > 0.0001f) ? (p.age / p.life) : 0.0f;
    case eSrcSpeed:         return std::sqrt(p.vx*p.vx + p.vy*p.vy + p.vz*p.vz);
    case eSrcVelocityX:     return p.vx;
    case eSrcVelocityY:     return p.vy;
    case eSrcVelocityZ:     return p.vz;
    case eSrcPositionX:     return p.px;
    case eSrcPositionY:     return p.py;
    case eSrcPositionZ:     return p.pz;
    case eSrcBounceCount:   return (float)p.bounceCount;
    case eSrcSpawnIndex:    return (float)p.id;
    case eSrcRandomID:      return hash01(p.id, ctx.seed * 0x85EBCA6Bu);
    case eSrcFlicker:       return flicker01(p.id, ctx.seed * 0xC2B2AE35u, ctx.time, ctx.flickerSpeed);
    default:                return 0.0f;
    }
}

void
populateSourceChoices(KnobChoicePtr k)
{
    std::vector<ChoiceOption> entries;
    entries.push_back(ChoiceOption("Age",            "", "Particle age in frames since spawn."));
    entries.push_back(ChoiceOption("Age / Lifetime", "", "Normalized 0-1 over the particle's life."));
    entries.push_back(ChoiceOption("Speed",          "", "Velocity magnitude."));
    entries.push_back(ChoiceOption("Velocity X",     "", "Signed X velocity component."));
    entries.push_back(ChoiceOption("Velocity Y",     "", "Signed Y velocity component."));
    entries.push_back(ChoiceOption("Velocity Z",     "", "Signed Z velocity component."));
    entries.push_back(ChoiceOption("Position X",     "", "World-X of the particle."));
    entries.push_back(ChoiceOption("Position Y",     "", "World-Y of the particle."));
    entries.push_back(ChoiceOption("Position Z",     "", "World-Z of the particle."));
    entries.push_back(ChoiceOption("Bounce Count",   "", "Number of collisions so far."));
    entries.push_back(ChoiceOption("Spawn Index",    "", "Particle ID."));
    entries.push_back(ChoiceOption("Random (per ID)", "", "Stable random value per particle (0-1), seeded by the section's Variation Seed. Same particle = same value every frame."));
    entries.push_back(ChoiceOption("Flicker (noise)", "", "Smooth per-particle noise over time (0-1) — intensity wobble, twinkle. Speed set by the node's Flicker Speed knob."));
    k->populateChoices(entries);
}

// Bias shapes for the Variation multiplier.
enum VariationBias {
    eBiasUniform = 0,     // 1 +/- amount, evenly spread
    eBiasFewHigh,         // most stay ~1, a few outliers up to 1 + 3*amount
    eBiasFewLow,          // most stay ~1, a few outliers down toward 1 - amount
};

inline float
variationFactor(uint32_t id, uint32_t seed, int bias, float amount)
{
    if (amount <= 0.0f) return 1.0f;
    const float u = hash01(id, seed * 0x27D4EB2Fu);
    switch (bias) {
    case eBiasFewHigh: {
        const float o = u * u * u; const float o6 = o * o; // u^6
        return 1.0f + amount * 3.0f * o6;
    }
    case eBiasFewLow: {
        const float o = u * u * u; const float o6 = o * o;
        const float f = 1.0f - amount * o6;
        return f > 0.0f ? f : 0.0f;
    }
    case eBiasUniform:
    default: {
        const float f = 1.0f + amount * (2.0f * u - 1.0f);
        return f > 0.0f ? f : 0.0f;
    }
    }
}

// Hue rotation for the Color section's variation (degrees, +/-).
inline void
rotateHue(float& r, float& g, float& b, float degrees)
{
    // Rodrigues rotation about the grey axis — cheap, good enough for jitter.
    const float rad = degrees * 3.14159265f / 180.0f;
    const float cosA = std::cos(rad);
    const float sinA = std::sin(rad);
    const float third = 1.0f / 3.0f;
    const float rt3 = 0.57735027f; // 1/sqrt(3)
    const float nr = r * (cosA + (1 - cosA) * third) + g * (third * (1 - cosA) - rt3 * sinA) + b * (third * (1 - cosA) + rt3 * sinA);
    const float ng = r * (third * (1 - cosA) + rt3 * sinA) + g * (cosA + third * (1 - cosA)) + b * (third * (1 - cosA) - rt3 * sinA);
    const float nb = r * (third * (1 - cosA) - rt3 * sinA) + g * (third * (1 - cosA) + rt3 * sinA) + b * (cosA + third * (1 - cosA));
    r = nr > 0 ? nr : 0;
    g = ng > 0 ? ng : 0;
    b = nb > 0 ? nb : 0;
}
} // anonymous namespace

// ----------------------------------------------------------------------------
// PImpl — three independent sections of knob pointers
// ----------------------------------------------------------------------------

struct SectionKnobs
{
    KnobBoolWPtr       enable;
    KnobBoolWPtr       isolate;
    KnobChoiceWPtr     source;
    KnobDoubleWPtr     sourceMin;
    KnobDoubleWPtr     sourceMax;
    KnobButtonWPtr     fit;        // sample upstream → write min/max
    KnobParametricWPtr curve;      // populated for Pscale + Alpha sections
    KnobGradientWPtr   gradient;   // populated for Color section
    KnobDoubleWPtr     mix;
    KnobButtonWPtr     reset;
    KnobStringWPtr     hint;       // read-only summary label (Phase 7)

    // Variation row (v2): stable per-ID breakup on the section's output.
    // Color: hue jitter (+/- amount * 90 deg). Others: multiplier with Bias.
    KnobDoubleWPtr     variationAmount;
    KnobIntWPtr        variationSeed;
    KnobChoiceWPtr     variationBias;  // scalar sections only

    // v2 layout: Range mode dropdown (min/max/fit only shown in Custom),
    // Shape presets + on-demand curve editor (scalar sections only).
    KnobChoiceWPtr     rangeMode;
    KnobChoiceWPtr     shape;
    KnobBoolWPtr       editCurve;
};

// Range dropdown modes.
enum RangeMode {
    eRangeNormalized = 0, // 0-1, the right answer for Age/Lifetime, Random, Flicker
    eRangeAuto,           // re-fit observed min/max every frame (great for Speed)
    eRangeFitNow,         // transient: samples once, then flips to Custom
    eRangeCustom,         // explicit min/max fields
};

// Shape preset dropdown (scalar sections). Order matters: serialized by index.
enum ShapePreset {
    eShapeLinear = 0,
    eShapeCoolingDecay,
    eShapeBell,
    eShapeGrowFade,
    eShapeEaseOut,
    eShapeConstant,
    eShapeCustom,         // set automatically when the curve is hand-edited
};

struct ParticleAttributePrivate
{
    KnobButtonWPtr resetAll;       // Phase 7 — top-level reset
    KnobDoubleWPtr flickerSpeed;   // v2 — node-level speed for Flicker sources
    SectionKnobs   color;
    SectionKnobs   pscale;
    SectionKnobs   alpha;
    SectionKnobs   emission;       // v2 — per-particle emission (Cycles glow)
};

// ----------------------------------------------------------------------------
// Construction
// ----------------------------------------------------------------------------

ParticleAttribute::ParticleAttribute(NodePtr node)
    : ParticleModifier(node)
    , _imp(new ParticleAttributePrivate())
{
}

ParticleAttribute::~ParticleAttribute()
{
}

std::string
ParticleAttribute::getPluginDescription() const
{
    return tr("Drive particle Color, Pscale, Alpha, and Emission from per-particle sources.\n\n"
              "Four independent sections in one node:\n"
              "  • Color    — gradient editor, RGBA output\n"
              "  • Pscale   — curve editor, size output\n"
              "  • Alpha    — curve editor, alpha-only output\n"
              "  • Emission — curve editor, self-emission (Cycles glow; off by default)\n\n"
              "Each section has its own Source (default Age/Lifetime), range, "
              "editor, Mix, and a Variation row — stable per-particle random "
              "breakup (hue jitter for Color; a biased multiplier for the "
              "others, e.g. 'a few sparks are much hotter'). New sources: "
              "Random (per ID) and Flicker (noise over time).\n\n"
              "Emission multiplies ParticleMaterial's Emission Strength per "
              "particle in CyclesRender, and brightens additive particles in "
              "ScanlineRender.\n\n"
              "Typical placement: after a ParticleSolver, before render. Chain "
              "several ParticleAttribute nodes to layer variations.").toStdString();
}

// ----------------------------------------------------------------------------
// Knob construction helpers
// ----------------------------------------------------------------------------

namespace {
// Build the standard knob set for one section into a KnobGroup.
// `useGradient` switches the editor: Color uses gradient, scalar
// sections use a parametric curve. `prefix` is the script-name prefix
// (e.g. "color"). Returns the knobs in `out`.
void
buildSection(ParticleAttribute* self,
             KnobPagePtr page,
             const QString& title,
             const std::string& prefix,
             bool useGradient,
             SectionKnobs& out)
{
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(self, title);
    group->setName(prefix);
    group->setAsTab();              // consecutive tab-groups render as a tab bar
    group->setDefaultValue(true);   // expanded by default
    page->addKnob(group);

    // Enable + Isolate on one row.
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(self, QObject::tr("Enable"));
        k->setName(prefix + "Enable");
        k->setDefaultValue(true);
        k->setAddNewLine(false);
        k->setHintToolTip(QObject::tr("Apply this section. When off, the section is skipped."));
        group->addKnob(k); out.enable = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(self, QObject::tr("Isolate"));
        k->setName(prefix + "Isolate");
        k->setDefaultValue(false);
        k->setHintToolTip(QObject::tr("Solo this section — when on, all other sections are muted so you can preview this one alone. Radio-like across sections."));
        group->addKnob(k); out.isolate = k;
    }

    // Source.
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(self, QObject::tr("Source"));
        k->setName(prefix + "Source");
        populateSourceChoices(k);
        k->setDefaultValue((int)eSrcAgeOverLife);
        k->setAnimationEnabled(true);
        k->setHintToolTip(QObject::tr("Which per-particle attribute drives the editor below. "
                                      "Common picks: Age/Lifetime for over-life animation; "
                                      "Speed for energy-based effects; Position Y for height tinting."));
        group->addKnob(k); out.source = k;
    }

    // Range mode + (Custom-only) Min / Max / Fit on the same row.
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(self, QObject::tr("Range"));
        k->setName(prefix + "RangeMode");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("0 - 1 (normalized)", "",
                                       "Source maps straight onto the editor. The right choice for "
                                       "Age/Lifetime, Random, and Flicker (they are already 0-1)."));
        entries.push_back(ChoiceOption("Auto (fit every frame)", "",
                                       "Re-fit the observed min/max of live particles every frame. "
                                       "Great for Speed — no numbers to manage, but the mapping "
                                       "shifts as the sim evolves."));
        entries.push_back(ChoiceOption("Fit Now -> Custom", "",
                                       "Sample upstream particles once, fill Min/Max with the "
                                       "observed range, and switch to Custom."));
        entries.push_back(ChoiceOption("Custom...", "",
                                       "Explicit Min/Max fields (shown when selected)."));
        k->populateChoices(entries);
        k->setDefaultValue((int)eRangeNormalized);
        k->setAddNewLine(false);
        k->setHintToolTip(QObject::tr("How the Source value maps onto the editor's 0-1 X axis."));
        group->addKnob(k); out.rangeMode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, QObject::tr("Min"));
        k->setName(prefix + "SourceMin");
        k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip(QObject::tr("Source value mapping to editor X = 0 (clamped below)."));
        group->addKnob(k); out.sourceMin = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, QObject::tr("Max"));
        k->setName(prefix + "SourceMax");
        k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip(QObject::tr("Source value mapping to editor X = 1 (clamped above)."));
        group->addKnob(k); out.sourceMax = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(self, QObject::tr("Fit"));
        k->setName(prefix + "Fit");
        k->setHintToolTip(QObject::tr("Sample upstream particles at the current frame and "
                                      "auto-fill Min/Max with the observed range of the "
                                      "selected Source attribute. No-op if no particle input is connected."));
        group->addKnob(k); out.fit = k;
    }

    // Editor: gradient (always visible) or curve (Shape presets + on-demand editor).
    if (useGradient) {
        KnobGradientPtr k = AppManager::createKnob<KnobGradient>(self, QObject::tr("Gradient"));
        k->setName(prefix + "Gradient");
        k->setAnimationEnabled(false);
        k->setHintToolTip(QObject::tr("Color stops. Click bar to add, drag to move, double-click to edit, right-click to delete."));
        group->addKnob(k); out.gradient = k;
    } else {
        {
            KnobChoicePtr k = AppManager::createKnob<KnobChoice>(self, QObject::tr("Shape"));
            k->setName(prefix + "Shape");
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption("Linear",         "", "Straight 0 -> 1 ramp."));
            entries.push_back(ChoiceOption("Cooling Decay",  "", "Bright at birth, decays to 0 — sparks, embers."));
            entries.push_back(ChoiceOption("Bell",           "", "Fade in, hold, fade out."));
            entries.push_back(ChoiceOption("Grow / Fade",    "", "Rapid growth, slow decay — the classic size-over-life."));
            entries.push_back(ChoiceOption("Ease Out",       "", "Starts at 1, eases down to 0."));
            entries.push_back(ChoiceOption("Constant 1.0",   "", "Flat 1 — the section's Variation/Mix still apply."));
            entries.push_back(ChoiceOption("Custom (edited)", "", "Set automatically when you hand-edit the curve. "
                                                                  "Your control points are kept and saved."));
            k->populateChoices(entries);
            k->setDefaultValue((int)eShapeLinear);
            k->setAddNewLine(false);
            k->setHintToolTip(QObject::tr("Curve preset. Picking one stamps its control points into the "
                                          "curve; open \"Edit Curve\" to add/drag/delete points freely — "
                                          "the preset flips to Custom and your edits are preserved."));
            group->addKnob(k); out.shape = k;
        }
        {
            KnobBoolPtr k = AppManager::createKnob<KnobBool>(self, QObject::tr("Edit Curve"));
            k->setName(prefix + "EditCurve");
            k->setDefaultValue(false);
            k->setAnimationEnabled(false);
            k->setEvaluateOnChange(false);
            k->setIsPersistent(false); // panels always open compact
            k->setHintToolTip(QObject::tr("Show the full curve editor below to remap freely "
                                          "(add/drag/delete points). Hide it again when done — "
                                          "the curve keeps working either way."));
            group->addKnob(k); out.editCurve = k;
        }
        KnobParametricPtr k = AppManager::createKnob<KnobParametric>(self, QObject::tr("Curve"), 1, false);
        k->setName(prefix + "Curve");
        k->setParametricRange(0.0, 1.0);
        k->setCurveColor(0, 0.85f, 0.85f, 0.85f);
        k->setHintToolTip(QObject::tr("Normalized source (X) → output (Y)."));
        (void)k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 0.0, 0.0, eKeyframeTypeLinear);
        (void)k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 1.0, 1.0, eKeyframeTypeLinear);
        k->setDefaultCurvesFromCurves();
        k->setSecretByDefault(true); // revealed by Edit Curve
        group->addKnob(k); out.curve = k;
    }

    // Mix.
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, QObject::tr("Mix"));
        k->setName(prefix + "Mix");
        k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(QObject::tr("How much of the editor output to apply. "
                                      "1.0 = full replace, 0.5 = blend half with original, "
                                      "0 = no effect (useful for keyframe-fading the section in/out)."));
        group->addKnob(k); out.mix = k;
    }

    // Variation row (v2) — per-ID breakup of this section's output.
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, QObject::tr(useGradient ? "Hue Variation" : "Variation"));
        k->setName(prefix + "VariationAmount");
        k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setAddNewLine(false);
        if (useGradient) {
            k->setHintToolTip(QObject::tr("Per-particle hue jitter: each particle's color is rotated "
                                          "by a stable random hue offset up to ±(amount × 90°). "
                                          "0 = off. Great for breaking up uniform fire/spark colors."));
        } else {
            k->setHintToolTip(QObject::tr("Per-particle random multiplier on this section's output. "
                                          "0 = off. Spread depends on Bias: Uniform = ±amount around 1; "
                                          "Few High Outliers = most stay put, rare particles up to 1+3×amount "
                                          "(sparks: 'some are much hotter'); Few Low Outliers = rare dimmer ones. "
                                          "Stable per particle ID — no flicker across frames."));
        }
        group->addKnob(k); out.variationAmount = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(self, QObject::tr("Seed"));
        k->setName(prefix + "VariationSeed");
        k->setDefaultValue(1); k->setAnimationEnabled(false);
        k->setAddNewLine(useGradient); // scalar sections continue the row with Bias
        k->setHintToolTip(QObject::tr("Random seed for this section's Variation AND its "
                                      "Random (per ID) source. Change to re-roll which particles differ."));
        group->addKnob(k); out.variationSeed = k;
    }
    if (!useGradient) {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(self, QObject::tr("Bias"));
        k->setName(prefix + "VariationBias");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Uniform",           "", "Evenly spread: 1 ± amount."));
        entries.push_back(ChoiceOption("Few High Outliers", "", "Most particles unchanged; a few boosted up to 1 + 3×amount."));
        entries.push_back(ChoiceOption("Few Low Outliers",  "", "Most particles unchanged; a few reduced toward 1 − amount."));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        group->addKnob(k); out.variationBias = k;
    }

    // Reset button.
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(self, QObject::tr("Reset"));
        k->setName(prefix + "Reset");
        k->setHintToolTip(QObject::tr("Restore this section's default editor + ranges. "
                                      "Use \"Reset All\" at the top for all sections at once."));
        group->addKnob(k); out.reset = k;
    }

    // Read-only summary label — refreshed by refreshAllHints() after any
    // knob change that might affect its content.
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(self, QObject::tr("Summary"));
        k->setName(prefix + "Hint");
        k->setDefaultValue("");
        k->setAsLabel();
        k->setHintToolTip(QObject::tr("Live summary of this section's current configuration."));
        group->addKnob(k); out.hint = k;
    }
}
} // anonymous namespace

void
ParticleAttribute::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Attribute"));

    // Top-level Reset All — single-click restore of all three sections.
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reset All"));
        k->setName("resetAll");
        k->setHintToolTip(tr("Restore every section to its default editor + ranges + Mix/Enable. "
                             "Equivalent to clicking each section's Reset and re-enabling them."));
        page->addKnob(k); _imp->resetAll = k;
    }

    // Node-level flicker speed — shared by every section using the
    // Flicker (noise) source.
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Flicker Speed"));
        k->setName("flickerSpeed");
        k->setDefaultValue(0.35); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Speed of the Flicker (noise) source, in cycles per frame. "
                             "0.35 ≈ a gentle wobble; 1+ = fast twinkle. Applies to every "
                             "section whose Source is set to Flicker."));
        page->addKnob(k); _imp->flickerSpeed = k;
    }

    buildSection(this, page, tr("Color"),    "color",    /*useGradient*/ true,  _imp->color);
    buildSection(this, page, tr("Pscale"),   "pscale",   /*useGradient*/ false, _imp->pscale);
    buildSection(this, page, tr("Alpha"),    "alpha",    /*useGradient*/ false, _imp->alpha);
    buildSection(this, page, tr("Emission"), "emission", /*useGradient*/ false, _imp->emission);

    // Emission ships DISABLED: it's new in v2, and old projects reloading this
    // node must not suddenly gain an emission ramp.
    if (KnobBoolPtr k = _imp->emission.enable.lock()) {
        k->setDefaultValue(false);
        k->setValue(false);
    }

    // Stamp defaults so the editors have meaningful starting states.
    stampColorDefaults();
    stampPscaleDefaults();
    stampAlphaDefaults();
    stampEmissionDefaults();

    // Populate hint labels with their initial summaries + conditional
    // visibility (curve editors hidden, Custom fields hidden, etc.).
    refreshAllHints();
    refreshDynamicUI();
}

// ----------------------------------------------------------------------------
// Default stampers (used at init + Reset clicks)
// ----------------------------------------------------------------------------

namespace {
void
stampCurvePoints(KnobParametricPtr k, const std::vector<std::pair<double, double>>& pts)
{
    if (!k) return;
    (void)k->deleteAllControlPoints(eValueChangedReasonNatronInternalEdited, 0);
    for (size_t i = 0; i < pts.size(); ++i) {
        (void)k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0,
                                 pts[i].first, pts[i].second, eKeyframeTypeSmooth);
    }
}

void
stampGradientStops(KnobGradientPtr k, const std::vector<KnobGradient::Stop>& stops)
{
    if (!k) return;
    k->setValue(KnobGradient::serialize(stops));
}

// Stamp a Shape preset's control points into a section's curve and sync the
// Shape dropdown. eShapeCustom is a no-op (the user's points stand).
void
stampShapePreset(const SectionKnobs& sec, int preset)
{
    KnobParametricPtr curve = sec.curve.lock();
    if (!curve || preset == (int)eShapeCustom) return;
    switch (preset) {
    case eShapeLinear:       stampCurvePoints(curve, {{0.0, 0.0}, {1.0, 1.0}}); break;
    case eShapeCoolingDecay: stampCurvePoints(curve, {{0.0, 1.0}, {0.35, 0.55}, {1.0, 0.0}}); break;
    case eShapeBell:         stampCurvePoints(curve, {{0.0, 0.0}, {0.5, 1.0}, {1.0, 0.0}}); break;
    case eShapeGrowFade:     stampCurvePoints(curve, {{0.0, 0.05}, {0.15, 0.95}, {0.6, 0.6}, {1.0, 0.1}}); break;
    case eShapeEaseOut:      stampCurvePoints(curve, {{0.0, 1.0}, {0.3, 0.4}, {1.0, 0.0}}); break;
    case eShapeConstant:     stampCurvePoints(curve, {{0.0, 1.0}, {1.0, 1.0}}); break;
    default: break;
    }
    if (KnobChoicePtr sk = sec.shape.lock()) {
        if (sk->getValue() != preset) sk->setValue(preset);
    }
}
} // anonymous namespace

void
ParticleAttribute::stampColorDefaults()
{
    // Warm fire ramp.
    std::vector<KnobGradient::Stop> stops(3);
    stops[0] = { 0.00, 1.000f, 0.820f, 0.290f, 1.0f };
    stops[1] = { 0.45, 1.000f, 0.353f, 0.000f, 1.0f };
    stops[2] = { 1.00, 0.102f, 0.000f, 0.063f, 1.0f };
    stampGradientStops(_imp->color.gradient.lock(), stops);
}

void
ParticleAttribute::stampPscaleDefaults()
{
    if (KnobChoicePtr k = _imp->pscale.shape.lock()) k->setValue((int)eShapeGrowFade);
    // Ease-out: rapid growth, slow decay. End at 0.1 (not 0) so dying
    // particles stay visible until their lifetime expires — going to 0
    // makes them vanish before the alpha-fade has a chance to read as
    // "fading out".
    stampCurvePoints(_imp->pscale.curve.lock(), {
        {0.0,  0.05},
        {0.15, 0.95},
        {0.6,  0.6 },
        {1.0,  0.1 },
    });
}

void
ParticleAttribute::stampAlphaDefaults()
{
    if (KnobChoicePtr k = _imp->alpha.shape.lock()) k->setValue((int)eShapeBell);
    // Bell: fade in, hold, fade out.
    stampCurvePoints(_imp->alpha.curve.lock(), {
        {0.0, 0.0},
        {0.5, 1.0},
        {1.0, 0.0},
    });
}

void
ParticleAttribute::stampEmissionDefaults()
{
    if (KnobChoicePtr k = _imp->emission.shape.lock()) k->setValue((int)eShapeCoolingDecay);
    // Cooling: bright at birth, decays to dark — the spark/ember shape.
    stampCurvePoints(_imp->emission.curve.lock(), {
        {0.0,  1.0 },
        {0.35, 0.55},
        {1.0,  0.0 },
    });
}

// ----------------------------------------------------------------------------
// Isolate radio + knobChanged dispatch
// ----------------------------------------------------------------------------

void
ParticleAttribute::enforceIsolateRadio(KnobI* changedKnob)
{
    // Identify which section was just turned on. Turn off the others.
    KnobBoolPtr isolates[4] = {
        _imp->color.isolate.lock(),
        _imp->pscale.isolate.lock(),
        _imp->alpha.isolate.lock(),
        _imp->emission.isolate.lock(),
    };
    for (int i = 0; i < 4; ++i) {
        if (isolates[i] && changedKnob == isolates[i].get() && isolates[i]->getValue()) {
            for (int j = 0; j < 4; ++j) {
                if (j != i && isolates[j]) isolates[j]->setValue(false);
            }
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// Fit helpers — sample upstream particles, compute min/max for the section's
// selected source, write into Source Min / Source Max.
// ----------------------------------------------------------------------------

namespace {
void
fitSectionRange(ParticleAttribute* self, SectionKnobs& sec, double time)
{
    // Resolve upstream particle stream.
    EffectInstancePtr input = skipDots(self->getInput(0));
    if (!input) return;
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(input.get());
    if (!provider) return;

    ParticleDataPtr data = provider->getParticleData(time);
    if (!data || data->particles.empty()) return;

    KnobChoicePtr sourceK = sec.source.lock();
    if (!sourceK) return;
    const int source = sourceK->getValue();

    SourceCtx ctx;
    ctx.time = time;
    if (KnobIntPtr sk = sec.variationSeed.lock()) ctx.seed = (uint32_t)std::max(0, sk->getValue());

    // Iterate once, track min/max.
    float lo =  std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < data->particles.size(); ++i) {
        const float v = readSource(data->particles[i], source, ctx);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (!std::isfinite(lo) || !std::isfinite(hi)) return;

    // Degenerate case: all particles produce the same value. Nudge by a
    // small epsilon so the range isn't zero-width (otherwise the curve
    // lookup collapses to a single point).
    if (std::abs(hi - lo) < 1e-6f) {
        const float pad = std::max(1e-3f, std::abs(lo) * 0.05f);
        lo -= pad;
        hi += pad;
    }

    KnobDoublePtr minK = sec.sourceMin.lock();
    KnobDoublePtr maxK = sec.sourceMax.lock();
    if (minK) minK->setValue((double)lo);
    if (maxK) maxK->setValue((double)hi);
}
} // anonymous namespace

void
ParticleAttribute::fitColorRange(double time)  { fitSectionRange(this, _imp->color,  time); }
void
ParticleAttribute::fitPscaleRange(double time) { fitSectionRange(this, _imp->pscale, time); }
void
ParticleAttribute::fitAlphaRange(double time)  { fitSectionRange(this, _imp->alpha,  time); }
void
ParticleAttribute::fitEmissionRange(double time) { fitSectionRange(this, _imp->emission, time); }

// ----------------------------------------------------------------------------
// Reset All + summary labels (Phase 7)
// ----------------------------------------------------------------------------

namespace {
// Restore one section to factory defaults (Enable on, Isolate off,
// Source = Age/Lifetime, range 0-1, Mix 1).
void
restoreSectionDefaults(const SectionKnobs& sec, bool enabledDefault = true)
{
    if (KnobBoolPtr   k = sec.enable.lock())    k->setValue(enabledDefault);
    if (KnobBoolPtr   k = sec.isolate.lock())   k->setValue(false);
    if (KnobChoicePtr k = sec.source.lock())    k->setValue((int)eSrcAgeOverLife);
    if (KnobDoublePtr k = sec.sourceMin.lock()) k->setValue(0.0);
    if (KnobDoublePtr k = sec.sourceMax.lock()) k->setValue(1.0);
    if (KnobDoublePtr k = sec.mix.lock())       k->setValue(1.0);
    if (KnobDoublePtr k = sec.variationAmount.lock()) k->setValue(0.0);
    if (KnobIntPtr    k = sec.variationSeed.lock())   k->setValue(1);
    if (KnobChoicePtr k = sec.variationBias.lock())   k->setValue(0);
    if (KnobChoicePtr k = sec.rangeMode.lock())       k->setValue((int)eRangeNormalized);
    if (KnobBoolPtr   k = sec.editCurve.lock())       k->setValue(false);
}

// Build the live summary text for one section.
std::string
buildHintText(const SectionKnobs& sec, bool isColor, const std::string& targetLabel)
{
    std::string sourceLabel = "?";
    int source = (int)eSrcAgeOverLife;
    if (KnobChoicePtr sk = sec.source.lock()) {
        source = sk->getValue();
        std::vector<ChoiceOption> entries = sk->getEntries_mt_safe();
        if (source >= 0 && source < (int)entries.size()) {
            sourceLabel = entries[source].label;
        }
    }
    double sMin = 0.0, sMax = 1.0, mix = 1.0;
    if (KnobDoublePtr k = sec.sourceMin.lock()) sMin = k->getValue();
    if (KnobDoublePtr k = sec.sourceMax.lock()) sMax = k->getValue();
    if (KnobDoublePtr k = sec.mix.lock())       mix  = k->getValue();
    bool enabled = sec.enable.lock()  ? sec.enable.lock()->getValue()  : true;
    bool soloed  = sec.isolate.lock() ? sec.isolate.lock()->getValue() : false;

    int detail = 0;
    if (isColor) {
        if (KnobGradientPtr g = sec.gradient.lock()) {
            std::vector<KnobGradient::Stop> stops = KnobGradient::parse(g->getValue());
            detail = (int)stops.size();
        }
    } else {
        if (KnobParametricPtr c = sec.curve.lock()) {
            int n = 0;
            (void)c->getNControlPoints(0, &n);
            detail = n;
        }
    }

    double variation = 0.0;
    if (KnobDoublePtr k = sec.variationAmount.lock()) variation = k->getValue();

    std::ostringstream os;
    if (!enabled) os << "[off] ";
    else if (soloed) os << "[solo] ";
    os << sourceLabel << " (" << sMin << "-" << sMax << ") -> " << targetLabel
       << " (" << (isColor ? "ramp" : "curve") << " x" << detail
       << ", mix " << mix;
    if (variation > 0.0) os << ", var " << variation;
    os << ")";
    return os.str();
}
} // anonymous namespace

void
ParticleAttribute::resetAll()
{
    restoreSectionDefaults(_imp->color);
    restoreSectionDefaults(_imp->pscale);
    restoreSectionDefaults(_imp->alpha);
    restoreSectionDefaults(_imp->emission, /*enabledDefault*/ false);
    stampColorDefaults();
    stampPscaleDefaults();
    stampAlphaDefaults();
    stampEmissionDefaults();
    refreshAllHints();
    refreshDynamicUI();
}

void
ParticleAttribute::refreshAllHints()
{
    if (KnobStringPtr k = _imp->color.hint.lock())
        k->setValue(buildHintText(_imp->color,  /*isColor*/ true,  "Color"));
    if (KnobStringPtr k = _imp->pscale.hint.lock())
        k->setValue(buildHintText(_imp->pscale, /*isColor*/ false, "Size"));
    if (KnobStringPtr k = _imp->alpha.hint.lock())
        k->setValue(buildHintText(_imp->alpha,  /*isColor*/ false, "Alpha"));
    if (KnobStringPtr k = _imp->emission.hint.lock())
        k->setValue(buildHintText(_imp->emission, /*isColor*/ false, "Emission"));
}

namespace {
// Conditional visibility/enabled state for one section (v2 layout pass).
void
refreshSectionUI(const SectionKnobs& sec, bool useGradient)
{
    const bool enabled = sec.enable.lock() ? sec.enable.lock()->getValue() : true;
    const int  range   = sec.rangeMode.lock() ? sec.rangeMode.lock()->getValue() : (int)eRangeCustom;
    const bool custom  = (range == (int)eRangeCustom);
    const bool editing = sec.editCurve.lock() ? sec.editCurve.lock()->getValue() : false;
    const double variation = sec.variationAmount.lock() ? sec.variationAmount.lock()->getValue() : 0.0;

    // Disabled section collapses to its Enable row.
    auto vis = [&](bool showWhenEnabled) { return enabled && showWhenEnabled; };
    if (KnobChoicePtr k = sec.source.lock())          k->setSecret(!vis(true));
    if (KnobChoicePtr k = sec.rangeMode.lock())       k->setSecret(!vis(true));
    if (KnobDoublePtr k = sec.sourceMin.lock())       k->setSecret(!vis(custom));
    if (KnobDoublePtr k = sec.sourceMax.lock())       k->setSecret(!vis(custom));
    if (KnobButtonPtr k = sec.fit.lock())             k->setSecret(!vis(custom));
    if (KnobGradientPtr k = sec.gradient.lock())      k->setSecret(!vis(true));
    if (KnobChoicePtr k = sec.shape.lock())           k->setSecret(!vis(true));
    if (KnobBoolPtr   k = sec.editCurve.lock())       k->setSecret(!vis(true));
    if (KnobParametricPtr k = sec.curve.lock())       k->setSecret(!vis(editing));
    if (KnobDoublePtr k = sec.mix.lock())             k->setSecret(!vis(true));
    if (KnobDoublePtr k = sec.variationAmount.lock()) k->setSecret(!vis(true));
    if (KnobIntPtr    k = sec.variationSeed.lock())   k->setSecret(!vis(true));
    if (KnobChoicePtr k = sec.variationBias.lock())   k->setSecret(!vis(true));
    if (KnobButtonPtr k = sec.reset.lock())           k->setSecret(!vis(true));
    if (KnobStringPtr k = sec.hint.lock())            k->setSecret(!vis(true));
    if (KnobBoolPtr   k = sec.isolate.lock())         k->setSecret(!enabled);

    // Variation sub-knobs grey out at amount 0 (visible but clearly inert).
    // Uses setAllDimensionsEnabled (plugin-level), never the user lock.
    const bool varOn = variation > 0.0;
    if (KnobIntPtr    k = sec.variationSeed.lock()) {
        const bool usesRandom = sec.source.lock()
            && (sec.source.lock()->getValue() == (int)eSrcRandomID
                || sec.source.lock()->getValue() == (int)eSrcFlicker);
        k->setAllDimensionsEnabled(varOn || usesRandom); // seed also feeds Random/Flicker
    }
    if (KnobChoicePtr k = sec.variationBias.lock()) k->setAllDimensionsEnabled(varOn);
    (void)useGradient;
}
} // anonymous namespace

void
ParticleAttribute::refreshDynamicUI()
{
    refreshSectionUI(_imp->color,    /*gradient*/ true);
    refreshSectionUI(_imp->pscale,   /*gradient*/ false);
    refreshSectionUI(_imp->alpha,    /*gradient*/ false);
    refreshSectionUI(_imp->emission, /*gradient*/ false);

    // Flicker Speed only matters while some section's Source is Flicker.
    bool anyFlicker = false;
    const SectionKnobs* secs[4] = { &_imp->color, &_imp->pscale, &_imp->alpha, &_imp->emission };
    for (int i = 0; i < 4; ++i) {
        KnobChoicePtr src = secs[i]->source.lock();
        if (src && src->getValue() == (int)eSrcFlicker) { anyFlicker = true; break; }
    }
    if (KnobDoublePtr k = _imp->flickerSpeed.lock()) k->setSecret(!anyFlicker);
}

void
ParticleAttribute::onKnobsLoaded()
{
    // Legacy projects predate the Range dropdown: they carry meaningful
    // Min/Max values with the knob at its default. Infer Custom so their
    // mapping is preserved exactly.
    const SectionKnobs* secs[4] = { &_imp->color, &_imp->pscale, &_imp->alpha, &_imp->emission };
    for (int i = 0; i < 4; ++i) {
        KnobChoicePtr range = secs[i]->rangeMode.lock();
        KnobDoublePtr mn = secs[i]->sourceMin.lock();
        KnobDoublePtr mx = secs[i]->sourceMax.lock();
        if (!range || !mn || !mx) continue;
        if (range->getValue() == (int)eRangeNormalized
            && (std::abs(mn->getValue()) > 1e-9 || std::abs(mx->getValue() - 1.0) > 1e-9)) {
            range->setValue((int)eRangeCustom);
        }
    }
    refreshDynamicUI();
    refreshAllHints();
}

bool
ParticleAttribute::knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec /*view*/,
                               double time, bool /*originatedFromMainThread*/)
{
    if (!k) return false;

    // Isolate radio behavior — only for user toggles, not internal flips.
    if (reason == eValueChangedReasonUserEdited) {
        KnobBoolPtr cI = _imp->color.isolate.lock();
        KnobBoolPtr pI = _imp->pscale.isolate.lock();
        KnobBoolPtr aI = _imp->alpha.isolate.lock();
        KnobBoolPtr eI = _imp->emission.isolate.lock();
        if ((cI && k == cI.get()) || (pI && k == pI.get()) || (aI && k == aI.get())
            || (eI && k == eI.get())) {
            enforceIsolateRadio(k);
            return true;
        }
    }

    // Reset All — top-level button restores every section.
    if (reason == eValueChangedReasonUserEdited
        && k == _imp->resetAll.lock().get()) {
        resetAll();
        return true;
    }

    // Reset buttons.
    if (reason == eValueChangedReasonUserEdited) {
        if (k == _imp->color.reset.lock().get())    { stampColorDefaults();    refreshAllHints(); return true; }
        if (k == _imp->pscale.reset.lock().get())   { stampPscaleDefaults();   refreshAllHints(); return true; }
        if (k == _imp->alpha.reset.lock().get())    { stampAlphaDefaults();    refreshAllHints(); return true; }
        if (k == _imp->emission.reset.lock().get()) { stampEmissionDefaults(); refreshAllHints(); return true; }
    }

    // Fit buttons — sample upstream at `time` and write min/max.
    if (reason == eValueChangedReasonUserEdited) {
        if (k == _imp->color.fit.lock().get())    { fitColorRange(time);    refreshAllHints(); return true; }
        if (k == _imp->pscale.fit.lock().get())   { fitPscaleRange(time);   refreshAllHints(); return true; }
        if (k == _imp->alpha.fit.lock().get())    { fitAlphaRange(time);    refreshAllHints(); return true; }
        if (k == _imp->emission.fit.lock().get()) { fitEmissionRange(time); refreshAllHints(); return true; }
    }

    // v2 layout pass — per-section Shape / Range / Edit Curve handling.
    {
        SectionKnobs* secs[4] = { &_imp->color, &_imp->pscale, &_imp->alpha, &_imp->emission };
        for (int i = 0; i < 4; ++i) {
            SectionKnobs& sec = *secs[i];

            // Shape preset picked -> stamp its points (Custom = no-op).
            if (reason == eValueChangedReasonUserEdited
                && sec.shape.lock() && k == sec.shape.lock().get()) {
                stampShapePreset(sec, sec.shape.lock()->getValue());
                refreshAllHints();
                return true;
            }

            // Curve hand-edited -> flip Shape to Custom (stamps come through
            // as NatronInternalEdited, so presets don't trip this).
            if (reason == eValueChangedReasonUserEdited
                && sec.curve.lock() && k == sec.curve.lock().get()) {
                if (KnobChoicePtr sk = sec.shape.lock()) {
                    if (sk->getValue() != (int)eShapeCustom) sk->setValue((int)eShapeCustom);
                }
                return false; // let the edit evaluate normally
            }

            // Range mode: "Fit Now" samples once, then lands in Custom.
            if (reason == eValueChangedReasonUserEdited
                && sec.rangeMode.lock() && k == sec.rangeMode.lock().get()) {
                if (sec.rangeMode.lock()->getValue() == (int)eRangeFitNow) {
                    fitSectionRange(this, sec, time);
                    sec.rangeMode.lock()->setValue((int)eRangeCustom);
                }
                refreshDynamicUI();
                refreshAllHints();
                return true;
            }

            // Edit Curve toggle / Enable toggle / Source or Variation change ->
            // visibility + grey-outs.
            if ( (sec.editCurve.lock()       && k == sec.editCurve.lock().get())
              || (sec.enable.lock()          && k == sec.enable.lock().get())
              || (sec.source.lock()          && k == sec.source.lock().get())
              || (sec.variationAmount.lock() && k == sec.variationAmount.lock().get()) ) {
                refreshDynamicUI();
                if (reason == eValueChangedReasonUserEdited) refreshAllHints();
                return false; // these knobs may still need normal evaluation
            }
        }
    }

    // Generic refresh: any user edit might change the summary text.
    if (reason == eValueChangedReasonUserEdited) {
        refreshAllHints();
    }

    return false;
}

// ----------------------------------------------------------------------------
// applyForce — iterate sections in order, respecting Enable + Isolate
// ----------------------------------------------------------------------------

namespace {
// Cached per-section state read once per applyForce call.
struct SectionState
{
    bool                            active = false;
    int                             source = 0;
    double                          sMin = 0.0;
    double                          sMax = 1.0;
    double                          invRange = 1.0;
    float                           mix = 1.0f;
    KnobParametricPtr               curve;
    std::vector<KnobGradient::Stop> stops;
    bool                            isGradient = false;
    // v2: variation + stochastic-source context
    float                           variation = 0.0f;
    int                             bias = 0;
    SourceCtx                       ctx;
    bool                            autoRange = false; // Range = Auto: fit from live data per frame
};

SectionState
readSection(const SectionKnobs& sec, double time, double flickerSpeed,
            bool isolatedAny, bool useGradient)
{
    SectionState s;
    KnobBoolPtr   enableK   = sec.enable.lock();
    KnobBoolPtr   isolateK  = sec.isolate.lock();
    KnobChoicePtr sourceK   = sec.source.lock();
    KnobDoublePtr minK      = sec.sourceMin.lock();
    KnobDoublePtr maxK      = sec.sourceMax.lock();
    KnobDoublePtr mixK      = sec.mix.lock();

    const bool enabled  = enableK  ? enableK->getValue()  : true;
    const bool isolated = isolateK ? isolateK->getValue() : false;
    if (!enabled) return s;
    if (isolatedAny && !isolated) return s;

    s.source = sourceK ? sourceK->getValue() : (int)eSrcAgeOverLife;
    s.mix    = mixK    ? (float)mixK->getValueAtTime(time) : 1.0f;

    // Range mode: normalized = 0-1; Auto = fitted from live data by the
    // caller; Custom (and the transient Fit Now) = the Min/Max knobs.
    const int rangeModeVal = sec.rangeMode.lock() ? sec.rangeMode.lock()->getValue()
                                                  : (int)eRangeCustom;
    if (rangeModeVal == (int)eRangeNormalized) {
        s.sMin = 0.0; s.sMax = 1.0;
    } else if (rangeModeVal == (int)eRangeAuto) {
        s.autoRange = true;
        s.sMin = 0.0; s.sMax = 1.0; // replaced by the caller's data fit
    } else {
        s.sMin = minK ? minK->getValueAtTime(time) : 0.0;
        s.sMax = maxK ? maxK->getValueAtTime(time) : 1.0;
    }
    const double range = (s.sMax - s.sMin);
    s.invRange = (std::abs(range) > 1e-9) ? (1.0 / range) : 0.0;

    if (KnobDoublePtr k = sec.variationAmount.lock()) s.variation = (float)k->getValueAtTime(time);
    if (KnobChoicePtr k = sec.variationBias.lock())   s.bias = k->getValue();
    s.ctx.time = time;
    s.ctx.flickerSpeed = flickerSpeed;
    if (KnobIntPtr k = sec.variationSeed.lock()) s.ctx.seed = (uint32_t)std::max(0, k->getValue());

    s.isGradient = useGradient;
    if (useGradient) {
        KnobGradientPtr gK = sec.gradient.lock();
        if (!gK) return s;
        s.stops = KnobGradient::parse(gK->getValue());
    } else {
        s.curve = sec.curve.lock();
        if (!s.curve) return s;
    }
    s.active = true;
    return s;
}
} // anonymous namespace

void
ParticleAttribute::applyForce(ParticleDataPtr data, double time)
{
    if (!data) return;

    const bool anyIsolated =
        ( _imp->color.isolate.lock()    && _imp->color.isolate.lock()->getValue() )
     || ( _imp->pscale.isolate.lock()   && _imp->pscale.isolate.lock()->getValue() )
     || ( _imp->alpha.isolate.lock()    && _imp->alpha.isolate.lock()->getValue() )
     || ( _imp->emission.isolate.lock() && _imp->emission.isolate.lock()->getValue() );

    const double flickerSpeed = _imp->flickerSpeed.lock()
                              ? _imp->flickerSpeed.lock()->getValueAtTime(time) : 0.35;

    SectionState colorS    = readSection(_imp->color,    time, flickerSpeed, anyIsolated, /*gradient*/ true);
    SectionState pscaleS   = readSection(_imp->pscale,   time, flickerSpeed, anyIsolated, /*gradient*/ false);
    SectionState alphaS    = readSection(_imp->alpha,    time, flickerSpeed, anyIsolated, /*gradient*/ false);
    SectionState emissionS = readSection(_imp->emission, time, flickerSpeed, anyIsolated, /*gradient*/ false);

    if (!colorS.active && !pscaleS.active && !alphaS.active && !emissionS.active) return;

    // Range = Auto: fit each such section's min/max from the live data.
    {
        SectionState* states[4] = { &colorS, &pscaleS, &alphaS, &emissionS };
        for (int si = 0; si < 4; ++si) {
            SectionState& s = *states[si];
            if (!s.active || !s.autoRange) continue;
            float lo =  std::numeric_limits<float>::infinity();
            float hi = -std::numeric_limits<float>::infinity();
            for (size_t i = 0; i < data->particles.size(); ++i) {
                const float v = readSource(data->particles[i], s.source, s.ctx);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            if (std::isfinite(lo) && std::isfinite(hi)) {
                if (hi - lo < 1e-6f) hi = lo + 1e-6f;
                s.sMin = lo;
                s.sMax = hi;
                s.invRange = 1.0 / (s.sMax - s.sMin);
            }
        }
    }

    for (size_t i = 0; i < data->particles.size(); ++i) {
        Particle& p = data->particles[i];

        auto normalize = [&p](const SectionState& s) {
            const float v = readSource(p, s.source, s.ctx);
            double t = (v - s.sMin) * s.invRange;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            return t;
        };

        auto lerp = [](float orig, float v, float m) { return orig + (v - orig) * m; };

        // Color — gradient sample → RGBA lerp; variation = per-ID hue jitter
        if (colorS.active) {
            const double t = normalize(colorS);
            float gr, gg, gb, ga;
            KnobGradient::sample(colorS.stops, (float)t, &gr, &gg, &gb, &ga);
            if (colorS.variation > 0.0f) {
                const float u = hash01(p.id, colorS.ctx.seed * 0x27D4EB2Fu);
                rotateHue(gr, gg, gb, colorS.variation * (2.0f * u - 1.0f) * 90.0f);
            }
            p.r = lerp(p.r, gr, colorS.mix);
            p.g = lerp(p.g, gg, colorS.mix);
            p.b = lerp(p.b, gb, colorS.mix);
            p.a = lerp(p.a, ga, colorS.mix);
        }

        // Pscale — curve sample → size lerp; variation = per-ID multiplier
        if (pscaleS.active) {
            const double t = normalize(pscaleS);
            double y = 0.0;
            pscaleS.curve->getValue(0, t, &y);
            float v = (float)y * variationFactor(p.id, pscaleS.ctx.seed, pscaleS.bias, pscaleS.variation);
            p.size = lerp(p.size, v, pscaleS.mix);
        }

        // Alpha — curve sample → a lerp; variation = per-ID multiplier
        if (alphaS.active) {
            const double t = normalize(alphaS);
            double y = 0.0;
            alphaS.curve->getValue(0, t, &y);
            float v = (float)y * variationFactor(p.id, alphaS.ctx.seed, alphaS.bias, alphaS.variation);
            p.a = lerp(p.a, v, alphaS.mix);
        }

        // Emission — curve sample → emission lerp; variation = per-ID multiplier
        // (Bias "Few High Outliers" = the 'some sparks are much hotter' control)
        if (emissionS.active) {
            const double t = normalize(emissionS);
            double y = 0.0;
            emissionS.curve->getValue(0, t, &y);
            float v = (float)y * variationFactor(p.id, emissionS.ctx.seed, emissionS.bias, emissionS.variation);
            p.emission = lerp(p.emission, v, emissionS.mix);
        }
    }
}

NATRON_NAMESPACE_EXIT
