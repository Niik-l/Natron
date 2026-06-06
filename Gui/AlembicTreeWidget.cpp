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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AlembicTreeWidget.h"

#include <set>
#include <sstream>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/KnobTypes.h"
#include "Engine/Dev/Scene3D/ReadAlembicArchive.h"

NATRON_NAMESPACE_ENTER

// Small helpers for the tree-row type column.
static QIcon
makeTypeIcon(bool isMesh)
{
    QPixmap pix(12, 12);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (isMesh) {
        // Mesh: small triangle in soft green.
        p.setBrush(QColor(120, 180, 110));
        p.setPen(QColor(80, 130, 70));
        QPolygon tri;
        tri << QPoint(1, 10) << QPoint(11, 10) << QPoint(6, 1);
        p.drawPolygon(tri);
    } else {
        // Xform: small RGB axis cross in muted yellow-orange.
        p.setPen(QPen(QColor(220, 170, 80), 1.6));
        p.drawLine(2, 6, 10, 6);
        p.drawLine(6, 2, 6, 10);
    }
    return QIcon(pix);
}

AlembicTreeWidget::AlembicTreeWidget(ReadAlembicArchive* archive, QWidget* parent)
    : QWidget(parent)
    , _archive(archive)
    , _search(nullptr)
    , _tree(nullptr)
    , _suppressItemChanged(false)
    , _applyingExcluded(false)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 4, 2, 2);
    mainLayout->setSpacing(4);

    // Toolbar: label + buttons
    QHBoxLayout* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(4);

    QLabel* heading = new QLabel(tr("Archive Contents"), this);
    QFont f = heading->font();
    f.setBold(true);
    heading->setFont(f);
    toolbar->addWidget(heading);
    toolbar->addStretch();

    QPushButton* refreshBtn = new QPushButton(tr("Refresh"), this);
    refreshBtn->setToolTip(tr("Rebuild the tree from the archive's current state."));
    QObject::connect(refreshBtn, &QPushButton::clicked, this, &AlembicTreeWidget::onRefreshClicked);
    toolbar->addWidget(refreshBtn);

    QPushButton* checkAllBtn = new QPushButton(tr("Check All"), this);
    QObject::connect(checkAllBtn, &QPushButton::clicked, this, &AlembicTreeWidget::onCheckAllClicked);
    toolbar->addWidget(checkAllBtn);

    QPushButton* uncheckAllBtn = new QPushButton(tr("Uncheck All"), this);
    QObject::connect(uncheckAllBtn, &QPushButton::clicked, this, &AlembicTreeWidget::onUncheckAllClicked);
    toolbar->addWidget(uncheckAllBtn);

    mainLayout->addLayout(toolbar);

    // Search bar
    _search = new QLineEdit(this);
    _search->setPlaceholderText(tr("Filter by name..."));
    _search->setClearButtonEnabled(true);
    QObject::connect(_search, &QLineEdit::textChanged, this, &AlembicTreeWidget::onSearchTextChanged);
    mainLayout->addWidget(_search);

    // Tree
    _tree = new QTreeWidget(this);
    _tree->setColumnCount(4);
    QStringList headers;
    headers << tr("Name") << tr("Type") << tr("Verts") << tr("Frames");
    _tree->setHeaderLabels(headers);
    _tree->setRootIsDecorated(true);
    _tree->setAlternatingRowColors(true);
    _tree->setUniformRowHeights(true);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    // Dark-theme palette to match Natron's panel background.
    _tree->setStyleSheet(QString::fromUtf8(
        "QTreeWidget {"
        "  background-color: #2a2a2a;"
        "  alternate-background-color: #323232;"
        "  color: #cccccc;"
        "  border: 1px solid #1c1c1c;"
        "  outline: 0;"
        "}"
        "QTreeWidget::item {"
        "  padding: 1px 0px;"
        "}"
        "QTreeWidget::item:hover {"
        "  background-color: #3a3a3a;"
        "}"
        "QTreeWidget::item:selected {"
        "  background-color: #4a5b6e;"
        "  color: #ffffff;"
        "}"
        "QTreeWidget::branch:has-children:!has-siblings:closed,"
        "QTreeWidget::branch:closed:has-children:has-siblings {"
        "  border-image: none;"
        "}"
        "QHeaderView::section {"
        "  background-color: #353535;"
        "  color: #cccccc;"
        "  padding: 4px;"
        "  border: 0px;"
        "  border-right: 1px solid #1c1c1c;"
        "  border-bottom: 1px solid #1c1c1c;"
        "}"
    ));

    if (QHeaderView* h = _tree->header()) {
        h->setStretchLastSection(false);
        h->resizeSection(0, 280);
        h->resizeSection(1, 70);
        h->resizeSection(2, 70);
        h->resizeSection(3, 70);
    }
    _tree->setMinimumHeight(180);

    QObject::connect(_tree, &QTreeWidget::itemChanged, this, &AlembicTreeWidget::onItemChanged);

    // Right-click context menu
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(_tree, &QTreeWidget::customContextMenuRequested,
                     this, &AlembicTreeWidget::onTreeContextMenuRequested);

    mainLayout->addWidget(_tree);

    // Live refresh — when the archive reloads (filename change, Reload click,
    // filter knob change), the tree rebuilds automatically.
    if (_archive) {
        QObject::connect(_archive, &ReadAlembicArchive::archiveReloaded,
                         this, &AlembicTreeWidget::refresh);
    }

    rebuildFromArchive();
}

