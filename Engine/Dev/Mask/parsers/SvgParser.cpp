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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "SvgParser.h"

#include <cassert>
#include <cmath>
#include <map>

#include <QIODevice>
#include <QStringList>
#include <QRegularExpression>
#include <QXmlStreamReader>

NATRON_NAMESPACE_ENTER

namespace VectorMask {

namespace {

const double kKappa = 0.5522847498; // circle-approximation constant

// ---------------------------------------------------------------------------
// Attribute / length / color parsing helpers
// ---------------------------------------------------------------------------

QString
attrValue(const QXmlStreamAttributes& atts, const char* name)
{
    // Match by local name so namespaced attributes (e.g. xml:*) still resolve.
    for (int i = 0; i < atts.size(); ++i) {
        if (atts[i].name().compare(QLatin1String(name), Qt::CaseInsensitive) == 0) {
            return atts[i].value().toString();
        }
    }
    return QString();
}

double
parseLength(const QString& valStr, double defaultValue = 0.0)
{
    if (valStr.isEmpty()) {
        return defaultValue;
    }
    const QString s = valStr.trimmed().toLower();
    bool ok = false;
    if (s.endsWith(QLatin1String("px"))) {
        return s.left(s.size() - 2).toDouble(&ok);
    } else if (s.endsWith(QLatin1String("pt"))) {
        return s.left(s.size() - 2).toDouble(&ok) * (96.0 / 72.0);
    } else if (s.endsWith(QLatin1String("in"))) {
        return s.left(s.size() - 2).toDouble(&ok) * 96.0;
    } else if (s.endsWith(QLatin1String("mm"))) {
        return s.left(s.size() - 2).toDouble(&ok) * (96.0 / 25.4);
    } else if (s.endsWith(QLatin1String("cm"))) {
        return s.left(s.size() - 2).toDouble(&ok) * (96.0 / 2.54);
    } else if (s.endsWith(QChar::fromLatin1('%'))) {
        return s.left(s.size() - 1).toDouble(&ok) * 0.01 * defaultValue;
    }
    return s.toDouble(&ok);
}

double
parseLengthOr(const QString& valStr, double defaultValue)
{
    if (valStr.isEmpty()) {
        return defaultValue;
    }
    return parseLength(valStr, defaultValue);
}

double
rgbComponent(const QString& val)
{
    if (val.endsWith(QChar::fromLatin1('%'))) {
        return val.left(val.size() - 1).toDouble() / 100.0;
    }
    return val.toDouble() / 255.0;
}

bool
parseColor(const QString& valStr, Color* out)
{
    if (valStr.isEmpty() || !out) {
        return false;
    }
    const QString s = valStr.trimmed().toLower();
    if (namedColor(s.toStdString(), out)) {
        return true;
    }
    if (s.startsWith(QChar::fromLatin1('#'))) {
        return Color::fromHex(s.toStdString(), out);
    }
    if (s.startsWith(QLatin1String("rgb"))) {
        const QRegularExpression rx(QLatin1String(
            "rgba?\\s*\\(\\s*([\\d.%]+)\\s*,\\s*([\\d.%]+)\\s*,\\s*([\\d.%]+)(?:\\s*,\\s*([\\d.%]+))?\\s*\\)"));
        QRegularExpressionMatch m = rx.match(s);
        if (m.hasMatch()) {
            const double r = rgbComponent(m.captured(1));
            const double g = rgbComponent(m.captured(2));
            const double b = rgbComponent(m.captured(3));
            const double a = m.captured(4).isEmpty() ? 1.0 : rgbComponent(m.captured(4));
            *out = Color(r, g, b, a);
            return true;
        }
    }
    return false;
}

/** Direct presentation attributes + inline CSS 'style' attribute, child wins. */
std::map<QString, QString>
parseStyleDict(const QXmlStreamAttributes& atts)
{
    static const char* styleKeys[] = {
        "fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity"
    };
    std::map<QString, QString> styles;
    for (int i = 0; i < atts.size(); ++i) {
        const QString name = atts[i].name().toString().toLower();
        for (int k = 0; k < 6; ++k) {
            if (name == QLatin1String(styleKeys[k])) {
                styles[name] = atts[i].value().toString();
                break;
            }
        }
    }
    const QString styleAttr = attrValue(atts, "style");
    if (!styleAttr.isEmpty()) {
        const QStringList items = styleAttr.split(QChar::fromLatin1(';'));
        for (QStringList::const_iterator it = items.begin(); it != items.end(); ++it) {
            const int colon = it->indexOf(QChar::fromLatin1(':'));
            if (colon != -1) {
                styles[it->left(colon).trimmed().toLower()] = it->mid(colon + 1).trimmed();
            }
        }
    }
    return styles;
}

// ---------------------------------------------------------------------------
// SVG path 'd' tokenizer: commands are single letters, numbers carry an optional
// exponent. The tokenizer mirrors TOKEN_RE in the Python source (the letter 'e'
// is consumed by number exponents, never emitted as a command).
// ---------------------------------------------------------------------------

struct PathToken
{
    bool isCommand = false;
    QChar cmd;
    double value = 0.0;

