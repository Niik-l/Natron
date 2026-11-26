// GridWrap grid geometry - see grid.h for the semantics and where each claim comes from.
#include "grid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace gw {

namespace {

const double kThird = 1.0 / 3.0;
const double kPi = 3.14159265358979323846;

// How close a lattice fraction has to come to a grid line to be treated as being on it.
// See localAtFraction().
const double kLatticeSnap = 1e-9;

// Newton iteration budget. A cell's interior is close to affine, so a start at the nearest
// forward-mapped lattice sample converges in a handful of steps; the budget is headroom
// for the folded, near-degenerate cells a hand-dragged grid produces, and it is bounded so
// that a hopeless query (an output pixel the grid genuinely does not cover) costs a fixed
// small amount. tests/gw_grid_test.cpp measures the iterations a real render uses.
enum { kNewtonIterations = 16 };

// How many of the nearest samples a query tries before it gives up on its bucket. Bounded
// so a folded region, where one bucket can hold a lot of samples, cannot turn one output
// pixel into a hundred Newton runs.
enum { kMaxCandidates = 32 };

// A knob value that is not a number, replaced by the value the neutral lattice would have there.
// Double2D params have no range guard and a script writes what it likes; a NaN in one component of one
// point must not turn into a NaN in every box, bucket index and tap index downstream of it.
double finiteOr(double v, double fallback)
{
    return std::isfinite(v) ? v : fallback;
}

inline void bernstein(double t, double b[4])
{
    const double s = 1.0 - t;
    b[0] = s * s * s;
    b[1] = 3.0 * t * s * s;
    b[2] = 3.0 * t * t * s;
    b[3] = t * t * t;
}

inline void bernsteinDeriv(double t, double d[4])
{
    const double s = 1.0 - t;
    d[0] = -3.0 * s * s;
    d[1] = 3.0 * s * (s - 2.0 * t);
    d[2] = 3.0 * t * (2.0 * s - t);
    d[3] = 3.0 * t * t;
}

// Which cell a fractional lattice coordinate (0..1 across the whole grid) falls in, and where
// inside that cell it is. `resampleGrid` needs the cell and not just the point, because the
// handles it hands out come from that cell's corners.
struct LatticeLocal {
    int r, c;
    double u, v;
};

// A lattice fraction times a cell count is only *nearly* an integer in binary - 2/3 * 3 comes
// out as 1.9999999999999998 - so a coordinate that landed on a grid line would otherwise be
// read as a hair inside the cell, and a rebuild over the same lattice would return a hair-moving
// copy of the grid it was given. Snapping the last kLatticeSnap of the way onto the boundary is
// what lets resampleGrid() recognise a coincident point and return the grid it was given,
// control for control (the F1/F2 checks).
bool localAtFraction(const Grid& g, double fu, double fv, LatticeLocal& out)
{
    const int cr = g.cellRows(), cc = g.cellCols();
    if (cr < 1 || cc < 1) return false;

    const double x = fu * double(cc);
    const double y = fv * double(cr);

    out.c = int(std::floor(x));
    out.r = int(std::floor(y));
    if (out.c < 0) out.c = 0;
    if (out.r < 0) out.r = 0;
    if (out.c > cc - 1) out.c = cc - 1;
    if (out.r > cr - 1) out.r = cr - 1;

    out.u = x - double(out.c);
    out.v = y - double(out.r);
    if (out.u < 0.0) out.u = 0.0;
    if (out.v < 0.0) out.v = 0.0;
    if (out.u > 1.0 - kLatticeSnap) out.u = 1.0;
    if (out.v > 1.0 - kLatticeSnap) out.v = 1.0;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Clamps (D-53)
// ---------------------------------------------------------------------------

int clampPoints(int n)
{
    if (n < kMinPoints) return kMinPoints;
    if (n > kMaxPoints) return kMaxPoints;
    return n;
}

int clampRenderRes(int n)
{
    if (n < kMinRenderRes) return kMinRenderRes;
    if (n > kMaxRenderRes) return kMaxRenderRes;
    return n;
}

double clampUnit(double v)
{
    if (!std::isfinite(v)) return 0.0;
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

// ---------------------------------------------------------------------------
// The neutral lattice (D-53)
// ---------------------------------------------------------------------------

double neutralHandlesFraction(int pointsPerAxis)
{
    const int n = clampPoints(pointsPerAxis);

    // One third of one lattice spacing, stated as a fraction of the basis extent: spacing =
    // extent / (n - 1), so the handle is 1 / (3(n - 1)) of the basis. In pixels that is the
    // ±85.3333 / ±56.9167 a real 1024x683 5-point GridWarp dump holds over a 256 x 170.75
    // spacing, and the ±170.667 / ±129.667 a 2048x1556 one holds over 512 x 389.
    return 1.0 / (3.0 * double(n - 1));
}

std::vector<FracPoint> neutralFractions(int rows, int cols)
{
    rows = clampPoints(rows);
    cols = clampPoints(cols);

    const double hu = neutralHandlesFraction(cols);
    const double hv = neutralHandlesFraction(rows);

    std::vector<FracPoint> out(size_t(rows) * size_t(cols));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            FracPoint& f = out[size_t(r) * size_t(cols) + size_t(c)];
            f.reset();
            f.x = double(c) / double(cols - 1);
            f.y = double(r) / double(rows - 1);
            f.h[kHVNeg][1] = -hv;
            f.h[kHVPos][1] = hv;
            f.h[kHUNeg][0] = -hu;
            f.h[kHUPos][0] = hu;
        }
    }
    return out;
}

GridPoint resolvePoint(const FracPoint& f, const Rect& basis)
{
    const double w = basis.width(), h = basis.height();

    GridPoint p;
    p.pos = Vec(basis.x0 + f.x * w, basis.y0 + f.y * h);
    for (int i = 0; i < kHandleCount; ++i)
        p.h[i] = Vec(f.h[i][0] * w, f.h[i][1] * h);
    return p;
}

FracPoint shrinkPoint(const GridPoint& p, const Rect& basis)
{
    const double w = basis.width(), h = basis.height();

    FracPoint f;
    f.reset();
    if (w > 0.0) {
        f.x = (p.pos.x - basis.x0) / w;
        for (int i = 0; i < kHandleCount; ++i) f.h[i][0] = p.h[i].x / w;
    }
    if (h > 0.0) {
        f.y = (p.pos.y - basis.y0) / h;
        for (int i = 0; i < kHandleCount; ++i) f.h[i][1] = p.h[i].y / h;
    }
    return f;
}

Grid resolveGrid(int rows, int cols, const Rect& basisIn, const std::vector<FracPoint>& knobs)
{
    rows = clampPoints(rows);
    cols = clampPoints(cols);

    // An empty basis would make the fractions meaningless; the unit square keeps the
    // round-trip exact instead. A render always passes a real basis (the Source clip's RoD,
    // or the `grid bounds` override).
    Rect basis = Rect::normalised(basisIn);
    if (basis.empty()) basis = Rect(0, 0, 1, 1);

    const std::vector<FracPoint> neutral = neutralFractions(rows, cols);

    Grid g(rows, cols);
    g.setBasis(basis);

    for (std::size_t i = 0; i < g.pointCount(); ++i) {
        // A knob a host was free to write as NaN or +-inf - a Double2D has no range guard, and a
        // script writes what it likes - is not a position. The neutral lattice's value for *that
        // component* is the fallback: the topology and the rest of the user's edit survive, and
        // every number downstream stays finite, which is what keeps the inverter's bucket indices,
        // a Rect union and a gather's tap indices defined whatever a knob holds
        // (tests/gw_grid_test.cpp B15, tests/gw_render_test.cpp H3).
        FracPoint f = i < knobs.size() ? knobs[i] : neutral[i];
        const FracPoint& dflt = neutral[i];
        f.x = finiteOr(f.x, dflt.x);
        f.y = finiteOr(f.y, dflt.y);
        for (int k = 0; k < kHandleCount; ++k) {
            f.h[k][0] = finiteOr(f.h[k][0], dflt.h[k][0]);
            f.h[k][1] = finiteOr(f.h[k][1], dflt.h[k][1]);
        }
        g.at(int(i / size_t(cols)), int(i % size_t(cols))) = resolvePoint(f, basis);
    }
    return g;
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

std::vector<FracPoint> Grid::fractions() const
{
    const Rect basis = _basis.empty() ? pointBounds() : _basis;

    std::vector<FracPoint> out(_pts.size());
    for (std::size_t i = 0; i < _pts.size(); ++i) out[i] = shrinkPoint(_pts[i], basis);
    return out;
}

void Grid::setCounts(int rows, int cols)
{
    rows = clampPoints(rows);
    cols = clampPoints(cols);

    if (rows == _rows && cols == _cols) return;

    // Points the old lattice also had are kept; the ones it did not have start neutral, so
    // that a topology change on its own never leaves a hole of zeros in the grid. Carrying a
    // *warp* across a topology change is resampleGrid()'s job, not this one's.
    const std::vector<FracPoint> neutral = neutralFractions(rows, cols);
    const Rect basis = _basis.empty() ? pointBounds() : _basis;

    std::vector<GridPoint> next(size_t(rows) * size_t(cols));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const std::size_t dst = size_t(r) * size_t(cols) + size_t(c);
            if (r < _rows && c < _cols) {
                next[dst] = _pts[size_t(r) * size_t(_cols) + size_t(c)];
            } else {
                next[dst] = resolvePoint(neutral[dst], basis);
            }
        }
    }

