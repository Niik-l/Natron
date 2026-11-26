#ifndef OFXLAB_GRIDWRAP_INTERACT_H
#define OFXLAB_GRIDWRAP_INTERACT_H

// ---------------------------------------------------------------------------
// The Viewer overlay's behaviour, host-free (D-72, D-78)
// ---------------------------------------------------------------------------
// The overlay has to answer four questions on a click: which control is under the pointer, where a
// drag moves it, which knob value that write is, and what the lattice looks like as lines. All four
// are float math on `Grid` + `Affine`, so they are here rather than in gridwrap.cpp, whose overlay
// shell turns them into GL strokes on one side and `Double2DParam` reads and writes on the other.
//
// What is *not* here is the drawing and the knobs: the overlay shell emits these vertices through GL
// and reads/writes the 640 lattice slots through OFX. The rule is the one ARCHITECTURE.md §4 states
// for the other plugins — the core is pure, the plugin translates events in and geometry out. The
// selection is the shell's too (see "The selection" below), because the interact is rebuilt from the
// knobs on every event and has no business holding state a host can change under it.
//
// Coordinate frames, and the one conversion the interact adds to the render's:
//   - canonical OFX image coordinates: x right, y **up** (grid.h). A pen position arrives in them
//     (`kOfxInteractPropPenPosition`, ofxsInteract.cpp:127-129), and a knob value is a fraction of the
//     **grid basis** over them (D-53, D-62).
//   - the Viewer's own origin is bottom-left too: Natron reports `kOfxHostNativeOriginBottomLeft`
//     (`Engine/OfxHost.cpp:315`), and a pen position reaches the plugin through
//     `ZoomContext::toZoomCoordinates`, which is the same canonical space the overlay strokes are
//     issued in (`Gui/ViewerGL.cpp:1907` feeding `:660-740`, which then calls `drawOverlays` at
//     `:725`). So no axis flips anywhere, and no upstream-node transform either: the macro that would
//     apply one, `NATRON_TRANSFORM_AFFECTS_OVERLAYS`, is not defined anywhere in Natron (D-78).
//   - what the Viewer *shows* is `affineFor(xform)` applied to the untransformed lattice, because the
//     render maps through that same affine (render.cpp's `xform.map(makeCell(..))`). A drag therefore
//     solves `control = affine⁻¹(pointer)` — the inverse `gw::Affine` does not carry, so this file
//     computes it, with the same determinant bound `applyTransform` uses to refuse a collapsed grid.
//
// The lattice it edits is the one the **knobs** hold: `rows`/`cols` positions and the four handles of
// each, over the stamped topology (`grid_topo_rows` / `grid_topo_cols`, D-53). That is the grid a
// render consumes whenever the stamp and `rows`/`cols` agree — which is every node the host has not
// had to refuse a rewrite on — and it is the only grid whose slots all exist to be written.
//
// What is grabbable is a subset of what is *drawn*, twice over. First the family: only the **visible**
// grids are grabbable — `HitOptions::showSource` / `showDestination` carry `show_source_grid` /
// `show_destination_grid`, and the hidden family's points and arms are drawn dimmed but contribute no hit
// candidate at all (D-84). Then, within what is visible, what is grabbable is a subset of what is
// drawn: the **controls** of the overlay's lattice — every lattice point of the visible families, plus
// the four arm tips of the selected point — and never some point along the curve between two lattice
// points. One control list therefore feeds both jobs: `drawnPoint` / `drawnTip` are the stroke controls
// *and* the hit candidates (D-78). The one place the subset is not the whole drawing is the arms: a
// lattice reads as a lattice only if every point carries its four arms, so the shell strokes them
// everywhere but hits only the selected point's, and marks that point's arms in the host's suggested
// colour so the grabbable ones are the bright ones.
//
// The lines themselves are the **cubics the renderer draws**, not the chords between the points.
// `latticeEdges` hands over the same four controls `edgeCubic` (grid.cpp) builds a cell boundary from,
// pushed through the family's affine, and `flattenCubic` turns each into chords flat enough to read as
// the curve at the zoom the Viewer is at. Tessellating here rather than in the shell is the same split
// as everything else in this file: the step count is float math on the controls, and the shell only
// decides how flat is flat enough. At neutral handles — which the defaults are (grid.h's
// `neutralHandlesFraction`) — cubic and chord are the same curve (grid.cpp's note on B3), so a default
// node draws two points per edge and has nothing to notice.
// ---------------------------------------------------------------------------

