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

#include "Viewport3D.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QMenu>
#include <QAction>
#include <QContextMenuEvent>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "Engine/AppInstance.h"
#include "Engine/Dev/Scene3D/CameraProvider.h"
#include "Engine/Dev/Scene3D/Card3D.h"
#include "Engine/Dev/Particles/ParticleProvider.h"
#include "Engine/Dev/Particles/ParticleInstance.h"
#include "Engine/Dev/Particles/ParticleSolver.h"
#include "Engine/Dev/Scene3D/ReadVDB.h"
#include "Engine/Dev/Scene3D/Volume3D.h"
#include "Engine/Dev/Scene3D/Cube3D.h"
#include "Engine/Dev/Scene3D/Cylinder3D.h"
#include "Engine/Dev/Scene3D/ReadAlembicCamera.h"
#include "Engine/Dev/Scene3D/ReadAlembicTransform.h"
#include "Engine/Dev/Scene3D/CameraMath.h"
#include "Engine/Dev/Scene3D/RotationConventions.h"
#include "Engine/Dev/Scene3D/Camera3DNode.h"
#include "Engine/Dev/Scene3D/Sphere3D.h"
#include "Engine/Dev/Scene3D/UVProject.h"
#include "Engine/Dev/DotUtils.h"
#include "Engine/Dev/Scene3D/Group3D.h"
#include "Engine/Dev/Scene3D/ReadGeo.h"
#include "Engine/Dev/Scene3D/Material3D.h"
#include "Engine/Dev/Scene3D/MaterialProvider.h"
#include "Engine/Dev/Scene3D/Project3D.h"
#include "Engine/Dev/Scene3D/MergeMat.h"
#include "Engine/Dev/Scene3D/Light3D.h"
#include "Engine/TimeLine.h"
#include "Engine/Dev/Deep/DeepToPoints.h"
#include "Engine/Dev/Deep/Blast.h"
#include "Engine/Dev/Deep/PointCloudProvider.h"
#include "Engine/Dev/Scene3D/Light3D.h"
#include "Engine/Dev/Scene3D/SceneGraph.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeGraph.h"
#include <QUndoStack>

// ImGui + ImGuizmo (MIT license)
#include "imgui.h"
#include "ImGuizmo.h"


// ============================================================================
// Section 1: Demo math functions — copied VERBATIM from ImGuizmo/example/main.cpp
// ============================================================================

static void Cross(const float* a, const float* b, float* r)
{
   r[0] = a[1] * b[2] - a[2] * b[1];
   r[1] = a[2] * b[0] - a[0] * b[2];
   r[2] = a[0] * b[1] - a[1] * b[0];
}

static float Dot(const float* a, const float* b)
{
   return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void Normalize(const float* a, float* r)
{
   float il = 1.f / (sqrtf(Dot(a, a)) + FLT_EPSILON);
   r[0] = a[0] * il;
   r[1] = a[1] * il;
   r[2] = a[2] * il;
}

// Per-face flat-shading factor for the viewport's eShaded / eShadedWire modes.
// Computes ambient + N.L using a fixed eye-space light direction (roughly
// top-right-behind-camera, same as Maya's default headlight).
//
// Two flavors:
//   ViewportLitFromVertexNormals — preferred when the geometry carries
//     per-vertex normals (Sphere3D / Card3D / Cube3D / Cylinder3D). The
//     averaged local normal is transformed into eye space via the upper-left
//     3x3 of the modelview matrix. Winding-agnostic since we use the supplied
//     normals directly, not a cross product.
//   ViewportLitFromVerts — fallback for ReadGeo / Alembic where we don't have
//     vertex normals. Cross product of (b-a) x (c-a) gives the face normal
//     assuming CCW-from-outside winding (the Maya/Houdini/Blender default
//     that exporters typically produce).
static const float kViewportLightDirEye[3] = { 0.4082482f, 0.5715476f, 0.7113249f };
static const float kViewportAmbient = 0.15f;

static float
ViewportLitFromEyeNormal(const float* nEye)
{
    float nn[3];
    Normalize(nEye, nn);
    float nl = Dot(nn, kViewportLightDirEye);
    if (nl < 0.0f) nl = 0.0f;
    return kViewportAmbient + nl * (1.0f - kViewportAmbient);
}

static float
ViewportLitFromVertexNormals(const float* n0, const float* n1, const float* n2,
                             const float mv[16])
{
    const float nAvg[3] = {
        (n0[0] + n1[0] + n2[0]) * (1.0f / 3.0f),
        (n0[1] + n1[1] + n2[1]) * (1.0f / 3.0f),
        (n0[2] + n1[2] + n2[2]) * (1.0f / 3.0f),
    };
    const float nEye[3] = {
        mv[0] * nAvg[0] + mv[4] * nAvg[1] + mv[8]  * nAvg[2],
        mv[1] * nAvg[0] + mv[5] * nAvg[1] + mv[9]  * nAvg[2],
        mv[2] * nAvg[0] + mv[6] * nAvg[1] + mv[10] * nAvg[2],
    };
    return ViewportLitFromEyeNormal(nEye);
}

static float
ViewportFaceLitFactor(const float* a, const float* b, const float* c, const float mv[16])
{
    const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float nLocal[3];
    Cross(e1, e2, nLocal);
    const float nEye[3] = {
        mv[0] * nLocal[0] + mv[4] * nLocal[1] + mv[8]  * nLocal[2],
        mv[1] * nLocal[0] + mv[5] * nLocal[1] + mv[9]  * nLocal[2],
        mv[2] * nLocal[0] + mv[6] * nLocal[1] + mv[10] * nLocal[2],
    };
    // Two-sided lighting via abs(N.L). ReadGeo / Alembic meshes have
    // unpredictable winding (Maya CCW, some tools CW, triangulated soups
    // can be mixed), so we can't tell which side is "outside" from the
    // cross product alone. Using abs lights both directions equally; the
    // depth test hides back faces on closed meshes anyway, so visually this
    // matches a correctly-wound mesh. Procedural primitives (Sphere / Card /
    // Cube / Cyl) keep their per-vertex normal path (ViewportLitFromVertexNormals)
    // which knows the outward direction.
    float nn[3];
    Normalize(nEye, nn);
    float nl = Dot(nn, kViewportLightDirEye);
    if (nl < 0.0f) nl = -nl;
    return kViewportAmbient + nl * (1.0f - kViewportAmbient);
}

static void Frustum(float left, float right, float bottom, float top, float znear, float zfar, float* m16)
{
   float temp, temp2, temp3, temp4;
   temp = 2.0f * znear;
   temp2 = right - left;
   temp3 = top - bottom;
   temp4 = zfar - znear;
   m16[0] = temp / temp2;
   m16[1] = 0.0;
   m16[2] = 0.0;
   m16[3] = 0.0;
   m16[4] = 0.0;
   m16[5] = temp / temp3;
   m16[6] = 0.0;
   m16[7] = 0.0;
   m16[8] = (right + left) / temp2;
   m16[9] = (top + bottom) / temp3;
   m16[10] = (-zfar - znear) / temp4;
   m16[11] = -1.0f;
   m16[12] = 0.0;
   m16[13] = 0.0;
   m16[14] = (-temp * zfar) / temp4;
   m16[15] = 0.0;
}

static void Perspective(float fovyInDegrees, float aspectRatio, float znear, float zfar, float* m16)
{
   float ymax, xmax;
   ymax = znear * tanf(fovyInDegrees * 3.141592f / 180.0f);
   xmax = ymax * aspectRatio;
   Frustum(-xmax, xmax, -ymax, ymax, znear, zfar, m16);
}

static void LookAt(const float* eye, const float* at, const float* up, float* m16)
{
   float X[3], Y[3], Z[3], tmp[3];

   tmp[0] = eye[0] - at[0];
   tmp[1] = eye[1] - at[1];
   tmp[2] = eye[2] - at[2];
   Normalize(tmp, Z);
   Normalize(up, Y);

   Cross(Y, Z, tmp);
   Normalize(tmp, X);

   Cross(Z, X, tmp);
   Normalize(tmp, Y);

   m16[0] = X[0];
   m16[1] = Y[0];
   m16[2] = Z[0];
   m16[3] = 0.0f;
   m16[4] = X[1];
   m16[5] = Y[1];
   m16[6] = Z[1];
   m16[7] = 0.0f;
   m16[8] = X[2];
   m16[9] = Y[2];
   m16[10] = Z[2];
   m16[11] = 0.0f;
   m16[12] = -Dot(X, eye);
   m16[13] = -Dot(Y, eye);
   m16[14] = -Dot(Z, eye);
   m16[15] = 1.0f;
}

// Matrix multiply — copied from ImGuizmo.cpp line 78
static void FPU_MatrixF_x_MatrixF(const float* a, const float* b, float* r)
{
   r[0] = a[0] * b[0] + a[1] * b[4] + a[2] * b[8] + a[3] * b[12];
   r[1] = a[0] * b[1] + a[1] * b[5] + a[2] * b[9] + a[3] * b[13];
   r[2] = a[0] * b[2] + a[1] * b[6] + a[2] * b[10] + a[3] * b[14];
   r[3] = a[0] * b[3] + a[1] * b[7] + a[2] * b[11] + a[3] * b[15];

   r[4] = a[4] * b[0] + a[5] * b[4] + a[6] * b[8] + a[7] * b[12];
   r[5] = a[4] * b[1] + a[5] * b[5] + a[6] * b[9] + a[7] * b[13];
   r[6] = a[4] * b[2] + a[5] * b[6] + a[6] * b[10] + a[7] * b[14];
   r[7] = a[4] * b[3] + a[5] * b[7] + a[6] * b[11] + a[7] * b[15];

   r[8] = a[8] * b[0] + a[9] * b[4] + a[10] * b[8] + a[11] * b[12];
   r[9] = a[8] * b[1] + a[9] * b[5] + a[10] * b[9] + a[11] * b[13];
   r[10] = a[8] * b[2] + a[9] * b[6] + a[10] * b[10] + a[11] * b[14];
   r[11] = a[8] * b[3] + a[9] * b[7] + a[10] * b[11] + a[11] * b[15];

   r[12] = a[12] * b[0] + a[13] * b[4] + a[14] * b[8] + a[15] * b[12];
   r[13] = a[12] * b[1] + a[13] * b[5] + a[14] * b[9] + a[15] * b[13];
   r[14] = a[12] * b[2] + a[13] * b[6] + a[14] * b[10] + a[15] * b[14];
   r[15] = a[12] * b[3] + a[13] * b[7] + a[14] * b[11] + a[15] * b[15];
}


// ============================================================================
// Section 2: Minimal ImGui OpenGL2 renderer — copied from existing Viewport3D.cpp
// ============================================================================

static GLuint g_DevImGuiFontTexture = 0;

static void ImGui_ImplOpenGL2_CreateFontsTexture()
{
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    glGenTextures(1, &g_DevImGuiFontTexture);
    glBindTexture(GL_TEXTURE_2D, g_DevImGuiFontTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    io.Fonts->TexID = (ImTextureID)(intptr_t)g_DevImGuiFontTexture;
}

static void ImGui_ImplOpenGL2_RenderDrawData(ImDrawData* drawData)
{
    int fbW = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    int fbH = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (fbW <= 0 || fbH <= 0) return;

    // Save GL state
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    // Ortho projection
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(drawData->DisplayPos.x, drawData->DisplayPos.x + drawData->DisplaySize.x,
            drawData->DisplayPos.y + drawData->DisplaySize.y, drawData->DisplayPos.y,
            -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    ImVec2 clipOff = drawData->DisplayPos;
    ImVec2 clipScale = drawData->FramebufferScale;

    for (int n = 0; n < drawData->CmdListsCount; n++) {
        const ImDrawList* cmdList = drawData->CmdLists[n];
        const ImDrawVert* vtxBuffer = cmdList->VtxBuffer.Data;
        const ImDrawIdx* idxBuffer = cmdList->IdxBuffer.Data;

        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtxBuffer + offsetof(ImDrawVert, pos)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtxBuffer + offsetof(ImDrawVert, uv)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtxBuffer + offsetof(ImDrawVert, col)));

        for (int cmdI = 0; cmdI < cmdList->CmdBuffer.Size; cmdI++) {
            const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmdI];
            ImVec2 clipMin((pcmd->ClipRect.x - clipOff.x) * clipScale.x, (pcmd->ClipRect.y - clipOff.y) * clipScale.y);
            ImVec2 clipMax((pcmd->ClipRect.z - clipOff.x) * clipScale.x, (pcmd->ClipRect.w - clipOff.y) * clipScale.y);
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) continue;
            glScissor((int)clipMin.x, (int)((float)fbH - clipMax.y), (int)(clipMax.x - clipMin.x), (int)(clipMax.y - clipMin.y));
            glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->GetTexID());
            glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
                           sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                           (const GLvoid*)(idxBuffer + pcmd->IdxOffset));
        }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glPopAttrib();
}


// Custom undo command for gizmo transform changes
class GizmoTransformUndoCommand : public QUndoCommand
{
public:
    GizmoTransformUndoCommand(const NATRON_NAMESPACE::EffectInstancePtr& effect,
                               float oldT[3], float oldR[3], float oldS[3],
                               float newT[3], float newR[3], float newS[3])
        : QUndoCommand(QString::fromUtf8("Gizmo Transform"))
        , _effect(effect)
    {
        for (int i = 0; i < 3; ++i) {
            _oldT[i] = oldT[i]; _oldR[i] = oldR[i]; _oldS[i] = oldS[i];
            _newT[i] = newT[i]; _newR[i] = newR[i]; _newS[i] = newS[i];
        }
    }

    void undo() override { apply(_oldT, _oldR, _oldS); }
    void redo() override { apply(_newT, _newR, _newS); }

private:
    void apply(float t[3], float r[3], float s[3]) {
        if (!_effect) return;
        const char* tNames[3] = {"translateX", "translateY", "translateZ"};
        const char* rNames[3] = {"rotateX", "rotateY", "rotateZ"};
        const char* sNames[3] = {"scaleX", "scaleY", "scaleZ"};
        for (int i = 0; i < 3; ++i) {
            NATRON_NAMESPACE::KnobIPtr k;
            k = _effect->getKnobByName(tNames[i]);
            if (k) dynamic_cast<NATRON_NAMESPACE::KnobDouble*>(k.get())->setValue(t[i]);
            k = _effect->getKnobByName(rNames[i]);
            if (k) dynamic_cast<NATRON_NAMESPACE::KnobDouble*>(k.get())->setValue(r[i]);
            k = _effect->getKnobByName(sNames[i]);
            if (k) dynamic_cast<NATRON_NAMESPACE::KnobDouble*>(k.get())->setValue(s[i]);
        }
    }

    NATRON_NAMESPACE::EffectInstancePtr _effect;
    float _oldT[3], _oldR[3], _oldS[3];
    float _newT[3], _newR[3], _newS[3];
};

NATRON_NAMESPACE_ENTER

// Upload a scene-linear RGBA preview texture, applying an approximate display
// transform (sRGB / Rec.709 OETF on RGB) so geo textures in the 3D view match the
// 2D viewer instead of reading dark. The cached textures are scene-linear (rendered
// from the image pipeline); without this the midtones look much darker than the
// display-transformed 2D viewer. Reuses a scratch buffer (paintGL is single-thread).
// borderColor (RGBA, optional): when either wrap mode is GL_CLAMP_TO_BORDER, texels
// sampled outside [0,1] return this colour. Used by Project3D "crop to frame": the
// plate maps to [0,1], so outside the camera frustum we return an opaque base-grey
// instead of smearing the plate's edge pixels — the geo keeps its normal shading there.
static void
uploadPreviewTextureSRGB(const float* pixels, int w, int h, GLint wrapS, GLint wrapT,
                         const float* borderColor = NULL)
{
    static std::vector<float> tmp;
    const size_t n = (size_t)w * (size_t)h;
    tmp.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = pixels[i * 4 + c];
            if (v <= 0.0f) { tmp[i * 4 + c] = 0.0f; continue; }
            tmp[i * 4 + c] = (v <= 0.0031308f) ? (v * 12.92f)
                                               : (1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f);
        }
        tmp[i * 4 + 3] = pixels[i * 4 + 3]; // alpha unchanged
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
    if ( borderColor && (wrapS == GL_CLAMP_TO_BORDER || wrapT == GL_CLAMP_TO_BORDER) ) {
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_FLOAT, tmp.data());
}

// Find a UVProject whose geo input (through Dots) resolves to `geoNode`, i.e. a
// "geo -> UVProject" chain. Used so the viewport can display the projected texture
// (UVProject input 2) with rewritten UVs on the geo, matching ScanlineRender.
static UVProject*
findUVProjectForGeo(GuiAppInstance* app, const NodePtr& geoNode)
{
    if (!app || !geoNode) return NULL;
    ProjectPtr proj = app->getProject();
    if (!proj) return NULL;
    NodesList nodes;
    proj->getNodes_recursive(nodes, true);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!*it || !(*it)->isActivated()) continue;
        UVProject* uvp = dynamic_cast<UVProject*>( (*it)->getEffectInstance().get() );
        if (!uvp) continue;
        EffectInstancePtr geo = skipDots(uvp->getGeoInput());
        if (geo && geo->getNode() == geoNode) return uvp;
    }
    return NULL;
}

// Borders for the Project3D viewport projection, sampled outside the plate frame [0,1] (and
// on culled back-faces in front/back mode, where the STW is pushed out of range):
//   * CROP ON  → fully transparent: the existing alpha blend makes those fragments contribute
//     nothing, so the geo is cropped away there (background/grid shows through), matching Nuke.
//   * CROP OFF (project-on cull only) → opaque base-grey: the geo stays visible, just unprojected.
static const float kProject3DBorderClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static const float kProject3DBorderGrey[4]  = { 0.45f, 0.45f, 0.45f, 1.0f };

// True if vertex `idx`'s Project3D projection lands inside the plate frame [0,1] (and in front
// of the projector). projSTW holds (s*w, t*w, w) per vertex; project-on-culled verts were
// pushed to a (2,2,1) out-of-range value upstream, so this also rejects them. Used to crop the
// wireframe to the projected region (only meaningful when Crop is on — see callers).
static inline bool
project3DVertVisible(const std::vector<float>& projSTW, int idx)
{
    if ( (idx * 3 + 2) >= (int)projSTW.size() ) return false;
    const float w = projSTW[idx * 3 + 2];
    if (w <= 0.f) return false;
    const float u = projSTW[idx * 3 + 0] / w;
    const float v = projSTW[idx * 3 + 1] / w;
    return (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f);
}

// Compute smooth per-vertex normals for a polygon-soup mesh (vertices x,y,z interleaved;
// faceCounts = per-face vertex count, or empty = faceIndices are triangles). Newell's method
// per face (robust for n-gons / non-planar), accumulated to each vertex, then normalized.
// MeshData carries no normals, so this is built on demand only when Project3D "Project On"
// front/back needs facing info for a mesh. Winding-dependent (relies on consistent outward
// winding, as Alembic/OBJ normally have).
static void
computeMeshVertexNormals(const std::vector<float>& verts,
                         const std::vector<int>& faceCounts,
                         const std::vector<int>& faceIndices,
                         std::vector<float>& outNormals)
{
    const int nv = (int)(verts.size() / 3);
    outNormals.assign((size_t)nv * 3, 0.f);
    auto addFace = [&](const int* ids, int c) {
        float nx = 0.f, ny = 0.f, nz = 0.f;
        for (int i = 0; i < c; ++i) {
            const int a = ids[i], b = ids[(i + 1) % c];
            if (a < 0 || a >= nv || b < 0 || b >= nv) return;
            const float* pa = &verts[a * 3];
            const float* pb = &verts[b * 3];
            nx += (pa[1] - pb[1]) * (pa[2] + pb[2]);
            ny += (pa[2] - pb[2]) * (pa[0] + pb[0]);
            nz += (pa[0] - pb[0]) * (pa[1] + pb[1]);
        }
        for (int i = 0; i < c; ++i) {
            const int a = ids[i];
            outNormals[a * 3 + 0] += nx;
            outNormals[a * 3 + 1] += ny;
            outNormals[a * 3 + 2] += nz;
        }
    };
    if (!faceCounts.empty()) {
        size_t off = 0;
        for (size_t f = 0; f < faceCounts.size(); ++f) {
            const int c = faceCounts[f];
            if (c >= 3 && off + (size_t)c <= faceIndices.size())
                addFace(&faceIndices[off], c);
            off += (size_t)std::max(0, c);
        }
    } else {
        for (size_t t = 0; t + 2 < faceIndices.size(); t += 3)
            addFace(&faceIndices[t], 3);
    }
    for (int i = 0; i < nv; ++i) {
        float* n = &outNormals[i * 3];
        const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    }
}

// Resolve the Project3D previewed in the 3D viewport from a geo's material. A Project3D
// projects directly; a MergeMat resolves to its FOREGROUND (A, then B) Project3D — the
// fixed-function viewport can't composite layers like ScanlineRender, so it previews the
// top projection with full settings (crop/alpha/project-on) instead of breaking. The full
// layered composite is shown in the render; a multi-pass viewport preview is a future step.
static Project3D*
resolveViewportProject3D(MaterialProvider* mat)
{
    if (!mat) return NULL;
    if (Project3D* p = dynamic_cast<Project3D*>(mat)) return p;
    if (MergeMat* m = dynamic_cast<MergeMat*>(mat)) {
        if (Project3D* a = resolveViewportProject3D(m->getInputMaterial(0))) return a;  // foreground
        return resolveViewportProject3D(m->getInputMaterial(1));                         // background
    }
    return NULL;
}

// One viewport projection layer (a single Project3D leaf). A geo carries 1 (lone Project3D)
// or several (MergeMat tree); the geo draw paints them back-to-front with alpha blending so
// they composite, mirroring the ScanlineRender shader's per-fragment compositing.
struct Project3DLayer {
    const float* texPixels;          // plate pixels (in the Project3D's CachedTexture — kept alive by it)
    int texW, texH;
    std::vector<float> projSTW;      // per-vertex (s*w, t*w, w)
    const float* borderColor;        // clear (crop) / grey (project-on cull, no crop) / NULL (smear)
    std::vector<unsigned char> vertCulled;  // 1 = facing-culled vertex (per-triangle cull in the draw)
    int op;                          // MergeMat::Operation compositing this over the layers below
    float mix;                       // 0..1 opacity
    Project3DLayer() : texPixels(NULL), texW(0), texH(0), borderColor(NULL), op(2 /*over*/), mix(1.f) {}
};

