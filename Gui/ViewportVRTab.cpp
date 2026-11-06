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
#include <QDateTime>
#include <QTimer>
#include <QDoubleSpinBox>

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/Button.h"
#include "Gui/ComboBox.h"
#include "Gui/GuiDefines.h"
#include "Gui/ScaleSliderQWidget.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewportVR.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h" // InspectorNode
#include "Engine/TimeLine.h"
#include "Engine/Project.h"
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
    , _aLabel(NULL)
    , _aInput(NULL)
    , _bLabel(NULL)
    , _bInput(NULL)
    , _gainSlider(NULL)
    , _gainOn(false)
    , _lastFstop(0.)
    , _gammaSlider(NULL)
    , _gammaOn(false)
    , _lastGamma(1.)
    , _playModeBtn(NULL)
    , _setInBtn(NULL)
    , _setOutBtn(NULL)
    , _proxyScaleActive(0)
    , _proxyActive(false)
{
    assert(viewerNode);
    _boundNode = viewerNode;
    _viewerNode = std::dynamic_pointer_cast<ViewerInstance>(viewerNode->getEffectInstance());
    assert(_viewerNode);
    _lastLookRefreshValid = false;
    _userStoppedMs = 0;

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
        _channelsDropdown->setFixedSize(btnSize);
        _channelsDropdown->setIconSize(btnIconSize);
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

    // ---- Input A selector (the viewer's "A:" combobox over the inspector ----
    // ---- inputs) + comparison B selector + operator dropdown.          ----
    {
        _aLabel = new QLabel(QString::fromUtf8("A:"), toolbar);
        _aLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(_aLabel);
        _aInput = new ComboBox(toolbar);
        _aInput->addItem( QString::fromUtf8(" - ") );
        _aInput->setMaximumWidthFromText( QString::fromUtf8("Transform1") );
        _aInput->setToolTip(QString::fromUtf8(
            "Input A: pick which node the VR viewport renders. The list fills "
            "with the nodes connected to the Viewer VR node in the graph."));
        QObject::connect( _aInput, SIGNAL(currentIndexChanged(QString)), this, SLOT(onFirstInputNameChanged(QString)) );
        toolbarLayout->addWidget(_aInput);
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

    // Recenter (the viewer's center button) — recenter + reset FOV.
    QToolButton* resetBtn = new QToolButton(toolbar);
    {
        QPixmap pixCenter;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_CENTER, &pixCenter);
        if ( !pixCenter.isNull() ) {
            resetBtn->setIcon( QIcon(pixCenter) );
        } else {
            resetBtn->setText(QString::fromUtf8("+"));
        }
    }
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
        QPixmap pixRoiOn, pixRoiOff;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_ROI_ENABLED, &pixRoiOn);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_ROI_DISABLED, &pixRoiOff);
        _roiBtn = new QToolButton(toolbar);
        if ( !pixRoiOn.isNull() ) {
            QIcon icRoi;
            icRoi.addPixmap(pixRoiOff, QIcon::Normal, QIcon::Off);
            icRoi.addPixmap(pixRoiOn, QIcon::Normal, QIcon::On);
            _roiBtn->setIcon(icRoi);
        } else {
            _roiBtn->setText(QString::fromUtf8("ROI"));
        }
        _roiBtn->setToolTip(QString::fromUtf8(
            "Render only the slice of the panorama visible through the current "
            "FOV (like the 2D viewer's Region of Interest): much faster. Turn "
            "off to always render the whole panorama."));
        _roiBtn->setFixedSize(btnSize);
        _roiBtn->setIconSize(btnIconSize);
        _roiBtn->setCheckable(true);
        _roiBtn->setChecked(true);
        _roiBtn->setDown(true);
        connect(_roiBtn, SIGNAL(toggled(bool)), this, SLOT(onRoiToggled(bool)));
        toolbarLayout->addWidget(_roiBtn);
    }

    // ---- proxy group (viewer render-scale activate + scale combo) ---------
    {
        QPixmap pixRs, pixRsChecked;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_RENDER_SCALE, &pixRs);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_RENDER_SCALE_CHECKED, &pixRsChecked);
        _proxyBtn = new QToolButton(toolbar);
        if ( !pixRs.isNull() ) {
            QIcon icRs;
            icRs.addPixmap(pixRs, QIcon::Normal, QIcon::Off);
            icRs.addPixmap(pixRsChecked, QIcon::Normal, QIcon::On);
            _proxyBtn->setIcon(icRs);
        } else {
            _proxyBtn->setText(QString::fromUtf8("P"));
        }
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
        QPixmap pixMerge;
        appPTR->getIcon(NATRON_PIXMAP_MERGE_GROUPING, &pixMerge);
        QLabel* opLabel = new QLabel(toolbar);
        if ( !pixMerge.isNull() ) {
            opLabel->setPixmap(pixMerge);
        } else {
            opLabel->setText(QString::fromUtf8("A/B"));
        }
        opLabel->setToolTip(QString::fromUtf8(
            "Operation applied between the viewer inputs A and B."));
        toolbarLayout->addWidget(opLabel);

        _compareDropdown = new QToolButton(toolbar);
        _compareDropdown->setText(QString::fromUtf8(" - "));
        _compareDropdown->setToolTip(QString::fromUtf8(
            "Comparison between inputs of the Viewer VR node: Difference / "
            "Minus / Onion-skin. The 2D viewer's A/B machinery renders both "
            "inputs; the VR sampler applies the operation."));
        _compareDropdown->setFixedHeight(22);
        _compareDropdown->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(_compareDropdown);
        const char* opLabels[9] = { " - ", "W-Under", "W-Over", "W-Minus", "W-OnionSkin",
                                    "S-Under", "S-Over", "S-Minus", "S-OnionSkin" };
        const int ops[9] = { (int)eViewerCompositingOperatorNone,
                             (int)eViewerCompositingOperatorWipeUnder,
                             (int)eViewerCompositingOperatorWipeOver,
                             (int)eViewerCompositingOperatorWipeMinus,
                             (int)eViewerCompositingOperatorWipeOnionSkin,
                             (int)eViewerCompositingOperatorStackUnder,
                             (int)eViewerCompositingOperatorStackOver,
                             (int)eViewerCompositingOperatorStackMinus,
                             (int)eViewerCompositingOperatorStackOnionSkin };
        for (int i = 0; i < 9; ++i) {
            QAction* a = menu->addAction(QString::fromUtf8(opLabels[i]));
            a->setData(ops[i]);
            connect(a, SIGNAL(triggered()), this, SLOT(onCompareOperatorSelected()));
        }
        _compareDropdown->setMenu(menu);
        toolbarLayout->addWidget(_compareDropdown);
    }

    // ---- Input B selector (the viewer's "B:" combobox; feeds the compare) --
    {
        _bLabel = new QLabel(QString::fromUtf8("B:"), toolbar);
        _bLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(_bLabel);
        _bInput = new ComboBox(toolbar);
        _bInput->addItem( QString::fromUtf8(" - ") );
        _bInput->setMaximumWidthFromText( QString::fromUtf8("Transform1") );
        _bInput->setEnabled_natron(false);
        _bInput->setToolTip(QString::fromUtf8(
            "Input B: the second image for the A/B comparison (enabled once a "
            "comparison operator is picked)."));
        QObject::connect( _bInput, SIGNAL(currentIndexChanged(QString)), this, SLOT(onSecondInputNameChanged(QString)) );
        toolbarLayout->addWidget(_bInput);
    }

    // ---- Gain / Gamma (the viewer's f-stop boxes + scale sliders) ----------
    {
        QPixmap pixGainOn, pixGainOff;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAIN_ENABLED, &pixGainOn);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAIN_DISABLED, &pixGainOff);
        QToolButton* gainToggle = new QToolButton(toolbar);
        if ( !pixGainOn.isNull() ) {
            QIcon icGain;
            icGain.addPixmap(pixGainOff, QIcon::Normal, QIcon::Off);
            icGain.addPixmap(pixGainOn, QIcon::Normal, QIcon::On);
            gainToggle->setIcon(icGain);
        } else {
            gainToggle->setText(QString::fromUtf8("G"));
        }
        gainToggle->setCheckable(true);
        gainToggle->setChecked(false);
        gainToggle->setFixedSize(btnSize);
        gainToggle->setIconSize(btnIconSize);
        gainToggle->setToolTip(QString::fromUtf8("Gain: switch between the neutral gain (1.0) and the last value used. The spin/slider are in f-stops (gain = 2^x)."));
        connect(gainToggle, SIGNAL(toggled(bool)), this, SLOT(onGainToggled(bool)));
        toolbarLayout->addWidget(gainToggle);

        _gainSpin = new QDoubleSpinBox(toolbar);
        _gainSpin->setRange(-6., 6.);
        _gainSpin->setSingleStep(0.25);
        _gainSpin->setDecimals(2);
        _gainSpin->setValue(0.);
        _gainSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _gainSpin->setFixedWidth(56);
        _gainSpin->setFixedHeight(22);
        _gainSpin->setToolTip(QString::fromUtf8("Viewer gain in f-stops (gain = 2^x)."));
        connect(_gainSpin, SIGNAL(valueChanged(double)), this, SLOT(onGainSpinChanged(double)));
        toolbarLayout->addWidget(_gainSpin);

        _gainSlider = new ScaleSliderQWidget(-6., 6., 0., false, ScaleSliderQWidget::eDataTypeDouble, gui, eScaleTypeLinear, toolbar);
        _gainSlider->setMinimumHeight(22);
        _gainSlider->setMaximumWidth(110);
        _gainSlider->setToolTip(QString::fromUtf8("Viewer gain in f-stops (drag; gain = 2^x)."));
        QObject::connect( _gainSlider, SIGNAL(positionChanged(double)), this, SLOT(onGainSpinChanged(double)) );
        toolbarLayout->addWidget(_gainSlider);

        QPixmap pixGammaOn, pixGammaOff;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAMMA_ENABLED, &pixGammaOn);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAMMA_DISABLED, &pixGammaOff);
        QToolButton* gammaToggle = new QToolButton(toolbar);
        if ( !pixGammaOn.isNull() ) {
            QIcon icGamma;
            icGamma.addPixmap(pixGammaOff, QIcon::Normal, QIcon::Off);
            icGamma.addPixmap(pixGammaOn, QIcon::Normal, QIcon::On);
            gammaToggle->setIcon(icGamma);
        } else {
            gammaToggle->setText(QString::fromUtf8("γ"));
        }
        gammaToggle->setCheckable(true);
        gammaToggle->setChecked(false);
        gammaToggle->setFixedSize(btnSize);
        gammaToggle->setIconSize(btnIconSize);
        gammaToggle->setToolTip(QString::fromUtf8("Gamma: switch between the neutral gamma (1.0) and the last value used."));
        connect(gammaToggle, SIGNAL(toggled(bool)), this, SLOT(onGammaToggled(bool)));
        toolbarLayout->addWidget(gammaToggle);

        _gammaSpin = new QDoubleSpinBox(toolbar);
        _gammaSpin->setRange(0.05, 10.);
        _gammaSpin->setSingleStep(0.05);
        _gammaSpin->setDecimals(2);
        _gammaSpin->setValue(1.0);
        _gammaSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _gammaSpin->setFixedWidth(56);
        _gammaSpin->setFixedHeight(22);
        _gammaSpin->setToolTip(QString::fromUtf8("Viewer gamma (the 2D viewer's Gamma)."));
        connect(_gammaSpin, SIGNAL(valueChanged(double)), this, SLOT(onGammaSpinChanged(double)));
        toolbarLayout->addWidget(_gammaSpin);

        _gammaSlider = new ScaleSliderQWidget(0., 4., 1., false, ScaleSliderQWidget::eDataTypeDouble, gui, eScaleTypeLinear, toolbar);
        _gammaSlider->setMinimumHeight(22);
        _gammaSlider->setMaximumWidth(110);
        _gammaSlider->setToolTip(QString::fromUtf8("Viewer gamma (drag)."));
        QObject::connect( _gammaSlider, SIGNAL(positionChanged(double)), this, SLOT(onGammaSpinChanged(double)) );
        toolbarLayout->addWidget(_gammaSlider);
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
        _setInBtn = new QToolButton(playerBar);
        {
            QPixmap pixIn;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_TIMELINE_IN, &pixIn);
            if ( !pixIn.isNull() ) {
                _setInBtn->setIcon( QIcon(pixIn) );
            } else {
                _setInBtn->setText(QString::fromUtf8("["));
            }
        }
        _setInBtn->setFocusPolicy(Qt::NoFocus);
        _setInBtn->setFixedSize(btnSize);
        _setInBtn->setIconSize(btnIconSize);
        _setInBtn->setToolTip(QString::fromUtf8("Set the playback in point at the current frame"));
        connect(_setInBtn, SIGNAL(clicked()), this, SLOT(onPlaybackInButtonClicked()));
        playerLayout->addWidget(_setInBtn);

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
        incrLabel->setToolTip(QString::fromUtf8("Increment step (fixed 5 in the VR pane; the 2D viewer's player has the editable increment)."));
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

        _setOutBtn = new QToolButton(playerBar);
        {
            QPixmap pixOut;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_TIMELINE_OUT, &pixOut);
            if ( !pixOut.isNull() ) {
                _setOutBtn->setIcon( QIcon(pixOut) );
            } else {
                _setOutBtn->setText(QString::fromUtf8("]"));
            }
        }
        _setOutBtn->setFocusPolicy(Qt::NoFocus);
        _setOutBtn->setFixedSize(btnSize);
        _setOutBtn->setIconSize(btnIconSize);
        _setOutBtn->setToolTip(QString::fromUtf8("Set the playback out point at the current frame"));
        connect(_setOutBtn, SIGNAL(clicked()), this, SLOT(onPlaybackOutButtonClicked()));
        playerLayout->addWidget(_setOutBtn);

        // Playback mode: loop / bounce / once (engine's setPlaybackMode).
        _playModeBtn = new QToolButton(playerBar);
        {
            QPixmap pixLoop;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_LOOP_MODE, &pixLoop);
            if ( !pixLoop.isNull() ) {
                _playModeBtn->setIcon( QIcon(pixLoop) );
            } else {
                _playModeBtn->setText(QString::fromUtf8("L"));
            }
        }
        _playModeBtn->setFocusPolicy(Qt::NoFocus);
        _playModeBtn->setFixedSize(btnSize);
        _playModeBtn->setIconSize(btnIconSize);
        _playModeBtn->setToolTip(QString::fromUtf8("Behaviour when the playback hits the end of the range: loop / bounce / stop."));
        connect(_playModeBtn, SIGNAL(clicked()), this, SLOT(onPlaybackModeClicked()));
        playerLayout->addWidget(_playModeBtn);

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
                // Cache color marks on the timeline, like the 2D viewer's: the
                // render engine populates the shared viewer cache; these slots
                // just repaint the marks.
                _timelineGui->connectSlotsToViewerCache();
            }
            if (_viewport) {
                _viewport->setVRPlaybackRange( (int)firstFrame, (int)lastFrame );
            }
            if (_rangeIn) {
                _rangeIn->blockSignals(true);
                _rangeIn->setValue( (double)firstFrame );
                _rangeIn->blockSignals(false);
            }
            if (_rangeOut) {
                _rangeOut->blockSignals(true);
                _rangeOut->setValue( (double)lastFrame );
                _rangeOut->blockSignals(false);
            }
        }
    }

    // The engine buttons: when a render starts/stops, the play button follows.
    if (_viewerNode) {
        RenderEnginePtr engine = _viewerNode->getRenderEngine();
        connect( engine.get(), SIGNAL(renderFinished(int)), this, SLOT(onEngineStopped(int)) );
        connect( engine.get(), SIGNAL(renderStarted(bool)), this, SLOT(onEngineStarted(bool)) );

        // Graph edits on the Viewer VR node (inputs connect/disconnect) refill
        // the A/B comboboxes — the same wiring ViewerTab uses for its A:/B:
        // selectors.
        NodePtr wrapper = _viewerNode->getNode();
        if (wrapper) {
            QObject::connect( wrapper.get(), SIGNAL(inputChanged(int)), this, SLOT(onInputChanged(int)) );
            QObject::connect( wrapper.get(), SIGNAL(inputLabelChanged(int,QString)), this, SLOT(onInputNameChanged(int,QString)) );
            refreshInputCombos();
        }

        // After look changes: schedule one cheap re-render — completes any tile
        // an aborted render pass left out (full-pano renders don't depend on
        // the look; the engine cache makes the refresh nearly free).
        _lookRefreshTimer = new QTimer(this);
        _lookRefreshTimer->setSingleShot(true);
        connect(_lookRefreshTimer, SIGNAL(timeout()), this, SLOT(onLookRefreshTimeout()));
        connect(_viewport, SIGNAL(lookChanged()), this, SLOT(onViewportLookChanged()));
        // Wheel/reset FOV changes on the canvas mirror into the spin box
        // (placed after _viewport creation — the toolbar section runs before
        // the widget exists).
        connect(_viewport, SIGNAL(fovChanged(double)), this, SLOT(onViewportFovChanged(double)));
        _lastLookRefreshValid = false;
    }

    // In/out spinboxes drive the timeline boundaries (the engine reads them
    // back through getViewerFrameRange at playback start).
    if (_rangeIn) {
        QObject::connect( _rangeIn, SIGNAL(valueChanged(double)), this, SLOT(onRangeInChanged(double)) );
    }
    if (_rangeOut) {
        QObject::connect( _rangeOut, SIGNAL(valueChanged(double)), this, SLOT(onRangeOutChanged(double)) );
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
    if (_channelsDropdown) {
        _channelsDropdown->setText(label);
    }
}

