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

#ifndef NATRON_GUI_PASSTABLEWIDGET_H
#define NATRON_GUI_PASSTABLEWIDGET_H

#include <vector>
#include <utility>

#include <QWidget>
#include <QJsonArray>
#include <QString>
#include <QStringList>

#include "Global/Macros.h"

class QTableWidget;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QVBoxLayout;

NATRON_NAMESPACE_ENTER

/**
 * @brief Spreadsheet-style editor for the CyclesRenderPassManager pass list.
 *
 * The full pass-list JSON is the backing store (held by a KnobPassTable). This
 * widget is a pure view: common fields in a table, the rest in a per-row detail
 * panel. Object/light scoping fields get a dropdown of discovered scene names
 * (fed via setAvailableObjects/Lights) so users pick instead of typing. On any
 * edit it re-serializes the WHOLE array — nothing is lost — and emits
 * passesChanged(). refreshRequested() asks the owner to re-discover scene names.
 */
class PassTableWidget
    : public QWidget
{
    Q_OBJECT

public:

    explicit PassTableWidget(QWidget* parent = NULL);
    virtual ~PassTableWidget();

    /** Load the full pass-list JSON into the table. Does NOT emit passesChanged. */
    void setJson(const QString& json);

    /** Serialize the current table (all fields preserved) back to a JSON string. */
    QString toJson() const;

    /** Feed the dropdown pickers with discovered scene names. */
    void setAvailableObjects(const QStringList& objects);
    void setAvailableLights(const QStringList& lights);

Q_SIGNALS:

    void passesChanged();
    void refreshRequested();
    void previewRequested(int passIndex);

private Q_SLOTS:

    void onCellChanged(int row, int col);
    void onAddRow();
    void onRemoveRow();
    void onSelectionChanged();
    void onDetailChanged();
    void onPickerActivated(int index);
    void onPreviewClicked();

private:

    void rebuildTable();
    void buildDetailPanel(QVBoxLayout* outer);
    void addTextField(QFormLayout* form, const QString& label, const QString& key);
    void addPickerField(QFormLayout* form, const QString& label, const QString& key, bool isLight);
    void addBoolField(QWidget* parent, const QString& label, const QString& key);
    void buildAovChecks(QFormLayout* form);
    void repopulatePickers();
    void loadDetail(int row);
    void commitDetail();

    // A scoping field's dropdown: picking an item appends it to the paired edit.
    struct PickerCombo
    {
        QComboBox* combo;
        QLineEdit* edit;
        bool       isLight;
    };

    QJsonArray    _passes;     // full data model — every field preserved
    QTableWidget* _table;
    bool          _updating;   // guards programmatic refresh from emitting signals
    int           _currentRow; // pass currently shown in the detail panel (-1 = none)

    QGroupBox*    _detailBox;
    std::vector< std::pair<QLineEdit*, QString> > _textFields;  // (widget, json key)
    std::vector< std::pair<QCheckBox*, QString> > _boolFields;  // (widget, json key)
    std::vector< std::pair<QCheckBox*, QString> > _aovChecks;   // (checkbox, AOV name)
    std::vector< PickerCombo >                    _pickers;     // object/light dropdowns

    QStringList _availObjects;
    QStringList _availLights;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_PASSTABLEWIDGET_H