    PathToken(QChar c)
        : isCommand(true), cmd(c) {}
    explicit PathToken(double v)
        : isCommand(false), value(v) {}
};

std::vector<PathToken>
tokenizePathData(const QString& d)
{
    std::vector<PathToken> tokens;
    const int n = d.size();
    int i = 0;
    while (i < n) {
        const QChar c = d[i];
        if (c.isSpace() || c == QChar::fromLatin1(',')) {
            ++i;
            continue;
        }
        if (c.isLetter()) {
            tokens.push_back(PathToken(c));
            ++i;
            continue;
        }
        // Number: [-+]?((\d*\.\d+)|\d+)([eE][-+]?\d+)?
        const int start = i;
        if (c == QChar::fromLatin1('+') || c == QChar::fromLatin1('-')) {
            ++i;
        }
        while (i < n && d[i].isDigit()) ++i;
        if (i < n && d[i] == QChar::fromLatin1('.')) {
            ++i;
            while (i < n && d[i].isDigit()) ++i;
        }
        if (i < n && (d[i] == QChar::fromLatin1('e') || d[i] == QChar::fromLatin1('E'))) {
            int j = i + 1;
            if (j < n && (d[j] == QChar::fromLatin1('+') || d[j] == QChar::fromLatin1('-'))) {
                ++j;
            }
            if (j < n && d[j].isDigit()) {
                while (j < n && d[j].isDigit()) ++j;
                i = j;
            }
        }
        if (i == start) {
            // Unparsable char: skip it (defensive, mirrors regex findall skipping junk)
            ++i;
            continue;
        }
        tokens.push_back(PathToken(d.mid(start, i - start).toDouble()));
    }
    return tokens;
}

} // anonymous namespace

std::vector<std::pair<std::vector<SegmentPtr>, bool> >
parseSvgPathData(const QString& dString)
{
    std::vector<PathToken> tokens = tokenizePathData(dString);

    std::vector<std::pair<std::vector<SegmentPtr>, bool> > subpaths;
    std::vector<SegmentPtr> currentSegments;
    Point2D currentPt(0.0, 0.0);
    Point2D subpathStart(0.0, 0.0);
    bool hasLastControl = false;
    Point2D lastControlPt(0.0, 0.0);
    QString lastCmd;
    bool isClosed = false;

    auto finishSubpath = [&]() {
        if (!currentSegments.empty()) {
            subpaths.push_back(std::make_pair(currentSegments, isClosed));
            currentSegments.clear();
            isClosed = false;
        }
    };

    auto getFloat = [&](bool* ok) -> double {
        if (tokens.empty()) {
            *ok = false;
            return 0.0;
        }
        const PathToken t = tokens.front();
        tokens.erase(tokens.begin());
        if (t.isCommand) {
            *ok = false;
            return 0.0;
        }
        *ok = true;
        return t.value;
    };

    auto getPoint = [&](const Point2D& relativeTo, bool isRelative, bool* ok) -> Point2D {
        const double x = getFloat(ok);
        if (!*ok) return Point2D();
        const double y = getFloat(ok);
        if (!*ok) return Point2D(x, 0.0);
        if (isRelative) {
            return Point2D(relativeTo.x + x, relativeTo.y + y);
        }
        return Point2D(x, y);
    };

    // Python: `_is_coord` checks the first char of the raw token. With the tokenizer
    // above, a number token is always a coord and a command token never is.
    auto peekIsCoord = [&]() -> bool {
        return !tokens.empty() && !tokens.front().isCommand;
    };
    auto putBackCoord = [&](double v) {
        tokens.insert(tokens.begin(), PathToken(v));
    };

    while (!tokens.empty()) {
        PathToken tok = tokens.front();
        tokens.erase(tokens.begin());

        QString cmd;
        if (tok.isCommand) {
            cmd = QString(tok.cmd);
        } else {
            // Implicit repeat of the previous command
            if (lastCmd == QLatin1String("M")) {
                cmd = QLatin1String("L");
            } else if (lastCmd == QLatin1String("m")) {
                cmd = QLatin1String("l");
            } else {
                cmd = lastCmd;
            }
            putBackCoord(tok.value);
        }
        if (cmd.isEmpty()) {
            continue;
        }

        const bool isRel = cmd[0].isLower();
        const QString up = cmd.toUpper();
        bool ok = true;

        if (up == QLatin1String("M")) {
            finishSubpath();
            currentPt = getPoint(currentPt, isRel, &ok);
            if (!ok) break;
            subpathStart = currentPt;
            hasLastControl = false;
            lastCmd = cmd;
        } else if (up == QLatin1String("L")) {
            while (peekIsCoord()) {
                const Point2D nextPt = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                currentSegments.push_back(SegmentPtr(new LineSegment(currentPt, nextPt)));
                currentPt = nextPt;
            }
            hasLastControl = false;
            lastCmd = cmd;
        } else if (up == QLatin1String("H")) {
            while (peekIsCoord()) {
                const double x = getFloat(&ok);
                if (!ok) break;
                const Point2D nextPt(isRel ? currentPt.x + x : x, currentPt.y);
                currentSegments.push_back(SegmentPtr(new LineSegment(currentPt, nextPt)));
                currentPt = nextPt;
            }
            hasLastControl = false;
            lastCmd = cmd;
        } else if (up == QLatin1String("V")) {
            while (peekIsCoord()) {
                const double y = getFloat(&ok);
                if (!ok) break;
                const Point2D nextPt(currentPt.x, isRel ? currentPt.y + y : y);
                currentSegments.push_back(SegmentPtr(new LineSegment(currentPt, nextPt)));
                currentPt = nextPt;
            }
            hasLastControl = false;
            lastCmd = cmd;
        } else if (up == QLatin1String("C")) {
            while (peekIsCoord()) {
                const Point2D c1 = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                const Point2D c2 = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                const Point2D pEnd = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                currentSegments.push_back(SegmentPtr(new CubicBezierSegment(currentPt, c1, c2, pEnd)));
                hasLastControl = true;
                lastControlPt = c2;
                currentPt = pEnd;
            }
            lastCmd = cmd;
        } else if (up == QLatin1String("S")) {
            while (peekIsCoord()) {
                Point2D c1;
                if ((lastCmd.toUpper() == QLatin1String("C") || lastCmd.toUpper() == QLatin1String("S")) && hasLastControl) {
                    // Reflection of lastControlPt across currentPt
                    c1 = Point2D(2.0 * currentPt.x - lastControlPt.x, 2.0 * currentPt.y - lastControlPt.y);
                } else {
                    c1 = currentPt;
                }
                const Point2D c2 = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                const Point2D pEnd = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                currentSegments.push_back(SegmentPtr(new CubicBezierSegment(currentPt, c1, c2, pEnd)));
                hasLastControl = true;
                lastControlPt = c2;
                currentPt = pEnd;
            }
            lastCmd = cmd;
        } else if (up == QLatin1String("Q")) {
            while (peekIsCoord()) {
                const Point2D qCtrl = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                const Point2D pEnd = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                CubicBezierSegment seg = quadToCubic(currentPt, qCtrl, pEnd);
                currentSegments.push_back(SegmentPtr(new CubicBezierSegment(seg)));
                hasLastControl = true;
                lastControlPt = qCtrl;
                currentPt = pEnd;
            }
            lastCmd = cmd;
        } else if (up == QLatin1String("T")) {
            while (peekIsCoord()) {
                Point2D qCtrl;
                if ((lastCmd.toUpper() == QLatin1String("Q") || lastCmd.toUpper() == QLatin1String("T")) && hasLastControl) {
                    qCtrl = Point2D(2.0 * currentPt.x - lastControlPt.x, 2.0 * currentPt.y - lastControlPt.y);
                } else {
                    qCtrl = currentPt;
                }
                const Point2D pEnd = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                CubicBezierSegment seg = quadToCubic(currentPt, qCtrl, pEnd);
                currentSegments.push_back(SegmentPtr(new CubicBezierSegment(seg)));
                hasLastControl = true;
                lastControlPt = qCtrl;
                currentPt = pEnd;
            }
            lastCmd = cmd;
        } else if (up == QLatin1String("A")) {
            while (peekIsCoord()) {
                const double rx = getFloat(&ok);
                if (!ok) break;
                const double ry = getFloat(&ok);
                if (!ok) break;
                const double phi = getFloat(&ok);
                if (!ok) break;
                const double largeArcI = getFloat(&ok);
                if (!ok) break;
                const double sweepI = getFloat(&ok);
                if (!ok) break;
                const Point2D pEnd = getPoint(currentPt, isRel, &ok);
                if (!ok) break;
                const std::vector<SegmentPtr> arcSegs =
                    arcToCubicBeziers(currentPt, rx, ry, phi, largeArcI != 0.0, sweepI != 0.0, pEnd);
                for (std::vector<SegmentPtr>::const_iterator it = arcSegs.begin(); it != arcSegs.end(); ++it) {
                    currentSegments.push_back(*it);
                }
                hasLastControl = false;
                currentPt = pEnd;
            }
            lastCmd = cmd;
        } else if (up == QLatin1String("Z")) {
            if (currentPt.distanceTo(subpathStart) > 1e-4) {
                currentSegments.push_back(SegmentPtr(new LineSegment(currentPt, subpathStart)));
            }
            currentPt = subpathStart;
            hasLastControl = false;
            isClosed = true;
            lastCmd = cmd;
        }
    }

    finishSubpath();
    return subpaths;
}

// ---------------------------------------------------------------------------
// SvgParser — document level
// ---------------------------------------------------------------------------

SvgParser::SvgParser()
    : _doc(0)
{
}

void
SvgParser::reset(VectorDocument* doc)
{
    _doc = doc;
    if (_doc) {
        *_doc = VectorDocument();
        _doc->sourceFormat = "SVG";
    }
}

bool
SvgParser::parseContent(const QString& svgContent, VectorDocument* doc, QString* errorString)
{
    reset(doc);
    QXmlStreamReader xml(svgContent);
    return parseStream(xml, doc, errorString);
}

bool
SvgParser::parseDevice(QIODevice* device, VectorDocument* doc, QString* errorString)
{
    reset(doc);
    QXmlStreamReader xml(device);
    return parseStream(xml, doc, errorString);
}

bool
SvgParser::parseStream(QXmlStreamReader& xml, VectorDocument* doc, QString* errorString)
{
    assert(_doc == doc);
    (void)errorString;

    // Per-element context: accumulated matrix, merged styles, layer name.
    struct Ctx
    {
        Matrix2D mat;
        std::map<QString, QString> styles;
        QString layerName;
    };
    std::vector<Ctx> stack;
    bool rootHandled = false;
    int ignoreDepth = -1; // inside defs/clipPath/gradients/pattern/mask

    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType type = xml.readNext();
        if (type == QXmlStreamReader::StartElement) {
            const int depth = (int)stack.size();
            const QString tag = xml.name().toString().toLower();

            if (!rootHandled) {
                // The root <svg> element: viewport size + viewBox mapping.
                rootHandled = true;
                const QXmlStreamAttributes atts = xml.attributes();
                double vbMinX = 0.0, vbMinY = 0.0, vbW = 1920.0, vbH = 1080.0;
                bool hasVb = false;
                const QString viewBoxStr = attrValue(atts, "viewBox");
                if (!viewBoxStr.isEmpty()) {
                    const QStringList parts = viewBoxStr.split(QRegularExpression(QLatin1String("[\\s,]+")));
                    QStringList nonEmpty;
                    for (QStringList::const_iterator it = parts.begin(); it != parts.end(); ++it) {
                        if (!it->isEmpty()) nonEmpty.push_back(*it);
                    }
                    if (nonEmpty.size() == 4) {
                        vbMinX = nonEmpty[0].toDouble();
                        vbMinY = nonEmpty[1].toDouble();
                        vbW = nonEmpty[2].toDouble();
                        vbH = nonEmpty[3].toDouble();
                        hasVb = true;
                    }
                }
                const double width = parseLengthOr(attrValue(atts, "width"), hasVb ? vbW : 1920.0);
                const double height = parseLengthOr(attrValue(atts, "height"), hasVb ? vbH : 1080.0);
                _doc->width = width;
                _doc->height = height;

                Matrix2D rootTransform = Matrix2D::identity();
                if (hasVb && vbW > 0.0 && vbH > 0.0 &&
                    (vbW != width || vbH != height || vbMinX != 0.0 || vbMinY != 0.0)) {
                    rootTransform = Matrix2D::scale(width / vbW, height / vbH)
                                    .multiplied(Matrix2D::translate(-vbMinX, -vbMinY));
                }
                Ctx root;
                root.mat = rootTransform;
                root.layerName = QLatin1String("Layer1");
                stack.push_back(root);
                continue;
            }

            Ctx ctx = stack.empty() ? Ctx() : stack.back();
            ctx.styles = parseStyleDict(xml.attributes());

            bool ignored = tag == QLatin1String("defs") ||
                           tag == QLatin1String("clippath") ||
                           tag == QLatin1String("lineargradient") ||
                           tag == QLatin1String("radialgradient") ||
                           tag == QLatin1String("pattern") ||
                           tag == QLatin1String("mask");
            if (ignored && ignoreDepth == -1) {
                ignoreDepth = depth;
            }

            // Layer naming on <g>
            if (tag == QLatin1String("g")) {
                QString gId = attrValue(xml.attributes(), "id");
                if (gId.isEmpty()) {
                    gId = attrValue(xml.attributes(), "label"); // inkscape:label
                }
                if (!gId.isEmpty()) {
                    ctx.layerName = gId;
                }
            }

            // Local transform
            const Matrix2D local = Matrix2D::fromSvgTransform(attrValue(xml.attributes(), "transform"));
            ctx.mat = ctx.mat.multiplied(local);

            stack.push_back(ctx);

            if (ignoreDepth == -1) {
                if (tag == QLatin1String("path")) {
                    const QString d = attrValue(xml.attributes(), "d");
                    if (!d.isEmpty()) {
                        QString id = attrValue(xml.attributes(), "id");
                        if (id.isEmpty()) id = QLatin1String("Path");
                        buildPathShapes(d, id, ctx.mat, ctx.styles, ctx.layerName);
                    }
                } else if (tag == QLatin1String("rect")) {
                    buildRectShape(xml.attributes(), ctx.mat, ctx.styles, ctx.layerName);
                } else if (tag == QLatin1String("circle")) {
                    const double cx = parseLength(attrValue(xml.attributes(), "cx"), 0.0);
                    const double cy = parseLength(attrValue(xml.attributes(), "cy"), 0.0);
                    const double r = parseLength(attrValue(xml.attributes(), "r"), 0.0);
                    if (r > 0.0) {
                        QString id = attrValue(xml.attributes(), "id");
                        if (id.isEmpty()) id = QLatin1String("Circle");
                        buildEllipseFromParams(id, cx, cy, r, r, ctx.mat, ctx.styles, ctx.layerName, "Circle");
                    }
                } else if (tag == QLatin1String("ellipse")) {
                    const double cx = parseLength(attrValue(xml.attributes(), "cx"), 0.0);
                    const double cy = parseLength(attrValue(xml.attributes(), "cy"), 0.0);
                    const double rx = parseLength(attrValue(xml.attributes(), "rx"), 0.0);
                    const double ry = parseLength(attrValue(xml.attributes(), "ry"), 0.0);
                    if (rx > 0.0 && ry > 0.0) {
                        QString id = attrValue(xml.attributes(), "id");
                        if (id.isEmpty()) id = QLatin1String("Ellipse");
                        buildEllipseFromParams(id, cx, cy, rx, ry, ctx.mat, ctx.styles, ctx.layerName, "Ellipse");
                    }
                } else if (tag == QLatin1String("line")) {
                    buildLineShape(xml.attributes(), ctx.mat, ctx.styles, ctx.layerName);
                } else if (tag == QLatin1String("polyline") || tag == QLatin1String("polygon")) {
                    buildPolygonShape(xml.attributes(), ctx.mat, ctx.styles, ctx.layerName,
                                      tag == QLatin1String("polygon"));
                }
            }
        } else if (type == QXmlStreamReader::EndElement) {
            const int depth = (int)stack.size();
            if (!stack.empty()) {
                stack.pop_back();
            }
            if (ignoreDepth != -1 && depth - 1 == ignoreDepth) {
                ignoreDepth = -1;
            }
        }
    }