#include <vector>

#include "grid.h"

namespace gw {

// The largest absolute lattice value a drag may write. It is the declared knob range, repeated here
// so a drag cannot push a value a descriptor does not describe (D-53; gridwrap.cpp's describe loop
// states the same ±10 on both components of every one of the 640 slots).
enum { kInteractFracRange = 10 };

// The hit tolerance as a *screen* measure, and how much of a lattice spacing it may eat before the
// spacing wins. 8 px is what Natron's own overlay hits with for its hosted tracker handles
// (`Gui/HostOverlay.cpp:2291`); 45% is the point where neighbouring controls would overlap.
enum { kInteractHitPixels = 8 };
static const double kInteractHitSpacingFrac = 0.45;

// Which of the two knob families a drag writes. The family is decided by *what you grab*: a click on a
// point selects that point's family, and a click again on the same spot — one that turns out to be a
// click and not a drag — walks to the other family's point (see `coincidentTwin`). It is never the
// render's morph axis (D-54) — nothing here reads `mix`. The keyboard does reach it, but only as the
// Ctrl latch that gates multi-select (D-85) and the free-handle drag (D-83), never as a way to pick a
// family: a Ctrl press on the other family's point is refused rather than switching families
// (`Selection::toggle`), and Ctrl also suppresses the coincident-twin flip, because Ctrl-clicking a
// point means "add this point", not "walk to its twin".
enum { kFamilySrc = 0, kFamilyDst = 1, kFamilyCount = 2 };

// What a hit is, in the order a click resolves it: a lattice point beats the handle arm of **any other**
// control at any distance, because grabbing the crossing of two grid lines means the point. The one
// exception is a point's own arms: when the nearest point *is* the already-selected point and the pointer
// is nearer to one of that point's arm tips than to the point itself, the point is demoted to
// `kRankArm`, so the nearer arm becomes the head. Without that exception the spacing clamp (see
// `hitRadiusFor`) makes the grab radius reach past an arm tip at high zoom, and a point always outranks
// its own arms means the arms cannot be grabbed any more — which is the opposite of what drawing them
// promises. `hitCandidates` sorts best-first on (rank, then distance, then — only to break a tie — the
// selected family, then the slot), so a genuinely nearer point in the other family is not beaten by
// family preference, and two points drawn on top of each other still resolve to the family you are
// already working in.
enum { kRankPoint = 0, kRankArm = 1 };

// How far apart two candidates have to be before a re-click stops meaning "the twin of what I already
// have". A fraction of the grab radius: the coincident src/dst twins of one lattice point are drawn at
// the *same* place (D-53 stamps both lattices from the same `rows`/`cols`/basis), so 0.25 is room for a
// twin whose handles have been dragged so its point sits a quarter of a grab away, and small enough that
// the neighbouring cell's point is never mistaken for a twin.
static const double kInteractTwinRadiusFrac = 0.25;

// How many candidates one call hands back. 8×8 lattice × 2 families + 4 arms is the whole drawable set
// of a GridWrap node, so this is a ceiling on what a pointer can be on at once, and the number the shell
// sizes its array with.
enum { kInteractMaxCandidates = 32 };

// One selected lattice point. Kept as a slot rather than a Control because a selection is a *knob*
// fact — the shell reads and writes it through `src_rNcM` / `dst_rNcM` — and the canonical positions
// are re-derived from the knobs on every event (D-72).
struct SelectedPoint {
    int row, col;

