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

#ifndef NATRON_ENGINE_MESHPROVIDER_H
#define NATRON_ENGINE_MESHPROVIDER_H

#include "MeshData.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A node that hands the scene a polygon mesh (MeshData) — ReadGeo,
 * GeoBuilder. SceneGraph, ScanlineRender, Cycles and the 3D viewport all go
 * through this instead of casting to ReadGeo, so a new mesh source only has
 * to implement getMeshData() and carry the usual translate/rotate/scale/
 * uniformScale knobs (read by name) to render everywhere.
 *
 * The returned pointer is a shared snapshot: callers hold it while reading and
 * never mutate it. Implementations rebuild and republish on edit.
 */
class MeshProvider
{
public:
    virtual ~MeshProvider() {}

    /** Mesh at `time`. -1 means "static / frame 0". May return null. */
    virtual MeshDataPtr getMeshData(double time) const = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_MESHPROVIDER_H
