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

#include "ViewportVRTab.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QFrame>
#include <QMenu>
#include <QAction>

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewportVR.h"
#include "Engine/AppManager.h"
#include "Engine/TimeLine.h"
#include "Engine/Project.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

ViewportVRTab::ViewportVRTab(Gui* gui, QWidget* parent)
    : QWidget(parent)
    , PanelWidget(this, gui)
    , _viewport(NULL)
    , _timelineGui(NULL)
    , _attachDropdown(NULL)
    , _layoutDropdown(NULL)
    , _eyeDropdown(NULL)
    , _refreshBtn(NULL)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ==================== Top Toolbar ====================
    QWidget* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    // Same as the 3D viewport: don't let the toolbar's minimum width become the
    // tab's minimum width (a splitter pane holding the tab would otherwise
    // collapse when dragged narrow); clip instead.
    {
        QSizePolicy sp = toolbar->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Ignored);
        toolbar->setSizePolicy(sp);
    }
    QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 2, 4, 2);
    toolbarLayout->setSpacing(4);

    // ---- Attach node dropdown (rebuilt on aboutToShow) ----
    {
        _attachDropdown = new QToolButton(toolbar);
        _attachDropdown->setText(QString::fromUtf8("Attach: None"));
        _attachDropdown->setToolTip(QString::fromUtf8(
            "Pick the node whose image this VR viewport shows — typically a Read "
            "connected to the equirectangular footage, or any processed node."));
        _attachDropdown->setFixedHeight(22);
        _attachDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_attachDropdown);
        _attachDropdown->setMenu(menu);
        connect(menu, SIGNAL(aboutToShow()), this, SLOT(onAttachMenuAboutToShow()));
        toolbarLayout->addWidget(_attachDropdown);
    }

    // ---- Stereo layout dropdown ----
    {
        _layoutDropdown = new QToolButton(toolbar);
        _layoutDropdown->setText(QString::fromUtf8("Layout: Mono"));
        _layoutDropdown->setToolTip(QString::fromUtf8(
            "How the 2:1 source packs the eyes: Mono (whole frame), Over-Under "
            "(left eye = top half), Side-by-Side (left eye = left half)."));
        _layoutDropdown->setFixedHeight(22);
        _layoutDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_layoutDropdown);
        const char* labels[3] = { "Mono", "Over-Under", "Side-by-Side" };
        const int modes[3]    = { (int)ViewportVR::eLayoutMono,
                                  (int)ViewportVR::eLayoutOverUnder,
                                  (int)ViewportVR::eLayoutSideBySide };
        for (int i = 0; i < 3; ++i) {
            QAction* a = menu->addAction(QString::fromUtf8(labels[i]));
            a->setData(modes[i]);
            connect(a, SIGNAL(triggered()), this, SLOT(onLayoutSelected()));
        }
        _layoutDropdown->setMenu(menu);
        toolbarLayout->addWidget(_layoutDropdown);
    }

    // ---- Eye dropdown ----
    {
        _eyeDropdown = new QToolButton(toolbar);
        _eyeDropdown->setText(QString::fromUtf8("Eye: Left"));
        _eyeDropdown->setToolTip(QString::fromUtf8("Which eye of a stereo-packed source to show (Mono layout ignores this)."));
        _eyeDropdown->setFixedHeight(22);
        _eyeDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_eyeDropdown);
        const char* eyeLabels[2] = { "Left", "Right" };
        for (int i = 0; i < 2; ++i) {
            QAction* a = menu->addAction(QString::fromUtf8(eyeLabels[i]));
            a->setData(i);
            connect(a, SIGNAL(triggered()), this, SLOT(onEyeSelected()));
        }
        _eyeDropdown->setMenu(menu);
        toolbarLayout->addWidget(_eyeDropdown);
    }

    // ---- Separator + reset + refresh ----
    QFrame* sep1 = new QFrame(toolbar);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sep1);

    QToolButton* resetBtn = new QToolButton(toolbar);
    resetBtn->setText(QString::fromUtf8("Reset"));
    resetBtn->setToolTip(QString::fromUtf8("Reset view (look at the pano center, FOV back to default)"));
    resetBtn->setFixedHeight(22);
    connect(resetBtn, SIGNAL(clicked()), this, SLOT(onResetView()));
    toolbarLayout->addWidget(resetBtn);

    const QSize btnSize( TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
    const QSize btnIconSize( TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE) );
    QPixmap pixRefresh;
    appPTR->getIcon(NATRON_PIXMAP_VIEWER_REFRESH, &pixRefresh);

    _refreshBtn = new QToolButton(toolbar);
    _refreshBtn->setFocusPolicy(Qt::NoFocus);
    _refreshBtn->setFixedSize(btnSize);
    _refreshBtn->setIconSize(btnIconSize);
    _refreshBtn->setIcon( QIcon(pixRefresh) );
    _refreshBtn->setToolTip(QString::fromUtf8("Force New Render: re-pull the attached node at the current frame."));
    connect(_refreshBtn, SIGNAL(clicked()), this, SLOT(onForceRefresh()));
    toolbarLayout->addWidget(_refreshBtn);

    // ---- Info label ----
    QLabel* infoLabel = new QLabel(QString::fromUtf8("VR Viewport — drag to look around, wheel to zoom"), toolbar);
    infoLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
    toolbarLayout->addWidget(infoLabel);

    toolbarLayout->addStretch();
    mainLayout->addWidget(toolbar);

    // ==================== VR widget ====================
    _viewport = new ViewportVR(gui);
    mainLayout->addWidget(_viewport, 1); // stretch factor 1 = takes all remaining space

    // ==================== Timeline Scrubber ====================
    {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            TimeLinePtr timeline = app->getTimeLine();
            if (timeline) {
                // Create TimeLineGui with NULL viewer/viewerTab (standalone pane mode)
                _timelineGui = new TimeLineGui(NULL, timeline, gui, NULL);
                double firstFrame = 1, lastFrame = 250;
                app->getFrameRange(&firstFrame, &lastFrame);
                _timelineGui->setBoundaries((SequenceTime)firstFrame, (SequenceTime)lastFrame);
                _timelineGui->seek(timeline->currentFrame());
                mainLayout->addWidget(_timelineGui);
            }
        }
    }

    // Follow the project timeline: a frame change re-fetches the attached node.
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

