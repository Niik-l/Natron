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

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
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
#include "Engine/Node.h"
#include "Engine/ViewerInstance.h"
#include "Engine/OutputSchedulerThread.h" // RenderEngine
#include "Engine/Utils.h" // ipow

NATRON_NAMESPACE_ENTER

ViewportVRTab::ViewportVRTab(NodePtr viewerNode, Gui* gui, QWidget* parent)
    : QWidget(parent)
    , PanelWidget(this, gui)
    , _viewport(NULL)
    , _timelineGui(NULL)
    , _proxyScaleActive(0)
    , _proxyActive(false)
{
    assert(viewerNode);
    _boundNode = viewerNode;
    _viewerNode = std::dynamic_pointer_cast<ViewerInstance>(viewerNode->getEffectInstance());
    assert(_viewerNode);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    const QSize btnSize( TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
    const QSize btnIconSize( TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE) );

    // ==================== Top toolbar (viewer-style display controls) =======
    QWidget* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    {
        QSizePolicy sp = toolbar->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Ignored);
        toolbar->setSizePolicy(sp);
    }
    QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 2, 4, 2);
    toolbarLayout->setSpacing(4);

    // ---- Display channels (the viewer's RGB dropdown: RGB/R/G/B/A/Y/Matte) --
    {
        _channelsDropdown = new QToolButton(toolbar);
        _channelsDropdown->setText(QString::fromUtf8("RGB"));
        _channelsDropdown->setToolTip(QString::fromUtf8(
            "Channels: RGB / Red / Green / Blue / Alpha / Luminance / Matte — the "
            "same display channels the 2D viewer offers, rendered by the viewer "
            "pipeline."));
        _channelsDropdown->setFixedHeight(22);
        _channelsDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_channelsDropdown);
        const char* labels[7] = { "RGB", "Red", "Green", "Blue", "Alpha", "Luminance", "Matte" };
        for (int i = 0; i < 7; ++i) {
            QAction* a = menu->addAction(QString::fromUtf8(labels[i]));
            a->setData(i);
            connect(a, SIGNAL(triggered()), this, SLOT(onDisplayChannelsSelected()));
        }
        _channelsDropdown->setMenu(menu);
        toolbarLayout->addWidget(_channelsDropdown);
    }

    QFrame* sep1 = new QFrame(toolbar);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    toolbarLayout->addWidget(sep1);

    // ---- FOV (the VR counterpart of the viewer's zoom%: wheel + spin) ------
    {
        QLabel* fovLabel = new QLabel(QString::fromUtf8("FOV"), toolbar);
        fovLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(fovLabel);
        _fovSpin = new QDoubleSpinBox(toolbar);
        _fovSpin->setRange(10., 130.);
        _fovSpin->setSingleStep(5.);
        _fovSpin->setDecimals(0);
        _fovSpin->setValue(70.);
        _fovSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _fovSpin->setFixedWidth(52);
        _fovSpin->setFixedHeight(22);
        _fovSpin->setToolTip(QString::fromUtf8("Field of view: replaces the 2D viewer's zoom. Wheel over the canvas also changes it."));
        connect(_fovSpin, SIGNAL(valueChanged(double)), this, SLOT(onFovChanged(double)));
        toolbarLayout->addWidget(_fovSpin);
    }

    // Recenter (the plus button in the viewer) — recenter + reset FOV.
    QToolButton* resetBtn = new QToolButton(toolbar);
    resetBtn->setText(QString::fromUtf8("+"));
    resetBtn->setToolTip(QString::fromUtf8("Recenter: look back at the pano center and reset the FOV"));
    resetBtn->setFixedSize(btnSize);
    resetBtn->setIconSize(btnIconSize);
    connect(resetBtn, SIGNAL(clicked()), this, SLOT(onResetView()));
    toolbarLayout->addWidget(resetBtn);

    // ---- Pause updates (the viewer's pause, viewerNode->setViewerPaused) ----
    {
        QPixmap pixPauseOn, pixPauseOff;
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PAUSE_ENABLED, &pixPauseOn);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PAUSE_DISABLED, &pixPauseOff);
        _pauseBtn = new QToolButton(toolbar);
        _pauseBtn->setFocusPolicy(Qt::NoFocus);
        _pauseBtn->setFixedSize(btnSize);
        _pauseBtn->setIconSize(btnIconSize);
        _pauseBtn->setCheckable(true);
        _pauseBtn->setChecked(false);
        QIcon icPause;
        icPause.addPixmap(pixPauseOff, QIcon::Normal, QIcon::Off);
        icPause.addPixmap(pixPauseOn, QIcon::Normal, QIcon::On);
        _pauseBtn->setIcon(icPause);
        _pauseBtn->setToolTip(QString::fromUtf8("Pause Updates: freeze the VR viewport (stop auto-refresh)."));
        connect(_pauseBtn, SIGNAL(toggled(bool)), this, SLOT(onPauseToggled(bool)));
        toolbarLayout->addWidget(_pauseBtn);
    }

    // ---- ROI windowing toggle — VR form of the user ROI: render only the ----
    // ---- pano slice visible through the FOV.                          ----
    {
        _roiBtn = new QToolButton(toolbar);
        _roiBtn->setText(QString::fromUtf8("ROI"));
        _roiBtn->setToolTip(QString::fromUtf8(
            "Render only the slice of the panorama visible through the current "
            "FOV (like the 2D viewer's Region of Interest): much faster. Turn "
            "off to always render the whole panorama."));
        _roiBtn->setFixedHeight(22);
        _roiBtn->setCheckable(true);
        _roiBtn->setChecked(true);
        _roiBtn->setDown(true);
        connect(_roiBtn, SIGNAL(toggled(bool)), this, SLOT(onRoiToggled(bool)));
        toolbarLayout->addWidget(_roiBtn);
    }

    // ---- proxy group (viewer render-scale activate + scale combo) ---------
    {
        _proxyBtn = new QToolButton(toolbar);
        _proxyBtn->setText(QString::fromUtf8("P"));
        _proxyBtn->setToolTip(QString::fromUtf8("Proxy mode: renders at a downscale factor to accelerate the VR preview."));
        _proxyBtn->setFixedSize(btnSize);
        _proxyBtn->setIconSize(btnIconSize);
        _proxyBtn->setCheckable(true);
        _proxyBtn->setChecked(false);
        connect(_proxyBtn, SIGNAL(toggled(bool)), this, SLOT(onProxyToggled(bool)));
        toolbarLayout->addWidget(_proxyBtn);

        _proxyScaleDropdown = new QToolButton(toolbar);
        _proxyScaleDropdown->setText(QString::fromUtf8("2"));
        _proxyScaleDropdown->setToolTip(QString::fromUtf8("Proxy scale factor."));
        _proxyScaleDropdown->setFixedHeight(22);
        _proxyScaleDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_proxyScaleDropdown);
        const int factors[5] = { 2, 4, 8, 16, 32 };
        for (int i = 0; i < 5; ++i) {
            QAction* a = menu->addAction(QString::number(factors[i]));
            a->setData(i);
            connect(a, SIGNAL(triggered()), this, SLOT(onProxyScaleChanged()));
        }
        _proxyScaleDropdown->setMenu(menu);
        toolbarLayout->addWidget(_proxyScaleDropdown);
    }

    // ---- comparison (A/B) + operator — feeds the same viewer inputs -------
    {
        _compareDropdown = new QToolButton(toolbar);
        _compareDropdown->setText(QString::fromUtf8("A/B"));
        _compareDropdown->setToolTip(QString::fromUtf8(
            "Comparison between inputs of the Viewer VR node: Difference / "
            "Minus / Onion-skin. The 2D viewer's A/B machinery renders both "
            "inputs; the VR sampler applies the operation."));
        _compareDropdown->setFixedHeight(22);
        _compareDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_compareDropdown);
        const char* opLabels[4] = { "A only", "B", "Difference/Minus", "Onion skin" };
        const int ops[4] = { (int)eViewerCompositingOperatorNone,
                             (int)eViewerCompositingOperatorStackUnder,
                             (int)eViewerCompositingOperatorStackMinus,
                             (int)eViewerCompositingOperatorStackOnionSkin };
        for (int i = 0; i < 4; ++i) {
            QAction* a = menu->addAction(QString::fromUtf8(opLabels[i]));
            a->setData(ops[i]);
            connect(a, SIGNAL(triggered()), this, SLOT(onCompareOperatorSelected()));
        }
        _compareDropdown->setMenu(menu);
        toolbarLayout->addWidget(_compareDropdown);
    }

    // ---- Gain / Gamma spins (the viewer's f-stop boxes, gain = 2^stops) ----
    {
        QLabel* gainLabel = new QLabel(QString::fromUtf8("Gain"), toolbar);
        gainLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(gainLabel);
        _gainSpin = new QDoubleSpinBox(toolbar);
        _gainSpin->setRange(0.01, 64.);
        _gainSpin->setSingleStep(0.25);
        _gainSpin->setDecimals(2);
        _gainSpin->setValue(1.0);
        _gainSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _gainSpin->setFixedWidth(56);
        _gainSpin->setFixedHeight(22);
        _gainSpin->setToolTip(QString::fromUtf8("Viewer gain (multiplies colors before the display transform; the 2D viewer spells it in f-stops)."));
        connect(_gainSpin, SIGNAL(valueChanged(double)), this, SLOT(onGainChanged(double)));
        toolbarLayout->addWidget(_gainSpin);

        QLabel* gammaLabel = new QLabel(QString::fromUtf8("Gamma"), toolbar);
        gammaLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(gammaLabel);
        _gammaSpin = new QDoubleSpinBox(toolbar);
        _gammaSpin->setRange(0.05, 10.);
        _gammaSpin->setSingleStep(0.05);
        _gammaSpin->setDecimals(2);
        _gammaSpin->setValue(1.0);
        _gammaSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _gammaSpin->setFixedWidth(56);
        _gammaSpin->setFixedHeight(22);
        _gammaSpin->setToolTip(QString::fromUtf8("Viewer gamma (the 2D viewer's Gamma)."));
        connect(_gammaSpin, SIGNAL(valueChanged(double)), this, SLOT(onGammaChanged(double)));
        toolbarLayout->addWidget(_gammaSpin);
    }

    // ---- VR-specific: stereo packing + eye (VR settings group) ------------
    {
        _layoutDropdown = new QToolButton(toolbar);
        _layoutDropdown->setText(QString::fromUtf8("Layout: Mono"));
        _layoutDropdown->setToolTip(QString::fromUtf8(
            "How the 2:1 source packs the eyes: Mono (whole frame), Over-Under "
            "(left eye = top half, the Google top-bottom convention), "
            "Side-by-Side (left eye = left half)."));
        _layoutDropdown->setFixedHeight(22);
        _layoutDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_layoutDropdown);
        const char* layoutLabels[3] = { "Mono", "Over-Under", "Side-by-Side" };
        const int modes[3]    = { (int)ViewportVR::eLayoutMono,
                                  (int)ViewportVR::eLayoutOverUnder,
                                  (int)ViewportVR::eLayoutSideBySide };
        for (int i = 0; i < 3; ++i) {
            QAction* a = menu->addAction(QString::fromUtf8(layoutLabels[i]));
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
    mainLayout->addWidget(toolbar);

    // ==================== VR canvas ====================
    _viewport = new ViewportVR(gui);
    mainLayout->addWidget(_viewport, 1); // stretch factor 1 = takes all remaining space
    if (_viewerNode) {
        // Engine → canvas: the same wiring ViewerTab does between ViewerGL
        // and the ViewerInstance (OpenGLViewerI contract).
        _viewerNode->setUiContext( (OpenGLViewerI*)_viewport );
    }

    // ==================== Player bar (viewer-style transport row) ==========
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

        // Playback in-point spin (bottom-left of the viewer's player bar)
        _rangeIn = new QDoubleSpinBox(playerBar);
        _rangeIn->setRange(std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
        _rangeIn->setDecimals(0);
        _rangeIn->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _rangeIn->setFixedWidth(52);
        _rangeIn->setFixedHeight(22);
        _rangeIn->setKeyboardTracking(false);
        _rangeIn->setToolTip(QString::fromUtf8("Playback first frame (in point)"));
        playerLayout->addWidget(_rangeIn);

        // fps readout/spin — drives the render engine's setDesiredFPS
        _fpsLabel = new QLabel(QString::fromUtf8("fps"), playerBar);
        _fpsLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        playerLayout->addWidget(_fpsLabel);
        QDoubleSpinBox* fpsSpin = new QDoubleSpinBox(playerBar);
        fpsSpin->setRange(1., 100.);
        fpsSpin->setDecimals(1);
        fpsSpin->setValue(24.);
        fpsSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        fpsSpin->setFixedWidth(52);
        fpsSpin->setFixedHeight(22);
        connect( fpsSpin, SIGNAL(valueChanged(double)), this, SLOT(onFpsSpinChanged(double)) );
        playerLayout->addWidget(fpsSpin);

        // transport buttons — same icons as the 2D viewer's player
        QPixmap pixFirst, pixBack1, pixForward1, pixLast, pixPlayDisabled, pixStop, pixPrevKey, pixNextKey, pixPrevIncr, pixNextIncr;
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_FIRST_FRAME, &pixFirst);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PREVIOUS, &pixBack1);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_NEXT, &pixForward1);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_LAST_FRAME, &pixLast);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PLAY_DISABLED, &pixPlayDisabled);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_STOP_ENABLED, &pixStop);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PREVIOUS_KEY, &pixPrevKey);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_NEXT_KEY, &pixNextKey);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PREVIOUS_INCR, &pixPrevIncr);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_NEXT_INCR, &pixNextIncr);

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
        prevBtn->setToolTip(QString::fromUtf8("Go to previous frame"));
        connect(prevBtn, SIGNAL(clicked()), this, SLOT(onSeekPrev()));
        playerLayout->addWidget(prevBtn);

        // Play: same icon trick as the 2D viewer's play button (down = stop).
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
        _playBtn->setToolTip(QString::fromUtf8("Play/stop through the viewer render engine (Space; fps from the fps box, loops over the in/out points)."));
        connect(_playBtn, SIGNAL(toggled(bool)), this, SLOT(onPlayToggled(bool)));
        playerLayout->addWidget(_playBtn);

        QToolButton* nextBtn = new QToolButton(playerBar);
        nextBtn->setFocusPolicy(Qt::NoFocus);
        nextBtn->setFixedSize(btnSize);
        nextBtn->setIconSize(btnIconSize);
        nextBtn->setIcon( QIcon(pixForward1) );
        nextBtn->setToolTip(QString::fromUtf8("Go to next frame"));
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

        // Frame counter spin (the "0" in the middle of the 2D player bar)
        _frameSpin = new QDoubleSpinBox(playerBar);
        _frameSpin->setRange(std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
        _frameSpin->setDecimals(0);
        _frameSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _frameSpin->setFixedWidth(60);
        _frameSpin->setFixedHeight(22);
        _frameSpin->setKeyboardTracking(false);
        _frameSpin->setToolTip(QString::fromUtf8("Current frame"));
        connect(_frameSpin, SIGNAL(valueChanged(double)), this, SLOT(onFrameSpinChanged(double)));
        playerLayout->addWidget(_frameSpin);

        // keyframe + increment steppers (same app slots as the 2D viewer)
        QToolButton* prevKeyBtn = new QToolButton(playerBar);
        prevKeyBtn->setFocusPolicy(Qt::NoFocus);
        prevKeyBtn->setFixedSize(btnSize);
        prevKeyBtn->setIconSize(btnIconSize);
        prevKeyBtn->setIcon( QIcon(pixPrevKey) );
        prevKeyBtn->setToolTip(QString::fromUtf8("Go to previous keyframe"));
        connect( prevKeyBtn, SIGNAL(clicked()), getGui()->getApp().get(), SLOT(goToPreviousKeyframe()) );
        playerLayout->addWidget(prevKeyBtn);

        QToolButton* incrPrev = new QToolButton(playerBar);
        incrPrev->setFocusPolicy(Qt::NoFocus);
        incrPrev->setFixedSize(btnSize);
        incrPrev->setIconSize(btnIconSize);
        incrPrev->setIcon( QIcon(pixPrevIncr) );
        incrPrev->setToolTip(QString::fromUtf8("Go back by the increment (5)"));
        connect(incrPrev, SIGNAL(clicked()), this, SLOT(onSeekPrevIncr()));
        playerLayout->addWidget(incrPrev);

        QLabel* incrLabel = new QLabel(QString::fromUtf8("5"), playerBar);
        incrLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        incrLabel->setToolTip(QString::fromUtf8("Increment step (fixed 5 in the first cut; editable increment arrives with the 2D-viewer toolbar parity pass)"));
        playerLayout->addWidget(incrLabel);

        QToolButton* incrNext = new QToolButton(playerBar);
        incrNext->setFocusPolicy(Qt::NoFocus);
        incrNext->setFixedSize(btnSize);
        incrNext->setIconSize(btnIconSize);
        incrNext->setIcon( QIcon(pixNextIncr) );
        incrNext->setToolTip(QString::fromUtf8("Go forward by the increment (5)"));
        connect(incrNext, SIGNAL(clicked()), this, SLOT(onSeekNextIncr()));
        playerLayout->addWidget(incrNext);

        QToolButton* nextKeyBtn = new QToolButton(playerBar);
        nextKeyBtn->setFocusPolicy(Qt::NoFocus);
        nextKeyBtn->setFixedSize(btnSize);
        nextKeyBtn->setIconSize(btnIconSize);
        nextKeyBtn->setIcon( QIcon(pixNextKey) );
        nextKeyBtn->setToolTip(QString::fromUtf8("Go to next keyframe"));
        connect( nextKeyBtn, SIGNAL(clicked()), getGui()->getApp().get(), SLOT(goToNextKeyframe()) );
        playerLayout->addWidget(nextKeyBtn);

        // Playback out-point spin
        playerLayout->addSpacing(8);
        _rangeOut = new QDoubleSpinBox(playerBar);
        _rangeOut->setRange(std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
        _rangeOut->setDecimals(0);
        _rangeOut->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _rangeOut->setFixedWidth(52);
        _rangeOut->setFixedHeight(22);
        _rangeOut->setKeyboardTracking(false);
        _rangeOut->setToolTip(QString::fromUtf8("Playback last frame (out point)"));
        playerLayout->addWidget(_rangeOut);

        playerLayout->addStretch();
        mainLayout->addWidget(playerBar);
    }

    // ==================== Timeline Scrubber ====================
    {
        GuiAppInstancePtr app = gui->getApp();
        if (app) {
            TimeLinePtr timeline = app->getTimeLine();
            double firstFrame = 1, lastFrame = 250;
            app->getFrameRange(&firstFrame, &lastFrame);
            if (timeline) {
                _timelineGui = new TimeLineGui(NULL, timeline, gui, NULL);
                _timelineGui->setBoundaries((SequenceTime)firstFrame, (SequenceTime)lastFrame);
                _timelineGui->seek(timeline->currentFrame());
                mainLayout->addWidget(_timelineGui);
                connect(timeline.get(), SIGNAL(frameChanged(SequenceTime,int)),
                        this, SLOT(onFrameChanged(SequenceTime,int)));
            }
            if (_viewport) {
                _viewport->setVRPlaybackRange( (int)firstFrame, (int)lastFrame );
            }
            if (_rangeIn) {
                _rangeIn->setValue( (double)firstFrame );
            }
            if (_rangeOut) {
                _rangeOut->setValue( (double)lastFrame );
            }
        }
    }

    // The engine buttons: when a render starts/stops, the play button follows.
    if (_viewerNode) {
        RenderEnginePtr engine = _viewerNode->getRenderEngine();
        connect( engine.get(), SIGNAL(renderFinished(int)), this, SLOT(onEngineStopped(int)) );
        connect( engine.get(), SIGNAL(renderStarted(bool)), this, SLOT(onEngineStarted(bool)) );
    }

    setFocusPolicy(Qt::ClickFocus);
}

