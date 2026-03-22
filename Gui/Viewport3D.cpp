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

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QWheelEvent>
#include <QKeyEvent>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "Engine/AppInstance.h"
#include "Engine/Dev/Scene3D/CameraProvider.h"
#include "Engine/Dev/Scene3D/Card3D.h"
#include "Engine/Dev/Particles/ParticleEmitter.h"
#include "Engine/Dev/Particles/ParticleGravity.h"
#include "Engine/Dev/Scene3D/ReadVDB.h"
#include "Engine/Dev/Scene3D/Volume3D.h"
#include "Engine/Dev/Scene3D/Cube3D.h"
#include "Engine/Dev/Scene3D/Cylinder3D.h"
#include "Engine/Dev/Scene3D/ReadAlembicCamera.h"
#include "Engine/Dev/Scene3D/Sphere3D.h"
#include "Engine/Dev/Scene3D/Group3D.h"
#include "Engine/Dev/Scene3D/ReadGeo.h"
#include "Engine/TimeLine.h"
#include "Engine/Dev/Deep/DeepToPoints.h"
#include "Engine/Dev/Scene3D/SceneGraph.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Camera3D.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"

NATRON_NAMESPACE_ENTER

struct Viewport3DPrivate
{
    Camera3D camera;

    // Mouse state
    int lastMouseX, lastMouseY;
    bool orbiting;
    bool panning;

    // Point cloud data (thread-safe)
    mutable QMutex cloudMutex;
    PointCloudDataPtr pointCloud;
    float pointSize;

    // Viewport size
    int viewW, viewH;

    // Look-through camera mode
    bool lookThroughCamera;
    std::string selectedCameraNodeName; // empty = first found, or specific node name

    // Selection and gizmo state
    enum GizmoMode { eGizmoNone, eGizmoTranslate, eGizmoRotate, eGizmoScale };
    GizmoMode gizmoMode;
    std::string selectedNodeName;  // script name of selected node
    int selectedCardIndex;         // -1 = node itself, 0+ = specific card
    int draggingAxis;              // -2 = none, 0 = X, 1 = Y, 2 = Z
    int hoveredAxis;               // -1 = none, 0 = X, 1 = Y, 2 = Z (for highlighting)

    // Cached GL matrices from last paintGL (for hit testing during mouse events)
    double cachedModelview[16];
    double cachedProjection[16];
    int cachedViewport[4];
    float gizmoStartX, gizmoStartY; // mouse pos when drag started

    // Scene graph (rebuilt each frame)
    SceneGraph sceneGraph;

    // Refresh timer
    QTimer* refreshTimer;

    Viewport3DPrivate()
        : camera()
        , lastMouseX(0)
        , lastMouseY(0)
        , orbiting(false)
        , panning(false)
        , pointSize(2.0f)
        , viewW(100)
        , viewH(100)
        , lookThroughCamera(false)
        , gizmoMode(eGizmoTranslate)
        , selectedCardIndex(-1)
        , draggingAxis(-2)
        , hoveredAxis(-1)
        , gizmoStartX(0)
        , gizmoStartY(0)
        , refreshTimer(nullptr)
    {
    }
};


