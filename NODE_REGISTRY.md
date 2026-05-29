# Natron Node Registry

All custom built-in nodes added to Natron beyond the original upstream codebase.
Updated: 2026-05-24

---

## 3D Geometry (8 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Sphere3D** | `fr.inria.built-in.Sphere3D` | Registered | Tessellated sphere with equirectangular UVs |
| **Card3D** | `fr.inria.built-in.Card3D` | Registered | Flat textured quad |
| **Cube3D** | `fr.inria.built-in.Cube3D` | Registered | 24-vertex cube with per-face UVs |
| **Cylinder3D** | `fr.inria.built-in.Cylinder3D` | Registered | Tessellated cylinder with caps |
| **ReadGeo** | `fr.inria.built-in.ReadGeo` | Registered | Single-mesh loader for `.abc` (Alembic, requires NATRON_HAVE_ALEMBIC) and `.obj` (Wavefront, no dependency). Parser dispatches on file extension. OBJ group/object directives populate the Object dropdown. Supports animated xforms AND animated (deforming) vertex meshes — all samples pre-loaded at file open, mutated per-frame via getMeshData(time). User T/R/S knobs compose with the embedded xform. |
| **ReadAlembicArchive** | `fr.inria.built-in.ReadAlembicArchive` | Registered | Multi-mesh Alembic archive loader — reads full scene hierarchies, builds per-entry world transforms from the archive chain. Supports animated xforms + animated deforming vertex meshes per entry. Used by ScanlineRender / 3D viewport for whole-scene `.abc` ingest. |
| **ReadVDB** | `fr.inria.built-in.ReadVDB` | Registered | OpenVDB volume loader. PrincipledVolume fire rendering via Cycles (density/temperature/flame grids, absorption, remap curves). Animated sequences with frame padding. Viewport wireframe bbox from grid bounds. |
| **Group3D** | `fr.inria.built-in.Group3D` | Registered | Groups 3D objects with unified transform |

## 3D Scene & Render (6 nodes — 5 registered, Project3D disabled)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Scene3D** | `fr.inria.built-in.Scene3D` | Registered | Aggregates 3D objects for rendering |
| **RenderPass** | `fr.inria.built-in.RenderPass` | Registered | Multi-pass filter: object visibility, holdout, shadow catcher, light selection |
| **ScanlineRender** | `fr.inria.built-in.ScanlineRender` | Registered | GLSL 3.3 + MRT rasterizer (4x MSAA), 100% modern pipeline (no fixed-function holdouts). Per-pixel AOVs (Depth, World Position, Normal, UV, Pref, Velocity) for meshes, particles, AND geo instances. Shading modes: Shaded (N.L diffuse from Light3D, fallback camera headlight) / Flat / Wireframe. Scene-wide multi-sample motion blur with Centered / Start / End / Custom shutter offset modes + temporal jitter — covers camera, geometry, animated Alembic xforms+verts (sub-frame lerp), particles, and instances. Velocity-stretch cheat mode preserved for `motionSamples == 1`. Volume rendering (ReadVDB + procedural Volume3D) via GLSL ray-march with non-cubic 3D textures matching VDB voxel aspect. Geo instancing via VBO/VAO + uniforms (Sphere3D, Cube3D), n-gon fan-triangulation, Sync-to-Project canvas. |
| **CyclesRender** | `fr.inria.built-in.CyclesRender` | Registered | Cycles path tracer (requires NATRON_CYCLES). 13 AOV passes (diffuse/glossy direct/indirect/color, emission, env, AO, normal, depth, UV, mist). Single-value AOVs (AO/Depth/Mist) display as 3-channel grayscale R=G=B in Natron's viewer (1-channel "A" planes render black in default RGB display). PrincipledVolume VDB rendering, native particle instancing + motion blur, vertex motion blur for animated Alembic archive meshes via `ATTR_STD_MOTION_VERTEX_POSITION` (sub-time vertex sampling), PBR materials via Material3D. CPU device for NanoVDB volume support. Sync-to-Project canvas. |
| **CyclesRenderPassManager** | `fr.inria.built-in.CyclesRenderPassManager` | Registered | **MVP 1-5C** — sink node owning a JSON-backed list of render-pass specs (id / name / type / enabled-solo-mute-output flags / camera+vis+light+material overrides / AOV subset / file path / format-bitdepth-compression / samples). "Render to Disk" button parses + filters the active set (`enabled && output && !mute && (!soloActive \|\| solo)`), resolves dollar-token paths (`$PASS` / `$SHOT` / `$RENDER` from env, frame via `####` / `%04d` / digit-group), and currently writes synthetic 64×64 multi-layer EXRs as proof-of-life for the disk path. Real Cycles invocation lands in step 5A (refactor CyclesRender::render() to be reusable). Eventual goal: per-batch grouping by shared scene state so the Cycles session runs once per unique config and produces all batched passes' AOVs in a single render. |
| ~~**Project3D**~~ | ~~`fr.inria.built-in.Project3D`~~ | **Disabled 2026-05-24** | ~~Camera projection onto geometry (standalone FBO renderer).~~ Superseded by UVProject (6 projection modes incl. perspective-correct STW). Code kept in tree; re-enable via one-line uncomment in `AppManager.cpp`. |
| **UVProject** | `fr.inria.built-in.UVProject` | Registered | Rewrite mesh UVs via projection — 6 modes (Perspective / PlanarXY/YZ/ZX / Spherical / Cylindrical). Perspective with Generate Perspective ON emits 3-component (s,t,w) coords for fragment-level perspective divide via glTexCoord4f. Reference Frame lock supported. |

