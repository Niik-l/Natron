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

#include "KnobGuiGradient.h"

#include <QHBoxLayout>

#include "Gui/GradientWidget.h"

NATRON_NAMESPACE_ENTER

KnobGuiGradient::KnobGuiGradient(KnobIPtr knob, KnobGuiContainerI* container)
    : KnobGui(knob, container)
    , _knob( std::dynamic_pointer_cast<KnobGradient>(knob) )
    , _widget(NULL)
{
}

KnobGuiGradient::~KnobGuiGradient()
{
}

KnobIPtr
KnobGuiGradient::getKnob() const
{
    return _knob.lock();
}

void
KnobGuiGradient::createWidget(QHBoxLayout* layout)
{
    _widget = new GradientWidget( layout->parentWidget() );
    _widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Phase 3: the widget will emit stopsChanged when the user mutates
    // the gradient. For Phase 2 the connection is wired but never fires.
    QObject::connect(_widget, SIGNAL(stopsChanged()), this, SLOT(onGradientChanged()));

    // Initial population from the knob's stored string.
    KnobGradientPtr k = _knob.lock();
    if (k) {
        _widget->setStops( KnobGradient::parse(k->getValue()) );
    } else {
        _widget->setStops( KnobGradient::defaultStops() );
    }

    layout->addWidget(_widget);
}

void
KnobGuiGradient::updateGUI(int /*dimension*/)
{
    KnobGradientPtr k = _knob.lock();
    if (!k || !_widget) return;
    _widget->setStops( KnobGradient::parse(k->getValue()) );
}

void
KnobGuiGradient::onGradientChanged()
{
    KnobGradientPtr k = _knob.lock();
    if (!k || !_widget) return;
    // Write the widget's current stops back into the knob's string storage.
    k->setValue(KnobGradient::serialize(_widget->stops()));
}

void
KnobGuiGradient::_hide() { if (_widget) _widget->hide(); }
void
KnobGuiGradient::_show() { if (_widget) _widget->show(); }
void
KnobGuiGradient::setEnabled() { /* Phase 3 wires read-only state */ }

void
KnobGuiGradient::removeSpecificGui()
{
    if (_widget) {
        _widget->deleteLater();
        _widget = NULL;
    }
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_KnobGuiGradient.cpp"