Viewport3D::Viewport3D(Gui* gui,
                       const QOpenGLWidget* shareWidget)
    : QOpenGLWidget()
    , _gui(gui)
    , _imp(new Viewport3DPrivate())
{
    Q_UNUSED(shareWidget);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
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
Viewport3D::resetCamera()
{
    _imp->camera.reset();
    update();
}

void
Viewport3D::initializeGL()
{
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glEnable(GL_POINT_SMOOTH);

    // Refresh at 30fps for interaction + auto-scan for point cloud data
    _imp->refreshTimer = new QTimer(this);
    connect(_imp->refreshTimer, SIGNAL(timeout()), this, SLOT(update()));
    _imp->refreshTimer->start(33);

    // Point cloud scanning is now done in paintGL directly
    // to ensure stale data is cleared immediately when nodes are deleted.
}

void
Viewport3D::resizeGL(int w, int h)
{
    _imp->viewW = w;
    _imp->viewH = h;
    _imp->camera.setAspect((float)w / std::max(1, h));
    glViewport(0, 0, w, h);
}

void
Viewport3D::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Camera setup (same as before)
    if (_imp->lookThroughCamera) {
        applyAlembicCamera();
    } else {
        _imp->camera.applyGL();
    }

    drawGrid();
    drawAxes();

    // Clear point cloud, then only re-set if an active DeepToPoints node exists.
    // Must use getNodes_recursive(_, true) — getNodes() returns deactivated/deleted nodes too.
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

    // Rebuild scene graph from node list
    Gui* gui = getGui();
    if (gui) {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            ProjectPtr project = app->getProject();
            if (project) {
                double time = app->getTimeLine()->currentFrame();
                NodesList nodes;
                project->getNodes_recursive(nodes, true); // only active nodes
                _imp->sceneGraph.rebuild(nodes, time);
            }
        }
    }

    // Render all scene nodes in one pass
    const std::vector<SceneNode>& sceneNodes = _imp->sceneGraph.nodes();
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (!sn.visible) continue;
        if (sn.sourceNode.lock() == nullptr) continue;

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
            case eSceneNodeParticles:  drawParticlesNode(sn); break;
            case eSceneNodeVolume:     drawVolumeNode(sn); break;
            case eSceneNodeLight:      drawLightNode(sn); break;
        }

        glPopMatrix();
    }

    // Draw point cloud (drawPointCloud checks internally if data exists)
    drawPointCloud();

    // Only draw camera frustums from scene graph when not looking through
    // (already handled in the traversal above via eSceneNodeCamera)

    drawTransformGizmo();

    // Cache matrices for hit testing
    glGetDoublev(GL_MODELVIEW_MATRIX, _imp->cachedModelview);
    glGetDoublev(GL_PROJECTION_MATRIX, _imp->cachedProjection);
    glGetIntegerv(GL_VIEWPORT, _imp->cachedViewport);
}

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
    // X axis — red
    glColor3f(0.8f, 0.2f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(3, 0, 0);
    // Y axis — green
    glColor3f(0.2f, 0.8f, 0.2f);
    glVertex3f(0, 0, 0); glVertex3f(0, 3, 0);
    // Z axis — blue
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
    glDisable(GL_DEPTH_TEST); // Points render better without depth fighting

    // Use vertex arrays for performance
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    // Interleaved: [x,y,z,r,g,b] per point, stride = 6 floats
    glVertexPointer(3, GL_FLOAT, PointCloudData::stride(), data);
    glColorPointer(3, GL_FLOAT, PointCloudData::stride(), data + 3);

    glDrawArrays(GL_POINTS, 0, (GLsizei)numPoints);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    glEnable(GL_DEPTH_TEST);
}

