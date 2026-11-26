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

#include "MaskMeshBuilder.h"

#include <cmath>
#include <list>
#include <vector>

#include "Engine/Bezier.h"
#include "Global/Enums.h"
#include "libtess.h"

NATRON_NAMESPACE_ENTER

namespace MaskMesh {

// ---------------------------------------------------------------------------
// libtess tessellation of closed contours in the XY plane -> plain triangles.
// Same vendored libtess the Roto fill uses, with normal (0,0,1) since every
// contour lies in the XY plane.
// ---------------------------------------------------------------------------

namespace {

struct TessData
{
    std::vector<std::vector<ParametricPoint> >* out;
    std::vector<ParametricPoint> current;   // primitive being collected
    unsigned int mode;
    std::list<ParametricPoint> allocatedIntersections;
    unsigned int error;
};

void
tess_begin(unsigned int which, void* polygonData)
{
    TessData* d = (TessData*)polygonData;
    d->mode = which;
    d->current.clear();
}

void
tess_vertex(void* data, void* polygonData)
{
    TessData* d = (TessData*)polygonData;
    const ParametricPoint* p = (const ParametricPoint*)data;
    d->current.push_back(*p);
}

void
tess_emit_current(TessData* d)
{
    const std::vector<ParametricPoint>& v = d->current;
    switch (d->mode) {
    case LIBTESS_GL_TRIANGLES:
        for (std::size_t i = 0; i + 2 < v.size(); i += 3) {
            std::vector<ParametricPoint> tri;
            tri.push_back(v[i]); tri.push_back(v[i + 1]); tri.push_back(v[i + 2]);
            d->out->push_back(tri);
        }
        break;
    case LIBTESS_GL_TRIANGLE_FAN:
        for (std::size_t i = 2; i < v.size(); ++i) {
            std::vector<ParametricPoint> tri;
            tri.push_back(v[0]); tri.push_back(v[i - 1]); tri.push_back(v[i]);
            d->out->push_back(tri);
        }
        break;
    case LIBTESS_GL_TRIANGLE_STRIP:
        for (std::size_t i = 2; i < v.size(); ++i) {
            std::vector<ParametricPoint> tri;
            if (i % 2 == 0) {
                tri.push_back(v[i - 2]); tri.push_back(v[i - 1]); tri.push_back(v[i]);
            } else {
                tri.push_back(v[i - 1]); tri.push_back(v[i - 2]); tri.push_back(v[i]);
            }
            d->out->push_back(tri);
        }
        break;
    default:
        break;
    }
    d->current.clear();
}

void
tess_end(void* polygonData)
{
    TessData* d = (TessData*)polygonData;
    tess_emit_current(d);
}

void
tess_error(unsigned int error, void* polygonData)
{
    TessData* d = (TessData*)polygonData;
    d->error = error;
}

void
tess_combine(double coords[3], void* /*data*/[4], double /*w*/[4], void** dataOut, void* polygonData)
{
    TessData* d = (TessData*)polygonData;
    d->allocatedIntersections.push_back(ParametricPoint());
    d->allocatedIntersections.back().x = coords[0];
    d->allocatedIntersections.back().y = coords[1];
    d->allocatedIntersections.back().t = 0.0;
    *dataOut = &d->allocatedIntersections.back();
}

/**
 * @brief Orient a closed contour CCW (shoelace signed area: > 0 is CCW in a
 * bottom-left y-up space). Contours may come in either orientation and
 * winding is the source of truth for normals.
 */
std::vector<ParametricPoint>
orientedCCW(const std::vector<std::pair<double, double> >& contour)
{
    double area = 0.0;
    for (std::size_t i = 0; i < contour.size(); ++i) {
        const std::pair<double, double>& a = contour[i];
        const std::pair<double, double>& b = contour[(i + 1) % contour.size()];
        area += a.first * b.second - b.first * a.second;
    }

    std::vector<ParametricPoint> oriented(contour.size());
    for (std::size_t i = 0; i < contour.size(); ++i) {
        oriented[i].x = contour[i].first;
        oriented[i].y = contour[i].second;
        oriented[i].t = 0.0;
    }
    if (area < 0.0) {
        std::reverse(oriented.begin(), oriented.end());
    }
    return oriented;
}

void
tessSetup(libtess_GLUtesselator* tess, std::vector<std::vector<ParametricPoint> >* triangles,
          TessData* tessData)
{
    tessData->out = triangles;
    tessData->error = 0;

    libtess_gluTessCallback(tess, LIBTESS_GLU_TESS_BEGIN_DATA, (void (*)())tess_begin);
    libtess_gluTessCallback(tess, LIBTESS_GLU_TESS_VERTEX_DATA, (void (*)())tess_vertex);
    libtess_gluTessCallback(tess, LIBTESS_GLU_TESS_END_DATA, (void (*)())tess_end);
    libtess_gluTessCallback(tess, LIBTESS_GLU_TESS_ERROR_DATA, (void (*)())tess_error);
    libtess_gluTessCallback(tess, LIBTESS_GLU_TESS_COMBINE_DATA, (void (*)())tess_combine);

    // All contours lie in the XY plane: fix the normal per the libtess README.
    libtess_gluTessNormal(tess, 0, 0, 1);
    libtess_gluTessBeginPolygon(tess, (void*)tessData);
}

void
tessAddContour(libtess_GLUtesselator* tess, std::vector<ParametricPoint>& oriented)
{
    libtess_gluTessBeginContour(tess);
    for (std::size_t i = 0; i < oriented.size(); ++i) {
        double coords[3] = { oriented[i].x, oriented[i].y, 0.0 };
        libtess_gluTessVertex(tess, coords, &oriented[i]);
    }
    libtess_gluTessEndContour(tess);
}

/**
 * @brief Tessellate several closed contours as ONE polygon with the nonzero
 * winding rule, every contour forced CCW: overlapping shapes union instead of
 * stacking coincident coplanar triangles (which z-fights in the viewport and
 * double-hits in the raytracer). This matches the mask semantics, where each
 * shape is painted over the previous ones.
 */
void
tessellateContoursUnion(const std::vector<std::vector<std::pair<double, double> > >& contours,
                        const std::vector<bool>& subtract,
                        std::vector<std::vector<ParametricPoint> >* triangles)
{
    std::vector<std::vector<ParametricPoint> > oriented;
    oriented.reserve(contours.size());
    int totalPts = 0;
    for (std::size_t i = 0; i < contours.size(); ++i) {
        if (contours[i].size() < 3) {
            continue;
        }
        // CCW = adds coverage, CW = subtracts it (nonzero winding rule), which
        // is how a minus-operator shape punches a hole in the union.
        std::vector<ParametricPoint> c = orientedCCW(contours[i]);
        if (i < subtract.size() && subtract[i]) {
            std::reverse(c.begin(), c.end());
        }
        oriented.push_back(c);
        totalPts += (int)oriented.back().size();
    }
    if (oriented.empty()) {
        return;
    }

    libtess_GLUtesselator* tess = libtess_gluNewTess();
    libtess_gluTessProperty(tess, LIBTESS_GLU_TESS_WINDING_RULE, LIBTESS_GLU_TESS_WINDING_NONZERO);
    TessData tessData;
    tessSetup(tess, triangles, &tessData);
    for (std::size_t i = 0; i < oriented.size(); ++i) {
        tessAddContour(tess, oriented[i]);
    }
    libtess_gluTessEndPolygon(tess);
    libtess_gluDeleteTess(tess);
}

/// Builder for the MeshData payload: same emission contract as GeoBuilder's
/// (CCW-from-outside winding, per-corner UVs, wireframe edges).
struct MeshBuilder
{
    MeshData& m;

