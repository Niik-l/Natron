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

#ifndef NATRON_ENGINE_DEEPUTILS_H
#define NATRON_ENGINE_DEEPUTILS_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

class EffectInstance;

/**
 * @brief Retrieve a DeepImage from an upstream EffectInstance by trying
 * all known deep node types via dynamic_cast.
 *
 * This is the SINGLE source of truth for the deep side-channel lookup.
 * When adding a new deep node type that produces deep output, add it here.
 */
DeepImagePtr getDeepImageFromEffect(EffectInstance* effect);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEEPUTILS_H
