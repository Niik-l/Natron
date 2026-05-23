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

#include "DevViewport3D.h"

#include <cmath>
#include <cstring>
#include <algorithm>

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
#include "Engine/Dev/Scene3D/Group3D.h"
#include "Engine/Dev/Scene3D/ReadGeo.h"
#include "Engine/Dev/Scene3D/Light3D.h"
#include "Engine/TimeLine.h"
#include "Engine/Dev/Deep/DeepToPoints.h"
#include "Engine/Dev/Deep/Blast.h"
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
    return ViewportLitFromEyeNormal(nEye);
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

// ============================================================================
// Section 3: Private struct — demo-style camera + ImGuizmo state
// ============================================================================

struct DevViewport3DPrivate
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

    // Toggle from the Grid button in Viewport3DTab. Default visible.
    bool showGrid;

    // Viewport shading style — driven by the Shading dropdown in Viewport3DTab.
    // Default Shaded+Wire so meshes show solid grey + edges (Maya-like) while
    // shapes carrying textures still display them.
    DevViewport3D::ShadingMode shadingMode;

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

    // Refresh timer
    QTimer* refreshTimer;

    // Particle shader (for per-particle size via gl_PointSize)
    GLuint particleShaderProgram;
    bool particleShaderReady;

    DevViewport3DPrivate()
        : camYAngle(2.7f)
        , camXAngle(0.4f)
        , camDistance(8.0f)
        , fov(27.0f)
        , imguiInitialized(false)
        , imguizmoOp(ImGuizmo::TRANSLATE)
        , imguizmoMode(ImGuizmo::WORLD)
        , selectedCardIndex(-1)
        , selectedPointIndex(-1)
        , boxSelecting(false)
        , boxStartX(0), boxStartY(0)
        , boxEndX(0), boxEndY(0)
        , showGrid(true)
        , shadingMode(DevViewport3D::eShadedWire)
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
        , refreshTimer(nullptr)
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

DevViewport3D::DevViewport3D(Gui* gui,
                             const QOpenGLWidget* shareWidget)
    : QOpenGLWidget()
    , _gui(gui)
    , _imp(new DevViewport3DPrivate())
{
    Q_UNUSED(shareWidget);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    // Don't let Qt auto-fire contextMenuEvent on right-click. We trigger the
    // Blast menu manually from mouseReleaseEvent only when the click had no
    // drag (so right-drag for navigation doesn't accidentally open the menu).
    setContextMenuPolicy(Qt::PreventContextMenu);
}

DevViewport3D::~DevViewport3D()
{
}

QSize
DevViewport3D::sizeHint() const
{
    return QSize(640, 480);
}

void
DevViewport3D::setPointCloud(const PointCloudDataPtr& cloud, float pointSize)
{
    QMutexLocker lock(&_imp->cloudMutex);
    _imp->pointCloud = cloud;
    _imp->pointSize = pointSize;
    update();
}

void
DevViewport3D::getCameraView(float m16[16]) const
{
    std::memcpy(m16, _imp->cameraView, sizeof(float) * 16);
}

void
DevViewport3D::getCameraProjection(float m16[16]) const
{
    std::memcpy(m16, _imp->cameraProjection, sizeof(float) * 16);
}

