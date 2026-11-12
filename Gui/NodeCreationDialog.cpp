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
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "NodeCreationDialog.h"

#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QVBoxLayout>
#include <QStringList>
#include <QKeyEvent>
#include <QAbstractItemView>
#include <QTimer>
#include <QApplication>
#include <QListView>
#include <QSettings>
#include "Global/QtCompat.h"
#include <QApplication>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QScreen>
#include <QGuiApplication>
#include <QAction>
#include <QPixmap>
#include <QIcon>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Plugin.h"
#include "Engine/Settings.h"
#include "Gui/LineEdit.h"
#include "Gui/GuiApplicationManager.h"

/*
   If defined, the list will contain matches ordered by increasing match length of the regexp
   otherwise they will be ordered with their score
 */
//#define NODE_TAB_DIALOG_USE_MATCHED_LENGTH

NATRON_NAMESPACE_ENTER

// Maximum number of rows the completion list shows before it scrolls.
#define NATRON_COMPLETER_MAX_VISIBLE_ROWS 14

NATRON_NAMESPACE_ANONYMOUS_ENTER

static QColor
settingsColor(void (Settings::*getter)(double*, double*, double*) const)
{
    double r = 0.5, g = 0.5, b = 0.5;
    ( appPTR->getCurrentSettings().get()->*getter )(&r, &g, &b);

    return QColor::fromRgbF(r, g, b);
}

/**
 * @brief Paints one completion row: the name on the left and, when the entry is of the form
 * "Name  [Group]" (a plug-in), the group as a dimmed tag on the right. The model text is left
 * untouched, it is what identifies the entry.
 **/
class CompleterItemDelegate
    : public QStyledItemDelegate
{
public:

    CompleterItemDelegate(QObject* parent)
        : QStyledItemDelegate(parent)
    {
    }

    static int hPad(const QFontMetrics& fm) { return fm.height() / 2; }

    static int vPad(const QFontMetrics& fm) { return std::max(3, fm.height() / 5); }

    virtual QSize sizeHint(const QStyleOptionViewItem& option,
                           const QModelIndex& /*index*/) const OVERRIDE FINAL
    {
        QFontMetrics fm(option.font);

        // The width follows the view (no horizontal scrolling): long names are elided.
        return QSize( 10, fm.height() + 2 * vPad(fm) );
    }

    virtual void paint(QPainter* painter,
                       const QStyleOptionViewItem& option,
                       const QModelIndex& index) const OVERRIDE FINAL
    {
        const QColor textColor = settingsColor(&Settings::getTextColor);
        const QColor selColor = settingsColor(&Settings::getSelectionColor);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QRect r = option.rect;
        QFontMetrics fm(option.font);
        const int pad = hPad(fm);
        const int bar = std::max(2, pad / 3);

        painter->save();
        painter->setFont(option.font);
        if (selected) {
            QColor fill = selColor;
            fill.setAlpha(70);
            painter->fillRect(r, fill);
            painter->fillRect(QRect( r.left(), r.top(), bar, r.height() ), selColor);
        } else if (hovered) {
            QColor fill = textColor;
            fill.setAlpha(22);
            painter->fillRect(r, fill);
        }

        QString name = index.data().toString();
        QString group;
        const int bracket = name.lastIndexOf( QString::fromUtf8("  [") );
        if ( (bracket > 0) && name.endsWith( QLatin1Char(']') ) ) {
            group = name.mid(bracket + 3, name.size() - bracket - 4);
            name = name.left(bracket);
        }

        QRect textRect = r.adjusted(pad + bar, 0, -pad, 0);
        if ( !group.isEmpty() ) {
            const QString g = fm.elidedText(group, Qt::ElideRight, textRect.width() / 2);
            const int gw = fm.horizontalAdvance(g);
            QColor dim = textColor;
            dim.setAlpha(selected ? 190 : 120);
            painter->setPen(dim);
            painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, g);
            textRect.setRight(textRect.right() - gw - pad);
        }
        painter->setPen( selected ? textColor.lighter(125) : textColor );
        painter->drawText( textRect, Qt::AlignLeft | Qt::AlignVCenter,
                           fm.elidedText( name, Qt::ElideRight, textRect.width() ) );
        painter->restore();
    }
};