    explicit MeshBuilder(MeshData& mesh) : m(mesh) {}

    int vert(float x, float y, float z, float u, float v)
    {
        m.vertices.push_back(x); m.vertices.push_back(y); m.vertices.push_back(z);
        m.uvs.push_back(u); m.uvs.push_back(v);
        return (int)(m.vertices.size() / 3) - 1;
    }

    void edge(int a, int b)
    {
        m.edgeIndices.push_back(a); m.edgeIndices.push_back(b);
    }

    void tri(int a, int b, int c)
    {
        m.faceIndices.push_back(a); m.faceIndices.push_back(b); m.faceIndices.push_back(c);
        m.faceCounts.push_back(3);
        edge(a, b); edge(b, c); edge(c, a);
    }

    void quad(int a, int b, int c, int d)
    {
        m.faceIndices.push_back(a); m.faceIndices.push_back(b); m.faceIndices.push_back(c); m.faceIndices.push_back(d);
        m.faceCounts.push_back(4);
        edge(a, b); edge(b, c); edge(c, d); edge(d, a);
    }
};

/**
 * @brief Emit fill triangles (CCW mask-space) at z, optionally reversed
 * (bottom cap). Zero-area faces are skipped: their normal normalizes 0/0 and
 * poisons the shading with NaNs in the viewport and the raytracer.
 */
void
emitFillTriangles(const std::vector<std::vector<ParametricPoint> >& tris,
                  float z, bool reverseWinding,
                  double s, double cx, double cy,
                  double invFormatW, double invFormatH, MeshBuilder& b)
{
    for (std::size_t t = 0; t < tris.size(); ++t) {
        const std::vector<ParametricPoint>& tri = tris[t];
        if (tri.size() != 3) {
            continue;
        }
        const double area2 = (tri[1].x - tri[0].x) * (tri[2].y - tri[0].y)
                             - (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y);
        // 1e-6 px^2: libtess seam vertices at contour intersections collapse
        // to identical float32 positions, whose normal normalizes 0/0.
        if (std::fabs(area2) < 1e-6) {
            continue;
        }
        int idx[3];
        for (int k = 0; k < 3; ++k) {
            const double wx = (tri[k].x - cx) * s;
            const double wy = (tri[k].y - cy) * s;
            idx[k] = b.vert((float)wx, (float)wy, z,
                            (float)(tri[k].x * invFormatW), (float)(tri[k].y * invFormatH));
        }
        if (reverseWinding) {
            b.tri(idx[2], idx[1], idx[0]);
        } else {
            b.tri(idx[0], idx[1], idx[2]);
        }
    }
}

/**
 * @brief Signed area of a closed ring (points without the closure
 * duplicate): > 0 is CCW in a bottom-left y-up space.
 */
double
contourSignedArea(const std::vector<std::pair<double, double> >& contour)
{
    double area = 0.0;
    const std::size_t n = contour.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& a = contour[i];
        const std::pair<double, double>& b = contour[(i + 1) % n];
        area += a.first * b.second - b.first * a.second;
    }
    return area * 0.5;
}

/// Reverse the point order in place when the ring's orientation differs from
/// the requested one (CCW adds coverage / walls face outward, CW subtracts /
/// walls face into the hole).
void
orientContour(std::vector<std::pair<double, double> >* contour, bool ccw)
{
    const bool isCCW = contourSignedArea(*contour) > 0.0;
    if (isCCW != ccw) {
        std::reverse(contour->begin(), contour->end());
    }
}

/// Bounding box of a closed ring.
void
contourBBox(const std::vector<std::pair<double, double> >& contour,
            double* x0, double* y0, double* x1, double* y1)
{
    *x0 = *y0 = 1e300;
    *x1 = *y1 = -1e300;
    for (std::size_t i = 0; i < contour.size(); ++i) {
        const std::pair<double, double>& p = contour[i];
        if (p.first < *x0) { *x0 = p.first; }
        if (p.first > *x1) { *x1 = p.first; }
        if (p.second < *y0) { *y0 = p.second; }
        if (p.second > *y1) { *y1 = p.second; }
    }
}

/// Crossing-count point-in-ring test.
bool
pointInContour(double px, double py, const std::vector<std::pair<double, double> >& contour)
{
    bool inside = false;
    const std::size_t n = contour.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double yi = contour[i].second, yj = contour[j].second;
        if ((yi > py) != (yj > py)) {
            const double xInt = contour[j].first
                                + (py - yj) * (contour[i].first - contour[j].first) / (yi - yj);
            if (xInt > px) {
                inside = !inside;
            }
        }
    }
    return inside;
}

