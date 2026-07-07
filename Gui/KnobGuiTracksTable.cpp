/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include "KnobGuiTracksTable.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/CameraTrackerNode.h"
#include "Engine/KnobTypes.h"

#include "Gui/CameraTrackerPanel.h"

NATRON_NAMESPACE_ENTER

KnobGuiTracksTable::KnobGuiTracksTable(KnobIPtr knob,
                                       KnobGuiContainerI *container)
    : KnobGui(knob, container)
    , _knob(knob)
    , _widget(0)
{
}

KnobGuiTracksTable::~KnobGuiTracksTable()
{
}

KnobIPtr
KnobGuiTracksTable::getKnob() const
{
    return _knob.lock();
}

void
KnobGuiTracksTable::createWidget(QHBoxLayout* layout)
{
    CameraTrackerNode* node = 0;
    KnobIPtr knob = _knob.lock();
    if (knob) {
        node = dynamic_cast<CameraTrackerNode*>( knob->getHolder() );
    }
    _widget = new CameraTrackerPanel( node, layout->parentWidget() );
    layout->addWidget(_widget);
}

void
KnobGuiTracksTable::_hide()
{
    if (_widget) _widget->hide();
}

void
KnobGuiTracksTable::_show()
{
    if (_widget) _widget->show();
}

void
KnobGuiTracksTable::setEnabled()
{
    KnobIPtr knob = _knob.lock();
    if (_widget && knob) _widget->setEnabled( knob->isEnabled(0) );
}

void
KnobGuiTracksTable::removeSpecificGui()
{
    if (_widget) {
        _widget->deleteLater();
        _widget = 0;
    }
}

NATRON_NAMESPACE_EXIT

#include "moc_KnobGuiTracksTable.cpp"
