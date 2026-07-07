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

#include "CameraTrackerPanel.h"

#include <sstream>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QItemSelectionModel>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/CameraTrackerNode.h"
#include "Engine/TimeLine.h"

#include "Gui/TableModelView.h"

NATRON_NAMESPACE_ENTER

enum ManualTrackCols
{
    COL_LABEL = 0,
    COL_FRAMES,
    COL_X,
    COL_Y,
    COL_ERROR,
    NUM_COLS
};

CameraTrackerPanel::CameraTrackerPanel(CameraTrackerNode* node,
                                       QWidget* parent)
    : QWidget(parent)
    , _node(node)
    , _view(0)
    , _model(0)
    , _rowIds()
    , _building(false)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    // (No title label — this widget sits directly under the Manual Tracks
    // group on the Tracking page and shows/hides with it, so the group header
    // is the section title.)

    // Same generic table widgets the built-in Tracker's panel is built on.
    _view = new TableView(this);
    QObject::connect( _view, SIGNAL(deleteKeyPressed()), this, SLOT(onRemoveClicked()) );
    _model = new TableModel(0, 0, _view);
    QObject::connect( _model, SIGNAL(s_itemChanged(TableItem*)), this, SLOT(onItemDataChanged(TableItem*)) );
    _view->setTableModel(_model);
    _view->setUniformRowHeights(true);
    QItemSelectionModel* selectionModel = _view->selectionModel();
    QObject::connect( selectionModel, SIGNAL(selectionChanged(QItemSelection,QItemSelection)), this,
                      SLOT(onModelSelectionChanged(QItemSelection,QItemSelection)) );
    QStringList headers;
    headers << tr("Label") << tr("Frames") << tr("X") << tr("Y") << tr("Error");
    _view->setColumnCount(NUM_COLS);
    _view->setHorizontalHeaderLabels(headers);
    _view->setAttribute(Qt::WA_MacShowFocusRect, 0);
    mainLayout->addWidget(_view);

    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* addBtn = new QPushButton(QString::fromUtf8("+"), this);
    addBtn->setToolTip( tr("Add a manual track at the image center on the current frame "
                           "(drag it into place in the viewer)") );
    addBtn->setFixedWidth(30);
    QObject::connect( addBtn, SIGNAL(clicked()), this, SLOT(onAddClicked()) );
    buttons->addWidget(addBtn);
    QPushButton* delBtn = new QPushButton(QString::fromUtf8("-"), this);
    delBtn->setToolTip( tr("Delete the selected manual track") );
    delBtn->setFixedWidth(30);
    QObject::connect( delBtn, SIGNAL(clicked()), this, SLOT(onRemoveClicked()) );
    buttons->addWidget(delBtn);
    buttons->addStretch();
    mainLayout->addLayout(buttons);

    if (_node) {
        QObject::connect( _node, SIGNAL(manualTracksChanged()), this, SLOT(onTracksChanged()) );
        AppInstancePtr app = _node->getApp();
        if (app && app->getTimeLine()) {
            QObject::connect( app->getTimeLine().get(), SIGNAL(frameChanged(SequenceTime,int)),
                              this, SLOT(onTimelineFrameChanged(SequenceTime,int)) );
        }
    }

    rebuild();
}

CameraTrackerPanel::~CameraTrackerPanel()
{
}

double
CameraTrackerPanel::currentTime() const
{
    if (_node) {
        AppInstancePtr app = _node->getApp();
        if (app && app->getTimeLine()) {
            return (double)app->getTimeLine()->currentFrame();
        }
    }
    return 1.0;
}