ViewportVRTab::~ViewportVRTab()
{
}

// ==================== slots: display ====================

void
ViewportVRTab::onDisplayChannelsSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a || !_viewerNode) {
        return;
    }
    DisplayChannelsEnum c = (DisplayChannelsEnum)a->data().toInt();
    _viewerNode->setDisplayChannels(c, true);
    QString label;
    switch (c) {
    case eDisplayChannelsRGB: label = QString::fromUtf8("RGB"); break;
    case eDisplayChannelsR:   label = QString::fromUtf8("Red"); break;
    case eDisplayChannelsG:   label = QString::fromUtf8("Green"); break;
    case eDisplayChannelsB:   label = QString::fromUtf8("Blue"); break;
    case eDisplayChannelsA:   label = QString::fromUtf8("Alpha"); break;
    case eDisplayChannelsY:   label = QString::fromUtf8("Luminance"); break;
    case eDisplayChannelsMatte: label = QString::fromUtf8("Matte"); break;
    }
    _channelsDropdown->setText(label);
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
    _layoutDropdown->setText( QString::fromUtf8("Layout: ") + label );
}

void
ViewportVRTab::onEyeSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a || !_viewport) {
        return;
    }
    _viewport->setEye( a->data().toInt() );
    _eyeDropdown->setText( QString::fromUtf8("Eye: ") + QString::fromUtf8(a->data().toInt() == 0 ? "Left" : "Right") );
}