    _rows = rows;
    _cols = cols;
    _pts.swap(next);
}

Rect Grid::pointBounds() const
{
    if (_pts.empty()) return Rect();

    Rect b(_pts[0].pos.x, _pts[0].pos.y, _pts[0].pos.x, _pts[0].pos.y);
    for (std::size_t i = 1; i < _pts.size(); ++i) {
        const Vec& p = _pts[i].pos;
        if (p.x < b.x0) b.x0 = p.x;
        if (p.y < b.y0) b.y0 = p.y;
        if (p.x > b.x1) b.x1 = p.x;
        if (p.y > b.y1) b.y1 = p.y;
    }
    return Rect::normalised(b);
}

Rect Grid::surfaceBounds() const { return gridFootprint(*this); }

Grid Grid::neutral(int rows, int cols, const Rect& basis)
{
    return resolveGrid(rows, cols, basis, std::vector<FracPoint>());
}

Vec Grid::spacing() const
{
    const Rect basis = _basis.empty() ? pointBounds() : _basis;
    if (basis.empty()) return Vec(1, 1);
    return Vec(basis.width() / double(_cols - 1), basis.height() / double(_rows - 1));
}

Grid resampleGrid(const Grid& from, int rows, int cols, const Rect& basisIn)
{
    Grid out = Grid::neutral(rows, cols, basisIn);
    if (from.cellRows() < 1 || from.cellCols() < 1) return out;   // nothing to read off: stay neutral

    // A handle is one third of the derivative of a grid line at its end, and `evalCell`'s `du` /
    // `dv` are derivatives with respect to the *cell's own* 0..1 parameter. A cell of a `cols`-wide
    // lattice spans 1 / (cols - 1) of the grid's parameter, so carrying the same piece of surface
    // onto a lattice with a different number of cells rescales every arm by the ratio of those
    // spans. It is a ratio of cell counts, not of spacings: the rebuild's positions and handles are
    // read off the source grid, in pixels, and nothing about the rect the result is *labelled* with
    // enters them (that rect only lets Grid::fractions() express the result again).
    const double ku = double(from.cellCols()) / double(out.cellCols());
    const double kv = double(from.cellRows()) / double(out.cellRows());

    for (int r = 0; r < out.rows(); ++r) {
        for (int c = 0; c < out.cols(); ++c) {
            const double fu = double(c) / double(out.cols() - 1);
            const double fv = double(r) / double(out.rows() - 1);

            LatticeLocal ll;
            if (!localAtFraction(from, fu, fv, ll)) continue;

            Cell cell;
            if (!makeCell(from, ll.r, ll.c, cell)) continue;

            GridPoint& p = out.at(r, c);
            evalCell(cell, ll.u, ll.v, &p.pos, 0, 0);

            // The new point landed on one of the old lattice points: carry that point's four arms
            // (and its position, so the carry is bit-for-bit). Every arm is already exactly the
            // derivative of the piece of surface on its side - which is what the branch below
            // computes for a point that landed *between* lattice points - so this is the same rule,
            // read without the round trip through the derivative. A same-topology rebuild is all
            // coincident points, which is why changing `rows` / `cols` to the number they already
            // have cannot rewrite a single handle knob (tests F1/F2), and why a rebuild back down
            // an aligned lattice (3x3 -> 5x5 and 4x4 -> 7x7 split every cell in halves, 3x3 -> 7x7 in
            // thirds) is exact in both directions.
            if ((ll.u == 0.0 || ll.u == 1.0) && (ll.v == 0.0 || ll.v == 1.0)) {
                const GridPoint& q = from.at(ll.r + (ll.v == 1.0 ? 1 : 0), ll.c + (ll.u == 1.0 ? 1 : 0));
                p.pos = q.pos;
                p.h[kHUPos] = q.h[kHUPos] * ku;
                p.h[kHUNeg] = q.h[kHUNeg] * ku;
                p.h[kHVPos] = q.h[kHVPos] * kv;
                p.h[kHVNeg] = q.h[kHVNeg] * kv;
                continue;
            }

            // Each arm is one third of the old surface's axis partial *taken from the side that arm
            // points into*: a grid line crossing an old grid line has two different tangents there
            // whenever its two arms are asymmetric (which is what four independent handle knobs per
            // point means), and an arm belongs to the piece of line on its own side. At the outer
            // edge of the grid there is no piece on the outer side, and no cell samples an outer arm
            // either, so it mirrors the arm that does exist and simply continues the curve.
            const int cc = from.cellCols(), cr = from.cellRows();
            const bool stepRight = (ll.u == 1.0), stepLeft = (ll.u == 0.0);
            // The lattice's own v grows with canonical y, which is up, so v == 1 sits on the grid
            // line one row *above* ll.r and v == 0 on the one below it.
            const bool stepUp = (ll.v == 1.0), stepDown = (ll.v == 0.0);
            const int cRight = std::min(ll.c + (stepRight ? 1 : 0), cc - 1);
            const int cLeft = std::max(ll.c - (stepLeft ? 1 : 0), 0);
            const int rUp = std::min(ll.r + (stepUp ? 1 : 0), cr - 1);
            const int rDown = std::max(ll.r - (stepDown ? 1 : 0), 0);
            const bool noRight = stepRight && cRight == ll.c;
            const bool noLeft = stepLeft && cLeft == ll.c;
            const bool noUp = stepUp && rUp == ll.r;
            const bool noDown = stepDown && rDown == ll.r;

            Vec duRight, duLeft, dvUp, dvDown;
            Cell neigh;
            if (makeCell(from, ll.r, cRight, neigh))
                evalCell(neigh, (stepRight && !noRight) ? 0.0 : ll.u, ll.v, 0, &duRight, 0);
            if (makeCell(from, ll.r, cLeft, neigh))
                evalCell(neigh, (stepLeft && !noLeft) ? 1.0 : ll.u, ll.v, 0, &duLeft, 0);
            // Stepping onto the neighbouring row means the shared grid line is that cell's own
            // v == 0 edge (or v == 1 for the cell on the other side), which is where its partial is read.
            if (makeCell(from, rUp, ll.c, neigh))
                evalCell(neigh, ll.u, (stepUp && !noUp) ? 0.0 : ll.v, 0, 0, &dvUp);
            if (makeCell(from, rDown, ll.c, neigh))
                evalCell(neigh, ll.u, (stepDown && !noDown) ? 1.0 : ll.v, 0, 0, &dvDown);
            if (noRight) duRight = duLeft;
            if (noLeft) duLeft = duRight;
            if (noUp) dvUp = dvDown;
            if (noDown) dvDown = dvUp;

            Vec arms[kHandleCount];
            arms[kHUPos] = duRight * (ku / 3.0);
            arms[kHUNeg] = duLeft * (-ku / 3.0);
            arms[kHVPos] = dvUp * (kv / 3.0);
            arms[kHVNeg] = dvDown * (-kv / 3.0);

            for (int k = 0; k < kHandleCount; ++k) {
                if (std::isfinite(arms[k].x) && std::isfinite(arms[k].y)) {
                    p.h[k] = arms[k];
                    continue;
                }
                // A NaN reached the surface from a host's knob: fall back to the neutral handle of
                // the new lattice rather than propagate it through a render.
                const bool uAxis = (k == kHUPos || k == kHUNeg);
                const double neutral = uAxis ? neutralHandlesFraction(out.cols()) * out.basis().width()
                                             : neutralHandlesFraction(out.rows()) * out.basis().height();
                p.h[k] = uAxis ? Vec(neutral * (k == kHUPos ? 1.0 : -1.0), 0.0)
                               : Vec(0.0, neutral * (k == kHVPos ? 1.0 : -1.0));
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The Transform tab
// ---------------------------------------------------------------------------

Affine affineFor(const Transform& t)
{
    const double degToRad = kPi / 180.0;

    // The linear part, in the order grid.h documents: skew, then scale, then rotate, all
    // about `center`, then translate. Positive `rotate_deg` turns the grid counter-clockwise as
    // it appears in the Viewer (Nuke's convention), which is the textbook matrix in this frame
    // because canonical coordinates are y-*up*: a lattice fraction of y = r/(rows-1) resolves to
    // `basis.y0 + y * height`, and OFX describes its origin as the bottom-left corner
    // (`kOfxImageEffectHostPropNativeOriginBottomLeft`). Measured end to end in a host, and the
    // sense itself off the delivered pixels, in DECISIONS D-64/D-65.
    double k00 = 1, k01 = 0, k10 = 0, k11 = 1;

    if (t.skewXDeg != 0.0 || t.skewYDeg != 0.0) {
        const double kx = std::tan(t.skewXDeg * degToRad);   // x leans on y
        const double ky = std::tan(t.skewYDeg * degToRad);   // y leans on x

        // Kx = [1 kx; 0 1], Ky = [1 0; ky 1].
        if (t.skewOrderXY) {                 // X-skew first, so K = Ky * Kx
            k00 = 1.0;             k01 = kx;
            k10 = ky;              k11 = ky * kx + 1.0;
        } else {                             // Y-skew first, so K = Kx * Ky
            k00 = kx * ky + 1.0;   k01 = kx;
            k10 = ky;              k11 = 1.0;
        }
    }

    // The scale is applied to the *already sheared* points, which is a row scaling: S·K, not K·S.
    // (The leftmost factor in this chain is the one applied last - the extra matrix below is applied
    // last of all, exactly as the knob help says - so writing S·K here is what makes the order the
    // four places that state it state: skew, then scale, then rotate, then translate.) With K = I, or
    // with an isotropic scale, the two compositions are the same matrix; they separate as soon as a
    // scale is anisotropic *and* a shear is non-zero, which is what S11/S12 in tests/gw_sense_test.cpp
    // measures.
    const double s00 = t.sx * k00, s01 = t.sx * k01;
    const double s10 = t.sy * k10, s11 = t.sy * k11;

    double a00 = s00, a01 = s01, a10 = s10, a11 = s11;
    if (t.rotateDeg != 0.0) {
        // R(theta) = [cos -sin; sin cos]: the rotation post-multiplies skew·scale, so a point
        // that has already been sheared and scaled is then turned counter-clockwise about `center`.
        const double th = t.rotateDeg * degToRad;
        const double cs = std::cos(th), sn = std::sin(th);
        a00 = cs * s00 - sn * s10;
        a01 = cs * s01 - sn * s11;
        a10 = sn * s00 + cs * s10;
        a11 = sn * s01 + cs * s11;
    }

    Affine af;
    af.a00 = a00; af.a01 = a01;
    af.a10 = a10; af.a11 = a11;
    af.tx = t.cx - (a00 * t.cx + a01 * t.cy) + t.tx;
    af.ty = t.cy - (a10 * t.cx + a11 * t.cy) + t.ty;

    if (t.useMatrix) {
        // The extra matrix is applied last: p' = M·(L·(p - c) + c + translate). Nuke's
        // Transform tab carries a 2x3; the 3x3 rows are accepted and their perspective row
        // ignored, because a warp this plugin builds stays affine (D-53).
        const double m00 = t.m[0], m01 = t.m[1], m02 = t.m[2];
        const double m10 = t.m[3], m11 = t.m[4], m12 = t.m[5];

        const double n00 = af.a00, n01 = af.a01, n10 = af.a10, n11 = af.a11;
        const double nx = af.tx, ny = af.ty;

        af.a00 = m00 * n00 + m01 * n10;
        af.a01 = m00 * n01 + m01 * n11;
        af.a10 = m10 * n00 + m11 * n10;
        af.a11 = m10 * n01 + m11 * n11;
        af.tx = m00 * nx + m01 * ny + m02;
        af.ty = m10 * nx + m11 * ny + m12;
    }

    return af;
}

bool applyTransform(Grid& g, const Affine& af)
{
    if (!std::isfinite(af.det()) || std::fabs(af.det()) < 1e-12) return false;

    for (int r = 0; r < g.rows(); ++r) {
        for (int c = 0; c < g.cols(); ++c) {
            GridPoint& p = g.at(r, c);
            p.pos = af.map(p.pos);
            // A handle is a tangent, so it takes the linear part only. This is why a scaled
            // grid keeps its curvature: the handles scale with it (D-53).
            for (int i = 0; i < kHandleCount; ++i) p.h[i] = af.lin(p.h[i]);
        }
    }
    return true;
}

bool blendGrids(const Grid& src, const Grid& dst, double t, Grid& out)
{
    if (src.rows() != dst.rows() || src.cols() != dst.cols()) return false;

    t = clampUnit(t);
    const double a = 1.0 - t, b = t;

    Grid o(src.rows(), src.cols());
    o.setBasis(Rect(a * src.basis().x0 + b * dst.basis().x0,
                    a * src.basis().y0 + b * dst.basis().y0,
                    a * src.basis().x1 + b * dst.basis().x1,
                    a * src.basis().y1 + b * dst.basis().y1));

    for (std::size_t i = 0; i < src.pointCount(); ++i) {
        const GridPoint& s = src.points()[i];
        const GridPoint& d = dst.points()[i];

        GridPoint& g = o.at(int(i / size_t(o.cols())), int(i % size_t(o.cols())));
        g.pos = s.pos * a + d.pos * b;
        for (int k = 0; k < kHandleCount; ++k) g.h[k] = s.h[k] * a + d.h[k] * b;
    }

    out = o;
    return true;
}

bool gridsEqual(const Grid& a, const Grid& b)
{
    if (a.rows() != b.rows() || a.cols() != b.cols()) return false;
    if (a.pointCount() != b.pointCount()) return false;

    for (std::size_t i = 0; i < a.pointCount(); ++i) {
        const GridPoint& pa = a.points()[i];
        const GridPoint& pb = b.points()[i];
        if (pa.pos.x != pb.pos.x || pa.pos.y != pb.pos.y) return false;
        for (int k = 0; k < kHandleCount; ++k) {
            if (pa.h[k].x != pb.h[k].x || pa.h[k].y != pb.h[k].y) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// One cell: the bicubic net (D-56)
// ---------------------------------------------------------------------------

// The cubic Bezier through two neighbouring lattice points: the departing point's handle in
// the direction of travel, the arriving point's handle pointing back into the segment. With
// neutral handles (±spacing/3 at both ends) this is exactly the straight chord - which is
// the check tests/gw_grid_test.cpp B3 makes, because it is what fixes the sign convention
// the dumps cannot fix on their own.
//
// Public, because the Viewer overlay draws the *same* four controls as its hit-testable
// chords and needs the pairing stated the way the render states it (D-78, interact.h).
void edgeCubic(const GridPoint& from, const GridPoint& to, int fromHandle,
               int toHandle, Vec out[4])
{
    out[0] = from.pos;
    out[1] = from.pos + from.h[fromHandle];
    out[2] = to.pos + to.h[toHandle];
    out[3] = to.pos;
}

bool makeCell(const Grid& g, int r, int c, Cell& out)
{
    if (r < 0 || c < 0 || r >= g.cellRows() || c >= g.cellCols()) return false;

    const GridPoint& p00 = g.at(r, c);
    const GridPoint& p10 = g.at(r, c + 1);
    const GridPoint& p01 = g.at(r + 1, c);
    const GridPoint& p11 = g.at(r + 1, c + 1);

    // The four boundary cubics: the two horizontal grid lines bounding the cell (the u-edges)
    // and the two vertical ones (the v-edges). Neighbouring cells use the *same* four controls
    // for the line they share, so the surface agrees across that line and its derivative *along*
    // the line agrees too - no special-casing at cell borders (tests/gw_grid_test.cpp C2). The
    // derivative *across* a grid line is a different matter, and is allowed to jump: that is what
    // four independent handle knobs per point is (grid.h's continuity note).
    Vec bot[4], top[4], left[4], right[4];
    edgeCubic(p00, p10, kHUPos, kHUNeg, bot);
    edgeCubic(p01, p11, kHUPos, kHUNeg, top);
    edgeCubic(p00, p01, kHVPos, kHVNeg, left);
    edgeCubic(p10, p11, kHVPos, kHVNeg, right);

    // The net's edge rows and columns **are** the four boundary cubics - copied, not
    // recomputed from the formula. The Coons formula reproduces them too, but only up to
    // rounding, and that difference is not cosmetic: every conservative box this plugin
    // reports (Grid::surfaceBounds, cellBounds, and through them the RoD) is the hull of
    // these controls, so a grid whose boundary cubics are straight chords must have a hull
    // that is exactly the cell rectangle rather than that rectangle plus 2e-13 px.
    // Only the 2x2 interior is invented. That is D-56, and tests/gw_grid_test.cpp C1 checks
    // the edge controls against the boundary cubics control by control, exactly.
    for (int i = 0; i < 4; ++i) {
        out.c[i][0] = bot[i];
        out.c[i][3] = top[i];
        out.c[0][i] = left[i];
        out.c[3][i] = right[i];
    }

    for (int i = 1; i <= 2; ++i) {
        const double fu = double(i) * kThird;
        for (int j = 1; j <= 2; ++j) {
            const double fv = double(j) * kThird;

            Vec bil(0, 0);
            bil.x = (1.0 - fu) * (1.0 - fv) * p00.pos.x + fu * (1.0 - fv) * p10.pos.x
                  + (1.0 - fu) * fv * p01.pos.x + fu * fv * p11.pos.x;
            bil.y = (1.0 - fu) * (1.0 - fv) * p00.pos.y + fu * (1.0 - fv) * p10.pos.y
                  + (1.0 - fu) * fv * p01.pos.y + fu * fv * p11.pos.y;

            out.c[i][j] = bot[i] * (1.0 - fv) + top[i] * fv
                        + left[j] * (1.0 - fu) + right[j] * fu
                        - bil;
        }
    }

    out.r = r;
    out.col = c;
    return true;
}

void evalCell(const Cell& cell, double u, double v, Vec* pos, Vec* du, Vec* dv)
{
    double bu[4], bv[4];
    bernstein(u, bu);
    bernstein(v, bv);

    if (pos) {
        double x = 0, y = 0;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                const double w = bu[i] * bv[j];
                x += w * cell.c[i][j].x;
                y += w * cell.c[i][j].y;
            }
        }
        *pos = Vec(x, y);
    }

    if (du) {
        double duu[4];
        bernsteinDeriv(u, duu);
        double x = 0, y = 0;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                const double w = duu[i] * bv[j];
                x += w * cell.c[i][j].x;
                y += w * cell.c[i][j].y;
            }
        }
        *du = Vec(x, y);
    }

    if (dv) {
        double dvv[4];
        bernsteinDeriv(v, dvv);
        double x = 0, y = 0;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                const double w = bu[i] * dvv[j];
                x += w * cell.c[i][j].x;
                y += w * cell.c[i][j].y;
            }
        }
        *dv = Vec(x, y);
    }
}

Rect cellBounds(const Cell& cell)
{
    Rect b(cell.c[0][0].x, cell.c[0][0].y, cell.c[0][0].x, cell.c[0][0].y);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            const Vec& p = cell.c[i][j];
            if (p.x < b.x0) b.x0 = p.x;
            if (p.y < b.y0) b.y0 = p.y;
            if (p.x > b.x1) b.x1 = p.x;
            if (p.y > b.y1) b.y1 = p.y;
        }
    }
    return Rect::normalised(b);
}