/// Resample a closed ring to uniform arc-length spacing (used before the
/// bevel offset: a traced ring carries pixel-scale noise whose per-vertex
/// normals scatter under a large offset).
bool
resampleRing(const std::vector<std::pair<double, double> >& ring, double spacing,
             std::vector<std::pair<double, double> >* out)
{
    const std::size_t n = ring.size();
    if (n < 3 || spacing <= 0.0) {
        return false;
    }
    double perim = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& a = ring[i];
        const std::pair<double, double>& b = ring[(i + 1) % n];
        perim += std::sqrt((b.first - a.first) * (b.first - a.first)
                           + (b.second - a.second) * (b.second - a.second));
    }
    if (perim <= 0.0) {
        return false;
    }
    const std::size_t count = std::max<std::size_t>(8, (std::size_t)std::ceil(perim / spacing));
    const double step = perim / count;

    out->clear();
    out->reserve(count + 1);
    double remain = step;
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& a = ring[i];
        const std::pair<double, double>& b = ring[(i + 1) % n];
        const double elen = std::sqrt((b.first - a.first) * (b.first - a.first)
                                      + (b.second - a.second) * (b.second - a.second));
        double pos = 0.0;
        while (elen - pos >= remain) {
            pos += remain;
            const double t = pos / elen;
            out->push_back(std::make_pair(a.first + (b.first - a.first) * t,
                                          a.second + (b.second - a.second) * t));
            remain = step;
        }
        remain -= (elen - pos);
    }
    return out->size() >= 3;
}