    return !xml.hasError();
}

void
SvgParser::applyStylesAndAppend(const VectorShape& shape, const std::map<QString, QString>& styles)
{
    VectorShape out = shape;

    Color fillColor;
    bool hasFill = false;
    std::map<QString, QString>::const_iterator fillIt = styles.find(QLatin1String("fill"));
    const QString fillVal = (fillIt != styles.end()) ? fillIt->second : QString(QLatin1String("#000000"));
    hasFill = parseColor(fillVal, &fillColor);

    Color strokeColor;
    bool hasStroke = false;
    std::map<QString, QString>::const_iterator strokeIt = styles.find(QLatin1String("stroke"));
    if (strokeIt != styles.end() && !strokeIt->second.isEmpty()) {
        hasStroke = parseColor(strokeIt->second, &strokeColor);
    }

    double opacity = 1.0;
    std::map<QString, QString>::const_iterator oIt = styles.find(QLatin1String("opacity"));
    if (oIt != styles.end()) {
        opacity = oIt->second.toDouble();
    }
    double fillOpacity = 1.0;
    oIt = styles.find(QLatin1String("fill-opacity"));
    if (oIt != styles.end()) {
        fillOpacity = oIt->second.toDouble();
    }
    double strokeOpacity = 1.0;
    oIt = styles.find(QLatin1String("stroke-opacity"));
    if (oIt != styles.end()) {
        strokeOpacity = oIt->second.toDouble();
    }

    if (hasFill) {
        fillColor.a *= (opacity * fillOpacity);
        out.hasFillColor = true;
        out.fillColor = fillColor;
    }
    if (hasStroke) {
        strokeColor.a *= (opacity * strokeOpacity);
        out.hasStrokeColor = true;
        out.strokeColor = strokeColor;
    }
    out.opacity = opacity;
    oIt = styles.find(QLatin1String("stroke-width"));
    out.strokeWidth = parseLengthOr(oIt != styles.end() ? oIt->second : QString(QLatin1String("1.0")), 1.0);

    if (_doc) {
        _doc->shapes.push_back(out);
    }
}