// Shared look of the completion popup: flat list, no horizontal scrollbar, a slim vertical
// scrollbar without arrow buttons, rows drawn by CompleterItemDelegate.
static void
setupCompleterView(QListView* view)
{
    const QColor base = settingsColor(&Settings::getBaseColor);
    const QColor text = settingsColor(&Settings::getTextColor);
    const QString rgba = QString::fromUtf8("rgba(%1,%2,%3,%4)");
    const QString handle = rgba.arg( text.red() ).arg( text.green() ).arg( text.blue() ).arg(90);
    const QString handleHover = rgba.arg( text.red() ).arg( text.green() ).arg( text.blue() ).arg(150);

    view->setItemDelegate( new CompleterItemDelegate(view) );
    view->setUniformItemSizes(true);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setTextElideMode(Qt::ElideRight);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setMouseTracking(true);
    view->viewport()->setAttribute(Qt::WA_Hover, true);
    view->setFocusPolicy(Qt::NoFocus);

    view->setStyleSheet( QString::fromUtf8(
                             "QListView { background-color: %1; border: 1px solid %2; outline: none; padding: 0px; }\n"
                             "QScrollBar:vertical { background: transparent; border: none; width: 10px; margin: 2px 2px 2px 0px; }\n"
                             "QScrollBar::handle:vertical { background: %3; border: none; border-radius: 4px; min-height: 28px; }\n"
                             "QScrollBar::handle:vertical:hover { background: %4; }\n"
                             "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; width: 0px; border: none; background: none; }\n"
                             "QScrollBar::up-arrow:vertical, QScrollBar::down-arrow:vertical { image: none; width: 0px; height: 0px; }\n"
                             "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }\n")
                         .arg( base.name() )
                         .arg( base.darker(170).name() )
                         .arg(handle)
                         .arg(handleHover) );
}

NATRON_NAMESPACE_ANONYMOUS_EXIT


struct CompleterLineEditPrivate
{
    QDialog* dialog;
    QListView* listView;
    QStringListModel* model;
    CompleterLineEdit::PluginsNamesMap names;
    bool quickExitEnabled;

    CompleterLineEditPrivate(const CompleterLineEdit::PluginsNamesMap& plugins,
                             bool quickExit,
                             QDialog* parent)
        : dialog(parent)
        , listView(NULL)
        , model(NULL)
        , names(plugins)
        , quickExitEnabled(quickExit)
    {
    }

    CompleterLineEditPrivate(const QStringList & displayWords,
                             const QStringList & internalIds,
                             bool quickExit,
                             QDialog* parent)
        : dialog(parent)
        , listView(NULL)
        , model(NULL)
        , quickExitEnabled(quickExit)
    {
        assert( displayWords.size() == internalIds.size() );

        for (int i = 0; i < displayWords.size(); ++i) {
            names.insert( std::make_pair( 0, std::make_pair(internalIds[i], displayWords[i]) ) );
        }
    }
};

CompleterLineEdit::CompleterLineEdit(const PluginsNamesMap& plugins,
                                     bool quickExit,
                                     QDialog* parent)
    : LineEdit(parent)
    , _imp( new CompleterLineEditPrivate(plugins, quickExit, parent) )
{
    _imp->listView = new QListView(parent);
    _imp->model = new QStringListModel(this);
    _imp->listView->setWindowFlags(Qt::ToolTip);
    _imp->listView->setModel(_imp->model);
    setupCompleterView(_imp->listView);

    connect( this, SIGNAL(textChanged(QString)), this, SLOT(filterText(QString)) );
    connect( _imp->listView, SIGNAL(clicked(QModelIndex)), this, SLOT(setTextFromIndex(QModelIndex)) );
}