void
DevViewport3D::resetCamera()
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
DevViewport3D::initializeGL()
{
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glEnable(GL_POINT_SMOOTH);

    // Initialize ImGui + ImGuizmo
    if (!_imp->imguiInitialized) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL; // don't save imgui.ini
        io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui_ImplOpenGL2_CreateFontsTexture();
        _imp->imguiInitialized = true;
    }

    // Compile particle point-sprite shader
    {
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
            printf("[DevViewport3D] Particle VS error: %s\n", log);
        }

        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fragSrc, NULL);
        glCompileShader(fs);
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetShaderInfoLog(fs, 512, NULL, log);
            printf("[DevViewport3D] Particle FS error: %s\n", log);
        }

        _imp->particleShaderProgram = glCreateProgram();
        glAttachShader(_imp->particleShaderProgram, vs);
        glAttachShader(_imp->particleShaderProgram, fs);
        glLinkProgram(_imp->particleShaderProgram);
        glGetProgramiv(_imp->particleShaderProgram, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512]; glGetProgramInfoLog(_imp->particleShaderProgram, 512, NULL, log);
            printf("[DevViewport3D] Particle shader link error: %s\n", log);
        } else {
            _imp->particleShaderReady = true;
        }

        glDeleteShader(vs);
        glDeleteShader(fs);
    }

    // Refresh at 30fps
    _imp->refreshTimer = new QTimer(this);
    connect(_imp->refreshTimer, SIGNAL(timeout()), this, SLOT(update()));
    _imp->refreshTimer->start(33);
}


void
DevViewport3D::resizeGL(int w, int h)
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
DevViewport3D::paintGL()
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
                    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
                        if (!(*it)->isActivated()) continue;
                        if ((*it)->isNodeDisabled()) continue;
                        EffectInstancePtr eff = (*it)->getEffectInstance();
                        if (!eff) continue;
                        // Scan for Blast and DeepToPoints — prefer Blast (filtered) over raw
                        Blast* blast = dynamic_cast<Blast*>(eff.get());
                        DeepToPoints* dtp = dynamic_cast<DeepToPoints*>(eff.get());
                        if (blast || dtp) {
                            // Remember candidates but don't break — keep scanning for Blast
                            if (blast) {
                                PointCloudDataPtr cloud = blast->getPointCloud();
                                if (cloud && cloud->numPoints() > 0) {
                                    QMutexLocker lock(&_imp->cloudMutex);
                                    _imp->pointCloud = cloud;
                                    _imp->pointSize = 2.0f;
                                    _imp->activeBlastNode = *it; // remember for selection push
                                }
                            } else if (dtp && !_imp->pointCloud) {
                                // Only use DeepToPoints if no Blast cloud found yet
                                PointCloudDataPtr cloud = dtp->getPointCloud();
                                if (cloud && cloud->numPoints() > 0) {
                                    float ptSize = 2.0f;
                                    KnobIPtr psKnob = eff->getKnobByName("pointSize");
                                    if (psKnob) {
                                        KnobDouble* psDbl = dynamic_cast<KnobDouble*>(psKnob.get());
                                        if (psDbl) ptSize = (float)psDbl->getValue();
                                    }
                                    QMutexLocker lock(&_imp->cloudMutex);
                                    _imp->pointCloud = cloud;
                                    _imp->pointSize = ptSize;
                                }
                            }
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
        ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 10), 0xFFFFFFFF, "DevViewport3D");
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

                // Read T/R/S from knobs, build matrix using ImGuizmo's Recompose
                float translation[3], rotation[3], scale[3];
                readTRSFromNode(effect, translation, rotation, scale);
                float objMat[16];
                ImGuizmo::RecomposeMatrixFromComponents(translation, rotation, scale, objMat);

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
                    // Decompose modified matrix back to T/R/S
                    float newT[3], newR[3], newS[3];
                    ImGuizmo::DecomposeMatrixToComponents(objMat, newT, newR, newS);

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
DevViewport3D::mousePressEvent(QMouseEvent* e)
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
            if (isOrbit)      _imp->lookThroughEditMode = DevViewport3DPrivate::LT_ORBIT;
            else if (isPan)   _imp->lookThroughEditMode = DevViewport3DPrivate::LT_PAN;
            else              _imp->lookThroughEditMode = DevViewport3DPrivate::LT_DOLLY;
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
DevViewport3D::mouseMoveEvent(QMouseEvent* e)
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
    if (_imp->lookThroughEditMode != DevViewport3DPrivate::LT_NONE) {
        Camera3DNode* editCam = getEditableCamera3D(_imp->lookThroughCam);
        if (editCam) {
            double time = 0.0;
            if (getGui() && getGui()->getApp()) {
                time = getGui()->getApp()->getTimeLine()->currentFrame();
            }
            switch (_imp->lookThroughEditMode) {
                case DevViewport3DPrivate::LT_ORBIT:
                    applyOrbitToCamera3D(editCam, time, _imp->lookThroughPivot, dx, dy); break;
                case DevViewport3DPrivate::LT_PAN:
                    applyPanToCamera3D(editCam, time, _imp->lookThroughPivot, dx, dy); break;
                case DevViewport3DPrivate::LT_DOLLY:
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
DevViewport3D::mouseReleaseEvent(QMouseEvent* e)
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
    _imp->lookThroughEditMode = DevViewport3DPrivate::LT_NONE;
}

void
DevViewport3D::wheelEvent(QWheelEvent* e)
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
DevViewport3D::keyPressEvent(QKeyEvent* e)
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
        // Frame selected — try point cloud first, then selected node, then reset
        bool framed = false;

        // If a point cloud is visible, frame it (F always frames the cloud)
        {
            QMutexLocker lock(&_imp->cloudMutex);
            if (_imp->pointCloud && _imp->pointCloud->numPoints() > 0) {
                float cx, cy, cz;
                _imp->pointCloud->getCenter(cx, cy, cz);
                float radius = _imp->pointCloud->getRadius();
                if (radius < 0.1f) radius = 2.0f;
                lock.unlock();
                _imp->camTarget[0] = cx;
                _imp->camTarget[1] = cy;
                _imp->camTarget[2] = cz;
                _imp->camDistance = radius * 2.5f;
                framed = true;
            }
        }

        // If no point cloud, frame selected node
        if (!framed && !_imp->selectedNodeName.empty()) {
            const std::vector<SceneNode>& nodes = _imp->sceneGraph.nodes();
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].name == _imp->selectedNodeName) {
                    NodePtr node = nodes[i].sourceNode.lock();
                    if (node) {
                        EffectInstancePtr effect = node->getEffectInstance();
                        float t[3], r[3], s[3];
                        readTRSFromNode(effect, t, r, s);
                        _imp->camTarget[0] = t[0];
                        _imp->camTarget[1] = t[1];
                        _imp->camTarget[2] = t[2];
                        _imp->camDistance = 5.0f;
                        framed = true;
                    }
                    break;
                }
            }
        }

        if (!framed) {
            resetCamera();
            return;
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
DevViewport3D::toggleTransformSpace()
{
    if (_imp->imguizmoMode == ImGuizmo::WORLD) {
        _imp->imguizmoMode = ImGuizmo::LOCAL;
    } else {
        _imp->imguizmoMode = ImGuizmo::WORLD;
    }
    update();
}

bool
DevViewport3D::isLocalSpace() const
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
DevViewport3D::pickPointAtPosition(int screenX, int screenY)
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
        update();
        return true;
    }

    // No hit — clear the multi-selection.
    _imp->selectedPointIndex = -1;
    _imp->selectedPointIndices.clear();
    return false;
}

