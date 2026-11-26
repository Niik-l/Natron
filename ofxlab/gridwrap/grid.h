// GridWrap: the grid geometry core (DECISIONS.md D-53, D-54, D-55, D-56).
//
// OFX-free on purpose, like ZDefocusZ's zd.h and TimeWarp's flow.h: this is the whole
// warp semantics of the plugin, and every claim in it is checked headlessly
// (tests/gw_grid_test.cpp) before a single pixel is resampled. It lives in
// plugins/gridwrap/ because only this plugin uses it (D-06).
//
// The reference is Nuke's GridWarp, whose semantics are recorded in
// references/gridwrap/nuke_gridwrap_knobs.md. There is no gizmo to port (unlike
// SharpenZ's X_Sharpen, D-08), so this file *is* the port, and the numbers it produces
// for an untouched grid are pinned against real GridWarp script dumps: the neutral
// handles measure ±(spacing/3) in the dumps, which is what neutralFractions() encodes
// as pure fractions and this file resolves to pixels (see the D-53 note on why the
// knobs hold fractions).
//
// ---------------------------------------------------------------------------
// The object: two lattices of points, each point with four tangent handles
// ---------------------------------------------------------------------------
// A grid is a `rows x cols` lattice of control points over a rectangle. Each point
// carries four **independent** tangent handles - the two tangents of the vertical grid
// line through it and the two tangents of the horizontal one - which is what Nuke's
// viewer tool edits and what its script dumps store:
//
//     { {2 256 0} { {2 0 56.9167} {2 0 -56.9167} {2 85.3333 0} {2 -85.3333 0} } }
//         position      handle 1        2           3          4
//
// The four are independent, so tangent continuity can be broken on either axis at any
// point. A grid *line* between two neighbouring points is the cubic Bezier through the
// departing point's handle pointing along the travel direction and the arriving point's
// handle pointing back, so a cell is bounded by four such cubics.
//
// ---------------------------------------------------------------------------
// A cell is a transfinite (Coons) patch - which is exactly a bicubic Bezier net
// ---------------------------------------------------------------------------
// Four boundary cubics do not define an interior, and there are many ways to fill one.
// The Coons patch is the transfinite interpolation of the two ruled surfaces swept
// from the u-edges and the v-edges, minus the bilinear patch of the four corners:
//
//     P(u,v) = (1-v)·bottom(u) + v·top(u) + (1-u)·left(v) + u·right(v)
//              - [(1-u)(1-v)·C00 + u(1-v)·C10 + (1-u)v·C01 + u·v·C11]
//
// For cubic boundaries that surface *is* a bicubic Bezier net: its 4x4 control points
// are the four edges' controls, and its two-by-two interior ones follow from the formula
// (D-56). So the cell costs 16 points, evaluates as a tensor-product Bernstein patch with
// exact derivatives, its control hull bounds it exactly (the Bezier hull property), and
// blending two grids point-by-point then building a cell commutes with building the cell
// from each grid first - because every step above is linear in the boundary data. That
// commutation is what makes the `morph` reading (D-54) cheap: one grid, one inversion per
// output pixel, two samples.
//
// One continuity caveat, because it is the whole of what a lattice rebuild has to live with.
// Neighbouring cells use the *same* four controls for the grid line they share, so the surface
// is continuous across that line and its derivative **along** the line agrees from both sides.
// The derivative **across** the line need not agree: it is swept from the neighbouring points'
// handles, which are different knobs. So the surface is C0 across a grid line (and C1 along
// it), and at a lattice point the axis partials are -3·hUNeg leaving left, +3·hUPos leaving
// right, -3·hVNeg leaving down, +3·hVPos leaving up - four numbers, not two. That is what four
// independent handle knobs per point *means*, and it is why a rebuild onto a lattice whose cells
// straddle an old grid line cannot reproduce the old map exactly (tests/gw_grid_test.cpp F8,
// F19; the exact cases are F1, F17, F18).
//
// ---------------------------------------------------------------------------
// Inverting it
// ---------------------------------------------------------------------------
// Rendering is inverse mapping: an output pixel needs the (u,v) that the operative grid
// maps *to* it. `renderRes` is read as the number of subdivisions between the Bezier
// curves (Foundry's wording) = per cell side (JOS's "triangles per grid cell side"), and
// the inverter uses exactly that: the cell is forward-mapped onto a (renderRes+1)^2
// sample lattice, the samples are bucketed in output space, and an output pixel starts
// from its nearest sample and Newton-refines. `renderRes` therefore trades cost against
// how far a Newton start is from its root, which is the trade-off both pages describe;
// the convergence ladder is measured, not assumed (D-55, tests/run.sh sweep).
#ifndef OFXLAB_GW_GRID_H
#define OFXLAB_GW_GRID_H