namespace {

std::vector<SegmentPtr>
transformSegments(const std::vector<SegmentPtr>& segments, const Matrix2D& matrix)
{
    std::vector<SegmentPtr> transformed;
    transformed.reserve(segments.size());
    for (std::vector<SegmentPtr>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
        const Point2D pStart = matrix.transformPoint((*it)->start);
        const Point2D pEnd = matrix.transformPoint((*it)->end);
        std::shared_ptr<CubicBezierSegment> cubic = std::dynamic_pointer_cast<CubicBezierSegment>(*it);
        if (cubic) {
            transformed.push_back(SegmentPtr(new CubicBezierSegment(pStart,
                                                                    matrix.transformPoint(cubic->c1),
                                                                    matrix.transformPoint(cubic->c2),
                                                                    pEnd)));
        } else {
            transformed.push_back(SegmentPtr(new LineSegment(pStart, pEnd)));
        }
    }
    return transformed;
}

} // anonymous namespace

void
SvgParser::buildPathShapes(const QString& d, const QString& id, const Matrix2D& matrix,
                           const std::map<QString, QString>& styles, const QString& layerName)
{
    const std::vector<std::pair<std::vector<SegmentPtr>, bool> > subpaths = parseSvgPathData(d);
    const std::string baseName = sanitizeName(id.toStdString(), "Path");

    for (size_t i = 0; i < subpaths.size(); ++i) {
        const std::vector<SegmentPtr>& segments = subpaths[i].first;
        const bool closed = subpaths[i].second;
        if (segments.empty()) {
            continue;
        }
        const std::vector<SegmentPtr> transformedSegs = transformSegments(segments, matrix);
        VectorShape shape;
        shape.name = (subpaths.size() > 1)
            ? baseName + "_" + std::to_string((long long)i + 1)
            : baseName;
        shape.vertices = segmentsToVertices(transformedSegs, closed);
        shape.closed = closed;
        shape.layerName = layerName.toStdString();
        applyStylesAndAppend(shape, styles);
    }
}

