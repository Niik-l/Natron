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

#ifndef NATRON_GUI_VIEWPORT3D_H
#define NATRON_GUI_VIEWPORT3D_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Global/GLIncludes.h"

CLANG_DIAG_OFF(uninitialized)
#include <QSize>
CLANG_DIAG_ON(uninitialized)

#include <QOpenGLWidget>

#include <Global/QtCompat.h>

#include "Engine/Dev/Deep/PointCloudData.h"
#include "Engine/Dev/Scene3D/SceneGraph.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

class Blast; // forward — full include not needed for the pointer return type
struct Viewport3DPrivate;

/**
 * @class Viewport3D — replacement 3D viewport using ImGuizmo demo camera architecture.
 *
 * Camera uses spherical coordinates (camYAngle, camXAngle, camDistance) and LookAt,
 * copied verbatim from the ImGuizmo demo's main.cpp.
 * Object matrices use ImGuizmo::RecomposeMatrixFromComponents so GL rendering
 * and ImGuizmo gizmo manipulation agree on orientation.
 */
class Viewport3D
    : public QOpenGLWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    Viewport3D(Gui* gui,
                  const QOpenGLWidget* shareWidget = NULL);

    virtual ~Viewport3D();

    Gui* getGui() const { return _gui; }

    /** @brief Get the camera view matrix (16 floats, row-major like ImGuizmo demo). */
    void getCameraView(float m16[16]) const;

    /** @brief Get the camera projection matrix (16 floats, row-major like ImGuizmo demo). */
    void getCameraProjection(float m16[16]) const;

    /**
     * @brief Set point cloud data to render. Thread-safe.
     */
    void setPointCloud(const PointCloudDataPtr& cloud, float pointSize = 2.0f);

public Q_SLOTS:

    void resetCamera();

    /**
     * @brief Scan the node graph for DeepToPoints nodes and grab
     * point cloud data from the first one found.
     */
    void refreshPointCloud();

public:
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    void toggleTransformSpace();
    bool isLocalSpace() const;

    /** Toggle the world-space grid in the viewport. Wired from Viewport3DTab's
     *  Grid button. */
    void setShowGrid(bool show);

    /** Look-through-camera mode. Pass null/empty NodePtr to revert to the
     *  free-orbit camera. Wired from Viewport3DTab's camera dropdown. */
    void setLookThroughCamera(const NodePtr& cameraNode);

    /** Current look-through camera node (may be null = orbit mode). */
    NodePtr getLookThroughCamera() const;

    /** Viewport shading style. Wired from Viewport3DTab's Shading dropdown.
     *  - eWireframe   wires only, textures suppressed (clean schematic).
     *  - eShaded      solid fill with per-face N.L diffuse lighting (light
     *                 from top-right-forward, ambient 0.15). Faceted look —
     *                 you can see individual polygons.
     *  - eShadedWire  shaded fill plus wireframe overlay (Maya default).
     *  - eFlat        solid fill, no lighting (the legacy "Shaded" behavior —
     *                 uniform colour per surface).
     */
    enum ShadingMode {
        eWireframe = 0,
        eShaded,
        eShadedWire,
        eFlat,
        eFaceOrientation   // front faces blue / back faces red (winding check)
    };
    void setShadingMode(ShadingMode mode);
    ShadingMode getShadingMode() const;

    // Isolate Selected — when enabled, only the selected node (and its
    // descendants) is drawn. Toggled from the Viewport3DTab toolbar.
    void setIsolateSelected(bool enabled);
    bool isIsolateSelected() const;

    // Pause Updates / Force New Render — mirrors the 2D viewer's pause + refresh.
    /** Pause (true) / resume (false). On-demand rendering: while paused, requestRedraw()
     *  and frame-change updates are ignored; manual camera nav still repaints. Resume
     *  repaints once to catch up. Toggled from the Viewport3DTab toolbar. */
    void setPaused(bool paused);
    bool isPaused() const;
    /** Event-driven repaint request (from Gui::redrawAllViewers on knob/graph changes).
     *  No-op while paused. */
    void requestRedraw();
    /** One-shot: re-pull the scene (point cloud) and repaint once, even when paused. */
    void forceRefresh();

private:

    // Which viewport currently drives ImGuizmo. Its context is a file-static
    // inside ImGuizmo (not per ImGui context), so only one Viewport3D may call
    // Manipulate — otherwise two open viewports overwrite each other's
    // view/projection and in-progress drag, and dragging geo jumps.
    static Viewport3D* s_gizmoOwner;

    virtual void initializeGL() OVERRIDE FINAL;
    virtual void paintGL() OVERRIDE FINAL;
    virtual void resizeGL(int w, int h) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL;
    virtual QSize sizeHint() const OVERRIDE FINAL;

    void drawGrid() const;
    void drawAxes() const;
    void drawPointCloud() const;
    bool pickPointAtPosition(int screenX, int screenY);
    void boxSelectPoints();
    // Mirror the current point selection to the Engine-side PointCloudProvider
    // (CameraTracker scene-orientation tools etc.). Caller holds cloudMutex.
    void notifyProviderSelectionChanged();
    void selectObjectAtPosition(int screenX, int screenY);

    /** Find the active Blast node (set during the render scan when scanning
     *  for the point cloud to display). Returns nullptr if no Blast is in the
     *  scene. */
    Blast* getActiveBlast() const;

    /** Show the Blast context menu at the given global position. Called from
     *  mouseReleaseEvent on right-button release ONLY when the click had no
     *  drag (so right-drag for navigation doesn't trigger the menu).
     *  Always shows; actions that need an active Blast or a selection are
     *  disabled when unavailable. */
    void showBlastContextMenu(const QPoint& globalPos);

    /** Houdini-style point deletion: create a Blast node (Selection mode)
     *  downstream of the currently displayed point-cloud provider, seeded
     *  with the current viewport selection. Each call chains a new Blast,
     *  so every delete is individually undoable/disableable. Bound to the
     *  Delete key and the context-menu "Create Blast from Selection". */
    void createBlastFromSelection();

    // Per-node draw methods (called from paintGL via SceneGraph traversal)
    void drawMeshNode(const SceneNode& sn) const;
    void drawCardNode(const SceneNode& sn) const;
    void drawCameraNode(const SceneNode& sn) const;
    void drawPointCloudNode(const SceneNode& sn) const;
    void drawGroupNode(const SceneNode& sn) const;
    void drawSphereNode(const SceneNode& sn) const;
    void drawCubeNode(const SceneNode& sn) const;
    void drawCylinderNode(const SceneNode& sn) const;
    void drawParticlesNode(const SceneNode& sn) const;
    void drawVolumeNode(const SceneNode& sn) const;
    void drawLightNode(const SceneNode& sn) const;
    void drawTransformNode(const SceneNode& sn) const;

    Gui* _gui;
    std::unique_ptr<Viewport3DPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORT3D_H
