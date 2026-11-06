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
#include <QKeyEvent>
#include <QTimer>
#include <QDoubleSpinBox>

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
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

ViewportVRTab::ViewportVRTab(Gui* gui, QWidget* parent)
    : QWidget(parent)
    , PanelWidget(this, gui)
    , _viewport(NULL)
    , _timelineGui(NULL)
    , _attachDropdown(NULL)
    , _layoutDropdown(NULL)
    , _eyeDropdown(NULL)
    , _channelsDropdown(NULL)
    , _refreshBtn(NULL)
    , _playBtn(NULL)
    , _gammaSpin(NULL)
    , _playTimer(NULL)
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

    // ---- Separator + gamma ----
    QFrame* sepGamma = new QFrame(toolbar);
    sepGamma->setFrameShape(QFrame::VLine);
    sepGamma->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sepGamma);

    {
        QLabel* gammaLabel = new QLabel(QString::fromUtf8("Gamma"), toolbar);
        gammaLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(gammaLabel);

        _gammaSpin = new QDoubleSpinBox(toolbar);
        _gammaSpin->setRange(0.05, 10.0);
        _gammaSpin->setSingleStep(0.05);
        _gammaSpin->setDecimals(2);
        _gammaSpin->setValue(1.0);
        _gammaSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _gammaSpin->setFixedWidth(58);
        _gammaSpin->setFixedHeight(22);
        _gammaSpin->setToolTip(QString::fromUtf8(
            "Display gamma of the VR viewport (color^(1/gamma)). Matches the 2D "
            "viewer's Gamma slider; 1.0 = identity."));
        connect(_gammaSpin, SIGNAL(valueChanged(double)), this, SLOT(onGammaChanged(double)));
        toolbarLayout->addWidget(_gammaSpin);
    }

    // ---- Channels dropdown ----
    {
        _channelsDropdown = new QToolButton(toolbar);
        _channelsDropdown->setText(QString::fromUtf8("Ch: RGB"));
        _channelsDropdown->setToolTip(QString::fromUtf8("Channel display of the VR viewport (applied on the GPU, no re-render)."));
        _channelsDropdown->setFixedHeight(22);
        _channelsDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_channelsDropdown);
        const char* chanLabels[5] = { "RGB", "Red", "Green", "Blue", "Luminance" };
        const int modes[5] = { (int)ViewportVR::eChannelsRGB,
                               (int)ViewportVR::eChannelsRed,
                               (int)ViewportVR::eChannelsGreen,
                               (int)ViewportVR::eChannelsBlue,
                               (int)ViewportVR::eChannelsLuminance };
        for (int i = 0; i < 5; ++i) {
            QAction* a = menu->addAction(QString::fromUtf8(chanLabels[i]));
            a->setData(modes[i]);
            connect(a, SIGNAL(triggered()), this, SLOT(onChannelsSelected()));
        }
        _channelsDropdown->setMenu(menu);
        toolbarLayout->addWidget(_channelsDropdown);
    }

    // ---- Separator + play/stop + reset + refresh ----
    QFrame* sep1 = new QFrame(toolbar);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sep1);

    const QSize btnSize( TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
    const QSize btnIconSize( TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE) );

    // Play button: same icon trick as the 2D viewer's play button — when the
    // button is down it shows the stop square, up it shows the play triangle.
    {
        QPixmap pixPlayDisabled, pixStop;
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PLAY_DISABLED, &pixPlayDisabled);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_STOP_ENABLED, &pixStop);

        _playBtn = new QToolButton(toolbar);
        _playBtn->setFocusPolicy(Qt::NoFocus);
        _playBtn->setFixedSize(btnSize);
        _playBtn->setIconSize(btnIconSize);
        _playBtn->setCheckable(true);
        _playBtn->setDown(false);
        QIcon icPlay;
        icPlay.addPixmap(pixStop, QIcon::Normal, QIcon::On);
        icPlay.addPixmap(pixPlayDisabled, QIcon::Normal, QIcon::Off);
        _playBtn->setIcon(icPlay);
        _playBtn->setToolTip(QString::fromUtf8(
            "Play / stop the project timeline in this VR viewport (Space). The "
            "timeline is shared, so the 2D viewers follow like with a scrub."));
        connect(_playBtn, SIGNAL(toggled(bool)), this, SLOT(onPlayToggled(bool)));
        toolbarLayout->addWidget(_playBtn);

        _playTimer = new QTimer(this);
        connect(_playTimer, SIGNAL(timeout()), this, SLOT(onPlayTick()));
    }

    QToolButton* resetBtn = new QToolButton(toolbar);
    resetBtn->setText(QString::fromUtf8("Reset"));
    resetBtn->setToolTip(QString::fromUtf8("Reset view (look at the pano center, FOV back to default)"));
    resetBtn->setFixedHeight(22);
    connect(resetBtn, SIGNAL(clicked()), this, SLOT(onResetView()));
    toolbarLayout->addWidget(resetBtn);

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
ViewportVRTab::onGammaChanged(double gamma)
{
    if (_viewport) {
        _viewport->setGammaDebug(gamma);
    }
}

void
ViewportVRTab::onChannelsSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a || !_viewport) {
        return;
    }
    const ViewportVR::ChannelsMode m = (ViewportVR::ChannelsMode)a->data().toInt();
    _viewport->setChannelsMode(m);
    QString label;
    switch (m) {
    case ViewportVR::eChannelsRGB:       label = QString::fromUtf8("RGB"); break;
    case ViewportVR::eChannelsRed:       label = QString::fromUtf8("Red"); break;
    case ViewportVR::eChannelsGreen:     label = QString::fromUtf8("Green"); break;
    case ViewportVR::eChannelsBlue:      label = QString::fromUtf8("Blue"); break;
    case ViewportVR::eChannelsLuminance: label = QString::fromUtf8("Luminance"); break;
    }
    _channelsDropdown->setText(QString::fromUtf8("Ch: ") + label);
}

void
ViewportVRTab::onPlayToggled(bool playing)
{
    if (!_playTimer || !getGui()) {
        return;
    }
    if (playing) {
        // Interval from the project's frame rate knob.
        double fps = 24;
        GuiAppInstancePtr app = getGui()->getApp();
        if (app) {
            ProjectPtr proj = app->getProject();
            if (proj) {
                KnobDoublePtr fpsKnob = std::dynamic_pointer_cast<KnobDouble>(
                    proj->getKnobByName(std::string("frameRate")));
                if (fpsKnob) {
                    fps = fpsKnob->getValue();
                }
            }
        }
        if (fps <= 1e-6) {
            fps = 24;
        }
        _playTimer->start( (int)std::lround(1000. / fps) );
    } else {
        _playTimer->stop();
    }
}

void
ViewportVRTab::onPlayTick()
{
    Gui* gui = getGui();
    if (!gui) {
        return;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return;
    }
    double firstFrame = 1, lastFrame = 250;
    app->getFrameRange(&firstFrame, &lastFrame);

    TimeLinePtr timeline = app->getTimeLine();
    if (!timeline) {
        return;
    }
    SequenceTime next = timeline->currentFrame() + 1;
    if (next > (SequenceTime)lastFrame) {
        next = (SequenceTime)firstFrame; // loop
    }
    timeline->seekFrame(next, true, NULL, eTimelineChangeReasonPlaybackSeek);
}

void
ViewportVRTab::keyPressEvent(QKeyEvent* e)
{
    // Space toggles play/stop, same as the 2D viewer's player.
    if ( (e->key() == Qt::Key_Space) && _playBtn ) {
        _playBtn->setChecked( !_playBtn->isChecked() );
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
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
