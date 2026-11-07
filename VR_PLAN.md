# VR / 360 — game plan (started 2026-11-06)

**Status 2026-11-07: Parts 1, 2, 3, 5a, 5b DONE** — the VR group + icon, the VR Viewer
node + pane (through the viewer-parity pass `ccdde305a`), and the six VR tool nodes
(see "Part 2/3 — VR tools, DELIVERED" below). Remaining: Part 4a/4b (writer metadata).

User request (workspace session 2026-11-06): a CaraVR-inspired VR/360 feature family —
a dedicated VR viewport, a set of VR effect nodes, spherical metadata on output. Ordered
**viewport-first** so the user's own VR footage can be inspected and later work tested
from day one, in this order:

1. **`VR Viewport`** — a dedicated dockable pane (like the 3D viewport) that renders an
   attached node's frame as an interactive equirectangular look-around view. Actually
   split into 5a (MVP) and 5b (persistence + stereo), both before everything else.
2. **`VR` node group + icon** — own icon in the left vertical tool box.
3. **`VRBlur`** — seam-safe latlong blur (the CaraVR C_Blur analog).
4. **`VRGlow`** — reuses the VRBlur machinery.
5. **Spherical metadata on the writers** — GSpherical for MP4/MOV (WriteFFmpeg), GPano
   XMP / EXR attributes for image sequences (WriteOIIO).

NOT in scope (deliberately): re-doing SphericalTransform — the built-in
(`Engine/Dev/Transform/SphericalTransform`) already covers equi ↔ cubemap
(LL-Cross / 6x1 / 3x2 + face inputs) ↔ rectilinear ↔ 4 fisheye models ↔ mirror ball,
and the VR parts build on it. The CaraVR stitching family
(C_CameraSolver / C_Stitcher / C_GenerateMap / C_STMap / C_Tracker /
C_DisparityGenerator) is future work — each is a multi-week project.

## What is already established (checked 2026-11-06)

- **Dedicated-viewport integration points** (copy the 3D viewport one-to-one):
  pane-menu entry in `TabWidget::createMenu` (Gui/TabWidget.cpp:437, "New 3D viewport")
  + slot `newViewport3DHere` (TabWidget.cpp:772–780); factory/registry
  `Gui::addNewViewport3D` (Gui20.cpp:628–643), remove (645–655), list (673–679),
  member list in `GuiPrivate.h:169–170`; project save
  (`ProjectGuiSerialization.cpp:143–150`, names in ProjectGuiSerialization.h:658/694)
  and load — recreate before `restoreLayout` (`ProjectGui.cpp:590–603`) via
  `appendTabToDefaultViewerPane`. Tab registration is script-name keyed
  (`Gui::registerTab` Gui20.cpp:436–446; relocation in `restoreTabWidget`
  Gui10.cpp:120–139); `TabWidget::removeTab` has the per-type close if/else at
  TabWidget.cpp:1170–1183 (Viewport3DTab has no branch — match that).
- **ViewerGL has no projection hooks** (pure 2D ortho zoom/pan, ViewerGL.cpp:3422;
  OCIO LUT path only) — VR preview must not live in the 2D viewer; the
  Viewport3D architecure is the host. The image-texture upload precedent to copy is
  `uploadPreviewTextureSRGB` (Viewport3D.cpp:429–534).
- **Node-group + icon machinery**: `PLUGIN_GROUP_*` defines (`Global/Macros.h:288–305`),
  group ordering list + `NAMED_PLUGIN_GROUP_NO` (Gui20.cpp:79, 86–102), label→icon
  fallback `getPixmapForGrouping` (Gui20.cpp:106–143), enum→path switch
  (`GuiApplicationManager.cpp:278–322`, icon set pinned Set3 via
  `NATRON_ICON_SET_FADED_COLOURS`, Macros.h:283–285). Icon PNGs live in
  `Gui/Resources/Images/GroupingIcons/Set3/` and are compiled in via
  `GuiResources.qrc` (reload = rebuild). Toolbox population is automatic
  (`findOrCreateToolButton` Gui20.cpp:852–987 → ordered by `getToolButtonsOrdered`
  989–1023 → `GuiPrivate::addToolButton` GuiPrivate.cpp:549–564); right-click graph
  menu follows (`NodeGraph35.cpp:486–492`).