/// Closed-ring Laplacian smoothing (rounds corners; the bevel silhouette is
/// built from the smoothed ring so all its bands stay consistent).
void
smoothRing(std::vector<std::pair<double, double> >* ring, int iterations, double lambda)
{
    const std::size_t n = ring->size();
    if (n < 3) {
        return;
    }
    std::vector<std::pair<double, double> > tmp(n);
    for (int it = 0; it < iterations; ++it) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::pair<double, double>& prev = (*ring)[(i + n - 1) % n];
            const std::pair<double, double>& cur = (*ring)[i];
            const std::pair<double, double>& next = (*ring)[(i + 1) % n];
            const double midx = 0.5 * (prev.first + next.first);
            const double midy = 0.5 * (prev.second + next.second);
            tmp[i].first = cur.first * (1.0 - lambda) + midx * lambda;
            tmp[i].second = cur.second * (1.0 - lambda) + midy * lambda;
        }
        ring->swap(tmp);
    }
}

/// Side wall band between two rings with the SAME vertex count (e.g. a ring
/// and its offset copy): quad (a0,b0,b1,a1) per edge, zA/zB world heights.
/// The normal faces outward on a CCW ring and into the hole on a CW (hole)
/// ring - in both cases towards the empty side of the wall.
void
emitBand(const std::vector<std::pair<double, double> >& ringA, float zA,
         const std::vector<std::pair<double, double> >& ringB, float zB,
         double s, double cx, double cy,
         double invFormatW, double invFormatH, MeshBuilder& b)
{
    const std::size_t n = ringA.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& a = ringA[i];
        const std::pair<double, double>& cpt = ringA[(i + 1) % n];
        const std::pair<double, double>& a2 = ringB[i];
        const std::pair<double, double>& cpt2 = ringB[(i + 1) % n];
        const int a0 = b.vert((float)((a.first - cx) * s), (float)((a.second - cy) * s), zA,
                              (float)(a.first * invFormatW), (float)(a.second * invFormatH));
        const int b0 = b.vert((float)((cpt.first - cx) * s), (float)((cpt.second - cy) * s), zA,
                              (float)(cpt.first * invFormatW), (float)(cpt.second * invFormatH));
        const int b1 = b.vert((float)((cpt2.first - cx) * s), (float)((cpt2.second - cy) * s), zB,
                              (float)(cpt2.first * invFormatW), (float)(cpt2.second * invFormatH));
        const int a1 = b.vert((float)((a2.first - cx) * s), (float)((a2.second - cy) * s), zB,
                              (float)(a2.first * invFormatW), (float)(a2.second * invFormatH));
        b.quad(a0, b0, b1, a1);
    }
}

