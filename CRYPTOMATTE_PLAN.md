# Cryptomatte — game plan (started 2026-11-03)

Community request (FEATURE_REQUESTS.md). Two deliverables, in this order:

1. **`Cryptomatte` decoder node** — reads Cryptomatte layers from any multi-layer EXR
   (Blender, Arnold, Redshift, Houdini, our own Cycles later), lets the user pick objects by
   name or by clicking in the viewer, outputs the matte.
2. **Cycles encoder** — CyclesRender and the pass node emit `CryptoObject/Material/Asset`
   layers with a manifest, so renders made inside Natron work with the decoder and with
   other packages.

## What is already established (checked 2026-11-03)

- **Spec.** ID = MurmurHash3_32(name) reinterpreted as float32, with the exponent nudged when
  it would be 0 or 255 (`util_hash_to_float`). Layers are `<Prefix>00`, `<Prefix>01`, ... each
  RGBA: `(r,g)` = (id, coverage) of rank 2n, `(b,a)` = rank 2n+1. The matte for a set of IDs is
  the sum of the coverages whose id is in the set, over all ranks. Manifest = JSON
  `{"name":"%08x"}` in the EXR header under `cryptomatte/<first 7 hex of hash(prefix)>/manifest`,
  with sibling keys `name` (= layer prefix), `hash` = "MurmurHash3_32", `conversion` =
  "uint32_to_float32". Sidecar `manif_file` is allowed by the spec (not needed for v1).
- **Reference file.** Blender 4.5 Cycles render with Object + Material + Asset at 6 levels (3
  RGBA layers each), `Combined`, `Depth`, 320x240, 16 spp: scratchpad
  `cryptomatte/ref/blender_crypto_0001.exr`, built by `cryptomatte/make_ref.py` (objects
  Cube_Left / Sphere_Mid / Suzanne_Right / Ground, materials RedMat / GreenMat / BlueMat, asset
  HeroGroup parenting the cube and sphere). Re-runnable headless with
  `"C:/Program Files/Blender Foundation/Blender 4.5/blender.exe" -b --python make_ref.py -- out.exr`.
  Copy both into `tools/cryptomatte-tests/` when the tests land.
- **Hash verified** against Blender's manifest for all tested names (pure-Python MurmurHash3 in
  the session log): `Cube_Left` 2ce615c5, `Sphere_Mid` b1051abe, `HeroGroup` fce19326, `RedMat`
  7b9cad36.
- **Natron ingest.** The Read node exposes every crypto layer as an RGBA plane named exactly like
  the EXR layer (`ViewLayer.CryptoObject00` ...). Read -> Write with the plane selected is
  **bit-exact** (oiiotool diff PASS at 32f). **Caveat:** routing a crypto plane through the OFX
  Shuffle node corrupted the IDs (values halved), so the decoder must fetch planes itself with
  `getImage(..., &planeDesc, ...)` and users should be told not to Shuffle crypto layers.
- **No metadata path.** Natron's OFX host carries no arbitrary metadata (NodeMetadata is clip
  prefs only) and WriteOIIO sets fixed keys only. The manifest therefore has to be read from the
  EXR header by the decoder itself, and written by our own EXR writer on the Cycles side
  (`CyclesRenderer::saveMultiLayerEXR`, which already drives OIIO directly).
- **Cycles checkout (`D:/_vfx_claude_2025/cycles`) has the full kernel side:** `PASS_CRYPTOMATTE`,
  `Film::set_cryptomatte_passes/depth`, `ObjectManager::get_cryptomatte_objects/assets`,
  `ShaderManager::get_cryptomatte_materials`, `util_murmur_hash3` + `util_hash_to_float`. IDs
  hash `Object::name`, `Object::asset_name`, `Shader::name`, none of which Natron sets today.
  Known bug there: the manifest string replaces its trailing comma with `}` and clobbers the `{`
  when the scene has no objects; guard for empty.

## Part 1 — `Cryptomatte` node

**Where:** `Engine/Dev/Keyer/Cryptomatte.{h,cpp}` (new dir: add the GLOB + APPEND lines in
`Engine/CMakeLists.txt`), plus `Engine/Dev/Keyer/MurmurHash3.{h,cpp}` (public-domain
MurmurHash3_x86_32, vendored so the node does not depend on NATRON_CYCLES). Plugin ID
`PLUGINID_NATRON_CRYPTOMATTE` = `fr.inria.built-in.Cryptomatte`, grouping `PLUGIN_GROUP_KEYER`,
registered in `Engine/AppManager.cpp` next to the other Dev nodes.