// ==================== slots: input A/B selectors ====================

void
ViewportVRTab::refreshInputCombos()
{
    // Rebuild the A/B comboboxes from the InspectorNode's connected inputs,
    // preserving the current selection (ViewerTab's inputNamesMap model).
    if (!_viewerNode) {
        return;
    }
    NodePtr wrapper = _viewerNode->getNode();
    if (!wrapper) {
        return;
    }
    const std::vector<NodeWPtr>& inputs = wrapper->getGuiInputs();

    // remember current selections by input index
    int curA = -1, curB = -1;
    {
        const InspectorNode* inspector = dynamic_cast<const InspectorNode*>(wrapper.get());
        if (inspector) {
            inspector->getActiveInputs(curA, curB);
        }
    }

    _inputNamesMap.clear();
    if (_aInput) {
        _aInput->blockSignals(true);
        _aInput->clear();
        _aInput->addItem( QString::fromUtf8(" - ") );
    }
    if (_bInput) {
        _bInput->blockSignals(true);
        _bInput->clear();
        _bInput->addItem( QString::fromUtf8(" - ") );
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        NodePtr inp = inputs[i].lock();
        if (!inp) {
            continue;
        }
        InputName entry;
        entry.input = inp;
        entry.name = QString::fromUtf8( inp->getLabel().c_str() );
        _inputNamesMap.insert( std::make_pair( (int)i, entry ) );
        if (_aInput) {
            _aInput->addItem(entry.name);
        }
        if (_bInput) {
            _bInput->addItem(entry.name);
        }
    }

    // select the engine's active inputs
    if ( (curA != -1) && _aInput) {
        std::map<int, InputName>::const_iterator found = _inputNamesMap.find(curA);
        if ( found != _inputNamesMap.end() ) {
            const int idx = _aInput->itemIndex(found->second.name);
            if (idx != -1) {
                _aInput->setCurrentIndex_no_emit(idx);
            }
        }
    } else if (_aInput) {
        // no A: fall back to the first connected input
        for (std::map<int, InputName>::const_iterator it = _inputNamesMap.begin(); it != _inputNamesMap.end(); ++it) {
            const int idx = _aInput->itemIndex(it->second.name);
            if (idx != -1) {
                _aInput->setCurrentIndex_no_emit(idx);
            }
            break;
        }
    }
    if ( (curB != -1) && _bInput) {
        std::map<int, InputName>::const_iterator found = _inputNamesMap.find(curB);
        if ( found != _inputNamesMap.end() ) {
            const int idx = _bInput->itemIndex(found->second.name);
            if (idx != -1) {
                _bInput->setCurrentIndex_no_emit(idx);
            }
        }
    }
    if (_aInput) {
        _aInput->blockSignals(false);
    }
    if (_bInput) {
        _bInput->blockSignals(false);
    }
}