// Compute one projection layer for an explicit Project3D over `localXYZ` (geo-local, xyz
// interleaved). `localNormals` (parallel; may be empty) drives Project-On front/back culling
// (culled verts get STW pushed out of [0,1] + a per-vertex cull flag). Returns false if the
// projection is inactive (no plate / no camera). `op`/`mix` are the MergeMat compositing params.
static bool
computeProject3DLayer(Project3D* p3d,
                      const std::vector<float>& localXYZ, const std::vector<float>& localNormals,
                      const float worldMatrix[16], double time, int op, float mix, Project3DLayer& out)
{
    if (!p3d) return false;
    p3d->updateCachedTexture(time);
    const Project3D::CachedTexture& pt = p3d->getCachedTexture();
    if (pt.width <= 0 || pt.height <= 0 || pt.pixels.empty()) return false;
    float vp[16];
    if (!p3d->getProjectorViewProj(time, vp)) return false;
    float mvp[16];   // mvp = projVP * worldMatrix (column-major)
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += vp[k*4+r] * worldMatrix[c*4+k];
            mvp[c*4+r] = s;
        }
    const size_t n = localXYZ.size() / 3;
    out.projSTW.resize(n * 3);
    for (size_t i = 0; i < n; ++i) {
        const float lx = localXYZ[i*3], ly = localXYZ[i*3+1], lz = localXYZ[i*3+2];
        const float cx = mvp[0]*lx + mvp[4]*ly + mvp[8]*lz  + mvp[12];
        const float cy = mvp[1]*lx + mvp[5]*ly + mvp[9]*lz  + mvp[13];
        const float cw = mvp[3]*lx + mvp[7]*ly + mvp[11]*lz + mvp[15];
        out.projSTW[i*3+0] = (cx + cw) * 0.5f;
        out.projSTW[i*3+1] = (cy + cw) * 0.5f;
        out.projSTW[i*3+2] = cw;
    }
    out.texPixels = pt.pixels.data(); out.texW = pt.width; out.texH = pt.height;
    out.op = op; out.mix = mix;
    const bool crop = p3d->getCropToFrame(time);
    out.borderColor = crop ? kProject3DBorderClear : NULL;
    out.vertCulled.clear();

    const int onMode = p3d->getProjectOn(time);   // 0 front, 1 back, 2 both
    if (onMode != Project3D::eProjectBoth && localNormals.size() == localXYZ.size()) {
        double ptx, pty, ptz, prx, pry, prz, pf, pha, pva;
        if (p3d->getProjectorCamera(time, ptx, pty, ptz, prx, pry, prz, pf, pha, pva)) {
            const float projPos[3] = { (float)ptx, (float)pty, (float)ptz };
            out.vertCulled.assign(n, 0);
            for (size_t i = 0; i < n; ++i) {
                const float* pL = &localXYZ[i*3];
                const float* nL = &localNormals[i*3];
                const float wx = worldMatrix[0]*pL[0]+worldMatrix[4]*pL[1]+worldMatrix[8]*pL[2] +worldMatrix[12];
                const float wy = worldMatrix[1]*pL[0]+worldMatrix[5]*pL[1]+worldMatrix[9]*pL[2] +worldMatrix[13];
                const float wz = worldMatrix[2]*pL[0]+worldMatrix[6]*pL[1]+worldMatrix[10]*pL[2]+worldMatrix[14];
                const float nx = worldMatrix[0]*nL[0]+worldMatrix[4]*nL[1]+worldMatrix[8]*nL[2];
                const float ny = worldMatrix[1]*nL[0]+worldMatrix[5]*nL[1]+worldMatrix[9]*nL[2];
                const float nz = worldMatrix[2]*nL[0]+worldMatrix[6]*nL[1]+worldMatrix[10]*nL[2];
                const float d = nx*(projPos[0]-wx) + ny*(projPos[1]-wy) + nz*(projPos[2]-wz);
                const bool receive = (onMode == Project3D::eProjectFront) ? (d > 0.f) : (d < 0.f);
                if (!receive) {
                    out.vertCulled[i] = 1;
                    const float wv = out.projSTW[i*3+2];
                    out.projSTW[i*3+0] = 2.0f * wv;
                    out.projSTW[i*3+1] = 2.0f * wv;
                }
            }
            if (!out.borderColor) out.borderColor = kProject3DBorderGrey;
        }
    }
    return true;
}

// Append projection layers (back-to-front) for a material `mat` to `out` (cap 4, like the
// renderer). Project3D → one layer; MergeMat → background (B) first, then foreground (A) over
// it with the MergeMat's operation. Mirrors ScanlineRender's flattenMaterialLayers.
static void
buildProject3DLayers(MaterialProvider* mat,
                     const std::vector<float>& localXYZ, const std::vector<float>& localNormals,
                     const float worldMatrix[16], double time, int op, float mix,
                     std::vector<Project3DLayer>& out)
{
    if (!mat || (int)out.size() >= 4) return;
    if (Project3D* p = dynamic_cast<Project3D*>(mat)) {
        Project3DLayer layer;
        if (computeProject3DLayer(p, localXYZ, localNormals, worldMatrix, time, op, mix, layer))
            out.push_back(std::move(layer));
        return;
    }
    if (MergeMat* m = dynamic_cast<MergeMat*>(mat)) {
        MaterialProvider* A = m->getInputMaterial(0);   // foreground
        MaterialProvider* B = m->getInputMaterial(1);   // background
        const int   mop  = m->getOperation(time);
        const float mmix = (float)m->getMix(time);
        buildProject3DLayers(B, localXYZ, localNormals, worldMatrix, time, MergeMat::eMergeOver, 1.f, out);
        buildProject3DLayers(A, localXYZ, localNormals, worldMatrix, time, mop, mmix, out);
        return;
    }
}

// Resolve a geo's material into its projection layers (empty if the material doesn't project).
static void
buildGeoProject3DLayers(const EffectInstancePtr& geoEffect,
                        const std::vector<float>& localXYZ, const std::vector<float>& localNormals,
                        const float worldMatrix[16], double time, std::vector<Project3DLayer>& out)
{
    if (localXYZ.empty()) return;
    MaterialProvider* mp = dynamic_cast<MaterialProvider*>(geoEffect.get());
    if (!mp) return;
    buildProject3DLayers(mp->getConnectedMaterial(), localXYZ, localNormals, worldMatrix, time,
                         MergeMat::eMergeOver, 1.f, out);
}

// Single-projection wrapper (back-compat for geo draws not yet converted to multi-pass): fill
// the legacy out-params from the TOP (foreground) layer only.
static void
applyProject3DToGeo(const EffectInstancePtr& geoEffect,
                    const std::vector<float>& localXYZ, const std::vector<float>& localNormals,
                    const float worldMatrix[16], double time,
                    const float*& texPixels, int& texW, int& texH, bool& texWrapRepeat,
                    std::vector<float>& projSTW, int& projComp, const float*& projBorderColor,
                    std::vector<unsigned char>& projVertCulled)
{
    projVertCulled.clear();
    if (texPixels || localXYZ.empty()) return;
    std::vector<Project3DLayer> layers;
    buildGeoProject3DLayers(geoEffect, localXYZ, localNormals, worldMatrix, time, layers);
    if (layers.empty()) return;
    Project3DLayer& top = layers.back();   // foreground
    texPixels = top.texPixels; texW = top.texW; texH = top.texH;
    texWrapRepeat = false; projComp = 3;
    projSTW.swap(top.projSTW); projBorderColor = top.borderColor; projVertCulled.swap(top.vertCulled);
}

// ============================================================================
// Section 3: Private struct — demo-style camera + ImGuizmo state
// ============================================================================

// Cached coarse density lattice for a ReadVDB viewport splat preview. Sampling a
// VDB re-reads the file (~80 ms), so we keep the lattice and only re-sample when
// the path or frame changes; the per-frame draw just rebuilds the splat buffer.
struct VdbSplatCache
{
    std::string path;
    int frame = -1000000;
    int res = 0;                  // viewport-display-res the lattice was sampled at
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> density;   // normalized [0,1], nx*ny*nz (z*ny*nx + y*nx + x)
    std::vector<float> fire;      // normalized [0,1] flames/temp; empty for pure smoke
};

struct Viewport3DPrivate
{
    // Camera (ImGuizmo demo-style spherical coords)
    float camYAngle;     // horizontal orbit angle (radians)
    float camXAngle;     // vertical orbit angle (radians)
    float camDistance;    // distance from target
    float camTarget[3];  // orbit target point
    float cameraView[16];
    float cameraProjection[16];
    float fov;

    // ImGuizmo state
    bool imguiInitialized;
    ImGuizmo::OPERATION imguizmoOp;
    ImGuizmo::MODE imguizmoMode;

    // Selection
    std::string selectedNodeName;
    int selectedCardIndex;

    // Per-ReadVDB cached density lattice for the viewport splat preview (keyed by
    // SceneNode name; re-sampled only when its path/frame changes).
    std::map<std::string, VdbSplatCache> vdbSplatCache;

    // Isolate Selected: when true, only the selected node (and its descendants)
    // is drawn in the viewport. Toggled from the Viewport3DTab toolbar.
    bool isolateSelected;

    // Point selection
    int selectedPointIndex;          // -1 = none
    std::string selectedPointCloud;  // name of the point cloud node
    float selectedPointPos[3];       // cached position of selected point
    float selectedPointCol[3];       // cached color of selected point
    std::vector<int> selectedPointIndices; // multi-selection

    // Active Blast node (set during render loop when we scan for the point
    // cloud to display). The right-click context menu uses this to target
    // its Add/Remove/Set/Clear Selection actions.
    NodeWPtr activeBlastNode;
    NodeWPtr activeProviderNode;   // whichever PointCloudProvider owns the displayed cloud

    // Toggle from the Grid button in Viewport3DTab. Default visible.
    bool showGrid;

    // Viewport shading style — driven by the Shading dropdown in Viewport3DTab.
    // Default Shaded+Wire so meshes show solid grey + edges (Maya-like) while
    // shapes carrying textures still display them.
    Viewport3D::ShadingMode shadingMode;

    // Right-click context menu — track the press position so we only show the
    // menu when the user releases without dragging (otherwise right-drag for
    // zoom/navigation would constantly trigger the menu). Threshold checked
    // in mouseReleaseEvent.
    bool rightButtonDown;
    int rightPressX, rightPressY;

    // "Look Through" — when set, the viewport view+projection matrices come
    // from this Camera3D / ReadAlembicCamera instead of the orbit camera.
    // Same math helpers as ScanlineRender (CameraMath::composeProjectionMatrix
    // + RotationConventions::composeInverse), so wireframes match the actual
    // render exactly. Null = orbit/free-fly mode.
    NodeWPtr lookThroughCam;

    // While the user is dragging in look-through mode AND the camera is a
    // Camera3D (editable, not Alembic), interactions write directly to the
    // camera's knobs (orbit / pan / dolly). Set in mousePressEvent, cleared
    // in mouseReleaseEvent.
    enum LookThroughEditMode { LT_NONE, LT_ORBIT, LT_PAN, LT_DOLLY };
    LookThroughEditMode lookThroughEditMode;
    float lookThroughPivot[3]; // world-space orbit pivot (captured at press)

    // Box select
    bool boxSelecting;
    int boxStartX, boxStartY;
    int boxEndX, boxEndY;

    // Mouse
    int lastMouseX, lastMouseY;
    bool orbiting, panning, zooming;
    float orbitPivot[3];  // pivot point for orbiting (set from depth buffer on orbit start)
    int viewW, viewH;

    // Scene graph (rebuilt each frame)
    SceneGraph sceneGraph;

    // Point cloud (keep for DeepToPoints)
    mutable QMutex cloudMutex;
    PointCloudDataPtr pointCloud;
    float pointSize;

    // Gizmo drag undo state
    bool gizmoDragging;
    float dragStartT[3], dragStartR[3], dragStartS[3];
    NodeWPtr dragNode; // weak ref to the node being dragged

    bool paused;  // Pause Updates: skip auto + frame-change repaints (on-demand rendering)

    // Particle shader (for per-particle size via gl_PointSize)
    GLuint particleShaderProgram;
    bool particleShaderReady;

    Viewport3DPrivate()
        : camYAngle(2.7f)
        , camXAngle(0.4f)
        , camDistance(8.0f)
        , fov(27.0f)
        , imguiInitialized(false)
        , imguizmoOp(ImGuizmo::TRANSLATE)
        , imguizmoMode(ImGuizmo::WORLD)
        , selectedCardIndex(-1)
        , isolateSelected(false)
        , selectedPointIndex(-1)
        , boxSelecting(false)
        , boxStartX(0), boxStartY(0)
        , boxEndX(0), boxEndY(0)
        , showGrid(true)
        , shadingMode(Viewport3D::eShadedWire)
        , rightButtonDown(false)
        , rightPressX(0), rightPressY(0)
        , lookThroughEditMode(LT_NONE)
        , lookThroughPivot{0, 0, 0}
        , lastMouseX(0)
        , lastMouseY(0)
        , orbiting(false)
        , panning(false)
        , zooming(false)
        , viewW(100)
        , orbitPivot{0, 0, 0}
        , viewH(100)
        , pointSize(2.0f)
        , gizmoDragging(false)
        , paused(false)
        , particleShaderProgram(0)
        , particleShaderReady(false)
    {
        camTarget[0] = 0.0f;
        camTarget[1] = 0.0f;
        camTarget[2] = 0.0f;
        std::memset(cameraView, 0, sizeof(cameraView));
        std::memset(cameraProjection, 0, sizeof(cameraProjection));
    }
};


// ============================================================================
// Constructor / Destructor
// ============================================================================

Viewport3D::Viewport3D(Gui* gui,
                             const QOpenGLWidget* shareWidget)
    : QOpenGLWidget()
    , _gui(gui)
    , _imp(new Viewport3DPrivate())
{
    Q_UNUSED(shareWidget);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    // Don't let Qt auto-fire contextMenuEvent on right-click. We trigger the
    // Blast menu manually from mouseReleaseEvent only when the click had no
    // drag (so right-drag for navigation doesn't accidentally open the menu).
    setContextMenuPolicy(Qt::PreventContextMenu);
}

Viewport3D::~Viewport3D()
{
}

QSize
Viewport3D::sizeHint() const
{
    return QSize(640, 480);
}

void
Viewport3D::setPointCloud(const PointCloudDataPtr& cloud, float pointSize)
{
    QMutexLocker lock(&_imp->cloudMutex);
    _imp->pointCloud = cloud;
    _imp->pointSize = pointSize;
    update();
}

void
Viewport3D::getCameraView(float m16[16]) const
{
    std::memcpy(m16, _imp->cameraView, sizeof(float) * 16);
}

void
Viewport3D::getCameraProjection(float m16[16]) const
{
    std::memcpy(m16, _imp->cameraProjection, sizeof(float) * 16);
}

void
Viewport3D::resetCamera()
{
    _imp->camYAngle = 2.7f;
    _imp->camXAngle = 0.4f;
    _imp->camDistance = 8.0f;
    _imp->camTarget[0] = 0.0f;
    _imp->camTarget[1] = 0.0f;
    _imp->camTarget[2] = 0.0f;
    update();
}


// ============================================================================
// Section 4: initializeGL
// ============================================================================

void
Viewport3D::initializeGL()
{
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glEnable(GL_POINT_SMOOTH);

    // Initialize ImGui + ImGuizmo.
    // The ImGui *context* is CPU-side state — create it once for this widget.
    if (!_imp->imguiInitialized) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL; // don't save imgui.ini
        io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        _imp->imguiInitialized = true;
    }
    // The font texture is a GL resource. QOpenGLWidget calls initializeGL again
    // whenever its GL context is recreated (e.g. when the widget is reparented
    // during a workspace/layout restore). The previous handle belongs to the now
    // destroyed context, so delete it and rebuild against the current context —
    // otherwise paintGL binds a stale texture and crashes (masked by NDEBUG).
    if (g_DevImGuiFontTexture) {
        glDeleteTextures(1, &g_DevImGuiFontTexture);
        g_DevImGuiFontTexture = 0;
    }
    ImGui_ImplOpenGL2_CreateFontsTexture();

    // Compile particle point-sprite shader. Also a per-context GL resource, so
    // drop any program from a previous context before rebuilding.
    {
        if (_imp->particleShaderProgram) {
            glDeleteProgram(_imp->particleShaderProgram);
            _imp->particleShaderProgram = 0;
        }
        _imp->particleShaderReady = false;
        const char* vtxSrc =
            "#version 430 compatibility\n"
            "attribute float psize;\n"
            "varying vec4 vColor;\n"
            "void main() {\n"
            "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
            "    gl_PointSize = max(1.0, psize * 500.0 / gl_Position.w);\n"
            "    vColor = gl_Color;\n"
            "}\n";
        const char* fragSrc =
            "#version 430 compatibility\n"
            "varying vec4 vColor;\n"
            "void main() {\n"
            "    vec2 pc = gl_PointCoord - vec2(0.5);\n"
            "    float dist = dot(pc, pc);\n"
            "    if (dist > 0.25) discard;\n"
            "    float alpha = 1.0 - smoothstep(0.15, 0.25, dist);\n"
            "    gl_FragColor = vec4(vColor.rgb, vColor.a * alpha);\n"
            "}\n";

        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vtxSrc, NULL);
        glCompileShader(vs);
        GLint ok = 0;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetShaderInfoLog(vs, 512, NULL, log);
            printf("[Viewport3D] Particle VS error: %s\n", log);
        }

        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fragSrc, NULL);
        glCompileShader(fs);
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetShaderInfoLog(fs, 512, NULL, log);
            printf("[Viewport3D] Particle FS error: %s\n", log);
        }

        _imp->particleShaderProgram = glCreateProgram();
        glAttachShader(_imp->particleShaderProgram, vs);
        glAttachShader(_imp->particleShaderProgram, fs);
        glLinkProgram(_imp->particleShaderProgram);
        glGetProgramiv(_imp->particleShaderProgram, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetProgramInfoLog(_imp->particleShaderProgram, 512, NULL, log);
            printf("[Viewport3D] Particle shader link error: %s\n", log);
        } else {
            _imp->particleShaderReady = true;
        }

        glDeleteShader(vs);
        glDeleteShader(fs);
    }

    // On-demand rendering: no auto-refresh timer. The viewport repaints only when
    // something requests it — Gui::redrawAllViewers() (knob/graph changes, same path
    // the 2D viewers use), frame changes, camera navigation, or the Refresh button.
}


void
Viewport3D::resizeGL(int w, int h)
{
    _imp->viewW = w;
    _imp->viewH = h;
    glViewport(0, 0, w, h);
}


// ============================================================================
// Section 5: paintGL — THE CRITICAL PART
// ============================================================================

// Helper: read T/R/S from a node's knobs
static void readTRSFromNode(NATRON_NAMESPACE::EffectInstancePtr effect,
                            float translation[3], float rotation[3], float scale[3])
{
    translation[0] = translation[1] = translation[2] = 0.0f;
    rotation[0] = rotation[1] = rotation[2] = 0.0f;
    scale[0] = scale[1] = scale[2] = 1.0f;

    if (!effect) return;

    KnobIPtr kTX = effect->getKnobByName("translateX");
    KnobIPtr kTY = effect->getKnobByName("translateY");
    KnobIPtr kTZ = effect->getKnobByName("translateZ");
    if (kTX) translation[0] = (float)dynamic_cast<KnobDouble*>(kTX.get())->getValue();
    if (kTY) translation[1] = (float)dynamic_cast<KnobDouble*>(kTY.get())->getValue();
    if (kTZ) translation[2] = (float)dynamic_cast<KnobDouble*>(kTZ.get())->getValue();

    KnobIPtr kRX = effect->getKnobByName("rotateX");
    KnobIPtr kRY = effect->getKnobByName("rotateY");
    KnobIPtr kRZ = effect->getKnobByName("rotateZ");
    if (kRX) rotation[0] = (float)dynamic_cast<KnobDouble*>(kRX.get())->getValue();
    if (kRY) rotation[1] = (float)dynamic_cast<KnobDouble*>(kRY.get())->getValue();
    if (kRZ) rotation[2] = (float)dynamic_cast<KnobDouble*>(kRZ.get())->getValue();

    KnobIPtr kSX = effect->getKnobByName("scaleX");
    KnobIPtr kSY = effect->getKnobByName("scaleY");
    KnobIPtr kSZ = effect->getKnobByName("scaleZ");
    if (kSX) scale[0] = (float)dynamic_cast<KnobDouble*>(kSX.get())->getValue();
    if (kSY) scale[1] = (float)dynamic_cast<KnobDouble*>(kSY.get())->getValue();
    if (kSZ) scale[2] = (float)dynamic_cast<KnobDouble*>(kSZ.get())->getValue();
}

// ==================== Look-through camera edit helpers ====================
// When the viewport is looking through a Camera3D (NOT a ReadAlembicCamera —
// .abc cameras get overwritten every frame so editing them is pointless), the
// orbit / pan / dolly drag interactions write directly to that camera's
// translateX/Y/Z and rotateX/Y/Z knobs. ReadAlembicCamera drags are ignored.

// Returns the look-through camera as a Camera3DNode (writable), or nullptr.
// All these helpers live in the file's existing NATRON_NAMESPACE block.
static Camera3DNode*
getEditableCamera3D(const NodeWPtr& weakNode)
{
    NodePtr node = weakNode.lock();
    if (!node) return nullptr;
    EffectInstancePtr eff = node->getEffectInstance();
    if (!eff) return nullptr;
    return dynamic_cast<Camera3DNode*>(eff.get());
}

static void
camPosFromKnobs(Camera3DNode* cam, double time, float pos[3])
{
    double tx = 0, ty = 0, tz = 0;
    KnobIPtr k;
    k = cam->getKnobByName("translateX"); if (k) tx = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = cam->getKnobByName("translateY"); if (k) ty = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = cam->getKnobByName("translateZ"); if (k) tz = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    pos[0] = (float)tx; pos[1] = (float)ty; pos[2] = (float)tz;
}

static void
camRotFromKnobs(Camera3DNode* cam, double time, double m[3][3])
{
    double rx = 0, ry = 0, rz = 0;
    KnobIPtr k;
    k = cam->getKnobByName("rotateX"); if (k) rx = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = cam->getKnobByName("rotateY"); if (k) ry = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    k = cam->getKnobByName("rotateZ"); if (k) rz = dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
    RotationConventions::compose(rx, ry, rz, m);
}

static void
writeCamPos(Camera3DNode* cam, double tx, double ty, double tz)
{
    KnobIPtr k;
    k = cam->getKnobByName("translateX"); if (k) dynamic_cast<KnobDouble*>(k.get())->setValue(tx);
    k = cam->getKnobByName("translateY"); if (k) dynamic_cast<KnobDouble*>(k.get())->setValue(ty);
    k = cam->getKnobByName("translateZ"); if (k) dynamic_cast<KnobDouble*>(k.get())->setValue(tz);
}

static void
writeCamRot(Camera3DNode* cam, double rx, double ry, double rz)
{
    KnobIPtr k;
    k = cam->getKnobByName("rotateX"); if (k) dynamic_cast<KnobDouble*>(k.get())->setValue(rx);
    k = cam->getKnobByName("rotateY"); if (k) dynamic_cast<KnobDouble*>(k.get())->setValue(ry);
    k = cam->getKnobByName("rotateZ"); if (k) dynamic_cast<KnobDouble*>(k.get())->setValue(rz);
}