Vec cellDerivativeMax(const Cell& cell)
{
    // dP/du = sum_i 3(c[i+1][j] - c[i][j]) B_i^2(u) B_j^2(v), and the degree-2 Bernstein
    // basis sums to 1 on [0,1], so the longest difference control is an upper bound on
    // |dP/du| everywhere in the cell. Same in v. Upper bounds are what a gather pad needs.
    double mu = 0, mv = 0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j)
            mu = std::max(mu, (cell.c[i + 1][j] - cell.c[i][j]).len());
    }
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 4; ++i)
            mv = std::max(mv, (cell.c[i][j + 1] - cell.c[i][j]).len());
    }
    return Vec(3.0 * mu, 3.0 * mv);
}

double cellStretch(const Cell& source, const Cell& out, int renderRes)
{
    const int res = clampRenderRes(renderRes);
    const int n = res + 1;

    // Forward-maps both nets on the *same* lattice, which is the one the inverter runs on,
    // and takes the worst ratio of corresponding steps. Ratio > 1 means the input's pixels
    // are further apart than the output's there - minification - so its gather must grow by
    // that factor (D-55). A degenerate output cell (zero step) contributes nothing rather
    // than an infinity.
    double best = 0;

    const std::size_t nn = static_cast<std::size_t>(n);
    std::vector<double> sx(nn), sy(nn), ox(nn), oy(nn);
    bool havePrevRow = false;

    for (int j = 0; j < n; ++j) {
        const double v = double(j) / double(res);

        double rowSx = 0, rowSy = 0, rowOx = 0, rowOy = 0;
        bool haveRowPrev = false;

        for (int i = 0; i < n; ++i) {
            const double u = double(i) / double(res);

            Vec sp, op;
            evalCell(source, u, v, &sp, 0, 0);
            evalCell(out, u, v, &op, 0, 0);

            if (haveRowPrev) {
                const Vec ds(sp.x - rowSx, sp.y - rowSy);
                const Vec do_(op.x - rowOx, op.y - rowOy);
                if (do_.len() > 1e-9) best = std::max(best, ds.len() / do_.len());
            }
            if (havePrevRow) {
                const Vec ds(sp.x - sx[i], sp.y - sy[i]);
                const Vec do_(op.x - ox[i], op.y - oy[i]);
                if (do_.len() > 1e-9) best = std::max(best, ds.len() / do_.len());
            }

            rowSx = sp.x; rowSy = sp.y;
            rowOx = op.x; rowOy = op.y;
            sx[i] = sp.x; sy[i] = sp.y;
            ox[i] = op.x; oy[i] = op.y;
            haveRowPrev = true;
        }
        havePrevRow = true;
    }

    if (!std::isfinite(best)) return 0.0;

    // A stretch that differs from a *whole number* only by the rounding of those differences *is* that
    // whole number. The distinction is not cosmetic: `gatherTaps` is `2 * ceil(base * gain) + 1`, so a
    // gain of 1 + 1e-16 rounds the radius up to the next whole pixel and hands the render two extra taps
    // and a host a wider RoI for a minification that does not exist. The two cases a host actually
    // produces are both measurements of a ratio that is exactly a rational number: a translated grid (or
    // a same-topology copy of one), whose two nets have the same steps to within a few ulps, and a grid
    // the Transform tab scaled by a whole factor, whose steps are the same differences multiplied by
    // that factor - measured as 2.0000000000000155, which ceils to 3 and asks for a pad of 3 instead of 2
    // (tests/gw_render_test.cpp G4 and F5).
    //
    // Snapping *down* cannot make a gather read short: the tap this drops is one whose distance from the
    // sample position exceeds the radius by at most 1e-9 relative, and every filter in resample.h is 0
    // at or beyond its own radius, so that tap's weight is 0.
    //
    // 1e-9 relative is far below what this measurement can resolve: the steps are differences of double
    // positions on a (renderRes+1)^2 lattice, and the lattice itself discretises a cell into
    // 1/10th-of-a-cell samples at the default `render_res`.
    const double nearest = std::floor(best + 0.5);
    if (nearest >= 1.0 && std::fabs(best - nearest) <= 1e-9 * nearest) return nearest;
    return best;
}

