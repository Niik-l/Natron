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

#include <cmath>
#include <climits>

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
#include <QSpinBox>

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewportVR.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
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
    , _layoutDropdown(NULL)
    , _eyeDropdown(NULL)
    , _channelsDropdown(NULL)
    , _refreshBtn(NULL)
    , _playBtn(NULL)
    , _gammaSpin(NULL)
    , _playTimer(NULL)
    , _frameSpin(NULL)
    , _rangeIn(NULL)
    , _rangeOut(NULL)
    , _fpsLabel(NULL)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ==================== Top toolbar (viewer-style control bar) ============
    // Mirrors the 2D viewer's top bar rhythm: channel display + tone controls,
    // with the VR-specific packing settings as the trailing group ("room for
    // VR-specific settings" per the plan review). The pane is bound to a VR
    // Viewer node in the graph — not a selector here (node-tree logic).
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

    // ---- Channels dropdown (viewer "RGB" dropdown equivalent) ----
    {
        _channelsDropdown = new QToolButton(toolbar);
        _channelsDropdown->setText(QString::fromUtf8("RGB"));
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

    // ---- Gamma (the viewer's Gamma slider) ----
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
        _gammaSpin->setToolTip( QString::fromUtf8(
            "Display gamma of the VR viewport (color^(1/gamma)). Matches the 2D "
            "viewer's Gamma slider; 1.0 = identity."));
        connect(_gammaSpin, SIGNAL(valueChanged(double)), this, SLOT(onGammaChanged(double)));
        toolbarLayout->addWidget(_gammaSpin);
    }

    QFrame* sepVR = new QFrame(toolbar);
    sepVR->setFrameShape(QFrame::VLine);
    sepVR->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sepVR);

    // ---- VR-specific: stereo packing + eye ----
    {
        _layoutDropdown = new QToolButton(toolbar);
        _layoutDropdown->setText(QString::fromUtf8("Layout: Mono"));
        _layoutDropdown->setToolTip(QString::fromUtf8(
            "How the 2:1 source packes the eyes: Mono (whole frame), Over-Under "
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

    toolbarLayout->addStretch();

    // ---- Right edge: reset + refresh (like the viewer's refresh/pause group) ----
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
    _refreshBtn->setToolTip(QString::fromUtf8("Force New Render: re-pull the bound VR Viewer node at the current frame."));
    connect(_refreshBtn, SIGNAL(clicked()), this, SLOT(onForceRefresh()));
    toolbarLayout->addWidget(_refreshBtn);

    mainLayout->addWidget(toolbar);

    // ==================== VR widget ====================
    _viewport = new ViewportVR(gui);
    mainLayout->addWidget(_viewport, 1); // stretch factor 1 = takes all remaining space

    // ==================== Player bar (viewer-style transport row) ===========
    {
        QWidget* playerBar = new QWidget(this);
        playerBar->setFixedHeight(28);
        {
            QSizePolicy sp = playerBar->sizePolicy();
            sp.setHorizontalPolicy(QSizePolicy::Ignored);
            playerBar->setSizePolicy(sp);
        }
        QHBoxLayout* playerLayout = new QHBoxLayout(playerBar);
        playerLayout->setContentsMargins(4, 2, 4, 2);
        playerLayout->setSpacing(4);

        // fps readout (updated with the frame range)
        _fpsLabel = new QLabel(QString::fromUtf8("fps: 24.0"), playerBar);
        _fpsLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        playerLayout->addWidget(_fpsLabel);

        // transport buttons — same icons as the 2D viewer's player
        QPixmap pixFirst, pixBack1, pixForward1, pixLast, pixPlayDisabled, pixStop;
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_FIRST_FRAME, &pixFirst);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PREVIOUS, &pixBack1);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_NEXT, &pixForward1);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_LAST_FRAME, &pixLast);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PLAY_DISABLED, &pixPlayDisabled);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_STOP_ENABLED, &pixStop);

        QToolButton* firstBtn = new QToolButton(playerBar);
        firstBtn->setFocusPolicy(Qt::NoFocus);
        firstBtn->setFixedSize(btnSize);
        firstBtn->setIconSize(btnIconSize);
        firstBtn->setIcon( QIcon(pixFirst) );
        firstBtn->setToolTip(QString::fromUtf8("Go to first frame (Home)"));
        connect(firstBtn, SIGNAL(clicked()), this, SLOT(onSeekFirst()));
        playerLayout->addWidget(firstBtn);

        QToolButton* prevBtn = new QToolButton(playerBar);
        prevBtn->setFocusPolicy(Qt::NoFocus);
        prevBtn->setFixedSize(btnSize);
        prevBtn->setIconSize(btnIconSize);
        prevBtn->setIcon( QIcon(pixBack1) );
        prevBtn->setToolTip(QString::fromUtf8("Go to previous frame (left arrow)"));
        connect(prevBtn, SIGNAL(clicked()), this, SLOT(onSeekPrev()));
        playerLayout->addWidget(prevBtn);

        // Play button: same icon trick as the 2D viewer's play button — when the
        // button is down it shows the stop square, up it shows the play triangle.
        _playBtn = new QToolButton(playerBar);
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
            "Play / stop (Space). The timeline is shared, so the 2D viewers follow "
            "like with a scrub."));
        connect(_playBtn, SIGNAL(toggled(bool)), this, SLOT(onPlayToggled(bool)));
        playerLayout->addWidget(_playBtn);

        QToolButton* nextBtn = new QToolButton(playerBar);
        nextBtn->setFocusPolicy(Qt::NoFocus);
        nextBtn->setFixedSize(btnSize);
        nextBtn->setIconSize(btnIconSize);
        nextBtn->setIcon( QIcon(pixForward1) );
        nextBtn->setToolTip(QString::fromUtf8("Go to next frame (right arrow)"));
        connect(nextBtn, SIGNAL(clicked()), this, SLOT(onSeekNext()));
        playerLayout->addWidget(nextBtn);

        QToolButton* lastBtn = new QToolButton(playerBar);
        lastBtn->setFocusPolicy(Qt::NoFocus);
        lastBtn->setFixedSize(btnSize);
        lastBtn->setIconSize(btnIconSize);
        lastBtn->setIcon( QIcon(pixLast) );
        lastBtn->setToolTip(QString::fromUtf8("Go to last frame (End)"));
        connect(lastBtn, SIGNAL(clicked()), this, SLOT(onSeekLast()));
        playerLayout->addWidget(lastBtn);

        // Frame counter spin (the “0” in the middle of the viewer's player bar)
        _frameSpin = new QSpinBox(playerBar);
        _frameSpin->setRange(1, INT_MAX);
        _frameSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _frameSpin->setFixedWidth(64);
        _frameSpin->setFixedHeight(22);
        _frameSpin->setKeyboardTracking(false);
        _frameSpin->setToolTip(QString::fromUtf8("Current frame"));
        connect(_frameSpin, SIGNAL(valueChanged(int)), this, SLOT(onFrameSpinChanged(int)));
        playerLayout->addWidget(_frameSpin);

        // In / Out spins — the "10 | 250" range pair. Values follow the project
        // range; editing the range itself happens in the project settings (the
        // timeline widget shows the same range).
        playerLayout->addSpacing(8);
        _rangeIn = new QSpinBox(playerBar);
        _rangeIn->setRange(INT_MIN, INT_MAX);
        _rangeIn->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _rangeIn->setFixedWidth(56);
        _rangeIn->setFixedHeight(22);
        _rangeIn->setKeyboardTracking(false);
        _rangeIn->setToolTip(QString::fromUtf8("Playback first frame"));
        playerLayout->addWidget(_rangeIn);
        QLabel* rangeBar = new QLabel(QString::fromUtf8("|"), playerBar);
        rangeBar->setStyleSheet(QString::fromUtf8("color: rgb(120, 120, 120);"));
        playerLayout->addWidget(rangeBar);
        _rangeOut = new QSpinBox(playerBar);
        _rangeOut->setRange(INT_MIN + 1, INT_MAX);
        _rangeOut->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _rangeOut->setFixedWidth(56);
        _rangeOut->setFixedHeight(22);
        _rangeOut->setKeyboardTracking(false);
        _rangeOut->setToolTip(QString::fromUtf8("Playback last frame"));
        playerLayout->addWidget(_rangeOut);

        playerLayout->addStretch();
        mainLayout->addWidget(playerBar);
    }

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

    // Follow the project timeline: a frame change re-fetches the bound node and
    // keeps the transport spin in sync.
    {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            TimeLinePtr timeline = app->getTimeLine();
            if (timeline) {
                connect(timeline.get(), SIGNAL(frameChanged(SequenceTime,int)),
                        this, SLOT(onFrameChanged(SequenceTime,int)));
            }
            // Sync the transport spins + fps readout with the project state.
            double firstFrame = 1, lastFrame = 250;
            app->getFrameRange(&firstFrame, &lastFrame);
            if (_frameSpin) {
                _frameSpin->setRange( (int)firstFrame - 1, (int)lastFrame + 1 );
            }
            if (_rangeIn) {
                _rangeIn->setValue( (int)firstFrame );
            }
            if (_rangeOut) {
                _rangeOut->setValue( (int)lastFrame );
            }
            double fps = 24;
            ProjectPtr proj = app->getProject();
            if (proj) {
                KnobDoublePtr fpsKnob = std::dynamic_pointer_cast<KnobDouble>(
                    proj->getKnobByName(std::string("frameRate")));
                if (fpsKnob) {
                    fps = fpsKnob->getValue();
                }
            }
            if (_fpsLabel && (fps > 1e-6)) {
                _fpsLabel->setText( QString::fromUtf8("fps: %1").arg(fps, 0, 'f', 1) );
            }
        }
    }

    _playTimer = new QTimer(this);
    connect(_playTimer, SIGNAL(timeout()), this, SLOT(onPlayTick()));

    // A pane created from the pane corner menu binds to the FIRST VR Viewer
    // node already in the project (a pane created by node creation is bound
    // directly by the createNodeGui hook). The node-tree wire is the only
    // attachment model — no selector lives on the pane.
    autoBindToVRViewer();

    setFocusPolicy(Qt::ClickFocus);
}

