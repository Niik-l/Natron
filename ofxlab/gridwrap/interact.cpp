// ---------------------------------------------------------------------------
// The Viewer overlay's behaviour, host-free (D-72, D-78) — see interact.h
// ---------------------------------------------------------------------------
// Everything here is float math on `gw::Grid` + `gw::Affine`: no OFX, no GL, no host. That is what lets
// the same code that hit-tests a drag decide how flat a curve has to be before the shell may draw a chord
// instead of the curve itself, and it is why the interact does not know what a `Double2DParam` is: the
// shell in gridwrap.cpp does the translating in both directions.

#include "interact.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gw {

namespace {

// The arm a grid line *leaves* a point through, and the arm its neighbour arrives through. The table
// order is the handle order (`kHVNeg`=down, `kHVPos`=up, `kHUNeg`=left, `kHUPos`=right — grid.h), and
// it is the same pairing `makeCell` uses for a cell's four boundaries (grid.cpp), so the curve the
// overlay strokes and the boundary the render warps by read the same two arms.
int departingHandle(bool horizontal) { return horizontal ? kHUPos : kHVPos; }
int arrivingHandle(bool horizontal) { return horizontal ? kHUNeg : kHVNeg; }

bool handlesAreDrawn(const HitOptions& opt, const Selection& sel)
{
    return sel.valid() && opt.radiusPixels > 0;
}

// Whether the panel's `showSourceGrid` / `showDestinationGrid` toggle lets this family be *picked*
// (D-84). The overlay still draws a hidden family — dimmer, so the grid you turned off has not
// vanished from the picture — but a press must not grab a point or an arm of it, or the toggle would be
// a drawing hint rather than a visibility rule. Used as the gate on both the candidate loops, so the
// drawn grid and the clickable grid are the same grid.
bool familyIsVisible(const HitOptions& opt, int family)
{
    return family == kFamilyDst ? opt.showDestination : opt.showSource;
}

// How far a drawn vertex is from the pointer, in units of the per-axis grab: `(dx/rx)² + (dy/ry)²`, so
// `<= 1` means *inside* the radius, and the per-axis radii are what make a non-square PAR hit
// circularly on the screen instead of elliptically in canonical space. A non-finite pen or vertex has no
// distance, so it reports the worst number there is: it fails the `<= 1` test and sorts last, which is
// how a NaN pointer or a NaN knob value is refused without a separate branch in every loop.
double normalisedErr(const Vec& pen, const Vec& p, const Vec& radius)
{
    if (!std::isfinite(pen.x) || !std::isfinite(pen.y) || !std::isfinite(p.x) || !std::isfinite(p.y))
        return std::numeric_limits<double>::max();
    const double dx = (p.x - pen.x) / radius.x;
    const double dy = (p.y - pen.y) / radius.y;
    return dx * dx + dy * dy;
}

} // namespace

Vec toFractions(const Vec& canonical, const Rect& basis)
{
    // The inverse of resolvePoint's two products (grid.cpp). An empty basis has nothing to divide by;
    // callers guard on basis.empty() before they ask for a fraction.
    const double w = basis.width(), h = basis.height();
    if (w == 0.0 || h == 0.0)
        return Vec(0.0, 0.0);
    return Vec((canonical.x - basis.x0) / w, (canonical.y - basis.y0) / h);
}

Vec fromFractions(const Vec& frac, const Rect& basis)
{
    return Vec(basis.x0 + frac.x * basis.width(), basis.y0 + frac.y * basis.height());
}

Vec offsetFractions(const Vec& offset, const Rect& basis)
{
    // The read-back of resolvePoint's two products (grid.cpp): a handle is stored as
    // `Vec(f.h[i][0] * width, f.h[i][1] * height)`, an offset in canonical units with **no basis-origin
    // term in it**, and shrinkPoint divides by width/height and subtracts nothing. So dividing is the
    // whole inverse here — and `toFractions`, which subtracts the origin, would fold the grid's own
    // position into every handle knob. An empty basis has nothing to divide by, and callers guard on
    // basis.empty() first.
    const double w = basis.width(), h = basis.height();
    if (w == 0.0 || h == 0.0)
        return Vec(0.0, 0.0);
    return Vec(offset.x / w, offset.y / h);
}