## Camera & Lighting (4 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Camera3D** | `fr.inria.built-in.Camera3D` | Registered | Camera with T/R, focal length, aperture, DOF. V Aperture default is computed from the project format (24.576mm H × projectH/projectW) so a fresh camera matches the project aspect out of the box. |
| **ReadAlembicCamera** | `fr.inria.built-in.ReadAlembicCamera` | Registered | Import animated camera from .abc |
| **ReadAlembicTransform** | `fr.inria.built-in.ReadAlembicTransform` | Registered | Import animated transform/null/locator from .abc. Outputs translate/rotate/scale. Connect to ParticleEmitter transform input. |
| **Light3D** | `fr.inria.built-in.Light3D` | Registered | Point/Spot/Area/Distant/Dome light with HDRI |

## Materials (2 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Material3D** | `fr.inria.built-in.Material3D` | Registered | Standalone PBR material with texture map inputs |
| **Volume3D** | `fr.inria.built-in.Volume3D` | Registered | Procedural volume (sphere, box). Translate/Rotate/Scale knobs, stepSize, volumeBounces. Renders via Cycles procedural shader graph. |

## Deep Compositing — Tier 1+2 (17 nodes, registered)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **DeepRead** | `fr.inria.built-in.DeepRead` | Registered | Read deep EXR files |
| **DeepWrite** | `fr.inria.built-in.DeepWrite` | Registered | Write deep EXR files |
| **DeepFlatten** | `fr.inria.built-in.DeepFlatten` | Registered | Flatten deep to 2D |
| **DeepMerge** | `fr.inria.built-in.DeepMerge` | Registered | Merge deep images |
| **DeepRecolor** | `fr.inria.built-in.DeepRecolor` | Registered | Recolor deep samples |
| **DeepSlice** | `fr.inria.built-in.DeepSlice` | Registered | Slice deep by depth range |
| **DeepHoldout** | `fr.inria.built-in.DeepHoldout` | Registered | Deep holdout matte |
| **DeepFromImage** | `fr.inria.built-in.DeepFromImage` | Registered | Convert 2D to deep |
| **DeepGrade** | `fr.inria.built-in.DeepGrade` | Registered | Grade deep samples |
| **DeepReformat** | `fr.inria.built-in.DeepReformat` | Registered | Reformat deep resolution |
| **DeepCrop** | `fr.inria.built-in.DeepCrop` | Registered | Crop deep images |
| **DeepToPoints** | `fr.inria.built-in.DeepToPoints` | Registered | Convert deep to point cloud. Optional Camera3D input unprojects pixels+depth into world space. |
| **DeepTransform** | `fr.inria.built-in.DeepTransform` | Registered | Transform deep images |
| **DeepExpression** | `fr.inria.built-in.DeepExpression` | Registered | Expression-based deep processing |
| **DeepColorCorrect** | `fr.inria.built-in.DeepColorCorrect` | Registered | Color correct deep samples |
| **DeepDefocus** | `fr.inria.built-in.DeepDefocus` | Registered | Defocus deep images |
| **Blast** | `fr.inria.built-in.Blast` | Registered | Point-cloud filter. Bounding-box mode (delete inside/outside a Cube3D bounds input) and Selection mode (right-click in 3D viewport). Invert toggle, info display, lazy computation. |

