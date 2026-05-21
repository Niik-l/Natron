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

#ifndef NATRON_ENGINE_KNOBSHUFFLE_H
#define NATRON_ENGINE_KNOBSHUFFLE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../Knob.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A knob that displays an interactive channel routing widget.
 * Used by DevShuffle to show visual noodle-style channel routing.
 * Stores routing as a simple string "0,1,2,3".
 **/
class KnobShuffle
    : public QObject, public AnimatingKnobStringHelper
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobHelper * BuildKnob(KnobHolder* holder,
                                  const std::string &label,
                                  int dimension,
                                  bool declaredByPlugin = true)
    {
        return new KnobShuffle(holder, label, dimension, declaredByPlugin);
    }

    KnobShuffle(KnobHolder* holder,
                const std::string &description,
                int dimension,
                bool declaredByPlugin);

    virtual ~KnobShuffle();

    static const std::string & typeNameStatic() WARN_UNUSED_RETURN;

private:

    virtual const std::string & typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    static const std::string _typeNameStr;
};

typedef std::shared_ptr<KnobShuffle> KnobShufflePtr;
typedef std::weak_ptr<KnobShuffle> KnobShuffleWPtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBSHUFFLE_H
