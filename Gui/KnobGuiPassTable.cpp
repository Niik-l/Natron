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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "KnobGuiPassTable.h"

#include <QHBoxLayout>
#include <QStringList>

#include "Engine/KnobTypes.h"

#include "Gui/PassTableWidget.h"
#include "Engine/Dev/Cycles/CyclesRenderPassManager.h"

NATRON_NAMESPACE_ENTER

KnobGuiPassTable::KnobGuiPassTable(KnobIPtr knob, KnobGuiContainerI *container)
    : KnobGui(knob, container)
    , _knob( std::dynamic_pointer_cast<KnobPassTable>(knob) )
    , _widget(NULL)
{
}

KnobGuiPassTable::~KnobGuiPassTable()
{
}

KnobIPtr
KnobGuiPassTable::getKnob() const
{
    return _knob.lock();
}

void
KnobGuiPassTable::createWidget(QHBoxLayout* layout)
{
    _widget = new PassTableWidget( layout->parentWidget() );
    _widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    _widget->setMinimumHeight(160);

    QObject::connect(_widget, SIGNAL(passesChanged()), this, SLOT(onPassesChanged()));
    QObject::connect(_widget, SIGNAL(refreshRequested()), this, SLOT(onRefreshObjects()));
    QObject::connect(_widget, SIGNAL(previewRequested(int)), this, SLOT(onPreviewRequested(int)));

    onRefreshObjects();   // initial population of the dropdown pickers

    KnobPassTablePtr knob = _knob.lock();
    if (knob) {
        _widget->setJson( QString::fromStdString( knob->getValue() ) );
    }

    layout->addWidget(_widget);
}

void
KnobGuiPassTable::onRefreshObjects()
{
    if (!_widget) return;
    KnobPassTablePtr knob = _knob.lock();
    if (!knob) return;
    CyclesRenderPassManager* mgr = dynamic_cast<CyclesRenderPassManager*>( knob->getHolder() );
    if (!mgr) return;

    std::vector<std::string> geo, lights;
    mgr->discoverSceneObjects(geo, lights);

    QStringList qgeo, qlights;
    for (size_t i = 0; i < geo.size(); ++i)    qgeo    << QString::fromStdString(geo[i]);
    for (size_t i = 0; i < lights.size(); ++i) qlights << QString::fromStdString(lights[i]);
    _widget->setAvailableObjects(qgeo);
    _widget->setAvailableLights(qlights);
}

void
KnobGuiPassTable::onPreviewRequested(int passIndex)
{
    KnobPassTablePtr knob = _knob.lock();
    if (!knob) return;
    KnobHolder* holder = knob->getHolder();
    if (!holder) return;
    // Drive the node's secret previewPassIndex knob; setting it re-renders the
    // node, so a connected Viewer shows the selected pass.
    KnobIPtr k = holder->getKnobByName("previewPassIndex");
    KnobInt* ik = dynamic_cast<KnobInt*>( k.get() );
    if (ik) ik->setValue(passIndex);
}

void
KnobGuiPassTable::onPassesChanged()
{
    KnobPassTablePtr knob = _knob.lock();
    if (!knob || !_widget) return;
    // Push the edited table back into the string-backed knob. This persists in
    // the project file and is what the renderer reads — same value path as the
    // old plain-JSON KnobString.
    knob->setValue( _widget->toJson().toStdString() );
}

void
KnobGuiPassTable::updateGUI(int /*dimension*/)
{
    if (!_widget) return;
    KnobPassTablePtr knob = _knob.lock();
    if (!knob) return;
    // Refresh from the knob (external change: load, undo/redo, script). setJson
    // does not emit passesChanged, so this won't loop back into onPassesChanged.
    _widget->setJson( QString::fromStdString( knob->getValue() ) );
}

void KnobGuiPassTable::_hide() { if (_widget) _widget->hide(); }
void KnobGuiPassTable::_show() { if (_widget) _widget->show(); }
void KnobGuiPassTable::setEnabled() {}

void
KnobGuiPassTable::removeSpecificGui()
{
    _widget = NULL;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_KnobGuiPassTable.cpp"
