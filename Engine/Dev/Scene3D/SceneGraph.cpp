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
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>

#include "Camera3DNode.h"
#include "Card3D.h"
#include "Cube3D.h"
#include "Cylinder3D.h"
#include "../Deep/DeepToPoints.h"
#include "Light3D.h"
#include "../Particles/ParticleProvider.h"
#include "ReadVDB.h"
#include "Volume3D.h"
#include "Sphere3D.h"
#include "Group3D.h"
#include "../../Node.h"
#include "../Deep/PointCloudData.h"
#include "ReadAlembicCamera.h"
#include "ReadAlembicTransform.h"
#include "ReadAlembicArchive.h"
#include "ReadGeo.h"
#include "GeoMaterialOverride.h"
#include "../DotUtils.h"

#include <set>
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
    // Convention: extrinsic XYZ (M = Rz*Ry*Rx column-vector, applied to a vector
    // as v' = M*v means Rx hits first). Same as Maya/Blender/Houdini default.
    // ImGuizmo's matMul does standard A*B on row-major storage, so its
    // rot[0]*rot[1]*rot[2] = Rx_rv*Ry_rv*Rz_rv = Rz_col*Ry_col*Rx_col — i.e.,
    // extrinsic XYZ. We replicate the same multiplication so buildTRS and
    // ImGuizmo agree on the matrix, and glMultMatrixf(out) renders objects
    // in the orientation ImGuizmo::Manipulate expects — no boundary transpose.

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