void
Viewport3D::drawTransformGizmo() const
{
    if (_imp->selectedNodeName.empty()) return;

    // Use SceneGraph to find the selected node's world position
    float gizmoPos[3] = {0, 0, 0};
    bool found = false;

    const std::vector<SceneNode>& sceneNodes = _imp->sceneGraph.nodes();
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (sn.name != _imp->selectedNodeName) continue;

        // For cards, also match subIndex
        if (sn.type == eSceneNodeCard && _imp->selectedCardIndex >= 0 &&
            sn.subIndex != _imp->selectedCardIndex) continue;

        // World matrix column-major: translation is at [12], [13], [14]
        gizmoPos[0] = sn.worldMatrix[12];
        gizmoPos[1] = sn.worldMatrix[13];
        gizmoPos[2] = sn.worldMatrix[14];
        found = true;
        break;
    }

    if (!found) return;

    // Draw gizmo at the object's position
    glDisable(GL_DEPTH_TEST); // Gizmo always on top

    float arrowLen = 1.5f;
    float arrowHead = 0.15f;

    glPushMatrix();
    glTranslatef(gizmoPos[0], gizmoPos[1], gizmoPos[2]);

    if (_imp->gizmoMode == Viewport3DPrivate::eGizmoTranslate) {
        glLineWidth(3.0f);
        glBegin(GL_LINES);

        int activeAxis = (_imp->draggingAxis >= 0) ? _imp->draggingAxis : _imp->hoveredAxis;

        // X axis — Red (bright yellow when hovered/dragged)
        if (activeAxis == 0) glColor3f(1.0f, 1.0f, 0.3f);
        else glColor3f(1.0f, 0.2f, 0.2f);
        glVertex3f(0, 0, 0); glVertex3f(arrowLen, 0, 0);
        glVertex3f(arrowLen, 0, 0); glVertex3f(arrowLen - arrowHead, arrowHead, 0);
        glVertex3f(arrowLen, 0, 0); glVertex3f(arrowLen - arrowHead, -arrowHead, 0);

        // Y axis — Green (bright yellow when hovered/dragged)
        if (activeAxis == 1) glColor3f(1.0f, 1.0f, 0.3f);
        else glColor3f(0.2f, 1.0f, 0.2f);
        glVertex3f(0, 0, 0); glVertex3f(0, arrowLen, 0);
        glVertex3f(0, arrowLen, 0); glVertex3f(arrowHead, arrowLen - arrowHead, 0);
        glVertex3f(0, arrowLen, 0); glVertex3f(-arrowHead, arrowLen - arrowHead, 0);

        // Z axis — Blue (bright yellow when hovered/dragged)
        if (activeAxis == 2) glColor3f(1.0f, 1.0f, 0.3f);
        else glColor3f(0.2f, 0.2f, 1.0f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, arrowLen);
        glVertex3f(0, 0, arrowLen); glVertex3f(0, arrowHead, arrowLen - arrowHead);
        glVertex3f(0, 0, arrowLen); glVertex3f(0, -arrowHead, arrowLen - arrowHead);

        glEnd();
    } else if (_imp->gizmoMode == Viewport3DPrivate::eGizmoRotate) {
        // Draw rotation circles
        glLineWidth(2.0f);
        float radius = arrowLen * 0.8f;
        int segments = 48;

        // X rotation (red circle in YZ plane)
        glColor3f(1.0f, 0.2f, 0.2f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segments; ++i) {
            float angle = (float)i / segments * 2.0f * (float)M_PI;
            glVertex3f(0, radius * cosf(angle), radius * sinf(angle));
        }
        glEnd();

        // Y rotation (green circle in XZ plane)
        glColor3f(0.2f, 1.0f, 0.2f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segments; ++i) {
            float angle = (float)i / segments * 2.0f * (float)M_PI;
            glVertex3f(radius * cosf(angle), 0, radius * sinf(angle));
        }
        glEnd();

        // Z rotation (blue circle in XY plane)
        glColor3f(0.2f, 0.2f, 1.0f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segments; ++i) {
            float angle = (float)i / segments * 2.0f * (float)M_PI;
            glVertex3f(radius * cosf(angle), radius * sinf(angle), 0);
        }
        glEnd();
    } else if (_imp->gizmoMode == Viewport3DPrivate::eGizmoScale) {
        // Draw scale handles (lines with boxes at the ends)
        glLineWidth(3.0f);
        float boxSize = 0.1f;

        glBegin(GL_LINES);
        glColor3f(1.0f, 0.2f, 0.2f);
        glVertex3f(0, 0, 0); glVertex3f(arrowLen, 0, 0);
        glColor3f(0.2f, 1.0f, 0.2f);
        glVertex3f(0, 0, 0); glVertex3f(0, arrowLen, 0);
        glColor3f(0.2f, 0.2f, 1.0f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, arrowLen);
        glEnd();

        // Boxes at ends
        float ends[3][3] = {{arrowLen, 0, 0}, {0, arrowLen, 0}, {0, 0, arrowLen}};
        float colors[3][3] = {{1,0.2f,0.2f}, {0.2f,1,0.2f}, {0.2f,0.2f,1}};
        for (int a = 0; a < 3; ++a) {
            glColor3f(colors[a][0], colors[a][1], colors[a][2]);
            float cx = ends[a][0], cy = ends[a][1], cz = ends[a][2];
            glBegin(GL_LINE_LOOP);
            glVertex3f(cx - boxSize, cy - boxSize, cz - boxSize);
            glVertex3f(cx + boxSize, cy - boxSize, cz - boxSize);
            glVertex3f(cx + boxSize, cy + boxSize, cz - boxSize);
            glVertex3f(cx - boxSize, cy + boxSize, cz - boxSize);
            glEnd();
        }
    }

    glLineWidth(1.0f);
    glPopMatrix();
    glEnable(GL_DEPTH_TEST);
}

bool
Viewport3D::worldToScreen(float wx, float wy, float wz, float& sx, float& sy) const
{
    // Use cached GL matrices from last paintGL
    const double* mv = _imp->cachedModelview;
    const double* pj = _imp->cachedProjection;
    const int* vp = _imp->cachedViewport;

    // Multiply by modelview
    double x = mv[0]*wx + mv[4]*wy + mv[8]*wz + mv[12];
    double y = mv[1]*wx + mv[5]*wy + mv[9]*wz + mv[13];
    double z = mv[2]*wx + mv[6]*wy + mv[10]*wz + mv[14];
    double w = mv[3]*wx + mv[7]*wy + mv[11]*wz + mv[15];

    // Multiply by projection
    double cx = pj[0]*x + pj[4]*y + pj[8]*z + pj[12]*w;
    double cy = pj[1]*x + pj[5]*y + pj[9]*z + pj[13]*w;
    double cw = pj[3]*x + pj[7]*y + pj[11]*z + pj[15]*w;

    if (std::abs(cw) < 0.0001) return false; // Behind camera

    // NDC to screen
    double ndcX = cx / cw;
    double ndcY = cy / cw;

    sx = (float)(vp[0] + (1.0 + ndcX) * vp[2] * 0.5);
    sy = (float)(vp[1] + (1.0 + ndcY) * vp[3] * 0.5);
    // Flip Y for Qt (screen coords: 0 at top)
    sy = (float)_imp->viewH - sy;

    return true;
}

