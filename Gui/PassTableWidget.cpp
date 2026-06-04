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

#include "PassTableWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QSet>
#include <QAbstractItemView>

// This build defines QT_NO_CAST_FROM_ASCII, so every QString built from a
// literal — including QJsonObject keys — must be wrapped. Use QStringLiteral.
#define K(s) QStringLiteral(s)

NATRON_NAMESPACE_ENTER

// Column layout. Keep in sync with the header labels and the read/write
// switch in onCellChanged().
enum PassColumn
{
    eColName = 0,
    eColType,
    eColEnabled,
    eColSamples,
    eColAovs,
    eColFile,
    eColCount
};

// A new pass seeded with every schema field so scoping/override fields exist
// (and are preserved) even though the table doesn't show them as columns.
static QJsonObject
defaultPass()
{
    QJsonObject o;
    o[K("id")]                   = K("p");
    o[K("name")]                 = K("new_pass");
    o[K("type")]                 = K("bty");
    o[K("group")]                = K("beauty");
    o[K("enabled")]              = true;
    o[K("solo")]                 = false;
    o[K("mute")]                 = false;
    o[K("output")]               = true;
    o[K("aovs")]                 = QJsonArray{ K("Combined") };
    o[K("filePath")]             = K("$RENDER/$SHOT/$PASS/$SHOT_$PASS.####.exr");
    o[K("format")]               = K("EXR (Multilayer)");
    o[K("bitDepth")]             = K("16-bit Half");
    o[K("compression")]          = K("ZIP");
    o[K("samples")]              = 128;
    o[K("candidateLights")]      = K("*");
    o[K("excludeLights")]        = K("");
    o[K("soloLight")]            = K("");
    o[K("candidateObjects")]     = K("*");
    o[K("excludeObjects")]       = K("");
    o[K("soloObject")]           = K("");
    o[K("cameraOverride")]       = K("");
    o[K("materialOverride")]     = K("");
    o[K("shadowCatcherObjects")] = K("");
    o[K("holdoutObjects")]       = K("");
    o[K("traceObjects")]         = K("");
    return o;
}

static QString
aovsToText(const QJsonValue& v)
{
    QStringList parts;
    if (v.isArray()) {
        const QJsonArray arr = v.toArray();
        for (const QJsonValue& e : arr) {
            parts << e.toString();
        }
    }
    return parts.join(K(", "));
}

static QJsonArray
textToAovs(const QString& text)
{
    QJsonArray arr;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty()) arr.append(t);
    }
    return arr;
}

