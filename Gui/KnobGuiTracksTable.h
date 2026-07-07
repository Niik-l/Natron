/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_GUI_KNOBGUITRACKSTABLE_H
#define NATRON_GUI_KNOBGUITRACKSTABLE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Gui/KnobGui.h"

NATRON_NAMESPACE_ENTER

class CameraTrackerPanel;

/**
 * @brief Knob GUI for KnobTracksTable (CameraTracker manual tracks).
 *
 * Hosts the CameraTrackerPanel table widget as a KNOB, so the table nests
 * inside the Manual Tracks group like any parameter: indented under the group
 * header and shown/hidden by the group's expand arrow (the generic knob
 * _show/_hide path — no layout tricks). Same custom-widget-in-a-knob pattern
 * as KnobGuiPassTable.
 **/
class KnobGuiTracksTable
    : public KnobGui
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob, KnobGuiContainerI *container)
    {
        return new KnobGuiTracksTable(knob, container);
    }

    KnobGuiTracksTable(KnobIPtr knob, KnobGuiContainerI *container);
    virtual ~KnobGuiTracksTable();

    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

private:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL;
    virtual void setDirty(bool /*dirty*/) OVERRIDE FINAL {}
    virtual void setReadOnly(bool /*readOnly*/, int /*dimension*/) OVERRIDE FINAL {}
    virtual void updateGUI(int /*dimension*/) OVERRIDE FINAL {}
    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual bool shouldAddStretch() const OVERRIDE FINAL { return false; }

    KnobIWPtr _knob;
    CameraTrackerPanel* _widget;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUITRACKSTABLE_H