- **Built-in node checklist** (proven by Cryptomatte): EffectInstance subclass in
  `Engine/Dev/<Family>/`, plugin-ID macro in `Engine/EffectInstance.h:125–196`,
  register in `AppManager.cpp:1685–1749` (icon arg), CMake GLOB for a new family dir
  (`Engine/CMakeLists.txt:47–62`) + re-run `cmake .. -G "MinGW Makefiles"` for new
  files, NODE_REGISTRY.md row. CPU models to copy: `SphericalTransform` (simple
  single-plane), `LensDefocus` (multi-plane hygiene, CRYPTOMATTE_PLAN.md:55–59).
  GPU precedent on this fork is wgpu compute (`Dev/PostFX/DefocusGpu.*`), not `_GL`
  suffix nodes — deferred for VR v1.
- **Metadata facts**: the graph carries NO arbitrary per-image metadata
  (NodeMetadata has no map; CRYPTOMATTE_PLAN.md:36–39) — so an IngestVRMetaData node
  cannot feed the writer; the knobs live on the writers themselves. The plain `Write`
  node is a wrapper (`Engine/WriteNode.cpp`) that embeds the real writer by file
  extension and recreates its knobs in the GUI (WriteNode.cpp:75, 178–187, 900–909;
  FFmpeg special-cased as video writer at 968) — so new writer pages show up inside
  plain Write for both video and image-sequence outputs.
- **WriteFFmpeg** (`openfx-io/FFmpeg/`, canonical — NOT the drifted
  `openfx-arena/OpenFX-IO` copy): container tags via `av_dict_set(->metadata, ...)`
  at WriteFFmpeg.cpp:2452–2460, header written at 3931 (tags must be set before
  `avio_open` 3897 / header 3931); knob defines at 170–574; key defines go to
  FFmpegFile.h:71–88. Same movenc muxer for mov and mp4.
- **Spherical Video metadata (YouTube/Google)**: GSpherical XMP blob
  (`Spherical=true`, `Stitched=true`, `StitchingSoftware`, `ProjectionType=
  equirectangular`; optional `StereoMode=top-bottom|left-right`). MP4 accepts the
  XMP in `udta`; newer libavformat may natively write the `sv3d` spherical box —
  **spike** on the pinned ffmpeg 8.1.2 decides: (a) XMP tag via av_dict_set vs
  (b) native `AVSphericalMapping` stream side data. Acceptance: `ffprobe` reports
  spherical side data (`Spherical Mapping` / `Spherical Video`).
- **Image sequences (user correction: not video-only)**: the GPano "Spherical
  Panorama" XMP namespace is the stills convention (ProjectionType,
  FullPanoWidthPixels/HeightPixels, CroppedAreaLeft/Top/WidthPixels/HeightPixels,
  StereoMode mono|left-right|top-bottom). OIIO 3.1.15 (pinned) has XMP round-trip
  (`encode_xmp`/`decode_xmp`, `OpenImageIO/tiffutils.h:227–241`) driven by ImageSpec
  attributes; WriteOIIO already sets free-form `spec.attribute(...)` per frame
  (WriteOIIO.cpp:1126–1157). EXR: free-form header string attributes
  (GPanoProjection / GPanoStereoMode / ...) — lossless, round-trippable by our nodes.
  **Spike**: pin the exact OIIO XMP embedding path (dedicated `xmp` spec attribute vs
  encode_xmp-recognized attribute names) for JPEG/PNG/TIFF.
- **Tests + docs conventions**: `tools/cryptomatte-tests/` (bash driver taking the
  renderer path, `NatronRenderer -t script.py`, `oiiotool --diff --fail` asserts,
  `^CHECK` grep, DONE_OK/FAILED) and `tools/particle-tests/run_hide_proto_check.sh`;
  ffprobe/ffpath precedent `ReadNode.cpp:828–847` (`getFFProbeBinaryPath`).
- **Inventory reference (CaraVR)**: C_AlphaGenerator, C_Bilateral, C_Blender, C_Blur,
  C_CameraSolver, C_ColourMatcher, C_DisparityGenerator, C_GenerateMap,
  C_MetaDataTransform, C_RayRender, C_SphericalTransform, C_Stitcher, C_STMap,
  C_Tracker. VR Blur is ours; the solve/stitch/track class stays future work.

## Part 5a — VR Viewport MVP

**Where:** `Gui/ViewportVRTab.{h,cpp}` (the PanelWidget, modeled on Viewport3DTab)
+ inner `QOpenGLWidget` `ViewportVR` (same file pair or a second pair — one pair for
the MVP, mirroring Viewport3D's own-widget-within layout). Gui/ is CMake-globbed;
new files → re-run cmake.

**Model to copy:** ViewerTab's hidden-viewer ownership (each VR tab owns its own
engine Viewer instance pointed at the chosen node — gives the full render/refresh
pipeline per frame for free, including current time), Viewport3DTab's pane chrome
(toolbar + TimeLineGui strip, Viewport3DTab.cpp:63–334), Viewport3D's texture upload
path for the GL side.

