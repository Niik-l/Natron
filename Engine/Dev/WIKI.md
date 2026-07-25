# Natron Dev System — Wiki

User-facing reference for the custom dev nodes — architecture, knobs, and workflows.
Started as the particle-system wiki (the bulk below); now also covers volume rendering
(FastVolumeRender / Volume3D / ReadVDB). See `NODE_REGISTRY.md` for the full node list.

---

## Architecture

**Force nodes are stateless.** They modify velocity via `applyForce()` but don't own particle positions.

**ParticleSolver is the solver.** It walks upstream, gathers all force nodes, and runs the single simulation loop each frame: spawn → forces → integrate → collide → expire. Always place it at the end of the force chain.

**ParticleEmitter is the source.** It spawns particles and handles age-based appearance (color, size, fade).

```
Typical pipeline:

  ReadAlembicTransform ──(transform)──┐
                                      ↓
  ParticleEmitter → ParticleGravity → ParticleSolver ──→ ScanlineRender
                                          ↑ (geo)
                                       Cube3D
```

---

## Nodes

### ParticleEmitter
**Group:** Particles | **Inputs:** 2 (mask, transform) | **Output:** particles

The source of all particles. Spawns particles each frame with initial velocity, direction, color, and size.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Rate | Int | 100 | Particles spawned per frame |
| Lifetime | Double | 50 | Frames before particle dies |
| Lifetime Variance | Double | 10 | Random variation on lifetime |
| Velocity | Double | 1.0 | Initial speed |
| Velocity Variance | Double | 0.1 | Random variation on speed |
| Spread (degrees) | Double | 15 | Emission cone angle |
| Start Size | Double | 0.1 | Initial particle size |
| Size Variance | Double | 0.02 | Random variation on size |
| Seed | Int | 0 | Random seed |
| Emitter Shape | Choice | Point | Point, Disc, Sphere, Line |
| Shape Size | Double | 1.0 | Radius of emitter shape |
| Translate X/Y/Z | Double | 0,0,0 | Emitter world position |
| Emit Direction X/Y/Z | Double | 0,1,0 | Emission direction (Y-up default) |
| Start Color R/G/B/A | Double | 1,1,1,1 | Initial particle color |
| End Color R/G/B | Double | 1,1,1 | Color at end of life |
| End Size | Double | 0.1 | Size at end of life |
| Fade In | Double | 0.0 | Fraction of life for fade-in (0-1) |
| Fade Out | Double | 0.3 | Fraction of life for fade-out (0-1) |
| Mask Threshold | Double | 0.5 | Threshold for mask-based emission |
| Plane Scale | Double | 10.0 | Scale of mask emission plane |
| Plane Orientation | Choice | XZ | XY, XZ, YZ |
| Color From Image | Bool | false | Sample color from mask input |

**Transform input (input 1):** Connect a ReadAlembicTransform or any node with translateX/Y/Z and rotateX/Y/Z knobs. The emitter position and emit direction will follow the connected transform automatically. No expression linking needed.

---

### ParticleSolver
**Group:** Particles | **Inputs:** 2 (particles, geo) | **Output:** particles

The simulation solver. Walks upstream to find the emitter and all force nodes, then runs the integrated simulation loop. Also handles collision when geometry is connected.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Elasticity | Double | 0.5 | Bounce energy: 1.0 = perfect bounce, 0 = stick |
| Friction | Double | 0.2 | Tangential velocity loss: 0 = slide, 1 = full stop |
| Max Bounces | Int | 0 | Kill particle after N bounces. 0 = unlimited |
| Substeps | Int | 4 | Subdivisions per frame. Higher = smoother, less tunneling |
| Show Collisions | Bool | false | Debug: tint collided particles red |

**Geo input (input 1, optional):** Connect a Cube3D or Sphere3D for collision. Reads translate, rotate, scale, and size knobs. Supports OBB (rotated box) collision. Without geo, acts as a pure solver (forces + integration only).

**How it works:**
1. Walks upstream with `collectUpstreamForces()` to find emitter + forces
2. Each frame: picks up new particles from emitter
3. Applies all forces in chain order (gravity first, then wind, etc.)
4. Integrates positions (with substeps: dt = 1/substeps)
5. Applies collision (ray-AABB or ray-sphere, with push-out for spawn-inside)
6. Syncs appearance (color, size) from emitter's age-based interpolation
7. Removes expired particles