void
SvgParser::buildRectShape(const QXmlStreamAttributes& atts, const Matrix2D& matrix,
                          const std::map<QString, QString>& styles, const QString& layerName)
{
    const double x = parseLength(attrValue(atts, "x"), 0.0);
    const double y = parseLength(attrValue(atts, "y"), 0.0);
    const double w = parseLength(attrValue(atts, "width"), 0.0);
    const double h = parseLength(attrValue(atts, "height"), 0.0);
    if (w <= 0.0 || h <= 0.0) {
        return;
    }

    double rx = parseLength(attrValue(atts, "rx"), 0.0);
    double ry = parseLength(attrValue(atts, "ry"), 0.0);
    if (rx > 0.0 && ry == 0.0) {
        ry = rx;
    } else if (ry > 0.0 && rx == 0.0) {
        rx = ry;
    }
    rx = std::min(rx, w / 2.0);
    ry = std::min(ry, h / 2.0);

    std::vector<SegmentPtr> segments;
    if (rx > 0.0 && ry > 0.0) {
        // Rounded rectangle: 4 lines + 4 quarter-circle cubics
        const double kX = rx * kKappa;
        const double kY = ry * kKappa;

        const Point2D p0(x + rx, y);
        const Point2D p1(x + w - rx, y);
        segments.push_back(SegmentPtr(new LineSegment(p0, p1)));
        segments.push_back(SegmentPtr(new CubicBezierSegment(p1, Point2D(p1.x + kX, p1.y),
                                                             Point2D(x + w, y + ry - kY), Point2D(x + w, y + ry))));

        const Point2D p2(x + w, y + h - ry);
        segments.push_back(SegmentPtr(new LineSegment(Point2D(x + w, y + ry), p2)));
        segments.push_back(SegmentPtr(new CubicBezierSegment(p2, Point2D(p2.x, p2.y + kY),
                                                             Point2D(x + w - rx + kX, y + h), Point2D(x + w - rx, y + h))));

        const Point2D p3(x + rx, y + h);
        segments.push_back(SegmentPtr(new LineSegment(Point2D(x + w - rx, y + h), p3)));
        segments.push_back(SegmentPtr(new CubicBezierSegment(p3, Point2D(p3.x - kX, p3.y),
                                                             Point2D(x, y + h - ry + kY), Point2D(x, y + h - ry))));

        const Point2D p4(x, y + ry);
        segments.push_back(SegmentPtr(new LineSegment(Point2D(x, y + h - ry), p4)));
        segments.push_back(SegmentPtr(new CubicBezierSegment(p4, Point2D(p4.x, p4.y - kY),
                                                             Point2D(x + rx - kX, y), p0)));
    } else {
        segments.push_back(SegmentPtr(new LineSegment(Point2D(x, y), Point2D(x + w, y))));
        segments.push_back(SegmentPtr(new LineSegment(Point2D(x + w, y), Point2D(x + w, y + h))));
        segments.push_back(SegmentPtr(new LineSegment(Point2D(x + w, y + h), Point2D(x, y + h))));
        segments.push_back(SegmentPtr(new LineSegment(Point2D(x, y + h), Point2D(x, y))));
    }

    QString id = attrValue(atts, "id");
    if (id.isEmpty()) id = QLatin1String("Rectangle");
    const std::vector<SegmentPtr> transformedSegs = transformSegments(segments, matrix);
    VectorShape shape;
    shape.name = sanitizeName(id.toStdString(), "Rectangle");
    shape.vertices = segmentsToVertices(transformedSegs, true);
    shape.closed = true;
    shape.layerName = layerName.toStdString();
    applyStylesAndAppend(shape, styles);
}