#include <cmath>
#include <cstddef>
#include <vector>

namespace gw {

// ---------------------------------------------------------------------------
// Ranges and defaults, declared exactly once (D-53).
//
// ARCHITECTURE.md's GridWrap knob table, this file's clamps and the plugin's parameter
// descriptors are the three statements of the same numbers; a knob change edits all
// three together (repo convention, and what tests/gw_grid_test.cpp checks for the
// topology and resolution bounds).
// ---------------------------------------------------------------------------

// Control points per axis. Nuke's `divisions` allows 3..20; the ceiling here is 8,
// because the grid travels as ordinary OFX parameters (D-53) and 8x8 is 64 points x
// 5 knobs x 2 grids = 640 parameters, which is what a descriptor can describe and a
// host can enumerate without turning the knob panel into a wall.
enum {
    kMinPoints = 3,
    kMaxPoints = 8,
    kDefaultPoints = 5,          // Nuke's `divisions` default is 5 as well

    // `render_res`: subdivisions per cell side. Foundry 10, JOS 1..50.
    kMinRenderRes = 1,
    kMaxRenderRes = 50,
    kDefaultRenderRes = 10
};

// `warp` and `mix` are the two axes of the morph (D-54). Their defaults are Nuke's,
// and - which is the check that makes the corner reading in DECISIONS D-54 the one this
// file implements - the pair (1, 0) is Nuke's default `output` value, `source warped`.
enum { kWarpDefaultNum = 1, kMixDefaultNum = 0 };

// ---------------------------------------------------------------------------
// Plain geometry
// ---------------------------------------------------------------------------

struct Vec {
    double x, y;

    Vec() : x(0), y(0) {}
    Vec(double x_, double y_) : x(x_), y(y_) {}

    Vec operator+(const Vec& o) const { return Vec(x + o.x, y + o.y); }
    Vec operator-(const Vec& o) const { return Vec(x - o.x, y - o.y); }
    Vec operator*(double s) const { return Vec(x * s, y * s); }
    double dot(const Vec& o) const { return x * o.x + y * o.y; }
    double len2() const { return x * x + y * y; }
    double len() const { return std::sqrt(len2()); }
};

// An axis-aligned box in canonical coordinates: OFX's image-plane space, rooted at the format's
// 0,0, x right, and y increasing *up* the picture. Two sentences of the header say which way:
// the extent property "only returns the top right position, as the extent is always rooted at 0,0"
// (ofxImageEffect.h:772), and "row bytes can be negative, which allows hosts with a native top down
// row order to pass image into OFX without having to repack pixels" (:885). So a file a host writes
// puts this rect's *largest* y on its first scanline (D-64), and nothing inside this plugin converts:
// every read and write is anchored by coordinate (`getPixelAddress`, `SourceWindow::row`), never by
// position in a buffer, so the plugin's arithmetic is sign-agnostic and only a *checker* needs the
// direction — which is why D-64 states the measurement rather than trusting this comment.
struct Rect {
    double x0, y0, x1, y1;

