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
};

inline float
readSource(const Particle& p, int source)
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
    k->populateChoices(entries);
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
};

struct ParticleAttributePrivate
{
    KnobButtonWPtr resetAll;       // Phase 7 — top-level reset
    SectionKnobs   color;
    SectionKnobs   pscale;
    SectionKnobs   alpha;
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
    return tr("Drive particle Color, Pscale, and Alpha from per-particle sources.\n\n"
              "Three independent sections in one node:\n"
              "  • Color  — gradient editor, RGBA output\n"
              "  • Pscale — curve editor, size output\n"
              "  • Alpha  — curve editor, alpha-only output\n\n"
              "Each section has its own Source (default Age/Lifetime), range, "
              "editor, and Mix. Enable toggles the section. Isolate solos it so "
              "you can preview that attribute alone. Reset stamps the section's "
              "defaults.\n\n"
              "Typical placement: after a ParticleSolver, before render.").toStdString();
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

    // Source Min / Max / Fit on the same row.
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, QObject::tr("Source Min"));
        k->setName(prefix + "SourceMin");
        k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip(QObject::tr("Source value mapping to editor X = 0 (clamped below). "
                                      "If Source = Speed, set this to the slowest speed you care about. "
                                      "Click \"Fit\" to auto-detect from live particles."));
        group->addKnob(k); out.sourceMin = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, QObject::tr("Source Max"));
        k->setName(prefix + "SourceMax");
        k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip(QObject::tr("Source value mapping to editor X = 1 (clamped above). "
                                      "If Source = Speed, set this to the fastest speed you care about. "
                                      "Click \"Fit\" to auto-detect from live particles."));
        group->addKnob(k); out.sourceMax = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(self, QObject::tr("Fit"));
        k->setName(prefix + "Fit");
        k->setHintToolTip(QObject::tr("Sample upstream particles at the current frame and "
                                      "auto-fill Source Min/Max with the observed range of the "
                                      "selected Source attribute. No-op if no particle input is connected."));
        group->addKnob(k); out.fit = k;
    }

    // Editor: gradient or curve.
    if (useGradient) {
        KnobGradientPtr k = AppManager::createKnob<KnobGradient>(self, QObject::tr("Gradient"));
        k->setName(prefix + "Gradient");
        k->setAnimationEnabled(false);
        k->setHintToolTip(QObject::tr("Color stops. Click bar to add, drag to move, double-click to edit, right-click to delete."));
        group->addKnob(k); out.gradient = k;
    } else {
        KnobParametricPtr k = AppManager::createKnob<KnobParametric>(self, QObject::tr("Curve"), 1, false);
        k->setName(prefix + "Curve");
        k->setParametricRange(0.0, 1.0);
        k->setCurveColor(0, 0.85f, 0.85f, 0.85f);
        k->setHintToolTip(QObject::tr("Normalized source (X) → output (Y)."));
        (void)k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 0.0, 0.0, eKeyframeTypeLinear);
        (void)k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 1.0, 1.0, eKeyframeTypeLinear);
        k->setDefaultCurvesFromCurves();
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

    // Reset button.
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(self, QObject::tr("Reset"));
        k->setName(prefix + "Reset");
        k->setHintToolTip(QObject::tr("Restore this section's default editor + ranges. "
                                      "Use \"Reset All\" at the top for all three sections at once."));
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

    buildSection(this, page, tr("Color"),  "color",  /*useGradient*/ true,  _imp->color);
    buildSection(this, page, tr("Pscale"), "pscale", /*useGradient*/ false, _imp->pscale);
    buildSection(this, page, tr("Alpha"),  "alpha",  /*useGradient*/ false, _imp->alpha);

    // Stamp defaults so the editors have meaningful starting states.
    stampColorDefaults();
    stampPscaleDefaults();
    stampAlphaDefaults();

    // Populate hint labels with their initial summaries.
    refreshAllHints();
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
    // Bell: fade in, hold, fade out.
    stampCurvePoints(_imp->alpha.curve.lock(), {
        {0.0, 0.0},
        {0.5, 1.0},
        {1.0, 0.0},
    });
}

// ----------------------------------------------------------------------------
// Isolate radio + knobChanged dispatch
// ----------------------------------------------------------------------------