void
DevViewport3D::setShowGrid(bool show)
{
    if (_imp->showGrid == show) return;
    _imp->showGrid = show;
    update();
}

void
DevViewport3D::setShadingMode(DevViewport3D::ShadingMode mode)
{
    if (_imp->shadingMode == mode) return;
    _imp->shadingMode = mode;
    update();
}

DevViewport3D::ShadingMode
DevViewport3D::getShadingMode() const
{
    return _imp->shadingMode;
}

void
DevViewport3D::setLookThroughCamera(const NodePtr& cameraNode)
{
    _imp->lookThroughCam = cameraNode;
    update();
}

NodePtr
DevViewport3D::getLookThroughCamera() const
{
    return _imp->lookThroughCam.lock();
}

Blast*
DevViewport3D::getActiveBlast() const
{
    NodePtr active = _imp->activeBlastNode.lock();
    if (!active) return nullptr;
    EffectInstancePtr eff = active->getEffectInstance();
    if (!eff) return nullptr;
    return dynamic_cast<Blast*>(eff.get());
}

void
DevViewport3D::showBlastContextMenu(const QPoint& globalPos)
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
DevViewport3D::boxSelectPoints()
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
}

void
DevViewport3D::selectObjectAtPosition(int screenX, int screenY)
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
DevViewport3D::drawGrid() const
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
DevViewport3D::drawAxes() const
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
DevViewport3D::drawPointCloud() const
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
DevViewport3D::drawMeshNode(const SceneNode& sn) const
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
        if (!lit) glColor3f(baseGrey, baseGrey, baseGrey);
        glBegin(GL_TRIANGLES);
        auto emitTri = [&](int v0, int v1, int v2) {
            const float* p0 = &mesh->vertices[v0*3];
            const float* p1 = &mesh->vertices[v1*3];
            const float* p2 = &mesh->vertices[v2*3];
            if (lit) {
                float f = ViewportFaceLitFactor(p0, p1, p2, mvForLit);
                glColor3f(baseGrey * f, baseGrey * f, baseGrey * f);
            }
            glVertex3f(p0[0], p0[1], p0[2]);
            glVertex3f(p1[0], p1[1], p1[2]);
            glVertex3f(p2[0], p2[1], p2[2]);
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
                        emitTri(v0, v1, v2);
                    }
                }
                off += (size_t)c;
            }
        } else {
            // Already triangulated
            for (size_t i = 0; i + 2 < mesh->faceIndices.size(); i += 3) {
                const int v0 = mesh->faceIndices[i];
                const int v1 = mesh->faceIndices[i + 1];
                const int v2 = mesh->faceIndices[i + 2];
                if (v0 >= 0 && v0 < nv && v1 >= 0 && v1 < nv && v2 >= 0 && v2 < nv) {
                    emitTri(v0, v1, v2);
                }
            }
        }
        glEnd();
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
DevViewport3D::drawCardNode(const SceneNode& sn) const
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
    const bool hasTex = (tex.width > 0 && tex.height > 0 && !tex.pixels.empty());

    if (tex.width > 0 && tex.height > 0) {
        halfW = (float)tex.width / (float)tex.height * 0.5f;
    } else {
        halfW = 16.0f / 9.0f * 0.5f;
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
        if (hasTex) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.width, tex.height,
                         0, GL_RGBA, GL_FLOAT, tex.pixels.data());

            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(litF, litF, litF, 0.85f);

            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex3f(-halfW, -halfH, 0);
            glTexCoord2f(1, 0); glVertex3f( halfW, -halfH, 0);
            glTexCoord2f(1, 1); glVertex3f( halfW,  halfH, 0);
            glTexCoord2f(0, 1); glVertex3f(-halfW,  halfH, 0);
            glEnd();

            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
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
DevViewport3D::drawCameraNode(const SceneNode& sn) const
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
DevViewport3D::drawSphereNode(const SceneNode& sn) const
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
    const Sphere3D::CachedTexture& tex = sphere->getCachedTexture();
    const bool hasTex = (tex.width > 0 && tex.height > 0 && !tex.pixels.empty());

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
        if (hasTex) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.width, tex.height,
                         0, GL_RGBA, GL_FLOAT, tex.pixels.data());

            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (!lit) glColor4f(1.0f, 1.0f, 1.0f, 0.85f);

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
                    glColor4f(f, f, f, 0.85f);
                }
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    glTexCoord2f(sphereVerts[idx].u, sphereVerts[idx].v);
                    glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
                }
            }
            glEnd();

            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
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

        for (int row = 0; row <= rows; row += rowStep) {
            glBegin(GL_LINE_STRIP);
            for (int col = 0; col <= cols; ++col) {
                int idx = row * vertsPerRow + col;
                if (idx < (int)sphereVerts.size()) {
                    glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
                }
            }
            glEnd();
        }

        for (int col = 0; col <= cols; col += colStep) {
            glBegin(GL_LINE_STRIP);
            for (int row = 0; row <= rows; ++row) {
                int idx = row * vertsPerRow + col;
                if (idx < (int)sphereVerts.size()) {
                    glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
                }
            }
            glEnd();
        }

        glLineWidth(1.0f);
    }
}