    SelectedPoint() : row(-1), col(-1) {}
    SelectedPoint(int row_, int col_) : row(row_), col(col_) {}
};

// The shell's selection, passed *in* to every call that cares: one family, and the lattice points of it
// that are selected. **More than one point can be selected at once** (D-85): Ctrl-click adds a point to
// (or takes it back out of) the set, a plain click on a point that is *not* in the set replaces it with
// that one point, and a plain click on a point that already *is* in the set keeps the whole set (so the
// drag that follows moves all of them). A plain click is therefore exactly the single-point selection the
// overlay had before, and `points[0]` is always the primary: the one drawn with the selection ring and
// the enlarged cross, and the one a handle drag belongs to. Every member of the set draws its arms and is
// arm-grabbable. Empty means nothing is selected, which is how a freshly rebuilt interact (or one after a
// topology rewrite) starts.
//
// What multi-select means for a drag is decided by the drag's kind, not here: dragging a lattice point
// moves **every** selected point (D-85), dragging a handle arm moves the one point that arm belongs to
// (plus its mirrored twin, D-83) however many points are selected.
struct Selection {
    int family;                     // every selected point belongs to this family
    std::vector<SelectedPoint> points;   // `points[0]` is the primary

    Selection() : family(kFamilySrc) {}
    Selection(int family_, int row_, int col_)
      : family(family_), points(1, SelectedPoint(row_, col_)) {}

    bool valid() const { return !points.empty(); }
    int count() const { return static_cast<int>(points.size()); }

    // The primary point, and its two accessors: `row() < 0` means nothing is selected, exactly as the
    // single-point `row < 0` used to mean.
    const SelectedPoint& primary() const
    {
        static const SelectedPoint kNone;
        return points.empty() ? kNone : points[0];
    }
    int row() const { return primary().row; }
    int col() const { return primary().col; }

    bool contains(int row, int col) const
    {
        for (std::size_t i = 0; i < points.size(); ++i)
            if (points[i].row == row && points[i].col == col)
                return true;
        return false;
    }

    // Indexed access to the set, bounded by `count()`: what the hit test and the shell walk when they
    // draw arms for, or drag, every selected point.
    const SelectedPoint& at(int i) const { return points[static_cast<std::size_t>(i)]; }

    // A plain press: the family becomes this point's family and the set becomes just this point.
    void selectOnly(int family_, int row, int col)
    {
        family = family_;
        points.clear();
        points.push_back(SelectedPoint(row, col));
    }

    // A Ctrl press: add the point, or drop it if it was already in the set. The family only changes when
    // the press *starts a new set* — Ctrl-clicking a point of the other family while nothing is selected
    // selects it, and Ctrl-clicking a point of the other family while points are selected is refused
    // rather than silently switching families mid-selection, because the whole set is written to one
    // family's knobs. Returns true when the point is selected afterwards.
    bool toggle(int family_, int row, int col)
    {
        if (contains(row, col)) {
            for (std::size_t i = 0; i < points.size(); ++i)
                if (points[i].row == row && points[i].col == col) {
                    points.erase(points.begin() + i);
                    break;
                }
            if (points.empty())
                family = family_;   // an empty set carries the family it just lost
            return false;
        }
        if (!points.empty() && family_ != family)
            return false;           // one set, one family
        if (points.empty())
            family = family_;
        points.push_back(SelectedPoint(row, col));
        return true;
    }

    void clear() { points.clear(); }

    // Drop the selection when the stamped topology no longer contains it. A topology rewrite shrinks
    // or grows the lattice (D-53), and the shell keeps its selection across events, so this is the one
    // place the two are reconciled: a stale slot must never be drawn or dragged.
    void clampTo(int rows, int cols)
    {
        if (family < 0 || family >= kFamilyCount)
            family = kFamilySrc;
        for (std::size_t i = 0; i < points.size();) {
            if (points[i].row < 0 || points[i].col < 0 ||
                points[i].row >= rows || points[i].col >= cols)
                points.erase(points.begin() + i);
            else
                ++i;
        }
    }
};

// One drawable control: a lattice point, or the free end of one of its four handles. `handle` is one
// of kHVNeg/kHVPos/kHUNeg/kHUPos, or -1 for the point itself.
struct Control {
    int family;         // kFamilySrc | kFamilyDst
    int row, col;
    int handle;         // -1 for a lattice point

    bool found;

