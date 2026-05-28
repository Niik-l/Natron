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

#ifndef NATRON_ENGINE_PARTICLEATTRIBUTE_H
#define NATRON_ENGINE_PARTICLEATTRIBUTE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "ParticleModifier.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ParticleAttributePrivate;

/**
 * @brief Drive particle Color, Pscale, and Alpha from per-particle
 * sources via curves and gradients — all three editors visible in one
 * node so you can see them work together.
 *
 * Each section (Color, Pscale, Alpha) is independent:
 *   - Enable on/off
 *   - Isolate (solo) — radio-like across sections
 *   - Source dropdown (Age/Lifetime by default; pick any per-particle attr)
 *   - Source Min/Max (normalize source to [0,1] before lookup)
 *   - Editor: Color uses a Gradient, Pscale + Alpha use a Curve
 *   - Mix (lerp between original and curve/gradient output)
 *   - Reset (re-stamps the section's defaults)
 *
 * applyForce iterates particles once and, for each enabled section
 * (respecting Isolate), reads source, normalizes, looks up the editor,
 * lerps into the target field with Mix.
 *
 * Composability: placed after a ParticleSolver (typical) or anywhere in
 * the chain (no-op velocity contribution unless a section targets a
 * velocity attribute — V1 doesn't, so it's a pure look modifier).
 */
class ParticleAttribute
    : public ParticleModifier
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ParticleAttribute(n); }

    ParticleAttribute(NodePtr node);
    virtual ~ParticleAttribute();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_PARTICLEATTRIBUTE; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ParticleAttribute"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // ParticleModifier hook — apply enabled sections in order (Color,
    // Pscale, Alpha). One particle pass per call.
    virtual void applyForce(ParticleDataPtr data, double time) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view,
                             double time, bool originatedFromMainThread) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;

    // Default-config stampers (used by Reset buttons + at construction).
    void stampColorDefaults();
    void stampPscaleDefaults();
    void stampAlphaDefaults();

    // When the user turns ON Isolate in one section, turn off Isolate
    // in the other two so it's effectively a radio selection.
    void enforceIsolateRadio(KnobI* changedKnob);

    // Fit handlers — sample upstream particles at `time`, compute the
    // min/max of the section's selected Source, write into Source Min/Max.
    void fitColorRange(double time);
    void fitPscaleRange(double time);
    void fitAlphaRange(double time);

    // Reset All — stamps every section back to defaults and clears
    // Source/Range/Mix/Enable/Isolate overrides. Single click vs. three
    // Reset clicks across the sections.
    void resetAll();

    // Refresh the three sections' read-only summary labels. Called after
    // any knob change that affects what the labels display.
    void refreshAllHints();

    std::unique_ptr<ParticleAttributePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PARTICLEATTRIBUTE_H