void pixelGain(const Vec& inDu, const Vec& inDv, const Vec& opDu, const Vec& opDv,
               double cellGain, double& gainX, double& gainY)
{
    // `cellGain` <= 1 is the render's own "no minification in this cell" answer, and the gather a
    // gain of 1 produces is the filter's base radius: nothing to dilate, nothing to compute.
    if (!(cellGain > 1.0)) { gainX = 1.0; gainY = 1.0; return; }

    const double det = opDu.x * opDv.y - opDu.y * opDv.x;
    if (!(det > 0.0)) {                  // folded / degenerate / NaN: no inverse to divide by
        gainX = cellGain;
        gainY = cellGain;
        return;
    }

    // A = J_input · J_operative⁻¹, expanded by hand so this is one division.
    const double inv = 1.0 / det;
    const double a00 = (inDu.x * opDv.y - inDv.x * opDu.y) * inv;
    const double a01 = (inDv.x * opDu.x - inDu.x * opDv.x) * inv;
    const double a10 = (inDu.y * opDv.y - inDv.y * opDu.y) * inv;
    const double a11 = (inDv.y * opDu.x - inDu.y * opDv.x) * inv;

    // A number too large to be a slope is a fold this Jacobian test did not see, and a NaN would
    // silently turn the clamp into "no dilation at all". Fall back to the cell's own gain.
    if (!std::isfinite(a00) || !std::isfinite(a01) || !std::isfinite(a10) || !std::isfinite(a11)) {
        gainX = cellGain;
        gainY = cellGain;
        return;
    }

    // The two row sums: the axis-aligned extent of the image of the unit tangent square under A.
    // The >= 1 floor is `gatherDilation`'s own floor (resample.cpp), which answers 1 for a gain of 1
    // or less, so a locally *magnifying* part of a minifying cell asks for the base radius and no
    // more — which is the half of the fix the user reported.
    double gx = std::max(1.0, std::fabs(a00) + std::fabs(a01));
    double gy = std::max(1.0, std::fabs(a10) + std::fabs(a11));
    if (gx > cellGain) gx = cellGain;   // the gather window and RoI pad are sized from cellGain
    if (gy > cellGain) gy = cellGain;
    gainX = gx;
    gainY = gy;
}