    // Canonical pixels, **transform applied**: where the Viewer shows it.
    Vec point;          // the lattice point this control belongs to
    Vec tip;            // the handle's free end (== `point` for a lattice point)

    // Normalised squared distance from the pointer to `tip`, in units of the per-axis hit radius;
    // <= 1 for every candidate that was inside the radius.
    double err;

    int rank;           // kRankPoint | kRankArm

    Control() : family(kFamilySrc), row(-1), col(-1), handle(-1), found(false), err(0.0),
                rank(kRankPoint) {}

    // True when this control is one of the lattice points `sel` already names. A handle is never "already
    // selected" — a Selection names points, and an arm belongs to the point it hangs off — so this is the
    // test `hitCandidates` demotes on (see `kRankArm`) and `coincidentTwin` requires of its head, which is
    // what makes a re-click on a coincident spot walk to the other family (D-78). With a multi-point
    // selection (D-85) *every* member of the set counts, because each one draws arms and each one is a
    // point a press could be landing on.
    bool selectedBy(const Selection& sel) const
    {
        return found && handle == -1 && family == sel.family && sel.contains(row, col);
    }
};

// One knob write a drag means, beyond the control that was grabbed. Two things produce them:
//
// - **Multi-select** (D-85): a press that grabs a selected lattice point drags **every** selected point
//   by the same delta, so each other member of the set is one position write.
// - **Mirrored handles** (D-83): dragging a handle also writes the opposite handle of the same point as
//   the exact negative, which is what keeps the curve smooth through the point. `mirrored` marks that
//   write, because the mirror is a *consequence* of the drag rather than the drag itself: the shell's
//   read-back check only inspects the primary write, and the mirror is what the core guarantees.
struct DragWrite {
    int family;         // which family's slot to write
    int row, col;
    int handle;         // -1 for a position write

    Vec value;          // the two knob components, as fractions, already clamped

    bool mirrored;

    DragWrite()
        : family(kFamilySrc), row(-1), col(-1), handle(-1), value(), mirrored(false) {}
};

// What a drag onto a canonical pointer means as a knob write. A lattice point is a **two-component**
// write (both halves of the position's `Double2D`), and so is a handle: its x is the fraction of the
// basis **width** and its y of the basis **height**, which is the axis split `kHandleSpecs` states
// (gridwrap.cpp) and `resolvePoint` implements (grid.cpp).
struct Drag {
    bool changed;

    int family;         // which family's slot to write
    int row, col;
    int handle;         // -1 for a position write

    Vec writeValue;     // the two knob components, as fractions, already clamped to ±kInteractFracRange

    // Where the lattice point this control belongs to stands after the write: in knob fractions, and
    // as the Viewer shows it. A handle drag leaves the point where it was; the shell needs the drawn
    // position only to verify it read back what it asked for.
    Vec pointFrac;
    Vec drawnPos;

    // Every other write this same press produces (see above). They all belong to the same press, and the
    // shell writes them inside the **one** edit block the primary write opened, so a multi-point drag or
    // a mirrored handle is still one undo step (D-78 / D-80).
    std::vector<DragWrite> extraWrites;

    Drag()
        : changed(false), family(kFamilySrc), row(-1), col(-1), handle(-1),
          writeValue(), pointFrac(), drawnPos() {}
};

// What a drag may hit: only the screen radius and what a screen pixel is worth on each axis. Handle
// arms are only candidates when they are **drawn**, which is the shell's business — the arms of the
// selected point only, exactly like Nuke's GridWarp tracker, where the arms hang off the point you
// grabbed — so the arms that are hit-testable are the ones `hitTest` is handed as `Selection`, and an
// invalid selection means a point-only hit-test.
struct HitOptions {
    int radiusPixels;   // screen pixels
    double pixelScaleX; // canonical units per screen pixel, x
    double pixelScaleY; //   ... and y, which is a *different* number under a non-square PAR
    // The two grids' visibility knobs read by the shell (`show_source_grid`, `show_destination_grid`). They gate
    // what a click may *pick*: a hidden family contributes no point and no arm to the candidate list,
    // while the drawing still shows it — dimmed (D-84). Both default to on, which is what a fresh node
    // shows (D-78) and what every existing test of this core assumes: the flags only ever subtract.
    bool showSource;
    bool showDestination;

