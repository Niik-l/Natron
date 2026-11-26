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
// port of vectornuke/core/models.py, core/bezier.py and core/transform.py.

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "VectorDocument.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

#include <QRegularExpression>
#include <QStringList>

NATRON_NAMESPACE_ENTER

namespace VectorMask {

bool
Color::fromHex(const std::string& hexStr, Color* out, double defaultAlpha)
{
    if (hexStr.empty() || !out) {
        return false;
    }
    std::string s = hexStr;
    if (!s.empty() && s[0] == '#') {
        s = s.substr(1);
    }
    if (s.size() == 3 || s.size() == 4) {
        // #RGB / #RGBA: each digit doubled
        for (size_t i = 0; i < s.size(); ++i) {
            if (!std::isxdigit((unsigned char)s[i])) {
                return false;
            }
        }
        auto value = [&s](size_t i) -> double {
            const char cc[3] = { s[i], s[i], 0 };
            char* endp = 0;
            long v = std::strtol(cc, &endp, 16);
            if (endp != cc + 2) return 0.0;
            return (double)v / 255.0;
        };
        // Validate hex digits first
        for (size_t i = 0; i < s.size(); ++i) {
            if (!std::isxdigit((unsigned char)s[i])) {
                return false;
            }
        }
        out->r = value(0);
        out->g = value(1);
        out->b = value(2);
        out->a = (s.size() == 4) ? value(3) : defaultAlpha;
        return true;
    } else if (s.size() == 6 || s.size() == 8) {
        for (size_t i = 0; i < s.size(); ++i) {
            if (!std::isxdigit((unsigned char)s[i])) {
                return false;
            }
        }
        auto value = [&s](size_t i) -> double {
            const char cc[3] = { s[i], s[i + 1], 0 };
            char* endp = 0;
            long v = std::strtol(cc, &endp, 16);
            if (endp != cc + 2) return 0.0;
            return (double)v / 255.0;
        };
        out->r = value(0);
        out->g = value(2);
        out->b = value(4);
        out->a = (s.size() == 8) ? value(6) : defaultAlpha;
        return true;
    }
    return false;
}

Color
Color::fromCmyk(double c, double m, double y, double k, double a)
{
    double r = (1.0 - c) * (1.0 - k);
    double g = (1.0 - m) * (1.0 - k);
    double b = (1.0 - y) * (1.0 - k);
    return Color(std::max(0.0, std::min(1.0, r)),
                 std::max(0.0, std::min(1.0, g)),
                 std::max(0.0, std::min(1.0, b)),
                 std::max(0.0, std::min(1.0, a)));
}

bool
namedColor(const std::string& nameLower, Color* out)
{
    if (!out) return false;
    static const std::map<std::string, Color> table = {
        { "none",       Color(0.0, 0.0, 0.0, 0.0) },
        { "transparent", Color(0.0, 0.0, 0.0, 0.0) },
        { "black",      Color(0.0, 0.0, 0.0, 1.0) },
        { "white",      Color(1.0, 1.0, 1.0, 1.0) },
        { "red",        Color(1.0, 0.0, 0.0, 1.0) },
        { "green",      Color(0.0, 0.5, 0.0, 1.0) },
        { "lime",       Color(0.0, 1.0, 0.0, 1.0) },
        { "blue",       Color(0.0, 0.0, 1.0, 1.0) },
        { "yellow",     Color(1.0, 1.0, 0.0, 1.0) },
        { "cyan",       Color(0.0, 1.0, 1.0, 1.0) },
        { "magenta",    Color(1.0, 0.0, 1.0, 1.0) },
        { "gray",       Color(0.5, 0.5, 0.5, 1.0) },
        { "grey",       Color(0.5, 0.5, 0.5, 1.0) },
        { "orange",     Color(1.0, 0.647, 0.0, 1.0) },
        { "purple",     Color(0.5, 0.0, 0.5, 1.0) },
    };
    std::map<std::string, Color>::const_iterator it = table.find(nameLower);
    if (it == table.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

std::string
sanitizeName(const std::string& name, const std::string& defaultName)
{
    if (name.empty()) {
        return defaultName;
    }
    std::string clean;
    clean.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        const char ch = name[i];
        if (std::isalnum((unsigned char)ch) || ch == '_') {
            clean.push_back(ch);
        } else {
            // Collapse consecutive underscores: only append if previous isn't one
            if (clean.empty() || clean[clean.size() - 1] != '_') {
                clean.push_back('_');
            }
        }
    }
    // Strip leading/trailing underscores
    size_t b = 0;
    size_t e = clean.size();
    while (b < e && clean[b] == '_') ++b;
    while (e > b && clean[e - 1] == '_') --e;
    clean = clean.substr(b, e - b);
    if (clean.empty()) {
        return defaultName;
    }
    if (std::isdigit((unsigned char)clean[0])) {
        return defaultName + "_" + clean;
    }
    return clean;
}

void
VectorShape::bounds(double* minX, double* minY, double* maxX, double* maxY) const
{
    if (vertices.empty()) {
        *minX = *minY = *maxX = *maxY = 0.0;
        return;
    }
    double lox = vertices.front().center.x;
    double loy = vertices.front().center.y;
    double hix = lox;
    double hiy = loy;
    for (size_t i = 1; i < vertices.size(); ++i) {
        lox = std::min(lox, vertices[i].center.x);
        loy = std::min(loy, vertices[i].center.y);
        hix = std::max(hix, vertices[i].center.x);
        hiy = std::max(hiy, vertices[i].center.y);
    }
    *minX = lox;
    *minY = loy;
    *maxX = hix;
    *maxY = hiy;
}

void
VectorDocument::bounds(double* minX, double* minY, double* maxX, double* maxY) const
{
    if (shapes.empty()) {
        *minX = 0.0;
        *minY = 0.0;
        *maxX = width;
        *maxY = height;
        return;
    }
    double lox = 0.0, loy = 0.0, hix = 0.0, hiy = 0.0;
    bool hasVertices = false;
    for (std::vector<VectorShape>::const_iterator it = shapes.begin(); it != shapes.end(); ++it) {
        if (it->vertices.empty()) continue;
        double x0, y0, x1, y1;
        it->bounds(&x0, &y0, &x1, &y1);
        if (!hasVertices) {
            lox = x0;
            loy = y0;
            hix = x1;
            hiy = y1;
            hasVertices = true;
        } else {
            lox = std::min(lox, x0);
            loy = std::min(loy, y0);
            hix = std::max(hix, x1);
            hiy = std::max(hiy, y1);
        }
    }
    if (!hasVertices) {
        *minX = 0.0;
        *minY = 0.0;
        *maxX = width;
        *maxY = height;
        return;
    }
    *minX = lox;
    *minY = loy;
    *maxX = hix;
    *maxY = hiy;
}

Matrix2D
Matrix2D::rotateRadians(double radians, double cx, double cy)
{
    const double cosA = std::cos(radians);
    const double sinA = std::sin(radians);
    if (cx != 0.0 || cy != 0.0) {
        // T(cx, cy) * R(radians) * T(-cx, -cy)
        return translate(cx, cy).multiplied(Matrix2D(cosA, sinA, -sinA, cosA, 0.0, 0.0))
               .multiplied(translate(-cx, -cy));
    }
    return Matrix2D(cosA, sinA, -sinA, cosA, 0.0, 0.0);
}

namespace {

QStringList
splitWs(const QString& s)
{
    const QRegularExpression rx(QLatin1String("[\\s,]+"));
    return s.split(rx, Qt::SkipEmptyParts);
}

} // anonymous namespace

Matrix2D
Matrix2D::fromSvgTransform(const QString& transformStr)
{
    if (transformStr.isEmpty()) {
        return identity();
    }
    Matrix2D res = identity();
    const QRegularExpression cmdRx(QLatin1String("([a-zA-Z]+)\\s*\\(([^)]*)\\)"));
    QRegularExpressionMatchIterator it = cmdRx.globalMatch(transformStr);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString cmd = match.captured(1).toLower();
        const QStringList args = splitWs(match.captured(2));

        if (args.isEmpty()) {
            continue;
        }
        bool ok = false;
        Matrix2D mat = identity();
        if (cmd == QString::fromUtf8("matrix") && args.size() >= 6) {
            const double v[6] = {
                args[0].toDouble(&ok), args[1].toDouble(), args[2].toDouble(),
                args[3].toDouble(), args[4].toDouble(), args[5].toDouble()
            };
            if (ok) {
                mat = Matrix2D(v[0], v[1], v[2], v[3], v[4], v[5]);
            }
        } else if (cmd == QString::fromUtf8("translate")) {
            mat = translate(args[0].toDouble(), args.size() > 1 ? args[1].toDouble() : 0.0);
        } else if (cmd == QString::fromUtf8("scale")) {
            mat = scale(args[0].toDouble(), args.size() > 1 ? args[1].toDouble() : args[0].toDouble());
        } else if (cmd == QString::fromUtf8("rotate")) {
            const double rad = args[0].toDouble() * M_PI / 180.0;
            const double cx = args.size() > 1 ? args[1].toDouble() : 0.0;
            const double cy = args.size() > 2 ? args[2].toDouble() : 0.0;
            mat = rotateRadians(rad, cx, cy);
        } else if (cmd == QString::fromUtf8("skewx")) {
            mat = skewXRadians(args[0].toDouble() * M_PI / 180.0);
        } else if (cmd == QString::fromUtf8("skewy")) {
            mat = skewYRadians(args[0].toDouble() * M_PI / 180.0);
        }
        res = res.multiplied(mat);
    }
    return res;
}

CubicBezierSegment
quadToCubic(const Point2D& start, const Point2D& qCtrl, const Point2D& end)
{
    // c1 = start + 2/3 * (qCtrl - start);  c2 = end + 2/3 * (qCtrl - end)
    const Point2D c1 = start + (qCtrl - start) * (2.0 / 3.0);
    const Point2D c2 = end + (qCtrl - end) * (2.0 / 3.0);
    return CubicBezierSegment(start, c1, c2, end);
}

namespace {

double
vectorAngle(double ux, double uy, double vx, double vy)
{
    const double dot = ux * vx + uy * vy;
    const double lenU = std::sqrt(ux * ux + uy * uy);
    const double lenV = std::sqrt(vx * vx + vy * vy);
    if (lenU == 0.0 || lenV == 0.0) {
        return 0.0;
    }
    double val = dot / (lenU * lenV);
    val = std::max(-1.0, std::min(1.0, val));
    double angle = std::acos(val);
    if ((ux * vy - uy * vx) < 0.0) {
        angle = -angle;
    }
    return angle;
}

Point2D
mapEllipsePoint(double px, double py, double rx, double ry, double cosPhi, double sinPhi,
                double cx, double cy)
{
    const double sx = px * rx;
    const double sy = py * ry;
    return Point2D(cosPhi * sx - sinPhi * sy + cx, sinPhi * sx + cosPhi * sy + cy);
}

} // anonymous namespace

std::vector<SegmentPtr>
arcToCubicBeziers(const Point2D& p1, double rx, double ry, double phiDeg,
                  bool largeArc, bool sweep, const Point2D& p2)
{
    std::vector<SegmentPtr> result;
    if (p1.distanceTo(p2) < 1e-6) {
        return result;
    }
    if (std::fabs(rx) < 1e-6 || std::fabs(ry) < 1e-6) {
        result.push_back(SegmentPtr(new LineSegment(p1, p2)));
        return result;
    }

    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const double phi = std::fmod(phiDeg, 360.0) * M_PI / 180.0;
    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);