/// Offset every ring vertex to the LEFT of the traversal direction by d
/// (canonical pixels): insets a CCW outer ring, grows a CW hole ring. Miter
/// joins are clamped to ~4x d so sharp corners bevel instead of spiking.
std::vector<std::pair<double, double> >
offsetContourLeft(const std::vector<std::pair<double, double> >& ring, double d)
{
    const std::size_t n = ring.size();
    std::vector<std::pair<double, double> > out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double>& prev = ring[(i + n - 1) % n];
        const std::pair<double, double>& cur = ring[i];
        const std::pair<double, double>& next = ring[(i + 1) % n];
        double e1x = cur.first - prev.first, e1y = cur.second - prev.second;
        double e2x = next.first - cur.first, e2y = next.second - cur.second;
        const double l1 = std::sqrt(e1x * e1x + e1y * e1y);
        const double l2 = std::sqrt(e2x * e2x + e2y * e2y);
        double mx = 0.0, my = 0.0, scale = d;
        if (l1 < 1e-9 && l2 < 1e-9) {
            mx = 0.0; my = 0.0; scale = 0.0;
        } else if (l1 < 1e-9) {
            mx = -e2y / l2; my = e2x / l2;
        } else if (l2 < 1e-9) {
            mx = -e1y / l1; my = e1x / l1;
        } else {
            e1x /= l1; e1y /= l1; e2x /= l2; e2y /= l2;
            double nx = -(e1y + e2y), ny = e1x + e2x;
            const double nl = std::sqrt(nx * nx + ny * ny);
            if (nl < 1e-9) {
                mx = -e1y; my = e1x; // 180-degree reversal
            } else {
                nx /= nl; ny /= nl;
                const double cosHalf = nx * (-e2y) + ny * e2x;
                scale = d / std::max(cosHalf, 0.25); // miter clamp
                mx = nx; my = ny;
            }
        }
        out[i].first = cur.first + mx * scale;
        out[i].second = cur.second + my * scale;
    }
    return out;
}

/**
 * @brief One family (outer contour + its hole contours) -> extruded solid:
 * caps tessellated as ONE polygon with the nonzero winding rule (the CW
 * holes punch through) and side walls along every contour. A bevel > 0
 * chamfers the edges: the caps keep the family's silhouette, the side walls
 * inset by the bevel over the middle band, 45-degree skirts join them, and
 * the hole edges countersink (holes grow by the bevel at the caps).
 */
void
emitExtrudedFamily(const std::vector<std::pair<double, double> >& outerIn,
                   const std::vector<std::vector<std::pair<double, double> > >& holesIn,
                   double s, double cx, double cy, double depth, double bevel,
                   double invFormatW, double invFormatH, MeshBuilder& b)
{
    // Orient for the walls and the tessellation: outer CCW, holes CW.
    std::vector<std::pair<double, double> > outer = outerIn;
    if (outer.size() < 3) {
        return;
    }
    orientContour(&outer, true);

    std::vector<std::vector<std::pair<double, double> > > contours;
    std::vector<bool> subtract;
    contours.push_back(outer);
    subtract.push_back(false);
    for (std::size_t i = 0; i < holesIn.size(); ++i) {
        std::vector<std::pair<double, double> > hole = holesIn[i];
        if (hole.size() < 3) {
            continue;
        }
        orientContour(&hole, false);
        contours.push_back(hole);
        subtract.push_back(true);
    }

    // Bevel (world units, like depth) in canonical pixels; clamped to keep a
    // middle wall band. All offsets are per-vertex LEFT offsets: insets the
    // CCW outer ring, grows the CW hole rings.
    const double bev = (depth > 0.0) ? std::min(bevel, 0.499 * depth) : 0.0;
    const bool hasBevel = bev > 0.0;
    const double dPx = hasBevel ? bev / s : 0.0;

    if (hasBevel) {
        // Resample + smooth the rings at the bevel's own scale: a traced ring
        // carries pixel-scale noise whose per-vertex normals scatter under a
        // large offset (the bevel mangled thin features). Resampling +
        // Laplacian smoothing gives coherent normals, rounds the silhouette
        // (the smoothing the bevel needs) and cuts the ring to a fraction of
        // its size. All bands AND the caps are built from the same smoothed
        // rings so the slab stays consistent.
        const double spacing = std::min(std::max(dPx, 2.0), 30.0);
        for (std::size_t i = 0; i < contours.size(); ++i) {
            std::vector<std::pair<double, double> > r;
            if (resampleRing(contours[i], spacing, &r)) {
                smoothRing(&r, 6, 0.5);
                contours[i].swap(r);
            }
        }
    }

    // Cap tessellation: original outer silhouette, holes outset by the
    // bevel at the faces (countersink). At bevel 0 this is the plain family
    // polygon.
    std::vector<std::vector<std::pair<double, double> > > capContours;
    std::vector<bool> capSubtract;
    capContours.push_back(outer);
    capSubtract.push_back(false);
    for (std::size_t i = 1; i < contours.size(); ++i) {
        capContours.push_back(hasBevel ? offsetContourLeft(contours[i], dPx) : contours[i]);
        capSubtract.push_back(true);
    }

    std::vector<std::vector<ParametricPoint> > tris;
    tessellateContoursUnion(capContours, capSubtract, &tris);
    if (tris.empty()) {
        return;
    }

    // Bottom cap (z=0, facing -Z): reversed winding per triangle.
    emitFillTriangles(tris, 0.0f, true, s, cx, cy, invFormatW, invFormatH, b);

    if (hasBevel) {
        // Skirt: original silhouette at z=0 sloping to the inset wall at
        // z=bev; holes outset at z=0 sloping to their original edge.
        const std::vector<std::pair<double, double> > outerInset = offsetContourLeft(outer, dPx);
        emitBand(outer, 0.0f, outerInset, (float)bev, s, cx, cy, invFormatW, invFormatH, b);
        for (std::size_t i = 1; i < contours.size(); ++i) {
            emitBand(capContours[i], 0.0f, contours[i], (float)bev, s, cx, cy, invFormatW, invFormatH, b);
        }
        // Middle wall band: inset outer / original holes.
        emitBand(outerInset, (float)bev, outerInset, (float)(depth - bev), s, cx, cy, invFormatW, invFormatH, b);
        for (std::size_t i = 1; i < contours.size(); ++i) {
            emitBand(contours[i], (float)bev, contours[i], (float)(depth - bev), s, cx, cy, invFormatW, invFormatH, b);
        }
        // Top skirt: mirror of the bottom one.
        emitBand(outerInset, (float)(depth - bev), outer, (float)depth, s, cx, cy, invFormatW, invFormatH, b);
        for (std::size_t i = 1; i < contours.size(); ++i) {
            emitBand(contours[i], (float)(depth - bev), capContours[i], (float)depth, s, cx, cy, invFormatW, invFormatH, b);
        }
    } else {
        // Straight walls along the outer and every hole.
        emitBand(outer, 0.0f, outer, (float)depth, s, cx, cy, invFormatW, invFormatH, b);
        for (std::size_t i = 1; i < contours.size(); ++i) {
            emitBand(contours[i], 0.0f, contours[i], (float)depth, s, cx, cy, invFormatW, invFormatH, b);
        }
    }

    // Top cap (z=depth, facing +Z): same winding as the tessellation.
    emitFillTriangles(tris, (float)depth, false, s, cx, cy, invFormatW, invFormatH, b);
}

} // anonymous namespace