ViewportVRTab::~ViewportVRTab()
{
}

// ==================== Slots ====================

void
ViewportVRTab::onAttachMenuAboutToShow()
{
    QMenu* menu = _attachDropdown->menu();
    if (!menu) {
        return;
    }
    menu->clear();

    QAction* noneAct = menu->addAction(QString::fromUtf8("(none)"));
    noneAct->setCheckable(true);
    noneAct->setChecked(true);
    noneAct->setProperty("attachNodeName", QString());
    connect(noneAct, SIGNAL(triggered()), this, SLOT(onAttachSelected()));

    menu->addSeparator();

    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    if (app) {
        ProjectPtr proj = app->getProject();
        if (proj) {
            // Every activated node in the project; the attached one ticks.
            NodesList allNodes;
            proj->getNodes_recursive(allNodes, true);
            int found = 0;
            for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
                if (!(*it)->isActivated()) continue;
                const std::string scriptName = (*it)->getScriptName();
                QAction* a = menu->addAction(QString::fromUtf8(scriptName.c_str()));
                a->setCheckable(true);
                a->setChecked( _viewport && _viewport->getAttachedNodeName() == QString::fromUtf8(scriptName.c_str()) );
                a->setProperty("attachNodeName", QString::fromUtf8(scriptName.c_str()));
                connect(a, SIGNAL(triggered()), this, SLOT(onAttachSelected()));
                ++found;
            }
            if (found == 0) {
                QAction* empty = menu->addAction(QString::fromUtf8("(no nodes in the project)"));
                empty->setEnabled(false);
            }
        }
    }
}

void
ViewportVRTab::onAttachSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a || !_viewport) {
        return;
    }
    const QString name = a->property("attachNodeName").toString();
    if (name.isEmpty()) {
        _viewport->setNodeScriptName(QString());
        _attachDropdown->setText(QString::fromUtf8("Attach: None"));
        return;
    }
    _viewport->setNodeScriptName(name);
    _attachDropdown->setText(QString::fromUtf8("Attach: ") + name);
}

void
ViewportVRTab::onLayoutSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a || !_viewport) {
        return;
    }
    const ViewportVR::Layout l = (ViewportVR::Layout)a->data().toInt();
    _viewport->setLayoutMode(l);
    QString label;
    switch (l) {
    case ViewportVR::eLayoutMono:      label = QString::fromUtf8("Mono"); break;
    case ViewportVR::eLayoutOverUnder: label = QString::fromUtf8("Over-Under"); break;
    case ViewportVR::eLayoutSideBySide: label = QString::fromUtf8("Side-by-Side"); break;
    }
    _layoutDropdown->setText(QString::fromUtf8("Layout: ") + label);
}

void
ViewportVRTab::onEyeSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a || !_viewport) {
        return;
    }
    const int eye = a->data().toInt();
    _viewport->setEye(eye);
    _eyeDropdown->setText(QString::fromUtf8("Eye: ") + QString::fromUtf8(eye == 0 ? "Left" : "Right"));
}

void
ViewportVRTab::onResetView()
{
    if (_viewport) {
        _viewport->resetLook();
    }
}

void
ViewportVRTab::onForceRefresh()
{
    if (_viewport) {
        _viewport->forceRefresh();
    }
}

void
ViewportVRTab::onFrameChanged(SequenceTime frame, int /*reason*/)
{
    Q_UNUSED(frame);
    // The widget re-fetches on the timeline frame; nothing else to do besides
    // letting its next paint pick the frame up.
    if (_viewport) {
        _viewport->update();
    }
}

NATRON_NAMESPACE_EXIT