void
ViewportVRTab::onFovChanged(double fov)
{
    if (!_viewport) {
        return;
    }
    double yaw, pitch;
    _viewport->getLook(&yaw, &pitch, NULL);
    _viewport->setLook(yaw, pitch, fov);
}

void
ViewportVRTab::onResetView()
{
    if (_viewport) {
        _viewport->resetLook();
        if (_fovSpin) {
            _fovSpin->setValue(70.);
        }
    }
}

void
ViewportVRTab::onPauseToggled(bool paused)
{
    if (_viewerNode) {
        _viewerNode->setViewerPaused(paused, true);
    }
}

void
ViewportVRTab::onRoiToggled(bool on)
{
    if (_viewport) {
        _viewport->setUserRoIEnabled(on);
    }
}

void
ViewportVRTab::onProxyToggled(bool on)
{
    _proxyActive = on;
    unsigned int level = on ? (unsigned int)(_proxyScaleActive + 1) : 0;
    if (_viewerNode) {
        _viewerNode->onMipmapLevelChanged(level);
    }
    if (_viewport) {
        _viewport->update();
    }
}

void
ViewportVRTab::onProxyScaleChanged()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a) {
        return;
    }
    _proxyScaleActive = a->data().toInt();
    if (_proxyScaleDropdown) {
        _proxyScaleDropdown->setText( QString::number( ipow(2, _proxyScaleActive + 1) ) );
    }
    if (_proxyActive && _viewerNode) {
        _viewerNode->onMipmapLevelChanged( (unsigned int)(_proxyScaleActive + 1) );
    }
    if (_viewport) {
        _viewport->update();
    }
}

