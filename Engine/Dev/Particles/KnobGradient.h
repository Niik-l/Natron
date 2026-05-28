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

#ifndef NATRON_ENGINE_KNOBGRADIENT_H
#define NATRON_ENGINE_KNOBGRADIENT_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <string>
#include <vector>

#include "../../../Global/Macros.h"

#include "../../Knob.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A knob that stores a color gradient (sequence of color stops).
 *
 * Storage is a single string for save/load simplicity, following the
 * KnobShuffle precedent. Format: comma-only, paired tokens (pos, hex).
 *
 *   "0,ffd14aff,0.45,ff5a00ff,1,1a0010ff"
 *
 * Comma-only chosen because KnobShuffle's proven format ("0,1,2,3") uses
 * the same separator and is known to survive Natron's YAML project
 * serialization. Earlier `:` and `|` separators both broke project loads.
 *
 * The Gui side (KnobGuiGradient + GradientWidget) parses this on display,
 * lets the user drag/click stops, and writes the new state back as a
 * fresh string.
 *
 * Consumers (e.g. ParticleAttribute) read the string via getValue() and
 * call the static `sampleGradient()` helper to evaluate the gradient at
 * a normalized parametric position t in [0, 1].
 */
class KnobGradient
    : public QObject, public AnimatingKnobStringHelper
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobHelper * BuildKnob(KnobHolder* holder,
                                  const std::string& label,
                                  int dimension,
                                  bool declaredByPlugin = true)
    {
        return new KnobGradient(holder, label, dimension, declaredByPlugin);
    }

    KnobGradient(KnobHolder* holder,
                 const std::string& description,
                 int dimension,
                 bool declaredByPlugin);
    virtual ~KnobGradient();

    static const std::string& typeNameStatic() WARN_UNUSED_RETURN;

    // ---------- Stops API (parses / serializes the stored string) ----------

    struct Stop {
        double position = 0.0;     // [0, 1]
        float  r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    };

    /// Default gradient used by Reset / first-creation (warm fire-style ramp).
    static std::vector<Stop> defaultStops();

    /// Parse "p:rrggbbaa;..." into Stop list. Returns defaultStops() on empty/invalid.
    static std::vector<Stop> parse(const std::string& serialized);

    /// Inverse of parse().
    static std::string       serialize(const std::vector<Stop>& stops);

    /**
     * Sample the gradient at parametric position t in [0, 1].
     * Stops are assumed sorted by position; out-of-range t clamps to
     * the nearest end stop. Returns rgba in [0,1] floats.
     *
     * This is the hot path called once per particle per frame from
     * ParticleAttribute::applyForce, so we keep it allocation-free
     * (caller passes in the parsed stops vector).
     */
    static void sample(const std::vector<Stop>& stops, float t,
                       float* r, float* g, float* b, float* a);

private:

    virtual const std::string& typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    static const std::string _typeNameStr;
};

typedef std::shared_ptr<KnobGradient> KnobGradientPtr;
typedef std::weak_ptr<KnobGradient>   KnobGradientWPtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBGRADIENT_H