PassTableWidget::PassTableWidget(QWidget* parent)
    : QWidget(parent)
    , _table(NULL)
    , _updating(false)
    , _currentRow(-1)
    , _detailBox(NULL)
{
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    _table = new QTableWidget(this);
    _table->setColumnCount(eColCount);
    QStringList headers;
    headers << tr("Name") << tr("Type") << tr("On") << tr("Samples")
            << tr("AOVs") << tr("File Path");
    _table->setHorizontalHeaderLabels(headers);
    _table->verticalHeader()->setVisible(false);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::DoubleClicked |
                            QAbstractItemView::SelectedClicked |
                            QAbstractItemView::EditKeyPressed);

    // Readability: zebra striping, taller rows, padded cells, bold header,
    // visible grid, brighter text.
    _table->setAlternatingRowColors(true);
    _table->setShowGrid(true);
    _table->verticalHeader()->setDefaultSectionSize(26);
    _table->setStyleSheet(QStringLiteral(
        "QTableWidget { alternate-background-color: rgb(58,58,58);"
        "               gridline-color: rgb(80,80,80);"
        "               color: rgb(225,225,225); }"
        "QTableWidget::item { padding: 2px 6px; color: rgb(225,225,225); }"
        "QTableWidget::item:selected { color: rgb(255,255,255); }"
        "QHeaderView::section { padding: 4px 6px; font-weight: bold;"
        "                       color: rgb(235,235,235); }"));

    QHeaderView* hh = _table->horizontalHeader();
    hh->setSectionResizeMode(eColName,    QHeaderView::Interactive);
    hh->setSectionResizeMode(eColType,    QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(eColEnabled, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(eColSamples, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(eColAovs,    QHeaderView::Interactive);
    hh->setSectionResizeMode(eColFile,    QHeaderView::Stretch);

    outer->addWidget(_table);

    QHBoxLayout* btns = new QHBoxLayout();
    btns->setContentsMargins(0, 0, 0, 0);
    QPushButton* addBtn = new QPushButton(tr("+ Add Pass"), this);
    QPushButton* remBtn = new QPushButton(tr("- Remove"), this);
    QPushButton* prvBtn = new QPushButton(tr("Preview Selected"), this);
    prvBtn->setToolTip(tr("Render the selected pass to a connected Viewer so you can check it before the disk render."));
    QPushButton* refBtn = new QPushButton(tr("Refresh Objects"), this);
    refBtn->setToolTip(tr("Re-scan the connected scene for objects and lights."));
    btns->addWidget(addBtn);
    btns->addWidget(remBtn);
    btns->addWidget(prvBtn);
    btns->addStretch();
    btns->addWidget(refBtn);
    outer->addLayout(btns);

    buildDetailPanel(outer);

    QObject::connect(_table, SIGNAL(cellChanged(int,int)), this, SLOT(onCellChanged(int,int)));
    QObject::connect(_table, SIGNAL(itemSelectionChanged()), this, SLOT(onSelectionChanged()));
    QObject::connect(addBtn, SIGNAL(clicked()), this, SLOT(onAddRow()));
    QObject::connect(remBtn, SIGNAL(clicked()), this, SLOT(onRemoveRow()));
    QObject::connect(prvBtn, SIGNAL(clicked()), this, SLOT(onPreviewClicked()));
    QObject::connect(refBtn, SIGNAL(clicked()), this, SIGNAL(refreshRequested()));
}

PassTableWidget::~PassTableWidget()
{
}

void
PassTableWidget::addTextField(QFormLayout* form, const QString& label, const QString& key)
{
    QLineEdit* e = new QLineEdit(_detailBox);
    form->addRow(label, e);
    _textFields.push_back( std::make_pair(e, key) );
    QObject::connect(e, SIGNAL(editingFinished()), this, SLOT(onDetailChanged()));
}

void
PassTableWidget::addPickerField(QFormLayout* form, const QString& label,
                                const QString& key, bool isLight)
{
    QWidget* row = new QWidget(_detailBox);
    QHBoxLayout* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);

    QLineEdit* e = new QLineEdit(row);
    QComboBox* c = new QComboBox(row);
    c->setMinimumContentsLength(8);
    h->addWidget(e, 1);
    h->addWidget(c, 0);
    form->addRow(label, row);

    _textFields.push_back( std::make_pair(e, key) );
    QObject::connect(e, SIGNAL(editingFinished()), this, SLOT(onDetailChanged()));

    PickerCombo pc;
    pc.combo = c;
    pc.edit  = e;
    pc.isLight = isLight;
    _pickers.push_back(pc);
    QObject::connect(c, SIGNAL(activated(int)), this, SLOT(onPickerActivated(int)));
}

void
PassTableWidget::addBoolField(QWidget* parent, const QString& label, const QString& key)
{
    QCheckBox* c = new QCheckBox(label, parent);
    if (parent->layout()) parent->layout()->addWidget(c);
    _boolFields.push_back( std::make_pair(c, key) );
    QObject::connect(c, SIGNAL(toggled(bool)), this, SLOT(onDetailChanged()));
}

void
PassTableWidget::buildAovChecks(QFormLayout* form)
{
    // (label, AOV pass name). Names match CyclesRender's standard-pass map +
    // the shadow-catcher trio. Checking a box adds its name to the pass "aovs"
    // array; custom/token AOVs (e.g. @all_light_groups) are preserved.
    static const struct { const char* label; const char* name; } kAovs[] = {
        { "Combined",              "Combined" },
        { "Shadow Catcher Matte",  "ShadowCatcherMatte" },
        { "Shadow Catcher (raw)",  "ShadowCatcher" },
        { "SC Sample Count",       "ShadowCatcherSampleCount" },
        { "Depth",                 "Depth" },
        { "Normal",                "Normal" },
        { "UV",                    "UV" },
        { "Mist",                  "Mist" },
        { "Ambient Occlusion",     "AO" },
        { "Diffuse Direct",        "DiffDir" },
        { "Diffuse Indirect",      "DiffInd" },
        { "Diffuse Color",         "DiffCol" },
        { "Glossy Direct",         "GlossDir" },
        { "Glossy Indirect",       "GlossInd" },
        { "Glossy Color",          "GlossCol" },
        { "Transmission Direct",   "TransDir" },
        { "Transmission Indirect", "TransInd" },
        { "Transmission Color",    "TransCol" },
        { "Emission",              "Emit" },
        { "Environment",           "Env" },
    };

    QGroupBox* box = new QGroupBox(tr("AOVs"), _detailBox);
    QGridLayout* grid = new QGridLayout(box);
    grid->setContentsMargins(6, 4, 6, 4);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(2);

    const int cols = 2;
    const int n = (int)(sizeof(kAovs) / sizeof(kAovs[0]));
    for (int i = 0; i < n; ++i) {
        QCheckBox* c = new QCheckBox(tr(kAovs[i].label), box);
        grid->addWidget(c, i / cols, i % cols);
        _aovChecks.push_back( std::make_pair(c, QString::fromLatin1(kAovs[i].name)) );
        QObject::connect(c, SIGNAL(toggled(bool)), this, SLOT(onDetailChanged()));
    }

    form->addRow(box);
}

void
PassTableWidget::buildDetailPanel(QVBoxLayout* outer)
{
    _detailBox = new QGroupBox(tr("Selected Pass"), this);
    QFormLayout* form = new QFormLayout(_detailBox);
    form->setLabelAlignment(Qt::AlignRight);

    addTextField(form, tr("Group"), K("group"));

    // Output flags on one row.
    QWidget* flags = new QWidget(_detailBox);
    QHBoxLayout* fl = new QHBoxLayout(flags);
    fl->setContentsMargins(0, 0, 0, 0);
    addBoolField(flags, tr("Solo"),   K("solo"));
    addBoolField(flags, tr("Mute"),   K("mute"));
    addBoolField(flags, tr("Output"), K("output"));
    fl->addStretch();
    form->addRow(tr("Flags"), flags);

    addTextField(form, tr("Format"),      K("format"));
    addTextField(form, tr("Bit Depth"),   K("bitDepth"));
    addTextField(form, tr("Compression"), K("compression"));

    buildAovChecks(form);

    addPickerField(form, tr("Candidate Objects"), K("candidateObjects"), false);
    addPickerField(form, tr("Exclude Objects"),   K("excludeObjects"),   false);
    addPickerField(form, tr("Solo Object"),       K("soloObject"),       false);

    addPickerField(form, tr("Shadow Catchers"),  K("shadowCatcherObjects"), false);
    addPickerField(form, tr("Holdouts"),         K("holdoutObjects"),       false);
    addPickerField(form, tr("Trace (phantom)"),  K("traceObjects"),         false);

    addPickerField(form, tr("Candidate Lights"), K("candidateLights"), true);
    addPickerField(form, tr("Exclude Lights"),   K("excludeLights"),   true);
    addPickerField(form, tr("Solo Light"),       K("soloLight"),       true);

    addTextField(form, tr("Camera Override"),   K("cameraOverride"));
    addTextField(form, tr("Material Override"), K("materialOverride"));

    _detailBox->setEnabled(false);
    outer->addWidget(_detailBox);
}

void
PassTableWidget::setAvailableObjects(const QStringList& objects)
{
    _availObjects = objects;
    repopulatePickers();
}

void
PassTableWidget::setAvailableLights(const QStringList& lights)
{
    _availLights = lights;
    repopulatePickers();
}

void
PassTableWidget::repopulatePickers()
{
    for (size_t i = 0; i < _pickers.size(); ++i) {
        QComboBox* c = _pickers[i].combo;
        const QStringList& src = _pickers[i].isLight ? _availLights : _availObjects;
        c->blockSignals(true);
        c->clear();
        c->addItem( tr("+ add...") );      // index 0 = placeholder
        for (int j = 0; j < src.size(); ++j) {
            c->addItem(src.at(j));
        }
        c->setCurrentIndex(0);
        c->blockSignals(false);
    }
}

void
PassTableWidget::setJson(const QString& json)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error == QJsonParseError::NoError && doc.isArray()) {
        _passes = doc.array();
    } else {
        // Malformed / empty — start clean rather than silently mangle. An empty
        // table still serializes to "[]".
        _passes = QJsonArray();
    }
    rebuildTable();

    // Restore the detail selection across a refresh (load / undo / external).
    int sel = (_currentRow >= 0 && _currentRow < _passes.size()) ? _currentRow
            : (_passes.size() > 0 ? 0 : -1);
    if (sel >= 0) {
        _table->selectRow(sel);   // fires onSelectionChanged -> loadDetail
    } else {
        loadDetail(-1);
    }
}