AlembicTreeWidget::~AlembicTreeWidget()
{
}

void
AlembicTreeWidget::refresh()
{
    // Skip the rebuild when WE triggered the reload by writing excludedPaths: the
    // archive's entry set is unchanged (only its visibility filter), the tree
    // already shows the right check states, and rebuilding here would run
    // _tree->clear() synchronously inside the itemChanged signal that started the
    // write — deleting the clicked item out from under Qt (use-after-free crash).
    // Legitimate reloads (file change, Reload button) still rebuild, and the
    // Refresh button calls rebuildFromArchive() directly.
    if (_applyingExcluded) return;
    rebuildFromArchive();
}

void
AlembicTreeWidget::onRefreshClicked()
{
    rebuildFromArchive();
}

void
AlembicTreeWidget::rebuildFromArchive()
{
    if (!_archive || !_tree) return;

    _suppressItemChanged = true;
    _tree->clear();

    const std::vector<ArchiveTreeEntry> entries = _archive->getEntryTree();

    // Read the current excluded set from the knob.
    std::set<std::string> excluded;
    {
        KnobIPtr ep = _archive->getKnobByName("excludedPaths");
        if (KnobString* es = dynamic_cast<KnobString*>(ep.get())) {
            const std::string raw = es->getValue();
            std::istringstream iss(raw);
            std::string line;
            while (std::getline(iss, line)) {
                // Trim CR (Windows line endings).
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                if (!line.empty()) excluded.insert(line);
            }
        }
    }

    // Build items in walk order so parents exist before children.
    std::vector<QTreeWidgetItem*> items(entries.size(), nullptr);
    for (size_t i = 0; i < entries.size(); ++i) {
        const ArchiveTreeEntry& e = entries[i];

        QTreeWidgetItem* parent = (e.parentLocalIdx >= 0 && e.parentLocalIdx < (int)items.size())
            ? items[e.parentLocalIdx] : nullptr;

        QTreeWidgetItem* item = parent
            ? new QTreeWidgetItem(parent)
            : new QTreeWidgetItem(_tree);

        item->setText(0, QString::fromStdString(e.name));
        item->setIcon(1, makeTypeIcon(e.isMesh));
        item->setText(1, e.isMesh ? tr("mesh") : tr("xform"));
        item->setText(2, e.numVertices > 0 ? QString::number(e.numVertices) : QString());
        item->setText(3, e.sampleCount > 0 ? QString::number(e.sampleCount) : QString());
        item->setToolTip(0, QString::fromStdString(e.fullPath));
        item->setData(0, Qt::UserRole, QString::fromStdString(e.fullPath));

        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool isExcluded = excluded.count(e.fullPath) != 0;
        item->setCheckState(0, isExcluded ? Qt::Unchecked : Qt::Checked);

        items[i] = item;
    }

    _tree->expandAll();
    _suppressItemChanged = false;

    // Re-apply any active search filter.
    if (_search && !_search->text().isEmpty()) {
        applySearchFilter(_search->text());
    }
}

void
AlembicTreeWidget::onSearchTextChanged(const QString& text)
{
    applySearchFilter(text);
}

void
AlembicTreeWidget::applySearchFilter(const QString& text)
{
    if (!_tree) return;
    const QString needle = text.trimmed().toLower();

    // Walk every item. Hide rows whose name doesn't match; show all when query is empty.
    // For matching rows, also show their ancestors (so the path stays visible) and
    // expand them so the match is on-screen.
    QTreeWidgetItemIterator it(_tree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        if (needle.isEmpty()) {
            item->setHidden(false);
        } else {
            const bool nameMatches = item->text(0).toLower().contains(needle)
                                  || item->toolTip(0).toLower().contains(needle);
            item->setHidden(!nameMatches);
            if (nameMatches) {
                // Unhide ancestors and expand them.
                QTreeWidgetItem* p = item->parent();
                while (p) {
                    p->setHidden(false);
                    p->setExpanded(true);
                    p = p->parent();
                }
            }
        }
        ++it;
    }
}

