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

#ifndef NATRON_ENGINE_POINTCLOUDPROVIDER_H
#define NATRON_ENGINE_POINTCLOUDPROVIDER_H

#include <vector>

#include "../../../Global/Macros.h"
#include "PointCloudData.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Interface for any node that can provide a point cloud.
 *
 * Implemented by DeepToPoints, Blast, and (future) Scatter / ParticleInstance /
 * any other node that emits a point cloud.
 *
 * Used by downstream consumers (Blast filter, viewport visualization, etc.) to
 * fetch points without caring which specific node type produced them. Mirrors
 * the CameraProvider / MaterialProvider pattern already used for cameras and
 * materials in this codebase — but lives inside NATRON_NAMESPACE because
 * PointCloudDataPtr is defined there.
 */
class PointCloudProvider
{
public:
    virtual ~PointCloudProvider() {}

    /**
     * @brief Get the current point cloud. May be null if not yet computed or
     * if the node has no points (e.g. empty source).
     *
     * Each point has six floats: x, y, z, r, g, b. Indexing is stable for the
     * life of the returned PointCloudDataPtr — downstream nodes (e.g. Blast
     * Mode 1 selection) can refer to points by their indices.
     */
    virtual PointCloudDataPtr getPointCloud() const = 0;

    /**
     * @brief Called by the 3D viewport whenever its point selection for this
     * provider's cloud changes (click-pick, box select, or clear). Indices
     * refer to the point order of getPointCloud() (stable, see above).
     * Default is a no-op; providers that offer selection-driven actions
     * (e.g. CameraTracker's set-origin / set-ground-plane) override this to
     * store the selection Engine-side.
     */
    virtual void setViewportSelection(const std::vector<int>& /*indices*/) {}
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_POINTCLOUDPROVIDER_H