void
ViewportVRTab::onCompareOperatorSelected()
{
    QAction* a = qobject_cast<QAction*>(sender());
    if (!a || !_viewport) {
        return;
    }
    ViewerCompositingOperatorEnum op = (ViewerCompositingOperatorEnum)a->data().toInt();
    _viewport->setCompositingOperator(op);
    // The engine reads getCompositingOperator() off the uiContext (our
    // ViewportVR), so the B input renders only when an op is active — same
    // mechanism as the 2D viewer's compare.
    QString label;
    switch (op) {
    case eViewerCompositingOperatorNone: label = QString::fromUtf8("A/B"); break;
    case eViewerCompositingOperatorStackUnder: label = QString::fromUtf8("B"); break;
    case eViewerCompositingOperatorStackMinus: label = QString::fromUtf8("Diff"); break;
    case eViewerCompositingOperatorStackOnionSkin: label = QString::fromUtf8("Onion"); break;
    case eViewerCompositingOperatorWipeUnder: label = QString::fromUtf8("B"); break;
    case eViewerCompositingOperatorWipeMinus: label = QString::fromUtf8("Diff"); break;
    case eViewerCompositingOperatorWipeOnionSkin: label = QString::fromUtf8("Onion"); break;
    default: label = QString::fromUtf8("A/B"); break;
    }
    _compareDropdown->setText(label);
}