    Rect() : x0(0), y0(0), x1(0), y1(0) {}
    Rect(double x0_, double y0_, double x1_, double y1_) : x0(x0_), y0(y0_), x1(x1_), y1(y1_) {}

    // Always a sane box: an empty one is a point, a reversed one is swapped.
    static Rect normalised(Rect r)
    {
        Rect o(r);
        if (o.x1 < o.x0) std::swap(o.x0, o.x1);
        if (o.y1 < o.y0) std::swap(o.y0, o.y1);
        return o;
    }

    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
    bool empty() const { return width() <= 0 || height() <= 0; }
    Vec center() const { return Vec(0.5 * (x0 + x1), 0.5 * (y0 + y1)); }
    bool contains(double x, double y) const
    {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    Rect inflated(double pad) const
    {
        return Rect(x0 - pad, y0 - pad, x1 + pad, y1 + pad);
    }
    static Rect unite(const Rect& a, const Rect& b)
    {
        if (a.empty()) return b;
        if (b.empty()) return a;
        return Rect(std::min(a.x0, b.x0), std::min(a.y0, b.y0),
                    std::max(a.x1, b.x1), std::max(a.y1, b.y1));
    }
    static Rect intersect(const Rect& a, const Rect& b)
    {
        Rect o(std::max(a.x0, b.x0), std::max(a.y0, b.y0),
               std::min(a.x1, b.x1), std::min(a.y1, b.y1));
        if (o.x1 < o.x0 || o.y1 < o.y0) return Rect();
        return o;
    }
};

// ---------------------------------------------------------------------------
// Which handle is which
// ---------------------------------------------------------------------------
// Handles are named by the axis they leave along and the direction they point in, in canonical
// pixels: `kHVPos`'s arm points towards larger y, which is *up* as the Viewer shows it (a lattice
// fraction maps to `basis.y0 + f * height`, and row 0 is therefore the bottom row of the grid). A
// grid line from (r,c) to (r,c+1) uses `at(r,c).h[kHUPos]` and `at(r,c+1).h[kHUNeg]`; ditto the
// vertical line from (r,c) to (r+1,c) and kHVPos/kHVNeg. Naming them by axis and sign, instead of
// Nuke's dump order, is what keeps the pairing unambiguous: the dumps list the four identically for
// every point (a neutral grid has no direction to read off them), so no sign convention can be
// inferred from them (references/.../nuke_gridwrap_knobs.md) — this frame is the one that decides,
// and the knob labels in gridwrap.cpp are the same four directions (D-65).
enum { kHVNeg = 0, kHVPos, kHUNeg, kHUPos, kHandleCount };

struct GridPoint {
    Vec pos;
    Vec h[kHandleCount];

    GridPoint() {}
};

// ---------------------------------------------------------------------------
// The knob-side view of a grid: pure fractions (D-53)
// ---------------------------------------------------------------------------
// Every grid quantity a parameter carries is a **fraction of the grid basis**, not a
// pixel: a position is (0..1, 0..1) across the basis rect and a handle is a fraction of
// the basis width (u handles) or height (v handles). Defaults, clamps and a lattice
// rebuild are then the same numbers at 1920x1080 and at 128x128, the descriptor can
// state them without knowing any format, and the plugin never writes to its own knobs.
// The pixel domain reappears in `resolveGrid()`, where a basis rect - the Source clip's
// region of definition, or the `grid bounds` override - turns them back.
struct FracPoint {
    double x, y;
    double h[kHandleCount][2];

