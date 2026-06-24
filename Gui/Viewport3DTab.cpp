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

#include "Viewport3DTab.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QKeyEvent>
#include <QFrame>
#include <QSpinBox>
#include <QPixmap>
#include <QIcon>
#include <QMenu>
#include <QAction>
#include <QActionGroup>

#include "Gui/Button.h"
#include "Gui/GuiDefines.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/SpinBox.h"
#include "Gui/TimeLineGui.h"
#include "Gui/Viewport3D.h"
#include "Engine/TimeLine.h"
#include "Engine/Project.h"
#include "Engine/Node.h"
#include "Engine/Dev/Scene3D/CameraProvider.h"

#ifdef NATRON_CYCLES
#include "Engine/Dev/Cycles/CyclesRenderer.h"
#include "Engine/Dev/Scene3D/SceneGraph.h"
#include <QMessageBox>
#include <QFileDialog>
#endif

NATRON_NAMESPACE_ENTER

Viewport3DTab::Viewport3DTab(Gui* gui, QWidget* parent)
    : QWidget(parent)
    , PanelWidget(this, gui)
    , _viewport(NULL)
    , _timelineGui(NULL)
    , _frameLabel(NULL)
    , _frameSpin(NULL)
    , _rangeStart(NULL)
    , _rangeEnd(NULL)
    , _translateBtn(NULL)
    , _rotateBtn(NULL)
    , _scaleBtn(NULL)
    , _gridBtn(NULL)
    , _spaceBtn(NULL)
    , _cameraDropdown(NULL)
    , _shadingDropdown(NULL)
    , _gridVisible(true)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ==================== Top Toolbar ====================
    QWidget* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    // The toolbar is full of fixed-size buttons + non-eliding text dropdowns, so its
    // layout minimum width is large (~700px). Without this, that minimum propagates up
    // as the whole tab's minimum width, and a splitter pane holding the 3D viewport
    // can't be dragged narrower than it — once it hits the floor the splitter collapses
    // the pane ("snaps away"). Ignoring the horizontal size hint lets the toolbar be
    // clipped instead of dictating the tab minimum, matching how the 2D viewer (whose
    // toolbar widgets shrink/elide on their own) behaves. Vertical policy untouched.
    {
        QSizePolicy sp = toolbar->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Ignored);
        toolbar->setSizePolicy(sp);
    }
    QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 2, 4, 2);
    toolbarLayout->setSpacing(4);

    // Gizmo mode buttons
    _translateBtn = new QToolButton(toolbar);
    _translateBtn->setText(QString::fromUtf8("W"));
    _translateBtn->setToolTip(QString::fromUtf8("Translate (W)"));
    _translateBtn->setCheckable(true);
    _translateBtn->setChecked(true);
    _translateBtn->setFixedSize(24, 22);
    connect(_translateBtn, SIGNAL(clicked()), this, SLOT(onGizmoTranslate()));
    toolbarLayout->addWidget(_translateBtn);

    _rotateBtn = new QToolButton(toolbar);
    _rotateBtn->setText(QString::fromUtf8("E"));
    _rotateBtn->setToolTip(QString::fromUtf8("Rotate (E)"));
    _rotateBtn->setCheckable(true);
    _rotateBtn->setFixedSize(24, 22);
    connect(_rotateBtn, SIGNAL(clicked()), this, SLOT(onGizmoRotate()));
    toolbarLayout->addWidget(_rotateBtn);

    _scaleBtn = new QToolButton(toolbar);
    _scaleBtn->setText(QString::fromUtf8("R"));
    _scaleBtn->setToolTip(QString::fromUtf8("Scale (R)"));
    _scaleBtn->setCheckable(true);
    _scaleBtn->setFixedSize(24, 22);
    connect(_scaleBtn, SIGNAL(clicked()), this, SLOT(onGizmoScale()));
    toolbarLayout->addWidget(_scaleBtn);

    // Separator
    QFrame* sep1 = new QFrame(toolbar);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sep1);

    // Grid toggle
    _gridBtn = new QToolButton(toolbar);
    _gridBtn->setText(QString::fromUtf8("Grid"));
    _gridBtn->setToolTip(QString::fromUtf8("Toggle Grid"));
    _gridBtn->setCheckable(true);
    _gridBtn->setChecked(true);
    _gridBtn->setFixedHeight(22);
    connect(_gridBtn, SIGNAL(clicked()), this, SLOT(onToggleGrid()));
    toolbarLayout->addWidget(_gridBtn);

    // World/Local toggle
    _spaceBtn = new QToolButton(toolbar);
    _spaceBtn->setText(QString::fromUtf8("W"));
    _spaceBtn->setToolTip(QString::fromUtf8("Transform Space: World (W) / Local (L)"));
    _spaceBtn->setFixedHeight(22);
    _spaceBtn->setFixedWidth(28);
    connect(_spaceBtn, SIGNAL(clicked()), this, SLOT(onToggleTransformSpace()));
    toolbarLayout->addWidget(_spaceBtn);

    // Reset camera
    QToolButton* resetBtn = new QToolButton(toolbar);
    resetBtn->setText(QString::fromUtf8("Reset"));
    resetBtn->setToolTip(QString::fromUtf8("Reset Camera (F)"));
    resetBtn->setFixedHeight(22);
    connect(resetBtn, SIGNAL(clicked()), this, SLOT(onResetCamera()));
    toolbarLayout->addWidget(resetBtn);

    // Force New Render + Pause Updates — same icons + size as the 2D viewer.
    const QSize v3dBtnSize( TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
    const QSize v3dBtnIconSize( TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE) );
    QPixmap pixRefresh, pixPauseOn, pixPauseOff;
    appPTR->getIcon(NATRON_PIXMAP_VIEWER_REFRESH, &pixRefresh);
    appPTR->getIcon(NATRON_PIXMAP_PLAYER_PAUSE_ENABLED, &pixPauseOn);
    appPTR->getIcon(NATRON_PIXMAP_PLAYER_PAUSE_DISABLED, &pixPauseOff);

    _refreshBtn = new QToolButton(toolbar);
    _refreshBtn->setFocusPolicy(Qt::NoFocus);
    _refreshBtn->setFixedSize(v3dBtnSize);
    _refreshBtn->setIconSize(v3dBtnIconSize);
    _refreshBtn->setIcon( QIcon(pixRefresh) );
    _refreshBtn->setToolTip(QString::fromUtf8("Force New Render: re-pull the scene and redraw once (even while paused)."));
    connect(_refreshBtn, SIGNAL(clicked()), this, SLOT(onForceRefresh()));
    toolbarLayout->addWidget(_refreshBtn);

    _pauseBtn = new QToolButton(toolbar);
    _pauseBtn->setFocusPolicy(Qt::NoFocus);
    _pauseBtn->setFixedSize(v3dBtnSize);
    _pauseBtn->setIconSize(v3dBtnIconSize);
    _pauseBtn->setCheckable(true);
    _pauseBtn->setChecked(false);
    {
        QIcon icPause;
        icPause.addPixmap(pixPauseOff, QIcon::Normal, QIcon::Off);
        icPause.addPixmap(pixPauseOn, QIcon::Normal, QIcon::On);
        _pauseBtn->setIcon(icPause);
    }
    _pauseBtn->setToolTip(QString::fromUtf8("Pause Updates: freeze the 3D viewport (stop auto-refresh). Camera navigation still works."));
    connect(_pauseBtn, SIGNAL(toggled(bool)), this, SLOT(onTogglePause(bool)));
    toolbarLayout->addWidget(_pauseBtn);

    // Separator
    QFrame* sep2 = new QFrame(toolbar);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sep2);

    // Info label
    QLabel* infoLabel = new QLabel(QString::fromUtf8("3D Viewport"), toolbar);
    infoLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
    toolbarLayout->addWidget(infoLabel);

    toolbarLayout->addStretch();

    // Camera dropdown — "Look Through" any CameraProvider node in the scene.
    // Menu rebuilds on aboutToShow so it always reflects current node graph.
    {
        _cameraDropdown = new QToolButton(toolbar);
        _cameraDropdown->setText(QString::fromUtf8("View: Default"));
        _cameraDropdown->setToolTip(QString::fromUtf8("Look through a Camera3D / ReadAlembicCamera node"));
        _cameraDropdown->setFixedHeight(22);
        _cameraDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* camMenu = new QMenu(_cameraDropdown);
        _cameraDropdown->setMenu(camMenu);
        connect(camMenu, SIGNAL(aboutToShow()), this, SLOT(onCameraMenuAboutToShow()));
        toolbarLayout->addWidget(_cameraDropdown);
    }

    // Shading dropdown — Wireframe / Shaded / Shaded+Wire (Maya-style default).
    {
        _shadingDropdown = new QToolButton(toolbar);
        _shadingDropdown->setText(QString::fromUtf8("Shading: Shaded+Wire"));
        _shadingDropdown->setToolTip(QString::fromUtf8("Viewport shading style"));
        _shadingDropdown->setFixedHeight(22);
        _shadingDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* shMenu = new QMenu(_shadingDropdown);
        const char* labels[4]  = { "Wireframe", "Flat", "Shaded", "Shaded+Wire" };
        const int   modes[4]   = { (int)Viewport3D::eWireframe,
                                   (int)Viewport3D::eFlat,
                                   (int)Viewport3D::eShaded,
                                   (int)Viewport3D::eShadedWire };
        for (int i = 0; i < 4; ++i) {
            QAction* a = shMenu->addAction(QString::fromUtf8(labels[i]));
            a->setData(modes[i]);
            connect(a, SIGNAL(triggered()), this, SLOT(onShadingModeSelected()));
        }
        _shadingDropdown->setMenu(shMenu);
        toolbarLayout->addWidget(_shadingDropdown);
    }