CompleterLineEdit::CompleterLineEdit(const QStringList & displayWords,
                                     const QStringList & internalIds,
                                     bool quickExit,
                                     QDialog* parent)
    : LineEdit(parent)
    , _imp( new CompleterLineEditPrivate(displayWords, internalIds, quickExit, parent) )
{
    _imp->listView = new QListView(parent);
    _imp->model = new QStringListModel(this);
    _imp->listView->setWindowFlags(Qt::ToolTip);
    _imp->listView->setModel(_imp->model);
    setupCompleterView(_imp->listView);

    connect( this, SIGNAL(textChanged(QString)), this, SLOT(filterText(QString)) );
    connect( _imp->listView, SIGNAL(clicked(QModelIndex)), this, SLOT(setTextFromIndex(QModelIndex)) );
}

CompleterLineEdit::~CompleterLineEdit()
{
}

QListView*
CompleterLineEdit::getView() const
{
    return _imp->listView;
}

void
CompleterLineEdit::filterText(const QString & txt)
{
    QStringList sl;

    if ( txt.isEmpty() ) {
        for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
            sl.push_front(it->second.second);
        }
    } else {
        QString pattern;
        for (int i = 0; i < txt.size(); ++i) {
            pattern.push_back( QLatin1Char('*') );
            pattern.push_back(txt[i]);
        }
        pattern.push_back( QLatin1Char('*') );
#ifdef NODE_TAB_DIALOG_USE_MATCHED_LENGTH
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QRegularExpression expr = QtCompat::wildcardToRegex(pattern, Qt::CaseInsensitive);
        std::map<int, QStringList> matchOrdered;
        for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
            QRegularExpressionMatch m = expr.match(it->second.first);
            if ( m.hasMatch() ) {
                QStringList& matchedForLength =  matchOrdered[m.capturedLength()];
                matchedForLength.push_front(it->second.second);
            }
        }
#else
        QRegExp expr(pattern, Qt::CaseInsensitive, QRegExp::WildcardUnix);
        std::map<int, QStringList> matchOrdered;
        for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
            if ( expr.exactMatch(it->second.first) ) {
                QStringList& matchedForLength =  matchOrdered[expr.matchedLength()];
                matchedForLength.push_front(it->second.second);
            }
        }
#endif
        for (std::map<int, QStringList>::iterator it = matchOrdered.begin(); it != matchOrdered.end(); ++it) {
            sl.append(it->second);
        }
#else
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QRegularExpression expr = QtCompat::wildcardToRegexUnanchored(pattern, Qt::CaseInsensitive);
#else
        QRegExp expr(pattern, Qt::CaseInsensitive, QRegExp::WildcardUnix);
#endif

        for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
            if ( it->second.first.contains(expr) ) {
                sl.push_front(it->second.second);
            }
        }
#endif
    }
    _imp->model->setStringList(sl);
    _imp->listView->setModel(_imp->model);

    int rowCount = _imp->model->rowCount();
    if (rowCount == 0) {
        // Nothing matches: do not leave the previous results on screen.
        _imp->listView->hide();

        return;
    }

    // The first match is what Enter creates: show it as selected.
    _imp->listView->setCurrentIndex( _imp->model->index(0) );

    // Size the list to its rows (up to NATRON_COMPLETER_MAX_VISIBLE_ROWS, less when the screen
    // ends sooner) so short result lists do not leave an empty box.
    QPoint p = mapToGlobal( QPoint( 0, height() ) );
    const int rowHeight = std::max( 1, _imp->listView->sizeHintForRow(0) );
    int visibleRows = std::min(rowCount, NATRON_COMPLETER_MAX_VISIBLE_ROWS);
    QScreen* screen = QGuiApplication::screenAt(p);
    if (screen) {
        const int room = screen->availableGeometry().bottom() - p.y() - 2 * _imp->listView->frameWidth();
        visibleRows = std::max( 1, std::min(visibleRows, room / rowHeight) );
    }
    _imp->listView->setFixedHeight(visibleRows * rowHeight + 2 * _imp->listView->frameWidth() + 2);

    _imp->listView->move(p);
    _imp->listView->show();
    // The window system may give the popup an invisible frame: once shown, line its contents
    // up with the bottom of the field so the two touch.
    const QPoint offset = _imp->listView->geometry().topLeft() - p;
    if ( !offset.isNull() ) {
        _imp->listView->move(_imp->listView->pos() - offset);
    }
} // CompleterLineEdit::filterText