---

### ParticleGravity
**Group:** Particles | **Inputs:** 1 (particles) | **Stateless**

Constant directional acceleration. Respects particle mass.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Gravity X | Double | 0 | X acceleration |
| Gravity Y | Double | -9.8 | Y acceleration (negative = down) |
| Gravity Z | Double | 0 | Z acceleration |
| Strength | Double | 0.01 | Multiplier on gravity vector |

---

### ParticleWind
**Group:** Particles | **Inputs:** 1 (particles) | **Stateless**

Directional wind force with per-particle gustiness variation.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Direction X/Y/Z | Double | 1,0,0 | Wind direction |
| Strength | Double | 1.0 | Wind force |
| Gustiness | Double | 0.3 | Per-particle random variation (0-1) |

---

### ParticleDrag
**Group:** Particles | **Inputs:** 1 (particles) | **Stateless**

Velocity damping. Slows particles over time.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Drag | Double | 0.02 | Damping per frame (0-1). Higher = more drag |

---

### ParticleAttract
**Group:** Particles | **Inputs:** 1 (particles) | **Stateless**

Attract or repel particles from a point in space.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Point X/Y/Z | Double | 0,0,0 | Center of attraction |
| Strength | Double | 1.0 | Force strength (negative = repel) |
| Falloff | Choice | Linear | None, Linear, Inverse Square |
| Max Distance | Double | 10.0 | Particles beyond this are unaffected |

---

### ParticleVortex
**Group:** Particles | **Inputs:** 1 (particles) | **Stateless**

Spinning vortex force around a configurable axis.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Axis X/Y/Z | Double | 0,1,0 | Rotation axis direction |
| Center X/Y/Z | Double | 0,0,0 | Vortex center point |
| Strength | Double | 1.0 | Spin speed (negative = reverse) |
| Inward Pull | Double | 0.0 | Radial force toward axis (negative = push out) |

---

### ParticleTurbulence
**Group:** Particles | **Inputs:** 1 (particles) | **Stateless**

3D curl noise for organic swirling motion. Uses FBM (fractal Brownian motion) with 3 independent Perlin noise fields.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Strength | Double | 1.0 | Force magnitude |
| Scale | Double | 1.0 | Noise frequency (smaller = larger swirls) |
| Speed | Double | 1.0 | Animation speed of the noise field |
| Octaves | Int | 3 | Fractal detail layers |
| Lacunarity | Double | 2.0 | Frequency multiplier per octave |
| Gain | Double | 0.5 | Amplitude multiplier per octave |

---

### ParticleTurbulence2D
**Group:** Particles | **Inputs:** 1 (particles) | **Stateless**

2D planar curl noise. Lighter computation than 3D turbulence, good for sheet-like flowing effects.

Same knobs as ParticleTurbulence.

---

### ParticleKillBox
**Group:** Particles | **Inputs:** 2 (particles, bounds) | **Stateless**

Removes particles inside or outside a bounding box.

**Bounds input (recommended):** connect a Cube3D — its full transform
(translate/rotate/scale/uniform scale/size, rotated boxes supported) defines the
kill volume, visible and movable with the gizmo in the 3D viewport. The volume
matches the wireframe exactly. A Cube3D feeding a bounds input auto-displays as
wireframe (its **Viewport Display** knob: Auto/Solid/Wireframe) so the particles
inside stay visible. Without a bounds input, the knobs below define an
axis-aligned box.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Min X/Y/Z | Double | -5,-5,-5 | Box minimum corner (no bounds input) |
| Max X/Y/Z | Double | 5,5,5 | Box maximum corner (no bounds input) |
| Center X/Y/Z | Double | 0,0,0 | Offset for the box (no bounds input) |
| Mode | Choice | Kill Inside | Kill Inside, Kill Outside |

---

### ParticleSpawn
**Group:** Particles | **Inputs:** 1 (parents) | **Output:** child particles

Secondary particle emitter. Spawns new particles from existing parent particles based on trigger conditions.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Trigger | Choice | Continuous | Continuous, On Birth, On Death, On Collision |
| Rate | Int | 3 | Children per triggered parent per frame |
| Inherit Velocity | Double | 0.5 | How much parent velocity carries to children (0-1) |
| Extra Speed | Double | 0.1 | Additional speed for children |
| Spread | Double | 45 | Emission cone angle for children |
| Child Lifetime | Double | 20 | Lifetime of child particles |
| Child Size | Double | 0.05 | Size of child particles |
| Child Color R/G/B | Double | 1, 0.5, 0.1 | Color of child particles |
| Probability | Double | 1.0 | Chance of spawning per triggered parent (0-1) |

