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
#include "../../KnobTypes.h"

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
    // Build TRS matrix matching ImGuizmo::RecomposeMatrixFromComponents exactly.
    // ImGuizmo uses Rodrigues rotation for each axis, then multiplies Rx*Ry*Rz
    // using FPU_MatrixF_x_MatrixF. We replicate that exact computation here
    // so buildTRS and RecomposeMatrixFromComponents produce identical m16 values.
    //
    // This means glMultMatrixf(out) renders objects in the same orientation
    // that ImGuizmo::Manipulate expects — no transposing needed at the boundary.

    // Build individual rotation matrices (same as ImGuizmo::RotationAxis)
    // Each is stored as m16[16] in ImGuizmo's layout
    auto rotAxis = [](float m[16], float axX, float axY, float axZ, float angleDeg) {
        float rad = angleDeg * (float)M_PI / 180.0f;
        float c = cosf(rad), s = sinf(rad), k = 1.0f - c;
        float xx = axX*axX*k+c,   xy = axX*axY*k,     zx = axZ*axX*k;
        float yy = axY*axY*k+c,   yz = axY*axZ*k,     zz = axZ*axZ*k+c;
        float xs = axX*s,         ys = axY*s,          zs = axZ*s;
        // Matches ImGuizmo::RotationAxis element layout exactly
        m[0]=xx;     m[1]=xy+zs;  m[2]=zx-ys;  m[3]=0;
        m[4]=xy-zs;  m[5]=yy;     m[6]=yz+xs;  m[7]=0;
        m[8]=zx+ys;  m[9]=yz-xs;  m[10]=zz;    m[11]=0;
        m[12]=0;     m[13]=0;     m[14]=0;      m[15]=1;
    };

    // Multiply matching ImGuizmo::FPU_MatrixF_x_MatrixF exactly
    auto matMul = [](const float a[16], const float b[16], float r[16]) {
        r[0]  = a[0]*b[0]  + a[1]*b[4]  + a[2]*b[8]   + a[3]*b[12];
        r[1]  = a[0]*b[1]  + a[1]*b[5]  + a[2]*b[9]   + a[3]*b[13];
        r[2]  = a[0]*b[2]  + a[1]*b[6]  + a[2]*b[10]  + a[3]*b[14];
        r[3]  = a[0]*b[3]  + a[1]*b[7]  + a[2]*b[11]  + a[3]*b[15];
        r[4]  = a[4]*b[0]  + a[5]*b[4]  + a[6]*b[8]   + a[7]*b[12];
        r[5]  = a[4]*b[1]  + a[5]*b[5]  + a[6]*b[9]   + a[7]*b[13];
        r[6]  = a[4]*b[2]  + a[5]*b[6]  + a[6]*b[10]  + a[7]*b[14];
        r[7]  = a[4]*b[3]  + a[5]*b[7]  + a[6]*b[11]  + a[7]*b[15];
        r[8]  = a[8]*b[0]  + a[9]*b[4]  + a[10]*b[8]  + a[11]*b[12];
        r[9]  = a[8]*b[1]  + a[9]*b[5]  + a[10]*b[9]  + a[11]*b[13];
        r[10] = a[8]*b[2]  + a[9]*b[6]  + a[10]*b[10] + a[11]*b[14];
        r[11] = a[8]*b[3]  + a[9]*b[7]  + a[10]*b[11] + a[11]*b[15];
        r[12] = a[12]*b[0] + a[13]*b[4] + a[14]*b[8]  + a[15]*b[12];
        r[13] = a[12]*b[1] + a[13]*b[5] + a[14]*b[9]  + a[15]*b[13];
        r[14] = a[12]*b[2] + a[13]*b[6] + a[14]*b[10] + a[15]*b[14];
        r[15] = a[12]*b[3] + a[13]*b[7] + a[14]*b[11] + a[15]*b[15];
    };

    float rotX[16], rotY[16], rotZ[16], tmp[16];
    rotAxis(rotX, 1, 0, 0, rx);
    rotAxis(rotY, 0, 1, 0, ry);
    rotAxis(rotZ, 0, 0, 1, rz);

    // mat = rotX * rotY * rotZ (same order as ImGuizmo)
    matMul(rotX, rotY, tmp);
    matMul(tmp, rotZ, out);

    // Apply scale to right/up/dir vectors (same as ImGuizmo)
    // right = out[0..3], up = out[4..7], dir = out[8..11]
    out[0] *= sx; out[1] *= sx; out[2] *= sx; out[3] *= sx;
    out[4] *= sy; out[5] *= sy; out[6] *= sy; out[7] *= sy;
    out[8] *= sz; out[9] *= sz; out[10] *= sz; out[11] *= sz;

    // Set translation
    out[12] = tx; out[13] = ty; out[14] = tz; out[15] = 1.0f;
}

