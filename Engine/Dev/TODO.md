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

**Status: Phase 2 complete (12 nodes). Solver architecture refactored. Collision with geometry working.**

### Architecture (2026-04-06 refactor)

- **Force nodes are stateless** — `applyForce()` modifies velocities in-place, no cached positions. Viewing from a force node shows emitter positions with velocity tweaks (preview only, no accumulation).
- **ParticleSolver is the solver** — walks upstream with `collectUpstreamForces()` to find the emitter and all force nodes. Runs the single simulation loop each frame: spawn → forces → integrate → collide → expire. Only node that owns position/velocity state.
- **Works without geo** — if no geometry is connected to the `geo` input, ParticleSolver acts as a pure solver (forces + integration, no collision). Always put ParticleSolver at the end of the chain.
- **OBB collision** — reads rotateX/Y/Z from connected Cube3D, transforms particles into local space for AABB test, transforms back. Standard technique (Unity/Unreal/Blender).
- This matches the standard pattern used by Blender, Unity, Unreal, and Houdini. See `Research_Particle_Sim_Loop.md`.

### Known Issues

- **No substeps in solver** — one integration step per frame. Fast particles can tunnel through thin colliders. Testbed has 4 substeps. Adding substeps to the solver loop is straightforward but not yet done.
- **Force preview is velocity-only** — viewing from a force node (e.g. Gravity) shows emitter positions, not accumulated force positions. Must view from ParticleSolver for proper simulation.
- **Node renamed** — ParticleCollide → ParticleSolver (2026-04-06). Plugin ID is now `ParticleSolver`.

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