    // Step 1: (x1', y1')
    const double dx = (p1.x - p2.x) / 2.0;
    const double dy = (p1.y - p2.y) / 2.0;
    const double x1Prime = cosPhi * dx + sinPhi * dy;
    const double y1Prime = -sinPhi * dx + cosPhi * dy;

    // Ensure radii are large enough
    const double lambda = (x1Prime * x1Prime) / (rx * rx) + (y1Prime * y1Prime) / (ry * ry);
    if (lambda > 1.0) {
        const double scaleR = std::sqrt(lambda);
        rx *= scaleR;
        ry *= scaleR;
    }

    // Step 2: (cx', cy')
    const double num = (rx * rx * ry * ry) - (rx * rx * y1Prime * y1Prime) - (ry * ry * x1Prime * x1Prime);
    const double den = (rx * rx * y1Prime * y1Prime) + (ry * ry * x1Prime * x1Prime);
    double sq = 0.0;
    if (den > 0.0) {
        sq = std::max(0.0, num / den);
    }
    double coef = std::sqrt(sq);
    if (largeArc == sweep) {
        coef = -coef;
    }
    const double cxPrime = coef * ((rx * y1Prime) / ry);
    const double cyPrime = coef * (-(ry * x1Prime) / rx);

    // Step 3: (cx, cy)
    const double cx = cosPhi * cxPrime - sinPhi * cyPrime + (p1.x + p2.x) / 2.0;
    const double cy = sinPhi * cxPrime + cosPhi * cyPrime + (p1.y + p2.y) / 2.0;

