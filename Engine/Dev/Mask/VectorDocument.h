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

#ifndef NATRON_ENGINE_DEV_MASK_VECTORDOCUMENT_H
#define NATRON_ENGINE_DEV_MASK_VECTORDOCUMENT_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Data model + pure math for the vector-mask import pipeline (ReadMask node).
 *
 * A VectorDocument is the DCC-neutral result of parsing a vector file (SVG in Part 1):
 * a list of VectorShapes made of ShapeVertices (center + absolute-conversion tangents
 * stored as OFFSETS from the center, exactly like Nuke's ShapeControlPoint), plus the
 * document canvas size. The parsing produces shapes in the file's top-left coordinate
 * space; transform_shape maps them to the project canvas (bottom-left, fit/offset).
 *
 * All of this is pure math with no Natron state, so the later format parsers
 * (AI/EPS/PDF, DXF/OBJ/Maya/Houdini/GeoJSON) produce the same VectorDocument.
 */
namespace VectorMask {

struct Point2D
{
    double x = 0.0;
    double y = 0.0;

    Point2D() {}
    Point2D(double xx, double yy)
        : x(xx), y(yy) {}

    Point2D operator+(const Point2D& o) const { return Point2D(x + o.x, y + o.y); }
    Point2D operator-(const Point2D& o) const { return Point2D(x - o.x, y - o.y); }
    Point2D operator*(double s) const { return Point2D(x * s, y * s); }
    Point2D operator/(double s) const
    {
        if (s == 0.0) return Point2D(0.0, 0.0);
        return Point2D(x / s, y / s);
    }

    double length() const { return std::sqrt(x * x + y * y); }
    double distanceTo(const Point2D& o) const
    {
        const double dx = x - o.x;
        const double dy = y - o.y;
        return std::sqrt(dx * dx + dy * dy);
    }
};

struct Color
{
    double r = 1.0, g = 1.0, b = 1.0, a = 1.0;

    Color() {}
    Color(double rr, double gg, double bb, double aa)
        : r(rr), g(gg), b(bb), a(aa) {}

    /**
     * @brief Parses #RGB / #RGBA / #RRGGBB / #RRGGBBAA hex strings. Returns a null
     * optional as an unset color when the string is not valid hex.
     */
    static bool fromHex(const std::string& hexStr, Color* out, double defaultAlpha = 1.0);

    /** Standard CMYK -> RGB conversion, clamped. */
    static Color fromCmyk(double c, double m, double y, double k, double a = 1.0);
};

/**
 * @brief Named color table (SVG color keywords + none/transparent), case-insensitive.
 * Returns false when the name is unknown.
 */
bool namedColor(const std::string& nameLower, Color* out);

/**
 * @brief Sanitizes a name for the Roto item script-name rules: ASCII letters, digits
 * and underscores only, no leading digit. Non-compliant characters become '_'
 * (consecutive collapsed, leading/trailing stripped); empty results take the default.
 */
std::string sanitizeName(const std::string& name, const std::string& defaultName = "item");

/** A vertex of a shape: center position, tangents stored as OFFSETS relative to center. */
struct ShapeVertex
{
    Point2D center;
    Point2D leftTangent;   //< incoming handle offset (Nuke leftTangent / Bezier P2)
    Point2D rightTangent;  //< outgoing handle offset (Nuke rightTangent / Bezier P1)

    ShapeVertex()
        : center(0.0, 0.0) {}
    explicit ShapeVertex(const Point2D& c)
        : center(c) {}
    ShapeVertex(const Point2D& c, const Point2D& l, const Point2D& r)
        : center(c), leftTangent(l), rightTangent(r) {}
};

struct VectorShape
{
    std::string name = "Shape";
    std::vector<ShapeVertex> vertices;
    bool closed = true;
    bool hasFillColor = false;      //< fill_color optional in vectorNuke
    Color fillColor;
    bool hasStrokeColor = false;
    Color strokeColor;
    double strokeWidth = 1.0;
    double opacity = 1.0;
    std::string layerName = "Layer1";
    int compositingOperator = -1;   //< -1: leave the node default; else a Natron::MergingFunctionEnum
                                    //< (e.g. eMergeMinus/eMergeXOR for traced holes)

    void bounds(double* minX, double* minY, double* maxX, double* maxY) const;
};

struct VectorDocument
{
    double width = 1920.0;
    double height = 1080.0;
    std::vector<VectorShape> shapes;
    std::string filePath;
    std::string sourceFormat;

    void bounds(double* minX, double* minY, double* maxX, double* maxY) const;
};

/**
 * @brief 2D affine matrix [a c tx; b d ty; 0 0 1] with SVG transform-attribute parsing.
 */
struct Matrix2D
{
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;

    Matrix2D() {}
    Matrix2D(double pa, double pb, double pc, double pd, double ptx, double pty)
        : a(pa), b(pb), c(pc), d(pd), tx(ptx), ty(pty) {}

    static Matrix2D identity() { return Matrix2D(1.0, 0.0, 0.0, 1.0, 0.0, 0.0); }
    static Matrix2D translate(double txx, double tyy = 0.0) { return Matrix2D(1.0, 0.0, 0.0, 1.0, txx, tyy); }
    static Matrix2D scale(double sx, double sy) { return Matrix2D(sx, 0.0, 0.0, sy, 0.0, 0.0); }
    static Matrix2D rotateRadians(double radians, double cx = 0.0, double cy = 0.0);
    static Matrix2D skewXRadians(double radians) { return Matrix2D(1.0, 0.0, std::tan(radians), 1.0, 0.0, 0.0); }
    static Matrix2D skewYRadians(double radians) { return Matrix2D(1.0, std::tan(radians), 0.0, 1.0, 0.0, 0.0); }

    Matrix2D multiplied(const Matrix2D& other) const
    {
        Matrix2D r;
        r.a = a * other.a + c * other.b;
        r.b = b * other.a + d * other.b;
        r.c = a * other.c + c * other.d;
        r.d = b * other.c + d * other.d;
        r.tx = a * other.tx + c * other.ty + tx;
        r.ty = b * other.tx + d * other.ty + ty;
        return r;
    }

    Point2D transformPoint(const Point2D& pt) const
    {
        return Point2D(a * pt.x + c * pt.y + tx, b * pt.x + d * pt.y + ty);
    }

    /** Vectors (tangent handles) are transformed without the translation. */
    Point2D transformVector(const Point2D& v) const
    {
        return Point2D(a * v.x + c * v.y, b * v.x + d * v.y);
    }

    /**
     * @brief Parses an SVG transform attribute: whitespace/comma separated chain of
     * matrix(a,b,c,d,e,f), translate(x[,y]), scale(x[,y]), rotate(deg[,cx,cy]),
     * skewX(deg), skewY(deg), applied left to right.
     */
    static Matrix2D fromSvgTransform(const QString& transformStr);
};

struct Segment
{
    Point2D start, end;
    virtual ~Segment() {}
    Segment(const Point2D& s, const Point2D& e)
        : start(s), end(e) {}
};

struct LineSegment
    : Segment
{
    LineSegment(const Point2D& s, const Point2D& e)
        : Segment(s, e) {}
};

struct CubicBezierSegment
    : Segment
{
    Point2D c1, c2;
    CubicBezierSegment(const Point2D& s, const Point2D& p1, const Point2D& p2, const Point2D& e)
        : Segment(s, e), c1(p1), c2(p2) {}
};

typedef std::shared_ptr<Segment> SegmentPtr;

/** Quadratic (start, qCtrl, end) -> exact cubic. */
CubicBezierSegment quadToCubic(const Point2D& start, const Point2D& qCtrl, const Point2D& end);

/**
 * @brief SVG elliptical arc -> exact cubic segments per W3C SVG 1.1 Appendix F.6,
 * split into segments of at most 90 degrees.
 */
std::vector<SegmentPtr> arcToCubicBeziers(const Point2D& p1, double rx, double ry, double phiDeg,
                                          bool largeArc, bool sweep, const Point2D& p2);

/**
 * @brief Assembles an ordered list of connected segments into shape vertices with
 * tangent offsets (the Nuke ShapeControlPoint convention). Closed paths get one vertex
 * per segment (vertex i = segment i start); open paths get N+1 vertices for N segments.
 */
std::vector<ShapeVertex> segmentsToVertices(const std::vector<SegmentPtr>& segments, bool closed);

/**
 * @brief Applies the fit/offset transform and the top-left -> bottom-left Y flip to a
 * shape (tangent offsets flip sign in Y and scale, per the vectorNuke rule).
 */
VectorShape transformShape(const VectorShape& shape,
                           double scaleX, double scaleY,
                           double offsetX, double offsetY,
                           bool flipY, double docHeight);

/**
 * @brief Flatten a shape's cubic Bezier segments (center + tangent-OFFSET
 * vertices: control points are center+rightTangent / next.center+leftTangent)
 * into a polyline. Each segment is sampled at `samplesPerSegment` steps.
 * Closed shapes append the wrap-around last->first segment; open shapes do
 * not. Returns false if fewer than 3 points result (degenerate shape).
 */
bool flattenShape(const VectorShape& shape,
                  std::vector<std::pair<double, double> >* out,
                  int samplesPerSegment = 16);

} // namespace VectorMask

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_VECTORDOCUMENT_H
