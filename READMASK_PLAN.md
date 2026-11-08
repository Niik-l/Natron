# ReadMask / MaskToGeo — game plan (started 2026-11-06)

User request (workspace session 2026-11-06): a vector-mask import pipeline for Natron,
based on **vectorNuke** (https://github.com/erinwnash/vectorNuke — a Nuke plugin that
imports vector/spline files from Blender, Photoshop, Maya, Houdini, Illustrator and CAD
software as native Bézier splines inside Roto/RotoPaint nodes), **ported to Natron C++
sources — no Python bridge, no UI dialog, no menu entries**; each node is
self-contained. Licence decision: **GPL-2.0** (user's call — vectorNuke's repo carries no
licence file; treated as GPL-2.0 for this fork, NATRON IS GPL-2.0. File headers note
"Derived from vectorNuke, GPL-2.0"). All 10 formats in scope but **the static-format
parsers are lowest priority** (see ordering).

Two nodes, in this order (revised 2026-11-06 — mask→3D loop lands before the extra
parsers):

1. **`ReadMask`** — a vector-file importer that *is* a Roto node (`ReadMask : RotoNode`,
   pattern proven by `RotoSmear`, RotoPaint.h:193–210). Parses SVG (v1) into real
   Bézier shapes in its own RotoContext; outputs the mask alpha like any Roto. Full
   vectorNuke format set back-ported later at lowest priority.
2. **`Bake Sequence`** (ReadMask button) — per-frame keyframing across a sequence so
   imported masks persist as project-native animated Béziers, file-independent
   afterwards. Static-import semantics for non-sequence formats.
3. **`MaskToGeo`** — converts Roto/ReadMask shapes into a 3D mesh consumable by the
   current Scene3D pipeline (ScanlineRender, CyclesRender, 3D viewport) via the
   MeshProvider contract. Flat pure-shape mesh by default, extrusion opt-in.
4. **Tests + docs catch-up** for parts 1–3.
5. *(lowest priority)* **Static vector parsers** — AI (Illustrator 9+ PDF-stream +
   legacy PostScript) / EPS (incl. Photoshop clipping paths) / PDF. Best-effort
   fidelity, acceptable per user ("PDF sequences" are not a thing — these are
   static imports parsed once, no per-frame robustness exposure).
6. *(lowest priority)* **DCC/CAD/GIS parsers** — DXF (LWPOLYLINE bulge arcs, LINE,
   CIRCLE, ARC, SPLINE), OBJ curves (cstype bzr / curv / l), Maya ASCII nurbsCurve,
   Houdini (.geo PGEOMETRY + JSON geo), GeoJSON (LineString/Polygon).

## What is already established (checked 2026-11-06)

- **Roto scripting/C++ surface is complete** — tangents settable:
  `Bezier::setLeftBezierPoint/setRightBezierPoint/setPointAtIndex(feather, index,
  time, x, y, lx, ly, rx, ry)` (Engine/Bezier.h:261–280, implementation
  setPointAtIndexInternal 2054–2158); curve build-up via
  `addControlPoint`/`addControlPointAfterIndex` (Bezier.h:141–155); open beziers via
  the private `setCurveFinished` path (C++ only — another reason the node is native,
  not a PyPlug). Feather points exist per CP (Bezier.h:59–66).
- **Shapes serialize inside the project** — `NodeSerialization.cpp:155–158` →
  `RotoContext::save` (RotoContext.cpp:972 / load 1013) → `BezierSerialization.h`
  persists NumPoints + per-point CP+FP with X/Y/tangent animation curves; open-bezier
  flag since v4. So edits on imported shapes save/load in .natron automatically —
  the only conflict is file-rebuild vs. user edits, resolved by the three modes
  (file-driven rebuild / bake / static import) under "Sequences & persistence".