**Integration points:** the four Viewport3D touch points listed under
"established", with scriptName `viewportv{N}` / label `VR Viewport{N}`.

**MVP behavior:**
- Equirectangular (2:1) input only for the MVP; mouse-drag = yaw/pitch look-around,
  wheel = FOV. DR-sensitive defaults: FOV 65–75°, drag = ~fov per viewport width.
- Toolbar: attached-node chooser; later 5b adds eye selection, cubemap detection,
  persistence. No 3D-scene rendering in the pane — it is an image-stream viewer.
- User's own VR footage (loaded via a plain Read) is the first test asset.

## Part 5b — VR Viewport completion

- Project save/load (the serialization touch points), over-under stereo eye toggle,
  side-by-side + cubemap input detection, polish pass (exposure knob, preferences
  integration if the module warrants it — decide after first GUI pass).

## Part 1 — `VR` node group + icon

**Where:** `Global/Macros.h` (`PLUGIN_GROUP_VR "VR"`),
`Gui/Gui20.cpp` (NAMED_PLUGIN_GROUP_NO +1, namedGroupsOrdered, getPixmapForGrouping
else-if), `Gui/GuiApplicationManager.cpp:278–322`
(`NATRON_PIXMAP_VR_GROUPING → GroupingIcons/Set3/vr_grouping_3.png`), PixmapEnum in
`Gui/MTypes.h`, new PNG in `Gui/Resources/Images/GroupingIcons/Set3/` (Set3 faded
palette, 360-sphere glyph), `GuiResources.qrc` entry.

Every VR node registers with grouping `VR` (fallback icon applies automatically when
`getPluginGrouping()` returns "VR"; explicit icon path optional per
AppManager.cpp:1728–1741).

## Part 2/3 — VR tools, DELIVERED 2026-11-07 (six nodes, expanded from the planned two)

The planned VRBlur/VRGlow became a six-node tool family sharing a seam-safe core in
`Engine/Dev/VR/VRFilterCore.{h,cpp}` (namespace `VRFilterCore`, pure float-buffer API,
QtConcurrent row batching with abort polls between batches). Core rules everywhere:
horizontal kernel WRAPS modulo the image width (no 0/360 seam), vertical CLAMPS at the
poles, and `latitudeComp` grows the horizontal sigma by 1/cos(latitude) (capped 8x) so
blurs stay isotropic ON THE SPHERE; large sigmas go through an internal
decimate→blur→upsample path, and per-row sigma variation (latitude comp) is handled by
processing rows in octave runs of similar sigma. Nodes (all `Engine/Dev/VR/`, IDs
`fr.inria.built-in.VR*`, grouping `PLUGIN_GROUP_VR`, `supportsTiles()` false, float-only,
modeled on VectorBlur/LensDefocus):

- **VRBlur** ("Blur VR") — Size / Size x Factor / Quality (kernel extent 2-4 sigma) /
  Latitude Compensation / Mix.
- **VRSharpen** ("Sharpen VR") — unsharp mask `src + Amount*(src - blur(src, Size))`, same
  knob family.
- **VRGlow** ("Glow VR") — Kawase-style bloom: Threshold + Softness isolation, a stack of
  kernels with geometrically increasing sigma (Size, Size*Ratio, ... Iterations kernels)
  evaluated on wrap-aware pyramid levels (each level picked so its local sigma lands near
  1.5 px), Combine Plus/Screen, Glow Only, Gain, Mix.
- **VRExpoGlow** ("ExpoGlow VR") — Fusion-style exponential glow: Size/4 pre-blur then a
  pyramid cascade weighted Falloff^level (no large convolutions at all). Same combine
  knobs.
- **VRDepthBlur** ("Depth Blur VR") — per-pixel sigma via slice compositing (6 discrete
  sigmas at half res, per-pixel lerped by depth). Depth Source choice: a depth-like channel
  of the Source (auto-detected by name — simplified LensDefocus depthScore — with a
  populated Depth Channel menu + refreshLayerChoices on input/knob change) or the optional
  Depth input. Size / Focus / Range drive the ramp; without depth the node passes through.