int
Viewport3D::hitTestGizmoAxis(int screenX, int screenY) const
{
    if (_imp->selectedNodeName.empty()) return -1;
    if (_imp->gizmoMode != Viewport3DPrivate::eGizmoTranslate) return -1;

    // Get gizmo position from SceneGraph
    float gizmoPos[3] = {0, 0, 0};

    const std::vector<SceneNode>& sceneNodes = _imp->sceneGraph.nodes();
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (sn.name != _imp->selectedNodeName) continue;
        if (sn.type == eSceneNodeCard && _imp->selectedCardIndex >= 0 &&
            sn.subIndex != _imp->selectedCardIndex) continue;

        gizmoPos[0] = sn.worldMatrix[12];
        gizmoPos[1] = sn.worldMatrix[13];
        gizmoPos[2] = sn.worldMatrix[14];
        break;
    }

    // Project gizmo origin and axis endpoints to screen
    float arrowLen = 1.5f;
    float originSX, originSY;
    if (!worldToScreen(gizmoPos[0], gizmoPos[1], gizmoPos[2], originSX, originSY)) return -1;

    float axisEndpoints[3][3] = {
        {gizmoPos[0] + arrowLen, gizmoPos[1], gizmoPos[2]},           // X
        {gizmoPos[0], gizmoPos[1] + arrowLen, gizmoPos[2]},           // Y
        {gizmoPos[0], gizmoPos[1], gizmoPos[2] + arrowLen}            // Z
    };

    float hitThreshold = 15.0f; // pixels
    int bestAxis = -1;
    float bestDist = hitThreshold;

    for (int a = 0; a < 3; ++a) {
        float endSX, endSY;
        if (!worldToScreen(axisEndpoints[a][0], axisEndpoints[a][1], axisEndpoints[a][2], endSX, endSY)) continue;

        // Distance from mouse to the line segment (origin → endpoint) in screen space
        float dx = endSX - originSX;
        float dy = endSY - originSY;
        float lineLen = sqrtf(dx*dx + dy*dy);
        if (lineLen < 1.0f) continue;

        // Project mouse onto the line
        float mx = screenX - originSX;
        float my = screenY - originSY;
        float t = (mx * dx + my * dy) / (lineLen * lineLen);
        t = std::max(0.0f, std::min(1.0f, t));

        float closestX = originSX + t * dx;
        float closestY = originSY + t * dy;

        float dist = sqrtf((screenX - closestX) * (screenX - closestX) +
                           (screenY - closestY) * (screenY - closestY));

        if (dist < bestDist) {
            bestDist = dist;
            bestAxis = a;
        }
    }

    return bestAxis;
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

    // Project each node's world-space origin to screen and find closest to click
    float bestDist = 60.0f; // max pick distance in pixels
    int bestIdx = -1;

    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (sn.type == eSceneNodePointCloud) continue;
        if (sn.sourceNode.lock() == nullptr) continue;

        // Node's world-space position is column 3 of worldMatrix (translation)
        float wx = sn.worldMatrix[12];
        float wy = sn.worldMatrix[13];
        float wz = sn.worldMatrix[14];

        // Project to screen using cached GL matrices
        float projX, projY;
        if (!worldToScreen(wx, wy, wz, projX, projY)) continue;

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
        // Clicked empty space — deselect
        _imp->selectedNodeName.clear();
        _imp->selectedCardIndex = -1;
    }
    update();
}