#ifdef NATRON_CYCLES
    // Cycles render button
    QToolButton* renderBtn = new QToolButton(toolbar);
    renderBtn->setText(QString::fromUtf8("Render"));
    renderBtn->setToolTip(QString::fromUtf8("Render scene with Cycles (saves PNG)"));
    renderBtn->setFixedHeight(22);
    renderBtn->setStyleSheet(QString::fromUtf8("QToolButton { background-color: #4a6e2e; color: white; padding: 0 8px; }"));
    connect(renderBtn, SIGNAL(clicked()), this, SLOT(onCyclesRender()));
    toolbarLayout->addWidget(renderBtn);
#endif

    mainLayout->addWidget(toolbar);

    // ==================== Second toolbar row (viewport actions) ============
    // A spare row for object/viewport actions; add future buttons here.
    {
        QWidget* toolbar2 = new QWidget(this);
        toolbar2->setFixedHeight(26);
        // Same as the top toolbar: don't let its content dictate the tab's minimum
        // width (otherwise the splitter pane snaps/collapses when dragged narrow).
        {
            QSizePolicy sp = toolbar2->sizePolicy();
            sp.setHorizontalPolicy(QSizePolicy::Ignored);
            toolbar2->setSizePolicy(sp);
        }
        QHBoxLayout* toolbar2Layout = new QHBoxLayout(toolbar2);
        toolbar2Layout->setContentsMargins(4, 2, 4, 2);
        toolbar2Layout->setSpacing(4);

        QToolButton* isolateBtn = new QToolButton(toolbar2);
        isolateBtn->setText(QString::fromUtf8("Isolate Selected"));
        isolateBtn->setToolTip(QString::fromUtf8("Show only the selected object (and its children) in the viewport. "
                                                 "Toggle off to show everything again."));
        isolateBtn->setCheckable(true);
        isolateBtn->setFixedHeight(22);
        // Lambda connect (no new slot → no moc change needed). _viewport is set
        // just below and is valid by the time the user can click.
        connect(isolateBtn, &QToolButton::toggled, this, [this](bool on) {
            if (_viewport) {
                _viewport->setIsolateSelected(on);
            }
        });
        toolbar2Layout->addWidget(isolateBtn);
        toolbar2Layout->addStretch();
        mainLayout->addWidget(toolbar2);
    }

    // ==================== 3D Viewport ====================
    _viewport = new Viewport3D(gui);
    mainLayout->addWidget(_viewport, 1); // stretch factor 1 = takes all remaining space

    // ==================== Timeline Scrubber ====================
    {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            TimeLinePtr timeline = app->getTimeLine();
            if (timeline) {
                // Create TimeLineGui with NULL viewer/viewerTab (3D viewport mode)
                _timelineGui = new TimeLineGui(NULL, timeline, gui, NULL);

                // Match project frame range
                double firstFrame = 1, lastFrame = 250;
                app->getFrameRange(&firstFrame, &lastFrame);
                _timelineGui->setBoundaries((SequenceTime)firstFrame, (SequenceTime)lastFrame);

                // Seek to current frame so playhead appears
                _timelineGui->seek(timeline->currentFrame());

                mainLayout->addWidget(_timelineGui);
            }
        }
    }


    // Connect to timeline for frame updates (sync frame spinbox + 3D viewport)
    {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            TimeLinePtr timeline = app->getTimeLine();
            if (timeline) {
                connect(timeline.get(), SIGNAL(frameChanged(SequenceTime,int)),
                        this, SLOT(onFrameChanged(SequenceTime,int)));
            }
        }
    }

    setFocusPolicy(Qt::ClickFocus);
}