void
ViewportVRTab::onGainChanged(double gain)
{
    if (_viewerNode) {
        _viewerNode->onGainChanged(gain);
    }
}

void
ViewportVRTab::onGammaChanged(double gamma)
{
    if (_viewerNode) {
        _viewerNode->onGammaChanged(gamma);
    }
}

void
ViewportVRTab::onFpsSpinChanged(double fps)
{
    if (_viewerNode) {
        _viewerNode->getRenderEngine()->setDesiredFPS(fps);
    }
}

// ==================== transport ====================

TimeLinePtr
ViewportVRTab::getTimeline() const
{
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    return app ? app->getTimeLine() : TimeLinePtr();
}

void
ViewportVRTab::refreshTransportState()
{
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    if (!app) {
        return;
    }
    double firstFrame = 1, lastFrame = 250;
    app->getFrameRange(&firstFrame, &lastFrame);
    if (_timelineGui) {
        _timelineGui->setBoundaries((SequenceTime)firstFrame, (SequenceTime)lastFrame);
    }
    if (_viewport) {
        _viewport->setVRPlaybackRange( (int)firstFrame, (int)lastFrame );
    }
    if (_rangeIn) {
        _rangeIn->setValue( firstFrame );
    }
    if (_rangeOut) {
        _rangeOut->setValue( lastFrame );
    }
}