void
Viewport3D::drawMeshNode(const SceneNode& sn) const
{
    // worldMatrix is already applied by paintGL via glMultMatrixf.
    // Just draw the wireframe mesh at local origin.
    NodePtr node = sn.sourceNode.lock();
    if (!node) return;

    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) return;

    ReadGeo* readGeo = dynamic_cast<ReadGeo*>(effect.get());
    if (!readGeo) return;

    MeshDataPtr mesh = readGeo->getMeshData(-1);
    if (!mesh || mesh->numVertices == 0) return;

    // Draw wireframe using GL_LINES with edge indices
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
Viewport3D::drawCardNode(const SceneNode& sn) const
{
    // worldMatrix already includes the card's translate/rotate/scale.
    // DON'T apply card transform again inside this function.
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

    // Try new Card3D first
    Card3D* card3dNew = dynamic_cast<Card3D*>(effect.get());
    if (card3dNew) {
        card3dNew->updateCachedTexture(time);
        const Card3D::CachedTexture& tex = card3dNew->getCachedTexture();

        // Aspect from texture or default
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
        return; // Unknown card type
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
Viewport3D::drawCameraNode(const SceneNode& sn) const
{
    // worldMatrix already positions the camera. Draw frustum at origin.
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

    // Try CameraProvider interface (Camera3DNode, ReadAlembicCamera)
    CameraProvider* camProvider = dynamic_cast<CameraProvider*>(effect.get());
    if (camProvider) {
        focalLength = camProvider->getCameraFocalLength(time);
        hAperture = camProvider->getCameraHAperture(time);
    }

    bool isStandaloneCamera = (camProvider != NULL);

    double fovRad = 2.0 * std::atan(hAperture / (2.0 * focalLength));
    float halfW = frustumLength * (float)std::tan(fovRad * 0.5);
    float halfH = halfW * 0.75f; // Assume 4:3 aspect for frustum display

    // Camera body (small box) — for standalone cameras
    if (isStandaloneCamera) {
        glColor3f(1.0f, 0.8f, 0.2f); // Yellow
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

        // Up indicator (small triangle on top)
        glColor3f(0.2f, 0.8f, 0.2f);
        glBegin(GL_LINE_STRIP);
        glVertex3f(-bs * 0.5f, bs, 0);
        glVertex3f(0, bs + bs * 0.7f, 0);
        glVertex3f(bs * 0.5f, bs, 0);
        glEnd();
    }

    // Draw frustum
    float frustumColor = isStandaloneCamera ? 0.6f : 0.8f;
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

    // Update cached texture from img input
    sphere->updateCachedTexture(time);

    // Generate sphere mesh
    std::vector<Sphere3D::SphereVertex> sphereVerts;
    std::vector<int> triIndices;
    sphere->generateSphereMesh(time, sphereVerts, triIndices);

    int rows = sphere->getRows(time);
    int cols = sphere->getColumns(time);
    int vertsPerRow = cols + 1;
    int numTris = (int)(triIndices.size() / 3);

    // Draw textured solid if texture available
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

    // Always draw wireframe on top
    bool selected = (sn.name == _imp->selectedNodeName);
    if (selected) {
        glColor3f(1.0f, 1.0f, 0.0f);
    } else {
        glColor3f(0.3f, 0.6f, 0.3f);
    }
    glLineWidth(selected ? 2.0f : 1.0f);

    // Sparse wireframe — every Nth line to avoid visual noise
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

    // Draw textured if available
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

    // Wireframe
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

    // Draw textured if available
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

    // Wireframe rings
    bool selected = (sn.name == _imp->selectedNodeName);
    glColor3f(selected ? 1.0f : 0.5f, selected ? 1.0f : 0.8f, selected ? 0.0f : 0.5f);
    glLineWidth(selected ? 2.0f : 1.0f);

    float r = (float)cyl->getRadius(time);
    float halfH = (float)cyl->getHeight(time) * 0.5f;
    int cols = cyl->getColumns(time);

    // Top ring
    glBegin(GL_LINE_LOOP);
    for (int c = 0; c < cols; ++c) {
        float theta = (float)c / (float)cols * 2.0f * (float)M_PI;
        glVertex3f(r * sinf(theta), halfH, r * cosf(theta));
    }
    glEnd();

    // Bottom ring
    glBegin(GL_LINE_LOOP);
    for (int c = 0; c < cols; ++c) {
        float theta = (float)c / (float)cols * 2.0f * (float)M_PI;
        glVertex3f(r * sinf(theta), -halfH, r * cosf(theta));
    }
    glEnd();

    // Vertical lines (every Nth column)
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

        // Point cloud preview for procedural volumes
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
        // For ReadVDB, just draw a unit bounding box
        // The worldMatrix already handles position/scale from the transform knobs
        // Don't call getVolumeData here — loading VDB from the paint thread is unsafe
        bboxMinX = -2.0f; bboxMinY = -1.0f; bboxMinZ = -2.0f;
        bboxMaxX = 2.0f; bboxMaxY = 2.0f; bboxMaxZ = 2.0f;
        colR = 0.5f; colG = 0.7f; colB = 1.0f;
    }

    // Bounding box wireframe
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

    // Draw a small sun/star shape
    float s = 0.3f;
    glColor3f(1.0f, 0.9f, 0.3f); // warm yellow
    glLineWidth(selected ? 3.0f : 2.0f);

    // Diamond shape
    glBegin(GL_LINE_LOOP);
    glVertex3f(0, s, 0);
    glVertex3f(s * 0.7f, 0, 0);
    glVertex3f(0, -s, 0);
    glVertex3f(-s * 0.7f, 0, 0);
    glEnd();

    // Rays
    float r = s * 1.5f;
    glBegin(GL_LINES);
    glVertex3f(0, s * 1.1f, 0); glVertex3f(0, r, 0);
    glVertex3f(0, -s * 1.1f, 0); glVertex3f(0, -r, 0);
    glVertex3f(s * 0.8f, 0, 0); glVertex3f(r * 0.7f, 0, 0);
    glVertex3f(-s * 0.8f, 0, 0); glVertex3f(-r * 0.7f, 0, 0);
    // Diagonal rays
    float d = s * 0.8f;
    float dr = r * 0.5f;
    glVertex3f(d * 0.5f, d * 0.5f, 0); glVertex3f(dr, dr, 0);
    glVertex3f(-d * 0.5f, d * 0.5f, 0); glVertex3f(-dr, dr, 0);
    glVertex3f(d * 0.5f, -d * 0.5f, 0); glVertex3f(dr, -dr, 0);
    glVertex3f(-d * 0.5f, -d * 0.5f, 0); glVertex3f(-dr, -dr, 0);
    glEnd();

    // Z-axis cross (so it's visible from all angles)
    glBegin(GL_LINE_LOOP);
    glVertex3f(0, s, 0);
    glVertex3f(0, 0, s * 0.7f);
    glVertex3f(0, -s, 0);
    glVertex3f(0, 0, -s * 0.7f);
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

    // Get particle data from the node
    ParticleDataPtr data;
    ParticleEmitter* emitter = dynamic_cast<ParticleEmitter*>(effect.get());
    ParticleGravity* gravity = dynamic_cast<ParticleGravity*>(effect.get());
    if (emitter) {
        data = emitter->getParticleData(time);
    } else if (gravity) {
        data = gravity->getParticleData(time);
    }

    if (!data || data->numParticles() == 0) return;

    // Render particles as colored points
    glPointSize(3.0f);
    glBegin(GL_POINTS);
    for (int i = 0; i < data->numParticles(); ++i) {
        const Particle& p = data->particles[i];
        float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
        float alpha = 1.0f - ageFrac; // fade out over life
        glColor4f(p.r, p.g, p.b, alpha);
        glVertex3f(p.px, p.py, p.pz);
    }
    glEnd();
    glPointSize(1.0f);
}

void
Viewport3D::drawPointCloudNode(const SceneNode& sn) const
{
    // Point clouds have identity worldMatrix; positions are in the vertex data.
    // Actual drawing is done by drawPointCloud() which uses the cached PointCloudDataPtr.
    // This is a no-op placeholder — drawPointCloud() is called separately in paintGL.
    Q_UNUSED(sn);
}

void
Viewport3D::drawGroupNode(const SceneNode& sn) const
{
    // worldMatrix already positions the group. Draw the box at origin.
    Q_UNUSED(sn);

    // Dashed wireframe cube (group boundary indicator)
    glColor3f(0.9f, 0.5f, 0.1f); // Orange
    glLineWidth(1.5f);
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, 0xAAAA); // Dashed line

    float s = 2.0f; // Group box size
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


void
Viewport3D::applyAlembicCamera() const
{
    Gui* gui = getGui();
    if (!gui) { _imp->camera.applyGL(); return; }

    GuiAppInstancePtr app = gui->getApp();
    if (!app) { _imp->camera.applyGL(); return; }

    ProjectPtr project = app->getProject();
    if (!project) { _imp->camera.applyGL(); return; }

    double time = app->getTimeLine()->currentFrame();

    NodesList nodes = project->getNodes();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        EffectInstancePtr effect = (*it)->getEffectInstance();
        if (!effect) continue;

        ReadAlembicCamera* abcCam = dynamic_cast<ReadAlembicCamera*>(effect.get());
        if (!abcCam) continue;

        // If a specific camera is selected, only use that one
        if (!_imp->selectedCameraNodeName.empty()) {
            std::string nodeName = (*it)->getScriptName();
            if (nodeName != _imp->selectedCameraNodeName) continue;
        }

        double tx, ty, tz, rx, ry, rz;
        abcCam->getCameraTransform(time, tx, ty, tz, rx, ry, rz);
        double focalLength = abcCam->getFocalLength(time);
        double hAperture = abcCam->getHAperture(time);

        // Build projection matrix from camera intrinsics
        float aspect = (float)_imp->viewW / std::max(1, _imp->viewH);
        double fovDeg = 2.0 * std::atan(hAperture / (2.0 * focalLength)) * (180.0 / M_PI);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        // Manual perspective matrix
        float f = 1.0f / tanf((float)fovDeg * 0.5f * (float)M_PI / 180.0f);
        float nearZ = 0.1f, farZ = 10000.0f;
        float proj[16] = {0};
        proj[0]  = f / aspect;
        proj[5]  = f;
        proj[10] = (farZ + nearZ) / (nearZ - farZ);
        proj[11] = -1.0f;
        proj[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        glLoadMatrixf(proj);

        // Build view matrix from camera transform
        // Apply inverse of camera transform (look FROM the camera)
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // Inverse rotation then inverse translation
        glRotatef(-(float)rz, 0, 0, 1);
        glRotatef(-(float)rx, 1, 0, 0);
        glRotatef(-(float)ry, 0, 1, 0);
        glTranslatef(-(float)tx, -(float)ty, -(float)tz);

        return; // Use first camera found
    }

    // No camera found, fall back to orbit
    _imp->camera.applyGL();
}

void
Viewport3D::showCameraMenu(const QPoint& globalPos)
{
    QMenu menu(this);

    // Orbit camera (default)
    QAction* orbitAction = menu.addAction(tr("Orbit Camera (default)"));
    orbitAction->setCheckable(true);
    orbitAction->setChecked(!_imp->lookThroughCamera);
    orbitAction->setData(QString::fromUtf8("__orbit__"));

    menu.addSeparator();

    // Find all ReadAlembicCamera nodes
    Gui* gui = getGui();
    if (gui) {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            ProjectPtr project = app->getProject();
            if (project) {
                NodesList nodes = project->getNodes();
                for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
                    EffectInstancePtr effect = (*it)->getEffectInstance();
                    if (!effect) continue;

                    ReadAlembicCamera* abcCam = dynamic_cast<ReadAlembicCamera*>(effect.get());
                    if (abcCam) {
                        std::string nodeName = (*it)->getScriptName();
                        QString label = QString::fromUtf8(nodeName.c_str());
                        QAction* camAction = menu.addAction(tr("Look through: %1").arg(label));
                        camAction->setCheckable(true);
                        camAction->setChecked(_imp->lookThroughCamera &&
                                              _imp->selectedCameraNodeName == nodeName);
                        camAction->setData(label);
                    }
                }
            }
        }
    }

    connect(&menu, SIGNAL(triggered(QAction*)), this, SLOT(onCameraMenuTriggered(QAction*)));
    menu.exec(globalPos);
}