QString
PassTableWidget::toJson() const
{
    return QString::fromUtf8(QJsonDocument(_passes).toJson(QJsonDocument::Indented));
}

void
PassTableWidget::rebuildTable()
{
    _updating = true;

    _table->setRowCount(0);
    _table->setRowCount(_passes.size());

    for (int row = 0; row < _passes.size(); ++row) {
        const QJsonObject o = _passes.at(row).toObject();

        QTableWidgetItem* nameItem = new QTableWidgetItem(o.value(K("name")).toString());
        _table->setItem(row, eColName, nameItem);

        QTableWidgetItem* typeItem = new QTableWidgetItem(o.value(K("type")).toString());
        _table->setItem(row, eColType, typeItem);

        QTableWidgetItem* enItem = new QTableWidgetItem();
        enItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        enItem->setCheckState(o.value(K("enabled")).toBool(true) ? Qt::Checked : Qt::Unchecked);
        enItem->setTextAlignment(Qt::AlignCenter);
        _table->setItem(row, eColEnabled, enItem);

        QTableWidgetItem* smpItem =
            new QTableWidgetItem(QString::number(o.value(K("samples")).toInt(128)));
        _table->setItem(row, eColSamples, smpItem);

        QTableWidgetItem* aovItem = new QTableWidgetItem(aovsToText(o.value(K("aovs"))));
        // Read-only summary — AOVs are edited via the detail-panel checkboxes.
        aovItem->setFlags(aovItem->flags() & ~Qt::ItemIsEditable);
        _table->setItem(row, eColAovs, aovItem);

        QTableWidgetItem* fileItem = new QTableWidgetItem(o.value(K("filePath")).toString());
        _table->setItem(row, eColFile, fileItem);
    }

    _updating = false;
}