- **vectorNuke core is DCC-neutral** — `core/models.py` (Point2D, ShapeVertex
  {center, left/right tangents as offset vectors}, Color RGB/CMYK/hex/named,
  VectorShape {closed, fill/stroke colors, stroke width, opacity, layer_name},
  VectorDocument {width, height, shapes}, name sanitizer) and `core/bezier.py`
  (CubicBezierSegment, quad→cubic 2/3-rule, SVG elliptical arc → exact cubics per
  W3C SVG 1.1 F.6 with ≤90° segment splitting, segments→vertices tangent assembly
  for open/closed paths, transform_shape with flip-Y + scale + offset). Pure math,
  direct C++ port ~500 LOC. The Nuke-specific parts (nuke_bridge/roto_builder.py,
  ui/importer_dialog.py, menu.py, mock_nuke.py) are **dropped, re-implemented natively**.
- **MeshProvider is the 3D discovery contract** — `Dev/Scene3D/MeshProvider.h:37–44`
  (`virtual MeshDataPtr getMeshData(double time) const`). SceneGraph::rebuild walks
  all nodes and `dynamic_cast<MeshProvider*>` (SceneGraph.cpp:304–326);
  ScanlineRender::extractGeometry does the same on input 1 ("obj/scn",
  ScanlineRender.cpp:1941–2010); CyclesRenderer fan-triangulates n-gons
  (CyclesRenderer.cpp:3035). No is-3D flag exists anywhere — declaring grouping "3D"
  is the only marker. Any node can wire into ScanlineRender input 1
  (NodeInputs.cpp:677–748 has no type gating); 2D-mask nodes are simply ignored by
  unknown-cast fallback.
- **Mesh payload** — `MeshData.h:43–96`: interleaved xyz `vertices`, n-gon
  `faceIndices`+`faceCounts`, `edgeIndices` wireframe, per-face-corner `uvs`
  (texCoordComponents == 2), `vertexColors`/`faceMaterials`, embedded transform[16];
  no normals channel — consumers compute from winding (CCW-from-outside is the
  source of truth, GeoBuilder.cpp:1257). No pre-triangulation required (fan-tri
  downstream) **but concave outlines must be tessellated by us**.
- **GeoBuilder is the node template** — EffectInstance + MaterialProvider +
  SubdivisionProvider + MeshProvider (GeoBuilder.h:71–101); local `Builder`
  helper (beginShape / vert / edge / quad / tri, GeoBuilder.cpp:1235–1274);
  snapshot assembled + hash-cached in getMeshData (1419–1497); TRS knobs by plain
  name — consumers read `translateX/Y/Z rotateX/Y/Z scaleX/Y/Z uniformScale
  (+ rotationPivot)` **by name** (ScanlineRender.cpp:1990–2010,
  SceneGraph.cpp:329–355) — MaskToGeo must use those exact knob names.
- **libtess is already in-tree and already tessellates exactly roto polygons** —
  vendored SGI/Mesa `libs/libtess` (plain C, libtess.h:76–87), used by
  `RotoContextPrivate::computeTriangles` over `Bezier::evaluateAtTime_DeCasteljau`
  contours with gluTessNormal(0,0,1) for the XY-plane case
  (RotoContext.cpp:3717–4026). MaskToGeo reuses both calls; no earcut/poly2tri
  exists elsewhere.
- **Built-in registration checklist** (proven by Cryptomatte/GeoBuilder): plugin-ID
  macro next to the 3D block in `Engine/EffectInstance.h:146–181`, include +
  `registerBuiltInPlugin<T>(icon, false, false)` in `AppManager::loadBuiltinNodePlugins`
  (AppManager.cpp:1641–1749, GeoBuilder example 1735). `Engine/CMakeLists.txt:47–72`
  GLOBS source dirs — a new family dir needs a GLOB+APPEND pair + re-run
  `cmake .. -G "MinGW Makefiles"` in build-qt6 for any new files.
- **ReadMask worker precedent** — the fork's mask-consuming nodes (PatchFill /
  ContentFill / LensDefocus) take a Roto via input and use the host mask-channel
  selector; ReadMask inverts the flow (produces the Roto). `Node::getRotoContext()`
  (Engine/Node.h:314) reaches any Roto/Paint/ReadMask node's shape tree for
  MaskToGeo's input pull. A 2D image input is fine for a 3D-side node (Card3D
  takes image inputs for materials, Card3D.cpp:590–596).