void
CompleterLineEdit::setTextFromIndex(const QModelIndex & index)
{
    QString text = index.data().toString();

    setText(text);
    Q_EMIT itemCompletionChosen();
    _imp->listView->hide();
    if (_imp->quickExitEnabled) {
        NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
        if (isNodeDialog) {
            isNodeDialog->finish(true);
        } else {
            if (_imp->dialog) {
                _imp->dialog->accept();
            }
        }
    }
}

void
CompleterLineEdit::keyPressEvent(QKeyEvent* e)
{
    int key = e->key();
    bool viewVisible = !_imp->listView->isHidden();

    assert( _imp->listView->model() );
    int count = _imp->listView->model()->rowCount();
    QModelIndex currentIndex = _imp->listView->currentIndex();

    if (key == Qt::Key_Escape) {
        if (_imp->quickExitEnabled) {
            NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
            if (isNodeDialog) {
                isNodeDialog->finish(false);
            } else {
                if (_imp->dialog) {
                    _imp->dialog->accept();
                }
            }
        } else {
            if ( _imp->listView->isVisible() ) {
                _imp->listView->hide();
            } else {
                _imp->dialog->reject();
            }
        }
        e->accept();
    } else if (key == Qt::Key_Down) {
        if (viewVisible) {
            int row = currentIndex.row() + 1;

            ///Handle circular selection
            if (row >= count) {
                row = 0;
            }

            QModelIndex index = _imp->listView->model()->index(row, 0);
            _imp->listView->setCurrentIndex(index);
        }
    } else if (key == Qt::Key_Up) {
        if (viewVisible) {
            int row = currentIndex.row() - 1;

            ///Handle circular selection
            if (row < 0) {
                row = count - 1;
            }

            QModelIndex index = _imp->listView->model()->index(row, 0);
            _imp->listView->setCurrentIndex(index);
        }
    } else if ( (key == Qt::Key_Enter) || (key == Qt::Key_Return) ) {
        _imp->listView->hide();
        if (_imp->model->rowCount() == 1) {
            setText( _imp->model->index(0).data().toString() );
            Q_EMIT itemCompletionChosen();
            if (_imp->quickExitEnabled) {
                NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
                if (isNodeDialog) {
                    isNodeDialog->finish(true);
                } else {
                    if (_imp->dialog) {
                        _imp->dialog->accept();
                    }
                }
            }
            e->accept();
        } else {
            const QItemSelection selection = _imp->listView->selectionModel()->selection();
            QModelIndexList indexes = selection.indexes();
            bool doDialogEnd = false;
            if (indexes.size() == 1) {
                setText( _imp->model->index( indexes[0].row() ).data().toString() );
                doDialogEnd = true;
            } else if ( indexes.isEmpty() ) {
                if (_imp->model->rowCount() > 1) {
                    doDialogEnd = true;
                    setText( _imp->model->index(0).data().toString() );
                }
            }
            if (doDialogEnd) {
                Q_EMIT itemCompletionChosen();
                if (_imp->quickExitEnabled) {
                    NodeCreationDialog* isNodeDialog = dynamic_cast<NodeCreationDialog*>(_imp->dialog);
                    if (isNodeDialog) {
                        isNodeDialog->finish(true);
                    } else {
                        if (_imp->dialog) {
                            _imp->dialog->accept();
                        }
                    }
                }
                e->accept();
            } else {
                QLineEdit::keyPressEvent(e);
            }
        }
    } else {
        QLineEdit::keyPressEvent(e);
    }
} // keyPressEvent

void
CompleterLineEdit::showCompleter()
{
    _imp->listView->setFixedWidth( width() );
    filterText( text() );
}

struct NodeCreationDialogPrivate
{
    QVBoxLayout* layout;
    CompleterLineEdit* textEdit;
    PluginsMap items;
    bool isFinished;

    NodeCreationDialogPrivate()
        : layout(NULL)
        , textEdit(NULL)
        , items( appPTR->getPluginsList() )
        , isFinished(false)
    {
    }
};