Rect gridFootprint(const Grid& g)
{
    Rect acc;
    Cell cell;
    for (int r = 0; r < g.cellRows(); ++r) {
        for (int c = 0; c < g.cellCols(); ++c) {
            if (!makeCell(g, r, c, cell)) continue;
            acc = Rect::unite(acc, cellBounds(cell));
        }
    }
    if (acc.empty()) return g.pointBounds();
    return acc;
}

std::vector<CellPlan> planCells(const Grid& src, const Grid& dst, const Grid& out, int renderRes)
{
    std::vector<CellPlan> plans;
    Cell co, cs, cd;

    for (int r = 0; r < out.cellRows(); ++r) {
        for (int c = 0; c < out.cellCols(); ++c) {
            if (!makeCell(out, r, c, co)) continue;

            CellPlan p;
            p.r = r;
            p.c = c;
            p.outBox = cellBounds(co);
            p.srcStretch = 0;
            p.dstStretch = 0;

            // The (u,v) domain of a lattice cell is the unit square whichever way it is
            // warped, so the gather region for an input is exactly that input's own cell hull.
            if (makeCell(src, r, c, cs)) {
                p.srcBox = cellBounds(cs);
                p.srcStretch = cellStretch(cs, co, renderRes);
            } else {
                p.srcBox = p.outBox;
            }
            if (makeCell(dst, r, c, cd)) {
                p.dstBox = cellBounds(cd);
                p.dstStretch = cellStretch(cd, co, renderRes);
            } else {
                p.dstBox = p.outBox;
            }

            p.srcGain = std::max(1.0, p.srcStretch);
            p.dstGain = std::max(1.0, p.dstStretch);
            plans.push_back(p);
        }
    }
    return plans;
}

