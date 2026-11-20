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

- [x] Part 1 — potrace backend + node skeleton + b/w trace + check harness
      (done 2026-11-20: vendored potrace 1.16 static lib, `Dev/Mask/parsers/PotraceTrace`,
      `Dev/Mask/TraceToMask` registered as `fr.inria.built-in.TraceToMask`; harness
      `tools/trace-tests/run_check.sh` green — 3 shapes, despeckle, y-flip, hole punch,
      IoU 1.0000. Notes: potrace's flat path list already includes holes, punch by
      `path->sign` (do NOT also recurse `childlist`); potrace y is y-up so `pathToShape`
      flips with `docHeight - y`; `RotoContext::generateUniqueName` always appends its
      own counter, so import a fixed base name ("Traced") and let it number the shapes;
      hole operator default = minus (`eMergeMinus`).)
- [x] Part 2 — stacked bands + holes-as-minus + MaskToGeo operator union
      (done 2026-11-20: N band thresholds span [Threshold Low, Threshold] inclusively,
      darkest/largest band traced first so brighter bands layer over it; each band/cluster
      imports into its own sub-layer under "Trace"; holes carry a minus operator end to end
      — potrace's path `sign` holds the CHARACTERS '+'/'-', not +-1, and transformShape
      must carry `compositingOperator` through (both bugs found via harness); MaskToGeo's
      flat fill reverses minus/XOR contours (CW) so the nonzero-winding tessellation cuts
      holes; extruded mode skips subtractive curves (no CSG yet). Harness: bands nest
      200/120/60 px in 3 layers, MaskToGeo hole cut verified via NATRON_SC_DUMP, IoU 0.984.)
- [x] Part 3 — UX (auto-retrace/bake/info) + docs
      (done 2026-11-20: Auto Re-trace was already wired knob-signal -> re-trace; Info knob
      live. Added **Bake Sequence** (+ Bake From/To, project-range fallback): traces input 1
      per frame in [From, To] and keys every traced CP into the current shapes (curve i <->
      i-th non-empty doc shape; frames whose shape/CP count changed keep their previous
      keys), turns Auto Re-trace off and reports "baked N frames (M skipped)". Harness:
      a 3-frame marching-rect sequence keys CPs that follow the rect per frame.
      Docs: NODE_REGISTRY (Mask Import 2 nodes), DEV_3D_SYSTEM_CHANGELOG entry.)
- [x] Part 4 — centerline (exploratory)
      (done 2026-11-20: Zhang-Suen thinning + degree-based chain extraction into open
      Beziers verified end to end. Three bugs surfaced: the 8-neighbor ring arrays were
      corrupted — dx had 0 in the SW slot (duplicate of S) and dy was scrambled in three
      slots, so the thinning barely eroded (8 px/iteration, skeletons never collapsed);
      the pass guards were diagonal-neighbor forms instead of the classic cardinal
      N&&E&&S / E&&S&&W / N&&E&&W / N&&S&&W; and rdpSimplify returned the whole polyline
      instead of collapsing to the endpoints when no point strays past the tolerance.
      The chain walk is now degree-based: endpoint chains, closed loops, then one chain
      per junction spoke (a diagonal step into a junction-adjacent pixel is refused so
      chains end at the junction instead of bending around it). Harness: a 5px stroke
      thins to ONE 2-CP curve at the stroke ends (ends recede ~3 px, classic Zhang-Suen
      erosion), a plus traces exactly 4 spokes meeting at the junction.)

## Field-report fixes (2026-11-20, after first real use)

User report: traces mismatched the image's size/aspect; after swapping to a bigger input
the node "still worked in the 1st size"; tracing froze the UI and eventually crashed.

1. **Fit default**: fitMode defaulted to Fit-to-Format (95% margin, format-anchored) -
   wrong for a tracer, whose shapes must sit ON the image. Default is now **Original
   Size**, and Original/Custom anchor at the SOURCE PLANE's own bounds (not the format's
   bottom-left), so traces land exactly on the image wherever it sits (chained roto
   outputs, centered images...).
2. **Stale fetch after an input swap/resize** (the "1st size" bug, two layers deep):
   (a) nothing re-traced on an input change - now the input node's knobsAgeChanged +
   the timeline frameChanged mark a deferred check (a zero-timer: the input emits
   knobsAgeChanged BEFORE recomputing its hash, so the direct slot must not trace);
   the check compares the input's knobs age (any upstream edit retraces - the shapes
   follow the image) and, for timeline moves, the input's RoD. (b) the fetch itself
   went through getImage, which outside a render action reuses the thread's leftover
   render TLS (frameArgs never popped) whose node hash still pointed at the pre-edit
   state - the fetch silently returned the OLD image (and the RoI was clipped to the
   node's own RoD = the old shapes' bbox). The fetch now follows the
   TrackMarker::getMarkerImage pattern: ParallelRenderArgsSetter (fresh args, isAnalysis)
   + direct renderRoI over the input's own RoD + cleanupTLSForThread. On the main thread
   on purpose: from a worker thread the reader would hijack the project format
   (renderRoIInternal sets it from non-main threads), which rescaled every downstream
   render.