void
ViewportVRTab::onSeekFirst()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline && _rangeIn) {
        timeline->seekFrame( (SequenceTime)_rangeIn->value(), true, NULL, eTimelineChangeReasonUserSeek );
    }
}

void
ViewportVRTab::onSeekLast()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline && _rangeOut) {
        timeline->seekFrame( (SequenceTime)_rangeOut->value(), true, NULL, eTimelineChangeReasonUserSeek );
    }
}

void
ViewportVRTab::onSeekPrev()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline) {
        timeline->seekFrame(timeline->currentFrame() - 1, true, NULL, eTimelineChangeReasonUserSeek);
    }
}

void
ViewportVRTab::onSeekNext()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline) {
        timeline->seekFrame(timeline->currentFrame() + 1, true, NULL, eTimelineChangeReasonUserSeek);
    }
}

void
ViewportVRTab::onSeekPrevKey() {}

void
ViewportVRTab::onSeekNextKey() {}

void
ViewportVRTab::onSeekPrevIncr()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline) {
        timeline->seekFrame(timeline->currentFrame() - 5, true, NULL, eTimelineChangeReasonUserSeek);
    }
}

void
ViewportVRTab::onSeekNextIncr()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline) {
        timeline->seekFrame(timeline->currentFrame() + 5, true, NULL, eTimelineChangeReasonUserSeek);
    }
}