**On Collision trigger** requires ParticleSolver upstream (sets the `collided` flag).

---

### ParticleMerge
**Group:** Particles | **Inputs:** 4 (A, B, C, D) | **Output:** merged particles

Combines up to 4 particle streams into one. No knobs.

---

### ParticleInstance
**Group:** Particles | **Inputs:** 5 (particles, geo A-D) | **Output:** particles (pass-through)

Instance geometry at particle positions. Each particle gets assigned a source geo based on the distribution mode.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Distribution | Choice | Random | Sequential (round-robin), Random (by particle ID), By ID (modulo) |
| Orient to Velocity | Bool | false | Rotate instances to face velocity direction |
| Scale Multiplier | Double | 1.0 | Global scale on all instances |
| Max Instances | Int | 10000 | Performance limit |

**Pipeline:**
```
ParticleSolver → ParticleInstance → Scene → ScanlineRender
                     ↑ ↑ ↑ ↑
                 Cube3D Sphere3D (geo A, B, C, D)
```

---

### ReadAlembicTransform
**Group:** 3D | **Inputs:** 0 | **Output:** animated transform knobs

Reads animated transforms (nulls/locators) from Alembic (.abc) files. Primary use: constrain a ParticleEmitter to a point on animated geometry.

| Knob | Type | Description |
|------|------|-------------|
| File | File | Path to .abc file |
| Transform | Choice | Dropdown of all IXform nodes found in the file |
| Reload | Button | Re-read the file |
| Frame Offset | Int | Shift animation timeline |
| FPS | Double | Frames per second (default 24) |
| Translate X/Y/Z | Double | Animated output — world position |
| Rotate X/Y/Z | Double | Animated output — rotation (degrees) |
| Scale X/Y/Z | Double | Animated output — scale |

**3D Viewport:** Shows an RGB axis cross + yellow diamond at the null position. Rotates with animation. Gizmo suppressed (read-only).

**Workflow:** Connect to ParticleEmitter's `transform` input. Position + emit direction follow automatically.

---

## Particle Attributes

Each particle carries these attributes through the pipeline:

| Attribute | Type | Description |
|-----------|------|-------------|
| px, py, pz | float | World position |
| prevPx, prevPy, prevPz | float | Previous frame position (for collision ray tests) |
| vx, vy, vz | float | Velocity |
| r, g, b, a | float | Color + opacity |
| size | float | Display/instance scale |
| age | float | Current age in frames |
| life | float | Total lifetime in frames |
| mass | float | Affects force strength (default 1.0) |
| id | uint32 | Unique ID, persists across frames |
| bounceCount | int | Number of times bounced |
| collided | bool | True on the frame of a bounce |

---

## ScanlineRender Particle Settings

On the **Particles** tab of ScanlineRender:

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Particle Mode | Choice | Sprite | Point, Disc (soft circle), Sphere (N.L lit), Sprite (flat quad) |
| Blend Mode | Choice | Additive | Additive (fire/sparks), Over (solid/smoke) |
| Particle Scale | Double | 1.0 | Global size multiplier |
| Motion Blur (stretch) | Double | 0 | Fast velocity-stretch motion blur (cheat). Multiplied by Shutter. Only active when Motion Samples = 1. Beauty-only — for motion-blurred AOVs use Motion Samples > 1. |
| Motion Samples | Int | 1 | Physically-accurate multi-sample motion blur. 1 = off, 4-8 = typical, 16 = film quality. Renders scene N times at sub-frame offsets and averages. Works for all modes including instances. |
| Shutter | Double | 0.5 | Shutter open fraction. 0.5 = 180° film shutter. Controls blur amount in both modes. |

## ScanlineRender Shading + AOVs

ScanlineRender draws via a GLSL 3.3 + MRT pipeline (the legacy fixed-function path was retired in Phase 3E).

**Shading Mode** knob on the **Output** tab — applies to mesh geometry (Sphere3D / Card3D / Cube3D / Cylinder3D / ReadGeo / Alembic):