// Compute extrinsic-XYZ Euler angles (degrees) for a camera at `pos` looking
// at `pivot`. World up is +Y. Camera's local -Z points at the pivot.
static void
eulerFromLookAt(const float pos[3], const float pivot[3],
                double& rxDeg, double& ryDeg, double& rzDeg)
{
    double fwd[3] = {
        (double)pivot[0] - (double)pos[0],
        (double)pivot[1] - (double)pos[1],
        (double)pivot[2] - (double)pos[2]
    };
    double len = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (len < 1e-6) { rxDeg = ryDeg = rzDeg = 0; return; }
    fwd[0] /= len; fwd[1] /= len; fwd[2] /= len;

    double worldUp[3] = { 0.0, 1.0, 0.0 };
    // right = normalize(cross(fwd, worldUp))
    double right[3] = {
        fwd[1]*worldUp[2] - fwd[2]*worldUp[1],
        fwd[2]*worldUp[0] - fwd[0]*worldUp[2],
        fwd[0]*worldUp[1] - fwd[1]*worldUp[0]
    };
    double rlen = std::sqrt(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
    if (rlen < 1e-6) {
        // Camera looking straight up/down — pick X as right
        right[0] = 1; right[1] = 0; right[2] = 0;
    } else {
        right[0] /= rlen; right[1] /= rlen; right[2] /= rlen;
    }
    // up = cross(right, fwd) — orthogonal to both
    double up[3] = {
        right[1]*fwd[2] - right[2]*fwd[1],
        right[2]*fwd[0] - right[0]*fwd[2],
        right[0]*fwd[1] - right[1]*fwd[0]
    };
    // R columns (camera-to-world, column-vector): right, up, -fwd (OpenGL -Z look)
    double R[3][3];
    R[0][0] = right[0]; R[0][1] = up[0]; R[0][2] = -fwd[0];
    R[1][0] = right[1]; R[1][1] = up[1]; R[1][2] = -fwd[1];
    R[2][0] = right[2]; R[2][1] = up[2]; R[2][2] = -fwd[2];
    RotationConventions::decompose(R, rxDeg, ryDeg, rzDeg);
}

// Orbit camera around `pivot` by mouse delta (dx, dy) pixels.
static void
applyOrbitToCamera3D(Camera3DNode* cam, double time, const float pivot[3], int dx, int dy)
{
    float pos[3];
    camPosFromKnobs(cam, time, pos);

    double v[3] = { pos[0] - pivot[0], pos[1] - pivot[1], pos[2] - pivot[2] };

    // Yaw around world +Y (horizontal mouse)
    const double SENS = 0.005; // radians/pixel, matches orbit-mode feel
    double yaw = -dx * SENS;
    double cy_ = std::cos(yaw), sy_ = std::sin(yaw);
    double v_y[3] = {
        cy_*v[0] + sy_*v[2],
        v[1],
        -sy_*v[0] + cy_*v[2]
    };

    // Pitch around current right vector (vertical mouse)
    double R[3][3];
    camRotFromKnobs(cam, time, R);
    double right[3] = { R[0][0], R[1][0], R[2][0] };

    double pitch = -dy * SENS;
    double cp = std::cos(pitch), sp = std::sin(pitch);
    // Rodrigues: v' = v*cos + (right x v)*sin + right*(right.v)*(1-cos)
    double rxv[3] = {
        right[1]*v_y[2] - right[2]*v_y[1],
        right[2]*v_y[0] - right[0]*v_y[2],
        right[0]*v_y[1] - right[1]*v_y[0]
    };
    double rdotv = right[0]*v_y[0] + right[1]*v_y[1] + right[2]*v_y[2];
    double v_new[3] = {
        v_y[0]*cp + rxv[0]*sp + right[0]*rdotv*(1-cp),
        v_y[1]*cp + rxv[1]*sp + right[1]*rdotv*(1-cp),
        v_y[2]*cp + rxv[2]*sp + right[2]*rdotv*(1-cp)
    };

    float newPos[3] = {
        (float)(pivot[0] + v_new[0]),
        (float)(pivot[1] + v_new[1]),
        (float)(pivot[2] + v_new[2])
    };
    writeCamPos(cam, newPos[0], newPos[1], newPos[2]);

    double newRX, newRY, newRZ;
    eulerFromLookAt(newPos, pivot, newRX, newRY, newRZ);
    writeCamRot(cam, newRX, newRY, newRZ);
}

// Pan: translate camera in its local right/up plane. Speed scales with the
// distance to pivot so drag feels consistent at any zoom.
static void
applyPanToCamera3D(Camera3DNode* cam, double time, const float pivot[3], int dx, int dy)
{
    float pos[3];
    camPosFromKnobs(cam, time, pos);
    double R[3][3];
    camRotFromKnobs(cam, time, R);

    double dpx = pos[0] - pivot[0], dpy = pos[1] - pivot[1], dpz = pos[2] - pivot[2];
    double dist = std::sqrt(dpx*dpx + dpy*dpy + dpz*dpz);
    const double SPEED = std::max(0.05, dist) * 0.002;

    // Drag right (dx>0) → camera moves LEFT (scene appears to move right under camera)
    // Drag down (dy>0) → camera moves UP (scene appears to move down)
    // Convention matches orbit-mode middle-drag pan.
    double newTx = (double)pos[0] - dx * SPEED * R[0][0] + dy * SPEED * R[0][1];
    double newTy = (double)pos[1] - dx * SPEED * R[1][0] + dy * SPEED * R[1][1];
    double newTz = (double)pos[2] - dx * SPEED * R[2][0] + dy * SPEED * R[2][1];
    writeCamPos(cam, newTx, newTy, newTz);
}

// Dolly: translate camera along its local view direction (-Z in camera frame).
static void
applyDollyToCamera3D(Camera3DNode* cam, double time, const float pivot[3], int dx, int dy)
{
    float pos[3];
    camPosFromKnobs(cam, time, pos);
    double R[3][3];
    camRotFromKnobs(cam, time, R);
    // View direction in world = -(col 2)
    double viewDir[3] = { -R[0][2], -R[1][2], -R[2][2] };

    double dpx = pos[0] - pivot[0], dpy = pos[1] - pivot[1], dpz = pos[2] - pivot[2];
    double dist = std::sqrt(dpx*dpx + dpy*dpy + dpz*dpz);
    const double SPEED = std::max(0.05, dist) * 0.005;

    // Match orbit-mode: drag right or DOWN = zoom IN (forward), drag left or
    // UP = zoom OUT (backward). Note Qt's y axis increases downward, so dy>0
    // = mouse moved down on screen.
    double delta = (dx + dy) * SPEED;
    double newTx = (double)pos[0] + delta * viewDir[0];
    double newTy = (double)pos[1] + delta * viewDir[1];
    double newTz = (double)pos[2] + delta * viewDir[2];
    writeCamPos(cam, newTx, newTy, newTz);
}

void
Viewport3D::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 1. Build camera matrices.
    //   - "Look Through" mode: use the chosen Camera3D / ReadAlembicCamera's
    //     transform + intrinsics. Same helpers as ScanlineRender, so the GL
    //     wireframe view matches the actual render.
    //   - Otherwise: orbit/free-fly camera built from spherical coords.
    NodePtr lookCamNode = _imp->lookThroughCam.lock();
    CameraProvider* lookCam = nullptr;
    if (lookCamNode) {
        EffectInstancePtr eff = lookCamNode->getEffectInstance();
        if (eff) lookCam = dynamic_cast<CameraProvider*>(eff.get());
    }

    // Gate rect in pixel coords — populated below when looking through a
    // camera, then used after the 3D pass to draw the gate outline.
    int gateX = 0, gateY = 0, gateW = _imp->viewW, gateH = _imp->viewH;

    if (lookCam) {
        // Current frame
        double time = 0.0;
        Gui* g = getGui();
        if (g) {
            GuiAppInstancePtr a = g->getApp();
            if (a) time = a->getTimeLine()->currentFrame();
        }

        double tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0;
        lookCam->getCameraPosition(time, tx, ty, tz, rx, ry, rz);
        const double focal = lookCam->getCameraFocalLength(time);
        const double hAp = lookCam->getCameraHAperture(time);
        const double vAp = lookCam->getCameraVAperture(time);
        const float near_ = (float)lookCam->getCameraNear(time);
        const float far_ = (float)lookCam->getCameraFar(time);

        // View matrix = inverse of camera-to-world. RotationConventions::
        // composeInverse builds R^T; we then translate by -t. Matches
        // ScanlineRender::buildViewMatrix exactly.
        double mInv[3][3];
        RotationConventions::composeInverse(rx, ry, rz, mInv);
        const float ntx = -(float)tx, nty = -(float)ty, ntz = -(float)tz;
        float* out = _imp->cameraView;
        out[0]  = (float)mInv[0][0]; out[1]  = (float)mInv[1][0]; out[2]  = (float)mInv[2][0]; out[3]  = 0.f;
        out[4]  = (float)mInv[0][1]; out[5]  = (float)mInv[1][1]; out[6]  = (float)mInv[2][1]; out[7]  = 0.f;
        out[8]  = (float)mInv[0][2]; out[9]  = (float)mInv[1][2]; out[10] = (float)mInv[2][2]; out[11] = 0.f;
        out[12] = (float)(mInv[0][0]*ntx + mInv[0][1]*nty + mInv[0][2]*ntz);
        out[13] = (float)(mInv[1][0]*ntx + mInv[1][1]*nty + mInv[1][2]*ntz);
        out[14] = (float)(mInv[2][0]*ntx + mInv[2][1]*nty + mInv[2][2]*ntz);
        out[15] = 1.f;

        // Projection — both apertures, no aspect (matches ScanlineRender).
        CameraMath::composeProjectionMatrix(focal, hAp, vAp, near_, far_, _imp->cameraProjection);

        // Camera gate — letterbox the 3D draw region to the camera sensor aspect
        // so the wireframe matches ScanlineRender's output bit-perfectly. Without
        // this the projection is stretched by the pane's aspect ratio.
        if (hAp > 1e-6 && vAp > 1e-6 && _imp->viewW > 0 && _imp->viewH > 0) {
            const double camAspect  = hAp / vAp;
            const double paneAspect = (double)_imp->viewW / (double)_imp->viewH;
            if (camAspect >= paneAspect) {
                gateW = _imp->viewW;
                gateH = (int)((double)_imp->viewW / camAspect + 0.5);
                gateX = 0;
                gateY = (_imp->viewH - gateH) / 2;
            } else {
                gateH = _imp->viewH;
                gateW = (int)((double)_imp->viewH * camAspect + 0.5);
                gateX = (_imp->viewW - gateW) / 2;
                gateY = 0;
            }
            glViewport(gateX, gateY, gateW, gateH);
        }
    } else {
        // Orbit camera (free-fly)
        float eye[3];
        eye[0] = cosf(_imp->camYAngle) * cosf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[0];
        eye[1] = sinf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[1];
        eye[2] = sinf(_imp->camYAngle) * cosf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[2];
        float at[3] = { _imp->camTarget[0], _imp->camTarget[1], _imp->camTarget[2] };
        float up[3] = { 0.f, 1.f, 0.f };
        LookAt(eye, at, up, _imp->cameraView);

        float aspect = (_imp->viewH > 0) ? (float)_imp->viewW / (float)_imp->viewH : 1.0f;
        Perspective(_imp->fov, aspect, 0.1f, 500.f, _imp->cameraProjection);
    }

    // 2. Set GL matrices
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(_imp->cameraProjection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(_imp->cameraView);

    // 3. Draw grid + axes (grid toggleable from the toolbar Grid button)
    if (_imp->showGrid) drawGrid();
    drawAxes();

    // 4. Scan for point cloud data
    {
        QMutexLocker lock(&_imp->cloudMutex);
        _imp->pointCloud.reset();
        _imp->pointSize = 2.0f;
    }
    {
        Gui* g = getGui();
        if (g) {
            GuiAppInstancePtr a = g->getApp();
            if (a) {
                ProjectPtr p = a->getProject();
                if (p) {
                    NodesList allNodes;
                    p->getNodes_recursive(allNodes, true);

                    // Which node is selected? Its cloud takes priority so e.g. a
                    // selected PointCloudGenerator isn't shadowed by the CameraTracker's
                    // sparse cloud (and vice-versa).
                    std::string selName;
                    {
                        NodeGraph* selGraph = g->getLastSelectedGraph();
                        if (selGraph) {
                            const std::list<NodeGuiPtr>& s = selGraph->getSelectedNodes();
                            if (!s.empty()) selName = s.front()->getNode()->getScriptName();
                        }
                    }

                    // Choose the best provider WITHOUT fetching (priority:
                    // selected = 3 > Blast = 2 > any other provider = 1), then fetch
                    // its cloud exactly once. (getPointCloud() can be expensive, so we
                    // never call it for providers we won't display.)
                    NodePtr chosenNode;
                    EffectInstance* chosenEff = NULL;
                    int chosenPrio = -1;
                    bool chosenIsBlast = false;
                    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
                        if (!(*it)->isActivated() || (*it)->isNodeDisabled()) continue;
                        EffectInstancePtr eff = (*it)->getEffectInstance();
                        if (!eff) continue;
                        Blast* blast = dynamic_cast<Blast*>(eff.get());
                        PointCloudProvider* prov = dynamic_cast<PointCloudProvider*>(eff.get());
                        if (!blast && !prov) continue;
                        bool sel = !selName.empty() && (*it)->getScriptName() == selName;
                        int prio = sel ? 3 : (blast ? 2 : 1);
                        if (prio > chosenPrio) {
                            chosenPrio = prio;
                            chosenNode = *it;
                            chosenEff = eff.get();
                            chosenIsBlast = (blast != NULL);
                        }
                    }
                    if (chosenEff) {
                        PointCloudProvider* prov = dynamic_cast<PointCloudProvider*>(chosenEff);
                        PointCloudDataPtr cloud = prov ? prov->getPointCloud() : PointCloudDataPtr();
                        if (cloud && cloud->numPoints() > 0) {
                            float ptSize = 2.0f;
                            KnobIPtr psKnob = chosenEff->getKnobByName("pointSize");
                            if (psKnob) {
                                KnobDouble* psDbl = dynamic_cast<KnobDouble*>(psKnob.get());
                                if (psDbl) ptSize = (float)psDbl->getValue();
                            }
                            QMutexLocker lock(&_imp->cloudMutex);
                            _imp->pointCloud = cloud;
                            _imp->pointSize = ptSize;
                            if (chosenIsBlast) _imp->activeBlastNode = chosenNode;
                            _imp->activeProviderNode = chosenNode;
                        }
                    }
                }
            }
        }
    }

    // 5. Rebuild scene graph from project nodes
    Gui* gui = getGui();
    if (gui) {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            ProjectPtr project = app->getProject();
            if (project) {
                double time = app->getTimeLine()->currentFrame();
                NodesList nodes;
                project->getNodes_recursive(nodes, true);
                _imp->sceneGraph.rebuild(nodes, time);
            }
        }
    }

    // 5b. Sync selection from node graph → 3D viewport
    if (gui) {
        NodeGraph* graph = gui->getLastSelectedGraph();
        if (graph) {
            const std::list<NodeGuiPtr>& sel = graph->getSelectedNodes();
            if (!sel.empty()) {
                std::string graphSelName = sel.front()->getNode()->getScriptName();
                // Only sync if the selected node exists in our scene graph
                const std::vector<SceneNode>& sns = _imp->sceneGraph.nodes();
                for (size_t si = 0; si < sns.size(); ++si) {
                    if (sns[si].name == graphSelName) {
                        _imp->selectedNodeName = graphSelName;
                        break;
                    }
                }
            }
        }
    }

    // 6. Render scene nodes using ImGuizmo::RecomposeMatrixFromComponents for transforms
    const std::vector<SceneNode>& sceneNodes = _imp->sceneGraph.nodes();
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (!sn.visible) continue;
        // Isolate Selected: when active AND something is selected, draw only the
        // selected node and its descendants (archive sub-entries are named
        // "<selected>/..."). Nothing selected => isolate is a no-op (draw all).
        if (_imp->isolateSelected && !_imp->selectedNodeName.empty()) {
            const std::string& sel = _imp->selectedNodeName;
            const bool isSel = (sn.name == sel);
            const bool isDescendant = (sn.name.size() > sel.size() &&
                                       sn.name.compare(0, sel.size(), sel) == 0 &&
                                       sn.name[sel.size()] == '/');
            if (!isSel && !isDescendant) continue;
        }
        NodePtr node = sn.sourceNode.lock();
        if (!node) continue;

        EffectInstancePtr effect = node->getEffectInstance();
        if (!effect) continue;

        // Use the SceneGraph-computed world matrix instead of reading the source
        // node's TRS knobs. This is required for multi-emit nodes (e.g.
        // ReadAlembicArchive) where many SceneNodes share one Natron source —
        // reading knobs from the source would draw them all at the same place.
        // Also picks up Group3D parent transforms correctly.
        glPushMatrix();
        glMultMatrixf(sn.worldMatrix);

        switch (sn.type) {
            case eSceneNodeMesh:       drawMeshNode(sn); break;
            case eSceneNodeCard:       drawCardNode(sn); break;
            case eSceneNodeCamera:     drawCameraNode(sn); break;
            case eSceneNodePointCloud: drawPointCloudNode(sn); break;
            case eSceneNodeGroup:      drawGroupNode(sn); break;
            case eSceneNodeSphere:     drawSphereNode(sn); break;
            case eSceneNodeCube:       drawCubeNode(sn); break;
            case eSceneNodeCylinder:   drawCylinderNode(sn); break;
            case eSceneNodeParticles: {
                // Only draw particles for the viewed/selected particle node.
                // If a particle node is selected, draw only that one.
                // If no particle node is selected, draw the last one in the list (terminal).
                bool isSelected = (sn.name == _imp->selectedNodeName);
                bool noParticleSelected = true;
                for (size_t pi = 0; pi < sceneNodes.size(); ++pi) {
                    if (sceneNodes[pi].type == eSceneNodeParticles &&
                        sceneNodes[pi].name == _imp->selectedNodeName) {
                        noParticleSelected = false;
                        break;
                    }
                }
                if (isSelected || noParticleSelected) {
                    // If no particle node is selected, only draw the last particle node
                    if (noParticleSelected) {
                        bool isLast = true;
                        for (size_t pi = i + 1; pi < sceneNodes.size(); ++pi) {
                            if (sceneNodes[pi].type == eSceneNodeParticles) {
                                isLast = false;
                                break;
                            }
                        }
                        if (isLast) drawParticlesNode(sn);
                    } else {
                        drawParticlesNode(sn);
                    }
                }
                break;
            }
            case eSceneNodeVolume:     drawVolumeNode(sn); break;
            case eSceneNodeLight:      drawLightNode(sn); break;
            case eSceneNodeTransform:  drawTransformNode(sn); break;
        }

        glPopMatrix();
    }

    // Draw point cloud
    drawPointCloud();

    // Restore full-pane viewport before ImGui overlay (gate only applies to
    // the 3D draw; gizmos & UI use the full pane in pixel coords).
    if (lookCam) {
        glViewport(0, 0, _imp->viewW, _imp->viewH);

        // Draw a thin outline around the camera gate, in pixel coords. The
        // outline marks the edge of the camera frustum — what actually gets
        // rendered. The dark area outside is the letterbox.
        if (gateW > 0 && gateH > 0 &&
            (gateW < _imp->viewW || gateH < _imp->viewH)) {
            glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT | GL_CURRENT_BIT);

            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glOrtho(0.0, (double)_imp->viewW, 0.0, (double)_imp->viewH, -1.0, 1.0);

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_LIGHTING);
            glLineWidth(1.0f);
            glColor3f(0.85f, 0.55f, 0.10f); // warm gate color, distinct from grid/axes

            // GL pixel-center convention: offset by 0.5 for crisp 1-pixel lines.
            const float x0 = (float)gateX + 0.5f;
            const float y0 = (float)gateY + 0.5f;
            const float x1 = (float)(gateX + gateW) - 0.5f;
            const float y1 = (float)(gateY + gateH) - 0.5f;
            glBegin(GL_LINE_LOOP);
                glVertex2f(x0, y0);
                glVertex2f(x1, y0);
                glVertex2f(x1, y1);
                glVertex2f(x0, y1);
            glEnd();

            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();

            glPopAttrib();
        }
    }

    // 7. ImGuizmo overlay
    if (_imp->imguiInitialized) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)_imp->viewW, (float)_imp->viewH);
        io.DeltaTime = 1.0f / 30.0f;
        io.MousePos = ImVec2((float)_imp->lastMouseX, (float)_imp->lastMouseY);

        ImGui::NewFrame();
        // Hidden window to prevent debug window stealing focus
        ImGui::SetNextWindowPos(ImVec2(-100, -100));
        ImGui::SetNextWindowSize(ImVec2(1, 1));
        ImGui::Begin("##h", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        ImGui::End();

        ImGuizmo::BeginFrame();
        ImGuizmo::SetRect(0, 0, (float)_imp->viewW, (float)_imp->viewH);
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetGizmoSizeClipSpace(0.2f); // 2x default size

        // Debug overlay
        ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 10), 0xFFFFFFFF, "Viewport3D");
        ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 30), 0xFFFFFFFF,
            _imp->selectedNodeName.empty() ? "No selection" : _imp->selectedNodeName.c_str());
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "Mouse: %d, %d  Size: %d x %d",
                 _imp->lastMouseX, _imp->lastMouseY, _imp->viewW, _imp->viewH);
        ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 50), 0xFFFFFFFF, dbg);

        // Box selection rectangle
        if (_imp->boxSelecting) {
            ImVec2 p1((float)_imp->boxStartX, (float)_imp->boxStartY);
            ImVec2 p2((float)_imp->boxEndX, (float)_imp->boxEndY);
            ImGui::GetForegroundDrawList()->AddRect(p1, p2, 0xFF00FFFF, 0.0f, 0, 1.5f);
            ImGui::GetForegroundDrawList()->AddRectFilled(p1, p2, 0x2200FFFF);
        }

        // Selected point info
        if (!_imp->selectedPointIndices.empty()) {
            char ptInfo[256];
            snprintf(ptInfo, sizeof(ptInfo),
                     "%d points selected  (first: #%d  pos=(%.3f, %.3f, %.3f))",
                     (int)_imp->selectedPointIndices.size(),
                     _imp->selectedPointIndex,
                     _imp->selectedPointPos[0], _imp->selectedPointPos[1], _imp->selectedPointPos[2]);
            ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 70), 0xFF00FFFF, ptInfo);
        } else if (_imp->selectedPointIndex >= 0) {
            char ptInfo[256];
            snprintf(ptInfo, sizeof(ptInfo),
                     "Point #%d  pos=(%.3f, %.3f, %.3f)  col=(%.3f, %.3f, %.3f)",
                     _imp->selectedPointIndex,
                     _imp->selectedPointPos[0], _imp->selectedPointPos[1], _imp->selectedPointPos[2],
                     _imp->selectedPointCol[0], _imp->selectedPointCol[1], _imp->selectedPointCol[2]);
            ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 70), 0xFF00FFFF, ptInfo);
        }

        // ImGuizmo Manipulate for selected node
        if (!_imp->selectedNodeName.empty()) {
            const std::vector<SceneNode>& sns = _imp->sceneGraph.nodes();
            for (size_t si = 0; si < sns.size(); ++si) {
                const SceneNode& sn = sns[si];
                if (sn.name != _imp->selectedNodeName) continue;
                NodePtr node = sn.sourceNode.lock();
                if (!node) break;
                EffectInstancePtr effect = node->getEffectInstance();
                if (!effect) break;

                // Skip ImGuizmo for read-only transform nodes (Alembic imports)
                if (sn.type == eSceneNodeTransform) break;

                // Read T/R/S from knobs, build matrix using ImGuizmo's Recompose.
                float translation[3], rotation[3], scale[3];
                readTRSFromNode(effect, translation, rotation, scale);
                // Pivot offset: seed the gizmo at (translate + pivot) so the handle
                // sits on the node's pivot and rotation/scale turn around it (e.g. an
                // Alembic archive's authored origin / bbox centre). pivot is {0,0,0}
                // for every other node, so this is a no-op there.
                const float* piv = sn.pivot;
                float gizT[3] = { translation[0] + piv[0],
                                  translation[1] + piv[1],
                                  translation[2] + piv[2] };
                float objMat[16];
                ImGuizmo::RecomposeMatrixFromComponents(gizT, rotation, scale, objMat);

                // Capture drag start values when ImGuizmo begins using
                if (ImGuizmo::IsUsing() && !_imp->gizmoDragging) {
                    _imp->gizmoDragging = true;
                    _imp->dragNode = node;
                    std::memcpy(_imp->dragStartT, translation, sizeof(float)*3);
                    std::memcpy(_imp->dragStartR, rotation, sizeof(float)*3);
                    std::memcpy(_imp->dragStartS, scale, sizeof(float)*3);
                }

                // Push undo when drag ends
                if (!ImGuizmo::IsUsing() && _imp->gizmoDragging) {
                    _imp->gizmoDragging = false;
                    NodePtr dragNode = _imp->dragNode.lock();
                    if (dragNode) {
                        NodeGuiIPtr nodeGuiI = dragNode->getNodeGui();
                        NodeGui* nodeGui = dynamic_cast<NodeGui*>(nodeGuiI.get());
                        if (nodeGui) {
                            // Push to NodeGraph's global undo stack (what Ctrl+Z uses),
                            // NOT the node's private per-node stack
                            NodeGraph* graph = nodeGui->getDagGui();
                            if (graph) {
                                float endT[3], endR[3], endS[3];
                                readTRSFromNode(effect, endT, endR, endS);
                                graph->pushUndoCommand(new GizmoTransformUndoCommand(
                                    effect,
                                    _imp->dragStartT, _imp->dragStartR, _imp->dragStartS,
                                    endT, endR, endS));
                            }
                        }
                    }
                }

                // Manipulate — draws gizmo AND handles interaction
                if (ImGuizmo::Manipulate(_imp->cameraView, _imp->cameraProjection,
                                          _imp->imguizmoOp, _imp->imguizmoMode,
                                          objMat, NULL, NULL))
                {
                    // Decompose modified matrix back to T/R/S, then back out the
                    // pivot offset so the plain translate knobs are stored.
                    float newT[3], newR[3], newS[3];
                    ImGuizmo::DecomposeMatrixToComponents(objMat, newT, newR, newS);
                    newT[0] -= piv[0]; newT[1] -= piv[1]; newT[2] -= piv[2];

                    KnobIPtr kTX = effect->getKnobByName("translateX");
                    KnobIPtr kTY = effect->getKnobByName("translateY");
                    KnobIPtr kTZ = effect->getKnobByName("translateZ");
                    if (kTX) dynamic_cast<KnobDouble*>(kTX.get())->setValue(newT[0]);
                    if (kTY) dynamic_cast<KnobDouble*>(kTY.get())->setValue(newT[1]);
                    if (kTZ) dynamic_cast<KnobDouble*>(kTZ.get())->setValue(newT[2]);

                    KnobIPtr kRX = effect->getKnobByName("rotateX");
                    KnobIPtr kRY = effect->getKnobByName("rotateY");
                    KnobIPtr kRZ = effect->getKnobByName("rotateZ");
                    if (kRX) dynamic_cast<KnobDouble*>(kRX.get())->setValue(newR[0]);
                    if (kRY) dynamic_cast<KnobDouble*>(kRY.get())->setValue(newR[1]);
                    if (kRZ) dynamic_cast<KnobDouble*>(kRZ.get())->setValue(newR[2]);

                    KnobIPtr kSX = effect->getKnobByName("scaleX");
                    KnobIPtr kSY = effect->getKnobByName("scaleY");
                    KnobIPtr kSZ = effect->getKnobByName("scaleZ");
                    if (kSX) dynamic_cast<KnobDouble*>(kSX.get())->setValue(newS[0]);
                    if (kSY) dynamic_cast<KnobDouble*>(kSY.get())->setValue(newS[1]);
                    if (kSZ) dynamic_cast<KnobDouble*>(kSZ.get())->setValue(newS[2]);

                    effect->endChanges();
                }
                break;
            }
        }

        glDisable(GL_DEPTH_TEST);
        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glEnable(GL_DEPTH_TEST);
    }
}