**Model to copy:** `Engine/Dev/PostFX/LensDefocus` for multi-plane input (`isMultiPlanar`,
`getComponentsNeededAndProduced`, plane list cached under a mutex, refreshed from
`onMetadataRefreshed` / `onInputChanged` / `onKnobsLoaded`), `Engine/Dev/Channel/DevShuffle`
for populating a plane choice from `getAvailableLayers`, `ColorChartMatch::calculateMatrix` for
fetching an input image outside render() on a GUI action, LensDefocus pick mode for the overlay.

**Inputs:** Source only.

**Knobs:**
- `Layer` (choice): one entry per crypto *set* found on the input: planes whose ID matches
  `^(.*)(\d\d)$` with `00` present, grouped by prefix (`ViewLayer.CryptoObject`,
  `ViewLayer.CryptoMaterial`, ...). Default: the first `*Object*` set. Stores the prefix string
  so it survives reconnects.
- `Matte List` (string, multi-line): names and/or `<hex>` ids separated by commas. Names resolve
  through the manifest; a `<2ce615c5>` token resolves without one. Unknown names are flagged in
  the node's info and ignored.
- `Picker` (bool "Pick in viewer"): while on, left-click in the viewer toggles the object under
  the cursor in the Matte List (adds if absent, removes if present); left-drag toggles all IDs
  with coverage > 0 in the box. Overlays get no modifier keys, so toggle rather than Shift/Ctrl
  semantics. Clicking an uncovered pixel does nothing.
- `Preview` (choice): Off / Colors. "Colors" = per-pixel sum over ranks of
  `coverage * hash_to_rgb(id)` (the standard keyable-surface look), with the selected objects
  tinted brighter so the pick is visible.
- `Output` (choice): "Matte in Alpha" (RGB passed through from Source Color) / "Matte in
  RGBA" / "Preview in RGB, Matte in Alpha" (default while Picker is on).
- No un/premult knob in v1: matte only, premultiplication stays a downstream job.
- `Manifest` (read-only string + "Reload" button): shows where the manifest came from (EXR
  header key / sidecar / none) and the object count; expandable list of names for copy-paste.
- `Matte List` doubles as the serialised state; everything else is derived.

**Render:** fully safe frame. Needs the Color plane (when Output passes RGB through) and all
`<prefix>NN` planes of the chosen set; declares the Color plane as output; pass-through of
other planes from Source. Per pixel: loop planes, two (id, cov) pairs per plane, stop at the
first id == 0 with cov == 0 (ranks are sorted by coverage, so trailing pairs are empty);
`cov` summed when `float_bits(id)` is in the selected set (an unordered_set<uint32_t>, copied
from the knob under a mutex at render start). Preview colour: `hash_to_rgb(id_bits)` = three
more MurmurHash rounds, standard gizmo formula. Clamp matte to [0,1].

**Manifest resolution (main thread, cached per source path + layer):**
1. Walk upstream from Source: stop at the first Read or Cycles render / pass node; otherwise
   follow the preferred input, up to 32 hops.
2. Read node: evaluate its `filename` knob at the current frame, open the header with OIIO
   (`ImageInput::open` + `spec().extra_attribs`; Engine already links OIIO under
   `NATRON_HAVE_OPENIMAGEIO`, verify both builds define it, else use OpenEXR directly), find the
   `cryptomatte/*/name` equal to the chosen prefix, take its `manifest`; if `manif_file` is set
   instead, read that JSON next to the EXR. Tiny hand-rolled JSON parser (flat string->string).
3. Cycles node upstream (Part 2): ask it directly for the manifest string of that layer.
4. No manifest: names cannot be resolved, hex ids and the picker (which writes `<hex>` tokens)
   still work.

**Viewer pick:** `onOverlayPenDown` in pick mode -> canonical pos -> pixel at the render scale
-> `getImage(0, time, RenderScale(), view, NULL, &plane00, ...)` (ColorChartMatch pattern) ->
rank-0 id -> name via manifest reverse map (`%08x` -> name) else `<hex>` -> toggle in the list
knob (`setValue` so it undoes). Box: `onOverlayPenMotion/Up` with a rubber band drawn in
`drawOverlay`; collect ids over all planes in the box. Draw the current matte list count in the
overlay corner so the user sees pick mode is on.

**Tests (headless, `tools/cryptomatte-tests/`):** `decode_check.py` builds Read ->
Cryptomatte -> Write on the Blender reference and asserts with oiiotool:
- all manifest names selected => matte == Combined alpha (within 1e-6) where alpha is 1;
- `Cube_Left` matte is zero right of x=120 and non-zero in the cube's region;
- `<2ce615c5>` == `Cube_Left`;
- Material layer: `GreenMat` == `Sphere_Mid` + `Ground` matte;
- Asset layer: `HeroGroup` == `Cube_Left` + `Sphere_Mid`.
GUI pass by the user: pick mode on the Blender EXR, Preview colours, undo of picks.