// ---------------------------------------------------------------------------
// The inversion lattice (D-55)
// ---------------------------------------------------------------------------

Inverter::Inverter()
    : _res(kDefaultRenderRes), _built(false), _bucketSize(1), _bx0(0), _by0(0),
      _bcols(0), _brows(0), _tol(1e-4), _meanStep(1), _minDet(1e-12)
{
}

Inverter::Inverter(const Grid& g, int renderRes)
    : _res(kDefaultRenderRes), _built(false), _bucketSize(1), _bx0(0), _by0(0),
      _bcols(0), _brows(0), _tol(1e-4), _meanStep(1), _minDet(1e-12)
{
    build(g, renderRes);
}

void Inverter::build(const Grid& g, int renderRes)
{
    _built = false;
    _grid = g;
    _res = clampRenderRes(renderRes);

    _cells.clear();
    _cellBoxes.clear();
    _samples.clear();
    _sorted.clear();
    _bucketStart.clear();
    _bucketSize = 1.0;
    _bx0 = _by0 = 0;
    _bcols = _brows = 0;
    _meanStep = 1.0;
    _tol = 1e-4;
    _minDet = 1e-12;

    if (g.cellRows() < 1 || g.cellCols() < 1) return;

    _cells.reserve(size_t(g.cellRows()) * size_t(g.cellCols()));
    for (int r = 0; r < g.cellRows(); ++r) {
        for (int c = 0; c < g.cellCols(); ++c) {
            Cell cell;
            if (!makeCell(g, r, c, cell)) continue;
            _cells.push_back(cell);
            _cellBoxes.push_back(cellBounds(cell));
        }
    }
    if (_cells.empty()) return;

    // Forward-map every cell onto its (renderRes+1)^2 sample lattice. renderRes is read as
    // the number of subdivisions between the Bezier curves (Foundry's wording for
    // `render_res`, and JOS's "triangles per grid cell side"), so this lattice *is* what the
    // knob means (D-55).
    const int n = _res + 1;
    _samples.reserve(_cells.size() * size_t(n) * size_t(n));

    double stepSum = 0;
    std::size_t steps = 0;

    std::vector<Vec> lat(size_t(n) * size_t(n));
    for (std::size_t ci = 0; ci < _cells.size(); ++ci) {
        const Cell& cell = _cells[ci];

        for (int j = 0; j < n; ++j) {
            const double v = double(j) / double(_res);
            for (int i = 0; i < n; ++i)
                evalCell(cell, double(i) / double(_res), v, &lat[size_t(j) * size_t(n) + size_t(i)], 0, 0);
        }

        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const Vec& p = lat[size_t(j) * size_t(n) + size_t(i)];
                _samples.push_back(Sample{ p.x, p.y, int(ci), i, j });

                if (i + 1 < n) {
                    stepSum += (lat[size_t(j) * size_t(n) + size_t(i + 1)] - p).len();
                    ++steps;
                }
                if (j + 1 < n) {
                    stepSum += (lat[size_t(j + 1) * size_t(n) + size_t(i)] - p).len();
                    ++steps;
                }
            }
        }
    }

    if (steps > 0) _meanStep = stepSum / double(steps);
    if (!std::isfinite(_meanStep) || _meanStep <= 0.0) _meanStep = 1.0;

    // "Found" means the residual is this small in canonical px: one hundred-thousandth of a
    // lattice step, so a lattice fine enough to resolve the warp resolves the root too.
    _tol = std::max(1e-5 * _meanStep, 1e-9);

    // A Jacobian this small means the cell is locally collapsed and a Newton step would be
    // meaningless. Scaled by the typical |dP/du x dP/dv| of this lattice (one cell's area per
    // unit parameter, i.e. (meanStep * res)^2).
    const double typical = _meanStep * double(_res);
    _minDet = 1e-9 * typical * typical;
    if (_minDet <= 0.0) _minDet = 1e-30;

    buildBuckets();
    _built = true;
}