3. **UI freeze / crash**: tracing ran synchronously on the main thread. The heavy
   compute (potrace/k-means/thinning) now runs on the global thread pool while a nested
   event loop pumps the GUI; the RotoContext import stays main-thread. Re-entrant knob
   edits during a trace are coalesced into one follow-up trace (traceRunning/traceDirty)
   instead of nesting - the likely crash source.
4. **Reset button** next to Trace: removes every traced shape/layer without re-tracing.
5. **Second freeze round + color-mode crash**: the FETCH still ran on the main thread
   (the upstream render for a big image is the slow part). It now runs on the worker
   thread too - the input's RoD is queried on the main thread (cheap, cached, fresh
   hash) and handed to the job as renderRoI's preComputedRoD; the worker does
   ParallelRenderArgsSetter + renderRoI + cleanupTLSForThread (the TrackMarker
   pattern). Two guards make the off-main fetch safe: the project's
   autoSetProjectFormat is suppressed+restored around the fetch (renderRoIInternal
   sets the project format from the reader only on non-main threads - it hijacked
   the format to the image size and rescaled every downstream render), and the
   valve below. The RotoContext import stays on the main thread, now wrapped in
   try/catch (an exception escaping a Qt slot aborts the app).
6. **Import valve -> Max Shapes knob (default 8, user request)**: a photographic source
   decomposes into thousands of contours (the noisy 600x600 stress traced 21,876 paths;
   the 1600x1600 penguin-like probe traces 11) - importing that many Beziers (2 internal
   nodes each) stalls the UI for minutes and can OOM, which matches the reported
   color-mode stall+crash. The valve is now the **Max Shapes** knob (Trace page, default
   8): traces above it are refused with "N shapes traced - too complex to import; raise
   Max Shapes or Despeckle" and the previous import is kept; bake frames above it are
   skipped; RAISING the knob imports the cached refused document WITHOUT re-tracing.
7. **Color-mode stall**: the naive k-means ran every pixel against every center for all
   8 iterations (2500x2500 x 4 x 8 = 200M distance evals, ~30 s for 1600x1600 measured).
   k-means now clusters a strided sample (<= 256k px) and assigns every pixel once
   against the converged centers: the same 1600x1600 color trace dropped to ~1.3 s.
8. **Pump hardening**: the nested event loop now runs with ExcludeUserInputEvents - the
   window keeps painting (no frozen look) but clicks/keys cannot re-enter the trace or
   the settings panel mid-import (the GUI-side crash on selecting Color is not
   reproducible headlessly; input re-entrancy during the 30 s stall was the plausible
   trigger and is now closed, together with the stall itself).

Harness: color mode (3 colored circles -> 4 clusters incl. the anti-aliased edge
cluster, one sub-layer each), a 900x900 smooth-blob color stress, and the noisy-image
valve check. TRACE_CHECK DONE_OK, 07-verify DONE_OK.

Harness: input-swap re-trace (300x300 rect -> 600x600 rect, shapes follow at 1:1
canonical placement), reset clears all shapes; whole suite TRACE_CHECK DONE_OK,
07-verify DONE_OK.
      (done 2026-11-20: Zhang-Suen thinning + degree-based chain extraction into open
      Beziers verified end to end. Three bugs surfaced: the 8-neighbor ring arrays were
      corrupted — dx had 0 in the SW slot (duplicate of S) and dy was scrambled in three
      slots, so the thinning barely eroded (8 px/iteration, skeletons never collapsed);
      the pass guards were diagonal-neighbor forms instead of the classic cardinal
      N&&E&&S / E&&S&&W / N&&E&&W / N&&S&&W; and rdpSimplify returned the whole polyline
      instead of collapsing to the endpoints when no point strays past the tolerance.
      The chain walk is now degree-based: endpoint chains, closed loops, then one chain
      per junction spoke (a diagonal step into a junction-adjacent pixel is refused so
      chains end at the junction instead of bending around it). Harness: a 5px stroke
      thins to ONE 2-CP curve at the stroke ends (ends recede ~3 px, classic Zhang-Suen
      erosion), a plus traces exactly 4 spokes meeting at the junction.)