void
CameraTrackerPanel::rebuild()
{
    if (!_node) return;
    // (Visibility is owned by KnobGuiTracksTable's _show/_hide — the generic
    // group-collapse path — so no setVisible here.)
    _building = true;

    while (_view->rowCount() > 0) {
        _model->removeRows(0);
    }
    _rowIds.clear();

    const std::vector<CameraTrackerNode::ManualTrackInfo> infos =
        _node->getManualTracksInfo( currentTime() );
    int selRow = -1;
    for (std::size_t i = 0; i < infos.size(); ++i) {
        const CameraTrackerNode::ManualTrackInfo& info = infos[i];
        const int row = _view->rowCount();
        _model->insertRow(row);
        _rowIds.push_back(info.id);
        if (info.selected) selRow = row;

        {
            TableItem* it = new TableItem;
            std::stringstream nm; nm << "track " << info.id;
            it->setText( QString::fromUtf8( nm.str().c_str() ) );
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            _view->setItem(row, COL_LABEL, it);
        }
        {
            TableItem* it = new TableItem;
            std::stringstream fr;
            fr << info.nFrames << " (" << info.firstFrame << "-" << info.lastFrame << ")";
            it->setText( QString::fromUtf8( fr.str().c_str() ) );
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            _view->setItem(row, COL_FRAMES, it);
        }
        {
            TableItem* it = new TableItem;
            it->setData(Qt::DisplayRole, info.x);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            it->setToolTip( tr("Marker X at the current frame — edit to reposition") );
            _view->setItem(row, COL_X, it);
        }
        {
            TableItem* it = new TableItem;
            it->setData(Qt::DisplayRole, info.y);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            it->setToolTip( tr("Marker Y at the current frame — edit to reposition") );
            _view->setItem(row, COL_Y, it);
        }
        {
            TableItem* it = new TableItem;
            if (info.error >= 0.0) it->setData(Qt::DisplayRole, info.error);
            else if (info.error <= -1.5) it->setText( tr("REJECTED") );
            else it->setText( QString::fromUtf8("-") );
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            _view->setItem(row, COL_ERROR, it);
        }
    }

    if (selRow >= 0) {
        const QModelIndex left = _model->index(selRow, 0);
        const QModelIndex right = _model->index(selRow, NUM_COLS - 1);
        _view->selectionModel()->select(QItemSelection(left, right),
                                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    _building = false;
}

void
CameraTrackerPanel::onTracksChanged()
{
    if (_building) return;
    rebuild();
}

void
CameraTrackerPanel::onTimelineFrameChanged(SequenceTime /*time*/, int /*reason*/)
{
    // Positions are per-frame; refresh X/Y (cheap full rebuild — small N).
    if (_building) return;
    rebuild();
}

void
CameraTrackerPanel::onItemDataChanged(TableItem* item)
{
    if (_building || !_node || !item) return;

    // Locate the edited cell.
    int row = -1, col = -1;
    for (int r = 0; r < _view->rowCount() && row < 0; ++r) {
        for (int c = 0; c < NUM_COLS; ++c) {
            if (_view->item(r, c) == item) { row = r; col = c; break; }
        }
    }
    if (row < 0 || row >= (int)_rowIds.size()) return;
    if (col != COL_X && col != COL_Y) return;

    TableItem* xIt = _view->item(row, COL_X);
    TableItem* yIt = _view->item(row, COL_Y);
    if (!xIt || !yIt) return;
    const double x = xIt->data(Qt::DisplayRole).toDouble();
    const double y = yIt->data(Qt::DisplayRole).toDouble();
    _node->panelSetManualTrackPosition(_rowIds[row], currentTime(), x, y);
}

void
CameraTrackerPanel::onModelSelectionChanged(const QItemSelection& selected,
                                            const QItemSelection& /*deselected*/)
{
    if (_building || !_node) return;
    const QModelIndexList indexes = selected.indexes();
    if ( indexes.isEmpty() ) return;
    const int row = indexes.front().row();
    if (row >= 0 && row < (int)_rowIds.size()) {
        _node->panelSelectManualTrack(_rowIds[row]);
    }
}

void
CameraTrackerPanel::onAddClicked()
{
    if (!_node) return;
    _node->panelAddManualTrack( currentTime() );
}

void
CameraTrackerPanel::onRemoveClicked()
{
    if (!_node) return;
    // Delete the row currently selected in the table (falls back to the node's
    // viewer selection through panelDeleteManualTrack semantics).
    const QModelIndexList indexes = _view->selectionModel()->selectedRows();
    if ( !indexes.isEmpty() ) {
        const int row = indexes.front().row();
        if (row >= 0 && row < (int)_rowIds.size()) {
            _node->panelDeleteManualTrack(_rowIds[row]);
            return;
        }
    }
}

NATRON_NAMESPACE_EXIT

#include "moc_CameraTrackerPanel.cpp"
