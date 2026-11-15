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

// Derived from vectorNuke (https://github.com/erinwnash/vectorNuke), GPL-2.0:
// port of vectornuke/parsers/svg_parser.py.

#ifndef NATRON_ENGINE_DEV_MASK_PARSERS_SVGPARSER_H
#define NATRON_ENGINE_DEV_MASK_PARSERS_SVGPARSER_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <map>
#include <utility>
#include <vector>

#include <QIODevice>
#include <QString>
#include <QXmlStreamReader>

#include "Global/Macros.h"

#include "../VectorDocument.h"

NATRON_NAMESPACE_ENTER

namespace VectorMask {

/**
 * @brief Parses SVG files into a VectorDocument: <path> (full grammar
 * M/L/H/V/C/S/Q/T/A/Z with implicit repeats), rect/circle/ellipse/line/polyline/
 * polygon primitives, nested transforms (matrix chain), and cascading
 * style/opacity/fill/stroke. Elements under defs/clipPath/gradients/pattern/mask
 * are skipped. Gradient fills degrade to no fill color (the shape still imports).
 */
class SvgParser
{
public:

    SvgParser();

    /**
     * @brief Parses SVG content. On failure returns false and sets errorString.
     * doc is cleared and filled with the result; sourceFormat is set to "SVG".
     */
    bool parseContent(const QString& svgContent, VectorDocument* doc, QString* errorString);
    bool parseDevice(QIODevice* device, VectorDocument* doc, QString* errorString);

private:

    void reset(VectorDocument* doc);
    bool parseStream(QXmlStreamReader& xml, VectorDocument* doc, QString* errorString);

    // Geometry builders (take the accumulated matrix, merged styles, layer name and
    // the element's attributes).
    void buildPathShapes(const QString& d, const QString& id, const Matrix2D& matrix,
                         const std::map<QString, QString>& styles, const QString& layerName);
    void buildRectShape(const QXmlStreamAttributes& atts, const Matrix2D& matrix,
                        const std::map<QString, QString>& styles, const QString& layerName);
    void buildEllipseFromParams(const QString& id, double cx, double cy, double rx, double ry,
                                const Matrix2D& matrix, const std::map<QString, QString>& styles,
                                const QString& layerName, const char* defaultName);
    void buildLineShape(const QXmlStreamAttributes& atts, const Matrix2D& matrix,
                        const std::map<QString, QString>& styles, const QString& layerName);
    void buildPolygonShape(const QXmlStreamAttributes& atts, const Matrix2D& matrix,
                           const std::map<QString, QString>& styles, const QString& layerName,
                           bool isPolygon);

    void applyStylesAndAppend(const VectorShape& shape, const std::map<QString, QString>& styles);

    VectorDocument* _doc;
};

/**
 * @brief Parses an SVG path 'd' string into a list of (segments, isClosed) subpaths.
 * Exposed for tests. Never throws; malformed data yields as many subpaths as parsed.
 */
std::vector<std::pair<std::vector<SegmentPtr>, bool> > parseSvgPathData(const QString& dString);

} // namespace VectorMask

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_PARSERS_SVGPARSER_H