    HitOptions()
      : radiusPixels(kInteractHitPixels),
        pixelScaleX(1.0),
        pixelScaleY(1.0),
        showSource(true),
        showDestination(true) {}
    // The shape the overlay shell has on hand at every event: the host's own tolerance in pixels plus
    // that frame's per-axis `pixelScale`, plus the two visibility toggles as the shell read them. A
    // shell that reads the scale off `PenArgs`/`DrawArgs` writes it here in one expression rather than
    // field by field.
    HitOptions(int radiusPixels_, double pixelScaleX_, double pixelScaleY_,
               bool showSource_ = true, bool showDestination_ = true)
      : radiusPixels(radiusPixels_),
        pixelScaleX(pixelScaleX_),
        pixelScaleY(pixelScaleY_),
        showSource(showSource_),
        showDestination(showDestination_) {}
};

// One grid line of the overlay: a row line (`horizontal`) or a column line, as the four controls of the
// cubic the renderer draws along it, in the order they are stroked — the two lattice points of the edge
// and the two handles that pair with them (`latticeEdgeCubic` in knob space, this the same quad pushed
// through the family's affine). The endpoints are what hit-testing grabs: a lattice point is where two
// grid lines cross, so it is the control a pointer is aiming at, and nothing along the curve between two
// of them is (which is what "one control list feeds the drawing and the hit test" means, D-78).
struct DrawnEdge {
    Vec cp[4];
    bool horizontal;
};

// A drawable control with the knob slot it stands for, so the shell can read the slot back out of the
// geometry it was handed. `handle` is -1 for a lattice position.
struct PolyPoint {
    Vec p;
    int row, col, handle;

    PolyPoint() : p(), row(-1), col(-1), handle(-1) {}
};

// A handle arm: from the point to its free end, which is drawn with a small tick across the end so an
// arm tip reads as a grab target rather than as a line ending.
struct Segment {
    Vec a, b;
    bool horizontalHandle;   // true for the kHUNeg/kHUPos arms (the basis-width axis)
};

// canonical <-> basis fractions, per component. The same law `resolvePoint`/`shrinkPoint` apply to a
// whole `GridPoint`, written for a bare Vec because the interact moves one control at a time.
Vec toFractions(const Vec& canonical, const Rect& basis);
Vec fromFractions(const Vec& frac, const Rect& basis);

// The **inverse of the above for a handle**: a lattice point to a position, an arm offset to the pair of
// numbers the handle knob holds. `resolvePoint` stores a handle as `Vec(f.h[i][0] * width, f.h[i][1] *
// height)` — a pure offset in canonical units with **no basis-origin term in it** (grid.cpp), and
// `shrinkPoint` reads it back by dividing by width/height and subtracting nothing. So a handle drag
// divides and does *not* subtract `basis.x0/y0`: doing that would fold the grid's position into every
// handle knob (D-78).
Vec offsetFractions(const Vec& offset, const Rect& basis);

// Clamp one knob component to the declared range.
double clampKnobValue(double v);

// The hit radius `hitTest` measures against, in canonical units per axis: the screen radius scaled by
// what a screen pixel is worth on that axis, capped at `kInteractHitSpacingFrac` of the drawn lattice
// pitch. Exported because the shell has to draw markers the size of the grab they stand for, and one
// function computing both is the only way those two numbers cannot drift apart (D-78).
Vec hitRadiusFor(const HitOptions& opt, const Vec& spacing);

// The inverse of an affine. False when the linear part is singular, which is the same refusal
// `applyTransform` makes at the same determinant bound: a collapsed grid has no inverse map, so a
// drag cannot be solved and is refused rather than guessed at.
bool inverseAffine(const Affine& af, Affine& out);

// A cubic Bézier flattened into chords: writes `out[0..return-1]`, always at least the two endpoints, so
// the caller can stroke `GL_LINE_STRIP` over what it returns without knowing the step count. Flatness is
// measured against the **chord**: the departure of `B(t)` from the straight line between the endpoints is
// `t(1-t)[-(2-t)P0 + 3(1-t)P1 + 3tP2 - (1+t)P3]`, whose part perpendicular to that line is at most
// `3t(1-t)·D <= 0.75·D`, where `D` is how far either interior control stands from the chord line (from
// `cp[0]` when the chord has collapsed to a point). A cubic subdivided into `n` equal pieces has a
// sagitta of `0.75·D/n²`, so `n = ceil(sqrt(0.75·D / tolerance))` segments is what delivers `tolerance`,
// floored at 2 pieces and capped at 64 — a straight edge is the `D <= tolerance` case and stays 1 piece,
// i.e. the chord (D-78).
//
// `tolerance` is in the same units as the controls, which for the overlay means **screen pixels pushed
// through the pixel scale**: flatness is a property of what the eye resolves, not of the canonical grid.
// `max` caps the points written (a `max` of 1 can only ever deliver `cp[0]`); a non-finite control or a
// non-finite/non-positive tolerance is answered by refusing to subdivide, or by taking the cap, rather
// than by inventing a step count. Returns the number of points written, or 0 when `out` is null or
// `max <= 0`.
int flattenCubic(const Vec cp[4], double tolerance, Vec* out, int max);

// One node's two lattices, as the overlay needs them. Rebuilt from the knobs for every event, which
// is what keeps it honest with `undo`, keyframed lattices, and a host that rewrites the knobs itself:
// the interact has no copy of the grid to go stale.
class Interactor {
public:
    // `srcAf`/`dstAf` are the affine each family is drawn through, which is `affineFor(xform)` for
    // both unless `use_source_transform` says the destination grid inherits the source's (D-62): the
    // shell decides that, because it is a knob reading and not geometry. The two grids must share a
    // topology, as `rows`/`cols` are one pair for both (D-53).
    Interactor(const Grid& src, const Grid& dst, const Rect& basis,
               const Affine& srcAf, const Affine& dstAf);

