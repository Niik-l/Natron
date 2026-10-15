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

#include "ArchiveTreeWidget.h"

#include <functional>
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
#include "Engine/EffectInstance.h"
#include "Engine/Dev/Scene3D/ArchiveProvider.h"

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

ArchiveTreeWidget::ArchiveTreeWidget(EffectInstance* archive, QWidget* parent)
    : QWidget(parent)
    , _archive(archive)
    , _provider(archive ? dynamic_cast<ArchiveProvider*>(archive) : NULL)
    , _search(nullptr)
    , _tree(nullptr)
    , _suppressItemChanged(false)
    , _applyingExcluded(false)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 4, 2, 2);
    mainLayout->setSpacing(4);
    // The tree is the widget's body — let the whole widget expand with it.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

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
    QObject::connect(refreshBtn, &QPushButton::clicked, this, &ArchiveTreeWidget::onRefreshClicked);
    toolbar->addWidget(refreshBtn);

    QPushButton* checkAllBtn = new QPushButton(tr("Check All"), this);
    QObject::connect(checkAllBtn, &QPushButton::clicked, this, &ArchiveTreeWidget::onCheckAllClicked);
    toolbar->addWidget(checkAllBtn);

    QPushButton* uncheckAllBtn = new QPushButton(tr("Uncheck All"), this);
    QObject::connect(uncheckAllBtn, &QPushButton::clicked, this, &ArchiveTreeWidget::onUncheckAllClicked);
    toolbar->addWidget(uncheckAllBtn);

    mainLayout->addLayout(toolbar);

    // Search bar
    _search = new QLineEdit(this);
    _search->setPlaceholderText(tr("Filter by name..."));
    _search->setClearButtonEnabled(true);
    QObject::connect(_search, &QLineEdit::textChanged, this, &ArchiveTreeWidget::onSearchTextChanged);
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
        // The name column takes the leftover width so long paths stay readable.
        h->setSectionResizeMode(0, QHeaderView::Stretch);
        h->setSectionResizeMode(1, QHeaderView::Fixed);
        h->setSectionResizeMode(2, QHeaderView::Fixed);
        h->setSectionResizeMode(3, QHeaderView::Fixed);
    }
    // Enough room to actually navigate large hierarchies (the archive
    // contents view is the main tool on this panel — give it the space).
    _tree->setMinimumHeight(360);
    _tree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QObject::connect(_tree, &QTreeWidget::itemChanged, this, &ArchiveTreeWidget::onItemChanged);

    // Right-click context menu
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(_tree, &QTreeWidget::customContextMenuRequested,
                     this, &ArchiveTreeWidget::onTreeContextMenuRequested);

    mainLayout->addWidget(_tree);

    // Live refresh - when the archive reloads (filename change, Reload click,
    // filter knob change), the tree rebuilds automatically. Both archive nodes
    // (ReadAlembicArchive / ReadGeoArchive) declare the same archiveReloaded()
    // signal, so the connection is string-based to stay class-agnostic.
    if (_archive) {
        QObject::connect(_archive, SIGNAL(archiveReloaded()),
                         this, SLOT(refresh()));
    }

    rebuildFromArchive();
}

ArchiveTreeWidget::~ArchiveTreeWidget()
{
}

void
ArchiveTreeWidget::refresh()
{
    // Skip the rebuild when WE triggered the reload by writing excludedPaths: the
    // archive's entry set is unchanged (only its visibility filter), the tree
    // already shows the right check states, and rebuilding here would run
    // _tree->clear() synchronously inside the itemChanged signal that started the
    // write - deleting the clicked item out from under Qt (use-after-free crash).
    // Legitimate reloads (file change, Reload button) still rebuild, and the
    // Refresh button calls rebuildFromArchive() directly.
    if (_applyingExcluded) return;
    rebuildFromArchive();
}

void
ArchiveTreeWidget::onRefreshClicked()
{
    rebuildFromArchive();
}