    FracPoint() { reset(); }
    void reset()
    {
        x = y = 0;
        for (int i = 0; i < kHandleCount; ++i) h[i][0] = h[i][1] = 0;
    }
};

// The untouched lattice on a `rows x cols` grid: positions i/(cols-1), j/(rows-1), and
// handles at **one third** of the spacing, which is the cubic-Bezier "control at 1/3 of
// the chord on both ends" arc and the value real GridWarp dumps hold (D-53).
// `neutralHandlesFraction(n)` is the same statement in one number.
double neutralHandlesFraction(int pointsPerAxis);
std::vector<FracPoint> neutralFractions(int rows, int cols);

// Resolve knob fractions into canonical pixels over `basis`.
GridPoint resolvePoint(const FracPoint& f, const Rect& basis);
FracPoint shrinkPoint(const GridPoint& p, const Rect& basis);   // the inverse

class Grid;   // the knob-side view below builds one

// The whole knob-side grid, with `rows`/`cols` clamped to kMinPoints..kMaxPoints and a
// short list of knob fractions (fewer than points) filled from the neutral lattice. The
// resulting Grid remembers `basis`, which is what lets Grid::fractions() express it back.
Grid resolveGrid(int rows, int cols, const Rect& basis, const std::vector<FracPoint>& knobs);

// ---------------------------------------------------------------------------
// The grid
// ---------------------------------------------------------------------------
class Grid {
public:
    Grid() : _rows(kDefaultPoints), _cols(kDefaultPoints), _basis()
    {
        _pts.resize(size_t(_rows) * _cols);
    }
    Grid(int rows, int cols) : _rows(rows), _cols(cols), _basis()
    {
        _pts.resize(size_t(_rows) * _cols);
    }

    int rows() const { return _rows; }
    int cols() const { return _cols; }
    int cellRows() const { return _rows - 1; }
    int cellCols() const { return _cols - 1; }

    // The rect the knob fractions were resolved against - empty when this Grid was built
    // straight from pixels, in which case fractions() uses pointBounds() instead.
    const Rect& basis() const { return _basis; }
    void setBasis(const Rect& basis) { _basis = basis; }

    std::size_t pointCount() const { return _pts.size(); }
    const std::vector<GridPoint>& points() const { return _pts; }
    std::vector<FracPoint> fractions() const;   // the knob-side view (inverse of resolveGrid)

    // Row-major. Row 0 carries the smallest fraction y and therefore sits at the *bottom* of the
    // grid as the Viewer shows it, matching Foundry's "control points are indexed starting from the
    // bottom left 1.1" for the same lattice.
    const GridPoint& at(int r, int c) const { return _pts[size_t(r) * _cols + c]; }
    GridPoint& at(int r, int c) { return _pts[size_t(r) * _cols + c]; }

    // Resize to a new topology, keeping nothing: use resampleGrid() to carry a warp over.
    void setCounts(int rows, int cols);

    // Box of the control points, and the conservative box the surface can reach - the
    // hull of every cell's 16 controls (D-56). They differ: handles reach past points.
    Rect pointBounds() const;
    Rect surfaceBounds() const;

    // Uniform lattice over a rect, neutral handles (±spacing/3). This is the grid a fresh
    // node shows, and the one Nuke's dumps show (see tests/gw_grid_test.cpp B1-B2).
    static Grid neutral(int rows, int cols, const Rect& basis);