ViewportVRTab::~ViewportVRTab()
{
}

// ==================== binding ====================

void
ViewportVRTab::setNodeBinding(const NodePtr& node)
{
    _boundNode = node;
    if (_viewport) {
        _viewport->setNodeScriptName( node ? QString::fromUtf8( node->getScriptName().c_str() ) : QString() );
    }
}

void
ViewportVRTab::autoBindToVRViewer()
{
    if (_boundNode.lock()) {
        return;
    }
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    ProjectPtr proj = app ? app->getProject() : ProjectPtr();
    if (!proj) {
        return;
    }
    NodesList allNodes;
    proj->getNodes_recursive(allNodes, true);
    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
        if (!(*it)->isActivated()) continue;
        EffectInstancePtr eff = (*it)->getEffectInstance();
        if (!eff) continue;
        if (eff->getPluginID() == PLUGINID_NATRON_VRVIEWER) {
            setNodeBinding(*it);
            return;
        }
    }
}

// ==================== Slots ====================

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
    _channelsDropdown->setText(label);
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

TimeLinePtr
ViewportVRTab::getTimeline() const
{
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    return app ? app->getTimeLine() : TimeLinePtr();
}

// === transport ===

void
ViewportVRTab::onSeekFirst()
{
    TimeLinePtr timeline = getTimeline();
    if (!timeline || !_rangeIn) {
        return;
    }
    timeline->seekFrame(_rangeIn->value(), true, NULL, eTimelineChangeReasonUserSeek);
}

