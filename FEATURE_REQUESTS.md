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

## Cryptomatte — DONE 2026-11-04 (decoder node + Cycles encoder with object/material/asset/custom layers; see CRYPTOMATTE_PLAN.md)

Requested 2026-11-03. The old community Python plugin was unusable. Cycles can produce
Cryptomatte passes natively but the fork's RenderPass / CyclesRender nodes do not wire
them yet (noted in `Engine/Dev/TODO.md`), and nothing decodes them.

Sketch:
- Encoder side: expose Cryptomatte (object / material / asset, depth levels) on the
  pass node so renders carry the `CryptoObject00..` layers plus the manifest in the EXR
  header.
- Decoder node `Cryptomatte`: input a multi-layer EXR; a layer selector; a matte list
  of names (text) plus a viewer overlay where clicking or box-dragging adds/removes
  IDs; output the combined coverage as alpha (and optionally the preview colours). The
  manifest must come from the EXR header: Natron does not pass arbitrary metadata
  through the graph, so the node either reads the header itself from the upstream
  Read's filename (what the old plugin did) or we add a metadata side channel. ID
  hashing is the standard MurmurHash3 of the name, so a name-only workflow works even
  without a manifest.
Size: about a week for the decoder with click-picking, plus a few days to wire the
Cycles passes.
