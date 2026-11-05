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

#ifndef NATRON_ENGINE_KNOBCRYPTONAMES_H
#define NATRON_ENGINE_KNOBCRYPTONAMES_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../Knob.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The Cryptomatte node's manifest shown as a list. The value is a plain string, one object
 * name per line (what the node also keeps in its secret "manifestNames" string knob for scripts);
 * only the GUI differs: KnobGuiCryptoNames draws it as a filterable list where the rows the matte
 * includes are highlighted and selecting rows (click, shift-click a range, ctrl-click to toggle)
 * edits the Matte List. The highlight comes from the node's secret "manifestSelected" knob and
 * the picks go back through its secret "manifestPick" knob (see Cryptomatte.cpp).
 **/
class KnobCryptoNames
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
        return new KnobCryptoNames(holder, label, dimension, declaredByPlugin);
    }

    KnobCryptoNames(KnobHolder* holder,
                    const std::string &description,
                    int dimension,
                    bool declaredByPlugin);

    virtual ~KnobCryptoNames();

    static const std::string & typeNameStatic() WARN_UNUSED_RETURN;

private:

    virtual const std::string & typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    static const std::string _typeNameStr;
};

typedef std::shared_ptr<KnobCryptoNames> KnobCryptoNamesPtr;
typedef std::weak_ptr<KnobCryptoNames> KnobCryptoNamesWPtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBCRYPTONAMES_H
