# Dev Nodes — Known Issues & TODO

Tracking known bugs, incomplete features, and planned improvements.

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

### TODO

- ReadVDB requires OpenVDB library (optional dependency, not tested on this build)
- ReadGeo/ReadAlembicCamera require Alembic library (optional, not tested)
- ScanlineRender output quality is basic — no anti-aliasing, no shadows
- Light3D shadow ray marching is functional but slow for dense volumes

---

## Particles

### Known Issues

- None currently — ParticleEmitter and ParticleGravity compile and register.

### TODO

- Particle rendering in ScanlineRender needs testing
- No particle caching — simulation re-runs on every frame scrub
- Turbulence force could use Perlin noise improvement

---

## 3D Viewport

ImGuizmo-based viewport with orbit camera, translate/rotate/scale gizmos (W/E/R keys), undo support, light type icons.

### Known Issues (Fixed)
- ~~Point cloud persists after deleting DeepToPoints node~~ — Fixed
- ~~No undo/redo for gizmo transforms~~ — Fixed: GizmoTransformUndoCommand

### TODO
- Look-through camera mode needs more testing with ReadAlembicCamera
- Selection highlight could be more visible
- Performance with very large point clouds (>10M points) untested

---

## Cycles Renderer

CyclesRender node with path tracing, PBR materials, lights, animation. Optional build with `-DNATRON_CYCLES=ON`.

### Files
- `Engine/Dev/Cycles/CyclesRender.h/cpp` — Natron node interface
- `Engine/Dev/Cycles/CyclesRenderer.h/cpp` — Cycles bridge (scene sync, materials, lights)
- `patches/cycles-mingw.patch` — 2 MinGW fixes for upstream Cycles source
- `App/CMakeLists.txt` — Cycles library linking (10 Cycles libs + 17 deps)

### TODO
- GPU rendering (CUDA/OptiX) — not built yet
- Deep EXR output — WIP in Cycles upstream
- Viewport rendered mode (live progressive) — future phase

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
