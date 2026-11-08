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
#include <QCheckBox>
#include <QFrame>
#include <QMenu>
#include <QAction>
#include <QKeyEvent>
#include <QDateTime>
#include <QTimer>
#include <QDoubleSpinBox>
#include <QDebug>

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/Button.h"
#include "Gui/ComboBox.h"
#include "Gui/GuiDefines.h"
#include "Gui/InfoViewerWidget.h"
#include "Gui/RenderStatsDialog.h"
#include "Gui/ScaleSliderQWidget.h"
#include "Gui/SpinBox.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewportVR.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImagePlaneDesc.h"
#include "Engine/Node.h" // InspectorNode
#include "Engine/TimeLine.h"
#include "Engine/Project.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ViewerInstance.h"
#include "Engine/OutputSchedulerThread.h" // RenderEngine
#include <set>

NATRON_NAMESPACE_ENTER

/** Group separator between the toolbar control clusters (the 2D viewer's
 *  firstSettingsRow look: 5px | vline | 5px). */
static void
addVRGroupSpacer(QBoxLayout* layout)
{
    layout->addSpacing(5);
    QFrame* line = new QFrame( layout->parentWidget() );
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Raised);
    QPalette palette;
    palette.setColor(QPalette::WindowText, Qt::black);
    line->setPalette(palette);
    layout->addWidget(line);
    layout->addSpacing(5);
}

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
    , _layerChoice(NULL)
    , _alphaChannelChoice(NULL)
    , _fullFrameBtn(NULL)
    , _refreshBtn(NULL)
    , _fpsLock(NULL)
    , _timeFormatCombo(NULL)
    , _fpsSpin(NULL)
    , _frameSpin(NULL)
    , _rangeIn(NULL)
    , _rangeOut(NULL)
    , _incrSpin(NULL)
    , _fpsLabel(NULL)
    , _infoViewerB(NULL)
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
    // DPI-aware metrics used to size the text dropdowns like the 2D viewer's.
    const QFontMetrics fm(font(), 0);

    // ==================== Top toolbars (two viewer-style display rows) ======
    // Same split as the 2D viewer's firstSettingsRow / secondSettingsRow:
    // row 1 = display + inputs (channels, A/B, FOV, recenter, pause, ROI,
    // proxy, compare); row 2 = gain / gamma sliders + the VR packing options.
    QWidget* toolbar = new QWidget(this);
    {
        QSizePolicy sp = toolbar->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Ignored);
        toolbar->setSizePolicy(sp);
    }
    QVBoxLayout* toolbarVBoxLayout = new QVBoxLayout(toolbar);
    toolbarVBoxLayout->setContentsMargins(0, 0, 0, 0);
    toolbarVBoxLayout->setSpacing(0);

    QWidget* row1 = new QWidget(toolbar);
    row1->setFixedHeight(28);
    QHBoxLayout* toolbarLayout = new QHBoxLayout(row1);
    toolbarLayout->setContentsMargins(4, 2, 4, 2);
    toolbarLayout->setSpacing(4);

    QWidget* row2 = new QWidget(toolbar);
    row2->setFixedHeight(28);
    QHBoxLayout* toolbarLayout2 = new QHBoxLayout(row2);
    toolbarLayout2->setContentsMargins(4, 2, 4, 2);
    toolbarLayout2->setSpacing(6);

    // ---- Layer + Alpha channel (the 2D viewer's first two fields) ----------
    {
        _layerChoice = new ComboBox(toolbar);
        _layerChoice->setToolTip( QString::fromUtf8("<p><b>") + tr("Layer:") + QString::fromUtf8("</b></p><p>")
                                  + tr("The layer that the Viewer VR node will fetch upstream in the tree. "
                                       "The channels of the layer will be mapped to the RGBA channels of the viewer according to "
                                       "its number of channels. (e.g: UV would be mapped to RG)") + QString::fromUtf8("</p>") );
        _layerChoice->setFixedWidth( fm.horizontalAdvance( QString::fromUtf8("Color.Toto.RGBA") ) + 3 * TO_DPIX(DROP_DOWN_ICON_SIZE) );
        _layerChoice->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _layerChoice->addItem( QString::fromUtf8("-") );
        connect(_layerChoice, SIGNAL(currentIndexChanged(int)), this, SLOT(onLayerComboChanged(int)));
        toolbarLayout->addWidget(_layerChoice);

        _alphaChannelChoice = new ComboBox(toolbar);
        _alphaChannelChoice->setToolTip( QString::fromUtf8("<p><b>") + tr("Alpha channel:") + QString::fromUtf8("</b></p><p>")
                                         + tr("Select here a channel of any layer that will be used when displaying the "
                                              "alpha channel with the <b>Channels</b> choice on the right.") + QString::fromUtf8("</p>") );
        _alphaChannelChoice->setFixedWidth( fm.horizontalAdvance( QString::fromUtf8("Color.alpha") ) + 3 * TO_DPIX(DROP_DOWN_ICON_SIZE) );
        _alphaChannelChoice->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _alphaChannelChoice->addItem( QString::fromUtf8("-") );
        connect(_alphaChannelChoice, SIGNAL(currentIndexChanged(int)), this, SLOT(onAlphaChannelComboChanged(int)));
        toolbarLayout->addWidget(_alphaChannelChoice);
    }

    // ---- Display channels (the viewer's RGB dropdown: RGB/R/G/B/A/Y/Matte) --
    {
        _channelsDropdown = new ComboBox(toolbar);
        _channelsDropdown->setToolTip(QString::fromUtf8(
            "Channels: RGB / Red / Green / Blue / Alpha / Luminance / Matte — the "
            "same display channels the 2D viewer offers, rendered by the viewer "
            "pipeline."));
        _channelsDropdown->setFixedWidth( fm.horizontalAdvance( QString::fromUtf8("Luminance") ) + 3 * TO_DPIX(DROP_DOWN_ICON_SIZE) );
        _channelsDropdown->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _channelsDropdown->addItem( QString::fromUtf8("Luminance") );
        _channelsDropdown->addItem( QString::fromUtf8("RGB") );
        _channelsDropdown->addItem( QString::fromUtf8("Red") );
        _channelsDropdown->addItem( QString::fromUtf8("Green") );
        _channelsDropdown->addItem( QString::fromUtf8("Blue") );
        _channelsDropdown->addItem( QString::fromUtf8("Alpha") );
        _channelsDropdown->addItem( QString::fromUtf8("Matte") );
        _channelsDropdown->setCurrentIndex_no_emit(1);
        connect(_channelsDropdown, SIGNAL(currentIndexChanged(int)), this, SLOT(onDisplayChannelsSelected(int)));
        toolbarLayout->addWidget(_channelsDropdown);
    }

    addVRGroupSpacer(toolbarLayout);

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

    // ---- Recenter (the viewer's center button) — recenter + reset FOV. The
    // full-frame (cogs) / ROI / proxy cluster and the refresh/pause group
    // follow in the same block.
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

    addVRGroupSpacer(toolbarLayout);

    // ---- Full-frame processing (the 2D viewer's "cogs" button) -------------
    // TEMPORARILY DISABLED like the ROI button: the whole pano is always
    // rendered. The button stays visible but grayed out / unpressable.
    {
        QPixmap pixFfOn, pixFfOff;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_FULL_FRAME_ON, &pixFfOn);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_FULL_FRAME_OFF, &pixFfOff);
        _fullFrameBtn = new Button(toolbar);
        if ( !pixFfOn.isNull() ) {
            QIcon icFf;
            icFf.addPixmap(pixFfOn, QIcon::Normal, QIcon::On);
            icFf.addPixmap(pixFfOff, QIcon::Normal, QIcon::Off);
            _fullFrameBtn->setIcon(icFf);
        } else {
            _fullFrameBtn->setText(QString::fromUtf8("FF"));
        }
        _fullFrameBtn->setToolTip( Qt::convertFromPlainText(tr("When checked, the VR panorama is always rendered in its entirety; when unchecked, only the part of the image visible through the current FOV is processed. Currently disabled: the whole panorama is always rendered."), Qt::WhiteSpaceNormal) );
        _fullFrameBtn->setFocusPolicy(Qt::NoFocus);
        _fullFrameBtn->setFixedSize(btnSize);
        _fullFrameBtn->setIconSize(btnIconSize);
        _fullFrameBtn->setEnabled(false); // grayed out, unpressable
        toolbarLayout->addWidget(_fullFrameBtn);
    }

    // ---- ROI windowing toggle — temporarily disabled -----------------------
    // The sub-window render path is being reworked; the whole pano is always
    // rendered. The button stays visible but grayed out / unpressable.
    {
        QPixmap pixRoiOn, pixRoiOff;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_ROI_ENABLED, &pixRoiOn);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_ROI_DISABLED, &pixRoiOff);
        _roiBtn = new Button(toolbar);
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
            "FOV (like the 2D viewer's Region of Interest): currently disabled, "
            "the whole panorama is always rendered."));
        _roiBtn->setFixedSize(btnSize);
        _roiBtn->setIconSize(btnIconSize);
        _roiBtn->setEnabled(false); // grayed out, unpressable
        toolbarLayout->addWidget(_roiBtn);
    }

    // ---- proxy group (viewer render-scale activate + scale combo) ---------
    {
        QPixmap pixRs, pixRsChecked;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_RENDER_SCALE, &pixRs);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_RENDER_SCALE_CHECKED, &pixRsChecked);
        _proxyBtn = new Button(toolbar);
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

        _proxyScaleDropdown = new ComboBox(toolbar);
        _proxyScaleDropdown->setToolTip(QString::fromUtf8("Proxy scale factor."));
        _proxyScaleDropdown->setFixedHeight(22);
        const int factors[5] = { 2, 4, 8, 16, 32 };
        for (int i = 0; i < 5; ++i) {
            _proxyScaleDropdown->addItem( QString::number(factors[i]) );
        }
        _proxyScaleDropdown->setCurrentIndex_no_emit(0);
        connect(_proxyScaleDropdown, SIGNAL(currentIndexChanged(int)), this, SLOT(onProxyScaleChanged(int)));
        toolbarLayout->addWidget(_proxyScaleDropdown);
    }

    addVRGroupSpacer(toolbarLayout);

    // ---- Refresh (re-render current frame) + Pause updates group -----------
    {
        QPixmap pixRefresh;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_REFRESH, &pixRefresh);
        _refreshBtn = new Button(toolbar);
        if ( !pixRefresh.isNull() ) {
            _refreshBtn->setIcon( QIcon(pixRefresh) );
        } else {
            _refreshBtn->setText(QString::fromUtf8("R"));
        }
        _refreshBtn->setToolTip( QString::fromUtf8("<p>") + tr("Forces a new render of the current frame.") + QString::fromUtf8("</p>") );
        _refreshBtn->setFocusPolicy(Qt::NoFocus);
        _refreshBtn->setFixedSize(btnSize);
        _refreshBtn->setIconSize(btnIconSize);
        connect(_refreshBtn, SIGNAL(clicked()), this, SLOT(onRefreshClicked()));
        toolbarLayout->addWidget(_refreshBtn);
    }

    // ---- Pause updates (the viewer's pause, viewerNode->setViewerPaused) ----
    {
        QPixmap pixPauseOn, pixPauseOff;
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PAUSE_ENABLED, &pixPauseOn);
        appPTR->getIcon(NATRON_PIXMAP_PLAYER_PAUSE_DISABLED, &pixPauseOff);
        _pauseBtn = new Button(toolbar);
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

    addVRGroupSpacer(toolbarLayout);

    // ---- Comparison (A/B) group at the end of the row, exactly like the 2D --
    // ---- viewer: A: [input] [merge icon] [op] B: [input].              ----
    {
        _aLabel = new QLabel(QString::fromUtf8("A:"), toolbar);
        _aLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(_aLabel);
        _aInput = new ComboBox(toolbar);
        _aInput->addItem( QString::fromUtf8(" - ") );
        // Fixed width sized for a plausible node label so the fields stay
        // readable when the pane narrows (the 2D viewer's A:/B: sizing).
        _aInput->setFixedWidth( fm.horizontalAdvance( QString::fromUtf8("ColorCorrect1") ) + 3 * TO_DPIX(DROP_DOWN_ICON_SIZE) );
        _aInput->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _aInput->setToolTip(QString::fromUtf8(
            "Input A: pick which node the VR viewport renders. The list fills "
            "with the nodes connected to the Viewer VR node in the graph."));
        QObject::connect( _aInput, SIGNAL(currentIndexChanged(QString)), this, SLOT(onFirstInputNameChanged(QString)) );
        toolbarLayout->addWidget(_aInput);
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

        _compareDropdown = new ComboBox(toolbar);
        _compareDropdown->setToolTip(QString::fromUtf8(
            "Comparison between inputs of the Viewer VR node: Difference / "
            "Minus / Onion-skin. The 2D viewer's A/B machinery renders both "
            "inputs; the VR sampler applies the operation."));
        _compareDropdown->setFixedWidth( fm.horizontalAdvance( QString::fromUtf8("W-OnionSkin") ) + 3 * TO_DPIX(DROP_DOWN_ICON_SIZE) );
        _compareDropdown->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _compareDropdown->addItem( QString::fromUtf8(" - ") );
        _compareDropdown->addItem( QString::fromUtf8("W-Under") );
        _compareDropdown->addItem( QString::fromUtf8("W-Over") );
        _compareDropdown->addItem( QString::fromUtf8("W-Minus") );
        _compareDropdown->addItem( QString::fromUtf8("W-OnionSkin") );
        _compareDropdown->addItem( QString::fromUtf8("S-Under") );
        _compareDropdown->addItem( QString::fromUtf8("S-Over") );
        _compareDropdown->addItem( QString::fromUtf8("S-Minus") );
        _compareDropdown->addItem( QString::fromUtf8("S-OnionSkin") );
        _compareDropdown->setCurrentIndex_no_emit(0);
        connect(_compareDropdown, SIGNAL(currentIndexChanged(int)), this, SLOT(onCompareOperatorSelected(int)));
        toolbarLayout->addWidget(_compareDropdown);
    }

    // ---- Input B selector (the viewer's "B:" combobox; feeds the compare) --
    {
        _bLabel = new QLabel(QString::fromUtf8("B:"), toolbar);
        _bLabel->setStyleSheet(QString::fromUtf8("color: rgb(150, 150, 150);"));
        toolbarLayout->addWidget(_bLabel);
        _bInput = new ComboBox(toolbar);
        _bInput->addItem( QString::fromUtf8(" - ") );
        _bInput->setFixedWidth( fm.horizontalAdvance( QString::fromUtf8("ColorCorrect1") ) + 3 * TO_DPIX(DROP_DOWN_ICON_SIZE) );
        _bInput->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        _bInput->setEnabled_natron(false);
        _bInput->setToolTip(QString::fromUtf8(
            "Input B: the second image for the A/B comparison (enabled once a "
            "comparison operator is picked)."));
        QObject::connect( _bInput, SIGNAL(currentIndexChanged(QString)), this, SLOT(onSecondInputNameChanged(QString)) );
        toolbarLayout->addWidget(_bInput);
    }

    toolbarLayout->addStretch();
    toolbarVBoxLayout->addWidget(row1);

    // ==================== Row 2: gain / gamma + VR packing ==================
    // ---- Gain / Gamma (the viewer's f-stop boxes + scale sliders) ----------
    {
        QPixmap pixGainOn, pixGainOff;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAIN_ENABLED, &pixGainOn);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAIN_DISABLED, &pixGainOff);
        QToolButton* gainToggle = new QToolButton(row2);
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
        toolbarLayout2->addWidget(gainToggle);

        _gainSpin = new QDoubleSpinBox(row2);
        _gainSpin->setRange(-6., 6.);
        _gainSpin->setSingleStep(0.25);
        _gainSpin->setDecimals(2);
        _gainSpin->setValue(0.);
        _gainSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _gainSpin->setFixedWidth(56);
        _gainSpin->setFixedHeight(22);
        _gainSpin->setToolTip(QString::fromUtf8("Viewer gain in f-stops (gain = 2^x)."));
        connect(_gainSpin, SIGNAL(valueChanged(double)), this, SLOT(onGainSpinChanged(double)));
        toolbarLayout2->addWidget(_gainSpin);

        _gainSlider = new ScaleSliderQWidget(-6., 6., 0., false, ScaleSliderQWidget::eDataTypeDouble, gui, eScaleTypeLinear, row2);
        _gainSlider->setMinimumHeight(22);
        _gainSlider->setMaximumWidth(110);
        _gainSlider->setToolTip(QString::fromUtf8("Viewer gain in f-stops (drag; gain = 2^x)."));
        QObject::connect( _gainSlider, SIGNAL(positionChanged(double)), this, SLOT(onGainSpinChanged(double)) );
        toolbarLayout2->addWidget(_gainSlider);

        QPixmap pixGammaOn, pixGammaOff;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAMMA_ENABLED, &pixGammaOn);
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_GAMMA_DISABLED, &pixGammaOff);
        QToolButton* gammaToggle = new QToolButton(row2);
        if ( !pixGammaOn.isNull() ) {
            QIcon icGamma;
            icGamma.addPixmap(pixGammaOff, QIcon::Normal, QIcon::Off);
            icGamma.addPixmap(pixGammaOn, QIcon::Normal, QIcon::On);
            gammaToggle->setIcon(icGamma);
        } else {
            gammaToggle->setText(QString::fromUtf8("Î³"));
        }
        gammaToggle->setCheckable(true);
        gammaToggle->setChecked(false);
        gammaToggle->setFixedSize(btnSize);
        gammaToggle->setIconSize(btnIconSize);
        gammaToggle->setToolTip(QString::fromUtf8("Gamma: switch between the neutral gamma (1.0) and the last value used."));
        connect(gammaToggle, SIGNAL(toggled(bool)), this, SLOT(onGammaToggled(bool)));
        toolbarLayout2->addWidget(gammaToggle);

        _gammaSpin = new QDoubleSpinBox(row2);
        _gammaSpin->setRange(0.05, 10.);
        _gammaSpin->setSingleStep(0.05);
        _gammaSpin->setDecimals(2);
        _gammaSpin->setValue(1.0);
        _gammaSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        _gammaSpin->setFixedWidth(56);
        _gammaSpin->setFixedHeight(22);
        _gammaSpin->setToolTip(QString::fromUtf8("Viewer gamma (the 2D viewer's Gamma)."));
        connect(_gammaSpin, SIGNAL(valueChanged(double)), this, SLOT(onGammaSpinChanged(double)));
        toolbarLayout2->addWidget(_gammaSpin);

        _gammaSlider = new ScaleSliderQWidget(0., 4., 1., false, ScaleSliderQWidget::eDataTypeDouble, gui, eScaleTypeLinear, row2);
        _gammaSlider->setMinimumHeight(22);
        _gammaSlider->setMaximumWidth(110);
        _gammaSlider->setToolTip(QString::fromUtf8("Viewer gamma (drag)."));
        QObject::connect( _gammaSlider, SIGNAL(positionChanged(double)), this, SLOT(onGammaSpinChanged(double)) );
        toolbarLayout2->addWidget(_gammaSlider);
    }

    // ---- Color transform (the 2D viewer's colorspace combo, row 2) ---------
    {
        _colorSpaceCombo = new ComboBox(row2);
        _colorSpaceCombo->setToolTip( QString::fromUtf8( "<p><b>") + tr("Viewer color process:") + QString::fromUtf8("</b></p><p>") + tr(
                                          "The operation applied to the image before it is displayed "
                                          "on screen. All the color pipeline "
                                          "is linear, thus the process converts from linear "
                                          "to your monitor's colorspace.") + QString::fromUtf8("</p>") );
        toolbarLayout2->addWidget(_colorSpaceCombo);

        _colorSpaceCombo->addItem( QString::fromUtf8("Linear(None)") );
        _colorSpaceCombo->addItem( QString::fromUtf8("sRGB") );
        _colorSpaceCombo->addItem( QString::fromUtf8("Rec.709") );
        _colorSpaceCombo->addItem( QString::fromUtf8("BT1886") );
        // Append the OCIO config's display/view transforms (e.g. ACES Output
        // Transforms) after the built-in colorspaces, exactly like ViewerTab's.
        const std::vector<std::string> ocioChoices = ViewerInstance::getOcioDisplayViewChoices();
        for (std::size_t i = 0; i < ocioChoices.size(); ++i) {
            const std::string& choice = ocioChoices[i];
            const std::string sep = " / ";
            const std::size_t pos = choice.find(sep);
            std::string display, view;
            if (pos != std::string::npos) {
                display = choice.substr(0, pos);
                view = choice.substr(pos + sep.size());
            } else {
                display = choice;
            }
            _ocioDisplayViews.push_back( std::make_pair(display, view) );
            _colorSpaceCombo->addItem( QString::fromUtf8( choice.c_str() ) );
        }
        _colorSpaceCombo->setCurrentIndex_no_emit(1); // sRGB
        connect(_colorSpaceCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onColorSpaceSelected(int)));
    }

    addVRGroupSpacer(toolbarLayout2);
    // ---- VR-specific: stereo packing + eye (VR settings group) ------------
    {
        _layoutDropdown = new QToolButton(row2);
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
        toolbarLayout2->addWidget(_layoutDropdown);
    }

    {
        _eyeDropdown = new QToolButton(row2);
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
        toolbarLayout2->addWidget(_eyeDropdown);
    }

    toolbarLayout2->addStretch();
    toolbarVBoxLayout->addWidget(row2);
    mainLayout->addWidget(toolbar);

    // ==================== VR canvas ====================
    _viewport = new ViewportVR(gui);
    mainLayout->addWidget(_viewport, 1); // stretch factor 1 = takes all remaining space
    if (_viewerNode) {
        // Engine -> canvas: the same wiring ViewerTab does between ViewerGL
        // and the ViewerInstance (OpenGLViewerI contract).
        _viewerNode->setUiContext( (OpenGLViewerI*)_viewport );
    }

    // ==================== Image info rows (the 2D viewer's InfoViewerWidget)
    // channels/format, resolution, RoD, pixel coordinates and the RGBA +
    // HSV(L) of the pixel under the cursor — the canvas fills them (below).
    // Two bars like the 2D viewer: "A:" first, "B:" under it (compare mode
    // only).
    _infoViewer = new InfoViewerWidget( QString::fromUtf8("A:"), this );
    mainLayout->addWidget(_infoViewer);
    _infoViewerB = new InfoViewerWidget( QString::fromUtf8("B:"), this );
    mainLayout->addWidget(_infoViewerB);
    _infoViewerB->hide();
    _viewport->setInfoViewer(_infoViewer);
    _viewport->setInfoViewerB(_infoViewerB);

    // ==================== Timeline Scrubber (above the player, 2D order) ====
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
                mainLayout->addSpacing( TO_DPIY(5) );
                mainLayout->addWidget(_timelineGui);
                connect(timeline.get(), SIGNAL(frameChanged(SequenceTime,int)),
                        this, SLOT(onFrameChanged(SequenceTime,int)));
                // Time-format display (TC/TF), wired like the 2D viewer's player.
                if (_timeFormatCombo) {
                    QObject::connect( _timeFormatCombo, SIGNAL(currentIndexChanged(int)),
                                      _timelineGui, SLOT(onTimeFormatChanged(int)) );
                }
                // Cache color marks on the timeline, like the 2D viewer's: the
                // render engine populates the shared viewer cache; these slots
                // just repaint the marks.
                _timelineGui->connectSlotsToViewerCache();
                // Like the 2D viewer (widget must be laid out first).
                QTimer::singleShot( 25, _timelineGui, SLOT(recenterOnBounds()) );
                _timelineGui->setFrameRangeEdited(false);
                connect( app->getProject().get(), SIGNAL(frameRangeChanged(int,int)), _timelineGui,
                         SLOT(onProjectFrameRangeChanged(int,int)) );
                // The TimeLineGui's own slot early-returns (NULL ViewerTab);
                // mirror the project range here so a reader connected later
                // extends the VR timeline + playback bounds (default 1..250).
                connect( app->getProject().get(), SIGNAL(frameRangeChanged(int,int)),
                         this, SLOT(onProjectFrameRangeChanged(int,int)) );
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

    // ==================== Player bar (the 2D viewer's player row, verbatim) =
    // Same widgets, icons, order and spacing as ViewerTab's playerButtonsContainer:
    // [in] [in] ---- |fps-lock fps: fps TC/TF turbo loop| ---- [first][rewind]
    // [frame][play][last] |prev next| |prevKF nextKF| |prevIncr incr nextIncr|
    // ---- [out][out]
    {
        QWidget* playerBar = new QWidget(this);
        playerBar->setFixedHeight(28);
        {
            QSizePolicy sp = playerBar->sizePolicy();
            sp.setHorizontalPolicy(QSizePolicy::Ignored);
            playerBar->setSizePolicy(sp);
        }
        QHBoxLayout* playerLayout = new QHBoxLayout(playerBar);
        playerLayout->setContentsMargins(0, 0, 0, 0);
        playerLayout->setSpacing(0);

        // Playback in-point: [ button + in spin (bottom-left of the 2D player)
        _setInBtn = new Button(playerBar);
        _setInBtn->setFocusPolicy(Qt::NoFocus);
        _setInBtn->setFixedSize(btnSize);
        _setInBtn->setIconSize(btnIconSize);
        {
            QPixmap pixIn;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_TIMELINE_IN, &pixIn);
            if ( !pixIn.isNull() ) {
                _setInBtn->setIcon( QIcon(pixIn) );
            } else {
                _setInBtn->setText(QString::fromUtf8("["));
            }
        }
        _setInBtn->setToolTip(QString::fromUtf8("Set the playback in point at the current frame"));
        connect(_setInBtn, SIGNAL(clicked()), this, SLOT(onPlaybackInButtonClicked()));
        playerLayout->addWidget(_setInBtn);

        _rangeIn = new SpinBox(playerBar, SpinBox::eSpinBoxTypeInt);
        _rangeIn->setValue(1);
        _rangeIn->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
        _rangeIn->setToolTip(QString::fromUtf8("The playback in point"));
        playerLayout->addWidget(_rangeIn);

        playerLayout->addStretch();

        // fps lock + label + fps spin + time format + turbo + playback mode
        _fpsLock = new QCheckBox(playerBar);
        _fpsLock->setFixedSize(btnSize);
        _fpsLock->setChecked(true);
        _fpsLock->setToolTip(QString::fromUtf8(
            "When unchecked, the playback frame rate is automatically set from the "
            "Viewer A input. When checked, the user setting is used."));
        connect(_fpsLock, SIGNAL(clicked(bool)), this, SLOT(onFpsLockToggled(bool)));
        playerLayout->addWidget(_fpsLock);
        playerLayout->addSpacing( TO_DPIX(5) );

        _fpsLabel = new QLabel(QString::fromUtf8("fps:"), playerBar);
        _fpsLabel->setToolTip(_fpsLock->toolTip());
        playerLayout->addWidget(_fpsLabel);

        _fpsSpin = new SpinBox(playerBar, SpinBox::eSpinBoxTypeDouble);
        _fpsSpin->decimals(1);
        _fpsSpin->setValue(24.);
        _fpsSpin->setIncrement(0.1);
        _fpsSpin->setToolTip(QString::fromUtf8("<p><b>fps:</b></p>Viewer playback framerate, in frames per second."));
        connect(_fpsSpin, SIGNAL(valueChanged(double)), this, SLOT(onFpsSpinChanged(double)));
        playerLayout->addWidget(_fpsSpin);

        _timeFormatCombo = new ComboBox(playerBar);
        _timeFormatCombo->addItem( tr("TC"), QIcon(), QKeySequence(), tr("Timecode") );
        _timeFormatCombo->addItem( tr("TF"), QIcon(), QKeySequence(), tr("Timeline Frames") );
        _timeFormatCombo->setCurrentIndex_no_emit(1);
        _timeFormatCombo->setToolTip( QString::fromUtf8("Set the time display format.") );
        {
            const QSize timeFormatSize( TO_DPIX(NATRON_LARGE_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
            _timeFormatCombo->setFixedSize(timeFormatSize);
        }
        playerLayout->addWidget(_timeFormatCombo);

        // Turbo (GUI freeze during playback): the application-wide toggle.
        {
            QPixmap pixFreezeEnabled, pixFreezeDisabled;
            appPTR->getIcon(NATRON_PIXMAP_FREEZE_ENABLED, &pixFreezeEnabled);
            appPTR->getIcon(NATRON_PIXMAP_FREEZE_DISABLED, &pixFreezeDisabled);
            QIcon icFreeze;
            icFreeze.addPixmap(pixFreezeEnabled, QIcon::Normal, QIcon::On);
            icFreeze.addPixmap(pixFreezeDisabled, QIcon::Normal, QIcon::Off);
            _turboBtn = new Button(icFreeze, QString(), playerBar);
            _turboBtn->setCheckable(true);
            _turboBtn->setChecked(false);
            _turboBtn->setDown(false);
            _turboBtn->setFixedSize(btnSize);
            _turboBtn->setIconSize(btnIconSize);
            _turboBtn->setToolTip(QString::fromUtf8(
                "<p><b>Turbo mode:</b></p><p>When checked, only the viewer is "
                "redrawn during playback, for maximum efficiency.</p>"));
            _turboBtn->setFocusPolicy(Qt::NoFocus);
            if (getGui()) {
                connect(_turboBtn, SIGNAL(clicked(bool)), getGui(), SLOT(onFreezeUIButtonClicked(bool)));
            }
            playerLayout->addWidget(_turboBtn);
        }

        // Playback mode: loop / bounce / once (engine's setPlaybackMode).
        _playModeBtn = new Button(playerBar);
        _playModeBtn->setFocusPolicy(Qt::NoFocus);
        _playModeBtn->setFixedSize(btnSize);
        _playModeBtn->setIconSize(btnIconSize);
        {
            QPixmap pixLoop;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_LOOP_MODE, &pixLoop);
            if ( !pixLoop.isNull() ) {
                _playModeBtn->setIcon( QIcon(pixLoop) );
            } else {
                _playModeBtn->setText(QString::fromUtf8("L"));
            }
        }
        _playModeBtn->setToolTip(QString::fromUtf8("Behaviour when the playback hits the end of the range: loop / bounce / stop."));
        connect(_playModeBtn, SIGNAL(clicked()), this, SLOT(onPlaybackModeClicked()));
        playerLayout->addWidget(_playModeBtn);

        playerLayout->addSpacing( TO_DPIX(10) );

        playerLayout->addStretch();

        // Transport cluster: first / rewind / current frame / play / last
        Button* firstBtn = new Button(playerBar);
        firstBtn->setFocusPolicy(Qt::NoFocus);
        firstBtn->setFixedSize(btnSize);
        firstBtn->setIconSize(btnIconSize);
        {
            QPixmap pixFirst;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_FIRST_FRAME, &pixFirst);
            if ( !pixFirst.isNull() ) {
                firstBtn->setIcon( QIcon(pixFirst) );
            }
        }
        firstBtn->setToolTip(QString::fromUtf8("Go to first frame (Home)"));
        connect(firstBtn, SIGNAL(clicked()), this, SLOT(onSeekFirst()));
        playerLayout->addWidget(firstBtn);

        _playBwdBtn = new Button(playerBar);
        _playBwdBtn->setFocusPolicy(Qt::NoFocus);
        _playBwdBtn->setFixedSize(btnSize);
        _playBwdBtn->setIconSize(btnIconSize);
        {
            QPixmap pixRewind, pixStop2;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_REWIND_DISABLED, &pixRewind);
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_STOP_ENABLED, &pixStop2);
            if ( !pixRewind.isNull() ) {
                QIcon icRewind;
                icRewind.addPixmap(pixStop2, QIcon::Normal, QIcon::On);
                icRewind.addPixmap(pixRewind, QIcon::Normal, QIcon::Off);
                _playBwdBtn->setIcon(icRewind);
            } else {
                _playBwdBtn->setText(QString::fromUtf8("——"));
            }
        }
        _playBwdBtn->setCheckable(true);
        _playBwdBtn->setDown(false);
        _playBwdBtn->setToolTip(QString::fromUtf8("Play backward through the viewer render engine (their stop is Space)"));
        connect(_playBwdBtn, SIGNAL(toggled(bool)), this, SLOT(onPlayBackwardToggled(bool)));
        playerLayout->addWidget(_playBwdBtn);

        _frameSpin = new SpinBox(playerBar, SpinBox::eSpinBoxTypeInt);
        _frameSpin->setValue(0);
        _frameSpin->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
        _frameSpin->setToolTip(QString::fromUtf8("Current frame number"));
        connect(_frameSpin, SIGNAL(valueChanged(double)), this, SLOT(onFrameSpinChanged(double)));
        playerLayout->addWidget(_frameSpin);

        _playBtn = new Button(playerBar);
        _playBtn->setFocusPolicy(Qt::NoFocus);
        _playBtn->setFixedSize(btnSize);
        _playBtn->setIconSize(btnIconSize);
        _playBtn->setCheckable(true);
        _playBtn->setDown(false);
        {
            QPixmap pixPlayDisabled, pixStop;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_PLAY_DISABLED, &pixPlayDisabled);
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_STOP_ENABLED, &pixStop);
            QIcon icPlay;
            icPlay.addPixmap(pixStop, QIcon::Normal, QIcon::On);
            icPlay.addPixmap(pixPlayDisabled, QIcon::Normal, QIcon::Off);
            _playBtn->setIcon(icPlay);
        }
        _playBtn->setToolTip(QString::fromUtf8("Play/stop through the viewer render engine (Space; fps from the fps box, loops over the in/out points)."));
        connect(_playBtn, SIGNAL(toggled(bool)), this, SLOT(onPlayToggled(bool)));
        playerLayout->addWidget(_playBtn);

        Button* lastBtn = new Button(playerBar);
        lastBtn->setFocusPolicy(Qt::NoFocus);
        lastBtn->setFixedSize(btnSize);
        lastBtn->setIconSize(btnIconSize);
        {
            QPixmap pixLast;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_LAST_FRAME, &pixLast);
            if ( !pixLast.isNull() ) {
                lastBtn->setIcon( QIcon(pixLast) );
            }
        }
        lastBtn->setToolTip(QString::fromUtf8("Go to last frame (End)"));
        connect(lastBtn, SIGNAL(clicked()), this, SLOT(onSeekLast()));
        playerLayout->addWidget(lastBtn);

        playerLayout->addSpacing( TO_DPIX(10) );

        // Single-frame steppers
        Button* prevBtn = new Button(playerBar);
        prevBtn->setFocusPolicy(Qt::NoFocus);
        prevBtn->setFixedSize(btnSize);
        prevBtn->setIconSize(btnIconSize);
        {
            QPixmap pixBack1;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_PREVIOUS, &pixBack1);
            if ( !pixBack1.isNull() ) {
                prevBtn->setIcon( QIcon(pixBack1) );
            }
        }
        prevBtn->setToolTip(QString::fromUtf8("Go to previous frame"));
        connect(prevBtn, SIGNAL(clicked()), this, SLOT(onSeekPrev()));
        playerLayout->addWidget(prevBtn);

        Button* nextBtn = new Button(playerBar);
        nextBtn->setFocusPolicy(Qt::NoFocus);
        nextBtn->setFixedSize(btnSize);
        nextBtn->setIconSize(btnIconSize);
        {
            QPixmap pixForward1;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_NEXT, &pixForward1);
            if ( !pixForward1.isNull() ) {
                nextBtn->setIcon( QIcon(pixForward1) );
            }
        }
        nextBtn->setToolTip(QString::fromUtf8("Go to next frame"));
        connect(nextBtn, SIGNAL(clicked()), this, SLOT(onSeekNext()));
        playerLayout->addWidget(nextBtn);

        playerLayout->addSpacing( TO_DPIX(10) );

        // Keyframe steppers
        Button* prevKeyBtn = new Button(playerBar);
        prevKeyBtn->setFocusPolicy(Qt::NoFocus);
        prevKeyBtn->setFixedSize(btnSize);
        prevKeyBtn->setIconSize(btnIconSize);
        {
            QPixmap pixPrevKey;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_PREVIOUS_KEY, &pixPrevKey);
            if ( !pixPrevKey.isNull() ) {
                prevKeyBtn->setIcon( QIcon(pixPrevKey) );
            }
        }
        prevKeyBtn->setToolTip(QString::fromUtf8("Go to previous keyframe"));
        if (getGui()) {
            connect( prevKeyBtn, SIGNAL(clicked()), getGui()->getApp().get(), SLOT(goToPreviousKeyframe()) );
        }
        playerLayout->addWidget(prevKeyBtn);

        Button* nextKeyBtn = new Button(playerBar);
        nextKeyBtn->setFocusPolicy(Qt::NoFocus);
        nextKeyBtn->setFixedSize(btnSize);
        nextKeyBtn->setIconSize(btnIconSize);
        {
            QPixmap pixNextKey;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_NEXT_KEY, &pixNextKey);
            if ( !pixNextKey.isNull() ) {
                nextKeyBtn->setIcon( QIcon(pixNextKey) );
            }
        }
        nextKeyBtn->setToolTip(QString::fromUtf8("Go to next keyframe"));
        if (getGui()) {
            connect( nextKeyBtn, SIGNAL(clicked()), getGui()->getApp().get(), SLOT(goToNextKeyframe()) );
        }
        playerLayout->addWidget(nextKeyBtn);

        playerLayout->addSpacing( TO_DPIX(10) );

        // Increment steppers + editable increment (the 2D player's default 10)
        Button* incrPrev = new Button(playerBar);
        incrPrev->setFocusPolicy(Qt::NoFocus);
        incrPrev->setFixedSize(btnSize);
        incrPrev->setIconSize(btnIconSize);
        {
            QPixmap pixPrevIncr;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_PREVIOUS_INCR, &pixPrevIncr);
            if ( !pixPrevIncr.isNull() ) {
                incrPrev->setIcon( QIcon(pixPrevIncr) );
            }
        }
        incrPrev->setToolTip(QString::fromUtf8("Go back by the increment"));
        connect(incrPrev, SIGNAL(clicked()), this, SLOT(onSeekPrevIncr()));
        playerLayout->addWidget(incrPrev);

        _incrSpin = new SpinBox(playerBar);
        _incrSpin->setValue(10);
        _incrSpin->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
        _incrSpin->setToolTip(QString::fromUtf8(
            "<p><b>Frame increment:</b></p>The previous/next increment buttons "
            "step with this increment."));
        playerLayout->addWidget(_incrSpin);

        Button* incrNext = new Button(playerBar);
        incrNext->setFocusPolicy(Qt::NoFocus);
        incrNext->setFixedSize(btnSize);
        incrNext->setIconSize(btnIconSize);
        {
            QPixmap pixNextIncr;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_NEXT_INCR, &pixNextIncr);
            if ( !pixNextIncr.isNull() ) {
                incrNext->setIcon( QIcon(pixNextIncr) );
            }
        }
        incrNext->setToolTip(QString::fromUtf8("Go forward by the increment"));
        connect(incrNext, SIGNAL(clicked()), this, SLOT(onSeekNextIncr()));
        playerLayout->addWidget(incrNext);

        playerLayout->addSpacing( TO_DPIX(10) );

        playerLayout->addStretch();

        // Playback out-point spin + ] button (bottom-right of the 2D player)
        _rangeOut = new SpinBox(playerBar, SpinBox::eSpinBoxTypeInt);
        _rangeOut->setValue(250);
        _rangeOut->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
        _rangeOut->setToolTip(QString::fromUtf8("The playback out point"));
        playerLayout->addWidget(_rangeOut);

        _setOutBtn = new Button(playerBar);
        _setOutBtn->setFocusPolicy(Qt::NoFocus);
        _setOutBtn->setFixedSize(btnSize);
        _setOutBtn->setIconSize(btnIconSize);
        {
            QPixmap pixOut;
            appPTR->getIcon(NATRON_PIXMAP_PLAYER_TIMELINE_OUT, &pixOut);
            if ( !pixOut.isNull() ) {
                _setOutBtn->setIcon( QIcon(pixOut) );
            } else {
                _setOutBtn->setText(QString::fromUtf8("]"));
            }
        }
        _setOutBtn->setToolTip(QString::fromUtf8("Set the playback out point at the current frame"));
        connect(_setOutBtn, SIGNAL(clicked()), this, SLOT(onPlaybackOutButtonClicked()));
        playerLayout->addWidget(_setOutBtn);

        // In/out spins drive the timeline boundaries (the engine reads them
        // back through getViewerFrameRange at playback start).
        QObject::connect( _rangeIn, SIGNAL(valueChanged(double)), this, SLOT(onRangeInChanged(double)) );
        QObject::connect( _rangeOut, SIGNAL(valueChanged(double)), this, SLOT(onRangeOutChanged(double)) );
        GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
        if (app) {
            double firstFrame = 1, lastFrame = 250;
            app->getFrameRange(&firstFrame, &lastFrame);
            _rangeIn->blockSignals(true);
            _rangeIn->setValue( (double)firstFrame );
            _rangeIn->blockSignals(false);
            _rangeOut->blockSignals(true);
            _rangeOut->setValue( (double)lastFrame );
            _rangeOut->blockSignals(false);
        }

        mainLayout->addWidget(playerBar);
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
            // Engine gave up (nothing rendered / all failed) -> blank canvas.
            QObject::connect( _viewerNode.get(), SIGNAL(viewerDisconnected()), this, SLOT(onViewerDisconnected()) );
            // Input A/B re-routes (InspectorNode auto-picks WipeUnder on the
            // second connection) keep the compare combo + layer combos in sync.
            InspectorNode* isInspector = dynamic_cast<InspectorNode*>( wrapper.get() );
            if (isInspector) {
                QObject::connect( isInspector, SIGNAL(activeInputsChanged()), this, SLOT(onActiveInputsChanged()) );
            }
            QObject::connect( _viewerNode.get(), SIGNAL(availableComponentsChanged()), this, SLOT(onAvailableComponentsChanged()) );
            refreshInputCombos();
            refreshLayerAndAlphaChannelComboBox();
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
    // Match the 2D viewer (ViewerTab): an Ignored horizontal size policy on
    // the pane stops the info bar / pixel-pick labels from growing the pane's
    // size hint — the info row adapts to the viewport width instead of
    // triggering unwanted resizes of the whole pane.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