void
ViewportVRTab::onFrameSpinChanged(double frame)
{
    TimeLinePtr timeline = getTimeline();
    if (timeline) {
        timeline->seekFrame( (SequenceTime)frame, true, NULL, eTimelineChangeReasonUserSeek );
    }
    if (_viewport) {
        _viewport->update();
    }
}

// Playback through the viewer render engine, the ViewerTab::startPause way:
// the engine walks the timeline itself (looping over the in/out points at the
// desired fps) and 2D viewers sharing the timeline follow.
void
ViewportVRTab::onPlayToggled(bool playing)
{
    if (!_viewerNode) {
        return;
    }
    if (playing) {
        GuiAppInstancePtr app = getGui()->getApp();
        if ( !app || app->checkAllReadersModificationDate(true) ) {
            return;
        }
        app->setLastViewerUsingTimeline(_viewerNode->getNode());
        std::vector<ViewIdx> viewsToRender;
        viewsToRender.push_back(_viewport ? _viewport->getCurrentView() : ViewIdx(0));
        _viewerNode->getRenderEngine()->renderFromCurrentFrame(app->isRenderStatsActionChecked(),
                                                               viewsToRender,
                                                               eRenderDirectionForward);
    } else {
        _viewerNode->getRenderEngine()->abortRenderingNoRestart();
    }
}

void
ViewportVRTab::onEngineStarted(bool forward)
{
    Q_UNUSED(forward);
    if (_playBtn && !_playBtn->isChecked()) {
        _playBtn->setChecked(true);
    }
}

void
ViewportVRTab::onEngineStopped(int)
{
    if (_playBtn && _playBtn->isChecked()) {
        _playBtn->setChecked(false);
    }
}

void
ViewportVRTab::onFrameChanged(SequenceTime frame, int)
{
    if (_frameSpin) {
        _frameSpin->blockSignals(true);
        _frameSpin->setValue((double)frame);
        _frameSpin->blockSignals(false);
    }
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
ViewportVRTab::onForceRefresh()
{
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    if (app) {
        refreshTransportState();
    }
    if (_viewerNode) {
        std::vector<ViewIdx> viewsToRender;
        viewsToRender.push_back(_viewport ? _viewport->getCurrentView() : ViewIdx(0));
        _viewerNode->getRenderEngine()->renderFromCurrentFrame(app ? app->isRenderStatsActionChecked() : false,
                                                               viewsToRender,
                                                               eRenderDirectionForward);
    }
}

NATRON_NAMESPACE_EXIT
