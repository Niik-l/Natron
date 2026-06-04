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

#include "KnobPassTable.h"

NATRON_NAMESPACE_ENTER

KnobPassTable::KnobPassTable(KnobHolder* holder,
                            const std::string &description,
                            int dimension,
                            bool declaredByPlugin)
    : QObject()
    , AnimatingKnobStringHelper(holder, description, dimension, declaredByPlugin)
{
}

KnobPassTable::~KnobPassTable()
{
}

const std::string KnobPassTable::_typeNameStr("CyclesPassTable");

const std::string&
KnobPassTable::typeNameStatic()
{
    return _typeNameStr;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_KnobPassTable.cpp"
