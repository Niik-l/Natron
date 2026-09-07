# Natron 3D System — Developer Reference

> **Status: live doc — updated alongside `RB-2.6` work.** Previous revisions are
> in git history (`git log -- DEV_3D_SYSTEM_CHANGELOG.md`).
>
> Companion docs:
> - [`NODE_REGISTRY.md`](NODE_REGISTRY.md) — registered-node list with one-line descriptions
> - [`Engine/Dev/WIKI.md`](Engine/Dev/WIKI.md) — user-facing knob/workflow reference
> - [`Engine/Dev/TODO.md`](Engine/Dev/TODO.md) — running completed/in-progress/blocked lists per subsystem
> - [`GIT_WORKFLOW.md`](GIT_WORKFLOW.md) — branch / commit conventions

## Overview

The 3D system adds a 3D viewport, Cycles renderer, GLSL/MRT scanline renderer,
material system, Alembic I/O, OpenVDB support, lighting, deep compositing, and
a particle simulation pipeline to Natron. All on the `RB-2.6` branch.

Recent milestones:

- **Read: OpenImageIO 3 colorspace names (2026-09-07)** — every Read of an
  8-bit JPEG/TIFF (anything decoded by ReadOIIO; PNG goes through ReadPNG) came
  up with File Colorspace `srgb_rec709_scene`, a text field instead of the menu,
  and `Color space 'srgb_rec709_scene' could not be found` — including the
  albedo Read the Megascans loader creates. OpenImageIO 3.x tags files with the
  built-in OCIO config's canonical names (`srgb_rec709_scene`, `lin_rec709`,
  `g22_rec709`, …) where 2.x wrote `sRGB` / `Linear`; openfx-io's
  `guessColorspace` only knew the legacy hints and wrote the new name straight
  into the knob, which Natron's classic configs (blender / nuke-default) do not
  have. Shipped like this since the first OIIO-3 build (06.14). Fixed in
  `tools/win-build/patches/openfx-io.patch` (documented in BUILDING.md §3b): a
  name→legacy-hint table applied when the active config lacks the raw name, the
  ACES texture-space names added to the sRGB lookup, and a safety net that
  re-guesses as an unlabelled file whenever a hint resolves to a name the config
  lacks — so the bit-depth default applies instead of an error. A related
  pre-existing quirk (an *empty* Read later given a JPEG never guesses at all)
  is recorded in `Engine/Dev/TODO.md`.

- **Megascans asset loader + the material slots it needed (2026-08-21/22)** —
  **Templates → 3D → Megascans Asset…** turns a Quixel Bridge folder into a wired
  graph: pick a mesh from a tris-labelled list, get ReadGeo + Material3D with
  albedo through a Read (gradeable in comp) and the data maps as file paths.
  Handles both layouts Quixel ships — flat "3d asset", and "3dplant" with
  variations in `VarN/` subfolders and maps split across `Textures/Atlas` and
  `Textures/Billboard` — reading the asset `.json` (two different schemas) for
  tris/variation/LOD but discovering files from DISK, since a download contains
  only what was ticked in Bridge. It also absorbs the traps: Megascans meshes are
  authored in **centimetres** (0.01 on the ReadGeo), a connected image input is
  baked to a **linear** `.hdr` so Diffuse Colorspace must say linear, and per-LOD
  normal maps must match the chosen LOD. FBX-only folders are told to re-download
  as Alembic/OBJ — ReadGeo reads those two, and the Autodesk FBX SDK is
  GPL-incompatible for a shipped binary.
  **Material3D** gained Specular, Displacement (+Scale/Midlevel), Opacity
  (+Invert) and Translucency (+Amount) — see NODE_REGISTRY for how each maps onto
  Principled, and why displacement is bump-only and translucency is a mixed
  Translucent BSDF rather than subsurface.
  **Two cache-correctness fixes fell out of it.** The scene hash enumerated a
  hand-written list of material getters, so a graded albedo (baked to a temp file
  whose PATH never changes) and every map knob added since were invisible: the
  render was skipped and the previous frame stayed on screen. It now folds in the
  material node's own `getHash()`, the six input chains' hashes, and the source
  geo node's hash. Separately, `resolveTextureFrame` rewrote the last digit group
  of any texture path with the current frame — silently loading `Normal_LOD1`
  when you asked for `Normal_LOD3`, and mangling the baked-albedo filename into
  one that never existed (the surface rendered pink). Animated textures must now
  say `####` or `%04d`. Same root mistake as the ReadVDB fix below; swept the
  tree afterwards, and the only other copy is in the retired
  CyclesRenderPassManager.

- **ReadVDB single-frame + offset fixes (2026-08-21)** — `resolveFramePath`
  substituted a frame number into any filename ending in digits, so a one-off
  `smoke_0000.vdb` or a versioned `fire_v003.vdb` was read as a sequence and
  failed — everywhere except the 3D viewport, whose preview paths quietly fall
  back to the un-substituted name (which is why it looked like a FastVolumeRender
  bug). `detectSequence` now decides once per file pick by counting matching
  siblings on disk. Frame Offset's slider reached only ±100 (a 1001-based
  timeline over a 0000-based sequence needs −1000) and had no tooltip; both
  fixed. The OpenVDB exception now reaches the persistent message instead of
  stderr, which a GUI launch never shows.

- **Scene3D / Viewport3D quality-of-life (2026-08-22)** — **Uniform Scale** on
  ReadGeo, honoured by `SceneGraph::rebuild`'s generic branch and
  `ScanlineRender::extractGeometry` (only the Alembic-archive branch read the
  knob before, so it was inert on every other node type); **Frustum Display
  Length** on ReadAlembicCamera, since Viewport3D already looked the knob up by
  name and imported cm-scale shots left the 3.0 default gizmo invisible. And a
  **two-viewport gizmo fix**: dragging geo in a look-through view jumped whenever
  a second 3D viewport was open. Two causes — `ImGui::CreateContext()` was called
  without ever storing the pointer or calling `SetCurrentContext`, so both
  widgets shared one context; and ImGuizmo keeps a single FILE-STATIC `gContext`
  (view/projection, screen rect, in-progress drag) that cannot be split per
  widget at all. Each viewport now owns an ImGui context, and the viewport last
  clicked owns the gizmo — so the manipulator appears in one view at a time,
  which is the price of that shared static.

- **Cycles transform motion blur (2026-08-17)** — Cycles blurred deforming
  geometry (vertex positions sampled at shutter open/close) and particles, but
  anything driven by a *matrix* rendered frozen: a keyframed geo node, an
  animated Group3D parent, or a rigid Alembic carrying a baked xform never moves
  a vertex, so there was nothing for the deformation path to see. Object
  transforms are now sampled too — `prepareCyclesPasses()` rebuilds the
  SceneGraph at shutter open/close (the rebuild resolves the whole parent chain,
  so group animation comes along for free) and passes per-node world matrices,
  keyed by `SceneNode::name`, to `CyclesRenderer::setMotionTransforms()`; the
  mesh object then gets `set_motion({open, centre, close})` in the same 3-step
  layout the ParticleInstance branch has always used. The two mechanisms
  compose, so a deforming mesh that is also moving gets both. Static meshes are
  skipped (no motion arrays allocated), the shutter matrices feed the render
  hash so a stale streak can't survive in cache, and the centre-time rebuild is
  done LAST because the provider mesh getters mutate shared state in place.
  Verified headlessly: a keyframed Cube3D differs by 125,152 px (6.04% of frame)
  between blur off/on, while the same scene with the cube static is
  bit-identical. Camera, volumes and lights still take a single centre
  transform — a keyframed camera does not blur yet.