void
Viewport3D::onCameraMenuTriggered(QAction* action)
{
    QString data = action->data().toString();
    if (data == QString::fromUtf8("__orbit__")) {
        _imp->lookThroughCamera = false;
        _imp->selectedCameraNodeName.clear();
    } else {
        _imp->lookThroughCamera = true;
        _imp->selectedCameraNodeName = data.toStdString();
    }
    update();
}


void
Viewport3D::mousePressEvent(QMouseEvent* e)
{
    _imp->lastMouseX = e->x();
    _imp->lastMouseY = e->y();

    if (e->button() == Qt::MiddleButton) {
        if (e->modifiers() & Qt::ShiftModifier) {
            _imp->panning = true;
        } else {
            _imp->orbiting = true;
        }
    } else if (e->button() == Qt::LeftButton) {
        if (e->modifiers() & Qt::AltModifier) {
            _imp->orbiting = true;
        } else {
            // Check if clicking on a gizmo axis first
            int hitAxis = hitTestGizmoAxis(e->x(), e->y());
            if (hitAxis >= 0) {
                // Start dragging the specific axis
                _imp->draggingAxis = hitAxis;
                _imp->gizmoStartX = (float)e->x();
                _imp->gizmoStartY = (float)e->y();
            } else {
                // Not on gizmo — select/deselect
                _imp->draggingAxis = -2; // -2 means "not dragging, just clicked"
                selectObjectAtPosition(e->x(), e->y());
            }
        }
    } else if (e->button() == Qt::RightButton) {
        if (e->modifiers() & Qt::AltModifier) {
            _imp->panning = true;
        } else {
            // Show camera selection context menu
            showCameraMenu(e->globalPos());
        }
    }
}

