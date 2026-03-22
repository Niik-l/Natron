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

#include "Gui/Button.h"
#include "Gui/GuiDefines.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/SpinBox.h"
#include "Gui/TimeLineGui.h"
#include "Gui/Viewport3D.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

Viewport3DTab::Viewport3DTab(Gui* gui, QWidget* parent)
    : QWidget(parent)
    , PanelWidget(this, gui)
    , _viewport(NULL)
    , _timelineGui(NULL)
    , _frameLabel(NULL)
    , _translateBtn(NULL)
    , _rotateBtn(NULL)
    , _scaleBtn(NULL)
    , _gridBtn(NULL)
    , _gridVisible(true)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ==================== Top Toolbar ====================
    QWidget* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
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

    // Reset camera
    QToolButton* resetBtn = new QToolButton(toolbar);
    resetBtn->setText(QString::fromUtf8("Reset"));
    resetBtn->setToolTip(QString::fromUtf8("Reset Camera (F)"));
    resetBtn->setFixedHeight(22);
    connect(resetBtn, SIGNAL(clicked()), this, SLOT(onResetCamera()));
    toolbarLayout->addWidget(resetBtn);

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

    mainLayout->addWidget(toolbar);

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
                        this, SLOT(onFrameChanged(double)));
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
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier);
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
    _viewport->update();
}

void
Viewport3DTab::onFrameChanged(double frame)
{
    _frameSpin->blockSignals(true);
    _frameSpin->setValue((int)frame);
    _frameSpin->blockSignals(false);
    _viewport->update();
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