- **Shuffle2, RV integration, camera freeze + first portable releases (2026-08-01..05)** —
  DevShuffle label renamed **Shuffle2** (plugin ID unchanged — projects load).
  Read/Write **Open in RV** hardened: modal error dialogs (persistent messages were
  cleared by the next render before they could be read), exe-existence check,
  [Project] variable expansion, RV's bin dir as working directory; NATRON_RV_PATH
  env var pre-fills the knob. **Freeze at Frame** on ReadAlembicCamera: a built-in,
  non-destructive camera FrameHold (freeze + frame + Use Current Frame button) —
  all time-based getters redirect, so viewport/ScanlineRender/Cycles/Project3D pin
  to the held frame (key deletion never sticks: the bake regenerates from the .abc;
  a 2D FrameHold can't remap provider-getter time). Particle templates: Rain
  removed, remaining prefixed "Temp -". Build/release: portable releases
  natron-2.6-2026.08.02 + .08.05 published (build-all.sh + runbook Part B/C);
  win-build fixes — cycles fork branch pin, alembic dep, un-prefixed OIDN module,
  wgpu auto-detect for FastVolumeRender (was silently missing from the first
  release asset).

- **Large-environment import + normals tooling (2026-07-29..31)** —
  **Alembic:** ReadAlembicArchive self-deadlock on load fixed (archiveMutex was held
  across the archiveReloaded emit; the tree-widget slot re-enters getEntryTree()
  synchronously); ReadAlembicCamera now normalizes scale out of the rotation matrix
  before Euler decompose (Maya exports bake unit-conversion scale — 78.5x on real
  assets — which clamped the asin term into a bogus ±90° rotate Y). **Viewport:**
  adaptive clip planes (far = max(10000, distance×400), near scales with distance;
  zoom range 0.01..50000 — the old fixed 0.1/500 clipped big terrains) and a new
  **Face Orientation** shading mode (front faces blue / back faces red via two cull
  passes, dim wire overlay; works on meshes + Card3D). **Normals:** `Reverse Normals`
  knob on ReadGeo + ReadAlembicArchive flips face winding (and the parallel
  per-face-vertex UVs) at the source, so viewport lighting, ScanlineRender, Cycles
  and Project3D front/back all agree; helper `reverseMeshWinding()` lives in
  MeshData.h. **ScanlineRender:** Project3D/MergeMat projections now render on
  ReadAlembicArchive geo — the archive multi-entry extract path never ran
  applyProjectorMaterial (projection showed in the viewport but not the render);
  layers are built once and stamped onto every entry's GeoData.

- **Deep workflow + stability sweep (2026-07-24/25)** —
  **Deep suite:** DeepRecolor now recolors RGB-less deep inputs (Karma DCMs carry only
  A/Z/ZBack — missing R/G/B channels are appended and filled from the flat Color input);
  DeepReformat declares its resized size as output format metadata (viewer display window
  follows the knob). **Houdini-style point blasting:** drag-select points in the 3D viewport
  and hit Delete — a Blast (Selection mode) is chained onto the displayed provider, seeded
  with the selection; each Delete chains another Blast (disable/delete one to restore that
  batch, Invert flips to keep-selected). Point clouds carry per-point source IDs (the deep
  sample index), so **`Blast::getDeepImage()` rebuilds the originating deep image minus the
  blasted samples** — a blasted cloud plugs straight back into DeepRecolor/DeepMerge/
  DeepFlatten/DeepWrite (use DeepToPoints Density 1.0 when blasting for edit).
  **DeepExpression 2.0:** Nuke-style per-channel expression node (temp-variable rows,
  rgba.red/…/deep.front/deep.back fields, RPN engine — operators, ternary, math functions,
  x/y/frame variables; writing a channel the deep lacks appends it). DeepFog registered.
  Memory-safety fixes: DeepMerge remaps merge inputs by channel NAME with a union output
  layout (previously strided input B with input A's channel count — heap over-read);
  DeepExpression/DeepFog count-pass/fill-pass mismatches (heap overflow) fixed.
  **Particles:** P0 thread-safety — Emitter/Spawn/ReadAlembicParticles gained the solver's
  mutex + immutable-snapshot contract; ScanlineRender multi-sample motion blur offsets a
  private copy instead of mutating the provider's shared snapshot (aborted renders could
  permanently offset the sim); viewport draws particles after opaque geometry (correct
  per-pixel occlusion). **Scene3D provider hardening:** geometry caches (ReadGeo /
  ReadAlembicArchive / Volume3D / ReadVDB) locked against reload-during-render; the seven
  texture-cache nodes (Card/Cube/Cylinder/Sphere3D, Material3D, Project3D, UVProject) now
  publish immutable `shared_ptr<const CachedTexture>` snapshots; Material3D bake paths
  locked. Fixes: Group3D→Dot→child now parents; ReadAlembicTransform FPS defaults to the
  project rate (matches Archive/Camera); ReadVDB loads after project reload; ReadGeo drops
  stale geometry on cleared path. Full audits (deep suite, particle system, provider sweep)
  ran across these sessions; remaining findings + fix tiers tracked in `Engine/Dev/TODO.md`
  and local notes.
  **Particle P1 batch A:** solver knob edits apply while parked on a frame (hash check
  before the fast path + live-slot reset); collision flags accumulate across substeps
  (Spawn On-Collision was missing ~75% of impacts); dead-ID tombstones stop solver-killed
  particles respawning at the emitter. **KillBox bounds volume:** optional Cube3D bounds
  input (full transform, rotated OBBs, gizmo-movable); OBB extents fixed to match the
  drawn wireframe exactly in KillBox AND Blast (both were 2×; Blast also ignored Uniform
  Scale/Size); Cube3D "Viewport Display" knob (Auto/Solid/Wireframe — Auto shows wireframe
  when feeding a KillBox/Blast bounds input). **Templates menu** reorganised into category
  submenus (3D / Particles / Project3D / Deep / HDRI placeholders) with a particle template
  pack: Sparks, Jet Exhaust, Heat Distortion (ST-distort heat-haze rig via IDistort), and
  Rain + Splashes.

- **CameraTracker matchmove suite (2026-07-07)** —
  The CameraTracker node (in-tree since inception but never built) was revived and developed
  across ~10 sessions in a separate worktree, then ingested here in one pass. **CameraTracker**
  (`Engine/CameraTrackerNode.{h,cpp}`): multi-scale bucketed Harris detect → predictive windowed
  KLT (~15 ms/frame at 4K) with adaptive re-detection → wide-baseline keyframes, RANSAC +
  dominant-motion moving-object rejection, Huber bundle with optional path-smoothness prior,
  two-stage solve, global re-triangulation, first-frame re-resect; focal from EXIF / footage
  self-calibration / bundle trust region; planar quad tracker (points-first homography from the
  point tracks, coplanar plane groups, manual-pin DOF ladder); manual tracking with drag
  magnifier, per-track pattern boxes and a native nested track table (**new knob type
  `KnobTracksTable`** + `KnobGuiTracksTable`/`CameraTrackerPanel`); scene orientation (set
  origin/ground/scale from picked points); validated at parity against a commercial matchmove
  ground truth (0.10% of path / 0.0028° rotation). **PointCloudGenerator** (new node): dense
  cloud from footage + a solved camera (multi-keyframe bidirectional tracking, N-view DLT).
  **LensWarp** (new node): undistort/redistort with the solve's exact libmv lens model
  (solve → undistort → pinhole comp → redistort workflow). Solver core: Huber loss +
  convergence fixes + smoothness prior in `libs/libmv` bundle, robust resection; **ceres
  un-crippled** (-O2/OpenMP/Schur — solves in seconds). Core fix: null-deref guard for
  overlay-bearing passthrough groups (`ViewerTabPrivate.cpp`). Viewport: provider-priority
  point-cloud display + selection push to Engine nodes. Known-open: one hard forward-motion
  clip collapses at init; planar corners-leaving-frame oscillation (fully-visible quads fine).

- **MergeMat node + Project3D Crop/Project-On in viewport & render (2026-06-24, pushed `79d1fb988`)** —
  **MergeMat** (`Engine/Dev/Scene3D/MergeMat.{h,cpp}`, new) is classic-Nuke's "Merge Material": a
  `MaterialProvider` with A (foreground) + B (background) inputs, an Operation knob (none/replace/over/
  stencil/mask/plus/max/min) + Mix, that layers multiple **Project3D** projections onto one geo and
  **chains** (MergeMat→MergeMat) for more. The renderer does the per-fragment compositing: ScanlineRender
  flattens the MergeMat tree into ≤4 projection layers (B first, then A over it) and composites them in
  the GLSL fragment shader (single-projector path widened to 4-layer arrays — separate samplers
  `u_projPlate0..3` to dodge dynamic sampler indexing in GLSL 330 — `mm_sampleProj` + `mm_composite`,
  occlusion stays layer-0-only); a lone Project3D = 1 layer so single-projection renders are unchanged.
  The fixed-function 3D viewport mirrors it **multi-pass** (one alpha-blended pass per layer, GL_LEQUAL +
  alpha-test so cropped pixels reveal lower layers), converted across all 5 geo draws (sphere/cube/
  cylinder/ReadGeo-mesh with on-demand vertex normals/Card3D). Same commit finished **Project3D Crop +
  Project-On (front/back)** honoring in BOTH viewport and ScanlineRender — the render shader discards
  culled/cropped fragments (transparent when cropping, grey unprojected geo when not, fixing the Back-mode
  opaque-black-block); the viewport uses clamp-to-border (clear/grey) + alpha-test + per-triangle facing
  cull (clean terminator). LIMITS: 4-layer cap; viewport blend ops over-exact / replace+plus approx /
  rest→over (render does them properly); Cycles falls back to the foreground material (future MixShader).
  Shipped same day (`4604e6b38`/`3bb76910d`): 3D-viewport **splitter-resize fix** + **live update on
  property-panel knob edits** (`Gui::redraw3DViewports`), **Uniform Scale** on all 4 primitive geos, and
  Card3D **Image Aspect** toggle (Nuke parity).

- **FastVolumeRender — real-time GPU VDB volume node (2026-06-18, ingested)** —
  A new built-in node sibling to CyclesRender: a wgpu-native compute ray-marcher that renders
  VDB smoke/fire (and procedural Volume3D) at interactive rates as a stand-in for Cycles
  volumes. Input 0 = a Scene (ReadVDB or Volume3D + Light3D(s), optionally under a Group3D),
  input 1 = optional Camera. Lighting comes from scene Light3D nodes (Distant = sun, Dome =
  ambient, multi-light Point/Spot/Area-beam each self-shadowed, fire-as-light); per-frame
  compression + uniform-buffer cache (look tweaks are draw-only); multi-plane AOVs incl.
  per-light-group passes. Gated `NATRON_FASTVOLUME`, depends on wgpu-native (external — see
  BUILDING.md); verified NVIDIA-only. Files in `Engine/Dev/FastVolume/` + build wiring. See
  `NODE_REGISTRY.md` for the full feature list + GPU-portability notes.

- **Volume3D — procedural cloud overhaul (2026-06-15→16)** —
  The procedural volume grew from "sphere/box" into a cloud toolkit: Sphere/Box base + an
  Enable-Noise toggle layering domain-warped FBM (Warp / Coverage / Edge Erosion / Edge Detail
  / Seed / Wind), a vertical density profile (Base/Top Flatness, Height Falloff) and
  Cumulus/Stratus/Cumulonimbus/Wispy presets. Per-voxel `volumeDensityAt()`, parallel z-slice
  generation, `getShapeHash(time)` caching. Feeds FastVolumeRender (GPU, density-only) as well
  as ScanlineRender and Cycles.

- **3D-viewport soft-splat previews for Volume3D + ReadVDB (2026-06-16)** —
  Both now preview as soft over-blended point-sprite splats (through the particle shader)
  instead of hard additive dots / a bare wireframe bbox. ReadVDB trilinearly samples the real
  density + fire grids on a coarse lattice (cached per path/frame/res via
  `getViewportDensitySamples`), smoke grey + fire via a black-body ramp, with a preview-only
  `viewportDisplayRes` slider.

- **ReadAlembicArchive 3D-viewport: locator fix + controls + frame-selected (2026-06-18)** —
  Fixed the giant locator "diamond" on baked-unit-scale archives: `Viewport3D::drawTransformNode`
  was re-applying `localMatrix` on top of the dispatch's already-applied `worldMatrix` AND
  inheriting the archive's baked scale — now drawn once at a constant per-axis world size.
  Added **Show Locators** + **Locator Size** knobs (new Display page, viewport-only). And **F
  (frame selected)** now fits the selection's real world-space mesh AABB (was the translate
  knob + a fixed distance), so it tracks the archive's user scale.

- **Gui 3D viewport renamed `DevViewport3D` → `Viewport3D` (2026-06-15)** —
  The viewport widget dropped its `Dev` prefix since it's a shipping feature,
  not an RnD node: `Gui/DevViewport3D.{h,cpp}` → `Gui/Viewport3D.{h,cpp}`,
  class `DevViewport3D` → `Viewport3D` (+ `DevViewport3DPrivate`, header guard,
  include, debug strings, on-screen overlay). Pure rename, no behaviour change.
  The `Viewport3DTab` wrapper and the user-facing "3D Viewport" tab label were
  already de-prefixed. (Older entries below pre-date the rename and have been
  swept to the new name for consistency.)

- **ScanlineRender: ambient fill colour control (2026-06-14)** —
  The previously hard-coded 0.15 ambient term is now a 3-component **Ambient**
  colour knob on the Output tab (Shaded mode). It multiplies the surface colour
  (ambient * albedo), so unlit / back-facing areas aren't pure black. Default
  0.15 grey preserves the prior look; set to black for no fill or tint for a
  coloured ambient. Wired into both the kBeauty (mesh/primitive) and kInstance
  (particle-instanced) shaders via a u_ambient uniform; no effect in Flat or
  Wireframe mode. (Transparency toggle is also built but held uncommitted pending
  a semantics review — see TODO.)

- **ScanlineRender: antialiasing level, overscan, and projection modes (2026-06-14)** —
  Nuke-ScanlineRender-parity controls on the Output tab.
  **Antialiasing** (None/Low/Medium/High = 1/2/4/8x MSAA; the sample count was
  previously hard-coded to 4x, now user-selectable and clamped to GL_MAX_SAMPLES).
  **Overscan** (pixels): the output RoD grows on all four sides and the frustum
  widens proportionally (aperture scaled per axis by paddedPixels/basePixels), so
  the original frame stays pixel-identical and the extra pixels reveal more scene
  — for downstream blur / transform / defocus. The output write loop already
  offsets by outBounds.x1, so the padded framebuffer maps 1:1.
  **Projection Mode**: Perspective (default) / Orthographic (parallel, sized by a
  new Ortho Width knob, height from aperture aspect; Depth AOV switched to a
  linear near..far mapping in ortho) / UV (rasterize each surface at its UV coords
  — gl_Position = uv*2-1 — to bake lit texture + Normal/Pref AOVs into the
  texture-map layout) / Spherical (per-vertex equirectangular lat-long from the
  camera, radius drives depth for occlusion; coarse geo distorts at the poles /
  ±180° seam). Projection is centralized in a buildProjectionForMode() helper
  used by all three matrix sites (main, motion-blur sub-sample, previous-frame
  velocity); UV/Spherical run in the kBeauty/kInstance vertex shaders via a
  u_projMode uniform (+ u_view/u_near/u_far for spherical). "Render Camera" mode
  was intentionally dropped — our cameras have no projection-type, so it would
  just equal Perspective.

