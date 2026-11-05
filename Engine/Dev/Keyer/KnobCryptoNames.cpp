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

#include "KnobCryptoNames.h"

NATRON_NAMESPACE_ENTER

KnobCryptoNames::KnobCryptoNames(KnobHolder* holder,
                                 const std::string &description,
                                 int dimension,
                                 bool declaredByPlugin)
    : QObject()
    , AnimatingKnobStringHelper(holder, description, dimension, declaredByPlugin)
{
}

KnobCryptoNames::~KnobCryptoNames()
{
}

const std::string KnobCryptoNames::_typeNameStr("CryptoNames");

const std::string&
KnobCryptoNames::typeNameStatic()
{
    return _typeNameStr;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_KnobCryptoNames.cpp"