void
SceneGraph::multiply(const float a[16], const float b[16], float out[16])
{
    // Matrix multiply matching ImGuizmo::FPU_MatrixF_x_MatrixF exactly
    // out = a * b
    float r[16];
    r[0]  = a[0]*b[0]  + a[1]*b[4]  + a[2]*b[8]   + a[3]*b[12];
    r[1]  = a[0]*b[1]  + a[1]*b[5]  + a[2]*b[9]   + a[3]*b[13];
    r[2]  = a[0]*b[2]  + a[1]*b[6]  + a[2]*b[10]  + a[3]*b[14];
    r[3]  = a[0]*b[3]  + a[1]*b[7]  + a[2]*b[11]  + a[3]*b[15];
    r[4]  = a[4]*b[0]  + a[5]*b[4]  + a[6]*b[8]   + a[7]*b[12];
    r[5]  = a[4]*b[1]  + a[5]*b[5]  + a[6]*b[9]   + a[7]*b[13];
    r[6]  = a[4]*b[2]  + a[5]*b[6]  + a[6]*b[10]  + a[7]*b[14];
    r[7]  = a[4]*b[3]  + a[5]*b[7]  + a[6]*b[11]  + a[7]*b[15];
    r[8]  = a[8]*b[0]  + a[9]*b[4]  + a[10]*b[8]  + a[11]*b[12];
    r[9]  = a[8]*b[1]  + a[9]*b[5]  + a[10]*b[9]  + a[11]*b[13];
    r[10] = a[8]*b[2]  + a[9]*b[6]  + a[10]*b[10] + a[11]*b[14];
    r[11] = a[8]*b[3]  + a[9]*b[7]  + a[10]*b[11] + a[11]*b[15];
    r[12] = a[12]*b[0] + a[13]*b[4] + a[14]*b[8]  + a[15]*b[12];
    r[13] = a[12]*b[1] + a[13]*b[5] + a[14]*b[9]  + a[15]*b[13];
    r[14] = a[12]*b[2] + a[13]*b[6] + a[14]*b[10] + a[15]*b[14];
    r[15] = a[12]*b[3] + a[13]*b[7] + a[14]*b[11] + a[15]*b[15];
    std::memcpy(out, r, sizeof(r));
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

            // Use knob-based T/R/S (same as Sphere3D, Card3D, etc.)
            KnobIPtr kTX = effect->getKnobByName("translateX");
            KnobIPtr kTY = effect->getKnobByName("translateY");
            KnobIPtr kTZ = effect->getKnobByName("translateZ");
            KnobIPtr kRX = effect->getKnobByName("rotateX");
            KnobIPtr kRY = effect->getKnobByName("rotateY");
            KnobIPtr kRZ = effect->getKnobByName("rotateZ");
            KnobIPtr kSX = effect->getKnobByName("scaleX");
            KnobIPtr kSY = effect->getKnobByName("scaleY");
            KnobIPtr kSZ = effect->getKnobByName("scaleZ");
            float tx = kTX ? (float)dynamic_cast<KnobDouble*>(kTX.get())->getValueAtTime(time) : 0;
            float ty = kTY ? (float)dynamic_cast<KnobDouble*>(kTY.get())->getValueAtTime(time) : 0;
            float tz = kTZ ? (float)dynamic_cast<KnobDouble*>(kTZ.get())->getValueAtTime(time) : 0;
            float rx = kRX ? (float)dynamic_cast<KnobDouble*>(kRX.get())->getValueAtTime(time) : 0;
            float ry = kRY ? (float)dynamic_cast<KnobDouble*>(kRY.get())->getValueAtTime(time) : 0;
            float rz = kRZ ? (float)dynamic_cast<KnobDouble*>(kRZ.get())->getValueAtTime(time) : 0;
            float sx = kSX ? (float)dynamic_cast<KnobDouble*>(kSX.get())->getValueAtTime(time) : 1;
            float sy = kSY ? (float)dynamic_cast<KnobDouble*>(kSY.get())->getValueAtTime(time) : 1;
            float sz = kSZ ? (float)dynamic_cast<KnobDouble*>(kSZ.get())->getValueAtTime(time) : 1;
            buildTRS(tx, ty, tz, rx, ry, rz, sx, sy, sz, sn.localMatrix);

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
            double lexp_unused;
            light3d->getLightParams(time, ltx, lty, ltz, lr, lg, lb, lint, lexp_unused);

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
