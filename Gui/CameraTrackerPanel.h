/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_GUI_CAMERATRACKERPANEL_H
#define NATRON_GUI_CAMERATRACKERPANEL_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include <vector>

#include "Global/GlobalDefines.h"   // SequenceTime
#include "Engine/EngineFwd.h"

class QItemSelection;

NATRON_NAMESPACE_ENTER

class CameraTrackerNode;
class TableView;
class TableModel;
class TableItem;

/**
 * @brief Manual-track table for the CameraTracker node (Tracker-panel style).
 *
 * Lives in the node's settings panel (added by NodeSettingsPanel::initializeExtraGui
 * when the node is a CameraTrackerNode). Built on the same generic
 * TableModel/TableView widgets as the built-in Tracker node's panel — one row per
 * hand-placed track: label, keyed-frame span, position at the current frame
 * (editable), and solve error. Row selection syncs with the viewer selection;
 * +/- buttons add and delete tracks.
 *
 * Data binding is through CameraTrackerNode's small panel API
 * (getManualTracksInfo / panelSelect / panelDelete / panelSetPosition /
 * panelAddManualTrack) and its manualTracksChanged() signal — the panel owns no
 * track state of its own.
 */
class CameraTrackerPanel
    : public QWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    CameraTrackerPanel(CameraTrackerNode* node, QWidget* parent = nullptr);
    virtual ~CameraTrackerPanel();

public Q_SLOTS:
    void onTracksChanged();
    void onItemDataChanged(TableItem* item);
    void onModelSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);
    void onAddClicked();
    void onRemoveClicked();
    void onTimelineFrameChanged(SequenceTime time, int reason);

private:
    void rebuild();
    double currentTime() const;

    CameraTrackerNode* _node;      // not owned
    TableView* _view;
    TableModel* _model;
    std::vector<int> _rowIds;      // row -> manual track id
    bool _building;                // guard: table is being (re)built
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_CAMERATRACKERPANEL_H