void Inverter::buildBuckets()
{
    _sorted.clear();
    _bucketStart.clear();
    _bcols = _brows = 0;

    if (_samples.empty()) {
        _bucketStart.assign(1, 0);
        _bucketSize = _meanStep;
        return;
    }

    double minx = _samples[0].x, maxx = minx;
    double miny = _samples[0].y, maxy = miny;
    for (std::size_t i = 1; i < _samples.size(); ++i) {
        const Sample& s = _samples[i];
        if (s.x < minx) minx = s.x;
        if (s.x > maxx) maxx = s.x;
        if (s.y < miny) miny = s.y;
        if (s.y > maxy) maxy = s.y;
    }

    _bx0 = minx;
    _by0 = miny;

    // One lattice step per bucket, so a query needs its own bucket and the eight around it to
    // see every sample a neighbouring step could be. If a pathological grid (a spiral, a cell
    // folded back on itself many times) would spread those buckets over an area far bigger
    // than the sample count justifies, the buckets grow instead - bounded work either way,
    // and deterministic because the growth is arithmetic.
    _bucketSize = _meanStep;
    if (_bucketSize < 1e-9) _bucketSize = 1e-9;

    const std::size_t budget = _samples.size() * 2 + 1024;
    for (int guard = 0; guard < 64; ++guard) {
        const int bc = int(std::ceil((maxx - minx) / _bucketSize)) + 1;
        const int br = int(std::ceil((maxy - miny) / _bucketSize)) + 1;
        if (bc < 1) return;
        _bcols = bc;
        _brows = br;
        if (size_t(bc) * size_t(br) <= budget) break;
        _bucketSize *= 2.0;
    }
    if (_bcols <= 0 || _brows <= 0) {
        _bcols = _brows = 1;
        _bucketSize = std::max(maxx - minx, maxy - miny);
        if (_bucketSize < 1e-9) _bucketSize = 1e-9;
    }

    const std::size_t buckets = size_t(_bcols) * size_t(_brows);
    _bucketStart.assign(buckets + 1, 0);

    for (std::size_t i = 0; i < _samples.size(); ++i) {
        const Sample& s = _samples[i];
        int bx = int(std::floor((s.x - _bx0) / _bucketSize));
        int by = int(std::floor((s.y - _by0) / _bucketSize));
        if (bx < 0) bx = 0;
        if (by < 0) by = 0;
        if (bx >= _bcols) bx = _bcols - 1;
        if (by >= _brows) by = _brows - 1;
        _bucketStart[size_t(by) * size_t(_bcols) + size_t(bx) + 1]++;
    }
    for (std::size_t i = 0; i < buckets; ++i)
        _bucketStart[i + 1] += _bucketStart[i];

    _sorted.resize(_samples.size());
    std::vector<std::size_t> cursor(_bucketStart.begin(), _bucketStart.end());
    // Samples enter their bucket in sample order, so a bucket's scan order is fixed.
    for (std::size_t i = 0; i < _samples.size(); ++i) {
        const Sample& s = _samples[i];
        int bx = int(std::floor((s.x - _bx0) / _bucketSize));
        int by = int(std::floor((s.y - _by0) / _bucketSize));
        if (bx < 0) bx = 0;
        if (by < 0) by = 0;
        if (bx >= _bcols) bx = _bcols - 1;
        if (by >= _brows) by = _brows - 1;
        const std::size_t b = size_t(by) * size_t(_bcols) + size_t(bx);
        _sorted[cursor[b]++] = int(i);
    }
}