ViewportVRTab::~ViewportVRTab()
{
}

InfoViewerWidget*
ViewportVRTab::getInfoViewer() const
{
    return _infoViewer;
}

// ==================== slots: display ====================

void
ViewportVRTab::onDisplayChannelsSelected(int index)
{
    // Combo order matches the 2D viewer's ChannelsComboBox: 0=Luminance,
    // 1=RGB, 2=Red, 3=Green, 4=Blue, 5=Alpha, 6=Matte.
    if (!_viewerNode) {
        return;
    }
    static const DisplayChannelsEnum map[7] = { eDisplayChannelsY,
                                                eDisplayChannelsRGB,
                                                eDisplayChannelsR,
                                                eDisplayChannelsG,
                                                eDisplayChannelsB,
                                                eDisplayChannelsA,
                                                eDisplayChannelsMatte };
    if ( (index < 0) || (index > 6) ) {
        return;
    }
    _viewerNode->setDisplayChannels(map[index], true);
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
    if (_bInput) {
        if ( (curB != -1) ) {
            std::map<int, InputName>::const_iterator found = _inputNamesMap.find(curB);
            if ( found != _inputNamesMap.end() ) {
                const int idx = _bInput->itemIndex(found->second.name);
                if (idx != -1) {
                    _bInput->setCurrentIndex_no_emit(idx);
                }
            }
        } else if (_inputNamesMap.size() >= 2) {
            // No B selected yet with 2+ inputs connected: the engine renders
            // B anyway (it walks the inspector's active pair); default the
            // combo to the first input that is NOT A so the UI matches.
            int aIdx = -1;
            if (_aInput) {
                aIdx = _aInput->activeIndex();
            }
            for (int i = 1; i < _bInput->count(); ++i) { // 0 = " - "
                if (i != aIdx) {
                    _bInput->setCurrentIndex_no_emit(i);
                    break;
                }
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
            const QString removedName = found->second.name;
            // Which combo slot(s) pointed at this input?
            int a = -1, b = -1;
            InspectorNode* inspector = dynamic_cast<InspectorNode*>(wrapper.get());
            if (inspector) {
                inspector->getActiveInputs(a, b);
            }
            if (_aInput) {
                _aInput->blockSignals(true);
                _aInput->removeItem(removedName);
                _aInput->blockSignals(false);
            }
            if (_bInput) {
                _bInput->blockSignals(true);
                _bInput->removeItem(removedName);
                _bInput->blockSignals(false);
            }
            _inputNamesMap.erase(found);
            // All inputs gone: blank the canvas + drop the compare overlay
            // (the engine never re-renders an unconnected graph, so the last
            // image would stick forever). The engine's disconnectTexture
            // arrives too late (it keeps the last frame on purpose).
            if (_inputNamesMap.empty() && _viewport) {
                _viewport->setCompositingOperator(eViewerCompositingOperatorNone);
                if (_compareDropdown) {
                    _compareDropdown->blockSignals(true);
                    _compareDropdown->setCurrentIndex_no_emit(0);
                    _compareDropdown->blockSignals(false);
                }
                if (_bInput) {
                    _bInput->setEnabled_natron(false);
                }
                if (_infoViewerB) {
                    _infoViewerB->hide();
                }
                _viewport->clearVRTextures();
            } else if (_viewport) {
                // Only one input left: if the removed one was active on B,
                // drop the B slot so the wipe overlay does not linger.
                if ( (b != -1) && (b == inputNb) ) {
                    _viewport->clearVRSlotB();
                    if (_infoViewerB) {
                        _infoViewerB->hide();
                    }
                }
            }
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
        refreshLayerAndAlphaChannelComboBox();
    } else if (_viewport) {
        // A disconnected: blank the canvas immediately (the engine only
        // re-renders, it does not push a "no input" frame).
        _viewport->clearVRTextures();
        refreshLayerAndAlphaChannelComboBox();
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
        if (_infoViewerB) {
            _infoViewerB->show();
        }
    } else if (_viewport) {
        // B disconnected: drop the stale B texture so the compare does not
        // keep sampling the previous image.
        _viewport->clearVRSlotB();
        if (_infoViewerB) {
            _infoViewerB->hide();
        }
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
    // ROI windowing is temporarily disabled (the button is grayed out); this
    // slot is kept for the connect() in case it is re-enabled.
    Q_UNUSED(on);
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
ViewportVRTab::onProxyScaleChanged(int index)
{
    // Combo index 0..4 = proxy factor 2/4/8/16/32 (mipmap level index + 1).
    _proxyScaleActive = index;
    if (_proxyActive && _viewerNode) {
        _viewerNode->onMipmapLevelChanged( (unsigned int)(index + 1) );
    }
    if (_viewport) {
        _viewport->update();
    }
}

void
ViewportVRTab::onCompareOperatorSelected(int index)
{
    // Combo order = the op enum order used at construction: 0=None, then
    // W-Under/W-Over/W-Minus/W-OnionSkin/S-Under/S-Over/S-Minus/S-OnionSkin.
    ViewerCompositingOperatorEnum op = (ViewerCompositingOperatorEnum)index;
    if (_viewport) {
        _viewport->setCompositingOperator(op);
    }
    // The engine reads getCompositingOperator() off the uiContext (our
    // ViewportVR), so the B input renders only when an op is active — same
    // mechanism as the 2D viewer's compare.
    if (_bInput) {
        _bInput->setEnabled_natron(op != eViewerCompositingOperatorNone);
    }
    // The B info row follows: visible only while a compare op is active.
    if (_infoViewerB) {
        if (op != eViewerCompositingOperatorNone) {
            _infoViewerB->show();
        } else {
            _infoViewerB->hide();
        }
    }
}

void
ViewportVRTab::onColorSpaceSelected(int index)
{
    if (!_viewerNode) {
        return;
    }
    const int builtInCount = 4; // Linear / sRGB / Rec.709 / BT1886
    if (index >= builtInCount) {
        // OCIO display/view transform entry (ViewerTab's routing model).
        const int ocioIndex = index - builtInCount;
        if ( (ocioIndex >= 0) && ( ocioIndex < (int)_ocioDisplayViews.size() ) ) {
            const std::pair<std::string, std::string>& dv = _ocioDisplayViews[ocioIndex];
            _viewerNode->setOcioDisplayView(dv.first, dv.second);
        }
        return;
    }
    // Leaving an OCIO view: clear it so the built-in LUT path takes over again.
    _viewerNode->setOcioDisplayView(std::string(), std::string());
    ViewerColorSpaceEnum colorspace = eViewerColorSpaceSRGB;
    switch (index) {
    case 0: colorspace = eViewerColorSpaceLinear; break;
    case 1: colorspace = eViewerColorSpaceSRGB; break;
    case 2: colorspace = eViewerColorSpaceRec709; break;
    case 3: colorspace = eViewerColorSpaceBT1886; break;
    default: break;
    }
    _viewerNode->onColorSpaceChanged(colorspace);
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

// ==================== slots: layer / alpha / full-frame / refresh ==========

void
ViewportVRTab::refreshLayerAndAlphaChannelComboBox()
{
    // ViewerTab::refreshLayerAndAlphaChannelComboBox counterpart, simplified:
    // the combos list the layers available on the active A input (the VR pane
    // renders input A only; B is compare-only).
    if (!_viewerNode || !_layerChoice || !_alphaChannelChoice) {
        return;
    }
    QString layerCurChoice = _layerChoice->getCurrentIndexText();
    QString alphaCurChoice = _alphaChannelChoice->getCurrentIndexText();

    std::set<ImagePlaneDesc> components;
    {
        int a = -1, b = -1;
        NodePtr wrapper = _viewerNode->getNode();
        InspectorNode* inspector = wrapper ? dynamic_cast<InspectorNode*>(wrapper.get()) : NULL;
        if (inspector) {
            inspector->getActiveInputs(a, b);
        }
        double time = getGui()->getApp()->getTimeLine()->currentFrame();
        EffectInstancePtr activeInput = _viewerNode->getInput(a);
        if (activeInput) {
            std::list<ImagePlaneDesc> compsAvailable;
            activeInput->getAvailableLayers(time, ViewIdx(0), -1, &compsAvailable);
            for (std::list<ImagePlaneDesc>::iterator it = compsAvailable.begin(); it != compsAvailable.end(); ++it) {
                components.insert(*it);
            }
        }
    }

    _layerChoice->blockSignals(true);
    _alphaChannelChoice->blockSignals(true);
    _layerChoice->clear();
    _alphaChannelChoice->clear();
    _layerChoice->addItem( QString::fromUtf8("-") );
    _alphaChannelChoice->addItem( QString::fromUtf8("-") );

    std::set<ImagePlaneDesc>::iterator foundColorIt = components.end();
    std::set<ImagePlaneDesc>::iterator foundOtherIt = components.end();
    for (std::set<ImagePlaneDesc>::iterator it = components.begin(); it != components.end(); ++it) {
        ChoiceOption option = it->getPlaneOption();
        _layerChoice->addItem( QString::fromUtf8( option.label.c_str() ) );
        if ( it->isColorPlane() ) {
            foundColorIt = it;
        } else {
            foundOtherIt = it;
        }
        const std::vector<std::string>& channels = it->getChannels();
        for (U32 i = 0; i < channels.size(); ++i) {
            ChoiceOption chanOpt = it->getChannelOption(i);
            _alphaChannelChoice->addItem( QString::fromUtf8( chanOpt.label.c_str() ) );
        }
    }

    // Default layer: the color plane, else any other layer.
    if ( foundColorIt != components.end() ) {
        const int idx = _layerChoice->itemIndex( QString::fromUtf8( foundColorIt->getPlaneLabel().c_str() )
                                                 + QLatin1Char('.') + QString::fromUtf8( foundColorIt->getChannelsLabel().c_str() ) );
        _layerChoice->setCurrentIndex_no_emit(idx != -1 ? idx : 0);
        _viewerNode->setActiveLayer(*foundColorIt, false);
        // Default alpha: the last channel of the color plane (A of RGBA).
        const std::vector<std::string>& channels = foundColorIt->getChannels();
        if ( (channels.size() == 4) || (channels.size() == 1) ) {
            const std::size_t last = channels.size() - 1;
            const QString alphaLabel = QString::fromUtf8( foundColorIt->getPlaneLabel().c_str() )
                                       + QLatin1Char('.') + QString::fromUtf8( channels[last].c_str() );
            const int aIdx = _alphaChannelChoice->itemIndex(alphaLabel);
            if (aIdx != -1) {
                _alphaChannelChoice->setCurrentIndex_no_emit(aIdx);
            }
            _viewerNode->setAlphaChannel(*foundColorIt, channels[last], false);
        }
    } else if ( foundOtherIt != components.end() ) {
        const int idx = _layerChoice->itemIndex( QString::fromUtf8( foundOtherIt->getPlaneLabel().c_str() )
                                                 + QLatin1Char('.') + QString::fromUtf8( foundOtherIt->getChannelsLabel().c_str() ) );
        _layerChoice->setCurrentIndex_no_emit(idx != -1 ? idx : 0);
        _viewerNode->setActiveLayer(*foundOtherIt, false);
    } else {
        _layerChoice->setCurrentIndex_no_emit(0);
        _viewerNode->setActiveLayer(ImagePlaneDesc::getNoneComponents(), false);
        _alphaChannelChoice->setCurrentIndex_no_emit(0);
        _viewerNode->setAlphaChannel(ImagePlaneDesc::getNoneComponents(), std::string(), false);
    }
    _layerChoice->blockSignals(false);
    _alphaChannelChoice->blockSignals(false);
}

void
ViewportVRTab::onLayerComboChanged(int index)
{
    // ViewerTab::onLayerComboChanged model: index 0 = "-", i >= 1 walks the
    // (sorted) available layers.
    if (!_viewerNode) {
        return;
    }
    std::set<ImagePlaneDesc> components;
    {
        int a = -1, b = -1;
        NodePtr wrapper = _viewerNode->getNode();
        InspectorNode* inspector = wrapper ? dynamic_cast<InspectorNode*>(wrapper.get()) : NULL;
        if (inspector) {
            inspector->getActiveInputs(a, b);
        }
        double time = getGui()->getApp()->getTimeLine()->currentFrame();
        EffectInstancePtr activeInput = _viewerNode->getInput(a);
        if (activeInput) {
            std::list<ImagePlaneDesc> compsAvailable;
            activeInput->getAvailableLayers(time, ViewIdx(0), -1, &compsAvailable);
            for (std::list<ImagePlaneDesc>::iterator it = compsAvailable.begin(); it != compsAvailable.end(); ++it) {
                components.insert(*it);
            }
        }
    }
    if ( (index < 0) || ( index >= (int)(components.size() + 1) ) ) {
        qDebug() << "ViewportVRTab::onLayerComboChanged: invalid index";
        return;
    }
    int i = 1; // because of the "-" choice
    int chanCount = 1;
    for (std::set<ImagePlaneDesc>::iterator it = components.begin(); it != components.end(); ++it, ++i) {
        chanCount += it->getChannels().size();
        if (i == index) {
            _viewerNode->setActiveLayer(*it, true);
            // If it has an alpha channel, select it in the alpha combo.
            if ( it->getChannels().size() == 4 ) {
                const std::vector<std::string>& channels = it->getChannels();
                const QString alphaLabel = QString::fromUtf8( it->getPlaneLabel().c_str() )
                                           + QLatin1Char('.') + QString::fromUtf8( channels[3].c_str() );
                const int aIdx = _alphaChannelChoice->itemIndex(alphaLabel);
                if (aIdx != -1) {
                    _alphaChannelChoice->setCurrentIndex_no_emit(aIdx);
                }
                _viewerNode->setAlphaChannel(*it, channels[3], true);
            }
            return;
        }
    }
    if (_alphaChannelChoice) {
        _alphaChannelChoice->setCurrentIndex_no_emit(0);
    }
    _viewerNode->setAlphaChannel(ImagePlaneDesc::getNoneComponents(), std::string(), false);
    _viewerNode->setActiveLayer(ImagePlaneDesc::getNoneComponents(), true);
}

void
ViewportVRTab::onAlphaChannelComboChanged(int index)
{
    // ViewerTab::onAlphaChannelComboChanged model: index 0 = "-", then walk
    // each layer's channels.
    if (!_viewerNode) {
        return;
    }
    std::set<ImagePlaneDesc> components;
    {
        int a = -1, b = -1;
        NodePtr wrapper = _viewerNode->getNode();
        InspectorNode* inspector = wrapper ? dynamic_cast<InspectorNode*>(wrapper.get()) : NULL;
        if (inspector) {
            inspector->getActiveInputs(a, b);
        }
        double time = getGui()->getApp()->getTimeLine()->currentFrame();
        EffectInstancePtr activeInput = _viewerNode->getInput(a);
        if (activeInput) {
            std::list<ImagePlaneDesc> compsAvailable;
            activeInput->getAvailableLayers(time, ViewIdx(0), -1, &compsAvailable);
            for (std::list<ImagePlaneDesc>::iterator it = compsAvailable.begin(); it != compsAvailable.end(); ++it) {
                components.insert(*it);
            }
        }
    }
    int i = 1; // because of the "-" choice
    for (std::set<ImagePlaneDesc>::iterator it = components.begin(); it != components.end(); ++it) {
        const std::vector<std::string>& channels = it->getChannels();
        if ( index >= ( (int)channels.size() + i ) ) {
            i += channels.size();
        } else {
            for (U32 j = 0; j < channels.size(); ++j, ++i) {
                if (i == index) {
                    _viewerNode->setAlphaChannel(*it, channels[j], true);
                    return;
                }
            }
        }
    }
    _viewerNode->setAlphaChannel(ImagePlaneDesc::getNoneComponents(), std::string(), true);
}

void
ViewportVRTab::onFullFrameToggled(bool on)
{
    // The 2D viewer's "cogs" button: render the whole image vs the visible
    // part. The canvas clips the rendered ROI to the FOV window when off;
    // the engine knob forces the mipmap level to 0 like the 2D viewer.
    if (_viewport) {
        _viewport->setFullFrameProcessing(on);
    }
    if (_viewerNode) {
        _viewerNode->setFullFrameProcessingEnabled(on);
    }
    // Re-render: the ROI changed (forceRender bypasses the texture cache).
    if (_viewerNode) {
        _viewerNode->forceFullComputationOnNextFrame();
        _viewerNode->renderCurrentFrame(true);
    }
}

void
ViewportVRTab::onRefreshClicked()
{
    // ViewerTab::refresh(): abort + force-full-computation + render current.
    if (!_viewerNode) {
        return;
    }
    Gui* gui = getGui();
    const bool stats = gui && qApp->keyboardModifiers().testFlag(Qt::ShiftModifier)
                       && qApp->keyboardModifiers().testFlag(Qt::ControlModifier);
    _viewerNode->getRenderEngine()->abortRenderingNoRestart();
    _viewerNode->forceFullComputationOnNextFrame();
    _viewerNode->clearPersistentMessage(true);
    if (!stats) {
        _viewerNode->renderCurrentFrame(true);
    } else {
        gui->getOrCreateRenderStatsDialog()->show();
        _viewerNode->renderCurrentFrameWithRenderStats(false);
    }
}

void
ViewportVRTab::onViewerDisconnected()
{
    // The engine gave up on both inputs (all renders failed / nothing
    // connected): blank the canvas, like ViewerGL::disconnectViewer.
    if (_viewport) {
        _viewport->clearVRTextures();
    }
}

void
ViewportVRTab::onActiveInputsChanged()
{
    // The InspectorNode changed the active A/B pair (e.g. connecting a second
    // input auto-selects it on B and picks WipeUnder when no op was active —
    // InspectorNode::refreshActiveInputs). Mirror the op into the combo so it
    // shows what is actually rendered, and re-sync the A/B combos to the
    // engine's active pair (the combo labels can also change here).
    refreshInputCombos();
    if (_viewport && _compareDropdown) {
        const ViewerCompositingOperatorEnum op = _viewport->getCompositingOperator();
        const int idx = (int)op;
        if ( (idx >= 0) && (idx < _compareDropdown->count()) ) {
            _compareDropdown->blockSignals(true);
            _compareDropdown->setCurrentIndex_no_emit(idx);
            _compareDropdown->blockSignals(false);
        }
        if (_bInput) {
            _bInput->setEnabled_natron(op != eViewerCompositingOperatorNone);
        }
        // The B info row follows the auto-picked op too.
        if (_infoViewerB) {
            if (op != eViewerCompositingOperatorNone) {
                _infoViewerB->show();
            } else {
                _infoViewerB->hide();
            }
        }
    }
}

void
ViewportVRTab::onAvailableComponentsChanged()
{
    refreshLayerAndAlphaChannelComboBox();
}

void
ViewportVRTab::onProjectFrameRangeChanged(int left, int right)
{
    // The VR tab's TimeLineGui is created with a NULL ViewerTab, so its own
    // onProjectFrameRangeChanged early-returns; replicate the 2D behavior
    // here: unless the user edited the range by hand, follow the project's
    // frame range (which a newly connected reader extends).
    if (_timelineGui) {
        if ( !_timelineGui->isFrameRangeEdited() ) {
            _timelineGui->setBoundaries( (SequenceTime)left, (SequenceTime)right );
            _timelineGui->setFrameRangeEdited(false);
            _timelineGui->centerOn( (SequenceTime)left, (SequenceTime)right );
        }
        _timelineGui->update();
    }
    if (_viewport) {
        _viewport->setVRPlaybackRange(left, right);
    }
    if (_rangeIn) {
        _rangeIn->blockSignals(true);
        _rangeIn->setValue( (double)left );
        _rangeIn->blockSignals(false);
    }
    if (_rangeOut) {
        _rangeOut->blockSignals(true);
        _rangeOut->setValue( (double)right );
        _rangeOut->blockSignals(false);
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
    // Same as the 2D viewer's [ button: rescale the timeline around the new
    // in/out bounds.
    if (_timelineGui) {
        _timelineGui->recenterOnBounds();
    }
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
    if (_timelineGui) {
        _timelineGui->recenterOnBounds();
    }
}

void
ViewportVRTab::onRangeInChanged(double value)
{
    if (_viewport && _rangeOut) {
        _viewport->setVRPlaybackRange( (int)value, (int)_rangeOut->value() );
    }
    if (_timelineGui && _rangeOut) {
        _timelineGui->setBoundaries( (SequenceTime)value, (SequenceTime)_rangeOut->value() );
        _timelineGui->recenterOnBounds();
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
        _timelineGui->recenterOnBounds();
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
ViewportVRTab::onSeekPrevIncr()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline && _incrSpin) {
        timeline->seekFrame(timeline->currentFrame() - (SequenceTime)_incrSpin->value(), true, NULL, eTimelineChangeReasonUserSeek);
    }
}

void
ViewportVRTab::onSeekNextIncr()
{
    TimeLinePtr timeline = getTimeline();
    if (timeline && _incrSpin) {
        timeline->seekFrame(timeline->currentFrame() + (SequenceTime)_incrSpin->value(), true, NULL, eTimelineChangeReasonUserSeek);
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
ViewportVRTab::startPlayback(bool forward)
{
    if (!_viewerNode) {
        return;
    }
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
                                                           forward ? eRenderDirectionForward : eRenderDirectionBackward);
}

void
ViewportVRTab::onPlayToggled(bool playing)
{
    if (!playing) {
        // Stop: pure user intent — the button carries the state; abort the
        // engine's playback. (Queued scrub-refresh renders still re-render the
        // current frame once; that is the 2D viewer's behavior too.)
        _userStoppedMs = QDateTime::currentMSecsSinceEpoch();
        if (_viewerNode) {
            _viewerNode->getRenderEngine()->abortRenderingNoRestart();
        }
        return;
    }
    startPlayback(true);
}

void
ViewportVRTab::onPlayBackwardToggled(bool playing)
{
    if (!playing) {
        _userStoppedMs = QDateTime::currentMSecsSinceEpoch();
        if (_viewerNode) {
            _viewerNode->getRenderEngine()->abortRenderingNoRestart();
        }
        return;
    }
    startPlayback(false);
}

void
ViewportVRTab::onFpsLockToggled(bool on)
{
    // The 2D player's fps lock: unchecked = the fps follows the input's frame
    // rate (ViewerTab::refreshFPSBoxFromClipPreferences); checked = the user's
    // value drives the render engine.
    if (_fpsSpin) {
        _fpsSpin->setEnabled(on);
    }
}

void
ViewportVRTab::onViewportLookChanged()
{
    // With full-frame rendering the look does not change the frame, so no
    // re-render. With full-frame processing OFF the rendered ROI follows the
    // look (the FOV window) — schedule one debounced re-render so the window
    // tracks the camera.
    if (_viewport && !_viewport->isFullFrameProcessing()) {
        if (!_lastLookRefreshValid) {
            _lastLookRefreshValid = true;
            if (_lookRefreshTimer) {
                _lookRefreshTimer->start(60);
            }
        }
    } else {
        _lastLookRefreshValid = false;
    }
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
    // intent. ViewerTab's engine-signal->button feedback loop made Stop feel
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

