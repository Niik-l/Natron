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

#include "SceneGraph.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "Camera3DNode.h"
#include "Card3D.h"
#include "Cube3D.h"
#include "Cylinder3D.h"
#include "../Deep/DeepToPoints.h"
#include "Light3D.h"
#include "../Particles/ParticleEmitter.h"
#include "../Particles/ParticleGravity.h"
#include "ReadVDB.h"
#include "Volume3D.h"
#include "Sphere3D.h"
#include "Group3D.h"
#include "../../Node.h"
#include "../Deep/PointCloudData.h"
#include "ReadAlembicCamera.h"
#include "ReadGeo.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

SceneGraph::SceneGraph()
{
}

void
SceneGraph::buildTRS(float tx, float ty, float tz,
                     float rx, float ry, float rz,
                     float sx, float sy, float sz,
                     float out[16])
{
    // Build column-major 4x4 TRS matrix
    // Order: Scale → RotateZ → RotateX → RotateY → Translate

    float crx = cosf(rx * (float)M_PI / 180.0f), srx = sinf(rx * (float)M_PI / 180.0f);
    float cry = cosf(ry * (float)M_PI / 180.0f), sry = sinf(ry * (float)M_PI / 180.0f);
    float crz = cosf(rz * (float)M_PI / 180.0f), srz = sinf(rz * (float)M_PI / 180.0f);

    // Rotation matrix = Ry * Rx * Rz (column-major)
    float r00 = cry * crz + sry * srx * srz;
    float r01 = crx * srz;
    float r02 = -sry * crz + cry * srx * srz;

    float r10 = -cry * srz + sry * srx * crz;
    float r11 = crx * crz;
    float r12 = sry * srz + cry * srx * crz;

    float r20 = sry * crx;
    float r21 = -srx;
    float r22 = cry * crx;

    // Column-major: out[col*4 + row]
    out[0]  = r00 * sx;  out[1]  = r01 * sx;  out[2]  = r02 * sx;  out[3]  = 0;
    out[4]  = r10 * sy;  out[5]  = r11 * sy;  out[6]  = r12 * sy;  out[7]  = 0;
    out[8]  = r20 * sz;  out[9]  = r21 * sz;  out[10] = r22 * sz;  out[11] = 0;
    out[12] = tx;        out[13] = ty;        out[14] = tz;        out[15] = 1;
}

