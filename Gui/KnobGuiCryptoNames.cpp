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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "KnobGuiCryptoNames.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTimer>

#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"

#include "Gui/Label.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_ENTER

namespace {
KnobString*
siblingString(const KnobIPtr& knob, const char* name)
{
    if (!knob) {
        return NULL;
    }
    KnobHolder* holder = knob->getHolder();
    if (!holder) {
        return NULL;
    }
    KnobIPtr k = holder->getKnobByName(name);
    return dynamic_cast<KnobString*>( k.get() );
}
} // namespace

KnobGuiCryptoNames::KnobGuiCryptoNames(KnobIPtr knob, KnobGuiContainerI *container)
    : KnobGui(knob, container)
    , _knob( std::dynamic_pointer_cast<KnobCryptoNames>(knob) )
    , _container(NULL)
    , _filter(NULL)
    , _count(NULL)
    , _list(NULL)
    , _pickTimer(NULL)
    , _names()
    , _included()
    , _updating(false)
    , _pickSeq(0)
{
}

KnobGuiCryptoNames::~KnobGuiCryptoNames()
{
}

KnobIPtr
KnobGuiCryptoNames::getKnob() const
{
    return _knob.lock();
}

void
KnobGuiCryptoNames::createWidget(QHBoxLayout* layout)
{
    _container = new QWidget( layout->parentWidget() );
    QVBoxLayout* vbox = new QVBoxLayout(_container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(2);

    QWidget* top = new QWidget(_container);
    QHBoxLayout* hbox = new QHBoxLayout(top);
    hbox->setContentsMargins(0, 0, 0, 0);
    _filter = new LineEdit(top);
    _filter->setPlaceholderText( tr("Filter names...") );
    _filter->setToolTip( tr("Show only the names containing this text (case-insensitive). Rows the filter "
                            "hides keep their state.") );
    _filter->setClearButtonEnabled(true);
    hbox->addWidget(_filter, 1);
    _count = new Label(top);
    hbox->addWidget(_count);
    vbox->addWidget(top);

    _list = new QListWidget(_container);
    _list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _list->setUniformItemSizes(true);
    _list->setTextElideMode(Qt::ElideNone);
    _list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    _list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    _list->setMinimumHeight(180);
    _list->setToolTip( tr("Click a name to matte just that object, shift-click for a range, ctrl-click to add "
                          "or remove one. The Matte List follows the selection; highlighted rows are the "
                          "objects the matte includes right now.") );
    vbox->addWidget(_list, 1);

    _pickTimer = new QTimer(_container);
    _pickTimer->setSingleShot(true);
    _pickTimer->setInterval(50);

    QObject::connect( _filter, SIGNAL(textChanged(QString)), this, SLOT(onFilterChanged(QString)) );
    QObject::connect( _list, SIGNAL(itemSelectionChanged()), this, SLOT(onListSelectionChanged()) );
    QObject::connect( _pickTimer, SIGNAL(timeout()), this, SLOT(sendPick()) );

    KnobCryptoNamesPtr knob = _knob.lock();
    if (knob) {
        if (KnobString* sel = siblingString(knob, "manifestSelected")) {
            QObject::connect( sel->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec,int,int)),
                              this, SLOT(onSelectedKnobChanged()) );
        }
    }

    layout->addWidget(_container);
    readIncluded();
    updateGUI(0);
}

void
KnobGuiCryptoNames::readIncluded()
{
    _included.clear();
    KnobString* sel = siblingString(_knob.lock(), "manifestSelected");
    if (!sel) {
        return;
    }
    const QStringList lines = QString::fromStdString( sel->getValue() ).split( QLatin1Char('\n'), Qt::SkipEmptyParts );
    for (int i = 0; i < lines.size(); ++i) {
        _included.insert(lines[i]);
    }
}

void
KnobGuiCryptoNames::rebuildRows()
{
    if (!_list) {
        return;
    }
    const QString f = _filter ? _filter->text().trimmed() : QString();
    _updating = true;
    _list->clear();
    for (int i = 0; i < _names.size(); ++i) {
        if ( !f.isEmpty() && !_names[i].contains(f, Qt::CaseInsensitive) ) {
            continue;
        }
        QListWidgetItem* item = new QListWidgetItem(_names[i], _list);
        item->setToolTip(_names[i]);
    }
    _updating = false;
    applyHighlight();
}

void
KnobGuiCryptoNames::applyHighlight()
{
    if (!_list) {
        return;
    }
    _updating = true;
    for (int i = 0; i < _list->count(); ++i) {
        QListWidgetItem* item = _list->item(i);
        item->setSelected( _included.contains( item->text() ) );
    }
    _updating = false;
    if (_count) {
        QString s = tr("%1 in matte").arg( _included.size() );
        if ( _list->count() != _names.size() ) {
            s = tr("%1 of %2 shown, ").arg( _list->count() ).arg( _names.size() ) + s;
        }
        _count->setText(s);
    }
}

void
KnobGuiCryptoNames::onFilterChanged(const QString& /*text*/)
{
    rebuildRows();
}

void
KnobGuiCryptoNames::onSelectedKnobChanged()
{
    readIncluded();
    applyHighlight();
}

void
KnobGuiCryptoNames::onListSelectionChanged()
{
    if (_updating || !_pickTimer) {
        return;
    }
    _pickTimer->start();
}

void
KnobGuiCryptoNames::sendPick()
{
    if (!_list) {
        return;
    }
    KnobString* pick = siblingString(_knob.lock(), "manifestPick");
    if (!pick) {
        return;
    }
    // The matte the user wants: the rows selected here, plus whatever the filter hides that is
    // in the matte already (a plain click must not drop objects the user cannot see).
    QSet<QString> shown;
    QStringList wanted;
    for (int i = 0; i < _list->count(); ++i) {
        QListWidgetItem* item = _list->item(i);
        shown.insert( item->text() );
        if ( item->isSelected() ) {
            wanted << item->text();
        }
    }
    for (QSet<QString>::const_iterator it = _included.begin(); it != _included.end(); ++it) {
        if ( !shown.contains(*it) ) {
            wanted << *it;
        }
    }
    // A sequence number first, so the same selection picked twice is still a new value.
    QString v = QString::number(++_pickSeq);
    for (int i = 0; i < wanted.size(); ++i) {
        v += QLatin1Char('\n') + wanted[i];
    }
    pick->setValue( v.toStdString() );
}

void
KnobGuiCryptoNames::updateGUI(int /*dimension*/)
{
    KnobCryptoNamesPtr knob = _knob.lock();
    if (!knob || !_list) {
        return;
    }
    _names = QString::fromStdString( knob->getValue() ).split( QLatin1Char('\n'), Qt::SkipEmptyParts );
    readIncluded();
    rebuildRows();
}

void KnobGuiCryptoNames::_hide() { if (_container) _container->hide(); }
void KnobGuiCryptoNames::_show() { if (_container) _container->show(); }

void
KnobGuiCryptoNames::setEnabled()
{
    KnobCryptoNamesPtr knob = _knob.lock();
    if (knob && _container) {
        _container->setEnabled( knob->isEnabled(0) );
    }
}

void
KnobGuiCryptoNames::removeSpecificGui()
{
    _container = NULL;
    _filter = NULL;
    _count = NULL;
    _list = NULL;
    _pickTimer = NULL;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_KnobGuiCryptoNames.cpp"