Viewport3DTab::~Viewport3DTab()
{
}

// ==================== Slots ====================

void
Viewport3DTab::onGizmoTranslate()
{
    _translateBtn->setChecked(true);
    _rotateBtn->setChecked(false);
    _scaleBtn->setChecked(false);
    // Forward to viewport via key event
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    _viewport->keyPressEvent(&ev);
}

void
Viewport3DTab::onGizmoRotate()
{
    _translateBtn->setChecked(false);
    _rotateBtn->setChecked(true);
    _scaleBtn->setChecked(false);
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier);
    _viewport->keyPressEvent(&ev);
}

void
Viewport3DTab::onGizmoScale()
{
    _translateBtn->setChecked(false);
    _rotateBtn->setChecked(false);
    _scaleBtn->setChecked(true);
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    _viewport->keyPressEvent(&ev);
}

void
Viewport3DTab::onResetCamera()
{
    _viewport->resetCamera();
}

void
Viewport3DTab::onToggleGrid()
{
    _gridVisible = _gridBtn->isChecked();
    _viewport->setShowGrid(_gridVisible);
}

void
Viewport3DTab::onCameraMenuAboutToShow()
{
    QMenu* menu = _cameraDropdown->menu();
    if (!menu) return;
    menu->clear();

    NodePtr current = _viewport->getLookThroughCamera();

    // "Default" — free-orbit (no look-through camera).
    QAction* defaultAct = menu->addAction(QString::fromUtf8("Default (Free Orbit)"));
    defaultAct->setCheckable(true);
    defaultAct->setChecked(!current);
    connect(defaultAct, SIGNAL(triggered()), this, SLOT(onCameraSelected()));
    defaultAct->setProperty("cameraNodeName", QString());

    menu->addSeparator();

    // Enumerate every CameraProvider node in the project right now.
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    if (app) {
        ProjectPtr proj = app->getProject();
        if (proj) {
            NodesList allNodes;
            proj->getNodes_recursive(allNodes, true);
            int found = 0;
            for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
                if (!(*it)->isActivated()) continue;
                EffectInstancePtr eff = (*it)->getEffectInstance();
                if (!eff) continue;
                if (!dynamic_cast<CameraProvider*>(eff.get())) continue;
                const std::string scriptName = (*it)->getScriptName();
                QAction* a = menu->addAction(QString::fromUtf8(scriptName.c_str()));
                a->setCheckable(true);
                a->setChecked(current && current->getScriptName() == scriptName);
                a->setProperty("cameraNodeName", QString::fromUtf8(scriptName.c_str()));
                connect(a, SIGNAL(triggered()), this, SLOT(onCameraSelected()));
                ++found;
            }
            if (found == 0) {
                QAction* none = menu->addAction(QString::fromUtf8("(no cameras in scene)"));
                none->setEnabled(false);
            }
        }
    }
}

