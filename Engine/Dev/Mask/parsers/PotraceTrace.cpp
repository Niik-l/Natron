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

// See PotraceTrace.h for the module overview.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "potracelib.h"
#include "bitmap.h"

#include "Dev/Mask/parsers/PotraceTrace.h"

NATRON_NAMESPACE_ENTER

namespace VectorMask {

namespace {

std::size_t
wordCount(int w)
{
    return (std::size_t)((w + (int)sizeof(potrace_word) * 8 - 1) / ((int)sizeof(potrace_word) * 8));
}

/**
 * @brief Build a potrace bitmap from a binary grid (row-major, row 0 = image
 * top, 1 = foreground). The caller frees map.
 */
bool
buildBitmap(int w,
            int h,
            const std::vector<unsigned char>& bits,
            potrace_bitmap_t* bm)
{
    bm->w = w;
    bm->h = h;
    bm->dy = (int)wordCount(w);
    bm->map = (potrace_word*)calloc(bm->dy * (std::size_t)h, sizeof(potrace_word));
    if (!bm->map) {
        return false;
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (bits[(std::size_t)y * w + x]) {
                BM_PUT(bm, x, y, 1);
            }
        }
    }
    return true;
}

/**
 * @brief Convert one potrace path into a VectorShape. potrace curves are
 * closed; segment 0 starts at the endpoint of the last segment (that is where
 * the SVG backend's moveto points). potrace coordinates are y-up (origin at
 * the bitmap's bottom-left); the document space is y-down like the SVG
 * parser's, so y is converted with docHeight - y and tangent dy offsets are
 * negated. sign < 0 marks a hole contour: it punches with the configured
 * operator (sign-based so nested islands inside holes still fill normally).
 */
VectorShape
pathToShape(const potrace_path_t* path,
            bool isHole,
            int holeOperator,
            const std::string& name,
            const std::string& layerName,
            double docHeight)
{
    VectorShape shape;
    shape.name = name;
    shape.closed = true;
    shape.layerName = layerName;
    if (isHole) {
        // A hole contour punches with the configured operator.
        shape.compositingOperator = holeOperator;
    }

    const potrace_curve_t& curve = path->curve;
    const int n = curve.n;
    if (n <= 0) {
        return shape;
    }

    // Start of segment 0 = endpoint of segment n-1.
    Point2D current;
    {
        const potrace_dpoint_t* last = curve.c[n - 1];
        const potrace_dpoint_t& p = (curve.tag[n - 1] == POTRACE_CURVETO) ? last[2] : last[1];
        current = Point2D(p.x, p.y);
    }

    std::vector<Point2D> rightTans(n, Point2D(0.0, 0.0));
    std::vector<Point2D> leftTans(n, Point2D(0.0, 0.0));
    std::vector<Point2D> centers(n, Point2D(0.0, 0.0));

    for (int i = 0; i < n; ++i) {
        centers[i] = current;
        const int tag = curve.tag[i];
        const potrace_dpoint_t* c = curve.c[i];
        Point2D end;
        if (tag == POTRACE_CURVETO) {
            rightTans[i] = Point2D(c[0].x - current.x, c[0].y - current.y);
            const Point2D next(c[2].x, c[2].y);
            leftTans[(i + 1) % n] = Point2D(c[1].x - next.x, c[1].y - next.y);
            end = next;
        } else { // POTRACE_CORNER: straight run, c[0] unused, c[1] = endpoint
            end = Point2D(c[1].x, c[1].y);
        }
        current = end;
    }

    shape.vertices.reserve(n);
    for (int i = 0; i < n; ++i) {
        // potrace y-up -> document y-down: flip y, negate tangent dy.
        const ShapeVertex v(Point2D(centers[i].x, docHeight - centers[i].y),
                            Point2D(leftTans[i].x, -leftTans[i].y),
                            Point2D(rightTans[i].x, -rightTans[i].y));
        shape.vertices.push_back(v);
    }
    return shape;
}

void
walkPaths(const potrace_path_t* plist,
          int holeOperator,
          const std::string& layerName,
          double docHeight,
          VectorDocument* doc,
          int* shapeCount,
          const Color* fillColor = NULL)
{
    // The flat next-list carries EVERY path (holes included, after their
    // parent); childlist/sibling mirror the hierarchy, so recursing into it
    // as well would count each hole twice. Walk the flat list and punch by
    // sign instead.
    for (const potrace_path_t* p = plist; p; p = p->next) {
        if (p->curve.n <= 0) {
            continue;
        }
        ++(*shapeCount);
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "%d", *shapeCount);
        const std::string name = layerName + "_" + suffix;
        // potrace stores the path sign as the characters '+'/'-' (not +-1).
        VectorShape shape = pathToShape(p, p->sign == '-', holeOperator, name, layerName, docHeight);
        if (fillColor) {
            shape.hasFillColor = true;
            shape.fillColor = *fillColor;
        }
        if (shape.vertices.size() >= 3) {
            doc->shapes.push_back(shape);
        }
    }
}