void
PassTableWidget::onCellChanged(int row, int col)
{
    if (_updating) return;
    if (row < 0 || row >= _passes.size()) return;
    QTableWidgetItem* item = _table->item(row, col);
    if (!item) return;

    QJsonObject o = _passes.at(row).toObject();
    switch (col) {
    case eColName:    o[K("name")]     = item->text(); break;
    case eColType:    o[K("type")]     = item->text(); break;
    case eColEnabled: o[K("enabled")]  = (item->checkState() == Qt::Checked); break;
    case eColSamples: o[K("samples")]  = item->text().toInt(); break;
    case eColAovs:    o[K("aovs")]     = textToAovs(item->text()); break;
    case eColFile:    o[K("filePath")] = item->text(); break;
    default: return;
    }
    _passes.replace(row, o);

    Q_EMIT passesChanged();
}

void
PassTableWidget::onSelectionChanged()
{
    if (_updating) return;
    loadDetail(_table->currentRow());
}

void
PassTableWidget::onPreviewClicked()
{
    const int row = _table->currentRow();
    if (row < 0 || row >= _passes.size()) return;
    Q_EMIT previewRequested(row);
}

void
PassTableWidget::onDetailChanged()
{
    commitDetail();
}

void
PassTableWidget::onPickerActivated(int index)
{
    if (index <= 0) return;   // placeholder row
    QComboBox* c = qobject_cast<QComboBox*>(sender());
    if (!c) return;

    QLineEdit* edit = NULL;
    for (size_t i = 0; i < _pickers.size(); ++i) {
        if (_pickers[i].combo == c) { edit = _pickers[i].edit; break; }
    }
    if (!edit) return;

    const QString name = c->itemText(index);
    c->setCurrentIndex(0);
    if (name.isEmpty()) return;

    // Parse the current semicolon list, treating "*" / empty as "none yet".
    QStringList cur;
    const QString curText = edit->text().trimmed();
    if (curText != K("*") && !curText.isEmpty()) {
        const QStringList parts = curText.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& p : parts) {
            const QString t = p.trimmed();
            if (!t.isEmpty()) cur << t;
        }
    }
    if (!cur.contains(name)) {
        cur << name;
        edit->setText(cur.join(K("; ")));
        commitDetail();
    }
}

