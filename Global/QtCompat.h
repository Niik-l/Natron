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
#ifndef NATRON_GLOBAL_QTCOMPAT_H
#define NATRON_GLOBAL_QTCOMPAT_H

#include "Global/Macros.h"

#include <QtGlobal> // for Q_OS_*
#include <QString>
#include <QUrl>
#include <QFileInfo>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QRegularExpression>
#else
#include <QRegExp>
#endif

NATRON_NAMESPACE_ENTER

namespace QtCompat {

// Wildcard-to-regex conversion helper for Qt5/Qt6 compatibility.
// On Qt6, QRegExp is removed; use QRegularExpression with wildcardToRegularExpression().
// On Qt5, use QRegExp with Wildcard pattern syntax directly.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
inline QRegularExpression wildcardToRegex(const QString& pattern, Qt::CaseSensitivity cs = Qt::CaseSensitive) {
    QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
    if (cs == Qt::CaseInsensitive) {
        opts |= QRegularExpression::CaseInsensitiveOption;
    }
    return QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern), opts);
}

// Unanchored wildcard for use with QString::contains() (partial matching).
inline QRegularExpression wildcardToRegexUnanchored(const QString& pattern, Qt::CaseSensitivity cs = Qt::CaseSensitive) {
    QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
    if (cs == Qt::CaseInsensitive) {
        opts |= QRegularExpression::CaseInsensitiveOption;
    }
    // wildcardToRegularExpression produces anchored pattern (\A...\z);
    // for partial matching, use UnanchoredWildcardConversion (Qt 6.6+)
    // or strip the anchors manually
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    return QRegularExpression(
        QRegularExpression::wildcardToRegularExpression(pattern, QRegularExpression::UnanchoredWildcardConversion),
        opts);
#else
    QString rx = QRegularExpression::wildcardToRegularExpression(pattern);
    // Strip \A(?:  prefix and  )\z  suffix added by wildcardToRegularExpression
    if (rx.startsWith(QLatin1String("\\A(?:")) && rx.endsWith(QLatin1String(")\\z"))) {
        rx = rx.mid(5, rx.length() - 8);
    }
    return QRegularExpression(rx, opts);
#endif
}
#else
inline QRegExp wildcardToRegex(const QString& pattern, Qt::CaseSensitivity cs = Qt::CaseSensitive) {
    return QRegExp(pattern, cs, QRegExp::Wildcard);
}

inline QRegExp wildcardToRegexUnanchored(const QString& pattern, Qt::CaseSensitivity cs = Qt::CaseSensitive) {
    return QRegExp(pattern, cs, QRegExp::Wildcard);
}
#endif
/*Removes the . and the extension from the filename and also
 * returns the extension as a string.*/
inline QString
removeFileExtension(QString & filename)
{
    //qDebug() << "remove file ext from" << filename;
    QFileInfo fi(filename);
    QString extension = fi.suffix();

    if ( !extension.isEmpty() ) {
        filename.truncate(filename.size() - extension.size() - 1);
    }

    //qDebug() << "->" << filename << fi.suffix();
    return extension;
}

// Define compatibility typedefs so code builds with Qt5 & Qt6
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef QEnterEvent QEnterEvent;
#elif QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
typedef QEvent QEnterEvent;
#else
#error "Unsupported version of QT"
#endif

} // namespace QtCompat

NATRON_NAMESPACE_EXIT

#endif // NATRON_GLOBAL_QTCOMPAT_H