## Deep Compositing — Tier 3 (19 nodes, code exists but NOT registered)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **DeepAO** | `fr.inria.built-in.DeepAO` | Not registered | Deep ambient occlusion |
| **DeepBlend** | `fr.inria.built-in.DeepBlend` | Not registered | Deep blend |
| **DeepChannelMath** | `fr.inria.built-in.DeepChannelMath` | Not registered | Per-channel math on deep |
| **DeepConsolidate** | `fr.inria.built-in.DeepConsolidate` | Not registered | Consolidate deep samples |
| **DeepContactShadow** | `fr.inria.built-in.DeepContactShadow` | Not registered | Contact shadows from deep |
| **DeepDepthWarp** | `fr.inria.built-in.DeepDepthWarp` | Not registered | Depth-based warping |
| **DeepDifference** | `fr.inria.built-in.DeepDifference` | Not registered | Deep difference matte |
| **DeepFog** | `fr.inria.built-in.DeepFog` | Not registered | Atmospheric fog from deep |
| **DeepFromFrames** | `fr.inria.built-in.DeepFromFrames` | Not registered | Build deep from frame sequence |
| **DeepGodRays** | `fr.inria.built-in.DeepGodRays` | Not registered | Volumetric god rays |
| **DeepLayerBreak** | `fr.inria.built-in.DeepLayerBreak` | Not registered | Break deep into layers |
| **DeepNormalMatte** | `fr.inria.built-in.DeepNormalMatte` | Not registered | Normal-based deep matte |
| **DeepNormalize** | `fr.inria.built-in.DeepNormalize` | Not registered | Normalize deep samples |
| **DeepPositionMatte** | `fr.inria.built-in.DeepPositionMatte` | Not registered | Position-based deep matte |
| **DeepQuantize** | `fr.inria.built-in.DeepQuantize` | Not registered | Quantize deep sample count |
| **DeepRelight** | `fr.inria.built-in.DeepRelight` | Not registered | Relight from deep data |
| **DeepSampleFilter** | `fr.inria.built-in.DeepSampleFilter` | Not registered | Filter deep samples |
| **DeepSplit** | `fr.inria.built-in.DeepSplit` | Not registered | Split deep by criteria |
| **DeepVelocityMatte** | `fr.inria.built-in.DeepVelocityMatte` | Not registered | Velocity-based deep matte |