/**
 * @brief One binary grid through potrace. bits: row-major, row 0 = top.
 */
bool
traceBinary(int w,
            int h,
            const std::vector<unsigned char>& bits,
            double despeckle,
            double cornerThreshold,
            bool curveOptimization,
            double optTolerance,
            int holeOperator,
            const std::string& layerName,
            VectorDocument* doc,
            int* shapeCount,
            const Color* fillColor = NULL)
{
    potrace_bitmap_t bm;
    memset(&bm, 0, sizeof(bm));
    if (!buildBitmap(w, h, bits, &bm)) {
        return false;
    }

    potrace_param_t* params = potrace_param_default();
    if (!params) {
        free(bm.map);
        return false;
    }
    params->turdsize = (int)std::max(0.0, despeckle);
    params->alphamax = std::max(0.0, std::min(1.334, cornerThreshold));
    params->opticurve = curveOptimization ? 1 : 0;
    params->opttolerance = std::max(0.0, optTolerance);

    potrace_state_t* state = potrace_trace(params, &bm);
    potrace_param_free(params);
    free(bm.map);
    if (!state || state->status != POTRACE_STATUS_OK) {
        if (state) {
            potrace_state_free(state);
        }
        return false;
    }

    walkPaths(state->plist, holeOperator, layerName, (double)h, doc, shapeCount, fillColor);
    potrace_state_free(state);
    return true;
}

/**
 * @brief k-means clustering over RGB pixels (0..1). assign[i] = cluster of
 * pixel i; centers[k*3..k*3+2] = cluster RGB. Deterministic init: centroids
 * spread over the luminance-sorted pixel list.
 */