| Mode | Behavior |
|------|----------|
| Shaded (default) | Per-pixel N.L diffuse + 0.15 ambient. Uses a `Light3D` if connected, otherwise a camera-relative headlight (Maya default convention). |
| Flat | No lighting — raw texture / vertex color. Matches the legacy pre-Phase-3 look. |
| Wireframe | Solid white GL_LINES derived from triangle indices. |

Particles always use their own additive/over blend and are not affected by Shading Mode.

**AOVs** — toggled on the **AOVs** tab. Each off by default; enabling adds a per-pixel plane to the output:

| AOV | Plane | Notes |
|-----|-------|-------|
| Depth | `depth.Z` | Linear camera-space distance. Background = camFar. Particles only contribute via the static Sphere mode (other modes are translucent, no depth write). |
| World Position | `world_position.xyz` | Reconstructed from depth via `inverse(proj × view)`. Same particle caveat as Depth. |
| Normal | `Normal.xyz` | World-space surface normal. For particles: camera-facing on Sprite/Disc/Point, true per-vertex on Sphere static. |
| UV | `uv.uvw` | Per-vertex UVs. Particles emit zero except Sprite static (quad-corner gradient). |
| Pref | `Pref.xyz` | Object-space reference position. Particles use their world position as a stable per-particle ID. |
| Velocity | `Velocity.xyz` | Screen-pixels-per-frame motion vector. Re-extracts geometry + camera at `time - 1`. Per-particle vel = `(p.vx, p.vy, p.vz)`. |

**Vector-blur / defocus workflow:** render at single-sample, enable Velocity + Depth (static Sphere particles needed for Depth), then run Nuke's VectorBlur + ZDefocus downstream. Avoids the cost of `Motion Samples > 1` while still producing motion-blurred + depth-of-field output.

**Anti-aliasing:** ScanlineRender uses **4x MSAA** via a multisampled FBO + blit-to-resolve pattern. All particles, geo, and instances get smoothed edges automatically.

## CyclesRender Particle Support

CyclesRender natively supports particles and ParticleInstance:

- **Particles** (ParticleSolver output) → rendered as `ccl::PointCloud` with per-particle vertex color, Principled BSDF emission, and motion blur (3-step motion attribute evaluated at shutter open/close times).
- **ParticleInstance** (instanced geo) → rendered via **native Cycles instancing**. Each connected geo source (Cube3D, Sphere3D) becomes a prototype `ccl::Mesh`, and each particle instance becomes a `ccl::Object` sharing that mesh. Extremely efficient — thousands of instances share just 1-4 meshes.
- **Motion blur on instances** → velocity-based extrapolation. Each `ccl::Object` gets a 3-element `set_motion()` array (shutter open / center / close), computed as `position + velocity * sampleDt`.

---

## Example Pipelines

### Basic particles with gravity
```
ParticleEmitter → ParticleGravity → ParticleSolver → Scene → ScanlineRender
```

### Collision with bouncing
```
ParticleEmitter → ParticleGravity → ParticleSolver → Scene → ScanlineRender
                                        ↑ (geo)            ↑ (cam)
                                     Cube3D            Camera3D
```

### Sparks on collision
```
ParticleEmitter → Gravity → ParticleSolver (Cube3D geo)
                                 ↓
                          ParticleSpawn (On Collision) → ParticleMerge → Scene → ScanlineRender
                                                              ↑
                                              ParticleSolver output ──┘
```

### Emitter following animated Maya null
```
ReadAlembicTransform ──(transform)── ParticleEmitter → Gravity → ParticleSolver
```

### Instanced geo on particles
```
ParticleEmitter → Gravity → ParticleSolver → ParticleInstance → Scene → ScanlineRender
                                                 ↑ ↑
                                            Cube3D  Sphere3D
```

### Complex setup (everything)
```
ReadAlembicTransform ──(transform)──┐
                                    ↓
ParticleEmitter → Gravity → Wind → ParticleSolver (Cube3D geo)
                                        ↓
                                 ParticleSpawn (On Collision)
                                        ↓
                                 ParticleMerge ← ParticleSolver output
                                        ↓
                                 ParticleInstance (Cube3D, Sphere3D)
                                        ↓
                                     Scene → ScanlineRender + Camera3D
```


---

## Templates Menu

The **Templates** menubar builds ready-to-play example graphs, organised by
category: **3D** (Basic 3D Scene, VDB Fire/Smoke), **Particles** (below), and
**Project3D / Deep / HDRI** (placeholders, templates to come). Each template is
a starting point — refine knobs to taste.