- **3D viewport: geometry shaded with its connected material/texture (2026-06-13)** —
  Geometry in the 3D viewport was flat/grey unless wireframe was on. Now, when a
  geo has a material or image connected, the viewport previews it textured across
  every draw path — Card3D, Sphere3D, Cube3D, Cylinder3D and meshes
  (ReadGeo / ReadAlembicArchive). Per geo the texture is resolved from a connected
  **Material3D**, the geo's own image input, or a downstream **UVProject**'s
  projection plate (a new `findUVProjectForGeo` walk locates the UVProject so its
  projected UVs show in the viewport, not only in ScanlineRender). Material3D and
  UVProject each gained a `CachedTexture` + `updateCachedTexture()` to render the
  preview (Material3D from its Diffuse input, UVProject from its img input).
  Cached preview textures are scene-linear, so a new `uploadPreviewTextureSRGB()`
  converts them through an sRGB transform on upload — previously the 3D view drew
  raw linear and looked darker than the 2D viewer; now they match. Supports
  primitives, meshes, and common image formats including EXR.

- **3D viewport tabs persist across project save/load (2026-06-13)** —
  A 3D viewport (`viewport3d{N}`) placed in a split pane was lost on reload: the
  tab was never serialized the way histograms are, so the saved pane layout
  referenced a tab that didn't exist on load and its split was dropped. The tab
  script names are now saved in `ProjectGuiSerialization` (new class version 13,
  back-compatible load), and on load each viewport3d is recreated + registered
  *before* `restoreLayout()` — mirroring how viewer tabs survive `wipeLayout` — so
  the layout relocation drops each 3D viewport back into its saved split pane.
  Note: like all workspace restore, this only applies when the
  **"Load workspace embedded within projects"** preference is enabled (off by
  default — when off, no saved layout is restored at all; the save side was always
  correct).

- **Viewport3D survives GL context recreation + timeline frame sync fixed (2026-06-13)** —
  When a 3D viewport is reparented during a workspace/layout restore, Qt can
  recreate the `QOpenGLWidget`'s GL context and call `initializeGL()` again. The
  ImGui font texture and particle shader from the destroyed context were kept
  (guarded by `imguiInitialized`), so `paintGL` bound stale GL handles and crashed
  intermittently on an in-session File→Open (masked by NDEBUG in release builds).
  `initializeGL` now rebuilds the font texture and particle shader program on every
  context init, keeps the ImGui CPU-side context one-time, and guards the refresh
  `QTimer` so it isn't stacked on a second init. Same GL-lifetime class as the
  earlier viewer shader-context crash. Also fixed the timeline sync: the slot
  `onFrameChanged(double)` didn't match `TimeLine::frameChanged(SequenceTime,int)`,
  so the connect silently failed and scrubbing never updated the 3D viewport — the
  slot now matches the signal signature.

- **Cycles: Alembic archive sub-meshes render under the CyclesRenderPass object tables (2026-06-12)** —
  A `ReadAlembicArchive` emits one SceneNode per sub-mesh (named
  `<archiveNode>/<entry/path>`), but the CyclesRenderPass visibility / holdout
  tables key on each geo *node's* script name. So archive entries missed the table
  lookup, fell into the "not in any category" branch, and were set invisible — the
  archive was discovered but didn't render. Archive entries now map back to their
  archive node's script name for the visibility, holdout and reflection-matte
  lookups, so a whole archive inherits its node's row like any other geo.

- **DevStamp routing node — the Stamps wireless-connection tool (2026-06-12)** —
  - A transparent pass-through node for decluttering the graph (inspired by
    Adrian Pueyo's Stamps for Nuke). An **Anchor** taps a source node; **Stamps**
    placed anywhere reconnect to it with their input wire hidden (the node's
    "Hide inputs" knob), so long cross-graph pipes disappear.
  - Unlike a Dot it is a full `EffectInstance` — it has a settings panel, a label,
    the Hide-inputs knob, and `title` / `tags` / `role` knobs (which the Python
    driver sets). `isIdentity → input 0`, so 2D image streams pass through
    unchanged; 3D / Cycles / material / camera streams are carried by topology.
  - **Transparency**: `DotUtils.h` gains `isGraphPassthrough()` (Dot OR DevStamp),
    and `skipDots()` routes through it — so the ~26 typed-input sites that already
    call `skipDots` see through Stamps for free. The 3 explicit Dot-skip sites
    (`SceneGraph` geo chain, `CyclesPassRender` scene discovery, `ParticleModifier`
    force chain) use the helper too. So a Stamp is transparent everywhere a Dot is.
  - Driven by a pure-Python tool (`~/.Natron/stamps.py` + `initGui.py`, currently
    local user config): F8 contextual hotkey (anchor+stamp / another stamp /
    searchable Anchor panel), reconnect-by-title, amber Anchor / green Stamp tints.

- **ColorChartMatch: Normalize, reference-colorspace fix, Target corner-pin (2026-06-12)** —
  Brought to parity with Marco Meyer's mmColorTarget gizmo.
  - **Normalize** — pre-scales the source samples by the Rec.709 luminance ratio of
    the mid-grey patch (target/source) before the least-squares solve, so the match
    fixes chroma without changing the plate's exposure. Mid-grey patch 21 (15 for
    ColorChecker Passport Video); auto-disables itself if that patch is off.
  - **Reference-colorspace bug fixed** — the chart reference (linear sRGB) was only
    converted to the working colorspace inside `knobChanged`, which never fired
    because ACEScg is the default — so reference matching compared an ACEScg source
    against an sRGB target (gamut mismatch, worst in red, vs the Nuke gizmo). The
    patch defaults are now converted to the default colorspace at init.
  - **Target corner-pin** — an independent Target corner-pin (`tgtTo`) plus a
    Source / Target / Corrected view switch. A target chart framed differently from
    the source can now be positioned and sampled on its own corners (previously it
    reused the source corner-pin position). `render`/`getRegionOfDefinition` show the
    viewed input (target fetched via `getImage` with a source fallback, render-clone
    safe); the overlay + corner dragging follow the active view.

- **ReadAlembicArchive: world-space user transform + Rotation Pivot (2026-06-11)** —
  - **Bug.** On an archive with a unit scale baked into its embedded chain (e.g. an
    FBX→Maya export with a 100× cm→m conversion on the top node), translating the
    archive moved the geometry by 100× the knob value, so the viewport gizmo (drawn in
    knob space) disconnected from the geo, worsening linearly with distance from the
    pivot — *"matches at 0, drifts apart as you transform."* Root cause: the user T/R/S
    was applied as the archive **root parent** (innermost in the matrix chain), and the
    multiply convention scales a parent's *translation* by its child's scale.
  - **Fix.** The archive's user T/R/S is now applied in **WORLD space** — post-multiplied
    onto each entry's final world matrix in a dedicated pass (`archiveWorldXforms`) after
    `computeWorldTransforms`, rather than baked into the root's local matrix. Translation
    is added un-scaled (1:1 with the knob regardless of baked unit scale) and rotation/
    scale pivot around the geometry centre in world space. The gizmo, the drag-writeback,
    and the Cycles/viewport render all read this same world matrix, so they stay locked
    together. Scale-agnostic — 1× archives are unaffected (no regression).
  - **Rotation Pivot knob.** New "Rotation Pivot" choice on the Transform tab:
    **Bounding-Box Center** (default — gizmo on the geo, rotate in place; computed per
    rebuild from the archive's own geometry bounds), **Authored Origin** (the archive's
    top-transform origin — for archives with a meaningful root, e.g. a rigged character),
    **World Origin** (legacy pivot around 0,0,0). `SceneNode` gained a `pivot[3]` +
    `SceneGraph::buildTRSPivot`; the gizmo seeds at `translate + pivot` and backs the
    pivot out on writeback. Diagnosed live with a `NATRON_DEBUG_PIVOT` instrument
    (since stripped) that compared the gizmo pivot against the actual rendered bbox
    centre — confirmed the 100× via `abcechobounds` on the source archive.

- **CyclesRenderPass: render-pass disk output + RV / linked-Read review (2026-06-11)** —
  - **Render to Disk.** New Output tab on CyclesRenderPass: a **Render to Disk** button
    + **Start / End / Increment** frame range (auto-filled from the project range)
    writes the pass over the range to `<Output Path>/<Pass Name>/v###/<Pass Name>.####.exr`.
    Output Path is a new knob on **CyclesRenderSettings** (defaults to the project folder);
    a connected CyclesRenderPass inherits it and appends `<Pass Name>/v###/`. The version
    **auto-increments** per render (scan the pass folder for the next free `v###`). One
    multi-layer EXR per frame — Combined/beauty (RGBA) + every enabled AOV as named layers,
    16-bit half / ZIP. Reuses `prepareCyclesPasses`/`executeCyclesPasses` (full res, scene
    slot 0) → `CyclesRenderer::saveMultiLayerEXR`. Shows Natron's native progress bar
    (`AppInstance::progressStart/progressUpdate/progressEnd`) with a Cancel button; the
    range render blocks the UI but `progressUpdate` pumps the event loop so it stays
    responsive (Cancel is polled between frames).
  - **Open in RV** — launches RV / OpenRV on the latest version's sequence (RV Executable
    knob, falls back to `NATRON_RV_PATH`); mirrors the Read node's button.
  - **Import / Update Render + linked Read.** Import Render creates a Read reading the
    latest version, linked to the pass (its script name stored in a hidden persistent knob,
    so the link survives save/reload). Re-rendering does **not** auto-update the Read —
    instead it's flagged outdated: a warning badge on the Read node (persistent message)
    plus a status line on the pass (`OUTDATED v003 → v005`). **Update Render** re-points the
    linked Read to the latest version and clears the flag. This keeps a broken re-render
    from silently replacing a good version in comp — adoption is an explicit click.

- **Dot-input fixes + RenderPass → CyclesRenderPass rename + denoise tick (2026-06-10)** —
  - **Denoise tick (OpenImageDenoise, CPU).** New **Denoise** checkbox on the
    CyclesRenderPass Preview tab (and the previously-dead CyclesRender / CyclesRenderSettings
    Denoise knobs are now wired). `CyclesPassRequest::denoise` → `renderer.setDenoise()`
    → `scene->integrator->set_use_denoise()` with `DENOISER_OPENIMAGEDENOISE` /
    `denoise_use_gpu=false`. Cycles auto-adds the denoising albedo/normal aux passes;
    scene passes default to `PassMode::DENOISED`, so the output driver's "Combined"
    read returns the denoised result (auto-falling-back to noisy when off).
    Deployment gotcha (MSYS2): OIDN's core looks for `OpenImageDenoise_device_cpu.dll`
    but the package ships it `lib`-prefixed, so the CPU device fails to load
    ("unsupported device type: CPU") until an un-prefixed copy is placed next to the
    core / exe — see PACKAGING_NOTES.
  - **Typed inputs now see through Dot routing nodes.** Camera / render-settings /
    material / particle-stream / projection-geo inputs were resolved as
    `dynamic_cast<T*>(getInput(slot).get())`, which returns null when a Dot sits
    between source and consumer (a Dot is none of those types), so the input
    silently did nothing. Added a shared `Engine/Dev/DotUtils.h` → `skipDots()` and
    routed every typed-input resolution through it: CyclesRender / CyclesRenderPass /
    ScanlineRender / Project3D / UVProject / CyclesRenderPassManager / DeepToPoints
    (cameras), the CyclesRenderSettings inputs, the geo-node material inputs
    (Card3D / Cube3D / Cylinder3D / Sphere3D / ReadAlembicArchive / ReadGeo +
    GeoMaterialOverride), and the particle chain (Attribute / Emitter / Instance /
    Merge / Modifier incl. the force-chain walk / Solver / Spawn / WriteAlembic).
    Scene3D / Group3D / GeoMaterialOverride also skip Dots in their frame-varying
    (`getHasAnimation`) check so animated content behind a Dot still re-renders.
    Scene/holdout discovery (`collectSceneNodes`) already saw through Dots.
  - **RenderPass node renamed to CyclesRenderPass** (label, plugin ID, C++ class,
    files `CyclesRenderPass.{h,cpp}`) for consistency with the Cycles* family. Old
    plugin ID `fr.inria.built-in.RenderPass` is aliased in
    `AppManager::getPluginBinaryFromOldID`, so existing projects still load the node.