void
Viewport3DTab::onCameraSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a) return;
    const QString name = a->property("cameraNodeName").toString();
    if (name.isEmpty()) {
        // Default — free orbit
        _viewport->setLookThroughCamera(NodePtr());
        _cameraDropdown->setText(QString::fromUtf8("View: Default"));
        return;
    }

    // Find the camera node by script name
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    if (!app) return;
    ProjectPtr proj = app->getProject();
    if (!proj) return;
    NodesList allNodes;
    proj->getNodes_recursive(allNodes, true);
    const std::string nameStd = name.toStdString();
    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
        if (!(*it)->isActivated()) continue;
        if ((*it)->getScriptName() == nameStd) {
            _viewport->setLookThroughCamera(*it);
            _cameraDropdown->setText(QString::fromUtf8("View: ") + name);
            return;
        }
    }
}

void
Viewport3DTab::onShadingModeSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a) return;
    const int modeInt = a->data().toInt();
    const Viewport3D::ShadingMode mode = (Viewport3D::ShadingMode)modeInt;
    _viewport->setShadingMode(mode);
    QString label;
    switch (mode) {
        case Viewport3D::eWireframe:   label = QString::fromUtf8("Wireframe"); break;
        case Viewport3D::eFlat:        label = QString::fromUtf8("Flat"); break;
        case Viewport3D::eShaded:      label = QString::fromUtf8("Shaded"); break;
        case Viewport3D::eShadedWire:  label = QString::fromUtf8("Shaded+Wire"); break;
    }
    _shadingDropdown->setText(QString::fromUtf8("Shading: ") + label);
}

void
Viewport3DTab::onToggleTransformSpace()
{
    _viewport->toggleTransformSpace();
    _spaceBtn->setText(_viewport->isLocalSpace()
        ? QString::fromUtf8("L")
        : QString::fromUtf8("W"));
}

void
Viewport3DTab::onTogglePause(bool paused)
{
    if (_viewport) {
        _viewport->setPaused(paused);
    }
}

void
Viewport3DTab::onForceRefresh()
{
    if (_viewport) {
        _viewport->forceRefresh();
    }
}