void
ViewportVRTab::onInputChanged(int inputNb)
{
    // A node connected/disconnected on the Viewer VR node in the graph: rebuild
    // the combobox entries (same as ViewerTab::onInputChanged).
    if (!_viewerNode) {
        return;
    }
    NodePtr wrapper = _viewerNode->getNode();
    if (!wrapper) {
        return;
    }
    const std::vector<NodeWPtr>& inputs = wrapper->getGuiInputs();
    NodePtr inp;
    if ( (inputNb >= 0) && ( inputNb < (int)inputs.size() ) ) {
        inp = inputs[inputNb].lock();
    }

    if (inp) {
        std::map<int, InputName>::iterator found = _inputNamesMap.find(inputNb);
        if ( found != _inputNamesMap.end() ) {
            found->second.input = inp;
            found->second.name = QString::fromUtf8( inp->getLabel().c_str() );
            if (_aInput) {
                const int idx = _aInput->itemIndex(found->second.name);
                if (idx != -1) {
                    _aInput->setItemText(idx, found->second.name);
                }
            }
            if (_bInput) {
                const int idx = _bInput->itemIndex(found->second.name);
                if (idx != -1) {
                    _bInput->setItemText(idx, found->second.name);
                }
            }
        } else {
            InputName entry;
            entry.input = inp;
            entry.name = QString::fromUtf8( inp->getLabel().c_str() );
            _inputNamesMap.insert( std::make_pair(inputNb, entry) );
            if (_aInput) {
                _aInput->addItem(entry.name);
            }
            if (_bInput) {
                _bInput->addItem(entry.name);
            }
            // First connected input auto-selects as A and starts a render.
            InspectorNode* inspector = dynamic_cast<InspectorNode*>(wrapper.get());
            int a = -1, b = -1;
            if (inspector) {
                inspector->getActiveInputs(a, b);
            }
            if ( (a == -1) && _aInput ) {
                const int idx = _aInput->itemIndex(entry.name);
                if (idx != -1) {
                    _aInput->setCurrentIndex(idx); // emits onFirstInputNameChanged
                }
            }
        }
    } else {
        std::map<int, InputName>::iterator found = _inputNamesMap.find(inputNb);
        if ( found != _inputNamesMap.end() ) {
            if (_aInput) {
                _aInput->blockSignals(true);
                _aInput->removeItem(found->second.name);
                _aInput->blockSignals(false);
            }
            if (_bInput) {
                _bInput->blockSignals(true);
                _bInput->removeItem(found->second.name);
                _bInput->blockSignals(false);
            }
            _inputNamesMap.erase(found);
        }
    }
}