    int rows() const { return _rows; }
    int cols() const { return _cols; }
    const Rect& basis() const { return _basis; }

    // A family's lattice as drawn: one edge per grid line, row lines first and then column lines, which
    // is the order the strokes are issued in. Each `DrawnEdge` carries the four controls of the cubic
    // `edgeCubic` builds for that grid line, pushed through the family's affine, so what the overlay
    // strokes and what the renderer warps by are the same curve (D-78). Empty for a family with no
    // topology.
    std::vector<DrawnEdge> latticeEdges(int family) const;

    // A family's lattice positions, tagged with the slot each one stands for.
    std::vector<PolyPoint> latticePoints(int family) const;

    // The four arms (and their tips) of one lattice point, drawn through that family's affine. Empty
    // for a point outside the topology.
    std::vector<Segment> handleArms(int family, int row, int col) const;
    std::vector<PolyPoint> handleTips(int family, int row, int col) const;

    // Every drawable control within the hit radius of a canonical pointer, over **both** families: all
    // the lattice points of each, plus the four arms of **every selected point**. Written best-first —
    // rank, then distance, then the selected family only to break a tie, then (row, col, family, handle)
    // so two runs give the same list — and capped at `max`, because the shell only ever consumes the head
    // of it (and `coincidentTwin`). Returns the count written.
    //
    // Two rules decide what the selected points contribute (D-78, widened to a set by D-85):
    // - If the best of all the points is a selected point and one of *that* point's own arms is nearer
    //   than it, the point is demoted to `kRankArm` and that point's arms are offered, so the nearer arm
    //   becomes the head.
    // - If the best of all the points is a selected point and no arm of it is nearer, that point's arms
    //   are not offered at all: a press on the point is a press on the point.
    // In every other case an arm is offered as an ordinary `kRankArm` candidate, which is what keeps an
    // arm grabbable when the pointer is on it and some *other* control is the nearest point.
    //
    // `opt.showSource` / `opt.showDestination` gate whole families: a family the panel has hidden
    // contributes no point and no arm (D-84).
    //
    // The distance is measured per axis against `radiusPixels * pixelScale`, so a non-square PAR
    // hit-tests circularly on the screen, and each radius is then clamped to
    // `kInteractHitSpacingFrac` of the drawn lattice spacing on its own axis so a dense lattice keeps
    // its points individually grabbable at any zoom (D-78).
    int hitCandidates(const Vec& canonicalPointer, const Selection& sel, const HitOptions& opt,
                      Control* out, int max) const;