static int
getPluginWeight(const QString& pluginID,
                int majVersion)
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );
    QString propName( pluginID + QString::number(majVersion) + QString::fromUtf8("_tab_weight") );
    int curValue = settings.value( propName, QVariant(0) ).toInt();

    return curValue;
}

static void
incrementPluginWeight(const QString& pluginID,
                      int majVersion)
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );
    QString propName( pluginID + QString::number(majVersion) + QString::fromUtf8("_tab_weight") );
    int curValue = settings.value( propName, QVariant(0) ).toInt();

    ++curValue;
    settings.setValue( propName, QVariant(curValue) );
}

NodeCreationDialog::NodeCreationDialog(const QString& initialFilter,
                                       QWidget* parent)
    : QDialog(parent)
    , _imp( new NodeCreationDialogPrivate() )
{
    setWindowTitle( tr("Node Creation Tool") );
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setObjectName( QString::fromUtf8("nodeCreationDialog") );
    setAttribute(Qt::WA_DeleteOnClose, true);
    _imp->layout = new QVBoxLayout(this);
    _imp->layout->setContentsMargins(0, 0, 0, 0);


    CompleterLineEdit::PluginsNamesMap pluginsMap;
    QString initialFilterName;
    std::string stdInitialFilter = initialFilter.toStdString();
    int i = 0;
    for (PluginsMap::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it) {
        if ( it->second.empty() ) {
            continue;
        }


        if (it->second.size() == 1) {
            std::pair<QString, QString> idNamePair;
            Plugin* p = ( *it->second.begin() );
            if ( !p->getIsUserCreatable() ) {
                continue;
            }

            idNamePair.second = p->generateUserFriendlyPluginID();

            int indexOfBracket = idNamePair.second.lastIndexOf( QString::fromUtf8("  [") );
            if (indexOfBracket != -1) {
                idNamePair.first = idNamePair.second.left(indexOfBracket);
            }

            int weight = getPluginWeight( p->getPluginID(), p->getMajorVersion() );
            pluginsMap.insert( std::make_pair(weight, idNamePair) );

            if (it->first == stdInitialFilter) {
                initialFilterName = idNamePair.first;
            }
            ++i;
        } else {
            QString bestMajorName;
            for (PluginVersionsOrdered::reverse_iterator it2 = it->second.rbegin(); it2 != it->second.rend(); ++it2) {
                if ( !(*it2)->getIsUserCreatable() ) {
                    continue;
                }
                std::pair<QString, QString> idNamePair;
                if ( it2 == it->second.rbegin() ) {
                    idNamePair.second = (*it2)->generateUserFriendlyPluginID();
                    bestMajorName = idNamePair.second;
                } else {
                    idNamePair.second = (*it2)->generateUserFriendlyPluginIDMajorEncoded();
                }


                int indexOfBracket = idNamePair.second.lastIndexOf( QString::fromUtf8("  [") );
                if (indexOfBracket != -1) {
                    idNamePair.first = idNamePair.second.left(indexOfBracket);
                }

                ++i;

                int weight = getPluginWeight( (*it2)->getPluginID(), (*it2)->getMajorVersion() );
                pluginsMap.insert( std::make_pair(weight, idNamePair) );
            }
            if (it->first == stdInitialFilter) {
                initialFilterName = bestMajorName;
            }
        }
    }

    _imp->textEdit = new CompleterLineEdit(pluginsMap, true, this);
    _imp->textEdit->setPlaceholderText( tr("Search nodes...") );
    {
        // The search field and the results below it read as one panel: same background and
        // thin border as the list, the selection colour as an underline, a magnifier in front.
        QFontMetrics fm = _imp->textEdit->fontMetrics();
        const QColor base = settingsColor(&Settings::getBaseColor);
        const QColor text = settingsColor(&Settings::getTextColor);
        const QColor sel = settingsColor(&Settings::getSelectionColor);

        // Wide enough for "Name   [Group]" rows without eliding the common ones.
        _imp->textEdit->setMinimumWidth( fm.horizontalAdvance( QLatin1Char('M') ) * 30 );
        // Fix the height on the dialog too: left to its size hint the frameless dialog comes
        // out shorter than the field and cuts off its bottom edge.
        const int fieldHeight = fm.height() + 16;
        _imp->textEdit->setFixedHeight(fieldHeight);
        setFixedHeight(fieldHeight);
        _imp->textEdit->setStyleSheet( QString::fromUtf8(
                                           "QLineEdit { background-color: %1; color: %2; border: 1px solid %3; "
                                           "border-bottom: 2px solid %4; border-radius: 0px; padding: 2px 4px; "
                                           "selection-background-color: %4; }")
                                       .arg( base.name() )
                                       .arg( text.name() )
                                       .arg( base.darker(170).name() )
                                       .arg( sel.name() ) );

        QPixmap pm(64, 64);
        pm.fill(Qt::transparent);
        {
            QPainter painter(&pm);
            painter.setRenderHint(QPainter::Antialiasing, true);
            QColor dim = text;
            dim.setAlpha(150);
            QPen pen(dim);
            pen.setWidthF(6.);
            pen.setCapStyle(Qt::RoundCap);
            painter.setPen(pen);
            painter.drawEllipse(QPointF(27., 27.), 16., 16.);
            painter.drawLine( QPointF(39., 39.), QPointF(54., 54.) );
        }
        _imp->textEdit->addAction(QIcon(pm), QLineEdit::LeadingPosition);
    }
    if ( !initialFilterName.isEmpty() ) {
        _imp->textEdit->setText(initialFilterName);
    }

    _imp->layout->addWidget(_imp->textEdit);

    // Centre the search field on the cursor.
    QPoint global = QCursor::pos();
    QSize sizeH = sizeHint();
    global.rx() -= sizeH.width() / 2;
    global.ry() -= sizeH.height() / 2;
    move( global.x(), global.y() );
    _imp->textEdit->setFocus();
    _imp->textEdit->selectAll();
    QTimer::singleShot( 20, _imp->textEdit, SLOT(showCompleter()) );
}

