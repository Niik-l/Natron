# Community feature requests

Requests from users (Discord / GitHub) that are not yet scheduled. One section per
request: what was asked, what exists today, a sketch of how to build it, and a rough
size. Move an item to `Engine/Dev/TODO.md` when work starts; delete it when it ships.
Ideas that came from inside the project live in the local `FUTURE_FEATURES.md`.

## Alt+H: hide input connections of the selected nodes — DONE 2026-11-03

Requested 2026-11-03. The node graph already had a per-node "Hide inputs" knob and a
View-menu action for it, but the action had no default key and the graph's key handler
never looked for it. Now bound to Alt+H (Node Graph group in Preferences > Shortcuts,
rebindable): toggles the knob on every selected node, state taken from the first one.

## GridWarp (Nuke-style grid warper)

Requested 2026-11-03. Nothing comparable exists: openfx-misc has no grid or spline
warper, and the fork's warps are all parametric (Transform, CornerPin, IDistort, Lens
Distortion).

Sketch: a native node in `Engine/Dev/` with two N x M grids (source and destination,
default 5 x 5) as animatable point knobs, each vertex carrying tangents so cells are
bicubic (Coons / Ferguson) patches rather than bilinear quads. Rendering is an inverse
map: for each output pixel find the destination cell, invert the patch by a few Newton
steps seeded from the previous pixel, sample the source with the Transform filter set.
Add a Mix slider that interpolates the two grids (morph), a Destination/Source overlay
toggle, "Copy source to destination", and subdivide / add-row-column tools. The viewer
overlay (drag vertices, tangents, whole rows, box select) is most of the work; the
Roto / CornerPin overlays are the models to copy. Size: 1 to 2 weeks for a usable v1.

## OFlow / Kronos (optical-flow retime and vector motion blur)

Requested 2026-11-03. Today there is only frame-blend retiming (Retime / TimeBlur) and
no motion vectors from 2D footage; the only vectors in the fork come from the 3D side
(Cycles vector pass, particle velocity).

Sketch, in three nodes sharing one flow engine:
1. `VectorGenerator`: dense optical flow between frame N and N +/- 1, written to
   forward / backward vector planes. A native DIS-style estimator (pyramidal inverse
   search + variational refinement, about 1k lines, CPU) avoids a heavyweight
   dependency; OpenCV or an ONNX RAFT model are the alternatives if quality is not
   enough.
2. `OFlow`: retime by splatting / warping both neighbours along the flow with
   occlusion weighting and a blend fallback where confidence is low. Speed curve or
   output-frame mapping, like Kronos.
3. `VectorBlur`: motion blur from a vector plane (also useful for Cycles and
   ScanlineRender renders, which already output vectors).
Quality tuning (edge handling, occlusions, flicker) is the long tail. Size: 2 to 3
weeks for a credible v1, more to approach Kronos.

## VR / 360 feature family (CaraVR-inspired) — scheduled, plan in VR_PLAN.md

Requested 2026-11-06 by the user: a dedicated VR viewport (like the dedicated 3D viewport),
a set of VR effect nodes (VR Blur etc., the CaraVR analog; SphericalTransform already
exists and stays), a VR node group with its own icon in the left vertical tool box, and
spherical (YouTube/GPano) metadata — on video (WriteFFmpeg) AND image-sequence outputs.

Sketch + established integration points + build order live in `VR_PLAN.md`
(viewport-first so the user's own VR footage is the test rig). The graph carries no
per-image metadata, so the knobs live on the writers (an IngestVRMetaData-before-Write
node cannot inform them; an optional validate-only Ingest node stays on the future list).
Size: ~8–10 working days at 8 h/day. Not in scope: the CaraVR stitching/solver/tracking
class (future work).

## ReadMask + MaskToGeo (vector-mask import, 3D conversion) — scheduled, plan in READMASK_PLAN.md

Requested 2026-11-06 by the user: a native vector-mask importer based on vectorNuke
(https://github.com/erinwnash/vectorNuke — GPL-2.0), ported to Natron C++ sources with no
Python bridge and no dialog/menu work, plus a MaskToGeo node that turns Roto/ReadMask
shapes into Scene3D geometry (MeshProvider). Ordered mask→3D loop first (ReadMask SVG,
bake, MaskToGeo), media-format parsers (AI/EPS/PDF, DXF/OBJ/Maya/Houdini/GeoJSON)
back-ported at lowest priority.

Sketch + established integration points + build order live in `READMASK_PLAN.md`.
Size: ~10–15 working days for the core loop; parser phases extra when pulled.

## Cryptomatte — DONE 2026-11-04 (decoder node + Cycles encoder; see CRYPTOMATTE_PLAN.md)

Requested 2026-11-03; the old community Python plugin was unusable. Shipped in six commits
(1356055b6 .. baa8cdf6a, pushed 2026-11-04):
- `Cryptomatte` decoder node (Keyer): layer selector over the input's Crypto* planes, matte
  list by name or `<hex>` ID, viewer picking (add / remove), preview colours, manifest from the
  EXR header through the upstream Read (or a sidecar file), unpremultiply / remove-channels,
  panel laid out like Nuke's gizmo. Verified against Blender's reference EXR (9/9).
- CyclesRender / the pass node write CryptoObject / CryptoMaterial / CryptoAsset plus a free
  **CryptoCustom** layer (Cycles fork 689977518); every 3D node has a Cryptomatte page
  (Crypto Asset / Crypto Custom, inherited down Group3D); ParticleInstance can give every
  instance its own ID (Crypto Per Instance). Manifests go in-graph and into the saved EXR
  header; Blender's compositor decodes our files identically (encoder 16/16, Blender 2/2).
- Still open: the pass node's Cryptomatte path is wired but untested; Scatter3D rendered as
  points / hair stays one ID per node.