void
PassTableWidget::loadDetail(int row)
{
    const bool valid = (row >= 0 && row < _passes.size());
    _currentRow = valid ? row : -1;
    if (_detailBox) _detailBox->setEnabled(valid);
    if (!valid) return;

    _updating = true;
    const QJsonObject o = _passes.at(row).toObject();
    for (size_t i = 0; i < _textFields.size(); ++i) {
        _textFields[i].first->setText( o.value(_textFields[i].second).toString() );
    }
    for (size_t i = 0; i < _boolFields.size(); ++i) {
        _boolFields[i].first->setChecked( o.value(_boolFields[i].second).toBool() );
    }
    // AOV checkboxes: tick the ones present in the pass's "aovs" array.
    {
        QSet<QString> have;
        const QJsonArray aovs = o.value(K("aovs")).toArray();
        for (const QJsonValue& v : aovs) have.insert(v.toString());
        for (size_t i = 0; i < _aovChecks.size(); ++i) {
            _aovChecks[i].first->setChecked( have.contains(_aovChecks[i].second) );
        }
    }
    _updating = false;
}

void
PassTableWidget::commitDetail()
{
    if (_updating) return;
    if (_currentRow < 0 || _currentRow >= _passes.size()) return;

    QJsonObject o = _passes.at(_currentRow).toObject();
    for (size_t i = 0; i < _textFields.size(); ++i) {
        o[_textFields[i].second] = _textFields[i].first->text();
    }
    for (size_t i = 0; i < _boolFields.size(); ++i) {
        o[_boolFields[i].second] = _boolFields[i].first->isChecked();
    }
    // AOVs: rebuild from the checkboxes, but PRESERVE any custom/token entries
    // (e.g. "@all_light_groups", "Combined_keyLight") that aren't in our list.
    {
        QSet<QString> known;
        for (size_t i = 0; i < _aovChecks.size(); ++i) known.insert(_aovChecks[i].second);
        QJsonArray aovs;
        const QJsonArray prev = o.value(K("aovs")).toArray();
        for (const QJsonValue& v : prev) {
            if (!known.contains(v.toString())) aovs.append(v);
        }
        for (size_t i = 0; i < _aovChecks.size(); ++i) {
            if (_aovChecks[i].first->isChecked()) aovs.append(_aovChecks[i].second);
        }
        o[K("aovs")] = aovs;
    }
    _passes.replace(_currentRow, o);

    Q_EMIT passesChanged();
}

void
PassTableWidget::onAddRow()
{
    _passes.append(defaultPass());
    rebuildTable();
    if (_table->rowCount() > 0) {
        _table->selectRow(_table->rowCount() - 1);  // -> loadDetail
    }
    Q_EMIT passesChanged();
}

void
PassTableWidget::onRemoveRow()
{
    const int row = _table->currentRow();
    if (row < 0 || row >= _passes.size()) return;
    _passes.removeAt(row);
    rebuildTable();
    // Reselect a sensible neighbour so the detail panel stays in sync.
    if (_passes.size() > 0) {
        const int sel = (row < _passes.size()) ? row : (_passes.size() - 1);
        _table->selectRow(sel);
    } else {
        loadDetail(-1);
    }
    Q_EMIT passesChanged();
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#undef K

#include "moc_PassTableWidget.cpp"