void
SvgParser::buildEllipseFromParams(const QString& id, double cx, double cy, double rx, double ry,
                                  const Matrix2D& matrix, const std::map<QString, QString>& styles,
                                  const QString& layerName, const char* defaultName)
{
    const double kX = rx * kKappa;
    const double kY = ry * kKappa;

    const Point2D pTop(cx, cy - ry);
    const Point2D pRight(cx + rx, cy);
    const Point2D pBottom(cx, cy + ry);
    const Point2D pLeft(cx - rx, cy);

    std::vector<SegmentPtr> segments;
    segments.push_back(SegmentPtr(new CubicBezierSegment(pTop, Point2D(cx + kX, cy - ry), Point2D(cx + rx, cy - kY), pRight)));
    segments.push_back(SegmentPtr(new CubicBezierSegment(pRight, Point2D(cx + rx, cy + kY), Point2D(cx + kX, cy + ry), pBottom)));
    segments.push_back(SegmentPtr(new CubicBezierSegment(pBottom, Point2D(cx - kX, cy + ry), Point2D(cx - rx, cy + kY), pLeft)));
    segments.push_back(SegmentPtr(new CubicBezierSegment(pLeft, Point2D(cx - rx, cy - kY), Point2D(cx - kX, cy - ry), pTop)));

    const std::vector<SegmentPtr> transformedSegs = transformSegments(segments, matrix);
    VectorShape shape;
    shape.name = sanitizeName(id.isEmpty() ? std::string(defaultName) : id.toStdString(), defaultName);
    shape.vertices = segmentsToVertices(transformedSegs, true);
    shape.closed = true;
    shape.layerName = layerName.toStdString();
    applyStylesAndAppend(shape, styles);
}

