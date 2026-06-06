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

#ifndef NATRON_GUI_MATERIALOVERRIDEPICKERWIDGET_H
#define NATRON_GUI_MATERIALOVERRIDEPICKERWIDGET_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

class QTreeWidget;
class QTreeWidgetItem;

NATRON_NAMESPACE_ENTER

class GeoMaterialOverride;

/**
 * @brief Surface picker for the GeoMaterialOverride ("Material Override") node.
 *
 * Lives in the node's settings panel (added by NodeSettingsPanel::initializeExtraGui
 * when the node is a GeoMaterialOverride). Reads the geometry wired into the node's
 * Geo input (walking to the upstream ReadAlembicArchive), shows its hierarchy with a
 * checkbox per entry, and writes the checked entries' full archive paths into the
 * node's `surfaces` knob — so the user picks surfaces visually instead of typing
 * paths.
 *
 * No Q_OBJECT: connections use lambdas, so the class needs no moc.
 */
class MaterialOverridePickerWidget
    : public QWidget
{
public:
    MaterialOverridePickerWidget(GeoMaterialOverride* ov, QWidget* parent = nullptr);
    virtual ~MaterialOverridePickerWidget();

private:
    // Rebuild the tree from the upstream archive (resolved through the Geo input),
    // restoring check state from the node's `surfaces` knob.
    void rebuildFromGeo();
    // Collect every checked entry's full path and write them (one per line) to the
    // node's `surfaces` knob.
    void writeSurfacesToKnob();

    GeoMaterialOverride* _ov;          // not owned
    QTreeWidget* _tree;
    bool _suppressItemChanged;         // prevent recursion while we build the tree
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_MATERIALOVERRIDEPICKERWIDGET_H