void
ArchiveTreeWidget::rebuildFromArchive()
{
    if (!_archive || !_provider || !_tree) return;

    _suppressItemChanged = true;
    _tree->clear();

    const std::vector<ArchiveTreeEntry> entries = _provider->getEntryTree();

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
    // Check-state model: each item's OWN state (in excludedPaths or not) is
    // kept in UserRole+1; the DISPLAYED check state is the effective one —
    // unchecked when the item itself or ANY ancestor is own-unchecked (the
    // engine applies the same subtree rule, so the display matches the
    // scene). writeExcludedPathsToKnob persists the MINIMAL frontier: items
    // that are own-unchecked but have no own-unchecked ancestor — so
    // re-checking a parent re-include its whole subtree.
    std::vector<QTreeWidgetItem*> items(entries.size(), nullptr);
    std::vector<char> ancOwnUnchecked(entries.size(), 0);
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

        const bool isExcluded = excluded.count(e.fullPath) != 0;
        const bool ancUnchecked = (e.parentLocalIdx >= 0 && e.parentLocalIdx < (int)entries.size())
                                      ? (ancOwnUnchecked[e.parentLocalIdx] != 0) : false;
        const bool effectiveUnchecked = isExcluded || ancUnchecked;
        ancOwnUnchecked[i] = effectiveUnchecked ? 1 : 0;

        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, effectiveUnchecked ? Qt::Unchecked : Qt::Checked);
        // Own state (what the knob carries) — the write path uses this.
        item->setData(0, Qt::UserRole + 1, isExcluded ? 1 : 0);

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
ArchiveTreeWidget::onSearchTextChanged(const QString& text)
{
    applySearchFilter(text);
}

void
ArchiveTreeWidget::applySearchFilter(const QString& text)
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
ArchiveTreeWidget::onTreeContextMenuRequested(const QPoint& pos)
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

    QObject::connect(expandAll,     &QAction::triggered, this, &ArchiveTreeWidget::onExpandAllTriggered);
    QObject::connect(collapseAll,   &QAction::triggered, this, &ArchiveTreeWidget::onCollapseAllTriggered);
    QObject::connect(checkBranch,   &QAction::triggered, this, &ArchiveTreeWidget::onCheckBranchTriggered);
    QObject::connect(uncheckBranch, &QAction::triggered, this, &ArchiveTreeWidget::onUncheckBranchTriggered);

    menu.exec(_tree->viewport()->mapToGlobal(pos));
}

void
ArchiveTreeWidget::onExpandAllTriggered()
{
    if (_tree) _tree->expandAll();
}

void
ArchiveTreeWidget::onCollapseAllTriggered()
{
    if (_tree) _tree->collapseAll();
}

void
ArchiveTreeWidget::setBranchChecked(QTreeWidgetItem* root, bool checked)
{
    if (!root) return;
    _suppressItemChanged = true;
    // Own-state model: the branch's OWN states are set; the displayed states
    // follow via redisplayChecks() from the caller.
    root->setData(0, Qt::UserRole + 1, checked ? 0 : 1);
    for (int i = 0; i < root->childCount(); ++i) {
        setBranchChecked(root->child(i), checked);
    }
    _suppressItemChanged = false;
}

void
ArchiveTreeWidget::onCheckBranchTriggered()
{
    if (!_tree) return;
    QTreeWidgetItem* sel = _tree->currentItem();
    if (!sel) return;
    setBranchChecked(sel, true);
    redisplayChecks();
    writeExcludedPathsToKnob();
}

void
ArchiveTreeWidget::onUncheckBranchTriggered()
{
    if (!_tree) return;
    QTreeWidgetItem* sel = _tree->currentItem();
    if (!sel) return;
    setBranchChecked(sel, false);
    redisplayChecks();
    writeExcludedPathsToKnob();
}