**Docs:** NODE_REGISTRY.md row + counts, wiki regen, README feature table line, user note about
not shuffling crypto layers, FEATURE_REQUESTS.md entry -> done.

## Part 2 — Cycles encoder

- **Names.** In `CyclesRenderer::syncSceneWithCamera`, set `Object::name` to the Natron node's
  script name (plus `/<mesh name>` for multi-mesh ReadGeo archives), `Object::asset_name` to
  the top-most Group3D / Scene3D ancestor's name (Blender uses the top parent), `Shader::name`
  to the Material3D node name (default shaders get `default_surface` etc. like Blender).
- **Knobs.** CyclesRender AOV page: `Cryptomatte Object` / `Material` / `Asset` bools + `Levels`
  (int, default 6; depth = ceil(min(16,levels)/2)). Pass node: AOV tokens `CryptoObject`,
  `CryptoMaterial`, `CryptoAsset` in `kAovs[]` (PassTableWidget) and `@cryptomatte` token
  expansion (the comment at CyclesRenderPassManager.cpp:932 invites exactly this).
- **Passes.** `film->set_cryptomatte_passes(flags)`, `set_cryptomatte_depth(depth)`; create
  `depth` passes per enabled type named `CryptoObject00..`, type `PASS_CRYPTOMATTE`, in the
  order object, material, asset (the kernel writes them in that order). Output driver already
  reads 4 channels; `passNameToPlane` maps `Crypto*NN` to an RGBA4 plane with the same name
  (not RGB3). `isDataAov_` already treats "crypto" as data, so no colour conversion.
- **Manifest.** After sync: `scene->object_manager->get_cryptomatte_objects(scene)` etc. (guard
  empty scene). Keep the three strings on the render node (`getCryptomatteManifest(prefix)`)
  for the in-graph decoder, and write them in `saveMultiLayerEXR` as
  `cryptomatte/<7hex>/{name,hash,conversion,manifest}` with lowercase `rgba` channel names (so
  DWA stays lossless, as Blender does).
- **Tests:** render the Scatter/Card demo scene with crypto on, decode with Part 1 by node name,
  compare against the per-object holdout render (matte == alpha of the lone object, 1e-3 at
  16 spp); round-trip the saved EXR through the decoder and through Blender's compositor
  Cryptomatte node as the independent check.

## Progress

- 2026-11-03: steps 1-3 and 5 done. `Cryptomatte` node built and registered, headless check
  `tools/cryptomatte-tests/run_check.sh` DONE_OK (9/9 against the Blender reference, which is
  now rendered film-transparent so its alpha is the object coverage). Picker written, GUI pass
  pending (project: tools/cryptomatte-tests/out/cryptomatte_gui.ntp). First GUI pass by the user OK ("works great").
- 2026-11-04: panel reworked to Nuke's Cryptomatte layout at the user's request: Layer Selection,
  Manifest Source (Metadata Auto / Sidecar File / None + file), Preview Mattes, Picker Add /
  Picker Remove modes, Matte List + Clear, Matte Output + rgba channel, Unpremultiply, Remove
  Channels (dynamic pass-through block). Check still 9/9. Part 2 idea from the Blender
  discussion: a per-node "Crypto Asset" string knob (default: top-level group name) gives a
  user-defined layer through Cycles' asset hash without kernel changes.

- 2026-11-04: Part 2 done. Cycles fork 689977518 (feature/cycles-deep): CRYPT_CUSTOM layer type,
  Object::cryptomatte_custom_name, manifest getter, post-sort of every layer (upstream sorted only
  the first), empty-scene manifest guard. Natron: CyclesRender AOV toggles + Levels, pass node
  tokens, per-node Crypto Asset / Crypto Custom knobs (Scene3D/CryptomatteKnobs, 13 node classes,
  inherited down Group3D), object/shader naming in CyclesRenderer, manifests in-graph + in the
  EXR header, decoder reads them from an upstream CyclesRender. Checks:
  tools/cryptomatte-tests/run_encode_check.sh (object sums == beauty alpha, custom hero ==
  sphere + cube, default == card, asset inheritance, material, file == live bit-exact) and
  run_blender_crosscheck.sh (Blender's compositor decodes our EXR, CryptoObject and the custom
  layer, identically). Note: the render node upsamples its native size to the project format,
  the saved EXR is native.

## Order of work

1. MurmurHash + hash_to_float + hash_to_rgb with unit checks against the Blender manifest.
2. Node skeleton: plane discovery, Layer knob, Matte List with names + hex, render path,
   headless decode_check green on the Blender file.           (~2 days)
3. Manifest from the EXR header through the upstream Read; Preview colours; info strings.
4. Viewer picker (click, box, undo), GUI pass.                  (~2 days)
5. Docs + registry + wiki, commit.
6. Cycles encoder (names, knobs, passes, manifest, EXR keys), tests, docs.   (~2-3 days)