double clampKnobValue(double v)
{
    if (!(v == v))
        return 0.0;   // a NaN knob is not a value a drag may extend; the drag solve refuses it first
    const double lim = double(kInteractFracRange);
    if (v > lim)
        return lim;
    if (v < -lim)
        return -lim;
    return v;
}

Vec hitRadiusFor(const HitOptions& opt, const Vec& spacing)
{
    // The screen radius in canonical units on each axis, capped so a radius can never swallow the cell
    // its own control sits in. A cap of zero (a single-column lattice, a zero pixel scale) means that
    // axis is not measurable, which is what `hitTest` refuses on.
    const double r = double(opt.radiusPixels);
    return Vec(std::min(r * opt.pixelScaleX, kInteractHitSpacingFrac * spacing.x),
               std::min(r * opt.pixelScaleY, kInteractHitSpacingFrac * spacing.y));
}

bool inverseAffine(const Affine& af, Affine& out)
{
    // applyTransform refuses a grid whose linear part is singular (grid.cpp), so the same bound makes
    // "this grid has an inverse map" and "this grid is renderable" one statement: a drag is refused
    // exactly where the render would have refused the transform.
    const double det = af.det();
    if (det == 0.0 || !std::isfinite(det) || std::fabs(det) < 1e-12)
        return false;

    const double inv = 1.0 / det;
    out.a00 = af.a11 * inv;
    out.a01 = -af.a01 * inv;
    out.a10 = -af.a10 * inv;
    out.a11 = af.a00 * inv;
    out.tx = -(out.a00 * af.tx + out.a01 * af.ty);
    out.ty = -(out.a10 * af.tx + out.a11 * af.ty);
    return true;
}

int flattenCubic(const Vec cp[4], double tolerance, Vec* out, int max)
{
    if (!out || max <= 0)
        return 0;
    if (max == 1)
    {
        out[0] = cp[0];
        return 1;
    }

    // A control that is not a number makes every part of the flatness test meaningless — including
    // where the endpoints land. Hand back the two endpoints as the caller read them and let it stroke
    // what it has; inventing a subdivision of a NaN curve only hides the bad knob that produced it.
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(cp[i].x) || !std::isfinite(cp[i].y))
        {
            out[0] = cp[0];
            out[1] = cp[3];
            return 2;
        }

    // How far the two interior controls stand from the chord line. `d.cross()` is not part of gw::Vec
    // (grid.h), so the perpendicular distance comes from the 2D wedge product over the chord length.
    const Vec d = cp[3] - cp[0];
    const double len2 = d.len2();
    double dev = 0.0;
    if (len2 > 0.0)
    {
        const double inv = 1.0 / std::sqrt(len2);
        for (int i = 1; i < 3; ++i)
        {
            const Vec a = cp[i] - cp[0];
            const double perp = std::fabs(a.x * d.y - a.y * d.x) * inv;
            if (perp > dev)
                dev = perp;
        }
    }
    else
    {
        // The chord has collapsed to a point: measure the departure from that point instead, which is
        // the only flatness a zero-length edge can have.
        for (int i = 1; i < 3; ++i)
        {
            const double l = (cp[i] - cp[0]).len();
            if (l > dev)
                dev = l;
        }
    }

    const int cap = max - 1;   // segments: `max` points, and out[0] and out[max-1] are the endpoints

    // No usable tolerance means no flatness promise to keep, so take the cap; a cubic whose bulge is
    // already inside the tolerance needs no subdivision at all and stays the chord.
    int n;
    if (!std::isfinite(tolerance) || tolerance <= 0.0)
        n = 64;
    else if (dev == 0.0 || dev <= tolerance)
        n = 1;
    else
    {
        const double want = std::sqrt(0.75 * dev / tolerance);   // sagitta of an n-piece cubic: 0.75D/n²
        n = int(std::ceil(want));
        if (n < 2)
            n = 2;
        if (n > 64)
            n = 64;
    }
    if (n > cap)
        n = cap;   // a `max` of 2 can only deliver the chord, and that is what it gets

    // Endpoints first and last, exactly as written, so a caller that strokes the result does not have
    // to know how many pieces it came back with. The interior points are the Bernstein form of the
    // cubic at t = i/n.
    out[0] = cp[0];
    out[n] = cp[3];
    for (int i = 1; i < n; ++i)
    {
        const double t = double(i) / double(n);
        const double s = 1.0 - t;
        const double b0 = s * s * s;
        const double b1 = 3.0 * s * s * t;
        const double b2 = 3.0 * s * t * t;
        const double b3 = t * t * t;
        out[i].x = b0 * cp[0].x + b1 * cp[1].x + b2 * cp[2].x + b3 * cp[3].x;
        out[i].y = b0 * cp[0].y + b1 * cp[1].y + b2 * cp[2].y + b3 * cp[3].y;
    }
    return n + 1;
}