void
ArchiveTreeWidget::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (_suppressItemChanged) return;
    if (column != 0) return; // only the checkbox column matters
    if (!item) return;

    // The click expresses intent for THIS item: its own state becomes the
    // clicked state. Checking a child under an unchecked parent propagates
    // the check UP the ancestor chain so the click takes visible effect.
    const bool nowChecked = (item->checkState(0) == Qt::Checked);
    item->setData(0, Qt::UserRole + 1, nowChecked ? 0 : 1);
    if (nowChecked) {
        QTreeWidgetItem* p = item->parent();
        while (p) {
            p->setData(0, Qt::UserRole + 1, 0);
            p = p->parent();
        }
    }
    redisplayChecks();
    writeExcludedPathsToKnob();
}

// Effective check state = own-unchecked OR any ancestor own-unchecked (the
// engine's subtree rule — the display must match the scene).
void
ArchiveTreeWidget::redisplayChecks()
{
    if (!_tree) return;
    std::function<void(QTreeWidgetItem*, bool)> walk =
        [&](QTreeWidgetItem* it, bool ancOwnUnchecked) {
        if (!it) return;
        const bool own = (it->data(0, Qt::UserRole + 1).toInt() != 0);
        const bool effUnchecked = own || ancOwnUnchecked;
        _suppressItemChanged = true;
        it->setCheckState(0, effUnchecked ? Qt::Unchecked : Qt::Checked);
        _suppressItemChanged = false;
        for (int i = 0; i < it->childCount(); ++i) {
            walk(it->child(i), effUnchecked);
        }
    };
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        walk(_tree->topLevelItem(i), false);
    }
}

void
ArchiveTreeWidget::onCheckAllClicked()
{
    if (!_tree) return;
    _suppressItemChanged = true;
    QTreeWidgetItemIterator it(_tree);
    while (*it) { (*it)->setData(0, Qt::UserRole + 1, 0); ++it; }
    _suppressItemChanged = false;
    redisplayChecks();
    writeExcludedPathsToKnob();
}

void
ArchiveTreeWidget::onUncheckAllClicked()
{
    if (!_tree) return;
    _suppressItemChanged = true;
    QTreeWidgetItemIterator it(_tree);
    while (*it) { (*it)->setData(0, Qt::UserRole + 1, 1); ++it; }
    _suppressItemChanged = false;
    redisplayChecks();
    writeExcludedPathsToKnob();
}

void
ArchiveTreeWidget::writeExcludedPathsToKnob()
{
    if (!_archive || !_tree) return;

    // Persist the MINIMAL frontier of the own-unchecked set: items that are
    // own-unchecked but have NO own-unchecked ancestor (the engine's subtree
    // rule already hides the descendants of an excluded ancestor, and keeping
    // only the frontier lets re-checking a parent re-include its subtree).
    std::ostringstream ss;
    bool first = true;
    std::function<void(QTreeWidgetItem*, bool)> walk =
        [&](QTreeWidgetItem* it, bool ancOwnUnchecked) {
        if (!it) return;
        const bool own = (it->data(0, Qt::UserRole + 1).toInt() != 0);
        const bool effUnchecked = own || ancOwnUnchecked;
        if (own && !ancOwnUnchecked) {
            QString path = it->data(0, Qt::UserRole).toString();
            if (!path.isEmpty()) {
                if (!first) ss << '\n';
                ss << path.toStdString();
                first = false;
            }
        }
        for (int i = 0; i < it->childCount(); ++i) {
            walk(it->child(i), effUnchecked);
        }
    };
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        walk(_tree->topLevelItem(i), false);
    }
KnobIPtr ep = _archive->getKnobByName("excludedPaths");
    KnobString* es = dynamic_cast<KnobString*>(ep.get());
    if (es) {
        // setValue -> knobChanged -> loadAlembicFile -> archiveReloaded -> refresh().
        // That cascade is synchronous and we're inside an itemChanged signal, so
        // guard refresh() against rebuilding the tree (and freeing the live item)
        // until this returns.
        _applyingExcluded = true;
        es->setValue(ss.str()); // triggers knobChanged -> archive re-filters
        _applyingExcluded = false;
    }
}

NATRON_NAMESPACE_EXIT

#include "moc_ArchiveTreeWidget.cpp"
