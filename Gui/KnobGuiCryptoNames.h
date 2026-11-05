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

#ifndef NATRON_GUI_KNOBGUICRYPTONAMES_H
#define NATRON_GUI_KNOBGUICRYPTONAMES_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <QSet>
#include <QStringList>

#include "Gui/KnobGui.h"
#include "Engine/Dev/Keyer/KnobCryptoNames.h"

class QListWidget;
class QTimer;
class QWidget;

NATRON_NAMESPACE_ENTER

class Label;
class LineEdit;

/**
 * @brief The Cryptomatte node's manifest as a filterable multi-selection list. Rows the matte
 * currently includes are shown selected (fed by the node's secret "manifestSelected" knob, which
 * follows the Matte List, the Expression and the viewer pickers); the user's own selection
 * changes (click, shift-click range, ctrl-click toggle) go back to the node through its secret
 * "manifestPick" knob, which rewrites the Matte List. A filter box narrows long manifests; rows
 * it hides keep their state.
 **/
class KnobGuiCryptoNames
    : public KnobGui
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob, KnobGuiContainerI *container)
    {
        return new KnobGuiCryptoNames(knob, container);
    }

    KnobGuiCryptoNames(KnobIPtr knob, KnobGuiContainerI *container);
    virtual ~KnobGuiCryptoNames();

    virtual KnobIPtr getKnob() const OVERRIDE FINAL;
    virtual bool getAllDimensionsVisible() const OVERRIDE FINAL { return true; }

public Q_SLOTS:

    void onFilterChanged(const QString& text);
    void onListSelectionChanged();
    void onSelectedKnobChanged();
    // Sends the list's selection to the node (coalesced: a drag over several rows picks once).
    void sendPick();

private:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL;
    virtual void setDirty(bool /*dirty*/) OVERRIDE FINAL {}
    virtual void setReadOnly(bool /*readOnly*/, int /*dimension*/) OVERRIDE FINAL {}
    virtual void updateGUI(int /*dimension*/) OVERRIDE FINAL;
    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual bool shouldAddStretch() const OVERRIDE FINAL { return false; }

    // Refills the rows from the names and the filter, then re-applies the highlight.
    void rebuildRows();
    // Selects the rows the matte includes (no pick is sent while doing so).
    void applyHighlight();
    void readIncluded();

    KnobCryptoNamesWPtr _knob;
    QWidget*     _container;
    LineEdit*    _filter;
    Label*       _count;
    QListWidget* _list;
    QTimer*      _pickTimer;  // coalesces selection changes into one pick
    QStringList  _names;      // every manifest name, manifest order
    QSet<QString> _included;  // the names the matte includes right now
    bool         _updating;   // selection is being set programmatically
    int          _pickSeq;    // makes every pick a distinct knob value
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUICRYPTONAMES_H