Particle templates:

| Template | What it builds |
|----------|----------------|
| Basic Particle System | Emitter → Gravity → Solver → Scene → ScanlineRender |
| Sparks | Emitter raining onto a Cube3D floor; Spawn On-Collision fires hot orange sparks, merged with the main stream |
| Jet Exhaust | Hot fast core + slow gray smoke trail — two emitter/force/solver chains merged (forces upstream of a Merge don't reach a shared solver, so each branch owns its chain); additive discs + stretch motion blur |
| Heat Distortion (ST Distort) | Red/green turbulent particle branches rendered additive → Blur → IDistort UV input, distorting the plate (Checkerboard placeholder — swap in footage). Production heat-haze rig |
| Rain + Splashes | Wide disc emitter, Max Bounces=1 kills drops on floor impact, Spawn On-Collision splash droplets, full stretch motion blur for streaks |

Adding a template (dev): a slot in `Gui.h`, a menu action in `Gui.cpp`
(Templates block), and a `runTemplatePython` block in `Gui50.cpp`.

---

## Deep Point Blasting (Houdini-style)

Delete regions of a deep render interactively and keep deep compositing:

```
DeepRead → DeepToPoints → [Blast → Blast → ...] → DeepRecolor / DeepMerge / DeepFlatten / DeepWrite
```

1. View the DeepToPoints in a 3D Viewport. Set **Density = 1.0** when blasting
   for edit (thinned points aren't in the cloud and can't be deleted).
2. Drag-select points, hit **Delete** (or Backspace) — a Blast node in
   Selection mode is created, wired to the displayed cloud and seeded with the
   selection. The points vanish immediately.
3. Each Delete chains another Blast. Disable or delete a Blast to restore that
   batch; its **Invert** knob flips delete-selected into keep-selected.
4. Right-click in the viewport for the menu version plus Add/Remove/Set/Clear
   refinement on an existing Blast.
5. The last Blast's output IS a deep image: points carry their deep sample
   index through the chain, and the Blast rebuilds the source deep minus the
   deleted samples — wire it into any deep node downstream.

Works on any point cloud (DeepToPoints, CameraTracker, PointCloudGenerator).
Blast stores indices, so re-solve trackers BEFORE blasting.

Karma DCM note: deep camera maps carry only A/Z/ZBack. DeepRecolor appends
R/G/B from a flat render (its main job); DeepFog after DeepRecolor gets
colored fog.

---

## DeepExpression (v2.0, Nuke-style)

Per-sample expressions. Empty field = channel unchanged; all expressions read
the INPUT values (simultaneous assignment).

- **Fields:** four temp-variable rows (`name` = `expression`, evaluated top to
  bottom, usable below), then `rgba.red/green/blue/alpha`, `deep.front`,
  `deep.back`. Writing a channel the deep lacks appends it.
- **Variables:** `rgba.red`… / `deep.front` / `deep.back` with aliases
  (`red`/`r`, `front`/`z`, `back`/`zback`), `x`, `y` (bottom-up), `frame`.
- **Language:** `+ - * / % ^`, comparisons, `&& || !`, ternary `?:`,
  `abs floor ceil round sqrt exp log log10 sin cos tan asin acos atan atan2
  pow fmod min max step clamp lerp mix smoothstep`, constants `pi`, `e`.
- **Examples:** thickness setup — temp `th = 0.1`, `deep.front: front - th/2`,
  `deep.back: back + th/2`. Depth fade — `rgba.alpha: alpha *
  smoothstep(20, 10, front)`.

Not supported vs Nuke: TCL `[value knob]` substitution, chans0-3 layer pickers.

---

## Volume Rendering

> **Scope note:** this wiki started as the particle-system reference; the volume nodes below
> were added when the FastVolumeRender GPU renderer landed. See `NODE_REGISTRY.md` for one-line
> summaries and `DEV_3D_SYSTEM_CHANGELOG.md` for history.

Wire `ReadVDB` (or a procedural `Volume3D`) + a `Light3D` under a `Group3D`, then into
`FastVolumeRender` for interactive GPU preview (or `CyclesRender` for final quality).

### FastVolumeRender
**Group:** 3D | **Inputs:** 2 (Scene, Camera) | **Output:** image (RGBA + AOVs)
**Build:** gated `NATRON_FASTVOLUME` (needs wgpu-native — see `BUILDING.md`).

Real-time GPU volume renderer (wgpu-native compute ray-marcher) — an interactive stand-in for
CyclesRender volumes. **Input 0 = Scene**: a `ReadVDB` *or* `Volume3D`, plus `Light3D`(s),
optionally under a `Group3D`. **Input 1 = Camera** (optional; auto-frames if absent).
**Lighting is entirely from scene Light3D nodes** (no sun/ambient knobs):
- A **Distant** light = the sun (direction = the light's world +Z, plus colour/intensity).
- A **Dome** light = ambient fill (solid colour; no HDRI sampling).
- Multiple key lights at once (Point/Distant/Spot/Area, cap 8), each self-shadowed. **Area = a
  rectangular beam** (Width/Height → laser/light-shaft).
- **Fire-as-light**: flames illuminate nearby smoke (Fire Light knob). Zero lights = unlit.

A fresh Light3D defaults to **Point** — set it to **Distant** for a sun. The light **must** be
upstream (through the Group/Scene input) or the render won't update when you tweak it.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Density Scale | Double | 0.55 | Extinction multiplier — thicker smoke |
| Albedo | Double | 0.72 | Scattering albedo (1 = white, lower = sootier) |
| Anisotropy | Double | 0.5 | Henyey-Greenstein phase (>0 = forward / silver lining) |
| Fire Intensity | Double | 2.2 | Flames-grid emission to camera |
| Fire Max | Double | 3.0 | Flames value mapped to the top of the fire ramp |
| Fire Light | Double | 1.0 | How much fire lights surrounding smoke (0 = emit only) |
| Steps | Int | 1024 | Ray-march steps |
| AOVs (page) | Bools | on | Emission, AmbientScatter, Depth, Temperature, Shadow, per-Light-Group RGBA |

**Performance:** per-frame compression + uniform-buffer cache — look/light tweaks reuse the
resident GPU upload (draw-only). First render of a session pays a one-time ~0.7–1 s GPU init;
scrubbing a VDB *sequence* is ~1 s/frame (per-frame recompress). GPU: verified NVIDIA only;
cross-vendor (DX12/Vulkan) so AMD/Intel should work but are untested; no GPU → error, no crash;
Windows-only as built.

### Volume3D (procedural cloud)
**Group:** 3D | **Inputs:** 0 | **Output:** volume (density-only — no fire)

Procedural cloud/volume primitive. Feeds FastVolumeRender (GPU), ScanlineRender, or Cycles.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Preset | Choice | Custom | Cumulus / Stratus / Cumulonimbus / Wispy (loads a full combo) |
| Base Shape | Choice | Sphere | Sphere or Box |
| Resolution | Int | 64 | Voxel grid resolution (fine Edge Detail needs ≥~112) |
| Density | Double | 8.0 | Density multiplier |
| Enable Noise | Bool | off | Layer cloud noise onto the base shape |
| Warp | Double | 0.3 | Domain-warp → billows / cauliflower |
| Noise Scale / Detail | Double | — | FBM frequency / octaves |
| Coverage | Double | 1.0 | Cloud amount (lower → scattered puffs, → 0 fades out) |
| Edge Erosion | Double | 0.0 | Thins the rim into wisps |
| Edge Detail | Double | 0.0 | Fine high-contrast carve — fragments the rim into tendrils |
| Seed | Int | 0 | Per-copy variation |
| Wind X/Y/Z | Double | 0 | Noise drift / boil over time (cells/frame) |
| Base Flatness | Double | 0.0 | Flat condensation base |
| Top Flatness | Double | 0.0 | Flat top / anvil (with Base = a slab) |
| Height Falloff | Double | 0.0 | Dense base → wispy top (range 0–4) |

A static Volume3D caches; animating Wind (or any shape knob) re-generates + re-uploads. The 3D
viewport previews it as soft splats. (Legacy Step Size / Volume Bounces apply only to
ScanlineRender / Cycles — FastVolumeRender ignores them.)

### ReadVDB (viewport preview)
The OpenVDB loader (Cycles PrincipledVolume + a FastVolumeRender source) previews in the 3D
viewport as soft fire/smoke **splats**: it trilinearly samples the real density + fire grids on
a coarse lattice (cached per path/frame/res), smoke grey + fire via a black-body ramp. The
**Viewport Display Res** knob (`viewportDisplayRes`, default 40) controls the lattice density
(preview-only — no render trigger).