    // Mean lattice spacing, for tolerance and cost bookkeeping.
    Vec spacing() const;

private:
    int _rows, _cols;
    Rect _basis;
    std::vector<GridPoint> _pts;
};

// The lattice rebuild Nuke says it attempts ("GridWarp attempts to modify the grid based
// on the current control point distribution"): the new uniform lattice is laid down on
// `basis` and each of its positions is read off the **old** grid's own surface at the
// same fractional coordinates, so a warp survives a topology change instead of resetting.
//
// The handles it hands out are one third of the old surface's axis partial there, **taken from
// the side that arm points into**, scaled by the ratio of the two grids' parameter cell widths
// (a ratio of cell counts, `(fromCells / toCells)` per axis, and not of pixel spacings - a handle
// is one third of a derivative with respect to the *cell's own* parameter). The side matters
// because a grid line crossing an old grid line has *two* tangents there whenever its two arms
// differ, which is what four independent handle knobs per point means; an arm belongs to the piece
// of line on its own side. Two consequences:
//
//   - A point that lands on an old lattice point keeps that point's four arms (and its position),
//     scaled. A same-topology rebuild is all such points, so setting `rows` / `cols` to the numbers
//     they already hold cannot rewrite one handle knob (F1/F2); and an aligned rebuild - one whose
//     new lattice points fall on old lattice points, which is what halving every cell does: 3x3 ->
//     5x5 and 4x4 -> 7x7 are both a 2x split, 3x3 -> 7x7 a 3x one - is exact, because a cubic
//     restricted to a sub-interval *is* the cubic its two endpoints and two endpoint derivatives
//     describe, and the Coons patch of the four restricted boundaries is the restriction of the Coons
//     patch. Averaging the four corner handles instead misses that by tens of px (F17, F18). A split
//     into halves is dyadic, so it round-trips bit for bit; a split into thirds is not, and is exact
//     only to the last binary digit (F17e).
//   - At the outer edge of the grid an outward arm has no piece of surface on its side, and no cell
//     samples it either, so it mirrors the arm that does exist and continues the curve, instead of
//     picking up a tangent that points inward.
//
// The rebuild reproduces the old grid's *map*, not only its outline: a grid with all four handles
// zeroed traverses each grid line ease-in-out (its controls are [A,A,B,B]), so a rebuild that keeps
// the map hands out handles that encode that easing (F13-F15). Positions and handles come from the
// source grid alone, and `basis` only labels the result, which is what makes a rebuild independent
// of a format change (F16). A source with no cells at all is the only case where the rebuild
// invents handles: it leaves the neutral lattice. (D-53.)
Grid resampleGrid(const Grid& from, int rows, int cols, const Rect& basis);

// ---------------------------------------------------------------------------
// The Transform tab (D-53): a rigid-plus-skew affine over points *and* handles
// ---------------------------------------------------------------------------
// The knobs move whole grids; they are short-hand for what dragging every point would do,
// so the linear part applies to the handles too (a handle is a tangent, not a position).
// Nuke's page does not state the multiplication order of translate/rotate/scale/skew, so the order
// is ours and it is stated here, once: **skew (in `skew_order`), then scale, then rotate about
// `center`, then translate, then the extra matrix** - the same sentence ARCHITECTURE.md's knob table,
// DECISIONS D-60 and the knob help text carry. `affineFor` composes it as R·S·K (leftmost applied
// last), so a scale scales the already-sheared points.
struct Transform {
    double tx, ty;          // translate
    double rotateDeg;       // rotate about `center`
    double sx, sy;          // scale (negative reverses that axis)
    double skewXDeg, skewYDeg;
    bool skewOrderXY;       // Nuke's `skew_order`: XY or YX
    double cx, cy;          // centre of rotation and scale
    bool useMatrix, matrix3x3;
    double m[9];            // the "extra matrix": 2x3 in Nuke, 3x3 where a host offers one

    Transform()
        : tx(0), ty(0), rotateDeg(0), sx(1), sy(1), skewXDeg(0), skewYDeg(0),
          skewOrderXY(true), cx(0), cy(0), useMatrix(false), matrix3x3(false)
    {
        m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 1; m[5] = 0;
        m[6] = 0; m[7] = 0; m[8] = 1;
    }
};

// The affine the transform describes: `map(p)` and `lin(d)` (the linear part, for handles).
struct Affine {
    double a00, a01, tx;
    double a10, a11, ty;

    Affine() : a00(1), a01(0), tx(0), a10(0), a11(1), ty(0) {}