Interactor::Interactor(const Grid& src, const Grid& dst, const Rect& basis,
                       const Affine& srcAf, const Affine& dstAf)
    : _src(src), _dst(dst), _basis(Rect::normalised(basis)), _af(), _inv(), _invertible()
{
    _af[kFamilySrc] = srcAf;
    _af[kFamilyDst] = dstAf;

    // The topology both families are drawn and grabbed with. `rows`/`cols` are one knob pair for both
    // lattices (D-53), and `Grid::at` is unchecked, so the shared count takes the *smaller* of the two
    // grids: a caller that handed in mismatched grids gets a lattice both of them contain rather than
    // an out-of-range read.
    _rows = std::min(_src.rows(), _dst.rows());
    _cols = std::min(_src.cols(), _dst.cols());

    for (int f = 0; f < kFamilyCount; ++f)
        _invertible[f] = inverseAffine(_af[f], _inv[f]);

    // The pitch the Viewer shows: the knob-space pitch of the lattice (the basis divided by the cell
    // count) pushed through the family's linear part, which is how a scale or skew changes how far
    // apart two control points *look*. The two families can be drawn through different affines, so the
    // tighter one governs the shared hit radius: the point is to stop a radius from swallowing a whole
    // cell, and it has to do that for whichever lattice is denser on screen.
    const double pitchX = _cols > 1 ? _basis.width() / double(_cols - 1) : 0.0;
    const double pitchY = _rows > 1 ? _basis.height() / double(_rows - 1) : 0.0;
    _spacing = Vec(1e300, 1e300);
    for (int f = 0; f < kFamilyCount; ++f) {
        const double sx = _af[f].lin(Vec(pitchX, 0.0)).len();
        const double sy = _af[f].lin(Vec(0.0, pitchY)).len();
        _spacing.x = std::min(_spacing.x, sx);
        _spacing.y = std::min(_spacing.y, sy);
    }
    if (!std::isfinite(_spacing.x))
        _spacing.x = 0.0;
    if (!std::isfinite(_spacing.y))
        _spacing.y = 0.0;
}

const Grid& Interactor::gridFor(int family) const
{
    return family == kFamilyDst ? _dst : _src;
}

const Affine& Interactor::affineFor(int family) const
{
    return _af[family == kFamilyDst ? kFamilyDst : kFamilySrc];
}

bool Interactor::inTopology(int row, int col) const
{
    return row >= 0 && col >= 0 && row < _rows && col < _cols;
}

Vec Interactor::drawnPoint(int family, int row, int col) const
{
    return affineFor(family).map(gridFor(family).at(row, col).pos);
}

Vec Interactor::drawnTip(int family, int row, int col, int handle) const
{
    const GridPoint& p = gridFor(family).at(row, col);
    return affineFor(family).map(p.pos + p.h[handle]);
}

