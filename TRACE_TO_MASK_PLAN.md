# TraceToMask — bitmap tracing node (game plan, started 2026-11-19)

User request (2026-11-19): a **bitmap-tracing node** that converts a raster image into a
layered vector mask exactly the way `ReadMask` produces one: real Roto Bézier shapes in the
node's own RotoContext, rendered as an alpha mask, exposed to the whole vector pipeline
(RotoPanel tree, viewer overlay, MaskToGeo, Scene3D/Cycles via MaskToGeo).

Studied implementations (open source):

1. **Potrace** (Peter Selinger, GPL-2.0+, pure C ~2.5k LOC) — the backend behind Inkscape's
   "Trace Bitmap" for 20 years. License is GPL-2.0+, compatible with this fork (Natron is
   GPL-2.0). Stable, dependency-free, single-thread C API (`potrace_trace(bm, opt)`).
   Pipeline, in stages (each stage independently useful):
   1. **Path decomposition**: thresholded 1-bit bitmap → connected black regions → closed
      contours with alternating winding (a contour's `children` are its holes).
   2. **Optimal polygon**: dynamic-programming straightest enclosing polygon per path.
   3. **Vertex analysis**: each polygon vertex classified corner vs smooth from the local
      deviation (alpha / maxd / epsilon tests).
   4. **Curve fitting**: best parabola between corners → converted to cubic Béziers.
   Key knobs: `turdsize` (despeckle — drop areas < N px), `turnpolicy`,
   `alphamax` (0..1.334, corner smoothing), `opticurve` + `opttolerance` (curve
   optimization). The potrace source distribution also contains **mkbitmap**, a prefilter
   (blur → high-pass → scale → threshold) designed for scanned line art.
2. **VTracer** (visioncortex, Rust core, permissive licence) — the modern color-aware
   tracer. Pipeline: color clustering (k-means; 1.0 adds watershed + fixed-palette
   snapping in OKLab) → per-cluster B/W trace → **stacked layering** (shapes stacked with
   no hole geometry — same trick as Inkscape multi-scan) → curve fitting in
   pixel/polygon/spline modes → paper.js-style simplification (fewest cubics within a px
   tolerance). Extras worth borrowing as *concepts*: speckle filter, Bradley–Roth adaptive
   threshold for uneven plates, gradient-step layered banding, seam-free "cutout".
3. **Autotrace** (GNU, GPL) — the only mature OSS implementation of **centerline** tracing
   (thin strokes → single skeleton curves instead of outlines). Optional phase-2 material.

Note: "EffectCraft" (user's list) could not be identified as an open-source suite —
asked the user for a link; the plan stands on Potrace/VTracer/Autotrace meanwhile.

## Recipe for Natron

**TraceToMask is ReadMask's twin**: ReadMask turns *vector files* into its RotoContext;
TraceToMask turns *bitmap pixels* into the same thing. Everything downstream is shared and
already works (RotoPanel tree with the subtree-eye fix, viewer overlay, mask render,
MaskToGeo → Scene3D/Cycles — tracing a logo bitmap then extruding it becomes a 2-node wire).

### Backend: vendor Potrace (Part 1)

- `libs/potrace/` — vendored potrace C sources (same pattern as `libs/libtess`), compiled
  into NatronEngine. No third-party build-system changes; a thin wrapper
  `Engine/Dev/Mask/parsers/PotraceTrace.{h,cpp}` converts a Natron float plane into
  potrace's 1-bit `potrace_bitmap_t` and walks the returned `potrace_path_t` list
  (contours + children = holes) into `VectorMask::VectorShape`s.
- Coordinate note: potrace returns curves with y-up in bitmap space — apply the same
  canonical y handling as the SVG parser (`docHeight - y`) and reuse ReadMask's
  `transformShape` fit pipeline unchanged.

### Node: `TraceToMask : RotoNode` (same skeleton as ReadMask)

- Input 1: the image to trace (any plane). Output: the alpha mask rendered from the traced
  shapes (same render path as ReadMask). `getRotoContext()` exposure is inherited.
- **Refactor shared with ReadMask**: extract ReadMask's VectorDocument → RotoContext
  import (the `RotoContext::addItem` path, name de-duplication, layer nesting) into one
  shared helper in `VectorDocument.cpp` (`VectorMask::importDocumentToRotoContext`);
  ReadMask and TraceToMask both call it. VectorDocument gains `sourceFormat = "TRACE"`.
- The traced document is built per trace run: each potrace path → one closed VectorShape
  (corners from potrace's corner vertices, smooth vertices from the fitted Bézier handles,
  stored as ShapeVertex tangent offsets exactly like the SVG importer does).

### Knobs (mapped to the algorithm)

*Source page*
- Channel (RGBA/A/luma + plane selector), **Invert**, **Pre-Blur** (px, separable
  gaussian before threshold — the mkbitmap idea), **Threshold Min/Max** (band between two
  levels, roto-Levels style).

*Trace page*
- **Mode**: `Single` (one threshold) | `Stacked Bands` (N thresholds from Min..Max, each
  band a Bézier layer stacked "over" — Inkscape multi-scan / VTracer stacking) — Part 2.
- **Despeckle** (`turdsize`, default 2).
- **Corner Threshold** (`alphamax` 0..1.334, default 1.0; 0 = polygon, 1.334 = smoothest).
- **Curve Optimization** (`opticurve` on/off) + **Opt Tolerance** (`opttolerance`, 0.2).
- **Holes as Minus**: potrace gives contour/child winding — import child contours as
  Bezier shapes with operator **minus** (the RotoPanel "over/minus" dropdown), so holes
  punch through exactly like hand-roto. (MaskToGeo's union fill must then honor the
  operator — currently it unions everything; see open questions.)

*Transform page*: reuse ReadMask's (Fit mode / Custom Scale / Custom Offset / Invert Y).

*Timing*
- Tracing is a discrete expensive step → **Trace** button + **Auto Re-trace** (default
  off) debounced like GeoBuilder's Re-build Materials. Trace happens at the current frame
  against input 1.
- **Bake Sequence**: reuse ReadMask's bake machinery so an animated input can be traced
  per frame and persisted as keyframed Béziers (file-independent, like ReadMask Part 2).
- **Info** label knob: "N shapes, M control points".

### Implementation stages

1. **Part 1 — b/w trace end to end**: vendor potrace; PotraceTrace wrapper; TraceToMask
   node with Source page (channel/invert/blur/threshold) + Trace page (despeckle, corner
   threshold, optimization) + Transform page + Trace button; single-layer import; mask
   render + overlay + MaskToGeo all live through the shared RotoContext.
   Check harness `tools/trace-tests/run_check.sh` (same pattern as mask-tests): render a
   synthetic 300×300 SVG to PNG via Read1 → feed TraceToMask → compare traced mask vs the
   binary source (IoU ≥ 0.98 target), shape-count + despeckle checks, y-flip orientation
   check. Headless render oracle caveat of this box applies — use the offline
   rasterization approach from mask-tests (NATRON_SC_DUMP not needed; the node outputs an
   image plane so compare the writer EXR directly).
2. **Part 2 — stacked bands + holes**: multi-threshold layering, holes as minus shapes,
   MaskToGeo operator-aware union (minus contours subtract), gradient-step default
   banding, speckle tuning per band.
3. **Part 3 — UX + animation**: auto-retrace debounce, per-frame retrace gating, Bake
   Sequence integration, Info knob, docs (NODE_REGISTRY/README/changelog) + plan markers.
4. **Part 4 — centerline mode (optional/exploratory)**: skeletonization (thinning) of the
   thresholded bitmap → open Bézier curves (autotrace-style); candidates: classic
   Zhang-Suen thinning + spurious-loop pruning + the same Bézier fitting as potrace's
   smooth stage. Open Béziers exist in the engine (`setCurveFinished` private path —
   same one ReadMask uses for open SVG subpaths).

### Open questions (ask user before Part 1 lands)

1. "EffectCraft" — which project is meant? (link) The plan currently stands on
   Potrace/VTracer/Autotrace only.
2. Holes: minus-operator shapes (hand-roto semantics, tree-editable) vs winding-based
   rendering (mask exactness)? Proposal above: minus shapes; MaskToGeo union learns the
   operator in Part 2.
3. Default stack: is a *stacked bands* default (N=4) wanted for photographic sources, or
   single-threshold default (line-art-first, matching the mask use case)? Proposal:
   single default, bands opt-in.

## Status

- [ ] Part 1 — potrace backend + node skeleton + b/w trace + check harness
- [ ] Part 2 — stacked bands + holes-as-minus + MaskToGeo operator union
- [ ] Part 3 — UX (auto-retrace/bake/info) + docs
- [ ] Part 4 — centerline (exploratory)