void
kmeansRGB(int w,
          int h,
          const std::vector<float>& rgb,
          int k,
          int iterations,
          std::vector<int>* assign,
          std::vector<float>* centers)
{
    const std::size_t n = (std::size_t)w * h;
    assign->assign(n, 0);
    centers->assign((std::size_t)k * 3, 0.0f);

    // luminance per pixel for deterministic spread init
    std::vector<std::pair<float, int> > byLum;
    byLum.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float lum = 0.2126f * rgb[i * 3] + 0.7152f * rgb[i * 3 + 1] + 0.0722f * rgb[i * 3 + 2];
        byLum.push_back(std::make_pair(lum, (int)i));
    }
    std::sort(byLum.begin(), byLum.end());
    for (int c = 0; c < k; ++c) {
        const int pick = byLum[(std::size_t)((n - 1) * c / (double)std::max(1, k - 1) + 0.5)].second;
        (*centers)[(std::size_t)c * 3 + 0] = rgb[(std::size_t)pick * 3 + 0];
        (*centers)[(std::size_t)c * 3 + 1] = rgb[(std::size_t)pick * 3 + 1];
        (*centers)[(std::size_t)c * 3 + 2] = rgb[(std::size_t)pick * 3 + 2];
    }

    for (int it = 0; it < iterations; ++it) {
        // assign
        for (std::size_t i = 0; i < n; ++i) {
            int best = 0;
            float bestDist = 1e30f;
            for (int c = 0; c < k; ++c) {
                const float dr = rgb[i * 3 + 0] - (*centers)[(std::size_t)c * 3 + 0];
                const float dg = rgb[i * 3 + 1] - (*centers)[(std::size_t)c * 3 + 1];
                const float db = rgb[i * 3 + 2] - (*centers)[(std::size_t)c * 3 + 2];
                const float dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) {
                    bestDist = dist;
                    best = c;
                }
            }
            (*assign)[i] = best;
        }
        // update
        std::vector<float> sum((std::size_t)k * 3, 0.0f);
        std::vector<int> count(k, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const int c = (*assign)[i];
            sum[(std::size_t)c * 3 + 0] += rgb[i * 3 + 0];
            sum[(std::size_t)c * 3 + 1] += rgb[i * 3 + 1];
            sum[(std::size_t)c * 3 + 2] += rgb[i * 3 + 2];
            ++count[c];
        }
        for (int c = 0; c < k; ++c) {
            if (count[c] > 0) {
                (*centers)[(std::size_t)c * 3 + 0] = sum[(std::size_t)c * 3 + 0] / count[c];
                (*centers)[(std::size_t)c * 3 + 1] = sum[(std::size_t)c * 3 + 1] / count[c];
                (*centers)[(std::size_t)c * 3 + 2] = sum[(std::size_t)c * 3 + 2] / count[c];
            }
        }
    }

    // final assign with the updated centers
    for (std::size_t i = 0; i < n; ++i) {
        int best = 0;
        float bestDist = 1e30f;
        for (int c = 0; c < k; ++c) {
            const float dr = rgb[i * 3 + 0] - (*centers)[(std::size_t)c * 3 + 0];
            const float dg = rgb[i * 3 + 1] - (*centers)[(std::size_t)c * 3 + 1];
            const float db = rgb[i * 3 + 2] - (*centers)[(std::size_t)c * 3 + 2];
            const float dist = dr * dr + dg * dg + db * db;
            if (dist < bestDist) {
                bestDist = dist;
                best = c;
            }
        }
        (*assign)[i] = best;
    }
}

// ---------------------------------------------------------------------------
// Zhang-Suen thinning + centerline extraction
// ---------------------------------------------------------------------------

int
neighbors8(const std::vector<unsigned char>& bits, int w, int h, int x, int y)
{
    static const int dx[8] = { 1, 1, 1, 0, 0, -1, -1, -1 };
    static const int dy[8] = { -1, 0, 1, 1, -1, 1, 0, -1 };
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        const int nx = x + dx[i];
        const int ny = y + dy[i];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
            continue;
        }
        n += bits[(std::size_t)ny * w + nx] ? 1 : 0;
    }
    return n;
}

bool
zhangSuenThin(std::vector<unsigned char>* bits, int w, int h)
{
    // Two sub-passes per iteration; pixels pending removal are marked 2 and
    // still count as ink for the current pass (parallel-removal semantics),
    // then cleared before the next pass.
    bool changed = false;
    static const int dx[8] = { 1, 1, 1, 0, 0, -1, -1, -1 };
    static const int dy[8] = { -1, 0, 1, 1, -1, 1, 0, -1 };
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                const std::size_t idx = (std::size_t)y * w + x;
                if (!(*bits)[idx]) {
                    continue;
                }
                unsigned char nb[8];
                for (int i = 0; i < 8; ++i) {
                    nb[i] = (*bits)[(std::size_t)(y + dy[i]) * w + (x + dx[i])] ? 1 : 0;
                }
                const int B = nb[0] + nb[1] + nb[2] + nb[3] + nb[4] + nb[5] + nb[6] + nb[7];
                if (B < 2 || B > 6) {
                    continue;
                }
                unsigned char seq[9];
                for (int i = 0; i < 8; ++i) {
                    seq[i] = nb[i];
                }
                seq[8] = nb[0];
                int A = 0;
                for (int i = 0; i < 8; ++i) {
                    if (seq[i] == 0 && seq[i + 1] == 1) {
                        ++A;
                    }
                }
                if (A != 1) {
                    continue;
                }
                if (pass == 0) {
                    if (nb[0] && nb[2] && nb[4]) {
                        continue;
                    }
                    if (nb[2] && nb[4] && nb[6]) {
                        continue;
                    }
                } else {
                    if (nb[0] && nb[2] && nb[6]) {
                        continue;
                    }
                    if (nb[0] && nb[4] && nb[6]) {
                        continue;
                    }
                }
                (*bits)[idx] = 2; // pending: still ink for this pass's reads
                changed = true;
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if ((*bits)[(std::size_t)y * w + x] == 2) {
                    (*bits)[(std::size_t)y * w + x] = 0;
                }
            }
        }
    }
    return changed;
}

