/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef NATRON_GUI_KNOBGUIPASSTABLE_H
#define NATRON_GUI_KNOBGUIPASSTABLE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Gui/KnobGui.h"
#include "Engine/Dev/Cycles/KnobPassTable.h"

NATRON_NAMESPACE_ENTER

class PassTableWidget;

class KnobGuiPassTable
    : public KnobGui
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob, KnobGuiContainerI *container)
    {
        return new KnobGuiPassTable(knob, container);
    }

    KnobGuiPassTable(KnobIPtr knob, KnobGuiContainerI *container);
    virtual ~KnobGuiPassTable();

    virtual KnobIPtr getKnob() const OVERRIDE FINAL;
    virtual bool getAllDimensionsVisible() const OVERRIDE FINAL { return true; }

public Q_SLOTS:

    void onPassesChanged();
    void onRefreshObjects();
    void onPreviewRequested(int passIndex);

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

    KnobPassTableWPtr _knob;
    PassTableWidget*  _widget;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUIPASSTABLE_H