    Vec map(const Vec& p) const { return Vec(a00 * p.x + a01 * p.y + tx,
                                             a10 * p.x + a11 * p.y + ty); }
    Vec lin(const Vec& d) const { return Vec(a00 * d.x + a01 * d.y, a10 * d.x + a11 * d.y); }
    double det() const { return a00 * a11 - a01 * a10; }
};

Affine affineFor(const Transform& t);

// Apply a transform to a grid in place. Returns false when the linear part is singular
// (a collapsed grid cannot be un-collapsed, so the caller keeps the grid it had).
bool applyTransform(Grid& g, const Affine& af);

// ---------------------------------------------------------------------------
// Blending the two grids (D-54)
// ---------------------------------------------------------------------------
// G(t) = (1-t)·S + t·D, point by point, handles included. Positions and handles blend by
// the same rule, which is what makes the four `output` modes corners of (warp, mix) and
// what makes the two passthrough corners exact copies (tests/gw_grid_test.cpp E1-E4).
// The grids must share a topology - `rows`/`cols` are one pair for both grids, as
// GridWarpTracker's `divisions_x`/`divisions_y` are (D-53).
bool blendGrids(const Grid& src, const Grid& dst, double t, Grid& out);

// Bit-for-bit equality of two grids' geometry: same topology, and every position and handle the
// same float. The basis is *not* compared, because it only labels how the points were resolved:
// two grids with the same points describe the same map, which is all the render's copy paths need
// (D-59). A NaN knob makes the grids unequal, which is the safe answer - the copy path is skipped
// and the gather runs on the clamped grid.
bool gridsEqual(const Grid& a, const Grid& b);

// ---------------------------------------------------------------------------
// One cell: the bicubic net
// ---------------------------------------------------------------------------
struct Cell {
    // c[i][j]: i along u (0..3), j along v (0..3). The u-edges are the two horizontal
    // grid cubics, the v-edges the vertical ones, and the 2x2 interior is Coons'.
    Vec c[4][4];
    int r, col;      // which lattice cell this is (row, column)
};

// The net of lattice cell (r,c). Returns false for a cell that does not exist.
bool makeCell(const Grid& g, int r, int c, Cell& out);

// The four controls of one grid line: the two lattice points at the ends, and each end's handle
// pointing along the line (`fromHandle`/`toHandle` are one of the four directions, and a row line
// pairs `kHUPos` with the neighbour's `kHUNeg`, a column line `kHVPos` with `kHVNeg`). `makeCell`
// builds its four boundary cubics with this, and the Viewer overlay's chords join the same four
// points (interact.h, D-78), so what is drawn, what is grabbable and what is rendered all read one
// pairing. With neutral handles the cubic *is* the chord (tests/gw_grid_test.cpp B3).
void edgeCubic(const GridPoint& from, const GridPoint& to, int fromHandle,
               int toHandle, Vec out[4]);

// Evaluate the net and its first parametric derivatives. `du` is dP/du, `dv` is dP/dv.
void evalCell(const Cell& cell, double u, double v, Vec* pos, Vec* du, Vec* dv);

// Conservative bounds: the convex hull property of a Bezier net says the surface cannot
// leave the box of its 16 controls. Also the bounds of the derivative net, which is what
// a resampling radius is derived from.
Rect cellBounds(const Cell& cell);
Vec cellDerivativeMax(const Cell& cell);

// The cell's own maximum stretch, measured on the same (renderRes+1)^2 lattice the
// inverter forward-maps: max over lattice steps of |d(source net)| / |d(operative net)|.
// `src` and `out` are the same lattice cell of two grids (D-55: this is the per-cell
// gather pad a host is asked for). A result within 1e-9 relative of a whole number is
// reported as that whole number, because the radius `gatherTaps` ceils is `base * gain`.
double cellStretch(const Cell& source, const Cell& out, int renderRes);

// The *local* minification gain at one point of the net, as two axis-aligned numbers.
//
// A cell's gain is one number for the whole cell — `cellStretch` is a maximum over the cell — so a
// cell that minifies in one corner and magnifies in another blurs the magnifying corner too. That is
// the "filtering works per grid patch, not per pixel" defect (D-87), and this is the per-pixel half
// of the fix. `inDu`/`inDv` are the derivatives of the *input* net (Source or Dest) at the parameter
// pair the pixel samples through; `opDu`/`opDv` are those of the net the pixel is being resampled
// *through* — the operative grid, which is what the inverter's parameters belong to. The ratio
//
//     A = J_input · J_operative⁻¹
//
// is the linear map taking a step in the operative net's tangent plane to the step it makes in the
// input net's, i.e. how much this particular pixel's neighbourhood is stretched once the warping of
// the operative net itself is divided out. `gainX`/`gainY` are the sums of the absolute values of
// that map's two rows: the footprint of a pixel gathered through a *separable* kernel is the
// axis-aligned bounding box of the cell's local image, and a separable gather can only blur along the
// x and y axes, so one number per axis is exactly what a separable gather can use — a rotated
// minifying lattice therefore gets no *new* blur, because its axis-aligned footprint is bounded by
// the cell's own gain (D-87). Both are clamped to `cellGain`, the cell's own gain: the render's
// gather window, its RoI padding and its Source requests are all sized from the cell gain
// (render.h's `CellPlanEntry`), so a local gain allowed past it would read pixels outside the window
// the host allocated, where `SourceWindow` edge-extends instead of reading real data. `cellGain` <= 1
// (no minification anywhere in the cell) answers 1,1: a magnifying cell does exactly what it did
// before, at no cost.
//
// A folded or degenerate cell has no Jacobian inverse to divide by — the answer is then `cellGain` on
// both axes, which is what the render used before this function existed.
void pixelGain(const Vec& inDu, const Vec& inDv, const Vec& opDu, const Vec& opDv,
               double cellGain, double& gainX, double& gainY);

// ---------------------------------------------------------------------------
// The inversion lattice (D-55)
// ---------------------------------------------------------------------------
struct Hit {
    int r, c;          // lattice cell
    double u, v;       // parameter inside it
    Vec pos;           // the net's answer at (u,v)
    // |query - pos| at the answer, canonical px. Reported on a refusal too, but a refusal is a
    // coverage statement and not a measurement of an error, so read it in two cases: when a
    // candidate *was* evaluated it is the distance from the query to the single closest point this
    // grid can reach (finite, and the caller can report or clamp to it); when the search never
    // reached the surface at all - no forward-mapped sample inside the bucket window, no cell hull
    // box containing the query - it stays at the +infinity `find` initialises it to, which means
    // "nothing was reachable" and must be tested with std::isinf rather than used as a number.
    double residual;
    double jac;        // det [du dv] at the answer; its sign says whether the cell flipped
    // dP/du and dP/dv at the answer, the two pieces `pixelGain` divides the input net's own
    // derivatives by. `jac` is their determinant, so this is not new information — it is the same
    // Jacobian kept in the form a per-pixel gain needs, and it is free because `refine` already has
    // it at the root it reports. Zero on a refusal, which never reaches a root.
    Vec du, dv;
};

// Forward-maps every cell onto a (renderRes+1)^2 sample lattice, buckets the samples in
// output space, and answers "which (u,v) of this grid lands on this canonical point".
//
// Deterministic by construction: candidates come from a fixed bucket scan, the starting
// sample is the nearest one, Newton's steps are fixed arithmetic, and the winner is the
// candidate whose root is closest with ties broken by (cell, sample index). Two runs of
// the same grid give the same answers pixel for pixel, which is what the offscreen
// probe's "no flicker between two renders of the same frame" claim rests on.
class Inverter {
public:
    Inverter();
    explicit Inverter(const Grid& g, int renderRes);