// ============================================================================
// Section 6: Mouse events
// ============================================================================

void
Viewport3D::mousePressEvent(QMouseEvent* e)
{
    _imp->lastMouseX = e->x();
    _imp->lastMouseY = e->y();

    // Feed ImGui mouse state directly (not polled)
    if (_imp->imguiInitialized) {
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = ImVec2((float)e->x(), (float)e->y());
        if (e->button() == Qt::LeftButton) io.MouseDown[0] = true;
        if (e->button() == Qt::RightButton) io.MouseDown[1] = true;
        if (e->button() == Qt::MiddleButton) io.MouseDown[2] = true;
        update(); // trigger paintGL so ImGuizmo processes the click
    }

    // Don't start orbit/pan/selection if ImGuizmo was active last frame
    if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return;

    // Look-through-edit intercept: if we're looking through a Camera3D (NOT
    // a ReadAlembicCamera) and the user starts an Alt+L / Mid / Alt+R drag,
    // redirect interactions to write the camera's knobs instead of the orbit
    // camera's state. Capture the orbit pivot 5 units in front of the camera.
    if (Camera3DNode* editCam = getEditableCamera3D(_imp->lookThroughCam)) {
        const bool isOrbit = (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier));
        const bool isPan = (e->button() == Qt::MiddleButton);
        const bool isDolly = (e->button() == Qt::RightButton && (e->modifiers() & Qt::AltModifier));
        if (isOrbit || isPan || isDolly) {
            double time = 0.0;
            if (getGui() && getGui()->getApp()) {
                time = getGui()->getApp()->getTimeLine()->currentFrame();
            }
            float pos[3]; camPosFromKnobs(editCam, time, pos);
            double R[3][3]; camRotFromKnobs(editCam, time, R);
            const float dist = 5.0f;
            // Pivot = camera_pos + 5 * (camera view direction) = pos - 5 * R col 2
            _imp->lookThroughPivot[0] = pos[0] - dist * (float)R[0][2];
            _imp->lookThroughPivot[1] = pos[1] - dist * (float)R[1][2];
            _imp->lookThroughPivot[2] = pos[2] - dist * (float)R[2][2];
            if (isOrbit)      _imp->lookThroughEditMode = Viewport3DPrivate::LT_ORBIT;
            else if (isPan)   _imp->lookThroughEditMode = Viewport3DPrivate::LT_PAN;
            else              _imp->lookThroughEditMode = Viewport3DPrivate::LT_DOLLY;
            // Right-button: still track for context-menu suppression in case
            // the user does a small drag (we won't show the menu either way
            // since they wanted to dolly, but keep the state coherent).
            if (e->button() == Qt::RightButton) {
                _imp->rightButtonDown = true;
                _imp->rightPressX = e->x();
                _imp->rightPressY = e->y();
            }
            return; // skip the orbit-camera flag-setting below
        }
    }

    if (e->button() == Qt::MiddleButton) {
        if ((e->modifiers() & Qt::AltModifier) || (e->modifiers() & Qt::ShiftModifier)) {
            _imp->panning = true;
        } else {
            _imp->orbiting = true;
            // Maya-style: orbit around selected object center
            if (!_imp->selectedNodeName.empty()) {
                const std::vector<SceneNode>& nodes = _imp->sceneGraph.nodes();
                for (size_t si = 0; si < nodes.size(); ++si) {
                    if (nodes[si].name == _imp->selectedNodeName) {
                        // Use the selected node's transform position as orbit target
                        _imp->camTarget[0] = nodes[si].localMatrix[12];
                        _imp->camTarget[1] = nodes[si].localMatrix[13];
                        _imp->camTarget[2] = nodes[si].localMatrix[14];
                        // Recalculate distance from eye to new target
                        float eye[3];
                        eye[0] = cosf(_imp->camYAngle) * cosf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[0];
                        eye[1] = sinf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[1];
                        eye[2] = sinf(_imp->camYAngle) * cosf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[2];
                        float dx = eye[0] - _imp->camTarget[0];
                        float dy = eye[1] - _imp->camTarget[1];
                        float dz = eye[2] - _imp->camTarget[2];
                        _imp->camDistance = sqrtf(dx*dx + dy*dy + dz*dz);
                        break;
                    }
                }
            }
        }
    } else if (e->button() == Qt::LeftButton) {
        if (e->modifiers() & Qt::AltModifier) {
            _imp->orbiting = true;
        } else {
            // Try point picking first, then start box select drag
            if (!pickPointAtPosition(e->x(), e->y())) {
                // Start potential box select — if drag is small, treat as click select
                _imp->boxSelecting = true;
                _imp->boxStartX = _imp->boxEndX = e->x();
                _imp->boxStartY = _imp->boxEndY = e->y();
            }
        }
    } else if (e->button() == Qt::RightButton) {
        if (e->modifiers() & Qt::AltModifier) {
            _imp->zooming = true;
        }
        // Always remember the press position so mouseReleaseEvent can decide
        // whether this was a click (→ show Blast menu) or a drag (→ ignore).
        _imp->rightButtonDown = true;
        _imp->rightPressX = e->x();
        _imp->rightPressY = e->y();
    }
}

void
Viewport3D::mouseMoveEvent(QMouseEvent* e)
{
    int dx = e->x() - _imp->lastMouseX;
    int dy = e->y() - _imp->lastMouseY;
    _imp->lastMouseX = e->x();
    _imp->lastMouseY = e->y();

    // Feed ImGui
    if (_imp->imguiInitialized) {
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = ImVec2((float)e->x(), (float)e->y());
    }

    // If ImGuizmo is being used, just update and return
    if (ImGuizmo::IsUsing()) {
        update();
        return;
    }

    // Look-through-edit: dispatch to the appropriate Camera3D mutation.
    if (_imp->lookThroughEditMode != Viewport3DPrivate::LT_NONE) {
        Camera3DNode* editCam = getEditableCamera3D(_imp->lookThroughCam);
        if (editCam) {
            double time = 0.0;
            if (getGui() && getGui()->getApp()) {
                time = getGui()->getApp()->getTimeLine()->currentFrame();
            }
            switch (_imp->lookThroughEditMode) {
                case Viewport3DPrivate::LT_ORBIT:
                    applyOrbitToCamera3D(editCam, time, _imp->lookThroughPivot, dx, dy); break;
                case Viewport3DPrivate::LT_PAN:
                    applyPanToCamera3D(editCam, time, _imp->lookThroughPivot, dx, dy); break;
                case Viewport3DPrivate::LT_DOLLY:
                    applyDollyToCamera3D(editCam, time, _imp->lookThroughPivot, dx, dy); break;
                default: break;
            }
            update();
        }
        return;
    }

    if (_imp->orbiting) {
        _imp->camYAngle += dx * 0.01f;
        _imp->camXAngle += dy * 0.01f;
        // Clamp vertical angle to avoid flipping
        _imp->camXAngle = std::max(-1.5f, std::min(1.5f, _imp->camXAngle));
        update();
    } else if (_imp->panning) {
        // Pan: move camTarget in camera-right and camera-up directions
        float panSpeed = _imp->camDistance * 0.002f;
        // Camera right direction (from view matrix row 0)
        float rightX = _imp->cameraView[0];
        float rightY = _imp->cameraView[4];
        float rightZ = _imp->cameraView[8];
        // Camera up direction (from view matrix row 1)
        float upX = _imp->cameraView[1];
        float upY = _imp->cameraView[5];
        float upZ = _imp->cameraView[9];

        _imp->camTarget[0] -= (dx * rightX - dy * upX) * panSpeed;
        _imp->camTarget[1] -= (dx * rightY - dy * upY) * panSpeed;
        _imp->camTarget[2] -= (dx * rightZ - dy * upZ) * panSpeed;
        update();
    } else if (_imp->zooming) {
        // Alt+RMB zoom: drag right/up = zoom in, left/down = zoom out
        float zoomSpeed = _imp->camDistance * 0.005f;
        _imp->camDistance -= (dx + dy) * zoomSpeed;
        if (_imp->camDistance < 0.1f) _imp->camDistance = 0.1f;
        update();
    } else if (_imp->boxSelecting) {
        _imp->boxEndX = e->x();
        _imp->boxEndY = e->y();
        update();
    }
}

void
Viewport3D::mouseReleaseEvent(QMouseEvent* e)
{
    // Feed ImGui
    if (_imp->imguiInitialized) {
        ImGuiIO& io = ImGui::GetIO();
        if (e->button() == Qt::LeftButton) io.MouseDown[0] = false;
        if (e->button() == Qt::RightButton) io.MouseDown[1] = false;
        if (e->button() == Qt::MiddleButton) io.MouseDown[2] = false;
        update();
    }

    // Finish box select (left-click drag)
    if (_imp->boxSelecting && e->button() == Qt::LeftButton) {
        _imp->boxSelecting = false;
        int bw = std::abs(_imp->boxEndX - _imp->boxStartX);
        int bh = std::abs(_imp->boxEndY - _imp->boxStartY);
        if (bw > 5 || bh > 5) {
            // Real drag — box select points
            boxSelectPoints();
        } else {
            // Tiny drag — treat as click, do object selection
            selectObjectAtPosition(_imp->boxStartX, _imp->boxStartY);
        }
        update();
    }

    // Right-button release: if this was a click (negligible drag), show the
    // Blast context menu. If the user dragged (e.g. right-drag to navigate),
    // suppress the menu.
    if (e->button() == Qt::RightButton && _imp->rightButtonDown) {
        const int dx = e->x() - _imp->rightPressX;
        const int dy = e->y() - _imp->rightPressY;
        const int CLICK_THRESHOLD = 5; // pixels
        const bool wasClick = (std::abs(dx) <= CLICK_THRESHOLD &&
                               std::abs(dy) <= CLICK_THRESHOLD);
        _imp->rightButtonDown = false;
        if (wasClick && !(e->modifiers() & Qt::AltModifier)) {
            showBlastContextMenu(e->globalPos());
        }
    }

    _imp->orbiting = false;
    _imp->panning = false;
    _imp->zooming = false;
    _imp->lookThroughEditMode = Viewport3DPrivate::LT_NONE;
}

void
Viewport3D::wheelEvent(QWheelEvent* e)
{
    float delta = e->angleDelta().y() / 120.0f;
    _imp->camDistance *= (1.0f - delta * 0.1f);
    // Clamp
    _imp->camDistance = std::max(0.1f, std::min(500.0f, _imp->camDistance));
    update();
}


// ============================================================================
// Section 7: keyPressEvent
// ============================================================================

void
Viewport3D::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_W) {
        _imp->imguizmoOp = ImGuizmo::TRANSLATE;
        update();
    } else if (e->key() == Qt::Key_E) {
        _imp->imguizmoOp = ImGuizmo::ROTATE;
        update();
    } else if (e->key() == Qt::Key_R) {
        _imp->imguizmoOp = ImGuizmo::SCALE;
        update();
    } else if (e->key() == Qt::Key_F) {
        // Frame selected — try point cloud first, then selected node, then reset.
        // Picks a single (targetX,Y,Z + distance) and dispatches to the right
        // camera mover: in perspective mode we just update camTarget/camDistance;
        // in look-through mode (on an editable Camera3D) we move the camera node
        // along its current view direction so the framed target sits at the
        // desired distance with the camera's orientation preserved.
        float targetX = 0, targetY = 0, targetZ = 0;
        float distance = 5.0f;
        bool  framed   = false;

        {
            QMutexLocker lock(&_imp->cloudMutex);
            if (_imp->pointCloud && _imp->pointCloud->numPoints() > 0) {
                _imp->pointCloud->getCenter(targetX, targetY, targetZ);
                float radius = _imp->pointCloud->getRadius();
                if (radius < 0.1f) radius = 2.0f;
                distance = radius * 2.5f;
                framed = true;
            }
        }

        if (!framed && !_imp->selectedNodeName.empty()) {
            // Frame the selection's ACTUAL world-space geometry bounds, so framing
            // stays correct when the node (e.g. an archive) is scaled. A multi-emit
            // archive shares one name across all its entries — union the world AABB of
            // every mesh entry, transforming each vertex by that entry's worldMatrix
            // (which includes the user scale). Falls back to the node's world position
            // when it has no mesh geometry (e.g. a locator-only selection).
            const std::vector<SceneNode>& nodes = _imp->sceneGraph.nodes();
            float mn[3] = {0,0,0}, mx[3] = {0,0,0};
            bool haveBounds = false;
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].name != _imp->selectedNodeName) continue;
                const MeshDataPtr& md = nodes[i].meshData;
                if (!md || md->vertices.empty()) continue;
                const float* M = nodes[i].worldMatrix;
                const std::vector<float>& v = md->vertices;
                for (size_t k = 0; k + 2 < v.size(); k += 3) {
                    const float lx = v[k], ly = v[k+1], lz = v[k+2];
                    const float wx = M[0]*lx + M[4]*ly + M[8]*lz + M[12];
                    const float wy = M[1]*lx + M[5]*ly + M[9]*lz + M[13];
                    const float wz = M[2]*lx + M[6]*ly + M[10]*lz + M[14];
                    if (!haveBounds) {
                        mn[0]=mx[0]=wx; mn[1]=mx[1]=wy; mn[2]=mx[2]=wz;
                        haveBounds = true;
                    } else {
                        if (wx < mn[0]) mn[0] = wx; else if (wx > mx[0]) mx[0] = wx;
                        if (wy < mn[1]) mn[1] = wy; else if (wy > mx[1]) mx[1] = wy;
                        if (wz < mn[2]) mn[2] = wz; else if (wz > mx[2]) mx[2] = wz;
                    }
                }
            }
            if (haveBounds) {
                targetX = 0.5f * (mn[0] + mx[0]);
                targetY = 0.5f * (mn[1] + mx[1]);
                targetZ = 0.5f * (mn[2] + mx[2]);
                const float ex = mx[0]-mn[0], ey = mx[1]-mn[1], ez = mx[2]-mn[2];
                float radius = 0.5f * std::sqrt(ex*ex + ey*ey + ez*ez);
                if (radius < 0.1f) radius = 1.0f;
                distance = radius * 2.5f;   // same fit factor as the point-cloud path
                framed = true;
            } else {
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i].name == _imp->selectedNodeName) {
                        const float* M = nodes[i].worldMatrix;
                        targetX = M[12]; targetY = M[13]; targetZ = M[14];
                        distance = 5.0f;
                        framed = true;
                        break;
                    }
                }
            }
        }

        if (!framed) {
            resetCamera();
            return;
        }

        // Look-through path: editable Camera3D — translate the camera node
        // along its current view direction (-Z in camera frame). For
        // ReadAlembicCamera (or any non-editable look-through source),
        // getEditableCamera3D returns null and we fall back to the
        // perspective-camera update so the user still sees a framed view
        // even if they're nominally "looking through" a read-only camera.
        if (Camera3DNode* editCam = getEditableCamera3D(_imp->lookThroughCam)) {
            double time = 0.0;
            if (getGui() && getGui()->getApp()) {
                time = getGui()->getApp()->getTimeLine()->currentFrame();
            }
            double R[3][3];
            camRotFromKnobs(editCam, time, R);
            // View direction in world space = -(rotation column 2). Same
            // convention as applyDollyToCamera3D.
            const double vdx = -R[0][2], vdy = -R[1][2], vdz = -R[2][2];
            const double newTx = (double)targetX - vdx * (double)distance;
            const double newTy = (double)targetY - vdy * (double)distance;
            const double newTz = (double)targetZ - vdz * (double)distance;
            writeCamPos(editCam, newTx, newTy, newTz);
        } else {
            _imp->camTarget[0] = targetX;
            _imp->camTarget[1] = targetY;
            _imp->camTarget[2] = targetZ;
            _imp->camDistance  = distance;
        }
        update();
    } else if (e->key() == Qt::Key_A) {
        resetCamera();
    } else if (e->key() == Qt::Key_Escape) {
        _imp->selectedNodeName.clear();
        _imp->selectedCardIndex = -1;
        update();
    } else {
        QOpenGLWidget::keyPressEvent(e);
    }
}

void
Viewport3D::toggleTransformSpace()
{
    if (_imp->imguizmoMode == ImGuizmo::WORLD) {
        _imp->imguizmoMode = ImGuizmo::LOCAL;
    } else {
        _imp->imguizmoMode = ImGuizmo::WORLD;
    }
    update();
}

bool
Viewport3D::isLocalSpace() const
{
    return _imp->imguizmoMode == ImGuizmo::LOCAL;
}


// ============================================================================
// Section 8: Selection
// ============================================================================

// Project world point to screen using the demo-style camera matrices
static bool worldToScreenDev(const float cameraView[16], const float cameraProjection[16],
                             int viewW, int viewH,
                             float wx, float wy, float wz, float& sx, float& sy)
{
    // Transform by view matrix (row-major: v = M * p, where M is stored row-major)
    float vx = cameraView[0]*wx + cameraView[4]*wy + cameraView[8]*wz + cameraView[12];
    float vy = cameraView[1]*wx + cameraView[5]*wy + cameraView[9]*wz + cameraView[13];
    float vz = cameraView[2]*wx + cameraView[6]*wy + cameraView[10]*wz + cameraView[14];
    float vw = cameraView[3]*wx + cameraView[7]*wy + cameraView[11]*wz + cameraView[15];

    // Transform by projection matrix
    float cx = cameraProjection[0]*vx + cameraProjection[4]*vy + cameraProjection[8]*vz + cameraProjection[12]*vw;
    float cy = cameraProjection[1]*vx + cameraProjection[5]*vy + cameraProjection[9]*vz + cameraProjection[13]*vw;
    float cw = cameraProjection[3]*vx + cameraProjection[7]*vy + cameraProjection[11]*vz + cameraProjection[15]*vw;

    if (std::abs(cw) < 0.0001f) return false;

    float ndcX = cx / cw;
    float ndcY = cy / cw;

    sx = (1.0f + ndcX) * viewW * 0.5f;
    sy = (1.0f - ndcY) * viewH * 0.5f; // Y flipped for Qt screen coords
    return true;
}

bool
Viewport3D::pickPointAtPosition(int screenX, int screenY)
{
    QMutexLocker lock(&_imp->cloudMutex);
    if (!_imp->pointCloud || _imp->pointCloud->numPoints() == 0) {
        _imp->selectedPointIndex = -1;
        return false;
    }

    const float* data = _imp->pointCloud->data();
    std::size_t numPoints = _imp->pointCloud->numPoints();
    const int stride = 6; // x,y,z,r,g,b

    float bestDist = 15.0f; // max pick distance in pixels
    int bestIdx = -1;

    for (std::size_t i = 0; i < numPoints; ++i) {
        float wx = data[i * stride + 0];
        float wy = data[i * stride + 1];
        float wz = data[i * stride + 2];

        float sx, sy;
        if (!worldToScreenDev(_imp->cameraView, _imp->cameraProjection,
                              _imp->viewW, _imp->viewH,
                              wx, wy, wz, sx, sy)) continue;

        float dx = sx - (float)screenX;
        float dy = sy - (float)screenY;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = (int)i;
        }
    }

    if (bestIdx >= 0) {
        _imp->selectedPointIndex = bestIdx;
        _imp->selectedPointPos[0] = data[bestIdx * stride + 0];
        _imp->selectedPointPos[1] = data[bestIdx * stride + 1];
        _imp->selectedPointPos[2] = data[bestIdx * stride + 2];
        _imp->selectedPointCol[0] = data[bestIdx * stride + 3];
        _imp->selectedPointCol[1] = data[bestIdx * stride + 4];
        _imp->selectedPointCol[2] = data[bestIdx * stride + 5];
        // Replace multi-selection with this single index. Selection is purely
        // visual now — committing to Blast is explicit via the right-click
        // context menu (see contextMenuEvent).
        _imp->selectedPointIndices.clear();
        _imp->selectedPointIndices.push_back(bestIdx);
        notifyProviderSelectionChanged();
        update();
        return true;
    }

    // No hit — clear the multi-selection.
    _imp->selectedPointIndex = -1;
    _imp->selectedPointIndices.clear();
    notifyProviderSelectionChanged();
    return false;
}

void
Viewport3D::notifyProviderSelectionChanged()
{
    // Mirror the current selection to the Engine-side provider so nodes can
    // offer selection-driven actions (CameraTracker set-origin/ground-plane,
    // snap-to-point). Caller must hold cloudMutex (all selection mutations do).
    NodePtr prov = _imp->activeProviderNode.lock();
    if (!prov) return;
    EffectInstancePtr eff = prov->getEffectInstance();
    if (!eff) return;
    PointCloudProvider* pcp = dynamic_cast<PointCloudProvider*>(eff.get());
    if (pcp) {
        pcp->setViewportSelection(_imp->selectedPointIndices);
    }
}

void
Viewport3D::setShowGrid(bool show)
{
    if (_imp->showGrid == show) return;
    _imp->showGrid = show;
    update();
}

void
Viewport3D::setShadingMode(Viewport3D::ShadingMode mode)
{
    if (_imp->shadingMode == mode) return;
    _imp->shadingMode = mode;
    update();
}

void
Viewport3D::setIsolateSelected(bool enabled)
{
    if (_imp->isolateSelected == enabled) return;
    _imp->isolateSelected = enabled;
    update();
}

bool
Viewport3D::isIsolateSelected() const
{
    return _imp->isolateSelected;
}

void
Viewport3D::setPaused(bool paused)
{
    _imp->paused = paused;
    // On-demand mode: pausing makes requestRedraw() + frame-change updates no-op;
    // manual camera navigation still repaints. On resume, repaint once to catch up
    // to the current scene.
    if (!paused) {
        update();
    }
}

void
Viewport3D::requestRedraw()
{
    // Event-driven repaint entry point — called by Gui::redrawAllViewers() on any
    // knob/graph change (same central trigger the 2D viewers use). No-op while paused.
    if (!_imp->paused) {
        update();
    }
}

bool
Viewport3D::isPaused() const
{
    return _imp->paused;
}

void
Viewport3D::forceRefresh()
{
    // One-shot "force new render": re-pull the scene data (point cloud) and repaint
    // once, regardless of the paused state.
    refreshPointCloud();
    update();
}

Viewport3D::ShadingMode
Viewport3D::getShadingMode() const
{
    return _imp->shadingMode;
}

void
Viewport3D::setLookThroughCamera(const NodePtr& cameraNode)
{
    _imp->lookThroughCam = cameraNode;
    update();
}

NodePtr
Viewport3D::getLookThroughCamera() const
{
    return _imp->lookThroughCam.lock();
}

Blast*
Viewport3D::getActiveBlast() const
{
    NodePtr active = _imp->activeBlastNode.lock();
    if (!active) return nullptr;
    EffectInstancePtr eff = active->getEffectInstance();
    if (!eff) return nullptr;
    return dynamic_cast<Blast*>(eff.get());
}

void
Viewport3D::showBlastContextMenu(const QPoint& globalPos)
{
    Blast* blast = getActiveBlast();
    if (!blast) return; // no Blast in scene → nothing to do

    const std::vector<int> viewportSel = _imp->selectedPointIndices;
    const bool hasSel = !viewportSel.empty();

    QMenu menu(this);

    QAction* addAct = menu.addAction(QString::fromUtf8("Blast: Add Selected"));
    addAct->setEnabled(hasSel);
    addAct->setToolTip(QString::fromUtf8("Add currently selected points to the Blast filter set"));

    QAction* removeAct = menu.addAction(QString::fromUtf8("Blast: Remove Selected"));
    removeAct->setEnabled(hasSel);
    removeAct->setToolTip(QString::fromUtf8("Remove currently selected points from the Blast filter set"));

    QAction* setAct = menu.addAction(QString::fromUtf8("Blast: Set as Selection"));
    setAct->setEnabled(hasSel);
    setAct->setToolTip(QString::fromUtf8("Replace the Blast filter set with the current viewport selection"));

    menu.addSeparator();

    QAction* clearAct = menu.addAction(QString::fromUtf8("Blast: Clear Selection"));
    clearAct->setToolTip(QString::fromUtf8("Empty the Blast filter set"));

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;

    if (chosen == addAct) {
        blast->addToSelection(viewportSel);
    } else if (chosen == removeAct) {
        blast->removeFromSelection(viewportSel);
    } else if (chosen == setAct) {
        blast->setSelectedIndices(viewportSel);
    } else if (chosen == clearAct) {
        blast->clearSelection();
    }
}