void
ViewportVRTab::onInputNameChanged(int inputNb, const QString& name)
{
    std::map<int, InputName>::iterator found = _inputNamesMap.find(inputNb);

    if ( found == _inputNamesMap.end() ) {
        return;
    }
    if (_aInput) {
        const int idx = _aInput->itemIndex(found->second.name);
        if (idx != -1) {
            _aInput->setItemText(idx, name);
        }
    }
    if (_bInput) {
        const int idx = _bInput->itemIndex(found->second.name);
        if (idx != -1) {
            _bInput->setItemText(idx, name);
        }
    }
    found->second.name = name;
}

void
ViewportVRTab::onFirstInputNameChanged(const QString& text)
{
    if (!_viewerNode) {
        return;
    }
    int inputIndex = -1;
    for (std::map<int, InputName>::iterator it = _inputNamesMap.begin(); it != _inputNamesMap.end(); ++it) {
        if (it->second.name == text) {
            inputIndex = it->first;
            break;
        }
    }
    _viewerNode->setInputA(inputIndex);
    // A real input change must refresh: the previous frame came from another node.
    if (inputIndex != -1) {
        _viewerNode->forceFullComputationOnNextFrame();
        _viewerNode->renderCurrentFrame(true);
    } else if (_viewport) {
        _viewport->clearVRTextures();
    }
}

