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

#ifndef NATRON_ENGINE_SCENEGRAPH_H
#define NATRON_ENGINE_SCENEGRAPH_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <cmath>
#include <string>
#include <vector>

#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Type of object in the 3D scene.
 */
enum SceneNodeType {
    eSceneNodeGroup,
    eSceneNodeMesh,
    eSceneNodeCard,
    eSceneNodeCamera,
    eSceneNodePointCloud,
    eSceneNodeSphere,
    eSceneNodeCube,
    eSceneNodeCylinder,
    eSceneNodeParticles,
    eSceneNodeVolume,
    eSceneNodeLight
};

/**
 * @brief A single node in the 3D scene graph.
 *
 * Stores a local transform and a computed world transform (parent chain composed).
 * All matrices are column-major (GL-ready for glMultMatrixf).
 */
struct SceneNode {
    SceneNodeType type;
    std::string name;          // Script name of the source Natron node
    NodeWPtr sourceNode;       // Weak ref — null if node was deleted

    float localMatrix[16];     // Column-major 4x4, computed from node's TRS knobs
    float worldMatrix[16];     // Column-major 4x4, = parent.world * this.local

    int parentIndex;           // -1 = root (no parent)
    std::vector<int> childIndices;

    int subIndex;              // For cards: which card (0-3). -1 for others.
    bool visible;

    SceneNode()
        : type(eSceneNodeGroup)
        , parentIndex(-1)
        , subIndex(-1)
        , visible(true)
    {
        setIdentity(localMatrix);
        setIdentity(worldMatrix);
    }

    static void setIdentity(float m[16])
    {
        for (int i = 0; i < 16; ++i) m[i] = 0;
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
};

/**
 * @brief A lightweight scene graph for the 3D viewport.
 *
 * Owns a flat vector of SceneNodes with parent/child indices forming a tree.
 * Rebuilt from the Natron node graph each frame. World transforms are computed
 * in a single depth-first pass.
 *
 * All matrices are stored column-major (GL-ready).
 * No OpenGL dependencies — this class lives in Engine.
 */
class SceneGraph
{
public:

    SceneGraph();

    /**
     * @brief Rebuild the scene graph from the project's node list.
     * Call this once per frame before rendering.
     *
     * Three passes:
     *   1. Discover nodes and extract local transforms
     *   2. Link Group3D inputs to children via parent/child indices
     *   3. Compute world transforms (depth-first traversal)
     */
    void rebuild(const NodesList& allNodes, double time);

    /** @brief Get all scene nodes (read-only). */
    const std::vector<SceneNode>& nodes() const { return _nodes; }

    /** @brief Get root node indices (nodes with no parent). */
    const std::vector<int>& rootIndices() const { return _rootIndices; }

    /** @brief Find a node by name. Returns -1 if not found. */
    int findByName(const std::string& name) const;

    /** @brief Number of nodes in the scene. */
    int size() const { return (int)_nodes.size(); }

    // Matrix utilities (column-major, GL-ready)
    static void buildTRS(float tx, float ty, float tz,
                         float rx, float ry, float rz,
                         float sx, float sy, float sz,
                         float out[16]);

    static void multiply(const float a[16], const float b[16], float out[16]);

private:

    void computeWorldTransforms();
    void computeWorldRecursive(int nodeIdx);

    std::vector<SceneNode> _nodes;
    std::vector<int> _rootIndices;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCENEGRAPH_H
