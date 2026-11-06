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

#include "Gui/PanelWidget.h"
#include "Gui/GuiFwd.h"

class QLabel;
class QToolButton;
class QTimer;
class QDoubleSpinBox;

NATRON_NAMESPACE_ENTER

class TimeLineGui;
class ViewportVR;

/**
 * @brief Wrapper panel for the VR / 360 viewport: the pane of a "Viewer VR"
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
 * The viewer bar is rebuilt here as a compact equivalent: the top row carries
 * the display controls (channels, FOV, recenter, pause updates, ROI/proxy,
 * compare) plus VR packing (Layout/Eye); the bottom row carries the playback
 * transport (first/prev/next/last, keyframe and increment steppers, play,
 * frame counter, in/out points, fps) and the timeline.
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

    /** The bound "Viewer VR" node (weak). */
    NodeWPtr getNodeBinding() const { return _boundNode; }

public Q_SLOTS:

    /** Stereo packing dropdown: Mono / Over-Under / Side-by-Side. */
    void onLayoutSelected();

    /** Eye dropdown: Left / Right (no-op in Mono). */
    void onEyeSelected();

    /** FOV spin box (the VR counterpart of the 2D viewer's Zoom). */
    void onFovChanged(double fov);

    void onResetView();
    void onForceRefresh();

    /** Display channels: RGB / R / G / B / A / Luminance / Matte, via the
     *  viewer node's setDisplayChannels (both inputs). */
    void onDisplayChannelsSelected();

    /** Pause Updates — viewerNode->setViewerPaused, same as the 2D viewer. */
    void onPauseToggled(bool paused);

    /** ROI windowing toggle: renders only the pano slice visible through the
     *  FOV (much faster) — the VR counterpart of the 2D user-ROI button. */
    void onRoiToggled(bool on);

    /** Proxy scale toggle + factor combo, via viewerNode->onMipmapLevelChanged. */
    void onProxyToggled(bool on);
    void onProxyScaleChanged();

    /** Comparison operator dropdown (A/B ops, applied in the VR sampler). */
    void onCompareOperatorSelected();

    /** Gain + gamma spins, wired to the viewer node's color pipeline. */
    void onGainChanged(double gain);
    void onGammaChanged(double gamma);

    /** Play/stop — the viewer's render engine (renderFromCurrentFrame), fps
     *  via setDesiredFPS, loop via the engine's playback mode. */
    void onPlayToggled(bool playing);
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
    void onSeekPrevKey();
    void onSeekNextKey();
    void onSeekPrevIncr();
    void onSeekNextIncr();

    void onFrameSpinChanged(double frame);

    void onFrameChanged(SequenceTime frame, int reason);

protected:

    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE;

private:

    TimeLinePtr getTimeline() const;
    void refreshTransportState();

    NodeWPtr _boundNode;
    ViewerInstancePtr _viewerNode;

    ViewportVR* _viewport;
    TimeLineGui* _timelineGui;

    // top bar
    QToolButton* _channelsDropdown;
    QToolButton* _layoutDropdown;
    QToolButton* _eyeDropdown;
    QDoubleSpinBox* _fovSpin;
    QToolButton* _pauseBtn;
    QToolButton* _roiBtn;
    QToolButton* _proxyBtn;
    QToolButton* _proxyScaleDropdown;
    QToolButton* _compareDropdown;
    QDoubleSpinBox* _gainSpin;
    QDoubleSpinBox* _gammaSpin;
    // bottom bar (transport)
    QToolButton* _playBtn;
    QTimer* _lookRefreshTimer;
    bool _lastLookRefreshValid;
    QDoubleSpinBox* _frameSpin;
    QDoubleSpinBox* _rangeIn;
    QDoubleSpinBox* _rangeOut;
    QLabel* _fpsLabel;
    int _proxyScaleActive;
    bool _proxyActive;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORTVRTAB_H