    void build(const Grid& g, int renderRes);
    bool built() const { return _built; }
    int renderRes() const { return _res; }
    const Grid& grid() const { return _grid; }

    // The lattice's own geometry, for cost and accuracy reporting.
    std::size_t cellCount() const { return _cells.size(); }
    std::size_t sampleCount() const { return _samples.size(); }
    // Mean step of the forward-mapped sample lattice, canonical px: this is the bucket
    // size, and the scale `tolerance()` is expressed against.
    double meanLatticeStep() const { return _meanStep; }

    // The bucket grid's actual cell size, which `buildBuckets` may have grown to a power-of-two
    // multiple of the mean step to keep the bucket count near the sample count. A caller that wants
    // "every cell that could have written near this point" uses this as its slack: a Newton answer
    // is only guaranteed to be within `tolerance()` of the query, and the cell that produced it can
    // have its hull box further away than that by up to one bucket step (render.h's RoI requests).
    double bucketSize() const { return _bucketSize; }

    // Writes the best answer into `out` and returns true when a root was found within
    // `tolerance()`. Coverage *is* this predicate: an output pixel the grid does not map anywhere
    // to finds no root, and the plugin composites `background` there instead.
    //
    // `out` is always written, and its fields are always in range or the sentinel: `residual`
    // starts at +infinity and is only replaced once a candidate has actually been evaluated, so a
    // caller that wants to report *how far* a refused pixel was from the surface has to test
    // `std::isfinite(out.residual)` first. Every other field is a finite number in a cell and a
    // parameter, including on a refusal (Hit::residual explains the two kinds of refusal).
    bool find(double x, double y, Hit& out) const;