void
Viewport3D::boxSelectPoints()
{
    QMutexLocker lock(&_imp->cloudMutex);
    _imp->selectedPointIndices.clear();
    _imp->selectedPointIndex = -1;

    if (!_imp->pointCloud || _imp->pointCloud->numPoints() == 0) return;

    const float* data = _imp->pointCloud->data();
    std::size_t numPoints = _imp->pointCloud->numPoints();
    const int stride = 6;

    // Normalize box coords (drag can go any direction)
    float minX = (float)std::min(_imp->boxStartX, _imp->boxEndX);
    float maxX = (float)std::max(_imp->boxStartX, _imp->boxEndX);
    float minY = (float)std::min(_imp->boxStartY, _imp->boxEndY);
    float maxY = (float)std::max(_imp->boxStartY, _imp->boxEndY);

    for (std::size_t i = 0; i < numPoints; ++i) {
        float wx = data[i * stride + 0];
        float wy = data[i * stride + 1];
        float wz = data[i * stride + 2];

        float sx, sy;
        if (!worldToScreenDev(_imp->cameraView, _imp->cameraProjection,
                              _imp->viewW, _imp->viewH,
                              wx, wy, wz, sx, sy)) continue;

        if (sx >= minX && sx <= maxX && sy >= minY && sy <= maxY) {
            _imp->selectedPointIndices.push_back((int)i);
        }
    }

    // Set single selection to first point for info display
    if (!_imp->selectedPointIndices.empty()) {
        int idx = _imp->selectedPointIndices[0];
        _imp->selectedPointIndex = idx;
        _imp->selectedPointPos[0] = data[idx * stride + 0];
        _imp->selectedPointPos[1] = data[idx * stride + 1];
        _imp->selectedPointPos[2] = data[idx * stride + 2];
        _imp->selectedPointCol[0] = data[idx * stride + 3];
        _imp->selectedPointCol[1] = data[idx * stride + 4];
        _imp->selectedPointCol[2] = data[idx * stride + 5];
    }
    // Note: selection is purely visual now — committing to Blast is explicit
    // via the right-click context menu.
    notifyProviderSelectionChanged();
}

void
Viewport3D::selectObjectAtPosition(int screenX, int screenY)
{
    const std::vector<SceneNode>& sceneNodes = _imp->sceneGraph.nodes();

    if (sceneNodes.empty()) {
        _imp->selectedNodeName.clear();
        _imp->selectedCardIndex = -1;
        update();
        return;
    }

    float bestDist = 60.0f; // max pick distance in pixels
    int bestIdx = -1;

    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (sn.type == eSceneNodePointCloud) continue;
        NodePtr node = sn.sourceNode.lock();
        if (!node) continue;

        EffectInstancePtr effect = node->getEffectInstance();
        if (!effect) continue;

        // Get node position from knobs (consistent with RecomposeMatrixFromComponents)
        float translation[3], rotation[3], scale[3];
        readTRSFromNode(effect, translation, rotation, scale);

        float projX, projY;
        if (!worldToScreenDev(_imp->cameraView, _imp->cameraProjection,
                              _imp->viewW, _imp->viewH,
                              translation[0], translation[1], translation[2],
                              projX, projY)) continue;

        float dx = projX - (float)screenX;
        float dy = projY - (float)screenY;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = (int)i;
        }
    }

    if (bestIdx >= 0) {
        const SceneNode& sn = sceneNodes[bestIdx];
        _imp->selectedNodeName = sn.name;
        _imp->selectedCardIndex = sn.subIndex;
    } else {
        // Check if click is near selected object's gizmo before deselecting
        bool keepSelection = false;
        if (!_imp->selectedNodeName.empty()) {
            for (size_t i = 0; i < sceneNodes.size(); ++i) {
                if (sceneNodes[i].name != _imp->selectedNodeName) continue;
                NodePtr node = sceneNodes[i].sourceNode.lock();
                if (!node) break;
                EffectInstancePtr effect = node->getEffectInstance();
                if (!effect) break;

                float translation[3], rotation[3], scale[3];
                readTRSFromNode(effect, translation, rotation, scale);

                float projX, projY;
                if (worldToScreenDev(_imp->cameraView, _imp->cameraProjection,
                                     _imp->viewW, _imp->viewH,
                                     translation[0], translation[1], translation[2],
                                     projX, projY)) {
                    float dx = projX - (float)screenX;
                    float dy = projY - (float)screenY;
                    float dist = sqrtf(dx * dx + dy * dy);
                    if (dist < 150.0f) keepSelection = true;
                }
                break;
            }
        }
        if (!keepSelection) {
            _imp->selectedNodeName.clear();
            _imp->selectedCardIndex = -1;
        }
    }
    update();
}


// ============================================================================
// Section 9: Scene rendering helpers — copied from old Viewport3D.cpp
// ============================================================================

void
Viewport3D::drawGrid() const
{
    glBegin(GL_LINES);
    glColor3f(0.3f, 0.3f, 0.3f);
    for (int i = -10; i <= 10; ++i) {
        glVertex3f((float)i, 0.0f, -10.0f);
        glVertex3f((float)i, 0.0f,  10.0f);
        glVertex3f(-10.0f, 0.0f, (float)i);
        glVertex3f( 10.0f, 0.0f, (float)i);
    }
    glEnd();
}