void
SceneGraph::buildTRSPivot(float tx, float ty, float tz,
                          float rx, float ry, float rz,
                          float sx, float sy, float sz,
                          float px, float py, float pz,
                          float out[16])
{
    // out = T(t) · T(P) · R · S · T(-P). For a point x this gives
    // RS·x + (t + P − RS·P), i.e. the rotation/scale block is unchanged and the
    // translation column is offset so R/S pivot around P. (Column-major.)
    float rs[16];
    buildTRS(0, 0, 0, rx, ry, rz, sx, sy, sz, rs);
    const float rspx = rs[0] * px + rs[4] * py + rs[8]  * pz;
    const float rspy = rs[1] * px + rs[5] * py + rs[9]  * pz;
    const float rspz = rs[2] * px + rs[6] * py + rs[10] * pz;
    std::memcpy(out, rs, sizeof(float) * 16);
    out[12] = tx + px - rspx;
    out[13] = ty + py - rspy;
    out[14] = tz + pz - rspz;
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

    // Archive user transforms applied in WORLD space (post-multiplied onto each
    // entry's final world matrix) rather than as the innermost root parent. An
    // Alembic archive's embedded chain can carry a large unit scale (e.g. a 100×
    // cm→m conversion on the top node from an FBX import). If we put the user T/R/S
    // on the root as the parent matrix, this codebase's multiply convention scales
    // the root's *translation* by that child scale, so the geo moves 100× the knob
    // and the gizmo (which lives in un-amplified knob space) disconnects from it.
    // Applying the user transform outermost keeps translation 1:1 and rotation
    // pivoting around the geo centre, consistently for gizmo + drag + render.
    struct ArchiveWorldXform { int begin; int end; float W[16]; };
    std::vector<ArchiveWorldXform> archiveWorldXforms;

    // ===== Per-part material overrides (GeoMaterialOverride nodes) =====
    // Pre-scan all nodes for GeoMaterialOverride decorators. Each maps a set of
    // archive sub-object paths → a material node, scoped to a target geo source
    // (the archive reached by walking input 0 through Dots / chained overrides).
    // During the archive's per-entry emission below, an entry whose full path is
    // listed gets its SceneNode.materialNode set. Keyed by the target geo's
    // EffectInstance* so we can match it against the archive being emitted.
    struct MatOverrideRule {
        std::set<std::string> paths;
        NodeWPtr matNode;
    };
    std::map<EffectInstance*, std::vector<MatOverrideRule> > matOverrides;
    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
        NodePtr onode = *it;
        if (!onode || !onode->isActivated()) continue;
        // A disabled (bypassed) Material Override applies no material — the geo
        // still passes through (collectSceneNodes recurses its Geo input), it just
        // reverts to the source's base material.
        if (onode->isNodeDisabled()) continue;
        EffectInstancePtr oeff = onode->getEffectInstance();
        GeoMaterialOverride* ov = dynamic_cast<GeoMaterialOverride*>(oeff.get());
        if (!ov) continue;

        // Resolve the target geo: walk input 0 through Dots and chained overrides.
        EffectInstancePtr geo = ov->getInput(0);
        while (geo) {
            if (isGraphPassthrough(geo->getPluginID())) { geo = geo->getInput(0); continue; }
            if (dynamic_cast<GeoMaterialOverride*>(geo.get())) { geo = geo->getInput(0); continue; }
            break;
        }
        if (!geo) continue;

        EffectInstancePtr mat = skipDots(ov->getInput(1));
        NodePtr matNode = mat ? mat->getNode() : NodePtr();
        if (!matNode) continue;

        MatOverrideRule rule;
        ov->getSurfacePaths(rule.paths);
        if (rule.paths.empty()) continue;
        rule.matNode = matNode;
        matOverrides[geo.get()].push_back(rule);
    }

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
            sn.meshData = mesh; // Renderers can read this directly now.

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

            // Compose userTRS × mesh->transform so animated .abc xforms play
            // back in Cycles + 3D viewport (they previously dropped the
            // embedded Alembic transform entirely — only ScanlineRender saw
            // it via mesh->transform). The embedded matrix is row-major in
            // MeshData; convert to column-major before composing. Order
            // matches ScanlineRender's extractGeometry: parent-style userTRS
            // applied on top of the embedded animation.
            float userTRS[16];
            buildTRS(tx, ty, tz, rx, ry, rz, sx, sy, sz, userTRS);
            float embedded[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    embedded[c * 4 + r] = mesh->transform[r * 4 + c];
            // localMatrix = userTRS * embedded (4x4 column-major).
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    float s = 0.0f;
                    for (int k = 0; k < 4; ++k)
                        s += userTRS[k * 4 + r] * embedded[c * 4 + k];
                    sn.localMatrix[c * 4 + r] = s;
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

        // --- ReadAlembicArchive (multi-emit: one Natron node → many SceneNodes) ---
        ReadAlembicArchive* abcArchive = dynamic_cast<ReadAlembicArchive*>(effect.get());
        if (abcArchive) {
            const int count = abcArchive->getSceneNodeCount();
            // Rotation pivot. The archive-wide pivot drives the root handle; each
            // entry also gets its OWN pivot (its bbox centre in bbox mode) so clicking
            // a specific mesh in a multi-object archive pivots around THAT object
            // rather than the whole-archive centre.
            float archivePivot[3] = {0.f, 0.f, 0.f};
            std::vector<float> entryPivots;   // count*3, per-entry pivot (root-local)
            // The user T/R/S built as a WORLD-space transform, post-multiplied onto
            // each entry below (see archiveWorldXforms). Identity until filled.
            float archiveUserW[16];
            SceneNode::setIdentity(archiveUserW);

            // Always push a root SceneNode for the archive itself, even if it's
            // currently empty — gives the user something to see and link in the
            // node graph (and a target for parenting under a Group3D later).
            SceneNode root;
            root.type = eSceneNodeGroup;
            root.name = nodeName;
            root.sourceNode = node;
            // User transform on the archive root → propagates to every entry via
            // the worldMatrix chain, so it scales/moves the whole archive (the
            // T/R/S + Uniform Scale knobs on the node). Read by name so archives
            // saved before this feature (knobs absent) fall back to identity.
            {
                KnobIPtr kTX = effect->getKnobByName("translateX");
                KnobIPtr kTY = effect->getKnobByName("translateY");
                KnobIPtr kTZ = effect->getKnobByName("translateZ");
                KnobIPtr kRX = effect->getKnobByName("rotateX");
                KnobIPtr kRY = effect->getKnobByName("rotateY");
                KnobIPtr kRZ = effect->getKnobByName("rotateZ");
                KnobIPtr kSX = effect->getKnobByName("scaleX");
                KnobIPtr kSY = effect->getKnobByName("scaleY");
                KnobIPtr kSZ = effect->getKnobByName("scaleZ");
                KnobIPtr kUS = effect->getKnobByName("uniformScale");
                float tx = kTX ? (float)dynamic_cast<KnobDouble*>(kTX.get())->getValueAtTime(time) : 0.f;
                float ty = kTY ? (float)dynamic_cast<KnobDouble*>(kTY.get())->getValueAtTime(time) : 0.f;
                float tz = kTZ ? (float)dynamic_cast<KnobDouble*>(kTZ.get())->getValueAtTime(time) : 0.f;
                float rx = kRX ? (float)dynamic_cast<KnobDouble*>(kRX.get())->getValueAtTime(time) : 0.f;
                float ry = kRY ? (float)dynamic_cast<KnobDouble*>(kRY.get())->getValueAtTime(time) : 0.f;
                float rz = kRZ ? (float)dynamic_cast<KnobDouble*>(kRZ.get())->getValueAtTime(time) : 0.f;
                float sx = kSX ? (float)dynamic_cast<KnobDouble*>(kSX.get())->getValueAtTime(time) : 1.f;
                float sy = kSY ? (float)dynamic_cast<KnobDouble*>(kSY.get())->getValueAtTime(time) : 1.f;
                float sz = kSZ ? (float)dynamic_cast<KnobDouble*>(kSZ.get())->getValueAtTime(time) : 1.f;
                float us = kUS ? (float)dynamic_cast<KnobDouble*>(kUS.get())->getValueAtTime(time) : 1.f;

                // --- Rotation pivot. The user T/R/S on the archive root rotates/
                // scales around this point instead of (0,0,0): 0 = bounding-box centre
                // (default), 1 = authored origin (the archive's top transform
                // translation), 2 = world origin. Computed in the archive-root space the user
                // transform acts in, so the whole archive turns in place. ---
                float pv[3] = {0.f, 0.f, 0.f};
                int pivotMode = 0;
                if (KnobIPtr kPV = effect->getKnobByName("rotationPivot")) {
                    if (KnobChoice* pc = dynamic_cast<KnobChoice*>(kPV.get())) pivotMode = pc->getValue();
                }
                entryPivots.assign((size_t)std::max(0, count) * 3, 0.f);
                if (pivotMode != 2 && count > 0) {
                    std::vector<float> entryW((size_t)count * 16);
                    std::vector<char>  hasEntryBox((size_t)count, 0);
                    float bmn[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
                    float bmx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                    bool gotBox = false, gotAuth = false;
                    float auth[3] = {0.f, 0.f, 0.f};
                    for (int i = 0; i < count; ++i) {
                        std::string en; int pli = -1; bool im = false; float lm[16];
                        if (!abcArchive->getSceneNodeAt(i, time, en, pli, im, lm)) {
                            SceneNode::setIdentity(&entryW[(size_t)i * 16]);
                            continue;
                        }
                        if (pli >= 0 && pli < i) {
                            multiply(&entryW[(size_t)pli * 16], lm, &entryW[(size_t)i * 16]);
                        } else {
                            std::memcpy(&entryW[(size_t)i * 16], lm, sizeof(float) * 16);
                            if (!gotAuth) { auth[0] = lm[12]; auth[1] = lm[13]; auth[2] = lm[14]; gotAuth = true; }
                        }
                        if (pivotMode == 0 && im) {
                            MeshDataPtr md = abcArchive->getMeshDataAt(i, time);
                            if (md && md->numVertices > 0) {
                                const float* W = &entryW[(size_t)i * 16];
                                const std::vector<float>& vtx = md->vertices;
                                float emn[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
                                float emx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                                for (size_t v = 0; v + 2 < vtx.size(); v += 3) {
                                    const float x = vtx[v], y = vtx[v + 1], z = vtx[v + 2];
                                    const float wx = W[0]*x + W[4]*y + W[8]*z  + W[12];
                                    const float wy = W[1]*x + W[5]*y + W[9]*z  + W[13];
                                    const float wz = W[2]*x + W[6]*y + W[10]*z + W[14];
                                    emn[0]=std::min(emn[0],wx); emx[0]=std::max(emx[0],wx);
                                    emn[1]=std::min(emn[1],wy); emx[1]=std::max(emx[1],wy);
                                    emn[2]=std::min(emn[2],wz); emx[2]=std::max(emx[2],wz);
                                }
                                // This entry's own centre — used when the user clicks THIS mesh.
                                entryPivots[(size_t)i*3+0] = 0.5f*(emn[0]+emx[0]);
                                entryPivots[(size_t)i*3+1] = 0.5f*(emn[1]+emx[1]);
                                entryPivots[(size_t)i*3+2] = 0.5f*(emn[2]+emx[2]);
                                hasEntryBox[i] = 1;
                                // Fold into the whole-archive bbox (for the root handle).
                                bmn[0]=std::min(bmn[0],emn[0]); bmx[0]=std::max(bmx[0],emx[0]);
                                bmn[1]=std::min(bmn[1],emn[1]); bmx[1]=std::max(bmx[1],emx[1]);
                                bmn[2]=std::min(bmn[2],emn[2]); bmx[2]=std::max(bmx[2],emx[2]);
                                gotBox = true;
                            }
                        }
                    }
                    if (pivotMode == 1 && gotAuth) { pv[0] = auth[0]; pv[1] = auth[1]; pv[2] = auth[2]; }
                    if (pivotMode == 0 && gotBox) {
                        pv[0] = 0.5f * (bmn[0] + bmx[0]);
                        pv[1] = 0.5f * (bmn[1] + bmx[1]);
                        pv[2] = 0.5f * (bmn[2] + bmx[2]);
                    }
                    // Entries without their own box (locators / authored / world modes)
                    // fall back to the archive-wide pivot.
                    for (int i = 0; i < count; ++i) {
                        if (!hasEntryBox[i]) {
                            entryPivots[(size_t)i*3+0]=pv[0];
                            entryPivots[(size_t)i*3+1]=pv[1];
                            entryPivots[(size_t)i*3+2]=pv[2];
                        }
                    }
                }
                // Build the user transform as a WORLD-space matrix. It is NOT baked
                // into root.localMatrix (which stays identity) — instead it is
                // post-multiplied onto every entry's final world matrix after the
                // world-transform pass, so its translation is applied 1:1 in world
                // space (not scaled by the archive's embedded unit scale) and its
                // rotation/scale pivot around pv (the geo centre).
                buildTRSPivot(tx, ty, tz, rx, ry, rz, sx * us, sy * us, sz * us,
                              pv[0], pv[1], pv[2], archiveUserW);
                archivePivot[0] = pv[0]; archivePivot[1] = pv[1]; archivePivot[2] = pv[2];
                root.pivot[0] = pv[0]; root.pivot[1] = pv[1]; root.pivot[2] = pv[2];
            }
            const int rootSgIdx = (int)_nodes.size();
            nameToIndex[nodeName] = rootSgIdx;
            _nodes.push_back(root);

            // Append one SceneNode per visible archive entry. Because the archive
            // walker emits parents before children, an entry's parentLocalIndex
            // refers to an entry that's already been appended (or -1 for archive-root).
            const int baseSgIdx = rootSgIdx + 1;
            for (int i = 0; i < count; ++i) {
                std::string entryName;
                int parentLocalIdx = -1;
                bool isMesh = false;
                float lm[16];
                if (!abcArchive->getSceneNodeAt(i, time, entryName, parentLocalIdx, isMesh, lm)) {
                    continue;
                }
                SceneNode sn;
                if (isMesh) {
                    MeshDataPtr md = abcArchive->getMeshDataAt(i, time);
                    if (md && md->numVertices > 0) {
                        sn.type = eSceneNodeMesh;
                        sn.meshData = md;
                    } else {
                        // No mesh data (e.g. empty mesh) — fall back to locator.
                        sn.type = eSceneNodeTransform;
                    }
                } else {
                    sn.type = eSceneNodeTransform;
                }
                sn.name = nodeName + entryName; // e.g. "ReadAlembicArchive1/Camera01Trackers/Tracker1"
                sn.sourceNode = node;
                sn.archiveEntryIdx = i;  // remember the entry index for sub-time re-queries (Cycles motion blur)

                // Per-part material override: if any GeoMaterialOverride targets
                // this archive and lists this entry, tag it (later rules win on
                // overlap). A listed token matches entryName (the full path, e.g.
                // "/lamp/polySurface2/polySurfaceShape2") three ways, most to least
                // specific: exact full path; ancestor ("/lamp/polySurface2" covers
                // its descendant shapes); or bare leaf name ("polySurfaceShape2")
                // when the token has no slash — lenient so users can type just the
                // name shown in the archive tree.
                {
                    std::map<EffectInstance*, std::vector<MatOverrideRule> >::iterator mit =
                        matOverrides.find(effect.get());
                    if (mit != matOverrides.end()) {
                        // Leaf name of this entry (last path component).
                        const size_t lastSlash = entryName.find_last_of('/');
                        const std::string entryLeaf = (lastSlash == std::string::npos)
                            ? entryName : entryName.substr(lastSlash + 1);
                        for (size_t r = 0; r < mit->second.size(); ++r) {
                            const std::set<std::string>& paths = mit->second[r].paths;
                            bool match = (paths.count(entryName) > 0);
                            for (std::set<std::string>::const_iterator pit = paths.begin();
                                 !match && pit != paths.end(); ++pit) {
                                const std::string& p = *pit;
                                if (p.empty()) continue;
                                // Ancestor: entryName starts with "<p>/".
                                if (entryName.size() > p.size() &&
                                    entryName.compare(0, p.size(), p) == 0 &&
                                    entryName[p.size()] == '/') {
                                    match = true;
                                    break;
                                }
                                // Bare leaf name (no slash in the token).
                                if (p.find('/') == std::string::npos && p == entryLeaf) {
                                    match = true;
                                    break;
                                }
                            }
                            if (match) {
                                sn.materialNode = mit->second[r].matNode;
                            }
                        }
                    }
                }

                std::memcpy(sn.localMatrix, lm, sizeof(lm));
                // This entry's own rotation pivot (its bbox centre in bbox mode, else
                // the archive-wide pivot) so clicking this mesh pivots around it.
                if ((size_t)(i * 3 + 2) < entryPivots.size()) {
                    sn.pivot[0] = entryPivots[(size_t)i*3+0];
                    sn.pivot[1] = entryPivots[(size_t)i*3+1];
                    sn.pivot[2] = entryPivots[(size_t)i*3+2];
                }
                sn.parentIndex = (parentLocalIdx < 0) ? rootSgIdx : (baseSgIdx + parentLocalIdx);
                _nodes[sn.parentIndex].childIndices.push_back((int)_nodes.size());
                _nodes.push_back(sn);
            }
            // Record this archive's node range [rootSgIdx, end) + its world-space user
            // transform, to be post-multiplied after the world-transform pass.
            {
                ArchiveWorldXform ax;
                ax.begin = rootSgIdx;
                ax.end   = (int)_nodes.size();
                std::memcpy(ax.W, archiveUserW, sizeof(ax.W));
                archiveWorldXforms.push_back(ax);
            }
            continue;
        }

        // --- ReadAlembicTransform (null/locator) ---
        ReadAlembicTransform* abcXform = dynamic_cast<ReadAlembicTransform*>(effect.get());
        if (abcXform) {
            KnobIPtr k;
            float tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0, sx = 1, sy = 1, sz = 1;
            k = abcXform->getKnobByName("translateX"); if (k) tx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("translateY"); if (k) ty = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("translateZ"); if (k) tz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("rotateX"); if (k) rx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("rotateY"); if (k) ry = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("rotateZ"); if (k) rz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("scaleX"); if (k) sx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("scaleY"); if (k) sy = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            k = abcXform->getKnobByName("scaleZ"); if (k) sz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);

            SceneNode sn;
            sn.type = eSceneNodeTransform;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS(tx, ty, tz, rx, ry, rz, sx, sy, sz, sn.localMatrix);

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

            // Read the light's rotation knobs so its localMatrix carries the
            // orientation — without this the viewport drew every light
            // un-rotated (most visible on Spot/Area), even though the Cycles
            // renderer DID rotate them via buildLightTransform. Mirrors the
            // geometry / ReadVDB path so lights rotate in the viewport like geo.
            float lrx = 0, lry = 0, lrz = 0;
            {
                KnobIPtr k;
                k = effect->getKnobByName("rotateX"); if (k) lrx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                k = effect->getKnobByName("rotateY"); if (k) lry = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                k = effect->getKnobByName("rotateZ"); if (k) lrz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            }

            SceneNode sn;
            sn.type = eSceneNodeLight;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)ltx, (float)lty, (float)ltz, lrx, lry, lrz, 1, 1, 1, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- ReadVDB ---
        ReadVDB* readVdb = dynamic_cast<ReadVDB*>(effect.get());
        if (readVdb) {
            double tx, ty, tz, sx, sy, sz;
            readVdb->getTransform(time, tx, ty, tz, sx, sy, sz);

            // Read rotation from knobs
            float rx = 0, ry = 0, rz = 0;
            {
                KnobIPtr k;
                k = effect->getKnobByName("rotateX"); if (k) rx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                k = effect->getKnobByName("rotateY"); if (k) ry = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                k = effect->getKnobByName("rotateZ"); if (k) rz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            }

            SceneNode sn;
            sn.type = eSceneNodeVolume;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS((float)tx, (float)ty, (float)tz,
                     rx, ry, rz,
                     (float)sx, (float)sy, (float)sz, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Volume3D ---
        Volume3D* vol3d = dynamic_cast<Volume3D*>(effect.get());
        if (vol3d) {
            Volume3D::VolumeParams vp = vol3d->getVolumeParams(time);

            // Read rotation from knobs
            float rx = 0, ry = 0, rz = 0;
            {
                KnobIPtr k;
                k = effect->getKnobByName("rotateX"); if (k) rx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                k = effect->getKnobByName("rotateY"); if (k) ry = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                k = effect->getKnobByName("rotateZ"); if (k) rz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
            }

            SceneNode sn;
            sn.type = eSceneNodeVolume;
            sn.name = nodeName;
            sn.sourceNode = node;
            buildTRS(vp.centerX, vp.centerY, vp.centerZ,
                     rx, ry, rz,
                     vp.scaleX, vp.scaleY, vp.scaleZ, sn.localMatrix);

            nameToIndex[nodeName] = (int)_nodes.size();
            _nodes.push_back(sn);
            continue;
        }

        // --- Particle nodes (emitter, gravity, drag, etc.) ---
        ParticleProvider* pProvider = dynamic_cast<ParticleProvider*>(effect.get());
        if (pProvider) {
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

    // ===== Direct-disable pass: mark SceneNodes whose source node is "D"-disabled.
    // Ancestor propagation runs as part of the world-transform depth-first walk
    // below, so a disabled Group3D / Scene3D collapses everything under it. =====
    for (int i = 0; i < (int)_nodes.size(); ++i) {
        NodePtr src = _nodes[i].sourceNode.lock();
        if (src && src->isNodeDisabled()) {
            _nodes[i].visible = false;
        }
    }

    // ===== Build root list (nodes with no parent) =====
    for (int i = 0; i < (int)_nodes.size(); ++i) {
        if (_nodes[i].parentIndex == -1) {
            _rootIndices.push_back(i);
        }
    }

    // ===== PASS 3: Compute world transforms (also propagates visibility) =====
    computeWorldTransforms();

    // ===== PASS 4: Apply archive user transforms in WORLD space =====
    // For each Alembic archive, post-multiply its user T/R/S (built around the geo
    // pivot) onto every node in its range. Because multiply(world, userW) transforms
    // the entry's world translation by userW's linear part and then adds userW's
    // translation un-scaled, the archive moves 1:1 with the translate knob and
    // rotates/scales around the pivot in world space — regardless of any large unit
    // scale baked into the archive's embedded chain. (No-op when userW is identity.)
    for (size_t a = 0; a < archiveWorldXforms.size(); ++a) {
        const ArchiveWorldXform& ax = archiveWorldXforms[a];
        for (int i = ax.begin; i < ax.end && i < (int)_nodes.size(); ++i) {
            float w[16];
            multiply(_nodes[i].worldMatrix, ax.W, w);
            std::memcpy(_nodes[i].worldMatrix, w, sizeof(w));
        }
    }
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
        // Propagate disabled state from ancestor: a disabled Group3D / Scene3D
        // hides every node downstream of it.
        if (!_nodes[node.parentIndex].visible) {
            node.visible = false;
        }
    } else {
        // Root node: world = local
        std::memcpy(node.worldMatrix, node.localMatrix, 16 * sizeof(float));
    }

    for (int i = 0; i < (int)node.childIndices.size(); ++i) {
        computeWorldRecursive(node.childIndices[i]);
    }
}

NATRON_NAMESPACE_EXIT