void
Viewport3D::mouseMoveEvent(QMouseEvent* e)
{
    int dx = e->x() - _imp->lastMouseX;
    int dy = e->y() - _imp->lastMouseY;
    _imp->lastMouseX = e->x();
    _imp->lastMouseY = e->y();

    if (_imp->orbiting) {
        _imp->camera.orbit(-dx * 0.005f, dy * 0.005f);
        update();
    } else if (_imp->panning) {
        _imp->camera.pan((float)dx, (float)dy);
        update();
    } else if (_imp->draggingAxis < 0) {
        // Not dragging — update hover highlight
        int newHover = hitTestGizmoAxis(e->x(), e->y());
        if (newHover != _imp->hoveredAxis) {
            _imp->hoveredAxis = newHover;
            update();
        }
    }

    if (_imp->draggingAxis >= 0 && !_imp->selectedNodeName.empty()) {
        // Gizmo dragging — axis already determined from hit test
        float sensitivity = _imp->camera.getRadius() * 0.003f;

        float delta = 0;
        if (_imp->draggingAxis == 0) {
            delta = dx * sensitivity; // X axis
        } else if (_imp->draggingAxis == 1) {
            delta = -dy * sensitivity; // Y axis (inverted for screen coords)
        } else if (_imp->draggingAxis == 2) {
            delta = dy * sensitivity; // Z axis (positive = into screen)
        }

        // Apply delta using SceneGraph to find the source node
        if (delta != 0) {
            // Find the selected scene node
            const std::vector<SceneNode>& sceneNodes = _imp->sceneGraph.nodes();
            for (size_t si = 0; si < sceneNodes.size(); ++si) {
                const SceneNode& sn = sceneNodes[si];
                if (sn.name != _imp->selectedNodeName) continue;
                if (sn.type == eSceneNodeCard && sn.subIndex != _imp->selectedCardIndex) continue;

                NodePtr node = sn.sourceNode.lock();
                if (!node) break;
                EffectInstancePtr effect = node->getEffectInstance();
                if (!effect) break;

                // Determine knob name based on node type and drag axis
                std::string knobName;
                const char* axisNames[3] = {"translateX", "translateY", "translateZ"};

                if (sn.type == eSceneNodeCard) {
                    knobName = axisNames[_imp->draggingAxis];
                } else if (sn.type == eSceneNodeGroup) {
                    knobName = axisNames[_imp->draggingAxis];
                    // Auto-enable group transform
                    KnobIPtr enKnob = effect->getKnobByName("enableTransform");
                    if (enKnob) {
                        KnobBool* boolKnob = dynamic_cast<KnobBool*>(enKnob.get());
                        if (boolKnob && !boolKnob->getValue()) boolKnob->setValue(true);
                    }
                } else if (sn.type == eSceneNodeCamera) {
                    knobName = axisNames[_imp->draggingAxis];
                } else if (sn.type == eSceneNodeMesh) {
                    knobName = axisNames[_imp->draggingAxis];
                } else if (sn.type == eSceneNodeSphere) {
                    knobName = axisNames[_imp->draggingAxis];
                } else if (sn.type == eSceneNodeCube) {
                    knobName = axisNames[_imp->draggingAxis];
                } else if (sn.type == eSceneNodeCylinder) {
                    knobName = axisNames[_imp->draggingAxis];
                } else if (sn.type == eSceneNodeVolume) {
                    knobName = axisNames[_imp->draggingAxis];
                } else if (sn.type == eSceneNodeLight) {
                    knobName = axisNames[_imp->draggingAxis];
                } else {
                    break;
                }

                if (!knobName.empty()) {
                    KnobIPtr knob = effect->getKnobByName(knobName);
                    if (knob) {
                        KnobDouble* dblKnob = dynamic_cast<KnobDouble*>(knob.get());
                        if (dblKnob) {
                            double curVal = dblKnob->getValue();
                            dblKnob->setValue(curVal + delta);
                        }
                    }
                }
                break;
            }
        }
        update();
    }
}