void Inverter::collectCandidates(double x, double y, int radius) const
{
    _cands.clear();
    if (_bcols <= 0 || _brows <= 0) return;

    const int bx = int(std::floor((x - _bx0) / _bucketSize));
    const int by = int(std::floor((y - _by0) / _bucketSize));

    for (int dy = -radius; dy <= radius; ++dy) {
        const int cy = by + dy;
        if (cy < 0 || cy >= _brows) continue;
        for (int dx = -radius; dx <= radius; ++dx) {
            const int cx = bx + dx;
            if (cx < 0 || cx >= _bcols) continue;

            const std::size_t b = size_t(cy) * size_t(_bcols) + size_t(cx);
            const std::size_t kEnd = size_t(_bucketStart[b + 1]);
            for (std::size_t k = size_t(_bucketStart[b]); k < kEnd; ++k) {
                const int si = _sorted[k];
                const double ddx = _samples[size_t(si)].x - x;
                const double ddy = _samples[size_t(si)].y - y;
                _cands.push_back(Cand{ ddx * ddx + ddy * ddy, si });
            }
        }
    }

    std::sort(_cands.begin(), _cands.end(), [](const Cand& a, const Cand& b) {
        if (a.d2 != b.d2) return a.d2 < b.d2;
        return a.sample < b.sample;   // total order: same distance, same sample index order
    });

    if (_cands.size() > kMaxCandidates) _cands.resize(kMaxCandidates);
}

bool Inverter::refine(int cellIndex, double u0, double v0, double targetX, double targetY,
                      Hit& out) const
{
    const Cell& cell = _cells[size_t(cellIndex)];

    double u = u0, v = v0;
    double bestRes = std::numeric_limits<double>::infinity();
    double bestU = u, bestV = v;

    for (int it = 0; it < kNewtonIterations; ++it) {
        Vec p, du, dv;
        evalCell(cell, u, v, &p, &du, &dv);

        const double ex = targetX - p.x;
        const double ey = targetY - p.y;
        const double res = std::sqrt(ex * ex + ey * ey);

        if (res < bestRes) {
            bestRes = res;
            bestU = u;
            bestV = v;
            if (res <= _tol) break;
        }

        const double det = du.x * dv.y - du.y * dv.x;
        if (!std::isfinite(det) || std::fabs(det) < _minDet) break;

        // One Newton step: the parameter correction is J^-1 times the error, with
        // J = [du dv] and J^-1 = [dv.y -dv.y; -dv.x du.x] / det (D-67). The u row therefore reads
        // `dv.x`, not `du.x`: the two are not interchangeable, and a cell whose du.x vanishes - a
        // quarter turn does exactly that, grid.cpp's own rotation makes du.x the cosine of the turn -
        // makes a wrong numerator drop the whole term instead of merely losing accuracy. G26..G31 count
        // coverage of a turned lattice over a whole output window, which is where that shows up.
        double nu = u + (ex * dv.y - ey * dv.x) / det;
        double nv = v + (du.x * ey - du.y * ex) / det;
        if (!std::isfinite(nu) || !std::isfinite(nv)) break;

        // Clamping to the cell is what makes an outside query converge to the boundary of
        // the nearest cell instead of running off into another cell's parameter space: the
        // root is then the cell's edge, and the residual says whether the grid reaches here.
        if (nu < 0.0) nu = 0.0;
        if (nu > 1.0) nu = 1.0;
        if (nv < 0.0) nv = 0.0;
        if (nv > 1.0) nv = 1.0;

        if (nu == u && nv == v) break;
        u = nu;
        v = nv;
    }

    Vec p, du, dv;
    evalCell(cell, bestU, bestV, &p, &du, &dv);

    out.r = cell.r;
    out.c = cell.col;
    out.u = bestU;
    out.v = bestV;
    out.pos = p;
    out.residual = bestRes;
    out.jac = du.x * dv.y - du.y * dv.x;
    out.du = du;
    out.dv = dv;

    return bestRes <= _tol;
}

bool Inverter::find(double x, double y, Hit& out) const
{
    out = Hit();
    out.residual = std::numeric_limits<double>::infinity();
    if (!_built) return false;

    // 1. The nearest forward-mapped samples, nearest first, first root wins. For a query the
    //    grid covers this is the ordinary case: one Newton run.
    collectCandidates(x, y, 1);
    for (std::size_t i = 0; i < _cands.size(); ++i) {
        const Sample& s = _samples[size_t(_cands[i].sample)];
        if (refine(s.cell, double(s.i) / double(_res), double(s.j) / double(_res), x, y, out))
            return true;
    }

    // 2. Fallback for a query the lattice's own neighbourhood could not reach: every cell
    //    whose hull box contains it, tried in cell order from the box-relative bilinear
    //    start. Cells whose box does not contain the query cost one rectangle test each, so a
    //    pixel the grid genuinely does not cover still costs nothing but that scan - which is
    //    what keeps "not covered" cheap, and this path is only taken when 1 failed.
    double bestU = 0, bestV = 0;
    int bestCell = -1;
    double bestStart = std::numeric_limits<double>::infinity();
    for (std::size_t ci = 0; ci < _cellBoxes.size(); ++ci) {
        const Rect& b = _cellBoxes[ci];
        if (!b.contains(x, y)) continue;

        const double w = b.width(), h = b.height();
        const double u0 = w > 0.0 ? clampUnit((x - b.x0) / w) : 0.5;
        const double v0 = h > 0.0 ? clampUnit((y - b.y0) / h) : 0.5;

        const double start2 = (u0 - 0.5) * (u0 - 0.5) + (v0 - 0.5) * (v0 - 0.5);
        if (start2 < bestStart) {   // a tie keeps the earlier cell: total order again
            bestStart = start2;
            bestU = u0;
            bestV = v0;
            bestCell = int(ci);
        }
    }

    if (bestCell >= 0) {
        Hit hit;
        if (refine(bestCell, bestU, bestV, x, y, hit)) {
            out = hit;
            return true;
        }
        if (hit.residual < out.residual) out = hit;   // report the near miss
    }

    return false;
}

} // namespace gw