std::vector<DrawnEdge> Interactor::latticeEdges(int family) const
{
    std::vector<DrawnEdge> out;

    // Row lines first, then column lines: the order the strokes are issued in. Each edge is one grid
    // line's four controls in the order they are stroked — the departing handle of the near point, the
    // arriving handle of the far one — and the pairing is the one `latticeEdgeCubic` uses, so the drawn
    // curve and the warped cell boundary are built from the same two arms. The endpoints are also the hit
    // candidates (see interact.h: the drawing and the grab read one control list, never a point along a
    // curve).
    const std::size_t n = std::size_t(_rows) * (_cols > 0 ? _cols - 1 : 0)
                        + std::size_t(_cols) * (_rows > 0 ? _rows - 1 : 0);
    out.reserve(n);

    for (int r = 0; r < _rows; ++r) {
        for (int c = 0; c + 1 < _cols; ++c) {
            DrawnEdge e;
            e.horizontal = true;
            e.cp[0] = drawnPoint(family, r, c);
            e.cp[1] = drawnTip(family, r, c, departingHandle(true));
            e.cp[2] = drawnTip(family, r, c + 1, arrivingHandle(true));
            e.cp[3] = drawnPoint(family, r, c + 1);
            out.push_back(e);
        }
    }
    for (int c = 0; c < _cols; ++c) {
        for (int r = 0; r + 1 < _rows; ++r) {
            DrawnEdge e;
            e.horizontal = false;
            e.cp[0] = drawnPoint(family, r, c);
            e.cp[1] = drawnTip(family, r, c, departingHandle(false));
            e.cp[2] = drawnTip(family, r + 1, c, arrivingHandle(false));
            e.cp[3] = drawnPoint(family, r + 1, c);
            out.push_back(e);
        }
    }
    return out;
}

std::vector<PolyPoint> Interactor::latticePoints(int family) const
{
    std::vector<PolyPoint> out;
    out.reserve(size_t(_rows) * _cols);
    for (int r = 0; r < _rows; ++r) {
        for (int c = 0; c < _cols; ++c) {
            PolyPoint pp;
            pp.p = drawnPoint(family, r, c);
            pp.row = r;
            pp.col = c;
            pp.handle = -1;
            out.push_back(pp);
        }
    }
    return out;
}

std::vector<Segment> Interactor::handleArms(int family, int row, int col) const
{
    std::vector<Segment> out;
    if (!inTopology(row, col))
        return out;

    const Vec p = drawnPoint(family, row, col);
    for (int h = 0; h < kHandleCount; ++h) {
        Segment s;
        s.a = p;
        s.b = drawnTip(family, row, col, h);
        s.horizontalHandle = (h == kHUNeg || h == kHUPos);
        out.push_back(s);
    }
    return out;
}

std::vector<PolyPoint> Interactor::handleTips(int family, int row, int col) const
{
    std::vector<PolyPoint> out;
    if (!inTopology(row, col))
        return out;

    for (int h = 0; h < kHandleCount; ++h) {
        PolyPoint pp;
        pp.p = drawnTip(family, row, col, h);
        pp.row = row;
        pp.col = col;
        pp.handle = h;
        out.push_back(pp);
    }
    return out;
}

bool Interactor::latticeEdgeCubic(int family, int row, int col, bool horizontal, Vec out[4]) const
{
    if (!inTopology(row, col))
        return false;
    if (horizontal) {
        if (col + 1 >= _cols)
            return false;
    } else if (row + 1 >= _rows) {
        return false;
    }

    const Grid& g = gridFor(family);
    const GridPoint& from = g.at(row, col);
    const GridPoint& to = g.at(horizontal ? row : row + 1, horizontal ? col + 1 : col);
    edgeCubic(from, to, departingHandle(horizontal), arrivingHandle(horizontal), out);
    return true;
}

bool Interactor::transformIsInvertible(int family) const
{
    return _invertible[family == kFamilyDst ? kFamilyDst : kFamilySrc];
}

