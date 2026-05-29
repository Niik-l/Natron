# Dev Nodes — Known Issues & TODO

Tracking known bugs, incomplete features, and planned improvements.

---

## My TODO (user priorities)

- [ ] Test all deep nodes
- [ ] Create deep sample picker
- [ ] Think about material assignment — particles / instances and attributes
- [ ] Point clouds
- [ ] Scatter node
- [ ] ParticleInstance works on scatter / point clouds
- [ ] Material ramps / expressions / variations
- [ ] Shader updates for volumes (WIP — PrincipledVolume path works, remap curves in, needs more iteration)
- [x] Fully test VDBs (basic fire/smoke rendering verified with EmberGen + Houdini VDBs via Cycles)
- [ ] Use backplate in Cycles renders
- [ ] Blender deep?
- [ ] Denoise

---

## DevShuffle (Channel Routing)

### Known Issues

- **Output layer routing may not work correctly for custom layers.** When a user creates a custom output layer (e.g. "test") and routes channels into it, the data may still end up in Color.RGBA instead of the selected layer. Root cause analysis done — two fixes applied (removed duplicate layer declaration in getComponentsNeededAndProduced, added plane matching in render) but may need further testing with complex multi-layer EXR workflows.

- **Row 2 output layer is ignored.** `getComponentsNeededAndProduced()` only reads Row 1's outputLayer. If Row 2 targets a different output layer, it's never declared as a produced output. The render function also doesn't handle routing Row 1 and Row 2 to different output layers simultaneously.

- **Needs testing with:** multi-layer EXR files, multiple DevShuffle nodes in series, AOV passthrough.

### Files
- `Engine/Dev/Channel/DevShuffle.h/cpp`
- `Engine/Dev/Channel/KnobShuffle.h/cpp`
- `Gui/ShuffleWidget.h/cpp`
- `Gui/KnobGuiShuffle.h/cpp`

---

## Deep Compositing

### Known Issues

- None currently — basic deep pipeline (DeepRead, DeepFlatten, DeepMerge, DeepToPoints, etc.) tested and working.

### Tier 1 Nodes — Needs Testing

- **DeepSplit** — test with various threshold values, soft edge blending
- **DeepPositionMatte** — works in pixel+depth space (not true world 3D). Default sizeZ increased to 50 for typical deep scenes. Test all 3 shapes + falloff.
- **DeepFog** — tested and working. Try different density/depth range combos.
- **DeepGodRays** — tested and working. Flat RGBA output — add/screen over flattened result.
- **DeepAO** — outputs AO as grey mask (R=G=B=A=ao) for grading over plate. May need intensity tuning.

### Tier 2 Nodes — Needs Testing

- **DeepRelight** — Estimates normals from depth gradients. Test: connect DeepRead → DeepRelight, adjust light position/direction, try all 3 mix modes (Add/Multiply/Replace). Check if normals look correct on different geometry. The normalScale knob controls how aggressively depth differences translate to surface angle — try values 0.1 to 10.

- **DeepContactShadow** — Darkens where samples are close in Z. **TODO: output shadow in alpha channel (like DeepAO) so it can be graded over plate.** Currently multiplies RGB only. Test: connect to a DeepMerge of two objects — should see darkening where they overlap in depth. Adjust proximity (Z distance threshold) and kernelRadius.

- **DeepConsolidate** — Test: connect after any deep pipeline, check that zero-alpha samples are removed, overlapping samples merged, output is sorted. Compare sample counts before/after in the info display.

- **DeepDepthWarp** — Test all 4 modes:
  - Remap Range: set inputNear/Far to actual Z range, outputNear/Far to desired range
  - Offset: add/subtract from all Z values
  - Scale: multiply all Z values
  - Power: non-linear depth remapping (power < 1 compresses near, power > 1 compresses far)
  - Connect DeepDepthWarp → DeepFlatten and compare with original to verify Z changes

### TODO

- DeepWrite needs testing with actual deep EXR output
- DeepDefocus bokeh quality could be improved
- DeepExpression needs documentation for available variables

---

## CameraTracker (3D Camera Tracking)

**Status: MVP pipeline complete.** Detect → Track → Solve → Camera3D output works end-to-end.

### Known Issues