void
Viewport3D::drawAxes() const
{
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    // X axis - red
    glColor3f(0.8f, 0.2f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(3, 0, 0);
    // Y axis - green
    glColor3f(0.2f, 0.8f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(0, 3, 0);
    // Z axis - blue
    glColor3f(0.2f, 0.2f, 0.8f);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, 3);
    glEnd();
    glLineWidth(1.0f);
}

void
Viewport3D::drawPointCloud() const
{
    QMutexLocker lock(&_imp->cloudMutex);
    if (!_imp->pointCloud || _imp->pointCloud->numPoints() == 0) {
        return;
    }

    const float* data = _imp->pointCloud->data();
    std::size_t numPoints = _imp->pointCloud->numPoints();
    float ptSize = _imp->pointSize;

    glPointSize(ptSize);
    glEnable(GL_POINT_SMOOTH);
    glEnable(GL_DEPTH_TEST);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glVertexPointer(3, GL_FLOAT, PointCloudData::stride(), data);
    glColorPointer(3, GL_FLOAT, PointCloudData::stride(), data + 3);

    glDrawArrays(GL_POINTS, 0, (GLsizei)numPoints);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    // Draw multi-selected points (yellow highlight)
    if (!_imp->selectedPointIndices.empty()) {
        glPointSize(ptSize * 2.5f);
        glColor3f(1.0f, 1.0f, 0.0f);
        glBegin(GL_POINTS);
        for (int idx : _imp->selectedPointIndices) {
            if (idx >= 0 && idx < (int)numPoints) {
                glVertex3f(data[idx * 6 + 0], data[idx * 6 + 1], data[idx * 6 + 2]);
            }
        }
        glEnd();
    }

    // Draw single selected point (larger + crosshair)
    if (_imp->selectedPointIndex >= 0 &&
        _imp->selectedPointIndex < (int)numPoints &&
        _imp->selectedPointIndices.empty()) {
        int idx = _imp->selectedPointIndex;
        float px = data[idx * 6 + 0];
        float py = data[idx * 6 + 1];
        float pz = data[idx * 6 + 2];

        glPointSize(ptSize * 3.0f);
        glColor3f(1.0f, 1.0f, 0.0f);
        glBegin(GL_POINTS);
        glVertex3f(px, py, pz);
        glEnd();

        float cs = 0.05f;
        glLineWidth(1.5f);
        glColor3f(1.0f, 1.0f, 1.0f);
        glBegin(GL_LINES);
        glVertex3f(px - cs, py, pz); glVertex3f(px + cs, py, pz);
        glVertex3f(px, py - cs, pz); glVertex3f(px, py + cs, pz);
        glVertex3f(px, py, pz - cs); glVertex3f(px, py, pz + cs);
        glEnd();
        glLineWidth(1.0f);
    }

    // Draw blast bounds wireframe (red) if a Blast node exists
    {
        Gui* gui = getGui();
        if (gui) {
            GuiAppInstancePtr app = gui->getApp();
            if (app) {
                ProjectPtr p = app->getProject();
                if (p) {
                    NodesList allNodes;
                    p->getNodes_recursive(allNodes, true);
                    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
                        if (!(*it)->isActivated()) continue;
                        if ((*it)->isNodeDisabled()) continue;
                        EffectInstancePtr eff = (*it)->getEffectInstance();
                        if (!eff) continue;
                        Blast* blast = dynamic_cast<Blast*>(eff.get());
                        if (blast) {
                            float bCenter[3], bExtent[3], bMatrix[16];
                            if (blast->getBlastOBB(0, bCenter, bExtent, bMatrix)) {
                                glLineWidth(2.0f);
                                glColor3f(1.0f, 0.2f, 0.2f); // red
                                // Draw the box in OBB local space — matrix carries
                                // translation + rotation so the wireframe matches a
                                // Cube3D bounds input's rotation in the viewport.
                                glPushMatrix();
                                glMultMatrixf(bMatrix);
                                const float ex = bExtent[0], ey = bExtent[1], ez = bExtent[2];
                                glBegin(GL_LINES);
                                // Bottom face
                                glVertex3f(-ex,-ey,-ez); glVertex3f(+ex,-ey,-ez);
                                glVertex3f(+ex,-ey,-ez); glVertex3f(+ex,-ey,+ez);
                                glVertex3f(+ex,-ey,+ez); glVertex3f(-ex,-ey,+ez);
                                glVertex3f(-ex,-ey,+ez); glVertex3f(-ex,-ey,-ez);
                                // Top face
                                glVertex3f(-ex,+ey,-ez); glVertex3f(+ex,+ey,-ez);
                                glVertex3f(+ex,+ey,-ez); glVertex3f(+ex,+ey,+ez);
                                glVertex3f(+ex,+ey,+ez); glVertex3f(-ex,+ey,+ez);
                                glVertex3f(-ex,+ey,+ez); glVertex3f(-ex,+ey,-ez);
                                // Verticals
                                glVertex3f(-ex,-ey,-ez); glVertex3f(-ex,+ey,-ez);
                                glVertex3f(+ex,-ey,-ez); glVertex3f(+ex,+ey,-ez);
                                glVertex3f(+ex,-ey,+ez); glVertex3f(+ex,+ey,+ez);
                                glVertex3f(-ex,-ey,+ez); glVertex3f(-ex,+ey,+ez);
                                glEnd();
                                glPopMatrix();
                                glLineWidth(1.0f);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    glEnable(GL_DEPTH_TEST);
}

void
Viewport3D::drawMeshNode(const SceneNode& sn) const
{
    // Prefer mesh data carried directly on the SceneNode (set by
    // ReadGeo and ReadAlembicArchive). Fall back to the source-node
    // dynamic_cast for older code paths that haven't been updated.
    MeshDataPtr mesh = sn.meshData;
    if (!mesh) {
        NodePtr node = sn.sourceNode.lock();
        if (!node) return;
        EffectInstancePtr effect = node->getEffectInstance();
        if (!effect) return;
        ReadGeo* readGeo = dynamic_cast<ReadGeo*>(effect.get());
        if (!readGeo) return;
        mesh = readGeo->getMeshData(-1);
    }
    if (!mesh || mesh->numVertices == 0) return;

    const ShadingMode mode = _imp->shadingMode;
    const int nv = (int)mesh->numVertices;

    // ----- Resolve a texture for the shaded fill -----
    // A downstream UVProject (geo -> UVProject) projects its img (input 2) onto this
    // mesh with rewritten PER-VERTEX UVs and takes precedence. Otherwise a connected
    // Material3D's diffuse texture, mapped by the mesh's own PER-FACE-VERTEX UVs.
    NodePtr meshSrc = sn.sourceNode.lock();
    double meshTime = 0.0;
    { Gui* g = getGui(); GuiAppInstancePtr a = g ? g->getApp() : GuiAppInstancePtr();
      if (a) meshTime = a->getTimeLine()->currentFrame(); }
    const float* texPixels = NULL;
    int texW = 0, texH = 0;
    bool texWrapRepeat = true;
    std::vector<float> projUVs, projSTW;  // per-vertex (UVProject rewriteUVs)
    int projComp = 0;                      // 0 = none, 2 = uv, 3 = stw
    bool useOwnUVs = false;                // material texture mapped by mesh->uvs
    const float* projBorderColor = NULL;   // Project3D border: clear=crop, grey=project-on cull, NULL=smear
    std::vector<unsigned char> projVertCulled;  // per-vertex Project3D facing cull (1=culled); empty=none
    std::vector<Project3DLayer> projLayers;     // Project3D / MergeMat projection layers (multi-pass)
    if (meshSrc) {
        Gui* g = getGui();
        GuiAppInstancePtr a = g ? g->getApp() : GuiAppInstancePtr();
        UVProject* uvp = a ? findUVProjectForGeo(a.get(), meshSrc) : NULL;
        if (uvp) {
            uvp->updateCachedTexture(meshTime);
            const UVProject::CachedTexture& ut = uvp->getCachedTexture();
            if (ut.width > 0 && ut.height > 0 && !ut.pixels.empty()) {
                std::vector<float> newUVs, newSTW; int comp = 0;
                uvp->rewriteUVs(mesh->vertices, sn.worldMatrix, meshTime, newUVs, newSTW, comp);
                if (comp == 2 || comp == 3) {
                    texPixels = ut.pixels.data(); texW = ut.width; texH = ut.height;
                    texWrapRepeat = false; projComp = comp;
                    if (comp == 2) projUVs.swap(newUVs); else projSTW.swap(newSTW);
                }
            }
        }
        if (!texPixels) {
            // MeshData carries no normals; build them on demand ONLY when a projecting material
            // might use Project On front/back (a MergeMat: any layer could; a Project3D: only if
            // not Both) — so non-projected / Both-mode meshes pay nothing. Then build the layers.
            EffectInstancePtr meshEff = meshSrc->getEffectInstance();
            std::vector<float> meshNormals;
            {
                MaterialProvider* mp = dynamic_cast<MaterialProvider*>(meshEff.get());
                MaterialProvider* mat = mp ? mp->getConnectedMaterial() : NULL;
                bool needNormals = (dynamic_cast<MergeMat*>(mat) != NULL);
                if (!needNormals)
                    if (Project3D* p = dynamic_cast<Project3D*>(mat))
                        needNormals = (p->getProjectOn(meshTime) != Project3D::eProjectBoth);
                if (needNormals)
                    computeMeshVertexNormals(mesh->vertices, mesh->faceCounts, mesh->faceIndices, meshNormals);
            }
            buildGeoProject3DLayers(meshEff, mesh->vertices, meshNormals, sn.worldMatrix, meshTime, projLayers);
        }
        if (!texPixels && projLayers.empty() && mesh->hasUVs && mesh->texCoordComponents == 2 && !mesh->uvs.empty()) {
            // Per-part archive override first, else the geo's connected material.
            Material3D* m3d = NULL;
            if (NodePtr mn = sn.materialNode.lock())
                m3d = dynamic_cast<Material3D*>(mn->getEffectInstance().get());
            if (!m3d) {
                MaterialProvider* mp = dynamic_cast<MaterialProvider*>(meshSrc->getEffectInstance().get());
                if (mp) m3d = dynamic_cast<Material3D*>(mp->getConnectedMaterial());
            }
            if (m3d) {
                m3d->updateCachedTexture(meshTime);
                const Material3D::CachedTexture& mt = m3d->getCachedTexture();
                if (mt.width > 0 && mt.height > 0 && !mt.pixels.empty()) {
                    texPixels = mt.pixels.data(); texW = mt.width; texH = mt.height;
                    useOwnUVs = true;
                }
            }
        }
    }
    const bool meshHasTex = (texPixels != NULL);

    // ----- Shaded fill (Shaded / Shaded+Wire / Flat) -----
    // Fan-triangulate the polygon-soup mesh via faceCounts. If faceCounts is
    // empty, treat faceIndices as already-triangulated (GL_TRIANGLES every 3).
    if (mode != eWireframe) {
        if (mode == eShadedWire) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        const bool lit = (mode == eShaded || mode == eShadedWire);
        const float baseGrey = 0.45f;
        float mvForLit[16];
        if (lit) glGetFloatv(GL_MODELVIEW_MATRIX, mvForLit);

        // One fill pass: a single UVProject/material texture, the grey fallback (hasTexPass=false),
        // or one MergeMat projection layer. Called once normally, or once per layer (blended).
        auto drawMeshPass = [&](const float* tPix, int tW, int tH, bool wrapRep, const float* border,
                                int pComp, const std::vector<float>& pUVs, const std::vector<float>& pSTW,
                                const std::vector<unsigned char>& vCulled, bool hasTexPass, int blendOp) {
            GLuint glTex = 0;
            if (hasTexPass) {
                glGenTextures(1, &glTex);
                glBindTexture(GL_TEXTURE_2D, glTex);
                const GLint meshWrap = wrapRep ? GL_REPEAT : (border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
                uploadPreviewTextureSRGB(tPix, tW, tH, meshWrap, meshWrap, border);
                glEnable(GL_TEXTURE_2D);
                glEnable(GL_BLEND);
                if (blendOp == 1) glBlendFunc(GL_ONE, GL_ZERO);
                else if (blendOp == 5) glBlendFunc(GL_ONE, GL_ONE);
                else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                if (border) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
            }
            if (!lit && !hasTexPass) glColor3f(baseGrey, baseGrey, baseGrey);
            auto emitVtx = [&](int v, size_t fv, bool culled) {
                if (pComp == 2)      glTexCoord2f(pUVs[v * 2], pUVs[v * 2 + 1]);
                else if (pComp == 3) {
                    if (culled) glTexCoord4f(2.0f, 2.0f, 0.0f, 1.0f);
                    else        glTexCoord4f(pSTW[v * 3], pSTW[v * 3 + 1], 0.0f, pSTW[v * 3 + 2]);
                }
                else if (useOwnUVs && (fv * 2 + 1) < mesh->uvs.size())
                                     glTexCoord2f(mesh->uvs[fv * 2], mesh->uvs[fv * 2 + 1]);
                const float* p = &mesh->vertices[v * 3];
                glVertex3f(p[0], p[1], p[2]);
            };
            glBegin(GL_TRIANGLES);
            auto emitTri = [&](int v0, int v1, int v2, size_t f0, size_t f1, size_t f2) {
                if (lit) {
                    const float* p0 = &mesh->vertices[v0*3];
                    const float* p1 = &mesh->vertices[v1*3];
                    const float* p2 = &mesh->vertices[v2*3];
                    float f = ViewportFaceLitFactor(p0, p1, p2, mvForLit);
                    if (hasTexPass) glColor4f(f, f, f, 0.95f);
                    else            glColor3f(baseGrey * f, baseGrey * f, baseGrey * f);
                } else if (hasTexPass) {
                    glColor4f(1.0f, 1.0f, 1.0f, 0.95f);
                }
                const bool triCulled = !vCulled.empty() &&
                    ( v0 < (int)vCulled.size() && v1 < (int)vCulled.size() &&
                      v2 < (int)vCulled.size() ) &&
                    ( vCulled[v0] || vCulled[v1] || vCulled[v2] );
                emitVtx(v0, f0, triCulled); emitVtx(v1, f1, triCulled); emitVtx(v2, f2, triCulled);
            };
            if (!mesh->faceCounts.empty()) {
                size_t off = 0;
                for (size_t f = 0; f < mesh->faceCounts.size(); ++f) {
                    const int c = mesh->faceCounts[f];
                    if (c < 3 || off + (size_t)c > mesh->faceIndices.size()) {
                        off += (size_t)std::max(0, c);
                        continue;
                    }
                    const int v0 = mesh->faceIndices[off];
                    for (int i = 1; i + 1 < c; ++i) {
                        const int v1 = mesh->faceIndices[off + i];
                        const int v2 = mesh->faceIndices[off + i + 1];
                        if (v0 >= 0 && v0 < nv && v1 >= 0 && v1 < nv && v2 >= 0 && v2 < nv) {
                            emitTri(v0, v1, v2, off, off + i, off + i + 1);
                        }
                    }
                    off += (size_t)c;
                }
            } else {
                for (size_t i = 0; i + 2 < mesh->faceIndices.size(); i += 3) {
                    const int v0 = mesh->faceIndices[i];
                    const int v1 = mesh->faceIndices[i + 1];
                    const int v2 = mesh->faceIndices[i + 2];
                    if (v0 >= 0 && v0 < nv && v1 >= 0 && v1 < nv && v2 >= 0 && v2 < nv) {
                        emitTri(v0, v1, v2, i, i + 1, i + 2);
                    }
                }
            }
            glEnd();
            if (hasTexPass) {
                glDisable(GL_TEXTURE_2D);
                if (border) glDisable(GL_ALPHA_TEST);
                glDisable(GL_BLEND);
                glDeleteTextures(1, &glTex);
            }
        };
        if (!projLayers.empty()) {
            glDepthFunc(GL_LEQUAL);
            for (size_t li = 0; li < projLayers.size(); ++li) {
                const Project3DLayer& L = projLayers[li];
                drawMeshPass(L.texPixels, L.texW, L.texH, false, L.borderColor, 3, projUVs, L.projSTW, L.vertCulled, true, L.op);
            }
            glDepthFunc(GL_LESS);
        } else {
            drawMeshPass(texPixels, texW, texH, texWrapRepeat, projBorderColor, projComp, projUVs, projSTW, projVertCulled, meshHasTex, 2);
        }
        if (mode == eShadedWire) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    // ----- Wireframe (Wireframe / Shaded+Wire) -----
    if ((mode == eWireframe || mode == eShadedWire) && !mesh->edgeIndices.empty()) {
        glColor3f(0.7f, 0.7f, 0.7f);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        for (size_t i = 0; i + 1 < mesh->edgeIndices.size(); i += 2) {
            int i0 = mesh->edgeIndices[i];
            int i1 = mesh->edgeIndices[i + 1];
            if (i0 >= 0 && i0 < nv && i1 >= 0 && i1 < nv) {
                glVertex3f(mesh->vertices[i0*3], mesh->vertices[i0*3+1], mesh->vertices[i0*3+2]);
                glVertex3f(mesh->vertices[i1*3], mesh->vertices[i1*3+1], mesh->vertices[i1*3+2]);
            }
        }
        glEnd();
    }
}

void
Viewport3D::drawCardNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) return;

    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    double time = app->getTimeLine()->currentFrame();

    float halfW = 0.5f, halfH = 0.5f;

    Card3D* card3dNew = dynamic_cast<Card3D*>(effect.get());
    if (!card3dNew) return;

    card3dNew->updateCachedTexture(time);
    const Card3D::CachedTexture& tex = card3dNew->getCachedTexture();

    // Texture + UVs — prefer a downstream UVProject (projects its img onto the card
    // with rewritten UVs), else the card's own cached texture.
    const float* texPixels = NULL;
    int texW = 0, texH = 0;
    bool texWrapRepeat = true;
    std::vector<float> projUVs, projSTW;
    int projComp = 0;
    const float* projBorderColor = NULL;  // Project3D border: clear=crop, grey=project-on cull, NULL=smear
    std::vector<unsigned char> projVertCulled;  // per-vertex Project3D facing cull (1=culled); empty=none
    UVProject* uvpCard = findUVProjectForGeo(app.get(), node);
    if (uvpCard) {
        uvpCard->updateCachedTexture(time);
        const UVProject::CachedTexture& ut = uvpCard->getCachedTexture();
        if (ut.width > 0 && ut.height > 0 && !ut.pixels.empty()) {
            texPixels = ut.pixels.data(); texW = ut.width; texH = ut.height;
            texWrapRepeat = false;
        } else {
            uvpCard = NULL;
        }
    }

    // Project3D / MergeMat material on the card's "mat" input. Resolve the top projection just
    // to size the card (image aspect); the actual projection layers are built below, once halfW
    // is known, and painted multi-pass (so a MergeMat stack composites on the card too).
    std::vector<Project3DLayer> projLayers;
    bool cardProjects = false;
    if (!texPixels) {
        MaterialProvider* mp = dynamic_cast<MaterialProvider*>(effect.get());
        Project3D* topP3d = mp ? resolveViewportProject3D(mp->getConnectedMaterial()) : NULL;
        if (topP3d) {
            topP3d->updateCachedTexture(time);
            const Project3D::CachedTexture& pt = topP3d->getCachedTexture();
            if (pt.width > 0 && pt.height > 0 && !pt.pixels.empty()) {
                texW = pt.width; texH = pt.height;   // for image aspect (halfW); texPixels stays NULL
                cardProjects = true;
            }
        }
    }

    if (!texPixels && !cardProjects && tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
        texPixels = tex.pixels.data(); texW = tex.width; texH = tex.height;
    }
    const bool hasTex = (texPixels != NULL);

    if (!card3dNew->getImageAspectEnabled()) {
        halfW = 0.5f;   // image-aspect off -> unit square (matches generateCardMesh)
    } else if (texW > 0 && texH > 0) {
        halfW = (float)texW / (float)texH * 0.5f;
    } else {
        halfW = 16.0f / 9.0f * 0.5f;
    }

    // For UVProject, rewrite the 4 corner UVs from the card's world-space corners.
    if (uvpCard) {
        float corners[12] = { -halfW, -halfH, 0.0f,  halfW, -halfH, 0.0f,
                               halfW,  halfH, 0.0f, -halfW,  halfH, 0.0f };
        std::vector<float> xyz(corners, corners + 12);
        std::vector<float> newUVs, newSTW; int comp = 0;
        uvpCard->rewriteUVs(xyz, sn.worldMatrix, time, newUVs, newSTW, comp);
        projComp = comp;
        if (comp == 2) projUVs.swap(newUVs);
        else if (comp == 3) projSTW.swap(newSTW);
    }

    // Build the projection layers (Project3D / MergeMat) from the 4 card corners now that halfW
    // is known. computeProject3DLayer handles the perspective-correct corner STW + project-on
    // (the card is flat, normal +Z, so front/back culling is whole-quad / uniform).
    if (cardProjects) {
        const float cc[12] = { -halfW, -halfH, 0.0f,  halfW, -halfH, 0.0f,
                                halfW,  halfH, 0.0f, -halfW,  halfH, 0.0f };
        std::vector<float> cxyz(cc, cc + 12);
        std::vector<float> cnrm(12, 0.0f);
        for (int i = 0; i < 4; ++i) cnrm[i*3+2] = 1.0f;   // card normal +Z (flat)
        MaterialProvider* mp = dynamic_cast<MaterialProvider*>(effect.get());
        buildProject3DLayers(mp ? mp->getConnectedMaterial() : NULL, cxyz, cnrm, sn.worldMatrix,
                             time, MergeMat::eMergeOver, 1.f, projLayers);
    }

    const ShadingMode mode = _imp->shadingMode;

    if (mode != eWireframe) {
        if (mode == eShadedWire) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        // Card is a flat XY plane with normal +Z in local space. Use that
        // directly instead of a cross product so winding doesn't matter.
        const bool lit = (mode == eShaded || mode == eShadedWire);
        float litF = 1.0f;
        if (lit) {
            float mvForLit[16];
            glGetFloatv(GL_MODELVIEW_MATRIX, mvForLit);
            const float n0[3] = { 0.0f, 0.0f, 1.0f };
            litF = ViewportLitFromVertexNormals(n0, n0, n0, mvForLit);
        }
        const float quadU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        const float quadV[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        const float quadX[4] = {-halfW,  halfW,  halfW, -halfW};
        const float quadY[4] = {-halfH, -halfH,  halfH,  halfH};
        // One textured quad pass (single texture, or one MergeMat layer blended).
        auto drawCardQuad = [&](const float* tPix, int tW, int tH, bool wrapRep, const float* border,
                                int pComp, const std::vector<float>& pUVs, const std::vector<float>& pSTW,
                                const std::vector<unsigned char>& vCulled, int blendOp) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            const GLint wrap = wrapRep ? GL_REPEAT : (border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
            uploadPreviewTextureSRGB(tPix, tW, tH, wrap, wrap, border);
            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            if (blendOp == 1) glBlendFunc(GL_ONE, GL_ZERO);
            else if (blendOp == 5) glBlendFunc(GL_ONE, GL_ONE);
            else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (border) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
            glColor4f(litF, litF, litF, 0.85f);
            // Flat card: project-on cull is whole-quad (uniform facing).
            const bool quadCulled = (vCulled.size() >= 4) &&
                (vCulled[0] || vCulled[1] || vCulled[2] || vCulled[3]);
            glBegin(GL_QUADS);
            for (int ci = 0; ci < 4; ++ci) {
                if (pComp == 2)
                    glTexCoord2f(pUVs[ci * 2], pUVs[ci * 2 + 1]);
                else if (pComp == 3) {
                    if (quadCulled) glTexCoord4f(2.0f, 2.0f, 0.0f, 1.0f);
                    else glTexCoord4f(pSTW[ci * 3], pSTW[ci * 3 + 1], 0.0f, pSTW[ci * 3 + 2]);
                } else
                    glTexCoord2f(quadU[ci], quadV[ci]);
                glVertex3f(quadX[ci], quadY[ci], 0.0f);
            }
            glEnd();
            glDisable(GL_TEXTURE_2D);
            if (border) glDisable(GL_ALPHA_TEST);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
        };
        if (!projLayers.empty()) {
            glDepthFunc(GL_LEQUAL);
            for (size_t li = 0; li < projLayers.size(); ++li) {
                const Project3DLayer& L = projLayers[li];
                drawCardQuad(L.texPixels, L.texW, L.texH, false, L.borderColor, 3, projUVs, L.projSTW, L.vertCulled, L.op);
            }
            glDepthFunc(GL_LESS);
        } else if (hasTex) {
            drawCardQuad(texPixels, texW, texH, texWrapRepeat, projBorderColor, projComp, projUVs, projSTW, projVertCulled, 2);
        } else {
            glColor3f(0.45f * litF, 0.45f * litF, 0.45f * litF);
            glBegin(GL_QUADS);
            glVertex3f(-halfW, -halfH, 0);
            glVertex3f( halfW, -halfH, 0);
            glVertex3f( halfW,  halfH, 0);
            glVertex3f(-halfW,  halfH, 0);
            glEnd();
        }
        if (mode == eShadedWire) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    if ((mode == eWireframe || mode == eShadedWire)) {
        bool selected = (sn.name == _imp->selectedNodeName);
        glColor3f(selected ? 1.0f : 0.2f, selected ? 1.0f : 0.8f, selected ? 0.0f : 0.2f);
        glLineWidth(2.0f);
        glBegin(GL_LINE_LOOP);
        glVertex3f(-halfW, -halfH, 0.0f);
        glVertex3f( halfW, -halfH, 0.0f);
        glVertex3f( halfW,  halfH, 0.0f);
        glVertex3f(-halfW,  halfH, 0.0f);
        glEnd();
        glLineWidth(1.0f);
    }
}

void
Viewport3D::drawCameraNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) return;

    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    double time = app->getTimeLine()->currentFrame();

    double focalLength = 50.0;
    double hAperture = 24.576;
    float frustumLength = 3.0f;

    // Per-camera viewport frustum length (Camera3D "Frustum Display Length" knob).
    // Display-only; cameras without the knob (e.g. ReadAlembicCamera) keep 3.0.
    {
        KnobIPtr fk = effect->getKnobByName("frustumDisplayLength");
        if (fk) {
            KnobDouble* fd = dynamic_cast<KnobDouble*>(fk.get());
            if (fd) frustumLength = (float)fd->getValueAtTime(time);
        }
    }

    CameraProvider* camProvider = dynamic_cast<CameraProvider*>(effect.get());
    if (camProvider) {
        focalLength = camProvider->getCameraFocalLength(time);
        hAperture = camProvider->getCameraHAperture(time);
    }

    bool isStandaloneCamera = (camProvider != NULL);

    double fovRad = 2.0 * std::atan(hAperture / (2.0 * focalLength));
    float halfW = frustumLength * (float)std::tan(fovRad * 0.5);
    float halfH = halfW * 0.75f;

    if (isStandaloneCamera) {
        glColor3f(1.0f, 0.8f, 0.2f);
        glLineWidth(1.5f);

        float bs = 0.3f;
        glBegin(GL_LINE_STRIP);
        glVertex3f(-bs, -bs, -bs); glVertex3f( bs, -bs, -bs);
        glVertex3f( bs,  bs, -bs); glVertex3f(-bs,  bs, -bs);
        glVertex3f(-bs, -bs, -bs);
        glEnd();
        glBegin(GL_LINE_STRIP);
        glVertex3f(-bs, -bs, bs); glVertex3f( bs, -bs, bs);
        glVertex3f( bs,  bs, bs); glVertex3f(-bs,  bs, bs);
        glVertex3f(-bs, -bs, bs);
        glEnd();
        glBegin(GL_LINES);
        glVertex3f(-bs, -bs, -bs); glVertex3f(-bs, -bs, bs);
        glVertex3f( bs, -bs, -bs); glVertex3f( bs, -bs, bs);
        glVertex3f( bs,  bs, -bs); glVertex3f( bs,  bs, bs);
        glVertex3f(-bs,  bs, -bs); glVertex3f(-bs,  bs, bs);
        glEnd();

        glColor3f(0.2f, 0.8f, 0.2f);
        glBegin(GL_LINE_STRIP);
        glVertex3f(-bs * 0.5f, bs, 0);
        glVertex3f(0, bs + bs * 0.7f, 0);
        glVertex3f(bs * 0.5f, bs, 0);
        glEnd();
    }

    float frustumColor = isStandaloneCamera ? 0.6f : 0.8f;
    Q_UNUSED(frustumColor);
    glColor3f(isStandaloneCamera ? 0.0f : 0.2f, isStandaloneCamera ? 0.8f : 0.8f, isStandaloneCamera ? 0.8f : 0.8f);
    glLineWidth(1.5f);

    glBegin(GL_LINES);
    glVertex3f(0, 0, 0); glVertex3f(-halfW, -halfH, -frustumLength);
    glVertex3f(0, 0, 0); glVertex3f( halfW, -halfH, -frustumLength);
    glVertex3f(0, 0, 0); glVertex3f( halfW,  halfH, -frustumLength);
    glVertex3f(0, 0, 0); glVertex3f(-halfW,  halfH, -frustumLength);
    glEnd();

    glBegin(GL_LINE_LOOP);
    glVertex3f(-halfW, -halfH, -frustumLength);
    glVertex3f( halfW, -halfH, -frustumLength);
    glVertex3f( halfW,  halfH, -frustumLength);
    glVertex3f(-halfW,  halfH, -frustumLength);
    glEnd();

    glLineWidth(1.0f);
}

void
Viewport3D::drawSphereNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    Sphere3D* sphere = effect ? dynamic_cast<Sphere3D*>(effect.get()) : NULL;
    if (!sphere) return;

    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    double time = app->getTimeLine()->currentFrame();

    sphere->updateCachedTexture(time);

    std::vector<Sphere3D::SphereVertex> sphereVerts;
    std::vector<int> triIndices;
    sphere->generateSphereMesh(time, sphereVerts, triIndices);

    int rows = sphere->getRows(time);
    int cols = sphere->getColumns(time);
    int vertsPerRow = cols + 1;
    int numTris = (int)(triIndices.size() / 3);

    const ShadingMode mode = _imp->shadingMode;

    // Texture + UVs. A downstream UVProject (geo -> UVProject) projects its img
    // (input 2) onto this geo with rewritten UVs — prefer it so the projection
    // shows in the viewport. Otherwise use the sphere's own cached texture.
    const float* texPixels = NULL;
    int texW = 0, texH = 0;
    bool texWrapRepeat = true;
    std::vector<float> projUVs;   // 2/vert (standard projection modes)
    std::vector<float> projSTW;   // 3/vert (perspective mode)
    int projComp = 0;
    const float* projBorderColor = NULL;  // Project3D border: clear=crop, grey=project-on cull, NULL=smear
    std::vector<unsigned char> projVertCulled;  // per-vertex Project3D facing cull (1=culled); empty=none
    std::vector<Project3DLayer> projLayers;     // Project3D / MergeMat projection layers (multi-pass)
    {
        UVProject* uvp = findUVProjectForGeo(app.get(), node);
        if (uvp) {
            uvp->updateCachedTexture(time);
            const UVProject::CachedTexture& ut = uvp->getCachedTexture();
            if (ut.width > 0 && ut.height > 0 && !ut.pixels.empty()) {
                std::vector<float> xyz; xyz.reserve(sphereVerts.size() * 3);
                for (size_t vi = 0; vi < sphereVerts.size(); ++vi) {
                    xyz.push_back(sphereVerts[vi].x);
                    xyz.push_back(sphereVerts[vi].y);
                    xyz.push_back(sphereVerts[vi].z);
                }
                std::vector<float> newUVs, newSTW; int comp = 0;
                uvp->rewriteUVs(xyz, sn.worldMatrix, time, newUVs, newSTW, comp);
                if (comp == 2 || comp == 3) {
                    texPixels = ut.pixels.data(); texW = ut.width; texH = ut.height;
                    texWrapRepeat = false; projComp = comp;
                    if (comp == 2) projUVs.swap(newUVs); else projSTW.swap(newSTW);
                }
            }
        }
        if (!texPixels) {
            std::vector<float> xyz; xyz.reserve(sphereVerts.size() * 3);
            std::vector<float> nrm; nrm.reserve(sphereVerts.size() * 3);
            for (size_t vi = 0; vi < sphereVerts.size(); ++vi) {
                xyz.push_back(sphereVerts[vi].x); xyz.push_back(sphereVerts[vi].y); xyz.push_back(sphereVerts[vi].z);
                nrm.push_back(sphereVerts[vi].nx); nrm.push_back(sphereVerts[vi].ny); nrm.push_back(sphereVerts[vi].nz);
            }
            buildGeoProject3DLayers(effect, xyz, nrm, sn.worldMatrix, time, projLayers);
        }
        if (!texPixels && projLayers.empty()) {
            const Sphere3D::CachedTexture& tex = sphere->getCachedTexture();
            if (tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
                texPixels = tex.pixels.data(); texW = tex.width; texH = tex.height;
            }
        }
    }
    const bool hasTex = (texPixels != NULL);

    // ----- Shaded fill -----
    if (mode != eWireframe) {
        if (mode == eShadedWire) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        const bool lit = (mode == eShaded || mode == eShadedWire);
        float mvForLit[16];
        if (lit) glGetFloatv(GL_MODELVIEW_MATRIX, mvForLit);
        // Sphere carries per-vertex normals (sphereVerts[i].nx/ny/nz) so use
        // those directly — winding-agnostic, always points outward.
        auto litForTri = [&](int i0, int i1, int i2) -> float {
            if (!lit) return 1.0f;
            const float n0[3] = { sphereVerts[i0].nx, sphereVerts[i0].ny, sphereVerts[i0].nz };
            const float n1[3] = { sphereVerts[i1].nx, sphereVerts[i1].ny, sphereVerts[i1].nz };
            const float n2[3] = { sphereVerts[i2].nx, sphereVerts[i2].ny, sphereVerts[i2].nz };
            return ViewportLitFromVertexNormals(n0, n1, n2, mvForLit);
        };
        // One textured fill pass for a given plate + projected coords. Called once for a single
        // texture (UVProject / material), or once per layer for a MergeMat stack (blended so the
        // layers composite, mirroring the ScanlineRender shader).
        auto drawTexFill = [&](const float* tPix, int tW, int tH, bool wrapRep, const float* border,
                               int pComp, const std::vector<float>& pUVs, const std::vector<float>& pSTW,
                               const std::vector<unsigned char>& vCulled, int blendOp) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            const GLint wrap = wrapRep ? GL_REPEAT : (border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
            uploadPreviewTextureSRGB(tPix, tW, tH, wrap, wrap, border);
            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            // MergeMat op -> GL blend. 'over' is exact; replace/plus approximated; others -> over.
            if (blendOp == 1) glBlendFunc(GL_ONE, GL_ZERO);           // replace
            else if (blendOp == 5) glBlendFunc(GL_ONE, GL_ONE);       // plus
            else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // over (+ stencil/mask/min/max approx)
            if (border) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
            if (!lit) glColor4f(1.0f, 1.0f, 1.0f, 0.85f);
            glBegin(GL_TRIANGLES);
            for (int t = 0; t < numTris; ++t) {
                const int i0 = triIndices[t * 3 + 0];
                const int i1 = triIndices[t * 3 + 1];
                const int i2 = triIndices[t * 3 + 2];
                if (i0 < 0 || i0 >= (int)sphereVerts.size() ||
                    i1 < 0 || i1 >= (int)sphereVerts.size() ||
                    i2 < 0 || i2 >= (int)sphereVerts.size()) continue;
                if (lit) { float f = litForTri(i0, i1, i2); glColor4f(f, f, f, 0.85f); }
                const bool triCulled = !vCulled.empty() &&
                    (vCulled[i0] || vCulled[i1] || vCulled[i2]);
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    if (pComp == 2)
                        glTexCoord2f(pUVs[idx * 2], pUVs[idx * 2 + 1]);
                    else if (pComp == 3) {
                        if (triCulled) glTexCoord4f(2.0f, 2.0f, 0.0f, 1.0f);
                        else glTexCoord4f(pSTW[idx * 3], pSTW[idx * 3 + 1], 0.0f, pSTW[idx * 3 + 2]);
                    } else
                        glTexCoord2f(sphereVerts[idx].u, sphereVerts[idx].v);
                    glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
                }
            }
            glEnd();
            glDisable(GL_TEXTURE_2D);
            if (border) glDisable(GL_ALPHA_TEST);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
        };
        if (!projLayers.empty()) {
            // Multi-pass: paint each projection layer back-to-front, blended. LEQUAL lets each
            // layer draw at the same depth as the one below it (alpha-test drops cropped pixels
            // so they don't write depth and lower/background layers show through).
            glDepthFunc(GL_LEQUAL);
            for (size_t li = 0; li < projLayers.size(); ++li) {
                const Project3DLayer& L = projLayers[li];
                drawTexFill(L.texPixels, L.texW, L.texH, false, L.borderColor, 3, projUVs, L.projSTW, L.vertCulled, L.op);
            }
            glDepthFunc(GL_LESS);
            // Crop the wireframe (below) to the top (foreground) layer.
            const Project3DLayer& top = projLayers.back();
            projComp = 3; projBorderColor = top.borderColor;
            projSTW = top.projSTW; projVertCulled = top.vertCulled;
        } else if (hasTex) {
            drawTexFill(texPixels, texW, texH, texWrapRepeat, projBorderColor, projComp, projUVs, projSTW, projVertCulled, 2);
        } else {
            // Grey fallback (no texture present)
            const float baseGrey = 0.45f;
            if (!lit) glColor3f(baseGrey, baseGrey, baseGrey);
            glBegin(GL_TRIANGLES);
            for (int t = 0; t < numTris; ++t) {
                const int i0 = triIndices[t * 3 + 0];
                const int i1 = triIndices[t * 3 + 1];
                const int i2 = triIndices[t * 3 + 2];
                if (i0 < 0 || i0 >= (int)sphereVerts.size() ||
                    i1 < 0 || i1 >= (int)sphereVerts.size() ||
                    i2 < 0 || i2 >= (int)sphereVerts.size()) continue;
                if (lit) {
                    float f = litForTri(i0, i1, i2);
                    glColor3f(baseGrey * f, baseGrey * f, baseGrey * f);
                }
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
                }
            }
            glEnd();
        }
        if (mode == eShadedWire) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    // ----- Wireframe -----
    if ((mode == eWireframe || mode == eShadedWire)) {
        bool selected = (sn.name == _imp->selectedNodeName);
        if (selected) {
            glColor3f(1.0f, 1.0f, 0.0f);
        } else {
            glColor3f(0.3f, 0.6f, 0.3f);
        }
        glLineWidth(selected ? 2.0f : 1.0f);

        int rowStep = std::max(1, rows / 12);
        int colStep = std::max(1, cols / 12);

        // Crop the wireframe to the projected region when a Project3D crop is active, so the
        // cropped-away part of the geo doesn't leave stray wireframe lines. Only for the clear
        // (crop) border — a grey/NULL border means the geo stays visible, so the wireframe does
        // too. A strip is broken whenever a vertex falls outside the projection (per-vertex
        // granularity, so the cut is at wireframe resolution, not the exact frame edge).
        const bool cropWire = (projBorderColor == kProject3DBorderClear) && (projComp == 3);
        // Emit a line-strip over the index sequence, restarting around culled/out-of-frame verts.
        auto emitWireRow = [&](int row) {
            bool open = false;
            for (int col = 0; col <= cols; ++col) {
                int idx = row * vertsPerRow + col;
                if ( idx >= (int)sphereVerts.size() ||
                     (cropWire && !project3DVertVisible(projSTW, idx)) ) {
                    if (open) { glEnd(); open = false; }
                    continue;
                }
                if (!open) { glBegin(GL_LINE_STRIP); open = true; }
                glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
            }
            if (open) glEnd();
        };
        auto emitWireCol = [&](int col) {
            bool open = false;
            for (int row = 0; row <= rows; ++row) {
                int idx = row * vertsPerRow + col;
                if ( idx >= (int)sphereVerts.size() ||
                     (cropWire && !project3DVertVisible(projSTW, idx)) ) {
                    if (open) { glEnd(); open = false; }
                    continue;
                }
                if (!open) { glBegin(GL_LINE_STRIP); open = true; }
                glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
            }
            if (open) glEnd();
        };

        for (int row = 0; row <= rows; row += rowStep) emitWireRow(row);
        for (int col = 0; col <= cols; col += colStep) emitWireCol(col);

        glLineWidth(1.0f);
    }
}

void
Viewport3D::drawCubeNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    Cube3D* cube = effect ? dynamic_cast<Cube3D*>(effect.get()) : NULL;
    if (!cube) return;

    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    double time = app->getTimeLine()->currentFrame();

    cube->updateCachedTexture(time);

    std::vector<Cube3D::CubeVertex> cubeVerts;
    std::vector<int> triIndices;
    cube->generateCubeMesh(time, cubeVerts, triIndices);
    int numTris = (int)(triIndices.size() / 3);

    const ShadingMode mode = _imp->shadingMode;

    // Texture + UVs — prefer a downstream UVProject (projects its img with rewritten
    // UVs), else the cube's own cached texture (incl. a connected material).
    const float* texPixels = NULL;
    int texW = 0, texH = 0;
    bool texWrapRepeat = true;
    std::vector<float> projUVs, projSTW;
    int projComp = 0;
    const float* projBorderColor = NULL;  // Project3D border: clear=crop, grey=project-on cull, NULL=smear
    std::vector<unsigned char> projVertCulled;  // per-vertex Project3D facing cull (1=culled); empty=none
    std::vector<Project3DLayer> projLayers;     // Project3D / MergeMat projection layers (multi-pass)
    {
        UVProject* uvp = findUVProjectForGeo(app.get(), node);
        if (uvp) {
            uvp->updateCachedTexture(time);
            const UVProject::CachedTexture& ut = uvp->getCachedTexture();
            if (ut.width > 0 && ut.height > 0 && !ut.pixels.empty()) {
                std::vector<float> xyz; xyz.reserve(cubeVerts.size() * 3);
                for (size_t vi = 0; vi < cubeVerts.size(); ++vi) {
                    xyz.push_back(cubeVerts[vi].x); xyz.push_back(cubeVerts[vi].y); xyz.push_back(cubeVerts[vi].z);
                }
                std::vector<float> newUVs, newSTW; int comp = 0;
                uvp->rewriteUVs(xyz, sn.worldMatrix, time, newUVs, newSTW, comp);
                if (comp == 2 || comp == 3) {
                    texPixels = ut.pixels.data(); texW = ut.width; texH = ut.height;
                    texWrapRepeat = false; projComp = comp;
                    if (comp == 2) projUVs.swap(newUVs); else projSTW.swap(newSTW);
                }
            }
        }
        if (!texPixels) {
            std::vector<float> xyz; xyz.reserve(cubeVerts.size() * 3);
            std::vector<float> nrm; nrm.reserve(cubeVerts.size() * 3);
            for (size_t vi = 0; vi < cubeVerts.size(); ++vi) {
                xyz.push_back(cubeVerts[vi].x); xyz.push_back(cubeVerts[vi].y); xyz.push_back(cubeVerts[vi].z);
                nrm.push_back(cubeVerts[vi].nx); nrm.push_back(cubeVerts[vi].ny); nrm.push_back(cubeVerts[vi].nz);
            }
            buildGeoProject3DLayers(effect, xyz, nrm, sn.worldMatrix, time, projLayers);
        }
        if (!texPixels && projLayers.empty()) {
            const Cube3D::CachedTexture& tex = cube->getCachedTexture();
            if (tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
                texPixels = tex.pixels.data(); texW = tex.width; texH = tex.height;
            }
        }
    }
    const bool hasTex = (texPixels != NULL);

    if (mode != eWireframe) {
        if (mode == eShadedWire) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        const bool lit = (mode == eShaded || mode == eShadedWire);
        float mvForLit[16];
        if (lit) glGetFloatv(GL_MODELVIEW_MATRIX, mvForLit);
        // Cube carries per-vertex normals (cubeVerts[i].nx/ny/nz).
        auto litForTri = [&](int i0, int i1, int i2) -> float {
            if (!lit) return 1.0f;
            const float n0[3] = { cubeVerts[i0].nx, cubeVerts[i0].ny, cubeVerts[i0].nz };
            const float n1[3] = { cubeVerts[i1].nx, cubeVerts[i1].ny, cubeVerts[i1].nz };
            const float n2[3] = { cubeVerts[i2].nx, cubeVerts[i2].ny, cubeVerts[i2].nz };
            return ViewportLitFromVertexNormals(n0, n1, n2, mvForLit);
        };
        auto drawTexFill = [&](const float* tPix, int tW, int tH, bool wrapRep, const float* border,
                               int pComp, const std::vector<float>& pUVs, const std::vector<float>& pSTW,
                               const std::vector<unsigned char>& vCulled, int blendOp) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            const GLint wrap = wrapRep ? GL_REPEAT : (border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
            uploadPreviewTextureSRGB(tPix, tW, tH, wrap, wrap, border);
            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            if (blendOp == 1) glBlendFunc(GL_ONE, GL_ZERO);
            else if (blendOp == 5) glBlendFunc(GL_ONE, GL_ONE);
            else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (border) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
            if (!lit) glColor4f(1.0f, 1.0f, 1.0f, 0.85f);
            glBegin(GL_TRIANGLES);
            for (int t = 0; t < numTris; ++t) {
                const int i0 = triIndices[t * 3 + 0];
                const int i1 = triIndices[t * 3 + 1];
                const int i2 = triIndices[t * 3 + 2];
                if (i0 < 0 || i0 >= (int)cubeVerts.size() ||
                    i1 < 0 || i1 >= (int)cubeVerts.size() ||
                    i2 < 0 || i2 >= (int)cubeVerts.size()) continue;
                if (lit) { float f = litForTri(i0, i1, i2); glColor4f(f, f, f, 0.85f); }
                const bool triCulled = !vCulled.empty() &&
                    (vCulled[i0] || vCulled[i1] || vCulled[i2]);
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    if (pComp == 2)
                        glTexCoord2f(pUVs[idx * 2], pUVs[idx * 2 + 1]);
                    else if (pComp == 3) {
                        if (triCulled) glTexCoord4f(2.0f, 2.0f, 0.0f, 1.0f);
                        else glTexCoord4f(pSTW[idx * 3], pSTW[idx * 3 + 1], 0.0f, pSTW[idx * 3 + 2]);
                    } else
                        glTexCoord2f(cubeVerts[idx].u, cubeVerts[idx].v);
                    glVertex3f(cubeVerts[idx].x, cubeVerts[idx].y, cubeVerts[idx].z);
                }
            }
            glEnd();
            glDisable(GL_TEXTURE_2D);
            if (border) glDisable(GL_ALPHA_TEST);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
        };
        if (!projLayers.empty()) {
            glDepthFunc(GL_LEQUAL);
            for (size_t li = 0; li < projLayers.size(); ++li) {
                const Project3DLayer& L = projLayers[li];
                drawTexFill(L.texPixels, L.texW, L.texH, false, L.borderColor, 3, projUVs, L.projSTW, L.vertCulled, L.op);
            }
            glDepthFunc(GL_LESS);
            const Project3DLayer& top = projLayers.back();
            projComp = 3; projBorderColor = top.borderColor;
            projSTW = top.projSTW; projVertCulled = top.vertCulled;
        } else if (hasTex) {
            drawTexFill(texPixels, texW, texH, texWrapRepeat, projBorderColor, projComp, projUVs, projSTW, projVertCulled, 2);
        } else {
            const float baseGrey = 0.45f;
            if (!lit) glColor3f(baseGrey, baseGrey, baseGrey);
            glBegin(GL_TRIANGLES);
            for (int t = 0; t < numTris; ++t) {
                const int i0 = triIndices[t * 3 + 0];
                const int i1 = triIndices[t * 3 + 1];
                const int i2 = triIndices[t * 3 + 2];
                if (i0 < 0 || i0 >= (int)cubeVerts.size() ||
                    i1 < 0 || i1 >= (int)cubeVerts.size() ||
                    i2 < 0 || i2 >= (int)cubeVerts.size()) continue;
                if (lit) {
                    float f = litForTri(i0, i1, i2);
                    glColor3f(baseGrey * f, baseGrey * f, baseGrey * f);
                }
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    glVertex3f(cubeVerts[idx].x, cubeVerts[idx].y, cubeVerts[idx].z);
                }
            }
            glEnd();
        }
        if (mode == eShadedWire) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    if ((mode == eWireframe || mode == eShadedWire)) {
        bool selected = (sn.name == _imp->selectedNodeName);
        glColor3f(selected ? 1.0f : 0.5f, selected ? 1.0f : 0.5f, selected ? 0.0f : 0.8f);
        glLineWidth(selected ? 2.0f : 1.0f);

        float s = (float)cube->getSize(time) * 0.5f;
        glBegin(GL_LINE_LOOP);
        glVertex3f(-s, -s,  s); glVertex3f( s, -s,  s); glVertex3f( s,  s,  s); glVertex3f(-s,  s,  s);
        glEnd();
        glBegin(GL_LINE_LOOP);
        glVertex3f(-s, -s, -s); glVertex3f( s, -s, -s); glVertex3f( s,  s, -s); glVertex3f(-s,  s, -s);
        glEnd();
        glBegin(GL_LINES);
        glVertex3f(-s, -s, -s); glVertex3f(-s, -s,  s);
        glVertex3f( s, -s, -s); glVertex3f( s, -s,  s);
        glVertex3f( s,  s, -s); glVertex3f( s,  s,  s);
        glVertex3f(-s,  s, -s); glVertex3f(-s,  s,  s);
        glEnd();

        glLineWidth(1.0f);
    }
}

void
Viewport3D::drawCylinderNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    Cylinder3D* cyl = effect ? dynamic_cast<Cylinder3D*>(effect.get()) : NULL;
    if (!cyl) return;

    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    double time = app->getTimeLine()->currentFrame();

    cyl->updateCachedTexture(time);

    std::vector<Cylinder3D::CylinderVertex> cylVerts;
    std::vector<int> triIndices;
    cyl->generateCylinderMesh(time, cylVerts, triIndices);
    int numTris = (int)(triIndices.size() / 3);

    const ShadingMode mode = _imp->shadingMode;

    // Texture + UVs — prefer a downstream UVProject (projects its img with rewritten
    // UVs), else the cylinder's own cached texture (incl. a connected material).
    const float* texPixels = NULL;
    int texW = 0, texH = 0;
    bool texWrapRepeat = true;
    std::vector<float> projUVs, projSTW;
    int projComp = 0;
    const float* projBorderColor = NULL;  // Project3D border: clear=crop, grey=project-on cull, NULL=smear
    std::vector<unsigned char> projVertCulled;  // per-vertex Project3D facing cull (1=culled); empty=none
    std::vector<Project3DLayer> projLayers;     // Project3D / MergeMat projection layers (multi-pass)
    {
        UVProject* uvp = findUVProjectForGeo(app.get(), node);
        if (uvp) {
            uvp->updateCachedTexture(time);
            const UVProject::CachedTexture& ut = uvp->getCachedTexture();
            if (ut.width > 0 && ut.height > 0 && !ut.pixels.empty()) {
                std::vector<float> xyz; xyz.reserve(cylVerts.size() * 3);
                for (size_t vi = 0; vi < cylVerts.size(); ++vi) {
                    xyz.push_back(cylVerts[vi].x); xyz.push_back(cylVerts[vi].y); xyz.push_back(cylVerts[vi].z);
                }
                std::vector<float> newUVs, newSTW; int comp = 0;
                uvp->rewriteUVs(xyz, sn.worldMatrix, time, newUVs, newSTW, comp);
                if (comp == 2 || comp == 3) {
                    texPixels = ut.pixels.data(); texW = ut.width; texH = ut.height;
                    texWrapRepeat = false; projComp = comp;
                    if (comp == 2) projUVs.swap(newUVs); else projSTW.swap(newSTW);
                }
            }
        }
        if (!texPixels) {
            std::vector<float> xyz; xyz.reserve(cylVerts.size() * 3);
            std::vector<float> nrm; nrm.reserve(cylVerts.size() * 3);
            for (size_t vi = 0; vi < cylVerts.size(); ++vi) {
                xyz.push_back(cylVerts[vi].x); xyz.push_back(cylVerts[vi].y); xyz.push_back(cylVerts[vi].z);
                nrm.push_back(cylVerts[vi].nx); nrm.push_back(cylVerts[vi].ny); nrm.push_back(cylVerts[vi].nz);
            }
            buildGeoProject3DLayers(effect, xyz, nrm, sn.worldMatrix, time, projLayers);
        }
        if (!texPixels && projLayers.empty()) {
            const Cylinder3D::CachedTexture& tex = cyl->getCachedTexture();
            if (tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
                texPixels = tex.pixels.data(); texW = tex.width; texH = tex.height;
            }
        }
    }
    const bool hasTex = (texPixels != NULL);

    if (mode != eWireframe) {
        if (mode == eShadedWire) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        const bool lit = (mode == eShaded || mode == eShadedWire);
        float mvForLit[16];
        if (lit) glGetFloatv(GL_MODELVIEW_MATRIX, mvForLit);
        auto litForTri = [&](int i0, int i1, int i2) -> float {
            if (!lit) return 1.0f;
            const float n0[3] = { cylVerts[i0].nx, cylVerts[i0].ny, cylVerts[i0].nz };
            const float n1[3] = { cylVerts[i1].nx, cylVerts[i1].ny, cylVerts[i1].nz };
            const float n2[3] = { cylVerts[i2].nx, cylVerts[i2].ny, cylVerts[i2].nz };
            return ViewportLitFromVertexNormals(n0, n1, n2, mvForLit);
        };
        auto drawTexFill = [&](const float* tPix, int tW, int tH, bool wrapRep, const float* border,
                               int pComp, const std::vector<float>& pUVs, const std::vector<float>& pSTW,
                               const std::vector<unsigned char>& vCulled, int blendOp) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            const GLint wrap = wrapRep ? GL_REPEAT : (border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
            uploadPreviewTextureSRGB(tPix, tW, tH, wrap, wrap, border);
            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            if (blendOp == 1) glBlendFunc(GL_ONE, GL_ZERO);
            else if (blendOp == 5) glBlendFunc(GL_ONE, GL_ONE);
            else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (border) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
            if (!lit) glColor4f(1.0f, 1.0f, 1.0f, 0.85f);
            glBegin(GL_TRIANGLES);
            for (int t = 0; t < numTris; ++t) {
                const int i0 = triIndices[t * 3 + 0];
                const int i1 = triIndices[t * 3 + 1];
                const int i2 = triIndices[t * 3 + 2];
                if (i0 < 0 || i0 >= (int)cylVerts.size() ||
                    i1 < 0 || i1 >= (int)cylVerts.size() ||
                    i2 < 0 || i2 >= (int)cylVerts.size()) continue;
                if (lit) { float f = litForTri(i0, i1, i2); glColor4f(f, f, f, 0.85f); }
                const bool triCulled = !vCulled.empty() &&
                    (vCulled[i0] || vCulled[i1] || vCulled[i2]);
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    if (pComp == 2)
                        glTexCoord2f(pUVs[idx * 2], pUVs[idx * 2 + 1]);
                    else if (pComp == 3) {
                        if (triCulled) glTexCoord4f(2.0f, 2.0f, 0.0f, 1.0f);
                        else glTexCoord4f(pSTW[idx * 3], pSTW[idx * 3 + 1], 0.0f, pSTW[idx * 3 + 2]);
                    } else
                        glTexCoord2f(cylVerts[idx].u, cylVerts[idx].v);
                    glVertex3f(cylVerts[idx].x, cylVerts[idx].y, cylVerts[idx].z);
                }
            }
            glEnd();
            glDisable(GL_TEXTURE_2D);
            if (border) glDisable(GL_ALPHA_TEST);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
        };
        if (!projLayers.empty()) {
            glDepthFunc(GL_LEQUAL);
            for (size_t li = 0; li < projLayers.size(); ++li) {
                const Project3DLayer& L = projLayers[li];
                drawTexFill(L.texPixels, L.texW, L.texH, false, L.borderColor, 3, projUVs, L.projSTW, L.vertCulled, L.op);
            }
            glDepthFunc(GL_LESS);
            const Project3DLayer& top = projLayers.back();
            projComp = 3; projBorderColor = top.borderColor;
            projSTW = top.projSTW; projVertCulled = top.vertCulled;
        } else if (hasTex) {
            drawTexFill(texPixels, texW, texH, texWrapRepeat, projBorderColor, projComp, projUVs, projSTW, projVertCulled, 2);
        } else {
            const float baseGrey = 0.45f;
            if (!lit) glColor3f(baseGrey, baseGrey, baseGrey);
            glBegin(GL_TRIANGLES);
            for (int t = 0; t < numTris; ++t) {
                const int i0 = triIndices[t * 3 + 0];
                const int i1 = triIndices[t * 3 + 1];
                const int i2 = triIndices[t * 3 + 2];
                if (i0 < 0 || i0 >= (int)cylVerts.size() ||
                    i1 < 0 || i1 >= (int)cylVerts.size() ||
                    i2 < 0 || i2 >= (int)cylVerts.size()) continue;
                if (lit) { float f = litForTri(i0, i1, i2); glColor3f(baseGrey * f, baseGrey * f, baseGrey * f); }
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    glVertex3f(cylVerts[idx].x, cylVerts[idx].y, cylVerts[idx].z);
                }
            }
            glEnd();
        }
        if (mode == eShadedWire) {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    if ((mode == eWireframe || mode == eShadedWire)) {
        bool selected = (sn.name == _imp->selectedNodeName);
        glColor3f(selected ? 1.0f : 0.5f, selected ? 1.0f : 0.8f, selected ? 0.0f : 0.5f);
        glLineWidth(selected ? 2.0f : 1.0f);

        float r = (float)cyl->getRadius(time);
        float halfH = (float)cyl->getHeight(time) * 0.5f;
        int cols = cyl->getColumns(time);

        glBegin(GL_LINE_LOOP);
        for (int c = 0; c < cols; ++c) {
            float theta = (float)c / (float)cols * 2.0f * (float)M_PI;
            glVertex3f(r * sinf(theta), halfH, r * cosf(theta));
        }
        glEnd();

        glBegin(GL_LINE_LOOP);
        for (int c = 0; c < cols; ++c) {
            float theta = (float)c / (float)cols * 2.0f * (float)M_PI;
            glVertex3f(r * sinf(theta), -halfH, r * cosf(theta));
        }
        glEnd();

        int step = std::max(1, cols / 8);
        glBegin(GL_LINES);
        for (int c = 0; c < cols; c += step) {
            float theta = (float)c / (float)cols * 2.0f * (float)M_PI;
            float x = r * sinf(theta);
            float z = r * cosf(theta);
            glVertex3f(x,  halfH, z);
            glVertex3f(x, -halfH, z);
        }
        glEnd();

        glLineWidth(1.0f);
    }
}

// Black-body-ish fire ramp for the viewport (dark red → orange → yellow → white),
// clamped to [0,1] for display. Mirrors the renderer's fire_color() shape.
static void
fireRamp(float x, float& r, float& g, float& b)
{
    x = std::min(1.0f, std::max(0.0f, x));
    r = std::min(1.0f, std::pow(x, 0.5f) * 1.5f);
    g = std::min(1.0f, std::pow(x, 1.7f) * 1.3f);
    b = std::min(1.0f, std::pow(x, 4.0f) * 1.2f);
}

// Draw a packed soft-splat buffer [px,py,pz, r,g,b,a, size] (8 floats/sample)
// through the particle point-sprite shader (soft round sprites, over-blended).
// Shared by the Volume3D and ReadVDB viewport previews.
static void
drawSoftSplatBuffer(const std::vector<float>& buf, GLuint shaderProg, bool shaderReady)
{
    const int FPP = 8;
    const int count = (int)(buf.size() / FPP);
    if (count <= 0) return;

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, buf.size() * sizeof(float), buf.data(), GL_STREAM_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // over (not additive)
    glDepthMask(GL_FALSE);
    const GLsizei stride = FPP * sizeof(float);

    if (shaderReady) {
        glUseProgram(shaderProg);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_POINT_SPRITE);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, stride, (void*)0);
        glColorPointer(4, GL_FLOAT, stride, (void*)(3 * sizeof(float)));
        GLint psizeLoc = glGetAttribLocation(shaderProg, "psize");
        if (psizeLoc >= 0) {
            glEnableVertexAttribArray(psizeLoc);
            glVertexAttribPointer(psizeLoc, 1, GL_FLOAT, GL_FALSE, stride, (void*)(7 * sizeof(float)));
        }
        glDrawArrays(GL_POINTS, 0, count);
        if (psizeLoc >= 0) glDisableVertexAttribArray(psizeLoc);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisable(GL_POINT_SPRITE);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glUseProgram(0);
    } else {
        // Fallback (no shader): round-ish smooth points.
        glEnable(GL_POINT_SMOOTH);
        glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
        glPointSize(4.0f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, stride, (void*)0);
        glColorPointer(4, GL_FLOAT, stride, (void*)(3 * sizeof(float)));
        glDrawArrays(GL_POINTS, 0, count);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisable(GL_POINT_SMOOTH);
        glPointSize(1.0f);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vbo);
}