void
SvgParser::buildLineShape(const QXmlStreamAttributes& atts, const Matrix2D& matrix,
                          const std::map<QString, QString>& styles, const QString& layerName)
{
    const double x1 = parseLength(attrValue(atts, "x1"), 0.0);
    const double y1 = parseLength(attrValue(atts, "y1"), 0.0);
    const double x2 = parseLength(attrValue(atts, "x2"), 0.0);
    const double y2 = parseLength(attrValue(atts, "y2"), 0.0);

    std::vector<SegmentPtr> segments;
    segments.push_back(SegmentPtr(new LineSegment(Point2D(x1, y1), Point2D(x2, y2))));

    QString id = attrValue(atts, "id");
    if (id.isEmpty()) id = QLatin1String("Line");
    const std::vector<SegmentPtr> transformedSegs = transformSegments(segments, matrix);
    VectorShape shape;
    shape.name = sanitizeName(id.toStdString(), "Line");
    shape.vertices = segmentsToVertices(transformedSegs, false);
    shape.closed = false;
    shape.layerName = layerName.toStdString();
    applyStylesAndAppend(shape, styles);
}

void
SvgParser::buildPolygonShape(const QXmlStreamAttributes& atts, const Matrix2D& matrix,
                             const std::map<QString, QString>& styles, const QString& layerName,
                             bool isPolygon)
{
    const QString rawPts = attrValue(atts, "points");
    const QStringList tokens = rawPts.split(QRegularExpression(QLatin1String("[\\s,]+")));
    std::vector<double> vals;
    for (QStringList::const_iterator it = tokens.begin(); it != tokens.end(); ++it) {
        if (!it->isEmpty()) {
            vals.push_back(it->toDouble());
        }
    }
    if (vals.size() < 4) {
        return;
    }

    std::vector<Point2D> points;
    for (size_t i = 0; i + 1 < vals.size(); i += 2) {
        points.push_back(Point2D(vals[i], vals[i + 1]));
    }

    std::vector<SegmentPtr> segments;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        segments.push_back(SegmentPtr(new LineSegment(points[i], points[i + 1])));
    }
    if (isPolygon && !points.empty()) {
        segments.push_back(SegmentPtr(new LineSegment(points.back(), points.front())));
    }

    QString id = attrValue(atts, "id");
    if (id.isEmpty()) id = isPolygon ? QLatin1String("Polygon") : QLatin1String("Polyline");
    const std::vector<SegmentPtr> transformedSegs = transformSegments(segments, matrix);
    VectorShape shape;
    shape.name = sanitizeName(id.toStdString(), isPolygon ? "Polygon" : "Polyline");
    shape.vertices = segmentsToVertices(transformedSegs, isPolygon);
    shape.closed = isPolygon;
    shape.layerName = layerName.toStdString();
    applyStylesAndAppend(shape, styles);
}

} // namespace VectorMask

NATRON_NAMESPACE_EXIT