void
Viewport3DTab::onCyclesRender()
{
#ifdef NATRON_CYCLES
    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    ProjectPtr project = app->getProject();
    if (!project) return;

    // Build scene graph from current nodes
    double time = app->getTimeLine()->currentFrame();
    NodesList nodes;
    project->getNodes_recursive(nodes, true);

    SceneGraph sceneGraph;
    sceneGraph.rebuild(nodes, time);

    if (sceneGraph.size() == 0) {
        QMessageBox::information(this, QString::fromUtf8("Cycles Render"),
            QString::fromUtf8("No 3D nodes in the scene. Add a Sphere3D, Cube3D, or Card3D first."));
        return;
    }

    // Ask where to save
    QString savePath = QFileDialog::getSaveFileName(this,
        QString::fromUtf8("Save Cycles Render"),
        QString::fromUtf8("D:/cycles_natron_render.png"),
        QString::fromUtf8("PNG (*.png);;EXR (*.exr)"));
    if (savePath.isEmpty()) return;

    // TODO: Viewport3D uses demo-style camera, not Camera3D.
    // Viewport Render button needs updating to use the new camera matrices.
    // For now, CyclesRender node is the primary render path.
    bool ok = false;
    (void)savePath; // suppress unused warning

    if (ok) {
        QMessageBox::information(this, QString::fromUtf8("Cycles Render"),
            QString::fromUtf8("Render complete! %1 nodes rendered.\nSaved to: %2")
            .arg(sceneGraph.size()).arg(savePath));
    } else {
        QMessageBox::warning(this, QString::fromUtf8("Cycles Render"),
            QString::fromUtf8("Render failed."));
    }
#endif
}

void
Viewport3DTab::onFrameChanged(SequenceTime frame, int /*reason*/)
{
    // The frame is shown by the timeline scrubber (_timelineGui); _frameSpin is
    // currently never created, so guard it (dereferencing it crashed when the
    // timeline frameChanged signal fired during project load). Just refresh the
    // 3D viewport at the new frame.
    if (_frameSpin) {
        _frameSpin->blockSignals(true);
        _frameSpin->setValue((int)frame);
        _frameSpin->blockSignals(false);
    }
    if (_viewport && !_viewport->isPaused()) {
        _viewport->update();
    }
}

void
Viewport3DTab::onPlayForward()
{
    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    TimeLinePtr tl = app->getTimeLine();
    if (!tl) return;

    int cur = (int)tl->currentFrame();
    int end = _rangeEnd->value();
    if (cur < end) {
        tl->seekFrame(cur + 1, true, NULL, eTimelineChangeReasonPlaybackSeek);
    }
}

void
Viewport3DTab::onPlayBackward()
{
    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    TimeLinePtr tl = app->getTimeLine();
    if (!tl) return;

    int cur = (int)tl->currentFrame();
    int start = _rangeStart->value();
    if (cur > start) {
        tl->seekFrame(cur - 1, true, NULL, eTimelineChangeReasonPlaybackSeek);
    }
}

void
Viewport3DTab::onPrevFrame()
{
    onPlayBackward();
}

void
Viewport3DTab::onNextFrame()
{
    onPlayForward();
}

void
Viewport3DTab::onFirstFrame()
{
    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    TimeLinePtr tl = app->getTimeLine();
    if (!tl) return;

    tl->seekFrame(_rangeStart->value(), true, NULL, eTimelineChangeReasonPlaybackSeek);
}

void
Viewport3DTab::onLastFrame()
{
    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    TimeLinePtr tl = app->getTimeLine();
    if (!tl) return;

    tl->seekFrame(_rangeEnd->value(), true, NULL, eTimelineChangeReasonPlaybackSeek);
}

void
Viewport3DTab::onFrameSpinChanged(int value)
{
    Gui* gui = getGui();
    if (!gui) return;
    GuiAppInstancePtr app = gui->getApp();
    if (!app) return;
    TimeLinePtr tl = app->getTimeLine();
    if (!tl) return;

    tl->seekFrame(value, true, NULL, eTimelineChangeReasonPlaybackSeek);
}

// ==================== Event forwarding ====================

void
Viewport3DTab::enterEvent(QtCompat::QEnterEvent* e)
{
    QWidget::enterEvent(e);
    takeClickFocus();
}

void
Viewport3DTab::leaveEvent(QEvent* e)
{
    QWidget::leaveEvent(e);
    removeClickFocus();
}

void
Viewport3DTab::keyPressEvent(QKeyEvent* e)
{
    // Forward key events to the viewport
    _viewport->keyPressEvent(e);
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_Viewport3DTab.cpp"