    // What "found" means, in canonical px: reported so a test can state it and a sweep
    // can show convergence against it.
    double tolerance() const { return _tol; }

private:
    struct Sample { double x, y; int cell; int i, j; };
    struct Cand { double d2; int sample; };

    void buildBuckets();
    bool refine(int cellIndex, double u0, double v0, double targetX, double targetY,
                Hit& out) const;
    void collectCandidates(double x, double y, int radius) const;

    Grid _grid;
    int _res;
    bool _built;
    std::vector<Cell> _cells;
    std::vector<Rect> _cellBoxes;      // each cell's hull box, for the fallback start
    std::vector<Sample> _samples;
    std::vector<int> _sorted;          // sample indices, grouped by bucket
    std::vector<int> _bucketStart;     // counting sort: bucket -> range of _sorted
    double _bucketSize, _bx0, _by0;
    int _bcols, _brows;
    double _tol, _meanStep, _minDet;
    mutable std::vector<Cand> _cands;  // candidate scratch, reused
};

// Bounds of the whole grid's surface (union of its cells' hulls), the number the plugin
// puts on kOfxImageEffectPropRegionOfDefinition for `output` = morph / warped modes.
Rect gridFootprint(const Grid& g);

// The per-cell plan a render runs from: for each cell, the box its output pixels land in,
// the boxes sampling the Source and Dest inputs reaches, and the factor the resampling
// radius owes those inputs. The (u,v) domain of a cell is the unit square whichever way
// it is warped, so the gather boxes are exactly the source and destination cells' own
// hulls - no inverse mapping needed to state them (D-55).
struct CellPlan {
    int r, c;
    Rect outBox;      // where this cell lands in output space (operative grid's hull)
    Rect srcBox;      // where sampling the Source input reaches (source cell's hull)
    Rect dstBox;      // ... and the Dest input (destination cell's hull)
    double srcStretch, dstStretch;   // max |d input| / |d operative| over the sample lattice
    double srcGain, dstGain;         // max(1, stretch): multiply the filter's base radius
};
std::vector<CellPlan> planCells(const Grid& src, const Grid& dst, const Grid& out, int renderRes);

// Clamps shared by the descriptors and the model (D-53).
int clampPoints(int n);
int clampRenderRes(int n);
double clampUnit(double v);

} // namespace gw

#endif // OFXLAB_GW_GRID_H