/**
 * @brief Ramer-Douglas-Peucker simplification on an open polyline.
 */
void
rdpSimplify(const std::vector<Point2D>& pts,
            double tol,
            std::vector<Point2D>* out)
{
    const int n = (int)pts.size();
    if (n < 3 || tol <= 0.0) {
        *out = pts;
        return;
    }
    const Point2D& a = pts.front();
    const Point2D& b = pts.back();
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    int maxIdx = -1;
    double maxDist = 0.0;
    for (int i = 1; i < n - 1; ++i) {
        double dist;
        if (len > 1e-9) {
            dist = std::fabs(dy * (pts[i].x - a.x) - dx * (pts[i].y - a.y)) / len;
        } else {
            const double ddx = pts[i].x - a.x;
            const double ddy = pts[i].y - a.y;
            dist = std::sqrt(ddx * ddx + ddy * ddy);
        }
        if (dist > maxDist) {
            maxDist = dist;
            maxIdx = i;
        }
    }
    if (maxIdx < 0 || maxDist <= tol) {
        *out = pts;
        return;
    }
    std::vector<Point2D> left(pts.begin(), pts.begin() + maxIdx + 1);
    std::vector<Point2D> right(pts.begin() + maxIdx, pts.end());
    std::vector<Point2D> leftOut, rightOut;
    rdpSimplify(left, tol, &leftOut);
    rdpSimplify(right, tol, &rightOut);
    out->assign(leftOut.begin(), leftOut.end());
    if (!rightOut.empty() && !out->empty()) {
        const Point2D& a = out->back();
        const Point2D& b = rightOut.front();
        if (a.distanceTo(b) < 1e-9) {
            out->insert(out->end(), rightOut.begin() + 1, rightOut.end());
        } else {
            out->insert(out->end(), rightOut.begin(), rightOut.end());
        }
    } else {
        out->insert(out->end(), rightOut.begin(), rightOut.end());
    }
}

} // anonymous namespace