int Interactor::hitCandidates(const Vec& pen, const Selection& sel, const HitOptions& opt,
                             Control* out, int max) const
{
    if (!out || max <= 0)
        return 0;

    // The radius in canonical units per axis: the screen radius scaled by what a screen pixel *is* on
    // that axis (Natron's two `pixelScale` values are `1/(zoomFactor * zoomAspectRatio)` and
    // `1/zoomFactor` — Gui/ZoomContext.h:123-137 — which is why a non-square PAR needs them per axis),
    // then capped at a fraction of the drawn lattice pitch so zooming out far enough does not make
    // every control compete for the same pixel. One radius for both families, because `_spacing` is
    // already the tighter of the two lattices (see the constructor).
    const Vec radius = hitRadiusFor(opt, _spacing);
    if (radius.x <= 0.0 || radius.y <= 0.0 || _basis.empty())
        return 0;   // no measurable screen, or no basis to resolve a control against

    std::vector<Control> cands;
    cands.reserve(size_t(_rows) * _cols * kFamilyCount +
                  kHandleCount * std::max(1, sel.count()));

    // Every lattice point of **both** families inside the grab. Rank 0: a point is the crossing of two
    // grid lines, which is what a pointer near a lattice is aiming at.
    //
    // A family the panel has hidden contributes nothing here at all (D-84): the overlay draws it dimmer,
    // but its points are not controls, so `show_src_grid` / `show_dst_grid` decide what a press can land
    // on and not just what the picture shows.
    for (int f = 0; f < kFamilyCount; ++f) {
        if (!familyIsVisible(opt, f))
            continue;
        for (int r = 0; r < _rows; ++r) {
            for (int c = 0; c < _cols; ++c) {
                const Vec p = drawnPoint(f, r, c);
                const double e2 = normalisedErr(pen, p, radius);
                if (e2 > 1.0)
                    continue;                 // outside the grab

                Control k;
                k.found = true;
                k.rank = kRankPoint;
                k.family = f;
                k.row = r;
                k.col = c;
                k.handle = -1;
                k.point = p;
                k.tip = p;
                k.err = e2;
                cands.push_back(k);
            }
        }
    }

    // Best-first, in the order a click resolves it: rank, then distance, then — and only because the
    // first two are equal — the family the selection is already in, then the slot. The family term sits
    // *after* distance on purpose: a point that is genuinely nearer in the other family wins, and the
    // preference only decides between two controls drawn at the same place, which is the coincident
    // src/dst twin case (D-78). Ties past that are broken by slot so two runs return the same list.
    const int selFamily = sel.family;
    struct Cmp {
        int selFamily;
        bool operator()(const Control& a, const Control& b) const
        {
            if (a.rank != b.rank)
                return a.rank < b.rank;
            if (a.err != b.err)
                return a.err < b.err;
            const bool ina = (a.family == selFamily), inb = (b.family == selFamily);
            if (ina != inb)
                return ina;
            if (a.row != b.row)
                return a.row < b.row;
            if (a.col != b.col)
                return a.col < b.col;
            if (a.family != b.family)
                return a.family < b.family;
            return a.handle < b.handle;
        }
    };
    const Cmp byRank = { selFamily };
    std::sort(cands.begin(), cands.end(), byRank);

    // The arms of **every** selected point, in the selected family only — because those are the arms that
    // are drawn (see `handleArms` and interact.h), and an undrawn handle is not grabbable. That is Nuke's
    // order too: you grab a point, its arms appear, then you drag one of them. Whether each point's arms
    // are *also* candidates is decided by the two rules in interact.h, which is what stops the spacing
    // clamp from making a point's own arms ungrabbable at high zoom (D-78). With the set widened to
    // several points (D-85) the rules are applied per point, because each one draws its own four arms and
    // each one is a control a press could be landing on; and the whole family is gated on visibility, so
    // the arms of a hidden grid are not pickable (D-84).
    const bool familySelectable = handlesAreDrawn(opt, sel) &&
                                  sel.family >= 0 && sel.family < kFamilyCount &&
                                  familyIsVisible(opt, sel.family);

    struct ArmOffer { int row, col; bool offer; };
    std::vector<ArmOffer> offers;
    if (familySelectable) {
        for (int si = 0; si < sel.count(); ++si) {
            const int r = sel.at(si).row, c = sel.at(si).col;
            if (!inTopology(r, c))
                continue;

            // The nearest of this point's four arm tips, measured whether or not it is inside the grab:
            // the comparison below needs "is an arm nearer than this point", and a point outside the grab
            // is not in the list at all, so the answer comes from this measurement and not from the arm
            // candidates.
            double bestArmErr = std::numeric_limits<double>::max();
            for (int h = 0; h < kHandleCount; ++h)
                bestArmErr = std::min(bestArmErr,
                                      normalisedErr(pen, drawnTip(sel.family, r, c, h), radius));

            ArmOffer off = { r, c, true };
            if (!cands.empty()) {
                Control& head = cands[0];
                if (head.handle == -1 && head.family == sel.family && head.row == r && head.col == c) {
                    if (bestArmErr < head.err)
                        head.rank = kRankArm;   // demoted: the pointer is on its arm, not on it
                    else
                        off.offer = false;      // a press on the point is a press on the point
                }
            }
            offers.push_back(off);
        }
    }

    for (size_t oi = 0; oi < offers.size(); ++oi) {
        if (!offers[oi].offer)
            continue;

        const int f = sel.family;
        const int r = offers[oi].row, c = offers[oi].col;
        const Vec p = drawnPoint(f, r, c);
        for (int h = 0; h < kHandleCount; ++h) {
            const Vec tip = drawnTip(f, r, c, h);
            const double e2 = normalisedErr(pen, tip, radius);
            if (e2 > 1.0)
                continue;

            Control k;
            k.found = true;
            k.rank = kRankArm;
            k.family = f;
            k.row = r;
            k.col = c;
            k.handle = h;
            k.point = p;              // the arm belongs to this point, and its drag writes that slot
            k.tip = tip;
            k.err = e2;
            cands.push_back(k);
        }
    }
    if (!offers.empty())
        std::sort(cands.begin(), cands.end(), byRank);

    const int n = int(std::min(cands.size(),
                               size_t(std::min(max, int(kInteractMaxCandidates)))));
    for (int i = 0; i < n; ++i)
        out[i] = cands[size_t(i)];
    return n;
}