void
ParticleAttribute::enforceIsolateRadio(KnobI* changedKnob)
{
    // Identify which section was just turned on. Turn off the other two.
    KnobBoolPtr cI = _imp->color.isolate.lock();
    KnobBoolPtr pI = _imp->pscale.isolate.lock();
    KnobBoolPtr aI = _imp->alpha.isolate.lock();

    if (cI && changedKnob == cI.get() && cI->getValue()) {
        if (pI) pI->setValue(false);
        if (aI) aI->setValue(false);
    } else if (pI && changedKnob == pI.get() && pI->getValue()) {
        if (cI) cI->setValue(false);
        if (aI) aI->setValue(false);
    } else if (aI && changedKnob == aI.get() && aI->getValue()) {
        if (cI) cI->setValue(false);
        if (pI) pI->setValue(false);
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

    // Iterate once, track min/max.
    float lo =  std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < data->particles.size(); ++i) {
        const float v = readSource(data->particles[i], source);
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

// ----------------------------------------------------------------------------
// Reset All + summary labels (Phase 7)
// ----------------------------------------------------------------------------

namespace {
// Restore one section to factory defaults (Enable on, Isolate off,
// Source = Age/Lifetime, range 0-1, Mix 1).
void
restoreSectionDefaults(const SectionKnobs& sec)
{
    if (KnobBoolPtr   k = sec.enable.lock())    k->setValue(true);
    if (KnobBoolPtr   k = sec.isolate.lock())   k->setValue(false);
    if (KnobChoicePtr k = sec.source.lock())    k->setValue((int)eSrcAgeOverLife);
    if (KnobDoublePtr k = sec.sourceMin.lock()) k->setValue(0.0);
    if (KnobDoublePtr k = sec.sourceMax.lock()) k->setValue(1.0);
    if (KnobDoublePtr k = sec.mix.lock())       k->setValue(1.0);
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

    std::ostringstream os;
    if (!enabled) os << "[off] ";
    else if (soloed) os << "[solo] ";
    os << sourceLabel << " (" << sMin << "-" << sMax << ") -> " << targetLabel
       << " (" << (isColor ? "ramp" : "curve") << " x" << detail
       << ", mix " << mix << ")";
    return os.str();
}
} // anonymous namespace

void
ParticleAttribute::resetAll()
{
    restoreSectionDefaults(_imp->color);
    restoreSectionDefaults(_imp->pscale);
    restoreSectionDefaults(_imp->alpha);
    stampColorDefaults();
    stampPscaleDefaults();
    stampAlphaDefaults();
    refreshAllHints();
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
        if ((cI && k == cI.get()) || (pI && k == pI.get()) || (aI && k == aI.get())) {
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
        if (k == _imp->color.reset.lock().get())  { stampColorDefaults();  refreshAllHints(); return true; }
        if (k == _imp->pscale.reset.lock().get()) { stampPscaleDefaults(); refreshAllHints(); return true; }
        if (k == _imp->alpha.reset.lock().get())  { stampAlphaDefaults();  refreshAllHints(); return true; }
    }

    // Fit buttons — sample upstream at `time` and write min/max.
    if (reason == eValueChangedReasonUserEdited) {
        if (k == _imp->color.fit.lock().get())  { fitColorRange(time);  refreshAllHints(); return true; }
        if (k == _imp->pscale.fit.lock().get()) { fitPscaleRange(time); refreshAllHints(); return true; }
        if (k == _imp->alpha.fit.lock().get())  { fitAlphaRange(time);  refreshAllHints(); return true; }
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
};

SectionState
readSection(const SectionKnobs& sec, double time, bool isolatedAny, bool useGradient)
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
    s.sMin   = minK    ? minK->getValueAtTime(time)  : 0.0;
    s.sMax   = maxK    ? maxK->getValueAtTime(time)  : 1.0;
    s.mix    = mixK    ? (float)mixK->getValueAtTime(time) : 1.0f;
    const double range = (s.sMax - s.sMin);
    s.invRange = (std::abs(range) > 1e-9) ? (1.0 / range) : 0.0;

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
        ( _imp->color.isolate.lock()  && _imp->color.isolate.lock()->getValue() )
     || ( _imp->pscale.isolate.lock() && _imp->pscale.isolate.lock()->getValue() )
     || ( _imp->alpha.isolate.lock()  && _imp->alpha.isolate.lock()->getValue() );

    SectionState colorS  = readSection(_imp->color,  time, anyIsolated, /*gradient*/ true);
    SectionState pscaleS = readSection(_imp->pscale, time, anyIsolated, /*gradient*/ false);
    SectionState alphaS  = readSection(_imp->alpha,  time, anyIsolated, /*gradient*/ false);

    if (!colorS.active && !pscaleS.active && !alphaS.active) return;

    for (size_t i = 0; i < data->particles.size(); ++i) {
        Particle& p = data->particles[i];

        auto normalize = [&p](int src, double sMin, double invR) {
            const float s = readSource(p, src);
            double t = (s - sMin) * invR;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            return t;
        };

        auto lerp = [](float orig, float v, float m) { return orig + (v - orig) * m; };

        // Color — gradient sample → RGBA lerp
        if (colorS.active) {
            const double t = normalize(colorS.source, colorS.sMin, colorS.invRange);
            float gr, gg, gb, ga;
            KnobGradient::sample(colorS.stops, (float)t, &gr, &gg, &gb, &ga);
            p.r = lerp(p.r, gr, colorS.mix);
            p.g = lerp(p.g, gg, colorS.mix);
            p.b = lerp(p.b, gb, colorS.mix);
            p.a = lerp(p.a, ga, colorS.mix);
        }

        // Pscale — curve sample → size lerp
        if (pscaleS.active) {
            const double t = normalize(pscaleS.source, pscaleS.sMin, pscaleS.invRange);
            double y = 0.0;
            pscaleS.curve->getValue(0, t, &y);
            p.size = lerp(p.size, (float)y, pscaleS.mix);
        }

        // Alpha — curve sample → a lerp
        if (alphaS.active) {
            const double t = normalize(alphaS.source, alphaS.sMin, alphaS.invRange);
            double y = 0.0;
            alphaS.curve->getValue(0, t, &y);
            p.a = lerp(p.a, (float)y, alphaS.mix);
        }
    }
}

NATRON_NAMESPACE_EXIT
