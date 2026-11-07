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

#ifndef NATRON_GUI_VIEWPORTVRTAB_H
#define NATRON_GUI_VIEWPORTVRTAB_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include "Global/QtCompat.h"
#include "Global/GlobalDefines.h"

#include <QWidget>
#include <map>

#include "Gui/PanelWidget.h"
#include "Gui/GuiFwd.h"

class QLabel;
class QToolButton;
class QTimer;
class QDoubleSpinBox;
class QCheckBox;

NATRON_NAMESPACE_ENTER

class TimeLineGui;
class ViewportVR;
class ComboBox;
class ScaleSliderQWidget;

/**
 * @brief Wrapper panel for the VR / 360 viewport: the pane of a "VR Viewer"
 * node.
 *
 * The pane is bound to one VRViewer node (ViewerInstance-derived, created as
 * an InspectorNode so the A/B inputs work exactly like a 2D viewer's). Every
 * control drives the same ViewerInstance/render-engine API the 2D viewer's
 * bars use — so channels, gain/gamma, colorspace, A/B + comparison operator,
 * proxy scale, ROI-windowing (which in VR means "render only the slice of the
 * pano visible through the FOV") and the playback engine (play through the
 * in/out points at the desired fps) behave exactly like the 2D viewer.
 *
 * The viewer bars are laid out exactly like the 2D viewer pane: the top two
 * rows carry the display controls (row 1: channels, inputs, FOV, recenter,
 * pause, ROI/proxy, compare — row 2: gain/gamma sliders + VR packing
 * Layout/Eye); below the canvas come the image-info row (InfoViewerWidget:
 * channels/format, resolution, RoD, pixel coordinates and the RGBA + HSV(L)
 * of the pixel under the cursor), the timeline and the playback transport,
 * which is widget-for-widget the 2D player row (first/rewind/frame counter/
 * forward/last, single-frame + keyframe + increment steppers, in/out points,
 * fps lock + fps + time format, turbo, playback mode).
 */
class ViewportVRTab
    : public QWidget
    , public PanelWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    explicit ViewportVRTab(NodePtr viewerNode, Gui* gui, QWidget* parent = NULL);
    virtual ~ViewportVRTab();

    ViewportVR* getViewport() const { return _viewport; }

    /** The bound "VR Viewer" node (weak). */
    NodeWPtr getNodeBinding() const { return _boundNode; }

public Q_SLOTS:

    /** Stereo packing dropdown: Mono / Over-Under / Side-by-Side. */
    void onLayoutSelected();

    /** Eye dropdown: Left / Right (no-op in Mono). */
    void onEyeSelected();

    /** FOV spin box (the VR counterpart of the 2D viewer's Zoom). */
    void onFovChanged(double fov);

    /** Wheel/reset-driven FOV changes coming from the canvas, mirrored into
     *  the spin box (no re-emit loop — the spin signal is blocked during it). */
    void onViewportFovChanged(double fov);

    void onResetView();
    void onForceRefresh();

    /** Layer / alpha-channel selectors (the 2D viewer's Layer + Alpha fields,
     *  fed by the active A input's available layers). */
    void onLayerComboChanged(int index);
    void onAlphaChannelComboChanged(int index);

    /** Full-frame processing toggle (the 2D viewer's "cogs" button): render
     *  the image in its entirety vs only the visible part. */
    void onFullFrameToggled(bool on);

    /** Refresh: force a re-render of the current frame (2D viewer's refresh
     *  button; Ctrl+Shift+click adds render stats like ViewerTab::refresh). */
    void onRefreshClicked();

    /** Display channels: RGB / R / G / B / A / Luminance / Matte, via the
     *  viewer node's setDisplayChannels (both inputs). */
    void onDisplayChannelsSelected(int index);

    /** Pause Updates — viewerNode->setViewerPaused, same as the 2D viewer. */
    void onPauseToggled(bool paused);

    /** ROI windowing toggle: renders only the pano slice visible through the
     *  FOV (much faster) — the VR counterpart of the 2D user-ROI button. */
    void onRoiToggled(bool on);

    /** Proxy scale toggle + factor combo, via viewerNode->onMipmapLevelChanged. */
    void onProxyToggled(bool on);
    void onProxyScaleChanged(int index);

    /** Comparison operator (A/B ops, applied in the VR sampler). */
    void onCompareOperatorSelected(int index);

    /** Color transform (the 2D viewer's colorspace combo: Linear/sRGB/Rec709/
     *  BT1886 + the OCIO config's display/view transforms). */
    void onColorSpaceSelected(int index);

    /** Input A/B selectors (the viewer's A:/B: comboboxes over the Inspector
     *  node's unfolded inputs). */
    void onInputChanged(int inputNb);
    void onInputNameChanged(int inputNb, const QString& name);
    void onFirstInputNameChanged(const QString& text);
    void onSecondInputNameChanged(const QString& text);

    /** Gain + gamma spins/sliders, wired to the viewer node's color pipeline.
     *  The spins/sliders are in f-stops for gain (gain = 2^x); the toggle
     *  buttons snap between neutral and the last used value. */
    void onGainSpinChanged(double fstop);
    void onGammaSpinChanged(double gamma);
    void onGainToggled(bool on);
    void onGammaToggled(bool on);

    /** Playback mode button: loop / bounce / once (engine's setPlaybackMode). */
    void onPlaybackModeClicked();

    /** Play/stop — the viewer render engine (renderFromCurrentFrame), fps
     *  via setDesiredFPS, loop via the engine's playback mode. Forward and
     *  backward are separate buttons, exactly like the 2D viewer's player. */
    void onPlayToggled(bool playing);
    void onPlayBackwardToggled(bool playing);
    void onFpsLockToggled(bool on);
    void onFpsSpinChanged(double fps);
    void onEngineStarted(bool forward);
    void onEngineStopped(int retCode);

    /** Cheap post-look-change re-render (completes tiles an aborted render
     *  pass left out; debounced). */
    void onViewportLookChanged();
    void onLookRefreshTimeout();

    void onSeekFirst();
    void onSeekPrev();
    void onSeekNext();
    void onSeekLast();
    void onSeekPrevIncr();
    void onSeekNextIncr();

    void onFrameSpinChanged(double frame);

    void onFrameChanged(SequenceTime frame, int reason);

    /** Timeline in/out set-at-current-frame buttons (viewer player). */
    void onPlaybackInButtonClicked();
    void onPlaybackOutButtonClicked();
    void onRangeInChanged(double value);
    void onRangeOutChanged(double value);

    /** The 2D viewer's InfoViewerWidget row mounted above the timeline: the
     *  canvas fills it (format / resolution / RoD / RGBA / HSV(L) mouse pick;
     *  see ViewportVR::setInfoViewer). */
    InfoViewerWidget* getInfoViewer() const;

    /** ViewerInstance::viewerDisconnected → blank the canvas (the 2D viewer's
     *  disconnectViewer path). */
    void onViewerDisconnected();

    /** Project frame range grew/shrank (reader connected): follow it like the
     *  2D viewer's timeline does (the VR tab's TimeLineGui has no ViewerTab
     *  pointer, so its own onProjectFrameRangeChanged early-returns). */
    void onProjectFrameRangeChanged(int left, int right);

    /** InspectorNode auto-picked a new A/B pair (second input connected):
     *  mirror it into the compare combo (2D viewer's onActiveInputsChanged). */
    void onActiveInputsChanged();

    /** The active input's available layers changed: refill the layer/alpha
     *  combos (2D viewer's onAvailableComponentsChanged). */
    void onAvailableComponentsChanged();

    /** Fill the layer/alpha combos from the active input's available layers
     *  (ViewerTab::refreshLayerAndAlphaChannelComboBox counterpart). */
    void refreshLayerAndAlphaChannelComboBox();