void
DevViewport3D::drawCubeNode(const SceneNode& sn) const
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
    const Cube3D::CachedTexture& tex = cube->getCachedTexture();
    const bool hasTex = (tex.width > 0 && tex.height > 0 && !tex.pixels.empty());

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
        if (hasTex) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.width, tex.height,
                         0, GL_RGBA, GL_FLOAT, tex.pixels.data());

            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (!lit) glColor4f(1.0f, 1.0f, 1.0f, 0.85f);

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
                    glColor4f(f, f, f, 0.85f);
                }
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    glTexCoord2f(cubeVerts[idx].u, cubeVerts[idx].v);
                    glVertex3f(cubeVerts[idx].x, cubeVerts[idx].y, cubeVerts[idx].z);
                }
            }
            glEnd();

            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
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
DevViewport3D::drawCylinderNode(const SceneNode& sn) const
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
    const Cylinder3D::CachedTexture& tex = cyl->getCachedTexture();
    const bool hasTex = (tex.width > 0 && tex.height > 0 && !tex.pixels.empty());

    if (mode != eWireframe) {
        if (mode == eShadedWire) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        if (hasTex) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.width, tex.height,
                         0, GL_RGBA, GL_FLOAT, tex.pixels.data());

            glEnable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            const bool lit = (mode == eShaded || mode == eShadedWire);
            float mvForLit[16];
            if (lit) glGetFloatv(GL_MODELVIEW_MATRIX, mvForLit);
            if (!lit) glColor4f(1.0f, 1.0f, 1.0f, 0.85f);

            glBegin(GL_TRIANGLES);
            for (int t = 0; t < numTris; ++t) {
                const int i0 = triIndices[t * 3 + 0];
                const int i1 = triIndices[t * 3 + 1];
                const int i2 = triIndices[t * 3 + 2];
                if (i0 < 0 || i0 >= (int)cylVerts.size() ||
                    i1 < 0 || i1 >= (int)cylVerts.size() ||
                    i2 < 0 || i2 >= (int)cylVerts.size()) continue;
                if (lit) {
                    const float n0[3] = { cylVerts[i0].nx, cylVerts[i0].ny, cylVerts[i0].nz };
                    const float n1[3] = { cylVerts[i1].nx, cylVerts[i1].ny, cylVerts[i1].nz };
                    const float n2[3] = { cylVerts[i2].nx, cylVerts[i2].ny, cylVerts[i2].nz };
                    float f = ViewportLitFromVertexNormals(n0, n1, n2, mvForLit);
                    glColor4f(f, f, f, 0.85f);
                }
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = triIndices[t * 3 + vi];
                    glTexCoord2f(cylVerts[idx].u, cylVerts[idx].v);
                    glVertex3f(cylVerts[idx].x, cylVerts[idx].y, cylVerts[idx].z);
                }
            }
            glEnd();

            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
        } else {
            const bool lit = (mode == eShaded || mode == eShadedWire);
            float mvForLit[16];
            if (lit) glGetFloatv(GL_MODELVIEW_MATRIX, mvForLit);
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
                if (lit) {
                    const float n0[3] = { cylVerts[i0].nx, cylVerts[i0].ny, cylVerts[i0].nz };
                    const float n1[3] = { cylVerts[i1].nx, cylVerts[i1].ny, cylVerts[i1].nz };
                    const float n2[3] = { cylVerts[i2].nx, cylVerts[i2].ny, cylVerts[i2].nz };
                    float f = ViewportLitFromVertexNormals(n0, n1, n2, mvForLit);
                    glColor3f(baseGrey * f, baseGrey * f, baseGrey * f);
                }
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

void
DevViewport3D::drawVolumeNode(const SceneNode& sn) const
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
            int step = std::max(1, res / 12);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glPointSize(3.0f);
            glBegin(GL_POINTS);
            for (int z = 0; z < res; z += step) {
                for (int y = 0; y < res; y += step) {
                    for (int x = 0; x < res; x += step) {
                        float density = volData[z * res * res + y * res + x];
                        if (density < 0.05f) continue;
                        float lx = ((float)x / (float)res - 0.5f);
                        float ly = ((float)y / (float)res - 0.5f);
                        float lz = ((float)z / (float)res - 0.5f);
                        float alpha = std::min(1.0f, density * 0.8f);
                        glColor4f(colR, colG, colB, alpha);
                        glVertex3f(lx, ly, lz);
                    }
                }
            }
            glEnd();
            glDisable(GL_BLEND);
        }
    } else if (vdb) {
        // Read actual VDB bounds from the grid (cached per-path)
        float minX, minY, minZ, maxX, maxY, maxZ;
        if (vdb->getVDBBounds(time, minX, minY, minZ, maxX, maxY, maxZ)) {
            bboxMinX = minX; bboxMinY = minY; bboxMinZ = minZ;
            bboxMaxX = maxX; bboxMaxY = maxY; bboxMaxZ = maxZ;
        } else {
            // Fallback if no file loaded yet
            bboxMinX = -1.0f; bboxMinY = -1.0f; bboxMinZ = -1.0f;
            bboxMaxX = 1.0f;  bboxMaxY = 1.0f;  bboxMaxZ = 1.0f;
        }
        colR = 0.5f; colG = 0.7f; colB = 1.0f;
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
DevViewport3D::drawLightNode(const SceneNode& sn) const
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
DevViewport3D::drawTransformNode(const SceneNode& sn) const
{
    bool selected = (sn.name == _imp->selectedNodeName);
    float axisLen = 0.8f;
    float lineW = selected ? 3.0f : 2.0f;

    glPushMatrix();
    glMultMatrixf(sn.localMatrix);

    glLineWidth(lineW);
    glBegin(GL_LINES);

    // X axis — red
    glColor3f(1.0f, 0.2f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(axisLen, 0, 0);

    // Y axis — green
    glColor3f(0.2f, 1.0f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(0, axisLen, 0);

    // Z axis — blue
    glColor3f(0.3f, 0.3f, 1.0f);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, axisLen);

    glEnd();

    // Draw a small diamond/cross at the origin to mark the null
    glColor3f(1.0f, 0.8f, 0.0f); // yellow
    float d = 0.12f;
    glBegin(GL_LINES);
    glVertex3f(-d, 0, 0); glVertex3f(d, 0, 0);
    glVertex3f(0, -d, 0); glVertex3f(0, d, 0);
    glVertex3f(0, 0, -d); glVertex3f(0, 0, d);
    // Diamond shape in XY plane
    glVertex3f(0, d, 0); glVertex3f(d, 0, 0);
    glVertex3f(d, 0, 0); glVertex3f(0, -d, 0);
    glVertex3f(0, -d, 0); glVertex3f(-d, 0, 0);
    glVertex3f(-d, 0, 0); glVertex3f(0, d, 0);
    glEnd();

    glLineWidth(1.0f);
    glPopMatrix();
}

void
DevViewport3D::drawParticlesNode(const SceneNode& sn) const
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
DevViewport3D::drawPointCloudNode(const SceneNode& sn) const
{
    Q_UNUSED(sn);
    // Actual drawing is done by drawPointCloud() which uses the cached PointCloudDataPtr.
}

void
DevViewport3D::drawGroupNode(const SceneNode& sn) const
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
DevViewport3D::refreshPointCloud()
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

        DeepToPoints* dtp = dynamic_cast<DeepToPoints*>(effect.get());
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