    // Step 4: theta1 and deltaTheta
    const double ux = (x1Prime - cxPrime) / rx;
    const double uy = (y1Prime - cyPrime) / ry;
    const double vx = (-x1Prime - cxPrime) / rx;
    const double vy = (-y1Prime - cyPrime) / ry;

    const double theta1 = vectorAngle(1.0, 0.0, ux, uy);
    double dTheta = vectorAngle(ux, uy, vx, vy);
    if (!sweep && dTheta > 0.0) {
        dTheta -= 2.0 * M_PI;
    } else if (sweep && dTheta < 0.0) {
        dTheta += 2.0 * M_PI;
    }

    // Split into segments of at most pi/2
    int numSegments = (int)std::ceil(std::fabs(dTheta) / (M_PI / 2.0));
    if (numSegments <= 0) {
        result.push_back(SegmentPtr(new LineSegment(p1, p2)));
        return result;
    }
    const double segmentDTheta = dTheta / (double)numSegments;
    const double alpha = segmentDTheta;

    double currentTheta = theta1;
    for (int i = 0; i < numSegments; ++i) {
        const double nextTheta = currentTheta + alpha;
        const double k = (4.0 / 3.0) * std::tan(alpha / 4.0);

        const double cosT1 = std::cos(currentTheta);
        const double sinT1 = std::sin(currentTheta);
        const double cosT2 = std::cos(nextTheta);
        const double sinT2 = std::sin(nextTheta);

        // Unit circle points and tangent handles
        const double e1x = cosT1, e1y = sinT1;
        const double e2x = cosT2, e2y = sinT2;
        const double c1x = e1x - k * sinT1;
        const double c1y = e1y + k * cosT1;
        const double c2x = e2x + k * sinT2;
        const double c2y = e2y - k * cosT2;

        const Point2D startPt = mapEllipsePoint(e1x, e1y, rx, ry, cosPhi, sinPhi, cx, cy);
        const Point2D handle1 = mapEllipsePoint(c1x, c1y, rx, ry, cosPhi, sinPhi, cx, cy);
        const Point2D handle2 = mapEllipsePoint(c2x, c2y, rx, ry, cosPhi, sinPhi, cx, cy);
        const Point2D endPt = mapEllipsePoint(e2x, e2y, rx, ry, cosPhi, sinPhi, cx, cy);

        result.push_back(SegmentPtr(new CubicBezierSegment(startPt, handle1, handle2, endPt)));
        currentTheta = nextTheta;
    }