void
Viewport3D::drawVolumeNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) return;

    Volume3D* vol = dynamic_cast<Volume3D*>(effect.get());
    ReadVDB* vdb = dynamic_cast<ReadVDB*>(effect.get());
    if (!vol && !vdb) return;

    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    double time = app->getTimeLine()->currentFrame();

    float bboxMinX = -0.5f, bboxMinY = -0.5f, bboxMinZ = -0.5f;
    float bboxMaxX = 0.5f, bboxMaxY = 0.5f, bboxMaxZ = 0.5f;
    float colR = 0.8f, colG = 0.8f, colB = 0.9f;

    if (vol) {
        Volume3D::VolumeParams vp = vol->getVolumeParams(time);
        colR = vp.colorR; colG = vp.colorG; colB = vp.colorB;

        std::vector<float> volData;
        int res = 0;
        vol->generateVolumeData(time, volData, res);

        if (res > 0 && !volData.empty()) {
            // Soft-splat cloud preview: sample the density field and draw each
            // sample as a soft round point-sprite through the same shader the
            // ParticleSystem uses. Splats are sized to the sample spacing (× the
            // node's world scale) so neighbours overlap into a continuous cloud,
            // over-blended (not additive, which blew out), with a gentle top-down
            // shade so it reads as a 3D form rather than flat haze.
            const int step = std::max(1, res / 32);          // ~32 samples/axis
            const float spacing = (float)step / (float)res;  // local-space gap

            // World scale from the node's matrix columns, so the splat size tracks
            // the actual on-screen size of the volume (incl. parent transforms).
            const float* M = sn.worldMatrix;
            const float sclX = std::sqrt(M[0]*M[0] + M[1]*M[1] + M[2]*M[2]);
            const float sclY = std::sqrt(M[4]*M[4] + M[5]*M[5] + M[6]*M[6]);
            const float sclZ = std::sqrt(M[8]*M[8] + M[9]*M[9] + M[10]*M[10]);
            const float splat = spacing * 2.4f * std::max(0.05f, (sclX + sclY + sclZ) / 3.0f);

            const int FPP = 8;   // px,py,pz, r,g,b,a, size  (matches particle shader)
            std::vector<float> buf;
            buf.reserve(4096 * FPP);
            for (int z = 0; z < res; z += step) {
                for (int y = 0; y < res; y += step) {
                    for (int x = 0; x < res; x += step) {
                        float density = volData[z * res * res + y * res + x];
                        if (density < 0.04f) continue;
                        float lx = ((float)x / (float)res - 0.5f);
                        float ly = ((float)y / (float)res - 0.5f);
                        float lz = ((float)z / (float)res - 0.5f);
                        float shade = 0.55f + 0.45f * (ly + 0.5f);   // dark base -> light top
                        float a = std::min(0.85f, density * 0.55f);
                        buf.push_back(lx); buf.push_back(ly); buf.push_back(lz);
                        buf.push_back(colR * shade); buf.push_back(colG * shade); buf.push_back(colB * shade);
                        buf.push_back(a);
                        buf.push_back(splat);
                    }
                }
            }
            drawSoftSplatBuffer(buf, _imp->particleShaderProgram, _imp->particleShaderReady);
        }
    } else if (vdb) {
        // Read actual VDB bounds from the grid (cached per-path)
        float minX, minY, minZ, maxX, maxY, maxZ;
        bool haveBounds = vdb->getVDBBounds(time, minX, minY, minZ, maxX, maxY, maxZ);
        if (haveBounds) {
            bboxMinX = minX; bboxMinY = minY; bboxMinZ = minZ;
            bboxMaxX = maxX; bboxMaxY = maxY; bboxMaxZ = maxZ;
        } else {
            // Fallback if no file loaded yet
            bboxMinX = -1.0f; bboxMinY = -1.0f; bboxMinZ = -1.0f;
            bboxMaxX = 1.0f;  bboxMaxY = 1.0f;  bboxMaxZ = 1.0f;
        }
        colR = 0.5f; colG = 0.7f; colB = 1.0f;

        // Soft-splat preview of the actual VDB density (same look as Volume3D).
        // Sampling re-reads the file (~80 ms), so the coarse lattice is CACHED per
        // (path, frame) and only re-sampled when those change; the per-frame work
        // is just rebuilding the splat buffer from the cached lattice.
        if (haveBounds) {
            std::string path;
            if (KnobIPtr pk = effect->getKnobByName("filePath")) {
                if (KnobStringBase* s = dynamic_cast<KnobStringBase*>(pk.get())) path = s->getValue();
            }
            const int frame = (int)std::floor(time + 0.5);
            int dispRes = 40;
            if (KnobIPtr rk = effect->getKnobByName("viewportDisplayRes")) {
                if (KnobInt* ri = dynamic_cast<KnobInt*>(rk.get())) dispRes = ri->getValue();
            }
            VdbSplatCache& c = _imp->vdbSplatCache[sn.name];
            if (c.path != path || c.frame != frame || c.res != dispRes || c.density.empty()) {
                c.path = path; c.frame = frame; c.res = dispRes;
                if (!vdb->getViewportDensitySamples(time, dispRes, c.density, c.fire, c.nx, c.ny, c.nz)) {
                    c.density.clear(); c.fire.clear(); c.nx = c.ny = c.nz = 0;
                }
            }
            if (!c.density.empty() && c.nx > 1 && c.ny > 1 && c.nz > 1) {
                const float ex = bboxMaxX - bboxMinX;
                const float ey = bboxMaxY - bboxMinY;
                const float ez = bboxMaxZ - bboxMinZ;
                const float* M = sn.worldMatrix;
                const float sclX = std::sqrt(M[0]*M[0] + M[1]*M[1] + M[2]*M[2]);
                const float sclY = std::sqrt(M[4]*M[4] + M[5]*M[5] + M[6]*M[6]);
                const float sclZ = std::sqrt(M[8]*M[8] + M[9]*M[9] + M[10]*M[10]);
                const float avgScl = std::max(0.05f, (sclX + sclY + sclZ) / 3.0f);
                // splat ~ average world-space gap between lattice samples
                const float spacing = ((ex / c.nx) + (ey / c.ny) + (ez / c.nz)) / 3.0f;
                const float splat = spacing * 2.4f * avgScl;

                const bool haveFire = (c.fire.size() == c.density.size());
                // Smoke = neutral grey; fire voxels glow via the fire ramp (emissive,
                // so they show even where smoke density is low).
                const float smR = 0.78f, smG = 0.78f, smB = 0.82f;
                const int FPP = 8;
                std::vector<float> buf;
                buf.reserve(4096 * FPP);
                for (int k = 0; k < c.nz; ++k) {
                    for (int j = 0; j < c.ny; ++j) {
                        for (int i = 0; i < c.nx; ++i) {
                            const size_t idx = ((size_t)k * c.ny + j) * c.nx + i;
                            float density = c.density[idx];
                            float fireVal = haveFire ? c.fire[idx] : 0.0f;
                            if (density < 0.04f && fireVal < 0.05f) continue;
                            float u = (float)i / (float)(c.nx - 1);
                            float v = (float)j / (float)(c.ny - 1);
                            float w = (float)k / (float)(c.nz - 1);
                            float px = bboxMinX + u * ex;
                            float py = bboxMinY + v * ey;
                            float pz = bboxMinZ + w * ez;
                            float shade = 0.55f + 0.45f * v;          // dark base -> light top
                            float r = smR * shade, g = smG * shade, b = smB * shade;
                            float a = std::min(0.85f, density * 0.7f);
                            if (fireVal > 0.02f) {
                                float fr, fg, fb; fireRamp(fireVal, fr, fg, fb);
                                float fw = std::min(1.0f, fireVal * 1.6f);   // fire blend weight
                                r = r * (1.0f - fw) + fr * fw;
                                g = g * (1.0f - fw) + fg * fw;
                                b = b * (1.0f - fw) + fb * fw;
                                a = std::max(a, std::min(0.9f, fireVal * 0.95f));
                            }
                            buf.push_back(px); buf.push_back(py); buf.push_back(pz);
                            buf.push_back(r); buf.push_back(g); buf.push_back(b);
                            buf.push_back(a);
                            buf.push_back(splat);
                        }
                    }
                }
                drawSoftSplatBuffer(buf, _imp->particleShaderProgram, _imp->particleShaderReady);
            }
        }
    }

    bool selected = (sn.name == _imp->selectedNodeName);
    glColor3f(selected ? 1.0f : 0.4f, selected ? 1.0f : 0.6f, selected ? 0.0f : 0.8f);
    glLineWidth(selected ? 2.0f : 1.0f);

    float x0 = bboxMinX, y0 = bboxMinY, z0 = bboxMinZ;
    float x1 = bboxMaxX, y1 = bboxMaxY, z1 = bboxMaxZ;
    glBegin(GL_LINE_LOOP);
    glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0);
    glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);
    glEnd();
    glBegin(GL_LINE_LOOP);
    glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1);
    glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
    glEnd();
    glBegin(GL_LINES);
    glVertex3f(x0, y0, z0); glVertex3f(x0, y0, z1);
    glVertex3f(x1, y0, z0); glVertex3f(x1, y0, z1);
    glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1);
    glVertex3f(x0, y1, z0); glVertex3f(x0, y1, z1);
    glEnd();

    glLineWidth(1.0f);
    glPointSize(1.0f);
}