- **vectorNuke format inventory (all 10, back-ported from Python sources
  parsers/*.py in lowest-priority phases)**: SVG (full path grammar
  M/L/H/V/C/S/Q/T/A/Z, primitives, transforms, styles — the big one, 23 KB py),
  AI/EPS/PDF (AI 9+ is PDF content streams; legacy PostScript AI separate;
  Photoshop *Export Paths to .ai* and PS-EPS clipping paths), DXF (AutoCAD 2D),
  OBJ (Blender curve exports), Maya ASCII (.ma nurbsCurve), Houdini (.geo classic
  PGEOMETRY + JSON/hjson), GeoJSON (LineString/Polygon).
- **Fit/placement model to copy** — vectorNuke fit modes: fit-format (uniform
  95 %-margin + center), center-format (1:1 center), original (bottom-left 1:1),
  custom scale/offset; flip-Y default ON (top-left vector space → bottom-left
  canvas); tgY-flip must also negate tangent Y. Port `calculate_transform`
  (roto_builder.py) verbatim.

## Part 1 — ReadMask core (SVG)

**Where:** new family dir `Engine/Dev/Mask/`:

- `VectorDocument.{h,cpp}` — port of models.py + bezier.py (Point2D, ShapeVertex,
  Color, VectorShape, VectorDocument, sanitizer; Segment/CubicBezierSegment,
  quad→cubic, arc→cubic, segments→vertices, transform).
- `parsers/SvgParser.{h,cpp}` — full SVG path grammar, rect/circle/ellipse/line/
  polyline/polygon primitives, nested transforms (matrix chain), style/opacity/
  fill/stroke color extraction into VectorDocument. (~1.5–2.5k LOC — the
  largest single piece.)
- `ReadMask.{h,cpp}` — `class ReadMask : public RotoNode`. Knobs (first cut):
  File (sequence-capable file knob), Fit (fit-format / center-format / original /
  custom), Custom Scale X/Y + Offset X/Y, Invert Y (default ON), Import Colors
  (default ON), Group by Layer (default ON), Feather Splines (default OFF —
  zero feather like vectorNuke), Auto Rebuild (default ON).
- **Context assembly:** parse → VectorDocument → RotoContext tree:
  `createBezier`-equivalent via RotoContext C++ API, CPs at time t via
  `setPointAtIndex`, open shapes via `setCurveFinished`, layer groups as
  `RotoLayer` children named after SVG groups / file (sanitized names).
  Colors → per-item color/opacity params.
- **Rebuild semantics:** hash of resolved file (path + mtime/size) cached;
  rebuild only on actual change (knob change or frame change resolving to a
  different file). No rebuild → no undo churn, no edit wipe when nothing changed.

**Registration:** `PLUGINID_NATRON_READMASK` macro (EffectInstance.h 3D block
area), AppManager include + register row, grouping "Image" (Roto family),
NODE_REGISTRY.md row. New files → cmake regen.

## Part 2 — Bake + persistence

- "Bake Sequence" button: for the sequence's frame range, for every shape CP,
  `setPointAtIndex(false, i, t, ...)` per frame → every frame becomes a keyframe;
  then Auto Rebuild effectively idles (shapes are project-native animated
  Béziers, file-independent). Long-sequence guard (warn above N frames).
- Static-import semantics: non-sequence formats parse once at load; Auto
  Rebuild only re-fires on file change. Documented in the node's tooltip/help.
- Note: while file-driven (pre-bake), user edits to imported shapes are
  **regenerated on rebuild by design** — bake is the persistence path. Stated in
  the panel description (user confirmed 2026-11-06).

## Part 3 — MaskToGeo

**Where:** `Engine/Dev/Scene3D/MaskToGeo.{h,cpp}` (goes into the existing
DevScene3D GLOB — no cmake-list edit beyond regen).
ID `…built-in.MaskToGeo`, grouping "3D", GeoBuilder-style registration.

**Inputs:** one image/roto input — connect a Roto / RotoPaint / **ReadMask**
node; pull shapes via `Node::getRotoContext()` + walk `RotoLayer`/`Bezier` tree.

**Build:** per active shape at time t: `Bezier::evaluateAtTime_DeCasteljau`
flatten → libtess triangulation (RotoContext.cpp:3717 pattern,
gluTessNormal(0,0,1)) → GeoBuilder `Builder` emission into one MeshData
(per-shape `beginShape`); CCW winding; flat shape in the XY plane at z=0.

**Knobs:** Source format mapping (mask-pixel → world scale, default: format
height = 1.0 world unit — Nuke-style normalized 3D space), **Extrude Depth
(default 0 = pure flat shape mesh, user's call 2026-11-06)** — extrude emits
side-wall quads between contour and offset cap + tess caps; TRS knobs with the
exact consumer-read names (translateX/Y/Z, rotateX/Y/Z, scaleX/Y/Z,
uniformScale, rotationPivot).

**Animation:** reads shapes at render time → animated masks (ReadMask file
sequences / baked keyframes / hand-animated Roto) become animated geo;
mesh snapshot hash-keyed per GeoBuilder pattern (curves + knobs in hash).

## Part 4 — Tests + docs for parts 1–3

- Fixtures: vectorNuke's `examples/` (sample.svg, demo-vector-logo.ai, …) —
  re-fetch at implementation time (commit the useful SVG subset; the repo has
  no licence file so only reference them externally — do NOT commit their
  fixtures; generate our own fixtures instead).
- `tools/mask-tests/` (local-only, gitignored like tools/geoio-tests):
  headless `NatronRenderer -t script.py` — ReadMask on fixture → assert shape/
  CP counts + mask PNG render (`^CHECK`-style asserts, DONE_OK/FAILED driver
  per cryptomatte-tests pattern); MaskToGeo → ScanlineRender frame render +
  vertex-count asserts; bake → scrub frames and assert keyframe counts.
- Docs ripple: NODE_REGISTRY.md rows, Engine/Dev/TODO.md move-out, README
  node count, DEV_3D_SYSTEM_CHANGELOG.md entry when MaskToGeo ships.

## Part 5 (lowest) — static parsers: AI / EPS / PDF

- `parsers/PdfContentParser.{h,cpp}` — AI 9+ (PDF-compatible AI) + plain PDF
  content streams; `parsers/EpsParser.{h,cpp}` — legacy PostScript AI +
  Photoshop EPS clipping paths. Static imports (no sequence expectations).
  Best-effort fidelity — stated on the node; fixtures generated by us
  (Illustrator/Photoshop exports collected during testing).

## Part 6 (lowest) — DCC/CAD/GIS parsers

- DXF (LWPOLYLINE with arc bulges, LINE, CIRCLE, ARC, SPLINE), OBJ curves
  (cstype bzr / curv / l), Maya ASCII (.ma nurbsCurve), Houdini (.geo
  PGEOMETRY + JSON-geo attributes.point.P / primitives.Poly), GeoJSON
  (LineString / Polygon). Each small and mechanical; fixtures generated
  from Blender/Houdini/CAD exports per format when implemented.

## Sequence semantics (user decision 2026-11-06)

- Sequence capability is **format-agnostic** (numbered files on the file knob;
  `%f`-style padding resolution via the knob's own sequence logic). Realistic
  per-frame exporters: SVG (Blender Grease Pencil, After Effects shapes), DXF.
  "PDF sequences" excluded by definition — AI/EPS/PDF are static imports.

## Docs + workflow

- Commits per part (`mask: ...` for ReadMask + parsers, `3d: ...` for
  MaskToGeo — match house prefixes), Co-Authored-By: Claude trailer, agents
  commit / user pushes. Branch `feature/import`.
- Rebuild: Engine parts via `build.cmd 04-natron 06-install 07-verify`;
  cmake regen (`cmake .. -G "MinGW Makefiles"` in build-qt6) after adding any
  new file.
- GPL-2.0 attribution: each ported source file carries
  "Derived from vectorNuke (https://github.com/erinwnash/vectorNuke), GPL-2.0"
  in its header comment.
- USER TEMPO (~10–15 working days total, revised ordering): ReadMask core +
  SVG days 1–5, bake day 6, MaskToGeo days 7–9, tests/docs day 10; parser
  phases 5–6 scheduled later at user discretion (~3–5 extra days when pulled).
  Biggest risks in order: (1) RotoContext rebuild-on-frame quirks
  (undo/refresh interplay — unknown until Part 1 testing), (2) PDF/AI
  content-stream variance across exporters, (3) extrude cap geometry.