## Particles (16 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **ParticleEmitter** | `fr.inria.built-in.ParticleEmitter` | Registered | Emitter with Point/Sphere/Box/Disc shapes |
| **ParticleGravity** | `fr.inria.built-in.ParticleGravity` | Registered | Gravity force for particles |
| **ParticleDrag** | `fr.inria.built-in.ParticleDrag` | Registered | Velocity damping |
| **ParticleTurbulence** | `fr.inria.built-in.ParticleTurbulence` | Registered | 3D curl noise turbulence (volumetric swirling) |
| **ParticleTurbulence2D** | `fr.inria.built-in.ParticleTurbulence2D` | Registered | Planar curl noise turbulence (sheet-like flow) |
| **ParticleWind** | `fr.inria.built-in.ParticleWind` | Registered | Directional wind force with gustiness |
| **ParticleKillBox** | `fr.inria.built-in.ParticleKillBox` | Registered | Bounding box particle kill zone (inside/outside) |
| **ParticleAttract** | `fr.inria.built-in.ParticleAttract` | Registered | Attract/repel from a point (linear/inverse square falloff) |
| **ParticleVortex** | `fr.inria.built-in.ParticleVortex` | Registered | Spiral vortex force around an axis with inward pull |
| **ParticleSpawn** | `fr.inria.built-in.ParticleSpawn` | Registered | Secondary particle emitter (trails, sparks) — On Birth/Death/Collision triggers |
| **ParticleSolver** | `fr.inria.built-in.ParticleSolver` | Registered | Particle solver + collision. Owns simulation loop (forces → integrate → collide). Reads Cube3D/Sphere3D geo for collision (OBB with rotation). Works without geo as pure solver. Elasticity, friction, spawn-inside push-out. **Multi-frame RAM cache (Phase A+B 2026-05-27):** `getParticleData()` consults a per-instance `std::map<int, CachedFrame>`; hash-based invalidation on upstream change; exact-frame hit skips integration; miss resumes from nearest cached frame. Cache page knobs: Cache Simulation / Max Cache MB / Cached Frames + Cache RAM (read-only) / Clear Cache. (Eviction in Phase C.) |
| **ParticleInstance** | `fr.inria.built-in.ParticleInstance` | Registered | Instance geo at particle positions. 4 geo inputs (A-D). Distribution: Sequential/Random/ByID. Orient to velocity, scale multiplier, max instances. |
| **WriteAlembicParticles** | `fr.inria.built-in.WriteAlembicParticles` | Registered (2026-05-27) | **First disk-writing 3D node.** Passthrough particle node — particles flow through input 0 unchanged. Knobs: File (.abc output) + Bake button. Click Bake → iterates project frame range, writes OPoints sample per frame (positions, IDs, velocities, Cd color, size). Synchronous (UI freezes during bake; per-frame progress on stdout). Uses Alembic OArchive (Ogawa backend) + OPoints schema. |
| **ReadAlembicParticles** | `fr.inria.built-in.ReadAlembicParticles` | Registered (2026-05-27) | Source node — reads OPoints from an Alembic archive back into a particle stream. Knobs: File + Reload + Info label. Pre-loads all samples on file open (positions/IDs/velocities + optional Cd color and size from arbGeomParams). Per-frame lookup snaps to nearest sample via Alembic TimeSampling. V1 picks the first IPoints object; multi-points dropdown future work. Marks itself frame-varying when sample count > 1. Closes the bake-once round-trip with WriteAlembicParticles. |
| **ParticleAttribute** | `fr.inria.built-in.ParticleAttribute` | Registered (2026-05-27) | Per-particle attribute editor with **three always-visible sections** (Color / Pscale / Alpha) so users can shape the full particle look in one node and see all three contribute together. Each section is independent: Enable (on/off), Isolate (solos this section, radio-like across the three), Source (Age/Lifetime / Speed / Velocity X-Z / Position X-Z / BounceCount / SpawnIndex), Source Min/Max (range remap, animatable), **Fit button** (sample upstream → auto-fill min/max), editor (custom **Gradient** widget for Color via the new `KnobGradient`; `KnobParametric` curve for Pscale + Alpha), Mix (animatable lerp), Reset (per-section), and a live **Summary** label showing the current config. Top-level **Reset All** restores every section to defaults. Apply order: Color → Pscale → Alpha; an enabled Isolate solos that section. Placement: downstream of `ParticleSolver` (so the solver's appearance-resync doesn't overwrite the modifications). |
| **ParticleMerge** | `fr.inria.built-in.ParticleMerge` | Registered | Combine multiple particle streams (4 inputs) |

## Channel (1 node)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **DevShuffle** | `fr.inria.built-in.DevShuffle` | Registered | Channel shuffle node |

## Transform (1 node)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **SphericalTransform** | `fr.inria.built-in.SphericalTransform` | Registered | 8 projection types (equirect, cubemap, fisheye, etc.), rotation, interpolation. **Faces format** (cubemap mode): 6 per-face inputs in canonical order `-Z, +Z, -X, +X, -Y, +Y` enables split → edit → recombine HDRI workflows (each face is a separate image stream). |

## Color (3 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **ColorChartMatch** | `fr.inria.built-in.ColorChartMatch` | Registered | Color chart matching with matrix export (ACEScg default) |
| **ColorMatrix** | `fr.inria.built-in.ColorMatrix` | Registered | 3×3 color-channel matrix multiply with Invert toggle. Standard CC-pipeline building block. |
| **Exposure** | `fr.inria.built-in.Exposure` | Registered | Exposure (stops) + linear multiplier. Operates in scene-linear. |

## Other (1 node)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **CameraTracker** | `fr.inria.built-in.CameraTracker` | Disabled (WIP) | 3D camera solve from 2D tracks (uses libmv, excluded from build — GCC 15 Eigen issues) |

---

## Summary

| Category | Registered | Not Registered | Total |
|----------|-----------|----------------|-------|
| 3D Geometry | 8 | 0 | 8 |
| 3D Scene & Render | 6 | 0 | 6 |
| Camera & Lighting | 4 | 0 | 4 |
| Materials | 2 | 0 | 2 |
| Deep (Tier 1+2) | 17 | 0 | 17 |
| Deep (Tier 3) | 0 | 19 | 19 |
| Particles | 16 | 0 | 16 |
| Channel | 1 | 0 | 1 |
| Transform | 1 | 0 | 1 |
| Color | 3 | 0 | 3 |
| Other | 1 | 0 | 1 |
| **Total** | **59** | **19** | **78** |
