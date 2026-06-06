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

#ifndef NATRON_GUI_ALEMBICTREEWIDGET_H
#define NATRON_GUI_ALEMBICTREEWIDGET_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

NATRON_NAMESPACE_ENTER

class ReadAlembicArchive;

/**
 * @brief Tree-view widget for ReadAlembicArchive.
 *
 * Lives in the node's settings panel (added by NodeSettingsPanel::initializeExtraGui
 * when the node is a ReadAlembicArchive). Displays the archive's full hierarchy
 * with type/vertex/sample columns and checkboxes for selective import.
 *
 * State persistence: each checkbox toggle rewrites the archive's hidden
 * `excludedPaths` KnobString (one path per line of unchecked items). The archive's
 * knobChanged handler then triggers a filter-only reload that respects the new
 * exclusion set. Saved with the project; restored on load.
 */
class AlembicTreeWidget
    : public QWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    AlembicTreeWidget(ReadAlembicArchive* archive, QWidget* parent = nullptr);
    virtual ~AlembicTreeWidget();

public Q_SLOTS:
    /** @brief Rebuild the tree from the archive's current parsed hierarchy.
     *  Call after the file path changes or the user clicks Reload.
     */
    void refresh();

private Q_SLOTS:
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onRefreshClicked();
    void onCheckAllClicked();
    void onUncheckAllClicked();
    void onSearchTextChanged(const QString& text);
    void onTreeContextMenuRequested(const QPoint& pos);
    void onExpandAllTriggered();
    void onCollapseAllTriggered();
    void onCheckBranchTriggered();
    void onUncheckBranchTriggered();

private:
    void writeExcludedPathsToKnob();
    void rebuildFromArchive();
    void setBranchChecked(QTreeWidgetItem* root, bool checked);
    void applySearchFilter(const QString& text);

    ReadAlembicArchive* _archive;     // not owned
    QLineEdit* _search;
    QTreeWidget* _tree;
    bool _suppressItemChanged;        // prevent recursion while we build the tree
    bool _applyingExcluded;           // writing excludedPaths → skip the self-induced
                                      // archiveReloaded rebuild (would delete the item
                                      // mid-itemChanged signal → use-after-free crash)
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_ALEMBICTREEWIDGET_H
