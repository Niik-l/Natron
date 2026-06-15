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

#ifndef NATRON_GUI_VIEWPORT3DTAB_H
#define NATRON_GUI_VIEWPORT3DTAB_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include "Global/QtCompat.h"
#include "Global/GlobalDefines.h"

#include <QWidget>

#include "Gui/PanelWidget.h"
#include "Gui/GuiFwd.h"

class QLabel;
class QSpinBox;
class QToolButton;

NATRON_NAMESPACE_ENTER

class TimeLineGui;
class Viewport3D;

/**
 * @brief Wrapper panel for the 3D viewport, matching the 2D viewer layout.
 *
 * Layout:
 *   Top toolbar — camera selection, gizmo mode, display options
 *   Viewport3D — the OpenGL 3D viewport
 *   Bottom bar — frame indicator and basic playback
 */
class Viewport3DTab
    : public QWidget
    , public PanelWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    Viewport3DTab(Gui* gui, QWidget* parent = NULL);
    virtual ~Viewport3DTab();

    Viewport3D* getViewport() const { return _viewport; }

public Q_SLOTS:

    void onGizmoTranslate();
    void onGizmoRotate();
    void onGizmoScale();
    void onResetCamera();
    void onToggleGrid();
    void onCyclesRender();
    void onFrameChanged(SequenceTime frame, int reason);
    void onPlayForward();
    void onPlayBackward();
    void onPrevFrame();
    void onNextFrame();
    void onFirstFrame();
    void onLastFrame();
    void onFrameSpinChanged(int value);
    void onToggleTransformSpace();

    /** Pause Updates (checkable) + Force New Render — mirror the 2D viewer. */
    void onTogglePause(bool paused);
    void onForceRefresh();

    /** Build the camera dropdown menu by enumerating CameraProvider nodes in
     *  the project right now. Called on aboutToShow so we always see fresh
     *  state (without subscribing to node-graph changes). */
    void onCameraMenuAboutToShow();
    void onCameraSelected();

    /** Shading-mode dropdown — Wireframe / Shaded / Shaded+Wire.
     *  QAction sender carries the chosen mode in its data() (int). */
    void onShadingModeSelected();

private:

    virtual void enterEvent(QtCompat::QEnterEvent* e) OVERRIDE FINAL;
    virtual void leaveEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;

    Viewport3D* _viewport;
    TimeLineGui* _timelineGui;
    QLabel* _frameLabel;
    QSpinBox* _frameSpin;
    QSpinBox* _rangeStart;
    QSpinBox* _rangeEnd;
    QToolButton* _translateBtn;
    QToolButton* _rotateBtn;
    QToolButton* _scaleBtn;
    QToolButton* _gridBtn;
    QToolButton* _spaceBtn;
    QToolButton* _cameraDropdown;
    QToolButton* _shadingDropdown;
    QToolButton* _pauseBtn;
    QToolButton* _refreshBtn;
    bool _gridVisible;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORT3DTAB_H
