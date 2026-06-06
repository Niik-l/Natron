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

#include "MaterialOverridePickerWidget.h"

#include <set>
#include <sstream>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Dev/Scene3D/GeoMaterialOverride.h"
#include "Engine/Dev/Scene3D/ReadAlembicArchive.h"

NATRON_NAMESPACE_ENTER

static QIcon
makeTypeIcon(bool isMesh)
{
    QPixmap pix(12, 12);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (isMesh) {
        p.setBrush(QColor(120, 180, 110));
        p.setPen(QColor(80, 130, 70));
        QPolygon tri;
        tri << QPoint(1, 10) << QPoint(11, 10) << QPoint(6, 1);
        p.drawPolygon(tri);
    } else {
        p.setPen(QPen(QColor(220, 170, 80), 1.6));
        p.drawLine(2, 6, 10, 6);
        p.drawLine(6, 2, 6, 10);
    }
    return QIcon(pix);
}

// Walk the override's Geo input (input 0) through Dots and chained overrides to
// the upstream ReadAlembicArchive. Null if none is connected.
static ReadAlembicArchive*
resolveArchive(GeoMaterialOverride* ov)
{
    if (!ov) return NULL;
    EffectInstancePtr geo = ov->getInput(0);
    while (geo) {
        if (geo->getPluginID() == PLUGINID_NATRON_DOT) { geo = geo->getInput(0); continue; }
        if (dynamic_cast<GeoMaterialOverride*>(geo.get())) { geo = geo->getInput(0); continue; }
        break;
    }
    return dynamic_cast<ReadAlembicArchive*>(geo.get());
}

// Parse the node's `surfaces` knob into a set of tokens (one per non-blank line).
static void
readSurfaceSet(GeoMaterialOverride* ov, std::set<std::string>& out)
{
    if (!ov) return;
    KnobIPtr k = ov->getKnobByName("surfaces");
    KnobString* ks = dynamic_cast<KnobString*>(k.get());
    if (!ks) return;
    std::istringstream iss(ks->getValue());
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        if (lead) line.erase(0, lead);
        if (!line.empty()) out.insert(line);
    }
}

MaterialOverridePickerWidget::MaterialOverridePickerWidget(GeoMaterialOverride* ov, QWidget* parent)
    : QWidget(parent)
    , _ov(ov)
    , _tree(NULL)
    , _suppressItemChanged(false)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 4, 2, 2);
    mainLayout->setSpacing(4);

    QHBoxLayout* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(4);

    QLabel* heading = new QLabel(tr("Override Surfaces"), this);
    QFont f = heading->font();
    f.setBold(true);
    heading->setFont(f);
    toolbar->addWidget(heading);
    toolbar->addStretch();

    QPushButton* refreshBtn = new QPushButton(tr("Refresh"), this);
    refreshBtn->setToolTip(tr("Re-read the geometry connected to the Geo input."));
    toolbar->addWidget(refreshBtn);
    mainLayout->addLayout(toolbar);

    QLabel* hint = new QLabel(tr("Tick the surfaces to shade with this node's Mat input."), this);
    hint->setWordWrap(true);
    mainLayout->addWidget(hint);

    _tree = new QTreeWidget(this);
    _tree->setColumnCount(1);
    _tree->setHeaderLabel(tr("Surface"));
    _tree->setRootIsDecorated(true);
    mainLayout->addWidget(_tree);

    // Lambda connections — no Q_OBJECT / moc needed. `this` is the context object
    // (a QWidget = QObject), so the connections auto-disconnect on destruction.
    QObject::connect(refreshBtn, &QPushButton::clicked, this,
                     [this]() { rebuildFromGeo(); });
    QObject::connect(_tree, &QTreeWidget::itemChanged, this,
                     [this](QTreeWidgetItem* /*item*/, int column) {
                         if (_suppressItemChanged) return;
                         if (column != 0) return;
                         writeSurfacesToKnob();
                     });

    rebuildFromGeo();
}

MaterialOverridePickerWidget::~MaterialOverridePickerWidget()
{
}

void
MaterialOverridePickerWidget::rebuildFromGeo()
{
    if (!_tree) return;

    _suppressItemChanged = true;
    _tree->clear();

    ReadAlembicArchive* archive = resolveArchive(_ov);
    if (!archive) {
        QTreeWidgetItem* item = new QTreeWidgetItem(_tree);
        item->setText(0, tr("(connect an Alembic archive to the Geo input)"));
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        _suppressItemChanged = false;
        return;
    }

    // Already-overridden tokens (full paths and/or bare leaf names).
    std::set<std::string> selected;
    readSurfaceSet(_ov, selected);

    const std::vector<ArchiveTreeEntry> entries = archive->getEntryTree();
    std::vector<QTreeWidgetItem*> items(entries.size(), NULL);
    for (size_t i = 0; i < entries.size(); ++i) {
        const ArchiveTreeEntry& e = entries[i];
        QTreeWidgetItem* parent = (e.parentLocalIdx >= 0 && e.parentLocalIdx < (int)items.size())
            ? items[e.parentLocalIdx] : NULL;
        QTreeWidgetItem* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(_tree);

        item->setText(0, QString::fromStdString(e.name));
        item->setIcon(0, makeTypeIcon(e.isMesh));
        item->setToolTip(0, QString::fromStdString(e.fullPath));
        item->setData(0, Qt::UserRole, QString::fromStdString(e.fullPath));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);

        // Checked if this entry's full path or bare name is already listed.
        const bool on = selected.count(e.fullPath) != 0 || selected.count(e.name) != 0;
        item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);

        items[i] = item;
    }

    _tree->expandAll();
    _suppressItemChanged = false;
}

void
MaterialOverridePickerWidget::writeSurfacesToKnob()
{
    if (!_ov || !_tree) return;

    // Collect every checked entry's full path.
    std::ostringstream ss;
    bool first = true;
    QTreeWidgetItemIterator it(_tree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        if (item->checkState(0) == Qt::Checked) {
            QString path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty()) {
                if (!first) ss << '\n';
                ss << path.toStdString();
                first = false;
            }
        }
        ++it;
    }

    KnobIPtr k = _ov->getKnobByName("surfaces");
    KnobString* ks = dynamic_cast<KnobString*>(k.get());
    if (ks) {
        ks->setValue(ss.str()); // triggers re-render with the new surface set
    }
}

NATRON_NAMESPACE_EXIT