void
Viewport3D::drawLightNode(const SceneNode& sn) const
{
    bool selected = (sn.name == _imp->selectedNodeName);
    glColor3f(1.0f, 0.9f, 0.3f);
    glLineWidth(selected ? 3.0f : 2.0f);

    // Determine light type from the node
    Light3D::LightType ltype = Light3D::eLightPoint;
    NodePtr node = sn.sourceNode.lock();
    if (node) {
        Light3D* light3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
        if (light3d) ltype = light3d->getLightType();
    }

    float s = 0.3f;
    int segments = 24;

    if (ltype == Light3D::eLightPoint) {
        // Point light: small sphere (3 circles)
        for (int axis = 0; axis < 3; ++axis) {
            glBegin(GL_LINE_LOOP);
            for (int i = 0; i < segments; ++i) {
                float a = (float)i / segments * 2.0f * (float)M_PI;
                float c = s * cosf(a), si = s * sinf(a);
                if (axis == 0) glVertex3f(0, c, si);
                else if (axis == 1) glVertex3f(c, 0, si);
                else glVertex3f(c, si, 0);
            }
            glEnd();
        }
        // Rays sticking out
        float r = s * 1.8f;
        glBegin(GL_LINES);
        glVertex3f(r, 0, 0); glVertex3f(s, 0, 0);
        glVertex3f(-r, 0, 0); glVertex3f(-s, 0, 0);
        glVertex3f(0, r, 0); glVertex3f(0, s, 0);
        glVertex3f(0, -r, 0); glVertex3f(0, -s, 0);
        glVertex3f(0, 0, r); glVertex3f(0, 0, s);
        glVertex3f(0, 0, -r); glVertex3f(0, 0, -s);
        glEnd();

    } else if (ltype == Light3D::eLightSpot) {
        // Spot light: cone pointing along -Z (where Cycles actually emits)
        float coneLen = 1.5f;
        float spotAngleDeg = 45.0f;
        if (node) {
            Light3D* l3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
            if (l3d) spotAngleDeg = (float)l3d->getSpotAngle(0);
        }
        float coneRadius = coneLen * tanf(spotAngleDeg * 0.5f * (float)M_PI / 180.0f);
        // Circle at base of cone
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segments; ++i) {
            float a = (float)i / segments * 2.0f * (float)M_PI;
            glVertex3f(coneRadius * cosf(a), coneRadius * sinf(a), -coneLen);
        }
        glEnd();
        // Lines from tip to base
        glBegin(GL_LINES);
        for (int i = 0; i < 8; ++i) {
            float a = (float)i / 8.0f * 2.0f * (float)M_PI;
            glVertex3f(0, 0, 0);
            glVertex3f(coneRadius * cosf(a), coneRadius * sinf(a), -coneLen);
        }
        // Direction arrow along -Z
        glVertex3f(0, 0, 0); glVertex3f(0, 0, -coneLen * 1.2f);
        glEnd();
        // Small sphere at origin
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segments; ++i) {
            float a = (float)i / segments * 2.0f * (float)M_PI;
            glVertex3f(s * 0.4f * cosf(a), s * 0.4f * sinf(a), 0);
        }
        glEnd();

    } else if (ltype == Light3D::eLightArea) {
        // Area light: rectangle in XY plane, showing actual size from knobs
        float hw = 1.0f, hh = 1.0f; // half-width, half-height
        if (node) {
            Light3D* l3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
            if (l3d) {
                hw = (float)l3d->getAreaSizeU(0) * 0.5f;
                hh = (float)l3d->getAreaSizeV(0) * 0.5f;
            }
        }
        // Rectangle outline
        glBegin(GL_LINE_LOOP);
        glVertex3f(-hw, -hh, 0);
        glVertex3f( hw, -hh, 0);
        glVertex3f( hw,  hh, 0);
        glVertex3f(-hw,  hh, 0);
        glEnd();
        // Cross inside
        glBegin(GL_LINES);
        glVertex3f(-hw, 0, 0); glVertex3f(hw, 0, 0);
        glVertex3f(0, -hh, 0); glVertex3f(0, hh, 0);
        glEnd();
        // Direction arrow along -Z (where Cycles actually emits)
        glBegin(GL_LINES);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, -1.2f);
        // Arrow head
        glVertex3f(0, 0, -1.2f); glVertex3f(0.15f, 0, -0.9f);
        glVertex3f(0, 0, -1.2f); glVertex3f(-0.15f, 0, -0.9f);
        glEnd();

    } else if (ltype == Light3D::eLightDistant) {
        // Distant/Sun light: parallel rays pointing along -Z (emission direction)
        glBegin(GL_LINES);
        float spacing = 0.4f;
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
                float px = x * spacing, py = y * spacing;
                glVertex3f(px, py, 0.5f);
                glVertex3f(px, py, -0.5f);
                // Arrow heads
                glVertex3f(px, py, -0.5f);
                glVertex3f(px + 0.08f, py, -0.3f);
                glVertex3f(px, py, -0.5f);
                glVertex3f(px - 0.08f, py, -0.3f);
            }
        }
        glEnd();

    } else if (ltype == Light3D::eLightDome) {
        // Dome/Environment light: hemisphere
        for (int ring = 0; ring < 3; ++ring) {
            float phi = (float)(ring + 1) / 4.0f * (float)M_PI * 0.5f;
            float rr = s * 3.0f * cosf(phi);
            float yy = s * 3.0f * sinf(phi);
            glBegin(GL_LINE_LOOP);
            for (int i = 0; i < segments; ++i) {
                float a = (float)i / segments * 2.0f * (float)M_PI;
                glVertex3f(rr * cosf(a), yy, rr * sinf(a));
            }
            glEnd();
        }
        // Base circle
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segments; ++i) {
            float a = (float)i / segments * 2.0f * (float)M_PI;
            glVertex3f(s * 3.0f * cosf(a), 0, s * 3.0f * sinf(a));
        }
        glEnd();
    }

    glLineWidth(1.0f);
}

void
Viewport3D::drawTransformNode(const SceneNode& sn) const
{
    // The dispatch loop has already applied sn.worldMatrix (= parent.world * local),
    // so we are at this transform's world frame — draw the locator at the origin.
    // Do NOT re-apply sn.localMatrix: worldMatrix already contains it, and re-applying
    // it double-transformed the gizmo.
    //
    // Keep the locator a fixed VISUAL size by dividing each axis by the node's world
    // scale, so a baked unit scale (e.g. an FBX->Alembic cm->m conversion, common in
    // these archives — and often non-uniform across the xform chain) doesn't balloon
    // it into a viewport-spanning diamond.
    bool selected = (sn.name == _imp->selectedNodeName);
    float lineW = selected ? 3.0f : 2.0f;

    // Viewport-only controls, read from the source node (e.g. ReadAlembicArchive's
    // "Show Locators" / "Locator Size"). Transform nodes from sources without these
    // knobs fall back to on / 0.8 (the previous fixed size).
    bool showLoc = true;
    float locSize = 0.8f;
    if (NodePtr src = sn.sourceNode.lock()) {
        if (EffectInstancePtr eff = src->getEffectInstance()) {
            if (KnobIPtr k = eff->getKnobByName("showLocators")) {
                if (KnobBool* b = dynamic_cast<KnobBool*>(k.get())) showLoc = b->getValue();
            }
            if (KnobIPtr k = eff->getKnobByName("locatorSize")) {
                if (KnobDouble* db = dynamic_cast<KnobDouble*>(k.get())) locSize = (float)db->getValue();
            }
        }
    }
    if (!showLoc) return;

    const float* M = sn.worldMatrix;
    float sx = std::sqrt(M[0]*M[0] + M[1]*M[1] + M[2]*M[2]);
    float sy = std::sqrt(M[4]*M[4] + M[5]*M[5] + M[6]*M[6]);
    float sz = std::sqrt(M[8]*M[8] + M[9]*M[9] + M[10]*M[10]);
    if (sx < 1e-6f) sx = 1.0f;
    if (sy < 1e-6f) sy = 1.0f;
    if (sz < 1e-6f) sz = 1.0f;
    // Axis lengths + diamond half-extents at a constant ~world size (locSize, with the
    // diamond at the original 0.12/0.8 = 0.15 ratio), per-axis so non-uniform scale
    // stays even.
    const float diamond = locSize * 0.15f;
    const float ax = locSize / sx,  ay = locSize / sy,  az = locSize / sz;
    const float dx = diamond / sx,  dy = diamond / sy,  dz = diamond / sz;

    glLineWidth(lineW);
    glBegin(GL_LINES);

    // X axis — red
    glColor3f(1.0f, 0.2f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(ax, 0, 0);

    // Y axis — green
    glColor3f(0.2f, 1.0f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(0, ay, 0);

    // Z axis — blue
    glColor3f(0.3f, 0.3f, 1.0f);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, az);

    glEnd();

    // Draw a small diamond/cross at the origin to mark the null
    glColor3f(1.0f, 0.8f, 0.0f); // yellow
    glBegin(GL_LINES);
    glVertex3f(-dx, 0, 0); glVertex3f(dx, 0, 0);
    glVertex3f(0, -dy, 0); glVertex3f(0, dy, 0);
    glVertex3f(0, 0, -dz); glVertex3f(0, 0, dz);
    // Diamond shape in XY plane
    glVertex3f(0, dy, 0); glVertex3f(dx, 0, 0);
    glVertex3f(dx, 0, 0); glVertex3f(0, -dy, 0);
    glVertex3f(0, -dy, 0); glVertex3f(-dx, 0, 0);
    glVertex3f(-dx, 0, 0); glVertex3f(0, dy, 0);
    glEnd();

    glLineWidth(1.0f);
}

void
Viewport3D::drawParticlesNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) return;

    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    double time = app->getTimeLine()->currentFrame();

    // If this is a ParticleInstance node, draw instanced geo wireframes
    ParticleInstance* instancer = dynamic_cast<ParticleInstance*>(effect.get());
    if (instancer) {
        std::vector<ParticleInstance::GeoInstance> instances;
        instancer->getInstances(time, instances);
        if (instances.empty()) return;

        // Limit viewport preview to avoid slowdown
        int maxPreview = std::min((int)instances.size(), 2000);

        glEnable(GL_DEPTH_TEST);
        glLineWidth(1.0f);

        for (int i = 0; i < maxPreview; ++i) {
            const ParticleInstance::GeoInstance& inst = instances[i];

            glPushMatrix();
            glTranslatef(inst.px, inst.py, inst.pz);
            if (inst.ry != 0) glRotatef(inst.ry, 0, 1, 0);
            if (inst.rx != 0) glRotatef(inst.rx, 1, 0, 0);
            if (inst.rz != 0) glRotatef(inst.rz, 0, 0, 1);
            glScalef(inst.sx, inst.sy, inst.sz);

            glColor4f(inst.r * 0.8f, inst.g * 0.8f, inst.b * 0.8f, 0.6f);

            // Draw a simple wireframe box as proxy for any geo type
            float s = 0.5f;
            glBegin(GL_LINES);
            // Bottom
            glVertex3f(-s,-s,-s); glVertex3f( s,-s,-s);
            glVertex3f( s,-s,-s); glVertex3f( s,-s, s);
            glVertex3f( s,-s, s); glVertex3f(-s,-s, s);
            glVertex3f(-s,-s, s); glVertex3f(-s,-s,-s);
            // Top
            glVertex3f(-s, s,-s); glVertex3f( s, s,-s);
            glVertex3f( s, s,-s); glVertex3f( s, s, s);
            glVertex3f( s, s, s); glVertex3f(-s, s, s);
            glVertex3f(-s, s, s); glVertex3f(-s, s,-s);
            // Verticals
            glVertex3f(-s,-s,-s); glVertex3f(-s, s,-s);
            glVertex3f( s,-s,-s); glVertex3f( s, s,-s);
            glVertex3f( s,-s, s); glVertex3f( s, s, s);
            glVertex3f(-s,-s, s); glVertex3f(-s, s, s);
            glEnd();

            glPopMatrix();
        }
        return;
    }

    ParticleDataPtr data;
    ParticleProvider* provider = dynamic_cast<ParticleProvider*>(effect.get());
    if (provider) {
        data = provider->getParticleData(time);
    }

    if (!data || data->numParticles() == 0) return;

    int count = data->numParticles();

    // Build interleaved buffer: [px, py, pz, r, g, b, a, size] per particle
    const int FLOATS_PER_PARTICLE = 8;
    std::vector<float> buf(count * FLOATS_PER_PARTICLE);
    for (int i = 0; i < count; ++i) {
        const Particle& p = data->particles[i];
        float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
        float alpha = p.a * (1.0f - ageFrac);
        buf[i * FLOATS_PER_PARTICLE + 0] = p.px;
        buf[i * FLOATS_PER_PARTICLE + 1] = p.py;
        buf[i * FLOATS_PER_PARTICLE + 2] = p.pz;
        buf[i * FLOATS_PER_PARTICLE + 3] = p.r;
        buf[i * FLOATS_PER_PARTICLE + 4] = p.g;
        buf[i * FLOATS_PER_PARTICLE + 5] = p.b;
        buf[i * FLOATS_PER_PARTICLE + 6] = alpha;
        buf[i * FLOATS_PER_PARTICLE + 7] = p.size;
    }

    // Upload to VBO
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, buf.size() * sizeof(float), buf.data(), GL_STREAM_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); // Don't write depth for transparent particles

    const GLsizei stride = FLOATS_PER_PARTICLE * sizeof(float);

    if (_imp->particleShaderReady) {
        // Shader path: per-particle size via gl_PointSize + soft circle
        glUseProgram(_imp->particleShaderProgram);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_POINT_SPRITE);

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, stride, (void*)0);
        glColorPointer(4, GL_FLOAT, stride, (void*)(3 * sizeof(float)));

        // Bind psize attribute
        GLint psizeLoc = glGetAttribLocation(_imp->particleShaderProgram, "psize");
        if (psizeLoc >= 0) {
            glEnableVertexAttribArray(psizeLoc);
            glVertexAttribPointer(psizeLoc, 1, GL_FLOAT, GL_FALSE, stride, (void*)(7 * sizeof(float)));
        }

        glDrawArrays(GL_POINTS, 0, count);

        if (psizeLoc >= 0) glDisableVertexAttribArray(psizeLoc);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);

        glDisable(GL_POINT_SPRITE);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glUseProgram(0);
    } else {
        // Fallback: fixed size, no shader
        glPointSize(3.0f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, stride, (void*)0);
        glColorPointer(4, GL_FLOAT, stride, (void*)(3 * sizeof(float)));
        glDrawArrays(GL_POINTS, 0, count);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glPointSize(1.0f);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vbo);

    // --- Draw collision shape wireframe if this is a ParticleSolver node ---
    {
        ParticleSolver* collider = dynamic_cast<ParticleSolver*>(effect.get());
        if (collider) {
            glColor3f(0.0f, 1.0f, 0.5f); // green wireframe
            glLineWidth(1.5f);

            KnobIPtr shapeKnob = effect->getKnobByName("shape");
            int shapeMode = shapeKnob ? dynamic_cast<KnobChoice*>(shapeKnob.get())->getValue() : 0;

            if (shapeMode == 0) {
                // Plane: draw a grid at plane height
                KnobIPtr hKnob = effect->getKnobByName("planeHeight");
                float h = hKnob ? (float)dynamic_cast<KnobDouble*>(hKnob.get())->getValueAtTime(time) : 0.0f;
                float sz = 5.0f;
                glBegin(GL_LINE_LOOP);
                glVertex3f(-sz, h, -sz);
                glVertex3f( sz, h, -sz);
                glVertex3f( sz, h,  sz);
                glVertex3f(-sz, h,  sz);
                glEnd();
                // Cross lines
                glBegin(GL_LINES);
                glVertex3f(-sz, h, 0); glVertex3f(sz, h, 0);
                glVertex3f(0, h, -sz); glVertex3f(0, h, sz);
                glEnd();
            } else if (shapeMode == 1) {
                // Box wireframe
                KnobIPtr k;
                k = effect->getKnobByName("boxMinX"); float mnx = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : -2;
                k = effect->getKnobByName("boxMinY"); float mny = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 0;
                k = effect->getKnobByName("boxMinZ"); float mnz = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : -2;
                k = effect->getKnobByName("boxMaxX"); float mxx = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 2;
                k = effect->getKnobByName("boxMaxY"); float mxy = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 4;
                k = effect->getKnobByName("boxMaxZ"); float mxz = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 2;

                glBegin(GL_LINES);
                // Bottom face
                glVertex3f(mnx,mny,mnz); glVertex3f(mxx,mny,mnz);
                glVertex3f(mxx,mny,mnz); glVertex3f(mxx,mny,mxz);
                glVertex3f(mxx,mny,mxz); glVertex3f(mnx,mny,mxz);
                glVertex3f(mnx,mny,mxz); glVertex3f(mnx,mny,mnz);
                // Top face
                glVertex3f(mnx,mxy,mnz); glVertex3f(mxx,mxy,mnz);
                glVertex3f(mxx,mxy,mnz); glVertex3f(mxx,mxy,mxz);
                glVertex3f(mxx,mxy,mxz); glVertex3f(mnx,mxy,mxz);
                glVertex3f(mnx,mxy,mxz); glVertex3f(mnx,mxy,mnz);
                // Verticals
                glVertex3f(mnx,mny,mnz); glVertex3f(mnx,mxy,mnz);
                glVertex3f(mxx,mny,mnz); glVertex3f(mxx,mxy,mnz);
                glVertex3f(mxx,mny,mxz); glVertex3f(mxx,mxy,mxz);
                glVertex3f(mnx,mny,mxz); glVertex3f(mnx,mxy,mxz);
                glEnd();
            } else if (shapeMode == 2) {
                // Sphere wireframe (3 circles)
                KnobIPtr k;
                k = effect->getKnobByName("sphereCenterX"); float cx = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 0;
                k = effect->getKnobByName("sphereCenterY"); float cy = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 2;
                k = effect->getKnobByName("sphereCenterZ"); float cz = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 0;
                k = effect->getKnobByName("sphereRadius"); float rad = k ? (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time) : 3;

                const int segs = 32;
                // XY circle
                glBegin(GL_LINE_LOOP);
                for (int s = 0; s < segs; ++s) {
                    float a = (float)s / segs * 6.2831853f;
                    glVertex3f(cx + rad * cosf(a), cy + rad * sinf(a), cz);
                }
                glEnd();
                // XZ circle
                glBegin(GL_LINE_LOOP);
                for (int s = 0; s < segs; ++s) {
                    float a = (float)s / segs * 6.2831853f;
                    glVertex3f(cx + rad * cosf(a), cy, cz + rad * sinf(a));
                }
                glEnd();
                // YZ circle
                glBegin(GL_LINE_LOOP);
                for (int s = 0; s < segs; ++s) {
                    float a = (float)s / segs * 6.2831853f;
                    glVertex3f(cx, cy + rad * cosf(a), cz + rad * sinf(a));
                }
                glEnd();
            }
            glLineWidth(1.0f);
        }
    }
}

void
Viewport3D::drawPointCloudNode(const SceneNode& sn) const
{
    Q_UNUSED(sn);
    // Actual drawing is done by drawPointCloud() which uses the cached PointCloudDataPtr.
}

void
Viewport3D::drawGroupNode(const SceneNode& sn) const
{
    Q_UNUSED(sn);

    glColor3f(0.9f, 0.5f, 0.1f);
    glLineWidth(1.5f);
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, 0xAAAA);

    float s = 2.0f;
    glBegin(GL_LINE_LOOP);
    glVertex3f(-s, -s, -s); glVertex3f(s, -s, -s);
    glVertex3f(s, s, -s); glVertex3f(-s, s, -s);
    glEnd();
    glBegin(GL_LINE_LOOP);
    glVertex3f(-s, -s, s); glVertex3f(s, -s, s);
    glVertex3f(s, s, s); glVertex3f(-s, s, s);
    glEnd();
    glBegin(GL_LINES);
    glVertex3f(-s, -s, -s); glVertex3f(-s, -s, s);
    glVertex3f(s, -s, -s); glVertex3f(s, -s, s);
    glVertex3f(s, s, -s); glVertex3f(s, s, s);
    glVertex3f(-s, s, -s); glVertex3f(-s, s, s);
    glEnd();

    glDisable(GL_LINE_STIPPLE);
    glLineWidth(1.0f);
}


// ============================================================================
// refreshPointCloud
// ============================================================================

void
Viewport3D::refreshPointCloud()
{
    Gui* gui = getGui();
    if (!gui) return;

    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;

    ProjectPtr project = app->getProject();
    if (!project) return;

    NodesList nodes = project->getNodes();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        EffectInstancePtr effect = (*it)->getEffectInstance();
        if (!effect) continue;

        PointCloudProvider* dtp = dynamic_cast<PointCloudProvider*>(effect.get());
        if (dtp) {
            PointCloudDataPtr cloud = dtp->getPointCloud();
            if (cloud && cloud->numPoints() > 0) {
                float ptSize = 2.0f;
                KnobIPtr psKnob = effect->getKnobByName("pointSize");
                if (psKnob) {
                    KnobDouble* psDbl = dynamic_cast<KnobDouble*>(psKnob.get());
                    if (psDbl) {
                        ptSize = (float)psDbl->getValue();
                    }
                }
                setPointCloud(cloud, ptSize);
                return;
            }
        }
    }

    // No DeepToPoints node found
    {
        QMutexLocker lock(&_imp->cloudMutex);
        if (_imp->pointCloud) {
            _imp->pointCloud.reset();
            update();
        }
    }
}

NATRON_NAMESPACE_EXIT
