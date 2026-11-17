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
| Rotate X/Y/Z | Double | 0,0,0 | Rotate the whole emitter — shape (disc/box/line/mask plane) and emit direction turn together; composes with the transform input |
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

### ParticleAttribute (v2)
**Group:** Particles | **Inputs:** 1 | **Output:** particles with modified attributes

Per-particle look editor — four tabbed sections: **Color** (gradient), **Pscale**,
**Alpha**, and **Emission** (curves; Emission ships disabled — enable it for
per-particle glow in Cycles and brighter additive particles in ScanlineRender).

Each section: Enable / Isolate (solo), **Source** (Age, Age/Lifetime, Speed,
Velocity/Position, Bounce Count, Spawn Index, **Random (per ID)**, **Flicker
(noise)** — smooth per-particle time noise, node-level Flicker Speed),
**Range** dropdown (0–1 normalized · Auto fit-every-frame · Fit Now · Custom),
**Shape** presets (Linear / Cooling Decay / Bell / Grow-Fade / Ease Out /
Constant) with an **Edit Curve** toggle for free point remapping (hand edits
flip Shape to "Custom" and keep your points), Mix, and a **Variation** row:
stable per-ID breakup — hue jitter on Color, a biased multiplier on scalars
(Bias **Few High Outliers** = "some sparks are much hotter"). All randomness is
seeded ID hashing: deterministic across frames, scrubs, and re-sims.

**Spark recipe:** Emission on (default Cooling Decay), Variation ≈ 0.7 + Few
High Outliers, Color Hue Variation ≈ 0.1, then chain a second ParticleAttribute
with Emission Source = Flicker for twinkle. Layering = chaining.

---

### ParticleMaterial
**Group:** Particles | **Inputs:** 2 (particles, mat) | **Output:** particles (pass-through)

Particle shading override for CyclesRender. Connect a material (Material3D,
MergeMat, …) to **mat** → particles render with the full PBR material; **Tint
With Particle Color** (default on) multiplies per-particle color into the base
color and drives alpha by age-fade, so ParticleAttribute ramps show through.
Without a material: the default per-particle color shader with **Emission
Strength / Roughness / Metallic** knobs — emission 0 = purely scene-lit
(a chain *without* this node keeps the historic always-glowing look). With a
material connected, Emission Strength > 0 overrides the material's emission.

---

### PointsToParticles
**Group:** Particles | **Inputs:** 1 (points) | **Output:** static particles

Adapter: converts any point cloud (DeepToPoints, Blast, CameraTracker,
PointCloudGenerator) into static particles, unlocking the particle ecosystem
for point clouds — ScanlineRender's particle modes (point/disc/sphere/sprite,
additive/over), CyclesRender's native point rendering, and ParticleInstance.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Particle Size | Double | 0.05 | Size for every particle (points carry none). World units. |

Particles are static (infinite life, zero velocity); colors come from the
cloud, IDs are preserved when the cloud carries them. Conversion is cached on
the upstream cloud, so re-renders are free until the cloud changes.

**2.5D re-projection pipeline:**
```
DeepRecolor → DeepToPoints (camera input) → PointsToParticles → Scene3D → ScanlineRender (offset camera)
```

---

### Scatter3D
**Group:** Particles | **Inputs:** 2 (surface, density) | **Output:** static particles with a surface normal