bool
isSubtractiveOperator(int op)
{
    return op == (int)eMergeMinus || op == (int)eMergeXOR;
}

bool
cleanContour(std::vector<std::pair<double, double> >* contour)
{
    const double kDupeEps = 0.02;
    if (!contour || contour->empty()) {
        return false;
    }
    std::vector<std::pair<double, double> > cleaned;
    cleaned.reserve(contour->size());
    for (std::size_t i = 0; i < contour->size(); ++i) {
        const std::pair<double, double>& p = (*contour)[i];
        if (!cleaned.empty()) {
            const double dx = p.first - cleaned.back().first;
            const double dy = p.second - cleaned.back().second;
            if (dx * dx + dy * dy < kDupeEps * kDupeEps) {
                continue;
            }
        }
        cleaned.push_back(p);
    }
    while (cleaned.size() > 1) {
        const double dx = cleaned.front().first - cleaned.back().first;
        const double dy = cleaned.front().second - cleaned.back().second;
        if (dx * dx + dy * dy < kDupeEps * kDupeEps) {
            cleaned.pop_back();
        } else {
            break;
        }
    }
    contour->swap(cleaned);
    return contour->size() >= 3;
}

MeshDataPtr
buildMesh(const std::vector<Contour>& contoursIn,
          double s, double cx, double cy,
          double formatW, double formatH,
          double depth, double bevel)
{
    const double invFormatW = 1.0 / (formatW != 0.0 ? formatW : 1.0);
    const double invFormatH = 1.0 / (formatH != 0.0 ? formatH : 1.0);

    MeshDataPtr mesh = std::make_shared<MeshData>();
    MeshBuilder b(*mesh);

    if (depth <= 0.0) {
        // Flat fill: union contours in ONE tessellation so overlapping shapes
        // don't stack coincident coplanar triangles - but chunked: libtess
        // degrades (and can crash) on very large single polygons (a photo
        // trace produces thousands of blobs), so the contours are unioned in
        // batches of at most kUnionBatch contours and the batch meshes are
        // concatenated. Overlapping shapes split across batches can stack;
        // traced blobs rarely overlap, and families (parent + holes) are
        // kept contiguous by the producers' document order.
        static const std::size_t kUnionBatch = 128;
        std::vector<std::vector<std::pair<double, double> > > batch;
        std::vector<bool> batchSubtract;
        batch.reserve(kUnionBatch);
        batchSubtract.reserve(kUnionBatch);
        auto flushBatch = [&]() {
            if (batch.empty()) {
                return;
            }
            std::vector<std::vector<ParametricPoint> > tris;
            tessellateContoursUnion(batch, batchSubtract, &tris);
            emitFillTriangles(tris, 0.0f, false, s, cx, cy, invFormatW, invFormatH, b);
            batch.clear();
            batchSubtract.clear();
        };

        for (std::size_t i = 0; i < contoursIn.size(); ++i) {
            std::vector<std::pair<double, double> > c = contoursIn[i].points;
            if (!cleanContour(&c)) {
                continue;
            }
            batch.push_back(c);
            batchSubtract.push_back(isSubtractiveOperator(contoursIn[i].op));
            if (batch.size() >= kUnionBatch) {
                flushBatch();
            }
        }
        flushBatch();
    } else {
        // Extruded: each additive contour anchors a family; a subtractive
        // contour joins the smallest family whose outer contains it (its
        // hole). Families are extruded independently, so overlapping
        // additive shapes keep their own slabs and walls; an island inside a
        // hole arrives as its own additive family afterwards (document
        // order) and stacks inside the hole, and a subtractive contour with
        // no containing outer is skipped (a hole with no material around it
        // cannot be extruded).
        struct Family
        {
            std::vector<std::pair<double, double> > outer;
            std::vector<std::vector<std::pair<double, double> > > holes;
            double x0, y0, x1, y1; // outer bounding box
            double bboxArea;
        };
        std::vector<Family> families;
        for (std::size_t i = 0; i < contoursIn.size(); ++i) {
            std::vector<std::pair<double, double> > c = contoursIn[i].points;
            if (!cleanContour(&c)) {
                continue;
            }
            if (!isSubtractiveOperator(contoursIn[i].op)) {
                Family fam;
                fam.outer.swap(c);
                contourBBox(fam.outer, &fam.x0, &fam.y0, &fam.x1, &fam.y1);
                fam.bboxArea = (fam.x1 - fam.x0) * (fam.y1 - fam.y0);
                families.push_back(fam);
                continue;
            }
            // Smallest containing outer wins (nesting: an outer inside an
            // outer). A few probe points, so a hole grazing the outer's
            // edge still attaches.
            int best = -1;
            double bestArea = 0.0;
            for (std::size_t f = 0; f < families.size(); ++f) {
                const Family& fam = families[f];
                const std::size_t n = c.size();
                const std::size_t probes = std::min<std::size_t>(n, 16);
                bool contained = false;
                for (std::size_t p = 0; p < probes; ++p) {
                    const std::pair<double, double>& q = c[(p * n) / probes];
                    if (q.first < fam.x0 || q.first > fam.x1
                        || q.second < fam.y0 || q.second > fam.y1) {
                        continue;
                    }
                    if (pointInContour(q.first, q.second, fam.outer)) {
                        contained = true;
                        break;
                    }
                }
                if (!contained) {
                    continue;
                }
                if (best < 0 || fam.bboxArea < bestArea) {
                    best = (int)f;
                    bestArea = fam.bboxArea;
                }
            }
            if (best >= 0) {
                families[best].holes.push_back(c);
            }
        }
        for (std::size_t f = 0; f < families.size(); ++f) {
            emitExtrudedFamily(families[f].outer, families[f].holes,
                               s, cx, cy, depth, bevel, invFormatW, invFormatH, b);
        }
    }

    mesh->numVertices = mesh->vertices.size() / 3;
    mesh->numFaces = mesh->faceCounts.size();
    mesh->texCoordComponents = 2;
    mesh->hasUVs = !mesh->uvs.empty();
    return mesh;
}

} // namespace MaskMesh

NATRON_NAMESPACE_EXIT
