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
| **ReadVDB** | `fr.inria.built-in.ReadVDB` | OpenVDB volume loader (PrincipledVolume rendering via Cycles) |
| **Group3D** | `fr.inria.built-in.Group3D` | Groups 3D objects with unified transform |

### 3D Scene & Render (6 nodes — 5 registered, Project3D disabled)
| Node | Plugin ID | Description |
|------|-----------|-------------|
| **Scene3D** | `fr.inria.built-in.Scene3D` | Aggregates 3D objects for rendering |
| **RenderPass** | `fr.inria.built-in.RenderPass` | Multi-pass filter: visibility, holdout, shadow catcher, light selection |
| **ScanlineRender** | `fr.inria.built-in.ScanlineRender` | GLSL 3.3 + MRT rasterizer; 6 AOVs; Shading modes; particle render |
| **CyclesRender** | `fr.inria.built-in.CyclesRender` | Cycles path tracer (NATRON_CYCLES); 12 AOVs |
| ~~**Project3D**~~ | ~~`fr.inria.built-in.Project3D`~~ | **Disabled 2026-05-24** — ~~Camera projection onto geometry~~. Superseded by UVProject. |
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
| **Volume3D** | `fr.inria.built-in.Volume3D` | Procedural volume (sphere, box) — Cycles shader graph |

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
| `Gui/DevViewport3D.{h,cpp}` | ImGuizmo-based 3D viewport (camera orbit, gizmos, shading modes, look-through camera, axis grid) |
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
ALL matrices are **column-major** (OpenGL convention). ImGuizmo's demo math functions (LookAt, Perspective, Cross, Dot, Normalize) are used verbatim throughout DevViewport3D to prevent convention mismatches.

### Cycles Integration
- **Hybrid approach**: Raw GL viewport for interactive work + Cycles path tracer for final renders
- **CPU-only first**: Embree for BVH acceleration, OIDN for denoising
- **Camera convention**: Cycles looks +Z (not -Z like OpenGL) — forward column negated in camera matrix
- **Light convention**: Cycles lights emit along -column2 of Object transform

### Viewport (DevViewport3D)
Replaced hand-rolled Viewport3D with ImGuizmo-based DevViewport3D:
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

`DevViewport3D` has the same modes (`eWireframe / eFlat / eShaded /
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
| 2026-03-29 | DevViewport3D (ImGuizmo), undo, light icons, spot/area knobs, code cleanup |
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