Scatter points over the surface of a geo node for instancing: ReadGeo, GeoBuilder,
ReadAlembicArchive or a primitive on `surface`; an optional mask on `density`, looked up
through the surface's UVs (white = full density, black = none). Points are area-weighted
(as many per square metre on a big triangle as on a small one) and carry the face normal.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Count | Int | 1000 | Points to place. Rejections (slope, density, distance) can leave fewer; Info says how many. |
| Seed | Int | 0 | Reshuffle. |
| Size | Double min/max | 1 / 1 | Particle size per point, uniform in the range. ParticleInstance multiplies the prototype by it. |
| Slope Limit | Double | 90 | Steepest face (degrees from horizontal) that gets points. 90 = everything up to vertical walls, 30 = gentle slopes only, 180 = also faces pointing down. |
| Min Distance | Double | 0 | Minimum spacing in world units (0 = off). Even, non-clumping spread at the cost of reaching Count. |
| Normal | Choice | Surface | Surface (face normal), Surface (Flipped) for inside-out meshes, World Up (+Y). |
| Density Gamma | Double | 1 | Contrast of the density mask (>1 thins greys). |
| Invert Density | Bool | false | Scatter where the mask is black. |
| Display Size | Double | 1 | Viewport-only multiplier on the drawn point size. Renders and ParticleInstance ignore it; turn it down when tiny Size values for small prototypes make the preview points look huge. |
| Info | String | — | "N points on T tris (area)" plus the rejection breakdown when short of Count. |

Static (infinite life, zero velocity), white, IDs 0..N-1; cached per node hash and frame.
The density mask needs UVs on the surface (Info says when it was ignored).

**Pipeline:**
```
ReadGeo (terrain) → Scatter3D → ParticleInstance → Scene3D → CyclesRender
         Read (density map) ↗         ↑ ↑ ↑ ↑
                   ReadGeo + Material3D (Megascans rock / plant variants on geo A-D)
```

---

### ParticleInstance
**Group:** Particles | **Inputs:** 5 (particles, geo A-D) | **Output:** particles (pass-through)

Instance geometry at particle positions. Each particle gets one source geo (Distribution) and is
drawn with that prototype's material. Geo inputs take **any geo the 3D system renders**: Cube3D /
Sphere3D / Card3D / Cylinder3D, ReadGeo (a Megascans asset with its Material3D), GeoBuilder,
ReadAlembicArchive. Particles from Scatter3D carry a surface normal; Align to Normal stands the
prototype up along it.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Distribution | Choice | Random | Sequential (round-robin), Random (by particle ID + Seed), By ID (modulo) |
| Orient to Velocity | Bool | false | Rotate instances to face velocity direction (+Z). Wins over Align to Normal while moving. |
| Align to Normal | Bool | true | Turn the prototype's Up Axis onto the particle's surface normal (Scatter3D). Particles without a normal stay upright. |
| Up Axis | Choice | Y | Which prototype axis is up: Y (Natron, Maya, Megascans) or Z (Blender, Houdini exports). |
| Random Rotation | Double x/y/z | 0 / 0 / 0 | Random angle in [-range, +range] degrees per axis, in the prototype's own frame (before alignment). Up axis 360 = free spin, a few degrees on the others = natural lean. |
| Scale Multiplier | Double | 1.0 | Global scale on all instances (times the particle size) |
| Random Scale | Double min/max | 1 / 1 | Per-instance uniform scale factor in the range. |
| Seed | Int | 0 | Reshuffles Random Rotation / Random Scale / Random distribution. |
| Apply Prototype Transform | Bool | true | Include each prototype node's own T/R/S (a ReadGeo scaled cm→m instances at the right size). Off = raw mesh. |
| Hide Instanced Geo | Bool | true | Hide the source geo on the geo inputs at its own place in the scene (viewport, ScanlineRender, Cycles); only the instances show. Off = source geo visible too. A disabled ParticleInstance hides nothing. |
| Max Instances | Int | 10000 | Performance limit |
| Crypto Asset / Crypto Custom | String | empty | Cryptomatte page (every 3D node has it): the CryptoAsset name (inherited from a parent Group3D when empty, else the node) and the free CryptoCustom tag ("default" when empty). |
| Crypto Per Instance | Bool | false | Cryptomatte page: one CryptoObject ID per instance, named `<node>/<index>` (listed in the manifest); off = all instances share the node's ID. The asset layer keeps the whole system as one matte. |