- **VRLightWrap** ("Light Wrap VR") — blur(Background) masked to the foreground edge and
  combined (Plus/Screen) over over(Source, Background). Edge Softness = 0 → classic
  (1-alpha) mask everywhere the fg is absent; > 0 → band = max(0, blur(1-a) - (1-a)),
  confining the wrap to just inside the fg edge. RoD overridden to the Source's.

Registration: plugin-ID macros in `Engine/EffectInstance.h` (after VRVIEWER),
`registerBuiltInPlugin<...>` in `AppManager.cpp` (VR block), rows in NODE_REGISTRY.md.
Tests: `tools/vr-tests/` (`make_pano_ref.py` pure-stdlib 16-bit PNG fixtures +
`vr_check.py` Read→VR node→Write EXR graphs + `run_vr_check.sh` oiiotool assertions):
seam-left == seam-right strip means (wrap, latComp on AND off), seam blob == mid blob,
blur spread, sharpen overshoot, glow/expoglow corner isolation + halo spread, depth-driven
near/far peaks, light-wrap edge brightening + band confinement. 12/12 PASS (DONE_OK).

The old Part 2/3 notes (VRBlur/VRGlow first cut) are superseded by the above; the knob
philosophy (rows mirroring the stock Blur node) carried over into the six-node family.

## Part 4a — WriteFFmpeg spherical metadata (video)

**Where:** `openfx-io/FFmpeg/WriteFFmpeg.cpp` + `FFmpegFile.h` key defines.

**Knobs** (new collapsible "VR / 360" page near kParamWriteNCLC, define block
~line 361): `writeVR360` (enable, default OFF), `vrProjectionType` (equirectangular —
read-only for now), `vrStereoMode` (none / auto-from-views / top-bottom /
side-by-side), `vrStitched` (default true), `vrStitchingSoftware` (default "Natron").

**Write path:** `av_dict_set(&_formatContext->metadata, kSphericalVideoTag, xmp)`
right after the hardcoded tags at 2452–2460 (video-stream StereoMode tag on the
stream metadata near 2943 as needed), before avio_open/header (3931). Valid for
mov + mp4 both.

**Spike:** pick between the XMP-tag route and native `AVSphericalMapping` stream
side data (libavformat sv3d writing) — whichever satisfies ffprobe on the pinned
ffmpeg 8.1.2; document both outcomes in this file.

**Repo/pin note (one-time):** this tree moves off upstream pinning — push the openfx-io
state to the user's own fork `alejandroua/openfx-io` and re-point `sync-env.ps1`
(OPENFX_IO_REF → the fork ref), same remote pattern as the Natron fork itself.

## Part 4b — WriteOIIO spherical metadata (stills/sequences)

**Where:** `openfx-io/OIIO/WriteOIIO.cpp` (same VR page pattern; the built-in Write
surfaces both pages when the extension is video vs stills).

- **EXR / any format:** free-form header attributes per frame —
  `GPanoProjection` (equirectangular), `GPanoStereoMode`, `GPanoStitchingSoftware`,
  plus the numeric GPano set (FullPano/CroppedArea) — trivial, lossless.
- **XMP-capable formats (JPEG/PNG/TIFF/WEBP)**: GPano "Spherical Panorama" XMP.
  Spike first (see established facts) to pin the OIIO 3.1.15 embedding path.
- StereoMode auto-derives from project Views when 2 views exist, manual knob overrides.

**Tests:** extend `tools/vr-tests/run_vr_meta_check.sh`: (4a) mp4 → ffprobe spherical
side data present, GPano/GSpherical keys correct; negative case (page off → no side
data). (4b) EXR → `oiiotool --printinfo` attribute assert per frame; TIFF/JPEG → XMP
grep (oiiotool --printinfo / exiftool if present).

## Docs + workflow

- FEATURE_REQUESTS.md intake entry (done with this plan); when each part ships:
  NODE_REGISTRY.md rows, Engine/Dev/TODO.md move-out, README node count,
  DEV_3D_SYSTEM_CHANGELOG.md entry.
- Commits per part (`vr: ...`, `openfx-io: ...`), Co-Authored-By: Claude trailer,
  agents commit / user pushes. Rebuild Engine parts via
  `build.cmd 04-natron 06-install 07-verify`; openfx-io parts via `build.cmd 05-plugins`.
- USER TEMPO (~8 h/day): viewport MVP day 1, 5b day 2, group+icon day 3,
  VRBlur days 4–5, VRGlow day 6, metadata 4a+4b days 7–8, docs ripple —
  ~8–10 working days total. Biggest risk: Part 5 feel-tuning (taste loop).