- **CyclesRenderPass: per-light ray visibility + reflection matte + combined light×color AOVs (2026-06-09)** —
  - **Per-light ray visibility** — the Lights controls moved onto the Objects page
    (behind a "Separate Passes" separator alongside Shadow Catchers / Reflection
    Matte). Each active light is a row `[Active] [Cam] [Refl] [Diff] [Trans]`; the
    four ray toggles drop that light — or the dome environment — from
    Camera/Reflection/Diffuse/Transmission rays per pass while it still lights the
    scene. Dome → `scene->background->set_visibility(mask)` (+ camera bit drives
    `set_transparent`); other lights → light-wrapper `ccl::Object->set_visibility`.
    Threaded CyclesRenderPass → `CyclesPassRequest::lightRayVis` → `CyclesRenderer`.
    Caveat: for non-dome lights the mask hides the light's visible SHAPE in that ray
    type, not its illumination (Cycles has no per-ray use_glossy/use_diffuse socket).
  - **Reflection Matte** — new object category. Flagged objects are re-rendered as
    pure white emitters in a SECOND `renderToBufferWithCameraMultiPass` pass
    (`_impl->emissiveMatteMode`, `createEmissiveMatteShader`) whose Combined is routed
    into a `ReflectionMatte` plane, so the matte reads both directly and in
    reflections. The original one-render shader-AOV plan was a DEAD END: Cycles'
    `svm_node_aov_check` (`kernel/svm/aov.h`) gates every `OutputAOVNode` write on the
    primary camera ray, so a shader AOV can never appear in a reflection — emission
    can. ~2× render time when enabled (only then).
  - **Combined Diffuse / Glossy / Transmission AOVs** — new toggles on CyclesRenderPass and
    CyclesRender. Synthesized post-render as `(Direct + Indirect) × Color` (the kernel
    only writes the direct/indirect/color sub-passes, never a category `PASS_GLOSSY`),
    so each lobe is viewable at beauty-matching levels instead of the blown-out raw
    HDR light pass. Sub-passes are auto-rendered and dropped if not requested on their
    own; one unified `combinedPlanes` loop drives all three.

- **Stability + playback fixes: viewer GL crashes, particle 3D-viewport playback, timeline drift, lighting (2026-06-08)** —
  - **Viewer GL crash fixes** — the viewer's shader programs are parented to the
    `QOpenGLContext`, so context recreation (splitting/docking/reparenting a viewer)
    left the owning `unique_ptr`s dangling, and viewer teardown freed GL resources
    with no current context → SIGSEGV in `Qt6OpenGL` (release builds compile out Qt's
    safety check, so it crashed instead of warning). Fixed: `initializeGL()` releases
    (not resets) the dangling programs and rebuilds on the live context; `~ViewerGL`
    makes the context current before destroying the Implementation.
  - **Particle 3D-viewport playback** — `getParticleData()` returned a deep copy on
    every call (concurrency-safety), but the 3D viewport calls it per repaint on the
    GUI thread, so playback copied all particles per frame on the main thread and
    stalled the player. It now returns the immutable cached snapshot directly on a
    cache hit (no copy). Also serialised `getParticleData()` against the paint/render
    data race, and added a non-blocking "Simulating particles…" banner on cold sims.
  - **Timeline click-drift + playback** — clicking a frame during slow renders could
    land 1-3 frames ahead because a user seek went through the playback-restart path;
    a user seek now clears playback-auto-restart so it renders a single frame, and the
    timeline-change handler no longer re-renders an actively-playing viewer.
  - **CyclesRenderPass lighting** — an empty Active-Lights selection now renders **no**
    lights (was: all); toggling any object/light checkbox invalidates the frame cache
    (`incrementKnobsAge`) so the change shows on every frame, not just after a scrub.

- **Colour management pass: native OCIO viewer + config-aware materials + pass output colourspace + CyclesRenderPass live preview (2026-06-07)** —
  - **Native OCIO-aware viewer** — the viewer colourspace dropdown now lists the
    active OCIO config's `Display / View` transforms (e.g. ACES Output Transforms)
    alongside the built-in Linear/sRGB/Rec.709/BT1886.
    - *Stage 1 (CPU, 8-bit path)* — `scaleToTexture8bits_generic` applies the OCIO
      display transform via `OIIO::ColorConfig` when a view is selected; the
      display/view is folded into the texture-cache key so switching is never stale.
    - *Stage 2 (GPU, 32-bit float path)* — `ViewerGL` builds a dedicated shader from
      OCIO's `GpuShaderDesc` (GLSL 1.2 + LUT textures) and applies the transform on
      the GPU, so ACES views are **real-time** (exposure/gamma/scrub with no
      re-render, full float precision). Falls back to the CPU/built-in path if the
      GPU shader can't be built. In 32-bit float mode the whole dropdown (built-in
      + OCIO) runs on the GPU; in 8-bit it all runs on the CPU.
  - **Config-aware Material3D colourspaces** — the diffuse/emission texture
    colourspace menus are populated from the active OCIO config (new header-only
    `Engine/OCIOColorSpaceUtils.h`) and return the entry id string, so names resolve
    under any config (fixes ACES, where plain "sRGB" is "sRGB - Texture" and was
    silently mis-decoding). Falls back to the legacy fixed list without OCIO.
  - **Per-pass output colourspace (disk renders)** — `CyclesRenderSettings` gains a
    default "Output Colorspace"; `CyclesRenderPassManager` adds a per-pass JSON
    `colorspace` override (per-pass → Settings → fallback). EXR is written
    scene-linear and tagged; PNG/JPG/TIFF are converted scene-linear → target so
    reviews display correctly (a scene-linear target auto-promotes to the config
    display space); data AOVs (depth/normal/position/vector/id/cryptomatte) are
    never converted. Fixes LDR pass output that was raw-linear-in-8-bit. The
    in-graph `CyclesRender` output deliberately stays scene-linear (use a Write node
    for EXR delivery in another space).
  - **CyclesRenderPass live Cycles preview + AOV tab** — `CyclesRenderPass` renders its own live
    Cycles preview reflecting that pass's visibility/holdout/shadow-catcher/light
    setup (new camera + settings inputs, Preview tab) via the shared
    `CyclesPassRender` helper (`prepareCyclesPasses` gained a `sceneInputSlot` param),
    plus an AOV Passes tab (multi-plane output mirroring `CyclesRender`). Wire
    scene+camera and connect to a Viewer — no separate `CyclesRender` needed.
  - **CyclesRenderPass scene-discovery fixes** — discovery now uses the renderer's
    recursive `collectSceneNodes` walk (via `enumerateSceneGeo`/`enumerateSceneLights`),
    so Dot routing nodes are seen through, lights nested in a Group3D are found, and
    container/decorator nodes (Scene3D/Group3D/GeoMaterialOverride) are no longer
    listed as renderable geo.
  - **ACES v4.0.0 chain consistency** — verified end-to-end: Read →
    scene_linear (ACEScg) → Cycles/comp → Viewer/Write, all reading the same `$OCIO`.
    `ColorChartMatch` already correct (config-independent colorimetric matrices,
    ACEScg default, AP0/AP1 primaries unchanged in ACES 2.0).