Random values are keyed on the particle ID, so an instance keeps its rotation and scale from
frame to frame. CyclesRender shares one mesh (and BVH) between all instances of a prototype, so
tens of thousands of instances cost little more than one. ScanlineRender and the 3D viewport
show the prototype with its Material3D diffuse texture; the viewport falls back to bounding
boxes above 1.5M triangles in total. With Hide Instanced Geo on (default) the source geo on
the geo inputs is not drawn at its own place anywhere, only its instances are.

**Pipelines:**
```
ParticleSolver → ParticleInstance → Scene → ScanlineRender          (classic: cubes/spheres on a sim)
                     ↑ ↑ ↑ ↑
                 Cube3D Sphere3D (geo A, B, C, D)

Scatter3D (ground) → ParticleInstance → Scene3D → CyclesRender      (set dressing)
                         ↑ ↑ ↑ ↑
         ReadGeo + Material3D (Megascans debris / plants, Random Rotation y=360, Random Scale 0.7-1.3)
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

**Trail mode (2026-07-29):** Particle Mode = Trail renders each particle as a
camera-facing ribbon through its PAST positions (Houdini particle-trail style)
— bounces draw bent Vs, arcs curve. Knobs (shown only in Trail mode): Trail
Length (past frames), Head/Tail Width (teardrop taper), Tail Fade (alpha at
the tail), Tail Tint (head→tail color — deep red = white-hot head, cooling
tail). CyclesRender gets the same look via ParticleMaterial's Trails section
(native Cycles curves). First render at a new frame simulates the history
frames; the solver's cache makes later renders cheap.

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

- **Particles** (ParticleSolver output) → rendered as `ccl::PointCloud` with per-particle color (`particle_color` attribute — NOT `vertex_color`, which is a reserved Cycles standard-attribute name that silently reads black) and per-particle emission (`particle_emission` × ParticleMaterial's Emission Strength). Shading is controlled by a **ParticleMaterial** node in the chain: full PBR material on the `mat` input (with per-particle color tint + age-fade alpha) or the default color shader with Emission/Roughness/Metallic knobs. Motion blur uses 3 motion steps extrapolated from each particle's **velocity** at the center frame — exact per particle, correctly shutter-scaled (re-simulating at shutter times index-mismatched particles as they spawn/die).
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
category: **3D** (Basic 3D Scene, Fast Volume Render), **Particles** (below),
**HDRI** (below), and **Project3D / Deep** (placeholders, templates to come).
Each template is a starting point — refine knobs to taste.

3D templates:

| Template | What it builds |
|----------|----------------|
| Basic 3D Scene | Sphere + Light + Scene + Camera + CyclesRender + Viewer |
| Fast Volume Render (procedural cloud) | Procedural **Volume3D** cumulus (noise on, dense, flattened base/top) lit by a blue key + red fill point light → Scene → **FastVolumeRender** → Viewer: real-time GPU volume preview, no Cycles. A pathless **ReadVDB** sits beside the cloud as the slot for a fire/smoke sequence — set its file and wire it to Scene input 4 to swap the procedural volume for a real one (no .vdb is shipped) |

HDRI templates:

| Template | What it builds |
|----------|----------------|
| Basic HDRI | Lat-long face-edit rig: source → Reformat conformed to a custom **LatLong4K** 4096x2048 format (created project-wide) → 6 branches [Grade face-ID tint → SphericalTransform lat-long→cube-face → RotoPaint copy-blend] → Faces-input SphericalTransform reassembly → Viewer. Swap the CheckerBoard for your HDRI Read, paint out lights per face, and the result reassembles seamlessly to lat-long. |

Particle templates:

| Template | What it builds |
|----------|----------------|
| Temp - Basic Particle System | Emitter → Gravity → Solver → Scene → ScanlineRender |
| Temp - Sparks | Emitter raining onto a Cube3D floor; Spawn On-Collision fires hot orange sparks, merged with the main stream |
| Temp - Jet Exhaust | Hot fast core + slow gray smoke trail — two emitter/force/solver chains merged (forces upstream of a Merge don't reach a shared solver, so each branch owns its chain); additive discs + stretch motion blur |
| Temp - Heat Distortion (ST Distort) | Red/green turbulent particle branches rendered additive → Blur → IDistort UV input, distorting the plate (Checkerboard placeholder — swap in footage). Production heat-haze rig |

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

Dots (and DevStamps) are transparent to deep data: the deep side-channel
resolves through routing nodes centrally, so a Dot on any deep wire is safe.

To re-render a deep image from a new camera (2.5D), append
**PointsToParticles** after DeepToPoints and render the particles with
ScanlineRender — see the PointsToParticles node above.

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

## Gaussian Splats

A 3D Gaussian splat (3DGS) scan is a photographed scene stored as millions of small, soft,
coloured blobs. `ReadSplats` loads one; `SplatRender` renders it. Splats carry the lighting they
were captured with: they cannot be relit and do not appear in other renderers' reflections.

Typical graph: `ReadSplats` + `Camera3D` -> `SplatRender` -> Viewer. To line a scan up with other
3D elements first, put the `ReadSplats` in a `Scene3D` and look at it in the 3D viewport or through
`ScanlineRender`, where it shows as a point preview.

### ReadSplats
**Group:** 3D | **Inputs:** 0 | **Output:** splat scene (to Scene3D, ScanlineRender or SplatRender)

Formats: the binary `.ply` written by the standard 3DGS trainers, `.splat` (base colour only),
`.spz` versions 1 to 3. A plain binary point-cloud `.ply` loads as points. Not read: ASCII `.ply`,
`.spz` version 4, `.ksplat` (each gives a message on the node).

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| File / Reload | File, Button | - | The scan to load |
| SH Degree | Choice | 3 | How much view-dependent colour to keep in memory. 3 = all (236 bytes per splat), 0 = base colour only (56 bytes per splat), enough for layout work |
| Translate / Rotate / Uniform Scale | Double | 0 / 0 / 1 | Transform tab: places the scan |
| Y Down (COLMAP) | Bool | on | Turns the scan half a turn around X; COLMAP-trained scenes are Y down. Ignored for `.spz`, which is Y up |
| Point Scale | Double | 2.0 | Preview tab: size of the preview points |
| Size Limit | Double | 4.0 | Largest preview point, as a multiple of the median splat; stops a few huge background splats covering the picture |
| Opacity Threshold | Double | 0.1 | Splats fainter than this are left out of the preview |
| Viewport Max Points | Int | 4,000,000 | Larger scans are thinned evenly in the viewport (0 = no limit) |
| Render Max Points | Int | 0 | Same for ScanlineRender (0 = all) |
| sRGB to Linear | Bool | on | Converts the stored colours to linear for rendering; the viewport shows them as stored |
| Crop Mode / Invert / Fit to Scene | Choice, Bool, Button | off | Crop tab: keep only the splats inside a box or sphere (drawn as a wireframe in the viewport) |
| Min Opacity / Max Size | Double | 0 / 0 | Crop tab: drop faint or oversized splats (0 = off) |

The crop and cleanup settings change what every downstream node sees, not only the preview.

**Point preview:** the 3D viewport and ScanlineRender draw one solid round point per splat in its
base colour. This is for placement, not for final pictures. The viewport buffer is uploaded once
and reused (about 5 ms per repaint for a 1.2M-splat scan).

**Memory:** a 10M-splat scan is about 2.3 GB at SH Degree 3 and 0.5 GB at SH Degree 0, and about the
same again on the graphics card once SplatRender has uploaded it.

### SplatRender
**Group:** 3D | **Inputs:** 2 (Scene, Camera) | **Output:** image (RGBA + AOVs)
**Build:** gated `NATRON_FASTVOLUME` (needs wgpu-native, see `BUILDING.md`).

Renders the splat picture on the GPU: each splat a soft ellipse, blended far to near, with the
view-dependent colour stored in the scan. **Input 0 = Scene**: one to eight `ReadSplats`, directly
or under a `Scene3D` / `Group3D` (the group's transform is applied). **Input 1 = Camera**
(optional; without one the scene is framed from the front). The camera maths are ScanlineRender's,
so a SplatRender and a ScanlineRender of the same camera line up.

| Knob | Type | Default | Description |
|------|------|---------|-------------|
| Splat Scale | Double | 1.0 | Size of every splat; lower values show the individual splats |
| Opacity | Double | 1.0 | Overall opacity multiplier |
| SH Degree | Choice | 3 | View-dependent colour used for the render (0 = none) |
| Antialiasing | Bool | on | The standard 0.3 px low-pass on each splat |
| Output Size | Choice | Project Format | Or Custom with Width and Height |
| Samples / Shutter / Shutter Offset / Custom Offset / Temporal Jitter | - | 1 / 0.5 / Centered / 0 / 0 | Motion Blur tab, same names and behaviour as ScanlineRender. Follows the camera and the scans' transforms. Samples 1 = off |
| Depth of Field / Samples | Bool, Int | off / 32 | Depth of Field tab: a true depth of field, the frame drawn Samples times across the lens opening |
| Focus / Focus Distance | Choice, Double | Camera / 5.0 | Focus from the camera or set on the node |
| F-Stop From Camera / F-Stop | Bool, Double | on / 2.8 | Lens opening |
| Aperture Scale | Double | 1.0 | A scan has no real unit; 1 treats one scene unit as a metre, 100 as a centimetre |
| Blades / Blade Curvature / Rotation / Anamorphic Squeeze | - | 0 / 0 / 0 / 1 | Iris shape, same names as LensDefocus |

**AOVs tab** (plane names match ScanlineRender's where it has the same layer):

| Plane | Holds |
|-------|-------|
| `Depth` (on by default) | Mean distance along the view axis, weighted by visibility; follows motion blur and depth of field. What LensDefocus reads |
| `NearestDepth` | Distance of the nearest splat at least half opaque: the first solid surface |
| `world_position` | World position, from Depth and the camera |
| `Pref` | Position in the scan's own space, before the ReadSplats transform |
| `Normal` | Approximate: each splat's thinnest axis, turned towards the camera |
| `Velocity` | Pixels per frame, ScanlineRender's convention, from camera and scan movement |
| `SceneMatte` | Share of the pixel's alpha from the first, second and third ReadSplats |

All planes except Depth are the sharp frame at the frame's own time.

**Depth of field, two routes.** The node's own Depth of Field tab is exact through semi-transparent
splats and thin edges and costs one draw per sample. Alternatively connect SplatRender to the Source
of a `LensDefocus` and the same camera to its Camera input: one draw, plus LensDefocus' lens
character (cat's eye, swirl, aberrations, real lenses). The two match within a fraction of a pixel
when Aperture Scale 1 is paired with LensDefocus Scene Units = Metres.

**Performance (RTX 2080, 1920x1080):** about 56 ms per frame for a 1.16M-splat scan; 115 to 175 ms
for 10M splats. Motion blur and depth of field cost one full draw per sample. Rendering slows
sharply when the graphics card's memory is full; check that first when it is slow.
`NATRON_SPLAT_TIMING=1` prints per-stage timings.

**Limits:** perspective cameras only; at most eight ReadSplats per node; with several scans,
sRGB to Linear is taken from the first; Normal is an estimate; Depth is an average at soft edges, so
a depth merge against geometry is approximate there. Verified on NVIDIA under Windows; it compiles
and starts on Linux but has not been rendered there.

**Writing multi-layer EXRs:** with a Write node set to All Planes in its default Parts mode, the
colour is not the first part in the file, so some readers show Depth instead of the picture. Set
the Write node's Parts to Single Part. This is a Write-node behaviour and applies to ScanlineRender
AOVs as well.