    // Ensure start and end strictly match p1 and p2
    if (!result.empty()) {
        result.front()->start = p1;
        result.back()->end = p2;
    }
    return result;
}

std::vector<ShapeVertex>
segmentsToVertices(const std::vector<SegmentPtr>& segments, bool closed)
{
    std::vector<ShapeVertex> vertices;
    if (segments.empty()) {
        return vertices;
    }

    // Filter out zero-length line segments (cubic segments are always kept)
    std::vector<SegmentPtr> validSegs;
    for (std::vector<SegmentPtr>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
        const bool isCubic = (std::dynamic_pointer_cast<CubicBezierSegment>(*it) != NULL);
        if (it->get()->start.distanceTo(it->get()->end) > 1e-6 || isCubic) {
            validSegs.push_back(*it);
        }
    }

    if (validSegs.empty()) {
        vertices.push_back(ShapeVertex(segments.front()->start));
        return vertices;
    }

    const int numSegs = (int)validSegs.size();

    if (closed) {
        // vertex i = seg[i].start; incoming handle from seg[i-1].c2, outgoing to seg[i].c1
        for (int i = 0; i < numSegs; ++i) {
            const Segment& segOut = *validSegs[i];
            (void)segOut;
            const Point2D center = segOut.start;
            std::shared_ptr<CubicBezierSegment> inCubic = std::dynamic_pointer_cast<CubicBezierSegment>(validSegs[(i - 1 + numSegs) % numSegs]);
            std::shared_ptr<CubicBezierSegment> outCubic = std::dynamic_pointer_cast<CubicBezierSegment>(validSegs[i]);
            const Point2D leftTan = inCubic ? (inCubic->c2 - center) : Point2D(0.0, 0.0);
            const Point2D rightTan = outCubic ? (outCubic->c1 - center) : Point2D(0.0, 0.0);
            vertices.push_back(ShapeVertex(center, leftTan, rightTan));
        }
    } else {
        // Vertex 0: start of seg 0; vertices 1..N-1: seg[i].start; vertex N: seg[N-1].end
        std::shared_ptr<CubicBezierSegment> seg0 = std::dynamic_pointer_cast<CubicBezierSegment>(validSegs[0]);
        vertices.push_back(ShapeVertex(validSegs[0]->start, Point2D(0.0, 0.0),
                                       seg0 ? (seg0->c1 - seg0->start) : Point2D(0.0, 0.0)));
        for (int i = 1; i < numSegs; ++i) {
            std::shared_ptr<CubicBezierSegment> inCubic = std::dynamic_pointer_cast<CubicBezierSegment>(validSegs[i - 1]);
            std::shared_ptr<CubicBezierSegment> outCubic = std::dynamic_pointer_cast<CubicBezierSegment>(validSegs[i]);
            const Point2D center = validSegs[i]->start;
            const Point2D leftTan = inCubic ? (inCubic->c2 - center) : Point2D(0.0, 0.0);
            const Point2D rightTan = outCubic ? (outCubic->c1 - center) : Point2D(0.0, 0.0);
            vertices.push_back(ShapeVertex(center, leftTan, rightTan));
        }
        std::shared_ptr<CubicBezierSegment> lastSeg = std::dynamic_pointer_cast<CubicBezierSegment>(validSegs[numSegs - 1]);
        vertices.push_back(ShapeVertex(validSegs[numSegs - 1]->end,
                                       lastSeg ? (lastSeg->c2 - lastSeg->end) : Point2D(0.0, 0.0),
                                       Point2D(0.0, 0.0)));
    }
    return vertices;
}

