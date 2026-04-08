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
#include "Engine/Dev/Scene3D/Sphere3D.h"
#include "Engine/Dev/Scene3D/Group3D.h"
#include "Engine/Dev/Scene3D/ReadGeo.h"
#include "Engine/Dev/Scene3D/Light3D.h"
#include "Engine/TimeLine.h"
#include "Engine/Dev/Deep/DeepToPoints.h"
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

void
DevViewport3D::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 1. Build camera matrices (demo-style spherical coords -> LookAt)
    float eye[3];
    eye[0] = cosf(_imp->camYAngle) * cosf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[0];
    eye[1] = sinf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[1];
    eye[2] = sinf(_imp->camYAngle) * cosf(_imp->camXAngle) * _imp->camDistance + _imp->camTarget[2];
    float at[3] = { _imp->camTarget[0], _imp->camTarget[1], _imp->camTarget[2] };
    float up[3] = { 0.f, 1.f, 0.f };
    LookAt(eye, at, up, _imp->cameraView);

    float aspect = (_imp->viewH > 0) ? (float)_imp->viewW / (float)_imp->viewH : 1.0f;
    Perspective(_imp->fov, aspect, 0.1f, 500.f, _imp->cameraProjection);

    // 2. Set GL matrices
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(_imp->cameraProjection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(_imp->cameraView);

    // 3. Draw grid + axes
    drawGrid();
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
                        EffectInstancePtr eff = (*it)->getEffectInstance();
                        if (!eff) continue;
                        DeepToPoints* dtp = dynamic_cast<DeepToPoints*>(eff.get());
                        if (dtp) {
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
                                break;
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

        // Build object matrix from knobs using ImGuizmo's RecomposeMatrixFromComponents
        float translation[3], rotation[3], scale[3];
        readTRSFromNode(effect, translation, rotation, scale);
        float objMatrix[16];
        ImGuizmo::RecomposeMatrixFromComponents(translation, rotation, scale, objMatrix);

        glPushMatrix();
        glMultMatrixf(objMatrix);

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
            selectObjectAtPosition(e->x(), e->y());
        }
    } else if (e->button() == Qt::RightButton) {
        if (e->modifiers() & Qt::AltModifier) {
            _imp->zooming = true;
        }
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

    _imp->orbiting = false;
    _imp->panning = false;
    _imp->zooming = false;
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
        // Frame selected
        if (!_imp->selectedNodeName.empty()) {
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
                    }
                    break;
                }
            }
        } else {
            // Frame point cloud or reset
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
            } else {
                lock.unlock();
                resetCamera();
                return;
            }
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
    glDisable(GL_DEPTH_TEST);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glVertexPointer(3, GL_FLOAT, PointCloudData::stride(), data);
    glColorPointer(3, GL_FLOAT, PointCloudData::stride(), data + 3);

    glDrawArrays(GL_POINTS, 0, (GLsizei)numPoints);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    glEnable(GL_DEPTH_TEST);
}

void
DevViewport3D::drawMeshNode(const SceneNode& sn) const
{
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) return;

    ReadGeo* readGeo = dynamic_cast<ReadGeo*>(effect.get());
    if (!readGeo) return;

    MeshDataPtr mesh = readGeo->getMeshData(-1);
    if (!mesh || mesh->numVertices == 0) return;

    glColor3f(0.7f, 0.7f, 0.7f);
    glLineWidth(1.0f);

    if (!mesh->edgeIndices.empty()) {
        glBegin(GL_LINES);
        for (size_t i = 0; i + 1 < mesh->edgeIndices.size(); i += 2) {
            int i0 = mesh->edgeIndices[i];
            int i1 = mesh->edgeIndices[i + 1];
            if (i0 >= 0 && i0 < (int)mesh->numVertices &&
                i1 >= 0 && i1 < (int)mesh->numVertices) {
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
    if (card3dNew) {
        card3dNew->updateCachedTexture(time);
        const Card3D::CachedTexture& tex = card3dNew->getCachedTexture();

        if (tex.width > 0 && tex.height > 0) {
            halfW = (float)tex.width / (float)tex.height * 0.5f;
        } else {
            halfW = 16.0f / 9.0f * 0.5f;
        }

        if (tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
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
            glColor4f(1.0f, 1.0f, 1.0f, 0.85f);

            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex3f(-halfW, -halfH, 0);
            glTexCoord2f(1, 0); glVertex3f( halfW, -halfH, 0);
            glTexCoord2f(1, 1); glVertex3f( halfW,  halfH, 0);
            glTexCoord2f(0, 1); glVertex3f(-halfW,  halfH, 0);
            glEnd();

            glDisable(GL_TEXTURE_2D);
            glDisable(GL_BLEND);
            glDeleteTextures(1, &glTex);
        }
    } else {
        return;
    }

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

    const Sphere3D::CachedTexture& tex = sphere->getCachedTexture();
    if (tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
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
        glColor4f(1.0f, 1.0f, 1.0f, 0.85f);

        glBegin(GL_TRIANGLES);
        for (int t = 0; t < numTris; ++t) {
            for (int vi = 0; vi < 3; ++vi) {
                int idx = triIndices[t * 3 + vi];
                if (idx >= 0 && idx < (int)sphereVerts.size()) {
                    glTexCoord2f(sphereVerts[idx].u, sphereVerts[idx].v);
                    glVertex3f(sphereVerts[idx].x, sphereVerts[idx].y, sphereVerts[idx].z);
                }
            }
        }
        glEnd();

        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glDeleteTextures(1, &glTex);
    }

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

    const Cube3D::CachedTexture& tex = cube->getCachedTexture();
    if (tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
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
        glColor4f(1.0f, 1.0f, 1.0f, 0.85f);

        glBegin(GL_TRIANGLES);
        for (int t = 0; t < numTris; ++t) {
            for (int vi = 0; vi < 3; ++vi) {
                int idx = triIndices[t * 3 + vi];
                if (idx >= 0 && idx < (int)cubeVerts.size()) {
                    glTexCoord2f(cubeVerts[idx].u, cubeVerts[idx].v);
                    glVertex3f(cubeVerts[idx].x, cubeVerts[idx].y, cubeVerts[idx].z);
                }
            }
        }
        glEnd();

        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glDeleteTextures(1, &glTex);
    }

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

    const Cylinder3D::CachedTexture& tex = cyl->getCachedTexture();
    if (tex.width > 0 && tex.height > 0 && !tex.pixels.empty()) {
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
        glColor4f(1.0f, 1.0f, 1.0f, 0.85f);

        glBegin(GL_TRIANGLES);
        for (int t = 0; t < numTris; ++t) {
            for (int vi = 0; vi < 3; ++vi) {
                int idx = triIndices[t * 3 + vi];
                if (idx >= 0 && idx < (int)cylVerts.size()) {
                    glTexCoord2f(cylVerts[idx].u, cylVerts[idx].v);
                    glVertex3f(cylVerts[idx].x, cylVerts[idx].y, cylVerts[idx].z);
                }
            }
        }
        glEnd();

        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glDeleteTextures(1, &glTex);
    }

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
        bboxMinX = -2.0f; bboxMinY = -1.0f; bboxMinZ = -2.0f;
        bboxMaxX = 2.0f; bboxMaxY = 2.0f; bboxMaxZ = 2.0f;
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