- **~~Viewer overlay disabled~~** — FIXED. Root cause was two null pointer bugs in `Gui/ViewerTabPrivate.cpp` (Natron core bug). Overlay now works — green crosses at track positions, green trails showing recent motion.
- **Forward-motion shots produce bad solves** — Camera positions in the millions. The two-view essential matrix decomposition is degenerate for forward/dolly motion. Rotation-first fallback is implemented but not fully effective.
- **Tracking blocks GUI thread** — Natron freezes during tracking. Needs background thread (like TrackerNode's TrackScheduler).
- **"Export" tab** — Originally "Output" but renamed due to script name conflict with internal Output node.

### TODO

1. ~~Fix viewer overlay crash~~ DONE
2. ~~Mask region for feature detection~~ DONE — ROI knobs + yellow dashed overlay
3. ~~Manual tracker points~~ DONE — "Add Track Mode" checkbox + click to place
4. **Camera path smoothing** — post-solve filter to remove jumps on early frames
5. Improve solve quality further (homography decomposition fallback)
6. Move tracking to background thread with progress dialog
7. Track management (select/delete individual tracks, bidirectional tracking)
8. Test with lateral-motion footage (should produce much better solves)

### Files
- `Engine/CameraTrackerNode.h/cpp`
- See also: CameraTracker STATUS.md for detailed status

---

## 3D System / Scene3D

### Known Issues

- None currently — Camera3D, Card3D, Sphere3D, Scene, ScanlineRender all working in 3D viewport.

### Completed (2026-05-29) — CyclesRenderPassManager MVP 1-5C (new sink node)

- **`Engine/Dev/Cycles/CyclesRenderPassManager.{h,cpp}`** — new sink node
  registered as `fr.inria.built-in.CyclesRenderPassManager` in the 3D
  category. Mirrors CyclesRender's input layout (bg / obj / cam) so it
  drops into existing scene wiring.
- **JSON-backed pass list.** Single `passesJson` `KnobString`
  (multi-line) seeded with a 2-pass default (beauty Combined + data
  Depth/Normal/UV). Persists in project files via normal knob
  serialization — verified round-trip on save/load.
- **Reset to Default button** — restores the seed JSON without
  re-creating the node.
- **Render to Disk button (5C scope)** — parses the JSON via Qt's
  `QJsonDocument`, filters by the data.js active rule
  (`enabled && output && !mute && (!soloActive || solo)`), resolves
  dollar-token paths (`$PASS` from the spec, `$SHOT` / `$RENDER` from
  env vars with `"shot"` / `"/tmp"` fallbacks), then frame-pattern
  resolves (`####` / `%04d` / trailing digit group, same logic as
  `CyclesRenderer::resolveTextureFrame`), and writes synthetic 64×64
  multi-layer EXRs at the resolved paths. Layer/channel names match
  the existing `CyclesRenderer::saveMultiLayerEXR` convention so files
  open in Read with the expected layer dropdown.
- **Architecture + open questions** documented in
  `RENDER_PASS_MANAGER_DESIGN.md` (local-only). Tracks the batching
  rule (group passes by shared scene state — camera+vis+light+samples+
  overrides — and run one Cycles session per batch with the union of
  AOVs), open questions (preview output, custom widget priority,
  token vocabulary, material override scope), and the MVP slice that
  validates each.
- **Audits + catalog** in `CYCLES_PASS_AUDIT.md` and `CYCLES_PASS_CATALOG.md`
  (local-only). The audit documents the current pass plumbing in
  `CyclesRender.cpp` (12 enabled passes via 12 knobs, single-Cycles-call
  multi-pass via `NatronMultiPassOutputDriver`, what controls already
  exist via the upstream `RenderPass` node, what's missing for the
  spec). The catalog enumerates all 49 `PassType` enums from
  `D:\_vfx_claude_2025\cycles\src\kernel\types.h:497-581` bucketed
  across the 6 categories the UI uses (Beauty / Shadows / Additive /
  Data / Reflection-Refraction / Matte-ID), plus the non-enum mechanisms
  (cryptomatte bitmask, light groups, AOVs, shadow catcher, holdout,
  denoising auto-allocation).

### Pending — CyclesRenderPassManager MVP 5A + 5B + 6

- **5A** — refactor `CyclesRender::render()` (`Engine/Dev/Cycles/CyclesRender.cpp:670-1080`)
  so its scene-graph build + Material3D bake + hash + multi-pass
  renderer call can be invoked from the Manager. Replaces the synthetic
  stub EXR with real Cycles output.
- **5B** (implicit) — extend the existing `saveMultiLayerEXR` to take
  per-pass format / bitdepth / compression from the JSON spec.
- **6** — batching engine: group active passes by hash of
  (camera + vis config + light config + shader override + samples) and
  run one Cycles session per batch, emitting the union of all batch
  members' AOV lists. Per-pass file output demuxes the rendered buffers
  to each pass's output path.

### Completed (2026-05-29) — CyclesRender Mist AOV + single-value display fix

- **`Engine/Dev/Cycles/CyclesRender.cpp` + `CyclesRenderer.cpp`** — added
  PASS_MIST end-to-end via the now-canonical 5-step add-pass recipe: knob
  → `getEnabledPasses` → `passNameToPlane` → `standardPasses[]` table →
  per-pixel broadcast block. Proves the pattern is reusable for the ~33
  remaining unwired Cycles passes (cryptomatte, motion vector, position,
  denoising aux, volume direct/indirect, etc.).
- **Display fix for single-value AOVs.** Depth / AO / Mist were defined as
  1-channel "A" planes, which Natron's viewer in default RGB mode renders
  as black (R/G/B undefined). Switched all three to 3-channel R/G/B
  planes with the scalar broadcast in the per-pixel blit. Bonus: AO now
  displays correctly too.
- **Recipe + trap saved as memory entries** — `reference_cycles_add_pass_recipe.md`
  (5-step pattern) and `feedback_natron_single_channel_plane_display.md`
  (the 1-ch "A" display trap).

### Completed (2026-05-29) — SphericalTransform Faces format

- **`Engine/Dev/Transform/SphericalTransform.{cpp,h}`** — input count 1 → 7;
  slot 0 is the legacy single-image source (`img`), slots 1-6 are per-face
  cubemap inputs labeled `-Z, +Z, -X, +X, -Y, +Y` (canonical / Nuke order).
  When `Input Projection = Cubemap` AND `Input Format = Faces`, render()
  pre-fetches each connected face into a `FaceSource` array and per output
  pixel computes its 3D direction → `cubeFaceFromDirection()` → looks up
  the internal-face → slot mapping via `kInternalFaceToFaceSlot[]` →
  reseats sampling state at that face. Missing faces output black per spec.
- **`getRegionOfDefinition` + `getPreferredMetadata` fallback** — slot 0
  may legitimately be empty in Faces mode; both fall through to the first
  connected face input as the size reference instead of returning failure
  (an early failure poisons the engine cache for subsequent knob changes).
- **`isInputOptional` returns `true` for all slots.** The root-cause bug
  fix — when slot 0 is mandatory and disconnected, the viewer's
  `checkTreeCanRender_internal` (`Engine/ViewerInstance.cpp:702-730`)
  silently bails out before any RoD/render call. Matches the pattern in
  Scene3D / DevShuffle / ScanlineRender / ParticleInstance / ParticleMerge
  / Group3D / CyclesRender.
- **Trademark scrub** — replaced 9 references to a competing compositing
  host with neutral phrasing (`"canonical order"`, `"compositing DCCs"`,
  `"standard optical models"`, etc.) across `SphericalTransform.{cpp,h}`
  and `SphericalProjections.h`.

### Completed (2026-05-28) — Scene-wide motion blur + GLSL closeout + 3D viewport `F` in look-through

- **Scene-wide multi-sample motion blur** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. New "Motion Blur" group on the Output tab: `Samples`, `Shutter`, `Shutter Offset` (Centered / Start / End / Custom — mirrors Nuke's ScanlineRender), `Custom Offset`, `Temporal Jitter` (deterministic per-(sample, frame) hash). Per-sample loop re-evaluates the camera (view + proj + projView), re-extracts every animated geo at sub-frame time, and re-queries instances with manual `pos + vel * sampleDt` extrapolation (the particle sim only runs once per frame, so getInstances/getParticles at sampleTime return frame-time data — extrapolation is required). Particle velocity-stretch cheat mode preserved for `motionSamples == 1`.
- **Animated Alembic sub-frame interpolation** — `Engine/Dev/Scene3D/ReadAlembicArchive.cpp` + `Engine/Dev/Scene3D/ReadGeo.cpp`. `getMeshDataAt` / `getEntryWorldMatrix` / `updateTransformAtTime` / `updateVerticesAtTime` all lerp between bracketing samples instead of snapping `floor(time + 0.5)`. Both timeMode 0 (per-frame) and timeMode 1 (source-time) paths use continuous sample indices. `ReadAlembicCamera` already correct (samples stored as Natron knob keyframes which interpolate natively).
- **Cycles vertex motion blur for Alembic** — `Engine/Dev/Cycles/CyclesRenderer.cpp`. Added `int archiveEntryIdx = -1` to `SceneNode` (`Engine/Dev/Scene3D/SceneGraph.{h,cpp}`) so the renderer can re-query the upstream archive at sub-times. Hoisted shutter open/close outside the per-node loop. Mesh path now sets `mesh->set_use_motion_blur(true)` + `set_motion_steps(3)` and populates `ATTR_STD_MOTION_VERTEX_POSITION` with sub-time vertex positions for both ReadAlembicArchive and ReadGeo sources. Vertex motion blur only — xform motion deferred (requires SceneGraph parent-chain re-eval at sub-time).
- **GLSL volume rendering migration** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. ReadVDB + procedural Volume3D ray-march migrated from legacy fixed-function (`gl_ModelViewMatrix` / `gl_Vertex` / `gl_FragColor`) to GLSL 3.30 core with explicit `u_modelView` / `u_projView` uniforms. New `drawVolumeProxyCube` helper builds a transient 36-vertex VBO + glDrawArrays, replacing the `glBegin(GL_QUADS)` immediate-mode draw. `GL_LUMINANCE` → `GL_R32F` / `GL_RED` for core-profile compatibility (shader still reads `.r`, no behavior change).
- **GLSL ParticleInstance migration** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. New `kInstanceVert` / `kInstanceFrag` shader pair (modeled on `kBeautyVert/Frag` but trimmed: per-instance color uniform, no texture/STW, stretch-fade uniforms for cheat-mode MB). One VBO per geo type built once per render with interleaved pos3 + normal3 + uv2 (stride 8); Cube3D forwards `CubeVertex.u/v`, Sphere3D gets synthesized spherical UVs. Per-vertex flat normals derived from triangle cross-product. Per-instance loop builds `localMatrix` (T·R·S) + `normalMatrix` (transpose(inverse(3x3))), uploads uniforms, `glDrawArrays`. Instances now participate in MRT — Normal / UV / Pref / Velocity AOVs all written (legacy immediate-mode path skipped MRT entirely). Multi-sample MB uses per-instance velocity extrapolation as above.
- **Normal AOV fix for file-loaded meshes** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. ReadGeo and ReadAlembicArchive paths weren't populating `geo.normals` → GLSL `normalize(vec3(0))` returns NaN → renders white in float AOV. Added `computeVertexNormalsFromTris` helper (area-weighted face-normal averaging per vertex), called from both mesh paths. Procedural shapes (Sphere3D etc.) already had explicit normals — unchanged.
- **Non-cubic VDB 3D texture** — `Engine/Dev/Scene3D/ReadVDB.{h,cpp}` + `Engine/Dev/Scene3D/ScanlineRender.cpp`. `VDBVolumeData::resolution` → `resX/resY/resZ`. Sampling loop maps the largest VDB axis to `maxResolution`, scales other axes proportionally (min 8 per axis), preserving the VDB's voxel aspect. Non-cubic smoke / fire no longer renders as a soft blob filling the bounding box.
- **Legacy fixed-function stack removed** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. With volume + instance now GLSL, the per-sample `glMatrixMode` / `glLoadMatrixf` calls inside the multi-sample loop have no remaining consumers and are gone. Every draw path in ScanlineRender is now GLSL 3.30 core, uniform-driven.
- **3D viewport `F` works in look-through** — `Gui/DevViewport3D.cpp`. F (Frame Selected / point cloud / reset) now also handles look-through-camera mode: if the look-through source is an editable Camera3D, the camera node's translate knobs get rewritten so the framed target sits at framing distance along the camera's view direction, preserving its rotation. ReadAlembicCamera (read-only) falls back to perspective-mode framing so the user still gets a useful view.

### Completed (2026-05-27) — ParticleAttribute (V2 three-section design) + KnobGradient + ScanlineRender Solid

- **ParticleAttribute** — `Engine/Dev/Particles/ParticleAttribute.{h,cpp}`. ParticleModifier passthrough with **three always-visible sections** (Color / Pscale / Alpha). Each section: Enable, Isolate (radio across sections), Source (Age/Lifetime / Speed / Velocity / Position / BounceCount / SpawnIndex), Source Min/Max + Fit button, editor (Gradient for Color, KnobParametric Curve for Pscale + Alpha), Mix, Reset, and a live read-only Summary label. Top-level **Reset All** button restores every section. `applyForce` iterates particles once, applying all enabled sections (Color → RGBA via gradient sample; Pscale → size via curve; Alpha → a via curve). Defaults: warm fire gradient for Color, ease-out (ending at 0.1 so dying particles stay visible) for Pscale, bell for Alpha. Placement: downstream of `ParticleSolver` (the solver's step-6 appearance-resync overwrites the modifications otherwise).
- **KnobGradient + KnobGuiGradient + GradientWidget** — new custom knob infrastructure backing ParticleAttribute's Color section. Engine: `Engine/Dev/Particles/KnobGradient.{h,cpp}` stores stops as a comma-separated YAML-safe string (`"0,ffd14aff,0.45,ff5a00ff,1,1a0010ff"` — `:` and `|` separators both broke project load, comma matched the KnobShuffle precedent). Gui: `Gui/KnobGuiGradient.{h,cpp}` + `Gui/GradientWidget.{h,cpp}` is a draggable gradient bar with click-to-add stops, drag-to-move, double-click-to-edit (QColorDialog), right-click delete. Registered in both `KnobFactory.cpp` (Engine) and `KnobGuiFactory.cpp` (Gui) mirroring how KnobShuffle is wired.
- **ScanlineRender Solid mode** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. New `Solid` knob on the Particles page. When on: blend func switches to `GL_ONE / GL_ZERO` (replace), depth-write enabled so particles occlude each other and the scene, and disc/sphere/sprite fan rings use full alpha at edges instead of fading to 0. Closes the "I want opaque hard-edged particles" gap (default behavior was soft fade-to-transparent for compositor-style blending).
- **Phase 7 polish** — Reset All button at top of ParticleAttribute, live Summary labels per section, refined tooltips on Source/Source Min/Source Max/Mix with concrete examples and unit hints.

### Completed (2026-05-27) — Alembic particle round-trip + ParticleSolver multi-frame cache

- **WriteAlembicParticles** — new node, `Engine/Dev/Particles/WriteAlembicParticles.{h,cpp}`. Passthrough particle node (extends `ParticleModifier` with a no-op `applyForce`, particles flow through unchanged). Knobs: `File` (KnobOutputFile, .abc path) and `Bake` button. Clicking Bake iterates the project's `getFrameRange()` at `getProjectFrameRate()` FPS, pulls particles from input 0 each frame via `ParticleProvider::getParticleData()`, and writes an OPoints sample per frame containing positions, IDs, velocities (as the standard velocities slot), plus RGBA color (`Cd` C4f) and per-particle size (float) as arbGeomParams. Uses `Alembic::AbcCoreOgawa::WriteArchive` + `Alembic::AbcGeom::OPoints`. Synchronous on the UI thread (no progress dialog yet — per-frame status logged to stdout). Registered as `fr.inria.built-in.WriteAlembicParticles`. **First disk-writing node in the 3D system.** Verified end-to-end: baked file loads cleanly in Houdini's particle view.
- **ReadAlembicParticles** — companion read node, `Engine/Dev/Particles/ReadAlembicParticles.{h,cpp}`. Inherits `EffectInstance + ParticleProvider` (a source, no inputs). Knobs: `File` (KnobFile), `Reload`, `Info` (read-only label showing "N samples, M-K particles"). On file change, opens the archive via `Alembic::AbcCoreFactory::IFactory`, walks to find the first `IPoints` (depth-first), and pre-loads every sample into RAM (positions, IDs, velocities, optional `Cd` color + `size` from arbGeomParams). `getParticleData(time)` maps Natron-time → Alembic seconds via project FPS, snaps to nearest sample via `TimeSampling::getNearIndex`, builds a `ParticleData` from the pre-loaded arrays. Marks itself frame-varying via `getPreferredMetadata` when sample count > 1 (mirrors the `ReadAlembicArchive` precedent — without it Scene3D/ScanlineRender would freeze on a single frame). V1 picks the first IPoints; multi-points dropdown is future work. Closes the round-trip: Solver → WriteAlembicParticles → (edit in Houdini) → ReadAlembicParticles → downstream.
- **ParticleSolver multi-frame RAM cache** (Phase A skeleton + Phase B wiring landed together):

- **Phase A — skeleton** (landed first): added "Cache" page on ParticleSolver with knobs `Cache Simulation` (bool, default on), `Max Cache (MB)` (default 1024, range 64-16384), read-only labels `Cached Frames` and `Cache RAM`, and a `Clear Cache` button. Backing data in `ParticleSolverPrivate`: `std::map<int, CachedFrame>` keyed by frame, `std::mutex` for thread safety, `frameCacheHash` U64 sentinel for upstream-input invalidation, `frameCacheBytes` for memory accounting. Render path unchanged in A.
- **Phase B — wired into the render path**: `getParticleData(time)` now consults the multi-frame cache. (1) Hash invalidation — XOR of input0/input1 `getHash()` + own knob values (elasticity, friction, maxBounces, substeps); mismatch with stored `frameCacheHash` wipes the cache before simulating. (2) Exact-frame hit returns immediately (deep-copies state + `knownIDs` out, skips the integration loop entirely). (3) Cache miss finds the nearest cached frame F ≤ endFrame via `std::map::upper_bound`, restores from F, integrates from F+1 to endFrame, caches each integrated frame as it goes. Live status labels update on every miss. **No LRU eviction yet** — cache grows unbounded (capped manually via `Clear Cache` or natural `Max Cache (MB)` ignored). Phase C adds eviction.
- **Debug coloring (showCollisions tint) factored out** so the cache stores uncolored state. Toggling the knob no longer requires a cache flush.
- **Existing single-slot cache (`cachedData`/`cachedFrame`/`knownIDs`) preserved** — acts as the fast path when the previous call's time matches exactly.

### Completed (2026-05-24) — Animated Alembic + viewport lighting + camera default

- **ReadGeo + ReadAlembicArchive: animated vertex (deforming) meshes** — was loading sample 0 only, freezing on the rest pose. Both nodes now pre-load all vertex samples at file-open into per-entry caches; `getMeshData(time)` / `getMeshDataAt(idx, time)` memcpy the matching sample into `mesh->vertices` before returning. Topology stays constant across samples (the standard Maya/Blender/Houdini export). Archive time-to-sample mapping mirrors `getEntryWorldMatrix` (timeMode 0 per-frame + Frame Offset, timeMode 1 nearest source-time).
- **ReadGeo + ReadAlembicArchive: `setIsFrameVarying(true)`** — declared via new `getPreferredMetadata` override when the loaded file carries animated content (xform OR vertex samples). Without this, Scene3D never saw a time-varying upstream — it only checks knob animation via `inp->getHasAnimation()`, not embedded `.abc` data — so ScanlineRender's image cache froze on a single frame even with animated geo. CyclesRender always worked because it sets frame-varying on its own node; the 3D viewport bypassed the cache via direct `getMeshData(time)` calls. Both nodes also call `refreshMetadata_public(true)` after a load.
- **ReadGeo: user TRS knobs honored in ScanlineRender + Cycles + viewport** — was silently dropped (only `mesh->transform` was applied). Both `ScanlineRender::extractGeometry` and `SceneGraph` now compose `userTRS × embedded`, so animated `.abc` xforms still play through alongside user offsets.
- **3D viewport shaded mode on imported meshes** — was uniformly dark because `ViewportFaceLitFactor` assumed CCW-from-outside winding. Imported meshes have unpredictable winding (CW, CCW, mixed triangulated soups), so the cross-product face normal could point inward → N.L clamped to 0. Switched to two-sided `abs(N.L)` on the cross-product path; procedural primitives (Sphere/Card/Cube/Cyl) keep `ViewportLitFromVertexNormals` untouched.
- **Camera3DNode: V Aperture default matches project aspect** — was hardcoded 18.672mm (Academy 35mm), forcing users to click "Match Project Aspect" on every new camera. Default is now computed in `initializeKnobs` as `24.576 * (projectH / projectW)`, so a fresh Camera3D on a 1920×1080 project drops in at ~13.824mm and reports "Sensor aspect matches project" immediately. Fallback to 18.672mm when no project is available.

### Completed (2026-05-23) — Phase 3 GLSL/MRT migration + Shading Modes

- **ScanlineRender GLSL 3.3 + MRT pipeline** — fixed-function `glBegin`/`glEnd` mesh + particle paths retired. Single VAO/VBO/IBO + `kBeautyVert`/`kBeautyFrag` shader pair drives every geo draw; particle draws use a parallel `kParticleVert`/`kParticleFrag` pair. UVProject's STW projective texturing handled in-shader via `u_hasTexture == 2` branch.
- **6 per-pixel AOVs** — Depth (linear camera-space), World Position (reconstructed via inverse(MVP)), Normal (world-space), UV, Pref (object-space ref position), Velocity (screen-pixels-per-frame). Declared on plane -1 via `isMultiPlanar()` + `getComponentsNeededAndProduced`. MRT attachments allocated lazily per-AOV. AOV blend overridden to `GL_ONE / GL_ZERO` (replace) via `glBlendFunci` so values don't accumulate across overlapping fragments.
- **Particle AOVs** — Normal/UV/Pref/Velocity work for all four particle modes (Point/Disc/Sphere/Sprite + motion-blur stretch variants). Per-vertex AOV defaults set via a `fillAovs` lambda (normal = +fwd camera-facing, pref = particle world pos, velocity = per-frame displacement). Sphere static overrides normal/pref with real per-vertex sphere values; Sprite static overrides UVs with quad-corner layout. Depth/WorldPos for particles is by-design only contributed by static Sphere (other modes disable depth writes for translucency). Motion-blur stretch is documented as beauty-only — for AOV-correct motion blur use Motion Samples > 1.
- **Shading modes** — new `Shading Mode` knob on ScanlineRender (default Shaded). Shaded does per-pixel N.L diffuse + 0.15 ambient against a `Light3D` if connected, otherwise a camera-relative headlight (Maya default convention). Flat preserves the legacy unlit behavior. Wireframe renders solid white GL_LINES derived from triangle indices.
- **3D viewport shading parity** — `DevViewport3D` adds `eFlat` to `ShadingMode` enum (legacy unlit). `eShaded` + `eShadedWire` now compute per-face flat N.L using averaged per-vertex normals on primitives (Sphere/Card/Cube/Cylinder — winding-agnostic) and cross-product face normals on ReadGeo/Alembic (CCW-from-outside assumption). Light is camera-locked top-right-eye, orbits with viewer.

### Completed (2026-04-08)

- **ReadVDB OpenVDB support enabled** — `NATRON_HAVE_OPENVDB` defined under `NATRON_CYCLES` build. VDB grids load directly via `VDBImageLoader` (no dense conversion).
- **ReadVDB PrincipledVolume fire rendering** — density, temperature, flame, color grids bound to Cycles PrincipledVolumeNode. Absorption color, density/temperature remap curves (FloatCurveNode). Defaults dialed for EmberGen/Houdini fire VDBs.
- **ReadVDB viewport wireframe bbox** — dynamic bounds from `getVDBBounds()` instead of hardcoded cube. Cached per resolved frame path.
- **Volume3D rotation** — Rotate X/Y/Z knobs added, SceneGraph reads them into `buildTRS()`. Also added stepSize and volumeBounces knobs.
- **Cycles CPU device for NanoVDB** — explicit CPU device selection required for volume rendering. Without it, volumes render empty.

### TODO

- ReadGeo/ReadAlembicCamera require Alembic library (optional, not tested)
- ScanlineRender shadows not implemented
- Light3D shadow ray marching is functional but slow for dense volumes
- Volume shader workflow still WIP — needs more iteration on user-facing controls

---

## Particles

**Status: Phase 2 complete (13 nodes). Solver architecture refactored. Collision with geometry working.**

### Architecture (2026-04-06 refactor)

- **Force nodes are stateless** — `applyForce()` modifies velocities in-place, no cached positions. Viewing from a force node shows emitter positions with velocity tweaks (preview only, no accumulation).
- **ParticleSolver is the solver** — walks upstream with `collectUpstreamForces()` to find the emitter and all force nodes. Runs the single simulation loop each frame: spawn → forces → integrate → collide → expire. Only node that owns position/velocity state.
- **Works without geo** — if no geometry is connected to the `geo` input, ParticleSolver acts as a pure solver (forces + integration, no collision). Always put ParticleSolver at the end of the chain.
- **OBB collision** — reads rotateX/Y/Z from connected Cube3D, transforms particles into local space for AABB test, transforms back. Standard technique (Unity/Unreal/Blender).
- This matches the standard pattern used by Blender, Unity, Unreal, and Houdini. See `Research_Particle_Sim_Loop.md`.

### Known Issues

- **Force preview is velocity-only** — viewing from a force node (e.g. Gravity) shows emitter positions, not accumulated force positions. Must view from ParticleSolver for proper simulation.
- **Node renamed** — ParticleCollide → ParticleSolver (2026-04-06). Plugin ID is now `ParticleSolver`.
- **Animated colliders snap per-frame, not per-substep** — `applyCollision` is called per substep but with frame-level time. A keyframed-rotating Cube3D collider effectively uses the frame-N pose for all substeps. Visible as "popping" only on fast-rotating colliders. Fix would be `applyCollision(p, frame + sub/numSubsteps, dt)` plus per-substep knob caching. Low priority.
- **Per-particle knob reads in `applyCollision`** — 10 knob lookups per particle per substep per frame on the collision geo. At 10k particles × 4 substeps = 400k knob reads/frame. Should hoist the reads outside the inner loop. Perf, not correctness.

### Completed (2026-04-06)

- **Solver architecture refactor** — force nodes stateless, ParticleSolver owns simulation loop. Bounced velocity persists correctly (particles roll off surfaces during playback).
- **OBB collision** — rotation support for Cube3D colliders (inverse-transform to local space)
- **Show Collisions debug** — checkbox tints collided particles red
- **Removed built-in shape knobs** — no more Plane/Box/Sphere dropdown. Collision shape comes from connected Cube3D/Sphere3D geo.
- **Collision functions** — `bounceParticle`, `rayAABB`, `collideGeoBox`, `collideGeoSphere`, `pushOutOfBox`, `pushOutOfSphere`
- **`prevPx/prevPy/prevPz`** on Particle struct, `size` knob fix
- **Shiboken crash recovery**
- **ParticleCollide → ParticleSolver rename** — node, class, plugin ID, all references
- **ParticleSpawn "On Collision" working** — Emitter → Gravity → Solver (Cube3D geo) → Spawn (On Collision) → Merge pipeline tested and working
- **ReadAlembicTransform** — reads IXform (null/locator) from Alembic files. Dropdown to select transform, FPS knob, frame offset. Outputs animated translateX/Y/Z, rotateX/Y/Z, scaleX/Y/Z.
- **ParticleEmitter transform input** — new "transform" input (input 1). Connect ReadAlembicTransform directly — emitter position and emit direction automatically follow the null. No expression linking needed.
- **Substeps knob** on ParticleSolver (default 4, range 1-32)
- **Full particle node audit** — all 12 nodes clean, no issues found
- **3D viewport axis handle** for ReadAlembicTransform — RGB axis cross + yellow diamond at null position, rotates with animation, gizmo suppressed (read-only)

### Completed (2026-04-07)

- **ScanlineRender particle modes** — Point, Disc, Sphere, Sprite + Additive/Over blend + global scale knob
- **ScanlineRender 4x MSAA** — multisampled FBO + blit-to-resolve pattern for proper anti-aliasing
- **Multi-sample motion blur (ScanlineRender)** — physically-accurate via sub-frame accumulation. Works on all particle modes and instances. Samples + Shutter knobs. Stretch cheat mode is mutually exclusive.
- **ParticleInstance** — instances Cube3D/Sphere3D geo at particle positions. Working in both ScanlineRender and CyclesRender.
- **Cycles particle instancing** — native Cycles instancing (shared ccl::Mesh, one ccl::Object per instance). Motion blur via set_motion() with 3 transform steps extrapolated from velocity.
- **maxBounces** — bounceCount on Particle struct, kills particle when exceeded
- **Color variance** — per-particle random color variation on emitter (deterministic by particle ID)
- **Sphere/instance smoothing** — bumped sphere tessellation (16×24 instance, 10×14 particle mode), disc segments (32), added back-face culling for 3D geo

### Completed (2026-05-22) — Audit pass: A1 / A2 / A3 fixes

- **A1: Guarded `dynamic_cast` on collision-geo knob lookups** — `ParticleSolver::applyCollision` and `ParticleEmitter::getParticleData` (transform input path) previously checked the `KnobIPtr` was non-null but blindly dereferenced the `dynamic_cast<KnobDouble*>` result. Future node types exposing `translateX` / `rotateX` etc. as non-`KnobDouble` would null-deref. Replaced inline casts with a `readDouble()` local helper that guards both checks.
- **A2: Threaded `dt` through `bounceParticle` / collide helpers** — at `Substeps > 1` (default 4), the post-bounce continuation displacement (`p.vx * remaining`) was using the full-frame velocity instead of scaling by the substep size. Particles overshot the bounce point by ×4 at default substeps — visible as a "spring" effect on fresh bounces. `bounceParticle`, `collidePlane`, `collideGeoBox`, `collideGeoSphere`, and `ParticleSolver::applyCollision` now all take `dt` and apply `p.vx * remaining * dt` for the continuation.
- **A3: Stopped resyncing `p.life` from the emitter in solver step 6** — the per-frame appearance-sync loop overwrote `p.life` with the emitter's value, undoing the kill that `bounceParticle` set when a particle settled (`speed < 0.0001f` → `p.life = p.age`). Settled particles accumulated indefinitely on collider surfaces. Now sync only `r/g/b/a/size`; `life` is assigned at emission and stays put.

### Still TODO

- **Material system redesign** — see `Research_Material_System.md`. Current state is further along than expected:
  - `Material3D` already exists as authoring node with 5 texture inputs
  - Cube3D etc. already have `mat` input (input 1) for direct Material3D connection
  - CyclesRenderer uses `MaterialProvider` to build shader graphs
  - ScanlineRender does NOT consume materials (raw image textures only)
  - **Gaps:** ParticleInstance is hardcoded orange, no AssignMaterial node, no Cycles shader caching, ScanlineRender doesn't do PBR
  - **Recommended: Hybrid (Option C)** — direct input for simple cases + AssignMaterial node for scene-wide/pattern-based
  - **Phase 1:** `resolveMaterial()` helper + `MaterialBindingTable` on MaterialProvider.h
  - **Phase 2 (biggest win):** Extend ParticleInstance to 9 inputs (4 geo + 4 mat + 1 master override). Precedence: `masterOverride > pairedMat > geo->getConnectedMaterial() > geo inline knobs`. Kill the orange hardcode. Add per-material shader cache in Cycles.
  - **Phase 3:** `AssignMaterial` node (scene passthrough + material input + name pattern). Downstream overrides upstream.
  - **Phase 4:** Face groups / collections / ScanlineRender PBR (deferred)
- **Per-instance color in Cycles shader** — use `ObjectInfoNode` to pipe per-particle color from `obj->set_color()` into the Principled BSDF base color (partial fix for the hardcoded orange, before full material system)
- **Particle caching** — simulation re-runs on every frame scrub (should cache per-frame)
- **Color ramp UI** — pick a gradient of colors for per-particle random sampling (see `Research_Particle_Color_And_MotionBlur.md`)
- **ReadAlembicTransform animation path** — draw the motion trail in the viewport
- **Mesh collision** — per-triangle ray intersection for Alembic geo (requires ReadAlembicGeo)
- **Vertex selection in 3D viewport** — click on mesh vertex (see `Research_Vertex_Selection_3DViewport.md`)
- **ParticleExpression** — per-particle scripting for custom forces/behaviors

---

## 3D Viewport

ImGuizmo-based viewport with orbit camera, translate/rotate/scale gizmos (W/E/R keys), undo support, light-type icons. Toolbar exposes: gizmo space toggle (world/local), Grid show/hide, **View** dropdown (look through any Camera3D / ReadAlembicCamera), **Shading** dropdown (Wireframe / Shaded / Shaded+Wire), and (NATRON_CYCLES builds) a Render button.

### Known Issues (Fixed)
- ~~Point cloud persists after deleting DeepToPoints node~~ — Fixed
- ~~No undo/redo for gizmo transforms~~ — Fixed: GizmoTransformUndoCommand

### Completed (2026-05-21)
- **Look-through camera** — pick any CameraProvider node from the View dropdown to drive the viewport. While looking through a Camera3D, `Alt+Left` orbit / `Middle` pan / `Alt+Right` dolly write back to the camera's translate/rotate knobs (Alembic cameras are read-only and skip this).
- **Camera gate** — when looking through a camera, the 3D draw is letterboxed to the camera's sensor aspect so the wireframe matches the ScanlineRender output bit-perfect. A thin warm-orange outline marks the gate edge.
- **Shading dropdown** — Wireframe / Shaded / Shaded+Wire (default). Meshes render as solid grey + edges by default. Shapes carrying textures (Sphere3D/Cube3D/Card3D/Cylinder3D) still display them in Shaded / Shaded+Wire; Wireframe suppresses textures for a clean schematic view.

### TODO
- Selection highlight could be more visible
- Performance with very large point clouds (>10M points) untested
- Tumble-pivot indicator (small dot at orbit centre while orbiting) — Maya/Blender convention
- "Show Camera Gate" toggle to hide the orange outline when not wanted

---

## Cycles Renderer

CyclesRender node with path tracing, PBR materials, lights, animation. Optional build with `-DNATRON_CYCLES=ON`.

### Files
- `Engine/Dev/Cycles/CyclesRender.h/cpp` — Natron node interface
- `Engine/Dev/Cycles/CyclesRenderer.h/cpp` — Cycles bridge (scene sync, materials, lights, volumes)
- `patches/cycles-mingw.patch` — 2 MinGW fixes for upstream Cycles source
- `App/CMakeLists.txt` — Cycles library linking (10 Cycles libs + 17 deps)

### Completed (2026-04-08)

- **PrincipledVolume VDB rendering** — ReadVDB grids loaded via `VDBImageLoader`, bound to Cycles standard attributes (density/temperature/flame/color). PrincipledVolumeNode with ValueNode density (never `set_density()` — blocks attribute reads). FloatCurveNode remap for density and temperature.
- **Volume3D procedural shader** — sphere/box shapes via Cycles shader graph (TextureCoordinate → shape math → ScatterVolume + AbsorptionVolume). No dense ccl::Volume needed.
- **Explicit CPU device** — required for NanoVDB volume support. Set automatically in session creation.

### TODO
- GPU rendering (CUDA/OptiX) — not built yet
- Deep EXR output — WIP in Cycles upstream
- Viewport rendered mode (live progressive) — future phase
- Volume shader workflow WIP — absorption/scatter/blackbody/remap exposure needs iteration

---

## LensFlare3D

**Status: Not ported yet.** Steps 1-5 done in old project (glow, ghosts, starburst, streak, halo, depth occlusion). Steps 6-8 remaining (deep occlusion, GPU rendering, polish).

---

## UI Overhaul

**Status: Mostly ported.** Applied:
- Node width 80→100, corner radius 5px, gradient backgrounds
- Yellow selection highlight, thicker wires (3px)
- Wider spinboxes, muted yellow slider default color
- Dark grey QLineEdit borders

### Remaining Issues
- **Slider handle color** may appear teal instead of yellow on first run — the Settings default is set to `(0.71, 0.63, 0.31)` but cached user preferences may override it. User can change in Edit → Preferences → Appearance → Colors.
- **OFX plugin icons** not showing on nodes (Grade, Blur, etc.) — the `Resources/` directory is missing from the OFX bundle. Need to copy icon files from openfx-misc source into `Misc.ofx.bundle/Contents/Resources/`.
