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
class QSpinBox;
class QToolButton;
class QTimer;
class QDoubleSpinBox;

NATRON_NAMESPACE_ENTER

class TimeLineGui;
class ViewportVR;

/**
 * @brief Wrapper panel for the dedicated VR / 360 viewport.
 *
 * The pane is bound to exactly one `VR Viewer` node (the VRViewport's sink in
 * the node graph, the way a 2D viewer pane belongs to a Viewer node): creating
 * a VR Viewer node opens a bound pane automatically, the pane created from the
 * pane corner menu binds to the first VR Viewer node in the project, and the
 * binding is restored from saved projects via the parallel ViewportVRNodes
 * serialization list.
 *
 * Controls follow the standard viewer layout (uff-reviewed against the 2D
 * viewer bars): the top bar carries the channel display, gamma and the
 * VR-specific packing settings (Layout / Eye) plus refresh; the bottom player
 * bar carries the playback transport (first/prev/play/next/last, frame counter,
 * in/out range, fps readout) and the timeline.
 */
class ViewportVRTab
    : public QWidget
    , public PanelWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    explicit ViewportVRTab(Gui* gui, QWidget* parent = NULL);
    virtual ~ViewportVRTab();

    ViewportVR* getViewport() const { return _viewport; }

    /** Bind the pane to a VR Viewer node (its iframe stream feeds the pane). */
    void setNodeBinding(const NodePtr& node);

    /** The bound node (weak) — NULL when unbound. */
    NodeWPtr getNodeBinding() const { return _boundNode; }

    /** If unbound, bind the FIRST VRViewer node in the project. */
    void autoBindToVRViewer();

public Q_SLOTS:

    /** Stereo packing dropdown: Mono / Over-Under / Side-by-Side. */
    void onLayoutSelected();

    /** Eye dropdown: Left / Right (no-op in Mono). */
    void onEyeSelected();

    /** Display-gamma spin box. */
    void onGammaChanged(double gamma);

    /** Channel-display dropdown: RGB / Red / Green / Blue / Luminance. */
    void onChannelsSelected();

    void onResetView();
    void onForceRefresh();

    /** Soft playback: drives the shared project timeline (everything that
     *  listens follows, like a scrub). Loop over the project range. */
    void onPlayToggled(bool playing);
    void onPlayTick();
    void onStopClicked();

    void onSeekFirst();
    void onSeekPrev();
    void onSeekNext();
    void onSeekLast();

    void onFrameSpinChanged(int frame);

    void onFrameChanged(SequenceTime frame, int reason);

protected:

    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE;

private:

    TimeLinePtr getTimeline() const;

    ViewportVR* _viewport;
    TimeLineGui* _timelineGui;
    QToolButton* _layoutDropdown;
    QToolButton* _eyeDropdown;
    QToolButton* _channelsDropdown;
    QToolButton* _refreshBtn;
    QToolButton* _playBtn;
    QDoubleSpinBox* _gammaSpin;
    QTimer* _playTimer;
    QSpinBox* _frameSpin;
    QSpinBox* _rangeIn;
    QSpinBox* _rangeOut;
    QLabel* _fpsLabel;
    NodeWPtr _boundNode;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORTVRTAB_H