void
AlembicTreeWidget::onTreeContextMenuRequested(const QPoint& pos)
{
    if (!_tree) return;
    QMenu menu(_tree);

    QAction* expandAll   = menu.addAction(tr("Expand All"));
    QAction* collapseAll = menu.addAction(tr("Collapse All"));
    menu.addSeparator();
    QAction* checkBranch   = menu.addAction(tr("Check Branch"));
    QAction* uncheckBranch = menu.addAction(tr("Uncheck Branch"));
    const bool haveItem = _tree->itemAt(pos) != nullptr;
    checkBranch->setEnabled(haveItem);
    uncheckBranch->setEnabled(haveItem);

    QObject::connect(expandAll,     &QAction::triggered, this, &AlembicTreeWidget::onExpandAllTriggered);
    QObject::connect(collapseAll,   &QAction::triggered, this, &AlembicTreeWidget::onCollapseAllTriggered);
    QObject::connect(checkBranch,   &QAction::triggered, this, &AlembicTreeWidget::onCheckBranchTriggered);
    QObject::connect(uncheckBranch, &QAction::triggered, this, &AlembicTreeWidget::onUncheckBranchTriggered);

    menu.exec(_tree->viewport()->mapToGlobal(pos));
}

void
AlembicTreeWidget::onExpandAllTriggered()
{
    if (_tree) _tree->expandAll();
}

void
AlembicTreeWidget::onCollapseAllTriggered()
{
    if (_tree) _tree->collapseAll();
}

void
AlembicTreeWidget::setBranchChecked(QTreeWidgetItem* root, bool checked)
{
    if (!root) return;
    _suppressItemChanged = true;
    root->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
    for (int i = 0; i < root->childCount(); ++i) {
        setBranchChecked(root->child(i), checked);
    }
    _suppressItemChanged = false;
}

void
AlembicTreeWidget::onCheckBranchTriggered()
{
    if (!_tree) return;
    QTreeWidgetItem* sel = _tree->currentItem();
    if (!sel) return;
    setBranchChecked(sel, true);
    writeExcludedPathsToKnob();
}

void
AlembicTreeWidget::onUncheckBranchTriggered()
{
    if (!_tree) return;
    QTreeWidgetItem* sel = _tree->currentItem();
    if (!sel) return;
    setBranchChecked(sel, false);
    writeExcludedPathsToKnob();
}

void
AlembicTreeWidget::onItemChanged(QTreeWidgetItem* /*item*/, int column)
{
    if (_suppressItemChanged) return;
    if (column != 0) return; // only the checkbox column matters
    writeExcludedPathsToKnob();
}

void
AlembicTreeWidget::onCheckAllClicked()
{
    if (!_tree) return;
    _suppressItemChanged = true;
    QTreeWidgetItemIterator it(_tree);
    while (*it) { (*it)->setCheckState(0, Qt::Checked); ++it; }
    _suppressItemChanged = false;
    writeExcludedPathsToKnob();
}

void
AlembicTreeWidget::onUncheckAllClicked()
{
    if (!_tree) return;
    _suppressItemChanged = true;
    QTreeWidgetItemIterator it(_tree);
    while (*it) { (*it)->setCheckState(0, Qt::Unchecked); ++it; }
    _suppressItemChanged = false;
    writeExcludedPathsToKnob();
}

void
AlembicTreeWidget::writeExcludedPathsToKnob()
{
    if (!_archive || !_tree) return;

    // Collect paths of every unchecked item.
    std::ostringstream ss;
    bool first = true;
    QTreeWidgetItemIterator it(_tree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        if (item->checkState(0) == Qt::Unchecked) {
            QString path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty()) {
                if (!first) ss << '\n';
                ss << path.toStdString();
                first = false;
            }
        }
        ++it;
    }

    KnobIPtr ep = _archive->getKnobByName("excludedPaths");
    KnobString* es = dynamic_cast<KnobString*>(ep.get());
    if (es) {
        // setValue → knobChanged → loadAlembicFile → archiveReloaded → refresh().
        // That cascade is synchronous and we're inside an itemChanged signal, so
        // guard refresh() against rebuilding the tree (and freeing the live item)
        // until this returns.
        _applyingExcluded = true;
        es->setValue(ss.str()); // triggers knobChanged → archive re-filters
        _applyingExcluded = false;
    }
}

NATRON_NAMESPACE_EXIT

#include "moc_AlembicTreeWidget.cpp"
