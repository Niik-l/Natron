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

### TODO

- DeepWrite needs testing with actual deep EXR output
- DeepDefocus bokeh quality could be improved
- DeepExpression needs documentation for available variables

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

### Known Issues (Fixed)

- ~~Point cloud persists after deleting DeepToPoints node~~ — Fixed: use `getNodes_recursive(nodes, true)` instead of `getNodes()` to filter deactivated nodes.

### TODO

- Look-through camera mode needs more testing with ReadAlembicCamera
- No undo/redo for gizmo transforms
- Selection highlight could be more visible
- Performance with very large point clouds (>10M points) untested

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