Control Interactor::hitTest(const Vec& pen, const Selection& sel, const HitOptions& opt) const
{
    Control cands[kInteractMaxCandidates];
    const int n = hitCandidates(pen, sel, opt, cands, int(kInteractMaxCandidates));
    if (n <= 0)
        return Control();   // the pointer is on nothing: `found` false, no slot, no drag
    return cands[0];
}

const Control* Interactor::coincidentTwin(const Control* cands, int count, const Selection& sel)
{
    if (!cands || count <= 0)
        return 0;

    const Control& head = cands[0];

    // A press on an arm is a press on an arm: nothing about it means "switch which grid I am editing".
    if (head.handle != -1)
        return 0;

    // And there has to be something to flip *away from*: the head must be the point we are already
    // holding, otherwise the press simply selects it and there is no second reading of the same spot.
    if (!head.selectedBy(sel))
        return 0;

    // The head *is* the selected point, so a click here can mean the other family's point in the same
    // slot — D-53 stamps both lattices from one `rows`/`cols`/basis, so a fresh node draws them at the
    // same place, and re-clicking that place is the only way src<->dst flips in a host that routes no
    // key press to a plugin overlay (D-78).
    //
    // Scan the whole list rather than stopping at the first rank or distance change: the demotion rule
    // (see `kRankArm`) can put a rank-1 entry of the *selected* family ahead of a rank-0 twin, so the
    // order is no longer strictly by rank and distance once a point has been demoted. The slot match is
    // what makes a twin a twin — the neighbouring cell's point is a different (row, col), however close
    // it sits — and the distance bound is what refuses a pair of grids warped apart.
    const double limit = std::sqrt(head.err) + kInteractTwinRadiusFrac;
    for (int i = 1; i < count; ++i) {
        const Control& c = cands[size_t(i)];
        if (c.handle != -1)
            continue;
        if (c.family == head.family)
            continue;
        if (c.row != head.row || c.col != head.col)
            continue;
        if (std::sqrt(c.err) > limit)
            continue;
        return &c;
    }
    return 0;
}

