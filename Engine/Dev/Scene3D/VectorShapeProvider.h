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

#ifndef NATRON_ENGINE_VECTORSHAPEPROVIDER_H
#define NATRON_ENGINE_VECTORSHAPEPROVIDER_H

// VectorShapeProvider: the non-roto counterpart of Node::getRotoContext().
//
// MaskToGeo historically walks the input node's RotoContext Beziers. Nodes
// that generate vector shapes without a roto context (TraceMask) implement
// this tiny interface instead: they hand back a VectorDocument whose shapes
// are ALREADY in canonical (project format) coordinates, with per-shape
// compositingOperator carrying hole semantics (eMergeMinus / eMergeXOR).
// MaskToGeo prefers a provider over the roto context, so both node kinds
// feed the same mesh builder.

#include "Dev/Mask/VectorDocument.h"

NATRON_NAMESPACE_ENTER

class VectorShapeProvider
{
public:
    virtual ~VectorShapeProvider() {}

    /**
     * @brief Shapes at `time`, in canonical (project format) coordinates.
     * The returned document must be safe to read while the caller holds it:
     * implementations hand back an immutable snapshot. May return a document
     * with no shapes (nothing traced yet).
     */
    virtual VectorMask::VectorDocument getShapeDocument(double time) const = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_VECTORSHAPEPROVIDER_H
