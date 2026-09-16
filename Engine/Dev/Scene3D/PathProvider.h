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

#ifndef NATRON_ENGINE_PATHPROVIDER_H
#define NATRON_ENGINE_PATHPROVIDER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A 3D rail: a smooth curve through control points that a camera (or
 * any consumer) can be placed on by a 0..1 parameter. Implemented by Path3D.
 *
 * evalPath() is arc-length parametrised, so equal steps in u are equal
 * distances along the curve — keying u from 0 to 1 gives constant speed.
 */
class PathProvider
{
public:
    virtual ~PathProvider() {}

    virtual int pathPointCount() const = 0;
    virtual bool pathPoint(int index, double out[3]) const = 0;
    virtual bool pathClosed() const = 0;

    /** Position and unit tangent at u in [0,1] (u wraps when the path is
     *  closed, clamps otherwise). False when the path has fewer than 2 points. */
    virtual bool evalPath(double u, double pos[3], double tangent[3]) const = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PATHPROVIDER_H
