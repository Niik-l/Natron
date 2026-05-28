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

#ifndef NATRON_GUI_KNOBGUIGRADIENT_H
#define NATRON_GUI_KNOBGUIGRADIENT_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Gui/KnobGui.h"
#include "Engine/Dev/Particles/KnobGradient.h"

NATRON_NAMESPACE_ENTER

class GradientWidget;

/**
 * @brief Gui wrapper for the KnobGradient custom knob.
 *
 * Glue between the Engine's KnobGradient (which stores a serialized
 * "p:rrggbbaa;..." string) and the user-facing GradientWidget (which
 * paints a gradient bar and edits color stops interactively).
 *
 * Phase 2 (scaffolding): displays a non-interactive gradient bar built
 * from the default stops. Phase 3 adds click-to-add, drag-to-move,
 * double-click-to-edit interactions.
 */
class KnobGuiGradient
    : public KnobGui
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob, KnobGuiContainerI* container)
    {
        return new KnobGuiGradient(knob, container);
    }

    KnobGuiGradient(KnobIPtr knob, KnobGuiContainerI* container);
    virtual ~KnobGuiGradient();

    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

public Q_SLOTS:

    // Phase 3: fires when the user edits the gradient (drag stop, add stop, etc).
    // Phase 2 stub: connected but never emitted.
    void onGradientChanged();

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

    KnobGradientWPtr _knob;
    GradientWidget*  _widget;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUIGRADIENT_H