VectorShape
transformShape(const VectorShape& shape,
               double scaleX, double scaleY,
               double offsetX, double offsetY,
               bool flipY, double docHeight)
{
    VectorShape out;
    out.name = shape.name;
    out.closed = shape.closed;
    out.hasFillColor = shape.hasFillColor;
    out.fillColor = shape.fillColor;
    out.hasStrokeColor = shape.hasStrokeColor;
    out.strokeColor = shape.strokeColor;
    out.strokeWidth = shape.strokeWidth;
    out.opacity = shape.opacity;
    out.layerName = shape.layerName;
    out.compositingOperator = shape.compositingOperator;

    out.vertices.reserve(shape.vertices.size());
    for (std::vector<ShapeVertex>::const_iterator it = shape.vertices.begin();
         it != shape.vertices.end(); ++it) {
        double cx = it->center.x * scaleX + offsetX;
        double cy = it->center.y;
        double lx = it->leftTangent.x * scaleX;
        double ly = it->leftTangent.y;
        double rx = it->rightTangent.x * scaleX;
        double ry = it->rightTangent.y;
        if (flipY) {
            cy = (docHeight - cy) * scaleY + offsetY;
            ly = -ly * scaleY;
            ry = -ry * scaleY;
        } else {
            cy = cy * scaleY + offsetY;
            ly = ly * scaleY;
            ry = ry * scaleY;
        }
        ShapeVertex v;
        v.center = Point2D(cx, cy);
        v.leftTangent = Point2D(lx, ly);
        v.rightTangent = Point2D(rx, ry);
        out.vertices.push_back(v);
    }
    return out;
}

bool
flattenShape(const VectorShape& shape,
             std::vector<std::pair<double, double> >* out,
             int samplesPerSegment)
{
    if (!out || shape.vertices.size() < 2 || samplesPerSegment < 1) {
        return false;
    }
    const int n = (int)shape.vertices.size();
    const int segs = shape.closed ? n : (n - 1);
    out->clear();
    out->reserve((std::size_t)(segs * samplesPerSegment) + 1);
    for (int i = 0; i < segs; ++i) {
        const ShapeVertex& v0 = shape.vertices[i];
        const ShapeVertex& v1 = shape.vertices[(i + 1) % n];
        const Point2D p0 = v0.center;
        const Point2D p1 = v0.center + v0.rightTangent;   // outgoing handle
        const Point2D p2 = v1.center + v1.leftTangent;    // incoming handle
        const Point2D p3 = v1.center;
        for (int k = 0; k < samplesPerSegment; ++k) {
            const double t = (double)k / samplesPerSegment;
            const double mt = 1.0 - t;
            // De Casteljau for a cubic segment.
            const double a = mt * mt * mt;
            const double b = 3.0 * mt * mt * t;
            const double c = 3.0 * mt * t * t;
            const double d = t * t * t;
            out->push_back(std::make_pair(a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                                          a * p0.y + b * p1.y + c * p2.y + d * p3.y));
        }
    }
    out->push_back(std::make_pair(shape.vertices[shape.closed ? 0 : (n - 1)].center.x,
                                  shape.vertices[shape.closed ? 0 : (n - 1)].center.y));
    return out->size() >= 3;
}

} // namespace VectorMask

NATRON_NAMESPACE_EXIT