protected:

    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE;

private:

    TimeLinePtr getTimeline() const;
    void refreshTransportState();
    void refreshInputCombos();
    void startPlayback(bool forward);

    NodeWPtr _boundNode;
    ViewerInstancePtr _viewerNode;

    ViewportVR* _viewport;
    TimeLineGui* _timelineGui;
    InfoViewerWidget* _infoViewer;
    InfoViewerWidget* _infoViewerB; //!< "B:" row, shown in compare mode

    // input A/B selector state (ViewerTab's inputNamesMap equivalent):
    // viewer input index -> node label; the InspectorNode unfolds inputs as
    // they connect and the comboboxes pick the active A/B pair.
    struct InputName
    {
        QString name;
        NodeWPtr input;
    };
    std::map<int, InputName> _inputNamesMap;

    // top bar
    ComboBox* _layerChoice;
    ComboBox* _alphaChannelChoice;
    ComboBox* _channelsDropdown;
    Button* _fullFrameBtn;
    Button* _refreshBtn;
    QToolButton* _layoutDropdown;
    QToolButton* _eyeDropdown;
    QDoubleSpinBox* _fovSpin;
    Button* _pauseBtn;
    Button* _roiBtn;
    Button* _proxyBtn;
    ComboBox* _proxyScaleDropdown;
    QLabel* _aLabel;
    ComboBox* _aInput;
    QLabel* _bLabel;
    ComboBox* _bInput;
    ComboBox* _compareDropdown;
    QDoubleSpinBox* _gainSpin;
    ScaleSliderQWidget* _gainSlider;
    bool _gainOn;
    double _lastFstop;
    QDoubleSpinBox* _gammaSpin;
    ScaleSliderQWidget* _gammaSlider;
    bool _gammaOn;
    double _lastGamma;
    ComboBox* _colorSpaceCombo;
    // OCIO display/view transforms appended after the 4 built-in colorspaces
    // (ViewerTab's model: combo index - 4 indexes this vector).
    std::vector<std::pair<std::string, std::string> > _ocioDisplayViews;
    // bottom bar (transport) — widget-for-widget the 2D viewer's player row:
    // first/rewind/current-frame/forward/last cluster, single-frame + keyframe
    // + increment steppers, in/out points, fps lock + fps + time format,
    // turbo (GUI freeze) and the playback-mode button.
    Button* _playBtn;
    Button* _playBwdBtn;
    Button* _playModeBtn;
    Button* _turboBtn;
    Button* _setInBtn;
    Button* _setOutBtn;
    QCheckBox* _fpsLock;
    ComboBox* _timeFormatCombo;
    QTimer* _lookRefreshTimer;
    bool _lastLookRefreshValid;
    qint64 _userStoppedMs; //!< when Stop was last pressed (grace window vs queued renders)
    SpinBox* _fpsSpin;
    SpinBox* _frameSpin;
    SpinBox* _rangeIn;
    SpinBox* _rangeOut;
    SpinBox* _incrSpin;
    QLabel* _fpsLabel;
    int _proxyScaleActive;
    bool _proxyActive;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORTVRTAB_H