void
ViewportVRTab::onSeekPrev()
{
    TimeLinePtr timeline = getTimeline();
    if (!timeline) {
        return;
    }
    timeline->seekFrame(timeline->currentFrame() - 1, true, NULL, eTimelineChangeReasonUserSeek);
}

void
ViewportVRTab::onSeekNext()
{
    TimeLinePtr timeline = getTimeline();
    if (!timeline) {
        return;
    }
    timeline->seekFrame(timeline->currentFrame() + 1, true, NULL, eTimelineChangeReasonUserSeek);
}

void
ViewportVRTab::onSeekLast()
{
    TimeLinePtr timeline = getTimeline();
    if (!timeline || !_rangeOut) {
        return;
    }
    timeline->seekFrame(_rangeOut->value(), true, NULL, eTimelineChangeReasonUserSeek);
}

void
ViewportVRTab::onFrameSpinChanged(int frame)
{
    TimeLinePtr timeline = getTimeline();
    if (!timeline) {
        return;
    }
    timeline->seekFrame(frame, true, NULL, eTimelineChangeReasonUserSeek);
}

void
ViewportVRTab::onPlayToggled(bool playing)
{
    if (!_playTimer) {
        return;
    }
    if (playing) {
        // Play from the in point if we're not inside the range.
        TimeLinePtr timeline = getTimeline();
        if (timeline && _rangeIn && _rangeOut) {
            SequenceTime f = timeline->currentFrame();
            if ( (f < (SequenceTime)_rangeIn->value()) || (f > (SequenceTime)_rangeOut->value()) ) {
                timeline->seekFrame(_rangeIn->value(), true, NULL, eTimelineChangeReasonPlaybackSeek);
            }
        }
        // Interval from the project's frame rate knob.
        double fps = 24;
        GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
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
ViewportVRTab::onStopClicked()
{
    if (_playBtn) {
        _playBtn->setChecked(false); // stops the playback timer
    }
}

void
ViewportVRTab::onPlayTick()
{
    TimeLinePtr timeline = getTimeline();
    if (!timeline || !_rangeIn || !_rangeOut) {
        return;
    }
    SequenceTime first = (SequenceTime)_rangeIn->value();
    SequenceTime last = (SequenceTime)_rangeOut->value();
    SequenceTime next = timeline->currentFrame() + 1;
    if (next > last) {
        next = first; // loop
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
    // Sync the frame spinbox; the view re-fetches via its own timeline listen.
    if (_frameSpin) {
        _frameSpin->blockSignals(true);
        _frameSpin->setValue((int)frame);
        _frameSpin->blockSignals(false);
    }
    if (_viewport) {
        _viewport->update();
    }
}

NATRON_NAMESPACE_EXIT