void
SceneGraph::multiply(const float a[16], const float b[16], float out[16])
{
    // Column-major 4x4 matrix multiply: out = a * b
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

int
SceneGraph::findByName(const std::string& name) const
{
    for (int i = 0; i < (int)_nodes.size(); ++i) {
        if (_nodes[i].name == name) return i;
    }
    return -1;
}

void
SceneGraph::rebuild(const NodesList& allNodes, double time)
{
    _nodes.clear();
    _rootIndices.clear();

    // Name → index map for Pass 2
    std::map<std::string, int> nameToIndex;

    // ===== PASS 1: Discover nodes and extract local transforms =====

    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
        NodePtr node = *it;
        if (!node) continue;

        // Skip deactivated/deleted nodes
        if (!node->isActivated()) continue;

        EffectInstancePtr effect = node->getEffectInstance();
        if (!effect) continue;

        std::string nodeName = node->getScriptName();

        // --- Group3D ---
        Group3D* group = dynamic_cast<Group3D*>(effect.get());
        if (group) {
            SceneNode sn;
            sn.type = eSceneNodeGroup;
            sn.name = nodeName;
            sn.sourceNode = node;

            float gtx, gty, gtz, grx, gry, grz, gsx, gsy, gsz;
            group->getGroupTransform(time, gtx, gty, gtz, grx, gry, grz, gsx, gsy, gsz);
            buildTRS(gtx, gty, gtz, grx, gry, grz, gsx, gsy, gsz, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- ReadGeo ---
        ReadGeo* readGeo = dynamic_cast<ReadGeo*>(effect.get());
        if (readGeo) {
            MeshDataPtr mesh = readGeo->getMeshData(time);
            if (!mesh || mesh->numVertices == 0) continue;

            SceneNode sn;
            sn.type = eSceneNodeMesh;
            sn.name = nodeName;
            sn.sourceNode = node;

            // Transpose Imath row-major → GL column-major
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    sn.localMatrix[c * 4 + r] = mesh->transform[r * 4 + c];
                }
            }

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Camera3DNode ---
        Camera3DNode* cam3d = dynamic_cast<Camera3DNode*>(effect.get());
        if (cam3d) {
            double ctx, cty, ctz, crx, cry, crz;
            cam3d->getCameraPosition(time, ctx, cty, ctz, crx, cry, crz);

            SceneNode sn;
            sn.type = eSceneNodeCamera;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)ctx, (float)cty, (float)ctz,
                     (float)crx, (float)cry, (float)crz,
                     1, 1, 1, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- ReadAlembicCamera ---
        ReadAlembicCamera* abcCam = dynamic_cast<ReadAlembicCamera*>(effect.get());
        if (abcCam) {
            double ctx, cty, ctz, crx, cry, crz;
            abcCam->getCameraTransform(time, ctx, cty, ctz, crx, cry, crz);

            SceneNode sn;
            sn.type = eSceneNodeCamera;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)ctx, (float)cty, (float)ctz,
                     (float)crx, (float)cry, (float)crz,
                     1, 1, 1, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Sphere3D ---
        Sphere3D* sphere3d = dynamic_cast<Sphere3D*>(effect.get());
        if (sphere3d) {
            double stx, sty, stz, srx, sry, srz, ssx, ssy, ssz;
            sphere3d->getSphereTransform(time, stx, sty, stz, srx, sry, srz, ssx, ssy, ssz);

            SceneNode sn;
            sn.type = eSceneNodeSphere;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)stx, (float)sty, (float)stz,
                     (float)srx, (float)sry, (float)srz,
                     (float)ssx, (float)ssy, (float)ssz, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Card3D ---
        Card3D* card3d_new = dynamic_cast<Card3D*>(effect.get());
        if (card3d_new) {
            double ctx, cty, ctz, crx, cry, crz, csx, csy;
            card3d_new->getCardTransform(time, ctx, cty, ctz, crx, cry, crz, csx, csy);

            SceneNode sn;
            sn.type = eSceneNodeCard;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)ctx, (float)cty, (float)ctz,
                     (float)crx, (float)cry, (float)crz,
                     (float)csx, (float)csy, 1.0f, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Cube3D ---
        Cube3D* cube3d = dynamic_cast<Cube3D*>(effect.get());
        if (cube3d) {
            double ctx, cty, ctz, crx, cry, crz, csx, csy, csz;
            cube3d->getCubeTransform(time, ctx, cty, ctz, crx, cry, crz, csx, csy, csz);

            SceneNode sn;
            sn.type = eSceneNodeCube;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)ctx, (float)cty, (float)ctz,
                     (float)crx, (float)cry, (float)crz,
                     (float)csx, (float)csy, (float)csz, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Cylinder3D ---
        Cylinder3D* cyl3d = dynamic_cast<Cylinder3D*>(effect.get());
        if (cyl3d) {
            double ctx, cty, ctz, crx, cry, crz, csx, csy, csz;
            cyl3d->getCylinderTransform(time, ctx, cty, ctz, crx, cry, crz, csx, csy, csz);

            SceneNode sn;
            sn.type = eSceneNodeCylinder;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)ctx, (float)cty, (float)ctz,
                     (float)crx, (float)cry, (float)crz,
                     (float)csx, (float)csy, (float)csz, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Light3D ---
        Light3D* light3d = dynamic_cast<Light3D*>(effect.get());
        if (light3d) {
            double ltx, lty, ltz, lr, lg, lb, lint;
            light3d->getLightParams(time, ltx, lty, ltz, lr, lg, lb, lint);

            SceneNode sn;
            sn.type = eSceneNodeLight;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)ltx, (float)lty, (float)ltz, 0, 0, 0, 1, 1, 1, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- ReadVDB ---
        ReadVDB* readVdb = dynamic_cast<ReadVDB*>(effect.get());
        if (readVdb) {
            double tx, ty, tz, sx, sy, sz;
            readVdb->getTransform(time, tx, ty, tz, sx, sy, sz);

            SceneNode sn;
            sn.type = eSceneNodeVolume;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)tx, (float)ty, (float)tz,
                     0, 0, 0,
                     (float)sx, (float)sy, (float)sz, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Volume3D ---
        Volume3D* vol3d = dynamic_cast<Volume3D*>(effect.get());
        if (vol3d) {
            Volume3D::VolumeParams vp = vol3d->getVolumeParams(time);
            SceneNode sn;
            sn.type = eSceneNodeVolume;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS(vp.centerX, vp.centerY, vp.centerZ,
                     0, 0, 0,
                     vp.scaleX, vp.scaleY, vp.scaleZ, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- ParticleEmitter / ParticleGravity ---
        ParticleEmitter* pEmitter = dynamic_cast<ParticleEmitter*>(effect.get());
        ParticleGravity* pGravity = dynamic_cast<ParticleGravity*>(effect.get());
        if (pEmitter || pGravity) {
            SceneNode sn;
            sn.type = eSceneNodeParticles;
            sn.name = nodeName;
            sn.sourceNode = node;
            SceneNode::setIdentity(sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- DeepToPoints ---
        DeepToPoints* dtp = dynamic_cast<DeepToPoints*>(effect.get());
        if (dtp) {
            PointCloudDataPtr cloud = dtp->getPointCloud();
            if (!cloud || cloud->numPoints() == 0) continue;

            SceneNode sn;
            sn.type = eSceneNodePointCloud;
            sn.name = nodeName;
            sn.sourceNode = node;
            // Point clouds have identity transform (positions are in the data)
            SceneNode::setIdentity(sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // Project3D no longer has embedded cameras — its camera inputs
        // are separate Camera3DNode/ReadAlembicCamera nodes that get
        // their own SceneGraph entries automatically.
    }

    // ===== PASS 2: Link Group3D inputs to children =====

    for (int i = 0; i < (int)_nodes.size(); ++i) {
        if (_nodes[i].type != eSceneNodeGroup) continue;

        NodePtr node = _nodes[i].sourceNode.lock();
        if (!node) continue;

        Group3D* group = dynamic_cast<Group3D*>(node->getEffectInstance().get());
        if (!group) continue;

        for (int inp = 0; inp < GROUP3D_MAX_INPUTS; ++inp) {
            EffectInstancePtr inputEffect = group->getInput(inp);
            if (!inputEffect) continue;

            std::string inputName = inputEffect->getNode()->getScriptName();

            // Find all scene nodes from this input node
            for (int j = 0; j < (int)_nodes.size(); ++j) {
                if (j == i) continue;
                if (_nodes[j].name == inputName && _nodes[j].parentIndex == -1) {
                    _nodes[j].parentIndex = i;
                    _nodes[i].childIndices.push_back(j);
                }
            }
        }
    }

    // ===== Build root list (nodes with no parent) =====
    for (int i = 0; i < (int)_nodes.size(); ++i) {
        if (_nodes[i].parentIndex == -1) {
            _rootIndices.push_back(i);
        }
    }

    // ===== PASS 3: Compute world transforms =====
    computeWorldTransforms();
}

void
SceneGraph::computeWorldTransforms()
{
    // Depth-first from roots
    for (int i = 0; i < (int)_rootIndices.size(); ++i) {
        computeWorldRecursive(_rootIndices[i]);
    }
}

void
SceneGraph::computeWorldRecursive(int nodeIdx)
{
    SceneNode& node = _nodes[nodeIdx];

    if (node.parentIndex >= 0) {
        // world = parent.world * local
        multiply(_nodes[node.parentIndex].worldMatrix, node.localMatrix, node.worldMatrix);
    } else {
        // Root node: world = local
        std::memcpy(node.worldMatrix, node.localMatrix, 16 * sizeof(float));
    }

    for (int i = 0; i < (int)node.childIndices.size(); ++i) {
        computeWorldRecursive(node.childIndices[i]);
    }
}

NATRON_NAMESPACE_EXIT