void
ViewportVRTab::onSecondInputNameChanged(const QString& text)
{
    if (!_viewerNode) {
        return;
    }
    int inputIndex = -1;
    for (std::map<int, InputName>::iterator it = _inputNamesMap.begin(); it != _inputNamesMap.end(); ++it) {
        if (it->second.name == text) {
            inputIndex = it->first;
            break;
        }
    }
    _viewerNode->setInputB(inputIndex);
    if (inputIndex != -1) {
        _viewerNode->forceFullComputationOnNextFrame();
        _viewerNode->renderCurrentFrame(true);
    }
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
ViewportVRTab::onViewportFovChanged(double fov)
{
    // Canvas-originated FOV change (wheel over the canvas, Reset): mirror it
    // into the spin box without re-emitting valueChanged back at the widget.
    if (_fovSpin) {
        _fovSpin->blockSignals(true);
        _fovSpin->setValue(fov);
        _fovSpin->blockSignals(false);
    }
}

void
ViewportVRTab::onResetView()
{
    if (_viewport) {
        _viewport->resetLook();
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
    if (_bInput) {
        _bInput->setEnabled_natron(op != eViewerCompositingOperatorNone);
    }
    QString label;
    switch (op) {
    case eViewerCompositingOperatorNone: label = QString::fromUtf8(" - "); break;
    case eViewerCompositingOperatorStackUnder: label = QString::fromUtf8("S-Under"); break;
    case eViewerCompositingOperatorStackMinus: label = QString::fromUtf8("S-Minus"); break;
    case eViewerCompositingOperatorStackOnionSkin: label = QString::fromUtf8("S-OnionSkin"); break;
    case eViewerCompositingOperatorWipeUnder: label = QString::fromUtf8("W-Under"); break;
    case eViewerCompositingOperatorWipeOver: label = QString::fromUtf8("W-Over"); break;
    case eViewerCompositingOperatorWipeMinus: label = QString::fromUtf8("W-Minus"); break;
    case eViewerCompositingOperatorWipeOnionSkin: label = QString::fromUtf8("W-OnionSkin"); break;
    default: label = QString::fromUtf8(" - "); break;
    }
    if (_compareDropdown) {
        _compareDropdown->setText(label);
    }
}

void
ViewportVRTab::onGainSpinChanged(double fstop)
{
    // Sliders/spin drive the same value; keep them + the toggle in sync.
    _lastFstop = fstop;
    if (!_gainOn) {
        _gainOn = true;
    }
    if (_gainSpin && (_gainSpin->value() != fstop)) {
        _gainSpin->blockSignals(true);
        _gainSpin->setValue(fstop);
        _gainSpin->blockSignals(false);
    }
    if (_gainSlider) {
        _gainSlider->seekScalePosition(fstop);
    }
    if (_viewerNode) {
        _viewerNode->onGainChanged( std::pow(2., fstop) );
    }
}

void
ViewportVRTab::onGammaSpinChanged(double gamma)
{
    _lastGamma = gamma;
    if (!_gammaOn) {
        _gammaOn = true;
    }
    if (_gammaSpin && (_gammaSpin->value() != gamma)) {
        _gammaSpin->blockSignals(true);
        _gammaSpin->setValue(gamma);
        _gammaSpin->blockSignals(false);
    }
    if (_gammaSlider) {
        _gammaSlider->seekScalePosition(gamma);
    }
    if (_viewerNode) {
        _viewerNode->onGammaChanged(gamma);
    }
}

void
ViewportVRTab::onGainToggled(bool on)
{
    // The viewer's f-stop toggle: off = neutral gain (1.0), on = last value.
    _gainOn = on;
    const double fstop = on ? _lastFstop : 0.;
    if (_gainSpin) {
        _gainSpin->blockSignals(true);
        _gainSpin->setValue(fstop);
        _gainSpin->blockSignals(false);
    }
    if (_gainSlider) {
        _gainSlider->seekScalePosition(fstop);
    }
    if (_viewerNode) {
        _viewerNode->onGainChanged( std::pow(2., fstop) );
    }
}

void
ViewportVRTab::onGammaToggled(bool on)
{
    // The viewer's gamma toggle: off = 1.0, on = last value.
    _gammaOn = on;
    const double gamma = on ? _lastGamma : 1.;
    if (_gammaSpin) {
        _gammaSpin->blockSignals(true);
        _gammaSpin->setValue(gamma);
        _gammaSpin->blockSignals(false);
    }
    if (_gammaSlider) {
        _gammaSlider->seekScalePosition(gamma);
    }
    if (_viewerNode) {
        _viewerNode->onGammaChanged(gamma);
    }
}

void
ViewportVRTab::onPlaybackModeClicked()
{
    if (!_viewerNode) {
        return;
    }
    // Cycle loop -> bounce -> once, like the 2D viewer's playback-mode button.
    RenderEnginePtr engine = _viewerNode->getRenderEngine();
    if (!engine) {
        return;
    }
    PlaybackModeEnum mode = engine->getPlaybackMode();
    mode = (PlaybackModeEnum)( ( (int)mode + 1 ) % 3 );
    engine->setPlaybackMode(mode);
    QPixmap pix;
    switch (mode) {
    case ePlaybackModeLoop:
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_LOOP_MODE, &pix);
        break;
    case ePlaybackModeBounce:
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_BOUNCE, &pix);
        break;
    case ePlaybackModeOnce:
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PLAY_ONCE, &pix);
        break;
    default:
        break;
    }
    if (_playModeBtn && !pix.isNull()) {
        _playModeBtn->setIcon( QIcon(pix) );
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
        _rangeIn->blockSignals(true);
        _rangeIn->setValue( firstFrame );
        _rangeIn->blockSignals(false);
    }
    if (_rangeOut) {
        _rangeOut->blockSignals(true);
        _rangeOut->setValue( lastFrame );
        _rangeOut->blockSignals(false);
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
ViewportVRTab::onPlaybackInButtonClicked()
{
    // Set the in point at the current frame (the viewer's [ button).
    TimeLinePtr timeline = getTimeline();
    if (!timeline || !_rangeIn || !_rangeOut) {
        return;
    }
    double in = (double)timeline->currentFrame();
    if (in > _rangeOut->value()) {
        in = _rangeOut->value();
    }
    _rangeIn->setValue(in); // onRangeInChanged propagates to viewport + timeline
}

void
ViewportVRTab::onPlaybackOutButtonClicked()
{
    // Set the out point at the current frame (the viewer's ] button).
    TimeLinePtr timeline = getTimeline();
    if (!timeline || !_rangeIn || !_rangeOut) {
        return;
    }
    double out = (double)timeline->currentFrame();
    if (out < _rangeIn->value()) {
        out = _rangeIn->value();
    }
    _rangeOut->setValue(out); // onRangeOutChanged propagates to viewport + timeline
}

void
ViewportVRTab::onRangeInChanged(double value)
{
    if (_viewport && _rangeOut) {
        _viewport->setVRPlaybackRange( (int)value, (int)_rangeOut->value() );
    }
    if (_timelineGui && _rangeOut) {
        _timelineGui->setBoundaries( (SequenceTime)value, (SequenceTime)_rangeOut->value() );
    }
}

void
ViewportVRTab::onRangeOutChanged(double value)
{
    if (_viewport && _rangeIn) {
        _viewport->setVRPlaybackRange( (int)_rangeIn->value(), (int)value );
    }
    if (_timelineGui && _rangeIn) {
        _timelineGui->setBoundaries( (SequenceTime)_rangeIn->value(), (SequenceTime)value );
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
        Gui* gui = getGui();
        if (!gui) {
            return;
        }
        GuiAppInstancePtr app = gui->getApp();
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
        // Stop: pure user intent — the button carries the state; abort the
        // engine's playback. (Queued scrub-refresh renders still re-render the
        // current frame once; that is the 2D viewer's behavior too.)
        _viewerNode->getRenderEngine()->abortRenderingNoRestart();
    }
}

void
ViewportVRTab::onViewportLookChanged()
{
    // Deliberately NOT re-rendering on look changes: with full-pano rendering
    // the look does not change the frame, and the continuous re-render was
    // stampeding the engine queue (the perceived "sticky" play/stop during
    // drags). The look only concerns the sampler now.
    _lastLookRefreshValid = false;
}

void
ViewportVRTab::onLookRefreshTimeout()
{
    if (_lastLookRefreshValid && _viewerNode) {
        _lastLookRefreshValid = false;
        _viewerNode->renderCurrentFrame(true);
    }
}

void
ViewportVRTab::onEngineStarted(bool forward)
{
    // Deliberately does NOT touch the play button: the button is pure user
    // intent. ViewerTab's engine-signal→button feedback loop made Stop feel
    // stuck (queued renders re-signal started and re-launch playback, while
    // machine-driven setChecked re-enters toggled and re-aborts).
    Q_UNUSED(forward);
}

void
ViewportVRTab::onEngineStopped(int)
{
    // Same policy: the button follows the user only. If the engine ran out of
    // frames (playback "once" mode), the button stays down; the next Play
    // press aborts & re-requests, which is also ViewerTab's practical flow.
}

void
ViewportVRTab::onFrameChanged(SequenceTime frame, int)
{
    if (_frameSpin) {
        _frameSpin->blockSignals(true);
        _frameSpin->setValue((double)frame);
        _frameSpin->blockSignals(false);
    }
    // Scrub = re-render at the new frame — but NEVER while a render is already
    // working: the engine advances the timeline itself during playback, and
    // re-posting a canAbort render request per tick aborted the playback right
    // after its first frame. When nothing is running (plain scrub), the posted
    // request renders the new frame.
    if (_viewerNode) {
        RenderEnginePtr engine = _viewerNode->getRenderEngine();
        if ( engine && !engine->hasThreadsWorking() ) {
            _viewerNode->renderCurrentFrame(true);
        }
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