NodeCreationDialog::~NodeCreationDialog()
{
}

QString
NodeCreationDialog::getNodeName(int *major) const
{
    QString name = _imp->textEdit->text();


    for (PluginsMap::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it) {
        if (it->second.size() == 1) {
            if ( ( *it->second.begin() )->generateUserFriendlyPluginID() == name ) {
                Plugin* p = ( *it->second.begin() );
                *major = p->getMajorVersion();
                const QString& ret = p->getPluginID();
                incrementPluginWeight(ret, *major);

                return ret;
            }
        } else {
            for (PluginVersionsOrdered::reverse_iterator it2 = it->second.rbegin(); it2 != it->second.rend(); ++it2) {
                if ( it2 == it->second.rbegin() ) {
                    if ( (*it2)->generateUserFriendlyPluginID() == name ) {
                        *major = (*it2)->getMajorVersion();
                        const QString& ret = (*it2)->getPluginID();
                        incrementPluginWeight(ret, *major);

                        return ret;
                    }
                } else {
                    if ( (*it2)->generateUserFriendlyPluginIDMajorEncoded() == name ) {
                        *major = (*it2)->getMajorVersion();
                        const QString& ret = (*it2)->getPluginID();
                        incrementPluginWeight(ret, *major);

                        return ret;
                    }
                }
            }
        }
    }

    return QString();
}

void
NodeCreationDialog::keyPressEvent(QKeyEvent* e)
{
    if ( (e->key() == Qt::Key_Return) || (e->key() == Qt::Key_Enter) ) {
        finish(true);
    } else if (e->key() == Qt::Key_Escape) {
        finish(false);
    } else {
        QDialog::keyPressEvent(e);
    }
}

void
NodeCreationDialog::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::ActivationChange) {
        if ( !isActiveWindow() && ( qApp->activeWindow() != _imp->textEdit->getView() ) ) {
            finish(false);

            return;
        }
    }
    QDialog::changeEvent(e);
}

void
NodeCreationDialog::finish(bool accepted)
{
    if (_imp->isFinished) {
        return;
    }
    _imp->isFinished = true;
    Q_EMIT dialogFinished(accepted);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_NodeCreationDialog.cpp"