Drag Interactor::dragFor(const Control& ctl, const Selection& sel,
                         const Vec& canonicalPointer, bool mirrorFree) const
{
    Drag d;
    if (!ctl.found || !inTopology(ctl.row, ctl.col))
        return d;
    if (_basis.empty())
        return d;   // fractions of nothing
    if (!_invertible[ctl.family])
        return d;   // a collapsed transform has no inverse map, so the pointer cannot be solved back

    const Grid& g = gridFor(ctl.family);
    const Affine& af = affineFor(ctl.family);
    const GridPoint& gp = g.at(ctl.row, ctl.col);

    // The pointer, in the family's own untransformed space: the law a drag rests on. The Viewer shows
    // `af.map(knob point)`, so the knob point the pointer names is `inv.map(pointer)` — the same
    // affine the render applies on the way out, inverted.
    const Vec target = _inv[ctl.family].map(canonicalPointer);
    if (!std::isfinite(target.x) || !std::isfinite(target.y))
        return d;

    d.family = ctl.family;
    d.row = ctl.row;
    d.col = ctl.col;
    d.handle = ctl.handle;

    if (ctl.handle == -1) {
        // A lattice point moves to the pointer, and *both* components of its position knob move with
        // it: the knob is one `Double2D`, and a position is a pair of basis fractions (D-53).
        Vec frac = toFractions(target, _basis);
        frac.x = clampKnobValue(frac.x);
        frac.y = clampKnobValue(frac.y);
        d.writeValue = frac;
        d.pointFrac = frac;
        d.drawnPos = af.map(fromFractions(frac, _basis));
        d.changed = true;

        // A selected point drags the **whole set** (D-85), and it drags it as a *move*: every other
        // selected point keeps its own position plus the grabbed point's delta, which is what preserves
        // the shape of the selection. Sending every point to the pointer instead would collapse the set
        // onto one spot. The delta is measured in the family's untransformed space, because that is the
        // space the knobs hold; the clamps are per point, so a point that runs into the ±10 wall stops
        // there while its neighbours keep moving.
        //
        // A point *outside* the selection moves on its own — that is the pre-multi-select path, and it
        // is also what happens when a press lands on a point that happens not to be in the set.
        if (sel.family == ctl.family && sel.contains(ctl.row, ctl.col) && sel.count() > 1) {
            const Vec delta = target - gp.pos;
            for (int si = 0; si < sel.count(); ++si) {
                const int r = sel.at(si).row, c = sel.at(si).col;
                if (r == ctl.row && c == ctl.col)
                    continue;               // the primary write already covers it
                if (!inTopology(r, c))
                    continue;               // the set was reconciled against the stamp, but be exact

                Vec other = toFractions(g.at(r, c).pos + delta, _basis);
                other.x = clampKnobValue(other.x);
                other.y = clampKnobValue(other.y);

                DragWrite w;
                w.family = ctl.family;
                w.row = r;
                w.col = c;
                w.handle = -1;
                w.value = other;
                w.mirrored = false;
                d.extraWrites.push_back(w);
            }
        }
        return d;
    }

    // A handle: the point stays, and the arm becomes `solved pointer - point`, expressed as the two
    // fractions the knob holds — x over the basis **width**, y over the basis **height**, because
    // `resolvePoint` scales the u-arms by width and the v-arms by height. So a horizontal drag is a
    // width fraction and a vertical drag a height fraction, which is the same split `kHandleSpecs`
    // names by knob label (gridwrap.cpp) and D-53 pins.
    const Vec arm = target - gp.pos;
    // `arm` is already an offset, not a position, so it goes through `offsetFractions`: that is the
    // inverse of `resolvePoint`'s products and of `shrinkPoint`, which subtracts no origin.
    Vec frac = offsetFractions(arm, _basis);
    frac.x = clampKnobValue(frac.x);
    frac.y = clampKnobValue(frac.y);
    d.writeValue = frac;
    d.pointFrac = toFractions(gp.pos, _basis);   // unchanged
    d.drawnPos = af.map(gp.pos);
    d.changed = true;

    // The mirror (D-83): the opposite arm of the same point takes the exact negative of what this one
    // now holds, so the cubic stays smooth through the point — a Bézier point whose two arms on one
    // axis are negatives of each other has its incoming and outgoing tangents collinear. Negated *after*
    // the clamps, so the pair stays symmetric at the ±10 wall too instead of drifting apart there.
    // `handle ^ 1` is the opposite arm of the same axis: kHVNeg<->kHVPos, kHUNeg<->kHUPos (grid.h's
    // order), and the perpendicular axis is left alone because a u-arm and a v-arm are different axes of
    // one offset, not a pair that has to balance.
    //
    // `mirrorFree` — Ctrl held — skips this: that arm then moves freely, which is how a corner is made.
    if (!mirrorFree) {
        DragWrite w;
        w.family = ctl.family;
        w.row = ctl.row;
        w.col = ctl.col;
        w.handle = ctl.handle ^ 1;
        w.value.x = -frac.x;
        w.value.y = -frac.y;
        w.mirrored = true;
        d.extraWrites.push_back(w);
    }
    return d;
}

} // namespace gw