bool
tracePlaneToDocument(int w,
                     int h,
                     const std::vector<float>& values,
                     const std::vector<float>& valuesRGB,
                     const TraceParams& params,
                     const std::string& layerName,
                     VectorDocument* doc)
{
    if (w <= 1 || h <= 1 || values.size() != (std::size_t)w * h) {
        return false;
    }
    doc->width = w;
    doc->height = h;

    std::vector<unsigned char> bits((std::size_t)w * h, 0);

    if (params.mode == eTraceModeCenterline) {
        // Threshold (centerline traces the INK: dark strokes on light plates
        // trace with invert on), thin, extract chains into open Beziers.
        std::vector<unsigned char> skel((std::size_t)w * h, 0);
        std::size_t foreground = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double v = values[(std::size_t)y * w + x];
                if (params.invert) {
                    v = 1.0 - v;
                }
                skel[(std::size_t)y * w + x] = (v > params.threshold) ? 1 : 0;
                if (skel[(std::size_t)y * w + x]) {
                    ++foreground;
                }
            }
        }
        if (!foreground) {
            return false;
        }
        for (int i = 0; i < 100; ++i) {
            if (!zhangSuenThin(&skel, w, h)) {
                break;
            }
        }

        const int dx[8] = { 1, 1, 1, 0, 0, -1, -1, -1 };
        const int dy[8] = { -1, 0, 1, 1, -1, 1, 0, -1 };
        std::vector<int> visit((std::size_t)w * h, 0);
        int shapeCount = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t idx = (std::size_t)y * w + x;
                if (!skel[idx] || visit[idx]) {
                    continue;
                }
                const int ncount = neighbors8(skel, w, h, x, y);
                if (ncount == 0) {
                    visit[idx] = 1;
                    continue;
                }
                // Endpoints (1) start chains; junction pixels (>=3) are walked
                // once per spoke. Plain 2-neighbor pixels belong to a chain
                // started elsewhere (walked from its endpoint/junction).
                if (ncount == 2) {
                    // Only start a chain on a 2-neighbor pixel if no adjacent
                    // pixel has been visited (isolated loop pixel).
                    bool anyVisitedNeighbor = false;
                    for (int i = 0; i < 8; ++i) {
                        const int tx = x + dx[i];
                        const int ty = y + dy[i];
                        if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
                            continue;
                        }
                        if (visit[(std::size_t)ty * w + tx]) {
                            anyVisitedNeighbor = true;
                            break;
                        }
                    }
                    if (anyVisitedNeighbor) {
                        continue;
                    }
                }
                std::vector<Point2D> chain;
                int cx = x, cy = y;
                chain.push_back(Point2D(cx + 0.5, cy + 0.5));
                visit[idx] = 1;
                for (int guard = 0; guard < w * h; ++guard) {
                    int nx = -1, ny = -1;
                    for (int i = 0; i < 8; ++i) {
                        const int tx = cx + dx[i];
                        const int ty = cy + dy[i];
                        if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
                            continue;
                        }
                        const std::size_t tidx = (std::size_t)ty * w + tx;
                        if (!skel[tidx] || visit[tidx]) {
                            continue;
                        }
                        nx = tx;
                        ny = ty;
                        break;
                    }
                    if (nx < 0) {
                        break;
                    }
                    const std::size_t tidx = (std::size_t)ny * w + nx;
                    visit[tidx] = 1;
                    chain.push_back(Point2D(nx + 0.5, ny + 0.5));
                    const int tn = neighbors8(skel, w, h, nx, ny);
                    cx = nx;
                    cy = ny;
                    if (tn != 2) {
                        break;
                    }
                }
                if (chain.size() < 2) {
                    continue;
                }
                std::vector<Point2D> simple;
                rdpSimplify(chain, std::max(0.1, params.simplifyTolerance), &simple);
                if (simple.size() < 2) {
                    continue;
                }
                VectorShape shape;
                ++shapeCount;
                shape.name = layerName + "_cl_" + std::to_string((long long)shapeCount);
                shape.closed = false;
                shape.layerName = layerName;
                shape.vertices.reserve(simple.size());
                for (std::size_t i = 0; i < simple.size(); ++i) {
                    Point2D leftTan(0.0, 0.0);
                    Point2D rightTan(0.0, 0.0);
                    if (i > 0 && i + 1 < simple.size()) {
                        Point2D dir(simple[i + 1].x - simple[i - 1].x,
                                    simple[i + 1].y - simple[i - 1].y);
                        const double len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                        if (len > 1e-9) {
                            dir.x /= len;
                            dir.y /= len;
                            const double dl = std::sqrt(
                                (simple[i].x - simple[i - 1].x) * (simple[i].x - simple[i - 1].x) +
                                (simple[i].y - simple[i - 1].y) * (simple[i].y - simple[i - 1].y));
                            const double dr = std::sqrt(
                                (simple[i + 1].x - simple[i].x) * (simple[i + 1].x - simple[i].x) +
                                (simple[i + 1].y - simple[i].y) * (simple[i + 1].y - simple[i].y));
                            const double scale = std::min(dl, dr) * 0.5;
                            leftTan = Point2D(-dir.x * scale, -dir.y * scale);
                            rightTan = Point2D(dir.x * scale, dir.y * scale);
                        }
                    }
                    shape.vertices.push_back(ShapeVertex(simple[i], leftTan, rightTan));
                }
                doc->shapes.push_back(shape);
            }
        }
        return !doc->shapes.empty();
    }

    if (params.mode == eTraceModeColor) {
        // k-means over the RGB pixels; each cluster traces as one stacked
        // shape (VTracer stacking: no hole geometry between clusters). Order
        // clusters by area descending so big regions sit at the bottom of the
        // tree and paint first; holes inside a cluster punch per holeOperator.
        if (valuesRGB.size() != (std::size_t)w * h * 3) {
            return false;
        }
        const int k = std::max(2, std::min(16, params.colorCount));
        std::vector<int> assign;
        std::vector<float> centers;
        kmeansRGB(w, h, valuesRGB, k, 8, &assign, &centers);

        std::vector<int> clusterArea(k, 0);
        for (std::size_t i = 0; i < assign.size(); ++i) {
            ++clusterArea[assign[i]];
        }
        std::vector<int> order(k);
        for (int c = 0; c < k; ++c) {
            order[c] = c;
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return clusterArea[a] > clusterArea[b];
        });

        int shapeCount = 0;
        for (int oi = 0; oi < k; ++oi) {
            const int c = order[oi];
            if (!clusterArea[c]) {
                continue;
            }
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    bits[(std::size_t)y * w + x] = (assign[(std::size_t)y * w + x] == c) ? 1 : 0;
                }
            }
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "%d", c + 1);
            const std::string clusterLayer = layerName + "_color" + suffix;
            Color fill(centers[(std::size_t)c * 3 + 0],
                       centers[(std::size_t)c * 3 + 1],
                       centers[(std::size_t)c * 3 + 2], 1.0);
            traceBinary(w, h, bits, params.despeckle, params.cornerThreshold,
                        params.curveOptimization, params.optTolerance,
                        params.holeOperator, clusterLayer, doc, &shapeCount, &fill);
        }
        return !doc->shapes.empty();
    }

    // bw mode: optional stacked threshold bands, cumulative like Inkscape's
    // multiple scans / VTracer stacking. N band thresholds span [Threshold
    // Low, Threshold] inclusively; band b = pixels brighter than t_b. The
    // regions are nested (a brighter band is a subset of a darker one), so the
    // darkest (largest) band is traced first and the brighter (smaller) bands
    // layer over it - shapes import in document order, which is also the paint
    // order of the RotoContext merge chain.
    const int bands = std::max(1, std::min(16, params.bands));
    const double tLow = (bands > 1)
                            ? std::min(params.thresholdLow, params.threshold)
                            : params.threshold;
    const double tStep = (bands > 1) ? (params.threshold - tLow) / (bands - 1) : 0.0;
    std::size_t anyForeground = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double v = values[(std::size_t)y * w + x];
            if (params.invert) {
                v = 1.0 - v;
            }
            if (v > tLow) {
                ++anyForeground;
            }
        }
    }
    if (!anyForeground) {
        return false;
    }

    int shapeCount = 0;
    for (int b = 0; b < bands; ++b) {
        const double tb = tLow + tStep * b;
        std::size_t count = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double v = values[(std::size_t)y * w + x];
                if (params.invert) {
                    v = 1.0 - v;
                }
                const bool on = v > tb;
                bits[(std::size_t)y * w + x] = on ? 1 : 0;
                if (on) {
                    ++count;
                }
            }
        }
        if (!count) {
            continue;
        }
        std::string bandLayer = layerName;
        if (bands > 1) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "%d", b + 1);
            bandLayer = layerName + "_band" + suffix;
        }
        traceBinary(w, h, bits, params.despeckle, params.cornerThreshold,
                    params.curveOptimization, params.optTolerance,
                    params.holeOperator, bandLayer, doc, &shapeCount);
    }
    return !doc->shapes.empty();
}

} // namespace VectorMask

NATRON_NAMESPACE_EXIT