    // The control a pointer is aiming at: the head of `hitCandidates`, or an unfound `Control` when the
    // pointer is on nothing.
    Control hitTest(const Vec& canonicalPointer, const Selection& sel, const HitOptions& opt) const;

    // The **other family's** twin of the head, if the head is the point `sel` already names: the same
    // (row, col) slot in the other family, drawn at the same place because D-53 stamps both lattices from
    // one `rows`/`cols`/basis — so re-clicking that spot is the only way src<->dst flips in a host that
    // routes no key press to a plugin overlay (D-78). Null in every other case: the head is some control
    // we are not already holding (nothing to flip *away* from), or the head is a handle arm (a press on
    // an arm is a press on an arm), or the other family's point in that slot stands further than
    // `kInteractTwinRadiusFrac` of the grab beyond the head (two grids warped apart are not twins).
    // The shell applies the answer only when the pen-down turns out to be a click and not a drag, which
    // is what keeps dragging an already-selected point working.
    static const Control* coincidentTwin(const Control* cands, int count, const Selection& sel);

    // The write that dragging `ctl` to a canonical pointer means, given the selection it happened inside.
    //
    // A lattice point that is **in the selection** drags the whole set: the primary fields write `ctl`'s
    // own slot, and every other selected point gets the *same delta* in `extraWrites` — the shared-delta
    // move, not "every point to the pointer", which would collapse the set onto one spot (D-85). A point
    // outside the selection moves on its own, exactly as it did before multi-select existed.
    //
    // A handle writes that handle, and — unless `mirrorFree` — the **opposite** handle of the same point
    // as the exact negative of the dragged one *after* clamping, so the pair stays symmetric through the
    // point and the curve stays smooth across it (D-83). `mirrorFree` is the Ctrl override: that arm
    // moves freely and its opposite stays where it was. The perpendicular pair is never touched either
    // way, because a u-handle and a v-handle are different axes of the same offset.
    //
    // `changed` is false for a control that was not found, for a slot outside the topology, and for a
    // drag under a singular transform.
    Drag dragFor(const Control& ctl, const Selection& sel, const Vec& canonicalPointer,
                 bool mirrorFree) const;

    // True when this family's affine can be inverted, i.e. its drags can be solved at all.
    bool transformIsInvertible(int family) const;

    // The cubic the **renderer** draws along one grid line: the two lattice points of the edge and the
    // two handles that pair with them, in knob space (no affine applied). `horizontal` picks the row
    // line (the kHUPos/kHUNeg pair) or the column line (kHVPos/kHVNeg), exactly as render.cpp's cell
    // boundaries do. Returns false for an edge outside the topology. `latticeEdges` is the same four
    // controls drawn, and `flattenCubic` is what strokes them.
    bool latticeEdgeCubic(int family, int row, int col, bool horizontal, Vec out[4]) const;

    // The drawn lattice pitch per axis, for the shell's own bookkeeping (a marker size, say). Pair it
    // with `hitRadiusFor` — that is the radius the grab actually used on each axis.
    Vec latticeSpacing() const { return _spacing; }

private:
    const Grid& gridFor(int family) const;
    const Affine& affineFor(int family) const;
    Vec drawnPoint(int family, int row, int col) const;
    Vec drawnTip(int family, int row, int col, int handle) const;
    bool inTopology(int row, int col) const;

    const Grid& _src;
    const Grid& _dst;
    Rect _basis;
    Affine _af[kFamilyCount];
    Affine _inv[kFamilyCount];
    bool _invertible[kFamilyCount];
    Vec _spacing;      // drawn pitch per axis
    int _rows, _cols;
};

} // namespace gw

#endif