- **Per-geo material override + archive transform + viewport isolate + node/tree fixes (2026-06-06)** —
  - **GeoMaterialOverride ("Material Override") node** — assigns a different
    material to specific sub-objects of an upstream `ReadAlembicArchive` without
    duplicating geometry. Sits in the geo chain (`archive → Material Override →
    Scene`): input 0 = Geo (passed through), input 1 = Mat. The `surfaces` knob
    lists archive paths (matched exact / ancestor / bare-leaf-name, so the names
    shown in the tree work). A pure decorator — `SceneGraph::rebuild` pre-scans
    override nodes and tags each matching `SceneNode.materialNode` (new field);
    `collectSceneNodes` descends the override's Geo input so the archive behind it
    is still collected. `CyclesRenderer`'s per-node shader pick uses
    `sn.materialNode`; precedence is **downstream/per-pass override → per-part
    override → archive base material**. Disabling the node (`isNodeDisabled()`)
    bypasses the override (geo still flows). Chain several for several materials.
  - **Surface picker UI** — `Gui/MaterialOverridePickerWidget` (added to the node
    panel via `NodeSettingsPanel::initializeExtraGui`, mirroring the archive's
    tree) shows a checkable hierarchy read from the connected archive; ticking
    writes full paths into the `surfaces` knob — point-and-click instead of typing.
    Built without `Q_OBJECT` (lambda connections) so there's no moc to generate.
  - **Render-cache correctness** — `CyclesRender`'s scene hash now incorporates
    the per-part override (a flag + the override material's params, mirroring the
    renderer's precedence), so editing the Surfaces list or the override material
    busts the cache and re-renders (it was serving stale before).
  - **CyclesRender "Refresh Passes" button** (AOV tab) — clears the internal cache,
    re-runs metadata to re-publish the output planes, and forces the viewers to
    re-render, for when a toggled AOV doesn't propagate without scrubbing.
  - **AlembicTreeWidget re-entrancy crash fix** — unticking a surface wrote the
    `excludedPaths` knob, which synchronously reloaded the archive and emitted
    `archiveReloaded` → `refresh()` → `_tree->clear()`, freeing the clicked item
    *inside* its own `itemChanged` signal → use-after-free SIGSEGV in Qt's
    item-view (the long-standing "crash on scrub/untick"). Fixed with an
    `_applyingExcluded` guard so the self-induced reload skips the tree rebuild
    (the entry set is unchanged — only the visibility filter).
  - **ReadAlembicArchive user transform** — new "Transform" tab (Translate /
    Rotate / Scale X/Y/Z + a **Uniform Scale**), applied to the archive's root
    `SceneNode` in `SceneGraph::rebuild` so it propagates to every entry via the
    worldMatrix chain (scale/move the whole archive when its geo comes in at the
    wrong scale). Read by knob name → archives saved before this load at identity;
    animatable; composes with a parent Group3D. Works in viewport + Cycles for
    free (both read worldMatrix; the Cycles cache already hashes it).
  - **3D viewport "Isolate Selected"** — new second toolbar row in
    `Viewport3DTab` (room for future buttons) with an Isolate toggle. When on,
    `Viewport3D` draws only the selected node + its descendants (archive
    sub-entries are `selected/...`); nothing selected = no-op. Viewport-only
    (doesn't affect the Cycles render); grid stays visible.
  - **Read node no longer grows on scrub** — `NodeGui::adjustSizeToContent` now
    derives the width deterministically from content (icon + label, or preview)
    instead of feeding `boundingRect()` back in. `QGraphicsRectItem::boundingRect()`
    includes the pen-width margin, so the old feedback crept the node wider on
    every label change — and a Read node updates its sublabel to the current
    frame's filename every frame, so scrubbing ballooned it. Affects only
    sublabel-bearing nodes (Read/Write/Merge); they now size-to-fit stably.
  - Design + revert notes: `GEO_MATERIAL_OVERRIDE_DESIGN.md`.

- **Geo auto-load on project open + viewport / review QoL (2026-06-04)** —
  - **Geometry & Alembic nodes load on project open** — `ReadGeo`,
    `ReadAlembicArchive`, `ReadAlembicCamera`, and `ReadAlembicTransform` only
    read their file inside `knobChanged` (file-path change or a manual **Reload**
    click). Restoring a saved knob value on project load does **not** fire
    `knobChanged`, so the path was correct but the data was never read — every
    loaded scene needed a manual Reload on each geo node. Each node now overrides
    `onKnobsLoaded()` (Natron's post-deserialization hook, `Node.cpp`) and re-runs
    its existing load path on the restored file. For `ReadGeo` the `.obj`/`.abc`
    dispatch + `isLoading` guard + metadata refresh was pulled out of the
    `knobChanged` lambda into a shared `loadGeoFromFile()` that both paths call;
    `ReadAlembicArchive` also refreshes metadata (so animation flags reach the
    cache). Empty paths stay a no-op, matching the existing `knobChanged` guards.
  - **Backdrop renders as a flat color** — `NodeGraphRectItem` painted a subtle
    top-to-bottom gradient over every node's base color, so a Backdrop's picked
    color read differently from what was chosen. Added an opt-in flat mode
    (`NodeGraphRectItem::setFlat`); `NodeGui::createGui` enables it for backdrops
    only. Regular nodes keep their gradient; a Backdrop now shows the picked color
    exactly, Nuke-style.
  - **Read "Open in RV" button** — mirrors the Write node's RV integration on the
    Read node: an "RV Executable" file knob (pre-filled from `NATRON_RV_PATH`) +
    an "Open in RV" button that launches RV/OpenRV detached on the Read node's
    source pattern (RV parses `####`/`%04d` natively). Falls back to the env var at
    click time and reports a persistent message when the exe path or source file
    is missing. Fast input review without spinning up a viewer.

- **Lighting / scene-traversal fixes + CyclesRender holdout input + Material3D transmission map (2026-06-03)** —
  - **Light rotation in viewport** — `SceneGraph::rebuild`'s Light3D branch was
    calling `buildTRS(..., 0,0,0, ...)`, discarding the `rotateX/Y/Z` knobs, so
    the viewport drew every light un-rotated (most visible on Spot/Area). Now
    reads the rotation knobs like the geometry path. (The Cycles render already
    rotated lights via `buildLightTransform`; this was a viewport-only gap.)
  - **Lights use the SceneGraph world matrix** — the render light loop now sets
    `obj->set_tfm(natronMatrixToCyclesTransform(sn.worldMatrix))` (like geometry)
    instead of rebuilding from local knobs, so grouped/nested lights follow the
    parent Group3D transform and the render matches the viewport. Byte-identical
    for ungrouped lights (`buildTRS` ≡ `buildLightTransform`). `buildLightTransform`
    + its 2 matrix helpers are now dead code (kept; harmless).
  - **Recursive scene-node collection** — `prepareCyclesPasses` /
    `enumerateScene{Lights,Geo}` walked containers only one level deep, so
    `Lights → Group3D → Scene3D → render` never collected the grouped lights/geo.
    New `collectSceneNodes()` descends through every nested Scene3D/Group3D.
  - **Dot pass-through** — scene traversal now sees through `Dot` routing nodes
    (`skipDots()` + Dot handling in `collectSceneNodes`), so geo/lights behind a
    Dot on any input (scene, holdout, CyclesRenderPass) pass through.
  - **CyclesRender "holdout" input (slot 4)** — geo wired here is added to the
    scene AND flagged `set_use_holdout(true)`, punching a transparent matte of
    its shape. Additive (independent of any CyclesRenderPass visMap). Threaded as
    `CyclesPassPrepared::holdoutNames` → `renderToBufferWithCameraMultiPass` /
    `syncSceneWithCamera` (new `holdoutObjects` param). Covers the primitive geo
    loop (Sphere/Card/Cube/Quad); ReadGeo/particles/volumes not yet wired.
  - **Material3D transmission map** — new input 5 "Transmission" + "Transmission
    Map" file knob, baked like the other maps and wired to the Principled
    "Transmission Weight" socket. Glass was already supported via the scalar
    Transmission + IOR knobs (→ `set_transmission_weight` / `set_ior`); this adds
    per-pixel masking. IOR remains scalar-only. (Transmission bounces default 8,
    on CyclesRenderSettings + CyclesRender — bump for thick/stacked glass.)

- **Cycles shadow catcher — working comp pass + the lamp-exclusion fix (2026-05-31)** —
  Shadow catcher (invisible card catches a sphere's shadow into alpha, for comp)
  now works, via the **CyclesRenderPass** node (interactive) and the
  **CyclesRenderPassManager** batch path. Two fixes: (1) the matte output was
  flattened onto a white backdrop with alpha=1 — now output directly as a
  premultiplied comp pass (RGB = objects, ALPHA = shadow density + coverage,
  transparent elsewhere); (2) **the lamp's wrapper object must be
  `set_is_shadow_catcher(true)`** — otherwise Cycles stamps
  `SHADER_EXCLUDE_SHADOW_CATCHER` on it (`scene/light.cpp:331`), the lamp is
  skipped in the catcher's unshadowed reference pass, `color_catcher=0`, and the
  shadow ratio collapses to a flat 1.0. Direct-light shadows DO work with a
  black world; no environment needed. `[Cycles SC]` diagnostics are gated behind
  env `NATRON_DEBUG_SC`. The CyclesRenderPass "everything-starts-excluded" model means
  you must mark the caster (Camera/Trace) as well as the catcher.

- **CyclesRenderPassManager — custom pass-table UI + live preview (2026-05-31→06-02)** —
  The "Passes (JSON)" text box is replaced by a custom spreadsheet **table
  widget** (`KnobPassTable` / `Gui/KnobGuiPassTable` / `Gui/PassTableWidget`,
  registered in both knob factories; backing store stays the same JSON string so
  save/load/undo are unchanged). Increments: table (Name/Type/On/Samples/AOVs/
  File + Add/Remove) → readability styling → per-pass "Selected Pass" detail
  panel → discovered-name **dropdown pickers** for object/light scoping + a
  "Refresh Objects" button (new `discoverSceneObjects()`) → **AOV checkbox** grid
  (custom/token AOVs preserved) → live **multi-plane preview**: "Preview Selected"
  renders the chosen pass to a connected Viewer with each ticked AOV as a Viewer
  layer (`render()` rewritten from a transparent-black sink; secret
  `previewPassIndex`; node made `isMultiPlanar()` + `getComponentsNeededAndProduced`).

- **CyclesRenderPassManager — material override + shadow catcher / holdout / phantom promotion (2026-05-30)** —
  closes the gap with the CyclesRenderPass node's full visibility model. Four
  new JSON fields per pass:
  - `materialOverride` (script name) — Blender-style material override:
    every mesh in the batch renders with this MaterialProvider's shader
    instead of its own. Standard clay-render / shadow-pass pattern.
    Particles + volumes keep their own shaders. Unresolvable name
    aborts the batch with a stderr error (same pattern as
    cameraOverride).
  - `shadowCatcherObjects` / `holdoutObjects` / `traceObjects` —
    semicolon-separated script names of scene geo that should be
    promoted from default-visible to a specific visibility profile.
    Shadow catcher catches shadows while being invisible to camera;
    holdout punches the alpha (clean-plate); trace contributes to
    reflections / refractions / shadows but is invisible to camera
    (phantom). Promotion priority: excluded > shadow catcher >
    holdout > trace > default. The standard shadow pass is
    `shadowCatcherObjects = "Ground"; traceObjects = "Hero"; materialOverride = "WhiteDiffuse"`.

  Implementation: `CyclesRenderer::renderToBufferWithCameraMultiPass`
  + `syncSceneWithCamera` gain a `MaterialProvider* materialOverride`
  parameter; the per-mesh `createMaterialShader` call site substitutes
  the override when present. `CyclesPassRequest::materialOverride`
  threads through `executeCyclesPasses`. The manager's per-batch
  visMap construction expands: when ANY of object scoping / SC / HO /
  TR is set, build a full map covering every scene object with the
  computed profile per the priority table. Batch key extends with
  `|mat=|sc=|ho=|tr=` segments so promotions / overrides force
  separate Cycles sessions. New `enumerateProjectMaterials` diagnostic
  prints every `Material3D` node so users can match what to put in
  `materialOverride`.

  Shadow-catcher output today: the shadow contribution is baked into
  the `Combined` pass via Cycles' `use_approximate_shadow_catcher`
  mode. Catcher-flagged objects appear in `Combined` darkened by the
  shadow factor, multiplied by their (override or own) surface color.
  Exposing the dedicated `PASS_SHADOW_CATCHER` trio
  (raw accumulator + sample count + matte) as a separate AOV is
  deferred — Cycles' pass accessor for that pass does an internal
  divide-by-Combined that bottoms out at 1.0 when the catcher
  accumulator isn't populated, so it needs all three passes wired up
  together plus a documented comp formula. See TODO.
- **CyclesPassRender — prepare/execute split, 5A.2 dedup landed (2026-05-30)** —
  collapses the scene-build / Material3D bake / Cycles invocation
  duplication between `CyclesRender::render()` and the shared helper.
  `renderCyclesPassesForEffect()` is now a thin wrapper around two new
  primitives: `prepareCyclesPasses()` (walks obj input → optional
  CyclesRenderPass → Scene3D/Group3D, builds the scene graph, bakes materials,
  resolves the camera) and `executeCyclesPasses()` (invokes Cycles with
  a caller-owned `CyclesRenderer&`). The new `CyclesPassPrepared` struct
  exposes the resolved sceneGraph, camera params, and CyclesRenderPass pointer
  so callers can inspect/hash before deciding to execute.
  `CyclesRender::render()` now does `prepare → hash → cache check →
  execute`, swapping out ~70 lines of duplicated scene-build + Cycles
  call code. The cache hash is bit-for-bit identical (the hash block
  reads `sceneGraph` / `renderPass` via local aliases bound to
  `prepared`), so existing render behavior is preserved.
  CyclesRenderPassManager keeps calling the wrapper unchanged.
- **CyclesRenderSettings — shared settings node + wiring (2026-05-30)** —
  splits sampling / integrator / DOF / motion-blur knobs out of CyclesRender
  into a dedicated `CyclesRenderSettings` sink node. Both `CyclesRender`
  and `CyclesRenderPassManager` gain a new optional input slot 3 ("settings");
  when wired, the consumer `dynamic_cast`s the input effect to
  `const CyclesRenderSettings*` and pulls values per-frame. (Originally
  scoped with a `CyclesSettingsProvider` abstract interface in the
  CameraProvider/MaterialProvider style, but Qt6's AUTOMOC silently
  rejected Q_OBJECT under multi-inheritance with a non-QObject second
  base — fall back to duck typing on the concrete class for now.) Default values on the Settings node
  exactly mirror CyclesRender's existing knob defaults (samples=6,
  maxBounces=8, diffuse/glossy=4, transmission=8, DOF off, MB off,
  shutterTime=0.5, shutterPosition=Center) so a freshly-created Settings
  node connected to a CyclesRender produces identical output to the
  standalone path.

  On CyclesRender, connecting a Settings node hides the Render / Integrator
  / DOF / Motion Blur knobs via `setSecret(true)` from a new `onInputChanged`
  handler so users can't accidentally edit values that aren't being read.
  Output / AOV / Focus-helper / EXR knobs stay visible (those are
  CyclesRender-only concerns, not shared settings). DOF apertureSize is
  still derived from the active camera's focal length + F-Stop since
  that's a lens property, not a render setting. As a side effect of moving
  integrator resolution above the cache hash, a latent bug is fixed:
  changing bounce knobs now invalidates the cache and triggers a re-render
  (previously they were resolved inside the cache-miss branch and so never
  participated in the hash).

  On CyclesRenderPassManager, the provider applies uniformly to every batch
  in every frame: per-pass JSON `samples` still wins over the provider for
  that pass, but DOF / Motion Blur / Integrator come from the provider when
  connected (PassManager has no local knobs for those). When the Settings
  input isn't connected the renderer falls back to its own defaults (today's
  behavior). The Render-to-Disk stderr dump now includes a "Settings input"
  line saying whether the provider was found.
- **CyclesRenderPassManager — light-group / object / camera scoping + non-EXR output (2026-05-30)** —
  rounds out the per-pass control surface. Four upgrades to the JSON spec
  and one to the OIIO writer:
  1. **`@all_light_groups` magic AOV token** — drops the misleading
     separate-file light-group example in favor of the production
     workflow: beauty carries every light group as layers so comp can
     relight by mixing them. Set a Light3D's `Light Group` knob (e.g.
     `keyLight`), add `"@all_light_groups"` to a pass's `aovs`, and the
     manager expands it at render time to one `Combined_<group>` per
     unique non-empty group found in the scene. Explicit
     `Combined_<group>` entries are still allowed; the expander
     deduplicates.
  2. **Per-pass object scoping** — new `candidateObjects` /
     `excludeObjects` / `soloObject` fields parallel to the light
     scoping. `enumerateSceneGeo` walks the same input-1 → optional
     CyclesRenderPass → Scene3D/Group3D traversal as `enumerateSceneLights`
     and collects every non-Light3D node. `resolveActiveObjects` honors
     solo > candidates > "all minus excludes". When scoped, a full
     `ObjectVisibility` map covering every scene object is built
     (visible → `rayVisibility = 0x7FF`, scoped-out → `isExcluded =
     true`) and routed through `CyclesPassRequest::visMap`. Unscoped
     batches still pass `nullptr` so the renderer skips the
     per-object visibility code path entirely.
  3. **Per-pass camera override** — new `cameraOverride` field takes
     the fully-qualified script name of any `CameraProvider`-derived
     node in the project (Camera3D / ReadAlembicCamera / …). The
     manager resolves it via `getApp()->getNodeByFullySpecifiedName` +
     `dynamic_cast<CameraProvider*>` and routes it through a new
     `CyclesPassRequest::cameraOverride` pointer that the shared
     helper consults before falling back to input slot 2. Empty →
     input-2 fallback; non-empty-but-unresolvable → batch aborts with
     a clear stderr line rather than silently rendering the wrong
     angle. `enumerateProjectCameras` recurses the project tree and
     dumps every candidate name so the user can match what to type.
  4. **PNG / TIFF / JPEG output** — `CyclesRenderer::saveSingleImage`
     joins `saveMultiLayerEXR`. Dispatches on extension; per-pass
     `bitDepth` / `compression` strings are reinterpreted per format
     (PNG 8/16-bit, TIFF 8/16/32-bit + ZIP/LZW/None, JPEG 8-bit RGB
     with quality 1-100). Beauty `Combined` writes RGBA in non-JPEG
     formats; other AOVs write RGB so depth/AO scalar-broadcast AOVs
     don't end up with alpha=0. The manager's per-pass save loop
     dispatches by extension on `resolvedPaths[idx]`: `.exr` → existing
     multi-layer path; other formats loop over the spec's AOVs and
     inject `_<AOVName>` before the extension on multi-AOV passes so
     the per-AOV files don't overwrite each other. Single-AOV passes
     write to the path as-is.

  Batching key now covers `samples + active-lights + visible-objects +
  camera-override` so each scoping config gets its own Cycles session
  while still allowing identical scopings to share. Default seed JSON
  shrinks to two passes (`beauty_main` + `data_utility`); the
  formerly-disabled `lg_key_example` is gone now that
  `@all_light_groups` is the canonical pattern.
- **CyclesRenderPassManager — real Cycles output + batching + frame range + per-pass output (2026-05-29)** —
  graduates the Manager from the synthetic stub MVP to a usable disk
  renderer. New `Engine/Dev/Cycles/CyclesPassRender.{h,cpp}` exposes
  `renderCyclesPassesForEffect(effect, request, outBuffers, errOut)`
  which builds the scene graph from input 1 (walking through optional
  `CyclesRenderPass`, then `Scene3D`/`Group3D` containers), bakes Material3D
  textures, pulls camera params from input 2, and calls
  `CyclesRenderer::renderToBufferWithCameraMultiPass`. Logic duplicates
  the corresponding block in `CyclesRender::render()` for now; the
  cleanup pass (5A.2) will rewire CyclesRender to also call the helper
  and delete the duplicate. The Manager's Render to Disk path: parse
  JSON → filter active set → loop frame range → per frame group active
  passes into batches keyed on shared scene state (currently `samples`)
  → render union of AOVs per batch → demux per-pass and save EXR with
  the pass's own bit-depth + compression. Frame Mode knob picks
  Current / Range / Range No Re-render (the last skips frames whose
  output files already exist — for fast resume). Output resolution
  auto-detected from `app->getProject()->getProjectDefaultFormat()`.
  `CyclesRenderer::saveMultiLayerEXR` gets a new 5-arg overload taking
  `ExrOutputOptions { bitDepth, compression }`; the existing 3-arg form
  still exists for backward compatibility with the manual EXR button on
  CyclesRender.
- **CyclesRenderPassManager — sink node MVP 1-5C (2026-05-29)** — new node
  in `Engine/Dev/Cycles/CyclesRenderPassManager.{cpp,h}`. Owns a
  JSON-backed list of pass specs (multi-line `KnobString`, persisted in
  project files via normal knob serialization). "Render to Disk" button
  parses the JSON, filters the active set by the data.js semantics
  (`enabled && output && !mute && (!soloActive || solo)`), resolves
  dollar-token paths (`$PASS` / `$SHOT` / `$RENDER` from env, frame
  pattern via the same `####` / `%04d` / digit-group logic
  CyclesRenderer already has at `:500-540`), and writes synthetic
  multi-layer EXRs at the resolved paths as proof-of-life for the
  full pipeline. Real Cycles invocation comes in step 5A (refactor
  `CyclesRender::render()` so its scene-state setup + multi-pass call
  can be shared with the Manager). Architecture sketch:
  `RENDER_PASS_MANAGER_DESIGN.md` (local-only).
- **CyclesRender — Mist AOV + display fix for single-value passes (2026-05-29)** —
  added `PASS_MIST` via the standard 5-step add-pass recipe (knob → enabled
  list → plane → standardPasses[] → broadcast). While shipping it caught a
  long-standing display trap: 1-channel "A" planes (Depth / AO) render
  black in the viewer's default RGB display mode because R/G/B are
  undefined. Switched all three single-value AOVs (Depth / AO / Mist) to
  3-channel R/G/B planes with the scalar broadcast in the per-pixel blit
  block. Trap captured in `feedback_natron_single_channel_plane_display.md`
  and the recipe in `reference_cycles_add_pass_recipe.md`.
- **SphericalTransform Faces format (2026-05-29)** — adds 6 per-face cubemap
  inputs (`-Z, +Z, -X, +X, -Y, +Y` canonical order) so HDRIs can be split
  into individual face streams, edited per face, and recombined via a
  second SphericalTransform with `Input Format = Faces`. Each output pixel
  resolves its 3D direction, picks the matching cube face, and samples
  the corresponding input. Missing faces render black. Also surfaces a
  Natron-engine gotcha (`isInputOptional` must be true for slots that
  can legitimately be empty, else `checkTreeCanRender_internal` bails
  before any RoD/render call) — captured in
  `feedback_natron_input_optional_required.md`.
- **Scene-wide motion blur + GLSL closeout (2026-05-28)** — ScanlineRender
  gains a scene-wide multi-sample motion-blur pass (Centered / Start / End /
  Custom shutter offset, temporal jitter) covering camera, geometry,
  particles, and geo instances. Animated Alembic archive paths gain
  sub-frame interpolation for both xform and vertex animation; ReadGeo
  gains the same sub-frame interp. CyclesRender wires
  `ATTR_STD_MOTION_VERTEX_POSITION` for Alembic archive meshes (vertex
  motion blur). The last two legacy fixed-function holdouts inside
  ScanlineRender (volume ray-march + ParticleInstance immediate-mode draw)
  are migrated to GLSL 3.30 core; ParticleInstance now participates in
  MRT (Normal / UV / Pref / Velocity AOVs). ReadVDB's dense 3D texture
  switches from a cubic 64³ to non-cubic `resX/resY/resZ` preserving the
  VDB voxel aspect. ReadGeo + ReadAlembicArchive paths populate per-vertex
  normals so the Normal AOV no longer renders white on file-loaded meshes.
  3D viewport's `F` (Frame Selected) shortcut now also works when looking
  through an editable Camera3D — the camera node's translate is rewritten
  so the framed target sits at framing distance along the camera's view
  direction, preserving its rotation.
- **Phase 3 (2026-05-22)** — ScanlineRender migrated from fixed-function GL to a
  GLSL 3.3 + MRT pipeline. Adds 6 per-pixel AOVs (Depth / WorldPos / Normal /
  UV / Pref / Velocity) for both meshes and particles.
- **Phase 3E (2026-05-23)** — Retired the legacy fixed-function path entirely
  (particles + meshes all GLSL). Added Shading Mode knob (Shaded / Flat /
  Wireframe) and 3D viewport lighting modes.
- **Particle system Phase 2 (2026-04-07)** — 13 particle nodes, refactored
  solver architecture, geo collisions, motion blur (stretch cheat + multi-sample),
  instancing.

---

## Node Inventory

> Detailed one-line descriptions live in [`NODE_REGISTRY.md`](NODE_REGISTRY.md);
> this is a counts-and-categories summary. All registered in
> `Engine/AppManager.cpp` via `registerBuiltInPlugin<>()`.

### 3D Geometry (8 nodes)
| Node | Plugin ID | Description |
|------|-----------|-------------|
| **Sphere3D** | `fr.inria.built-in.Sphere3D` | Tessellated sphere with equirectangular UVs |
| **Card3D** | `fr.inria.built-in.Card3D` | Flat textured quad, aspect from image |
| **Cube3D** | `fr.inria.built-in.Cube3D` | 24-vertex cube with per-face UVs |
| **Cylinder3D** | `fr.inria.built-in.Cylinder3D` | Tessellated cylinder with caps |
| **ReadGeo** | `fr.inria.built-in.ReadGeo` | Single-mesh `.abc` / `.obj` loader |
| **ReadAlembicArchive** | `fr.inria.built-in.ReadAlembicArchive` | Multi-mesh Alembic archive (full hierarchy) |
| **ReadVDB** | `fr.inria.built-in.ReadVDB` | OpenVDB volume loader (Cycles PrincipledVolume + FastVolumeRender GPU source); soft fire/smoke splat viewport preview (`viewportDisplayRes`) |
| **Group3D** | `fr.inria.built-in.Group3D` | Groups 3D objects with unified transform |

### 3D Scene & Render (7 nodes — FastVolumeRender gated `NATRON_FASTVOLUME`)
| Node | Plugin ID | Description |
|------|-----------|-------------|
| **Scene3D** | `fr.inria.built-in.Scene3D` | Aggregates 3D objects for rendering |
| **CyclesRenderPass** | `fr.inria.built-in.RenderPass` | Multi-pass filter: visibility, holdout, shadow catcher, light selection |
| **ScanlineRender** | `fr.inria.built-in.ScanlineRender` | GLSL 3.3 + MRT rasterizer; 6 AOVs; Shading modes; particle render; projects Project3D materials; bg-input res/format conform |
| **CyclesRender** | `fr.inria.built-in.CyclesRender` | Cycles path tracer (NATRON_CYCLES); 12 AOVs |
| **FastVolumeRender** | `fr.inria.built-in.FastVolumeRender` | Real-time GPU VDB volume renderer (wgpu-native compute ray-marcher); gated `NATRON_FASTVOLUME` — see NODE_REGISTRY |
| **Project3D** | `fr.inria.built-in.Project3D` | Re-enabled 2026-06-17 (b1d83b512) as a camera-projection **material shader** (Nuke parity) — projected by ScanlineRender onto a geo's mat input |
| **UVProject** | `fr.inria.built-in.UVProject` | Rewrite mesh UVs (6 projection modes incl. STW perspective) |

### Camera & Lighting (4 nodes)
| Node | Plugin ID | Description |
|------|-----------|-------------|
| **Camera3D** | `fr.inria.built-in.Camera3D` | Camera with T/R, focal length, aperture, DOF |
| **ReadAlembicCamera** | `fr.inria.built-in.ReadAlembicCamera` | Import animated camera from `.abc` |
| **ReadAlembicTransform** | `fr.inria.built-in.ReadAlembicTransform` | Import animated transform/null/locator from `.abc` |
| **Light3D** | `fr.inria.built-in.Light3D` | Point/Spot/Area/Distant/Dome light with HDRI |

### Materials (2 nodes)
| Node | Plugin ID | Description |
|------|-----------|-------------|
| **Material3D** | `fr.inria.built-in.Material3D` | Standalone PBR material with texture map inputs |
| **Volume3D** | `fr.inria.built-in.Volume3D` | Procedural cloud volume — Sphere/Box + Enable-Noise (warp/coverage/erosion/edge-detail/seed/wind), vertical profile (base/top flatness, height falloff), presets; renders via FastVolumeRender (GPU) / ScanlineRender / Cycles |

### Deep Compositing — registered (17 nodes)
DeepRead, DeepWrite, DeepFlatten, DeepMerge, DeepRecolor, DeepSlice, DeepHoldout,
DeepFromImage, DeepGrade, DeepReformat, DeepCrop, DeepToPoints, DeepTransform,
DeepExpression, DeepColorCorrect, DeepDefocus, Blast.

Tier 3 (19 more) exists in code but is unregistered pending further testing —
see `NODE_REGISTRY.md`.

### Particles (16 nodes, "Phase 2 complete")
ParticleEmitter, ParticleGravity, ParticleDrag, ParticleTurbulence,
ParticleTurbulence2D, ParticleWind, ParticleKillBox, ParticleAttract,
ParticleVortex, ParticleSpawn, ParticleSolver, ParticleInstance, ParticleMerge,
WriteAlembicParticles, ReadAlembicParticles, ParticleAttribute.

### Channel (1 node)
DevShuffle.

---

## Source Layout

All 3D-system code lives under `Engine/Dev/` + `Gui/` on `RB-2.6`. Authoritative
file inventory is `git ls-files Engine/Dev/ Gui/Dev*` — counts shift as work
lands. Categories:

| Path | Contains |
|------|----------|
| `Engine/Dev/Scene3D/` | 3D geometry nodes (Sphere/Card/Cube/Cyl), readers (ReadGeo, ReadAlembic*, ReadVDB), scene + camera + light + material providers, ScanlineRender, UVProject, Project3D |
| `Engine/Dev/Cycles/` | CyclesRender Natron node + CyclesRenderer API bridge (gated by `NATRON_CYCLES`) |
| `Engine/Dev/Deep/` | All Deep* nodes + PointCloudData + DeepUtils |
| `Engine/Dev/Particles/` | Particle solver + emitter + 11 modifier/instancer/spawn nodes + shared ParticleData header |
| `Gui/Viewport3D.{h,cpp}` | ImGuizmo-based 3D viewport (camera orbit, gizmos, shading modes, look-through camera, axis grid) |
| `Gui/Viewport3DTab.{h,cpp}` | Panel wrapper (toolbar + viewport + timeline integration) |
| `Gui/ShuffleWidget.{h,cpp}` + `KnobGuiShuffle.{h,cpp}` | DevShuffle UI |
| `Gui/ImGuizmo/` | ImGui + ImGuizmo vendored (MIT licensed) |
| `tools/glsl_mrt_testbed/` | Opt-in standalone GLSL 3.3 + MRT feasibility tester (`BUILD_GLSL_TESTBED=ON`) |

Build wiring lives in the top-level `CMakeLists.txt` (Qt6 toggle,
`NATRON_CYCLES`, `BUILD_GLSL_TESTBED`) and each subdir's `CMakeLists.txt`
(source globs + per-target deps).

---

## Key Interfaces

### MaterialProvider (Engine/Dev/Scene3D/MaterialProvider.h)

Abstract interface implemented by all geometry nodes + Material3D:

```cpp
class MaterialProvider {
    virtual void getMaterialBaseColor(double time, double& r, double& g, double& b) const = 0;
    virtual double getMaterialRoughness(double time) const = 0;
    virtual double getMaterialMetallic(double time) const = 0;
    virtual double getMaterialSpecular(double time) const = 0;
    virtual void getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const = 0;
    virtual double getMaterialTransmission(double time) const = 0;
    virtual double getMaterialIOR(double time) const = 0;
    virtual std::string getMaterialTextureFile() const = 0;

    // PBR texture map slots
    virtual std::string getMaterialNormalMapFile() const;
    virtual std::string getMaterialRoughnessMapFile() const;
    virtual std::string getMaterialMetallicMapFile() const;
    virtual std::string getMaterialEmissionMapFile() const;
    virtual double getMaterialNormalStrength(double time) const;

    virtual bool hasMaterialInput() const;
    virtual MaterialProvider* getConnectedMaterial() const;
};
```

### CameraProvider (Engine/Dev/Scene3D/CameraProvider.h)

Abstract interface implemented by Camera3DNode + ReadAlembicCamera:

```cpp
class CameraProvider {
    virtual void getCameraPosition(double time, double& tx, double& ty, double& tz,
                                   double& rx, double& ry, double& rz) const = 0;
    virtual double getFocalLength(double time) const = 0;
    virtual double getHAperture(double time) const = 0;
};
```

---

## Architecture Decisions

### Matrix Convention
ALL matrices are **column-major** (OpenGL convention). ImGuizmo's demo math functions (LookAt, Perspective, Cross, Dot, Normalize) are used verbatim throughout Viewport3D to prevent convention mismatches.

### Cycles Integration
- **Hybrid approach**: Raw GL viewport for interactive work + Cycles path tracer for final renders
- **CPU-only first**: Embree for BVH acceleration, OIDN for denoising
- **Camera convention**: Cycles looks +Z (not -Z like OpenGL) — forward column negated in camera matrix
- **Light convention**: Cycles lights emit along -column2 of Object transform

### Viewport (Viewport3D)
Replaced the original hand-rolled 3D viewport with this ImGuizmo-based rewrite:
- Spherical camera (camYAngle, camXAngle, camDistance) → LookAt matrix
- ImGuizmo::Manipulate for translate/rotate/scale gizmos
- ImGuizmo::DecomposeMatrixToComponents/RecomposeMatrixFromComponents for T/R/S ↔ matrix
- Orbit (middle-mouse), pan (shift+middle), zoom (scroll)
- W/E/R keys for gizmo mode, F for frame selected

### ReadGeo (Alembic)
- Uses `reinterpret_cast<const float*>` for raw Alembic data access (avoids Imath struct layout crashes)
- `getTop().getName()` returns "ABC" — path navigation skips root name
- UVs via `getExpanded()` on `IV2fGeomParam` — gives per-face-vertex UVs matching faceIndices order
- Fan triangulation for n-gon faces when passing to Cycles

### Cache System
- Natron's ImageKey cache ignores time for "non-animated" nodes
- CyclesRender uses a hidden animated knob + `setIsFrameVarying(true)` to force per-frame cache invalidation
- Scene hash built from ALL camera, light, geometry, and material parameters
- **ParticleSolver frame cache (Phase A skeleton + Phase B wired-in, landed 2026-05-27)** — multi-frame in-RAM cache on each `ParticleSolver` instance, keyed by frame number. Stored in `ParticleSolverPrivate` as `std::map<int, CachedFrame>` (where each entry carries a deep-copied `ParticleDataPtr` + per-frame `knownIDs`) + `std::mutex` + `frameCacheHash` (U64 sentinel) + `frameCacheBytes` (memory accounting). New "Cache" page on the node: Cache Simulation / Max Cache (MB) / read-only Cached Frames + Cache RAM labels / Clear Cache button. `getParticleData()` consults the cache: hash mismatch → wipe and resim; exact-frame hit → restore + skip integration; miss → resume from nearest cached frame ≤ endFrame via `upper_bound`, integrate forward, cache each frame as it goes. Hash combines `input0->getHash()`, `input1->getHash()`, and the solver's own knob values. Debug coloring (showCollisions tint) is applied after cache reads, so the cache stores uncolored state. **No LRU eviction yet** — Phase C adds the actual cap-and-evict on `Max Cache (MB)`.

### Undo
- Natron has TWO undo stacks: per-node and global (NodeGraph)
- Gizmo transforms push `GizmoTransformUndoCommand` to NodeGraph's global stack for Ctrl+Z

---

## ScanlineRender GLSL/MRT Pipeline (Phase 3 + 3E)

The legacy fixed-function rasterizer (`glBegin` / `glEnd` / fixed-function
texture pipeline) was retired in 2026-05-22 → 2026-05-23. Current path:

- **Shaders** — `kBeautyVert` / `kBeautyFrag` for meshes, `kParticleVert` /
  `kParticleFrag` for particles. Volume rendering still uses its own shader
  pair under `Engine/Dev/Scene3D/ScanlineRender.cpp` (volume migration is
  deferred; volumes rely on fixed-function `gl_ModelViewProjectionMatrix`).
- **Geometry path** — interleaved VAO/VBO (15 floats: pos / uv / stw / normal /
  prevPos) + IBO + uniform-driven MVP. `renderGeoObjectGlsl` runs once per
  GeoData per motion-blur sample.
- **Particle path** — per-mode CPU-side accumulator into a single VBO
  (18 floats: pos / color / normal / uv / pref / velocity), drawn as
  GL_POINTS / GL_LINES / GL_TRIANGLES depending on mode. AOV uniform gates
  mirror the mesh shader.
- **STW projective texturing** — UVProject's Perspective mode emits 3-component
  (s, t, w) attribs. Shader detects via `u_hasTexture == 2` and does the
  perspective divide in-fragment (`v_stw.xy / v_stw.w`).
- **MSAA** — 4x via multisampled FBO + blit-to-resolve. AOV attachments share
  the same MSAA count.

### Multi-pass Output: 6 AOVs

Declared on plane -1 via `isMultiPlanar() = true` + `getComponentsNeededAndProduced`.
Allocated lazily — only enabled AOVs get MRT attachments.

| AOV | Source | Notes |
|-----|--------|-------|
| `depth.Z` | GL depth attachment (last sample) | Linear camera-space distance. Bg = camFar. |
| `world_position.xyz` | Reconstructed via `inverse(proj * view)` × NDC | Same depth caveat — single-sample. |
| `Normal.xyz` | Fragment shader from world normal | Normalized in-shader. |
| `uv.uvw` | Per-vertex UVs | Particles emit (0,0) except Sprite static. |
| `Pref.xyz` | Object-space position (= in_pos for meshes) | Particles use world pos as stable per-particle ID. |
| `Velocity.xyz` | Screen-pixels-per-frame motion vector | Re-extracts geometry + camera at `time - 1`. |

Per-attachment blend override (`glBlendFunci` for slots 1–4 with
`GL_ONE / GL_ZERO`) prevents AOV stacking across overlapping particle fragments
while keeping beauty additive on slot 0.

### Shading Modes

Knob on the Output page, applied to mesh draws (particles always use their
own pipeline and aren't affected):

| Mode | Behavior |
|------|----------|
| Shaded (default) | Per-pixel N.L diffuse + 0.15 ambient. Light from `Light3D` if connected, else a camera-relative headlight (Maya default convention). |
| Flat | No lighting — texture / per-vertex color only. |
| Wireframe | Solid white GL_LINES from triangle indices. |

`Viewport3D` has the same modes (`eWireframe / eFlat / eShaded /
eShadedWire`) — lighting in the viewport is per-face flat against a fixed
eye-space light direction. Procedural primitives use their per-vertex normals;
ReadGeo/Alembic use a cross-product face normal (CCW-from-outside assumption).

---

## Particle System (Phase 2 complete)

13 nodes (see Inventory). Architecture refactor 2026-04-06. Key bits:

- **Solver loop** — sub-stepped (default 4 sub-steps/frame) for accurate
  collision response. `Substeps > 1` scales the continuation displacement by
  the substep size so post-bounce particles don't overshoot.
- **Particle struct** — `(p.px/py/pz, vx/vy/vz, r/g/b/a, size, age, life,
  bounceCount, collided)` — 13 fields, packed for cache. `id` is stable across
  frames for deterministic per-particle data.
- **Geo collision** — plane / box / sphere collide helpers + ReadGeo BVH path
  (cached per-frame). `bounceParticle` reflects velocity, optional restitution.
- **Motion blur** — two independent paths:
  - Stretch cheat (`Motion Blur` knob): single-sample, geometry-stretch fake.
    Only active when `Motion Samples = 1`. Beauty-only — see WIKI.md for
    AOV interaction notes.
  - Multi-sample (`Motion Samples`): sub-frame renders → CPU-side averaging.
    Works on all modes, all AOVs.
- **Cycles parity** — `ccl::PointCloud` for sprites with per-particle vertex
  color + emission; native instancing for ParticleInstance (shared
  `ccl::Mesh`, one `ccl::Object` per instance, 3-step motion attribute).

---

## Build Dependencies

### Required (always)
- Qt6, Python 3, Boost, Cairo, PkgConfig (existing Natron deps)

### Optional — Alembic Support
```cmake
find_library(ALEMBIC_LIBRARY NAMES Alembic)
find_path(ALEMBIC_INCLUDE_DIR NAMES Alembic/AbcGeom/All.h)
# Sets NATRON_HAVE_ALEMBIC ON/OFF
# Enables ReadGeo and ReadAlembicCamera Alembic loading
```

### Optional — Cycles Support
```cmake
# Set NATRON_CYCLES=ON and provide NATRON_CYCLES_DIR
# Adds: WITH_EMBREE, WITH_OPENIMAGEDENOISE, WITH_OPENSUBDIV, WITH_OCIO, WITH_PUGIXML
# Links: Cycles libraries (session, scene, device, kernel, etc.)
```

### MSYS2 Packages (for Windows/MinGW build)
```bash
pacman -S mingw-w64-x86_64-alembic mingw-w64-x86_64-openimageio
# Cycles: mingw-w64-x86_64-embree mingw-w64-x86_64-openimagedenoise
#         mingw-w64-x86_64-opensubdiv mingw-w64-x86_64-opencolorio
```

---

## Smoke-Test Checklist

Quick post-build sanity sweep — not exhaustive. Detailed regression tests live
in `Engine/Dev/TODO.md` per subsystem.

### 3D Viewport
- [ ] Open Viewport3D tab — grid, axes visible, orbit/pan/zoom work
- [ ] Create Sphere3D — ImGuizmo handles appear when selected, W/E/R cycles
- [ ] Switch Shading dropdown through Wireframe / Flat / Shaded / Shaded+Wire — light orbits with camera in Shaded modes
- [ ] Camera look-through + gate outline draws letterboxed to camera aspect

### ScanlineRender
- [ ] Sphere3D + ScanlineRender → renders with default Shaded mode (no Light3D needed — fallback headlight)
- [ ] Add Light3D via Scene3D → render updates to use the placed light
- [ ] Toggle Shading Mode → output changes between lit / flat / wireframe
- [ ] Enable Depth + WorldPos + Normal + UV + Pref + Velocity AOVs → 6 planes appear at output
- [ ] All 4 particle modes render (Point/Disc/Sphere/Sprite); AOVs work on particles too

### Alembic + ReadGeo
- [ ] Single `.abc` mesh loads via ReadGeo, UVs visible
- [ ] Multi-mesh archive loads via ReadAlembicArchive, transforms chain correctly
- [ ] `.obj` mesh loads via ReadGeo, group dropdown populated

### Cycles
- [ ] Camera3D + Scene3D + CyclesRender → final-quality render
- [ ] All 12 Cycles AOVs available, cache invalidates per-frame on animation
- [ ] Volume3D + ReadVDB → renders volumes (CPU device required)
- [ ] Particle instancing renders thousands of objects efficiently

---

## Known Issues / TODO

1. **Volume shader path still uses fixed-function matrices** — `glMatrixMode` + `gl_ModelViewProjectionMatrix` survive in the volume passes. A future phase migrates them to explicit uniforms (after which `glMatrixMode`/`glLoadMatrixf` can be removed entirely from ScanlineRender).
2. **Particles + Depth/WorldPos** — only static Sphere mode contributes to depth-based AOVs (others disable depth writes for translucent blending). See `FUTURE_FEATURES.md` for the depth-only pre-pass design.
3. **Material3D 2D input baking** — experimental; renders connected 2D nodes to temp `.hdr` files via `renderRoI()`. Needs more testing with complex node chains.
4. **ScanlineRender PBR** — does not consume materials. Only texture from input 0 is sampled. Material system Phase 2+ deferred (see TODO.md).
5. **ScanlineRender shadows** — not implemented (Cycles only).
6. **Sprite particles texturing** — not implemented (legacy was flat-colored; GLSL migration preserved that). Recipe in `FUTURE_FEATURES.md`.
7. **OFX plugins** — When running from build directory, set `OFX_PLUGIN_PATH` to load Read/Write nodes.

---

## Development Timeline

| Date | Milestone |
|------|-----------|
| 2026-03-23 | Initial clone, Qt6 build, architecture decision (Path C Hybrid) |
| 2026-03-24 | Cycles standalone build, link into Natron, CyclesRender node, Light3D |
| 2026-03-27 | Animation cache fix (3 bugs) |
| 2026-03-28 | Material system (MaterialProvider, Material3D, per-object shaders) |
| 2026-03-29 | Viewport3D (ImGuizmo), undo, light icons, spot/area knobs, code cleanup |
| 2026-03-29 | ReadGeo rewrite, Alembic Cycles rendering, UVs, material support |
| 2026-03-29 | PBR texture maps (Normal, Roughness, Metallic, Emission) |
| 2026-03-30 | Texture tab reorganization, Material3D 2D inputs, documentation |
| 2026-04-06 | Particle solver architecture refactor (Phase 2) |
| 2026-04-07 | ScanlineRender particle modes (Point/Disc/Sphere/Sprite), 4x MSAA, multi-sample motion blur, instancing |
| 2026-04-08 | ReadVDB + OpenVDB integration; PrincipledVolume fire rendering; Cycles CPU-device NanoVDB |
| 2026-05-21 | Camera aspect-match + look-through + gate + viewport shading dropdown |
| 2026-05-22 | Phase 3A–3D: GLSL/MRT migration, 6 per-pixel AOVs (Depth/WorldPos/Normal/UV/Pref/Velocity) |
| 2026-05-23 | Phase 3E: particles migrated to GLSL, fixed-function path retired, AOVs on particles |
| 2026-05-23 | Shading Modes (Shaded/Flat/Wireframe) for ScanlineRender + viewport, headlight fallback |
| 2026-05-24 | Animated Alembic vertex meshes (ReadGeo + Archive), ReadGeo user TRS honored, viewport two-sided imported-mesh lighting, Camera3D V aperture project-aware by default |
| 2026-06-06/07 | Viewer native OCIO display/view (CPU then GPU float path); CyclesRenderPass live preview + AOV tab |
| 2026-06-15 | ColorChartMatch overhaul, Camera3D frustum display, 3D viewport on-demand rendering (Pause/Refresh, no 30fps timer) |
| 2026-06-17/24 | Project3D rebuilt as camera-projection material (Crop/Project-On in render + viewport); MergeMat (classic-Nuke material layering, ≤4 layers) |
| 2026-07-07 | CameraTracker matchmove suite ingested (detect/track/solve, PointCloudGenerator, LensWarp, KnobTracksTable, libmv/ceres core) |
| 2026-07-24 | Karma DCM fixes (DeepRecolor/DeepReformat), Houdini-style Delete-to-Blast point deletion + blasted-cloud→deep round trip; deep-suite audit + crash-trio fixes; DeepExpression 2.0; DeepFog registered |
| 2026-07-25 | Particle audit P0 thread-safety + viewport particle depth-ordering; provider sweep G0+G2; KillBox Cube3D bounds + Cube3D wireframe display; Templates menu + particle template pack |
| 2026-07-26 | **Cycles native deep EXR output** (Blender PR #154410 ported to standalone Cycles + full Natron wiring: Deep toggle, DCM alpha-only mode, merge thresholds, versioned `<pass>_deep.####.exr` sequences, CyclesRenderPass Output tab redesign, Deep Merge comp-tree button); PointsToParticles adapter node; DeepToPoints camera-rotation unprojection; DeepRead `####` sequence support; deep side-channel resolves through Dots |
| 2026-07-26 | Lock Transform (Camera3D + Alembic readers + CameraTracker auto-lock; panel/gizmo/look-through guarded); generic per-knob user lock (right-click → Lock Parameter, separate flag from enabled, serialized, curve-editor/dope-sheet gates) |
| 2026-07-27 | Nuke-style format management (New/Edit/Delete Format in every format dropdown, custom formats project-wide); drag-to-insert on plain node drag (no Ctrl); SphericalTransform multithreaded + abort-aware + size guards; Templates > HDRI > Basic HDRI (lat-long cube-face edit rig with custom LatLong4K format) |
| 2026-07-27 | **Particle shading pass**: ParticleMaterial node (PBR material on particles + tint/emission overrides); FIXED silently-black per-particle color in Cycles (reserved `vertex_color` std-attr name -> `particle_color`); velocity-based Cycles particle motion blur (was index-mismatched re-sim -> criss-cross streaks); Card3D collision as thin aspect-correct slab; ParticleAttribute v2 (Emission section, Random/Flicker sources, per-ID Variation with bias, hue jitter) + tabbed layout with Shape presets, on-demand curve editor, Range dropdown |
| 2026-07-27 | **Particle perf pass** (audit-driven): all force loops + ParticleAttribute fan out across cores (ParticleParallel.h helper); collision knobs resolved once per frame instead of ~13 evals + 11 string lookups per particle per substep; Turbulence2D halved (computed every sample twice); solver preVel scratch reuse; ParticleAttribute curve LUTs; emitter per-particle knob eval hoisted; wind gust seeded by particle ID (index-reshuffle pop fixed); emitter Card3D mask honors Uniform Scale |
| 2026-07-28 | Cycles cancelled-render guard (aborted renders no longer cached as black frames — also the old "empty Combined" mystery); ParticleEmitter Rotate X/Y/Z (shape + direction turn together, composes with the transform input, gizmo-compatible); clipboard/serialization knob factory gains the four custom Dev knob types (DevGradient/TracksTable/DevShuffleRouting/CyclesPassTable) — copy-paste of ParticleAttribute, CameraTracker, DevShuffle and CyclesRenderPassManager was broken |
| 2026-07-29 | **Particle trails** (Houdini particle-trail style): ScanlineRender Trail mode (multi-frame ribbons through real past positions, head/tail width-alpha-color taper, contextual knobs) + Cycles trails via ParticleMaterial (native curve ribbons, per-key attributes); ParticleSpawn **At Age (burst)** trigger (per-ID staggered mid-air bursts — fireworks/crackle) |