void
Viewport3D::mouseReleaseEvent(QMouseEvent* /*e*/)
{
    _imp->orbiting = false;
    _imp->panning = false;
    _imp->draggingAxis = -2; // Reset to "not dragging"
}

void
Viewport3D::wheelEvent(QWheelEvent* e)
{
    float delta = e->angleDelta().y() / 120.0f;
    _imp->camera.dolly(delta);
    update();
}

void
Viewport3D::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_C) {
        // Toggle look-through camera mode
        _imp->lookThroughCamera = !_imp->lookThroughCamera;
        update();
    } else if (e->key() == Qt::Key_F) {
        // Frame to point cloud bounds if available, otherwise reset
        QMutexLocker lock(&_imp->cloudMutex);
        if (_imp->pointCloud && _imp->pointCloud->numPoints() > 0) {
            float cx, cy, cz;
            _imp->pointCloud->getCenter(cx, cy, cz);
            float radius = _imp->pointCloud->getRadius();
            if (radius < 0.1f) radius = 2.0f;
            lock.unlock();
            _imp->camera.setTarget(cx, cy, cz);
            _imp->camera.setOrbit(_imp->camera.getTheta(), _imp->camera.getPhi(), radius * 2.5f);
        } else {
            lock.unlock();
            _imp->camera.reset();
        }
        update();
    } else if (e->key() == Qt::Key_R) {
        refreshPointCloud();
    } else if (e->key() == Qt::Key_W) {
        _imp->gizmoMode = Viewport3DPrivate::eGizmoTranslate;
        update();
    } else if (e->key() == Qt::Key_E) {
        _imp->gizmoMode = Viewport3DPrivate::eGizmoRotate;
        update();
    } else if (e->key() == Qt::Key_T) {
        _imp->gizmoMode = Viewport3DPrivate::eGizmoScale;
        update();
    } else if (e->key() == Qt::Key_Escape) {
        _imp->selectedNodeName.clear();
        _imp->selectedCardIndex = -1;
        update();
    } else {
        QOpenGLWidget::keyPressEvent(e);
    }
}

void
Viewport3D::refreshPointCloud()
{
    Gui* gui = getGui();
    if (!gui) return;

    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;

    ProjectPtr project = app->getProject();
    if (!project) return;

    // Scan all nodes for DeepToPoints instances
    NodesList nodes = project->getNodes();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        EffectInstancePtr effect = (*it)->getEffectInstance();
        if (!effect) continue;

        DeepToPoints* dtp = dynamic_cast<DeepToPoints*>(effect.get());
        if (dtp) {
            PointCloudDataPtr cloud = dtp->getPointCloud();
            if (cloud && cloud->numPoints() > 0) {
                // Get point size from the node's knob
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

    // No DeepToPoints node found — clear any stale point cloud data
    {
        QMutexLocker lock(&_imp->cloudMutex);
        if (_imp->pointCloud) {
            _imp->pointCloud.reset();
            update();
        }
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Viewport3D.cpp"
