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

struct Viewport3DPrivate;

/**
 * @class A dockable 3D viewport panel for visualizing deep compositing data
 * as point clouds in 3D space. Uses QOpenGLWidget with orbit/pan/dolly camera.
 *
 * Follows the Histogram pattern: QOpenGLWidget + PanelWidget, Pimpl.
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

    /**
     * @brief Set point cloud data to render. Thread-safe.
     * Call this from the engine thread when DeepToPoints produces new data.
     */
    void setPointCloud(const PointCloudDataPtr& cloud, float pointSize = 2.0f);

public Q_SLOTS:

    void resetCamera();

    /**
     * @brief Scan the node graph for DeepToPoints nodes and grab
     * point cloud data from the first one found. Called automatically
     * on a timer, or manually via 'R' key.
     */
    void refreshPointCloud();

    void onCameraMenuTriggered(QAction* action);

public:
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;

private:

    virtual void initializeGL() OVERRIDE FINAL;
    virtual void paintGL() OVERRIDE FINAL;
    virtual void resizeGL(int w, int h) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL;
    virtual QSize sizeHint() const OVERRIDE FINAL;

    void drawGrid() const;
    void drawTransformGizmo() const;
    void selectObjectAtPosition(int screenX, int screenY);
    int hitTestGizmoAxis(int screenX, int screenY) const; // Returns 0=X, 1=Y, 2=Z, -1=none
    bool worldToScreen(float wx, float wy, float wz, float& sx, float& sy) const;
    void drawAxes() const;
    void drawPointCloud() const;
    void applyAlembicCamera() const;
    void showCameraMenu(const QPoint& globalPos);

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

    Gui* _gui;
    std::unique_ptr<Viewport3DPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORT3D_H
