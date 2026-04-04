# Natron Node Registry

All custom built-in nodes added to Natron beyond the original upstream codebase.
Updated: 2026-04-03

---

## 3D Geometry (7 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Sphere3D** | `fr.inria.built-in.Sphere3D` | Registered | Tessellated sphere with equirectangular UVs |
| **Card3D** | `fr.inria.built-in.Card3D` | Registered | Flat textured quad |
| **Cube3D** | `fr.inria.built-in.Cube3D` | Registered | 24-vertex cube with per-face UVs |
| **Cylinder3D** | `fr.inria.built-in.Cylinder3D` | Registered | Tessellated cylinder with caps |
| **ReadGeo** | `fr.inria.built-in.ReadGeo` | Registered | Alembic .abc mesh loader (requires NATRON_HAVE_ALEMBIC) |
| **ReadVDB** | `fr.inria.built-in.ReadVDB` | Registered | OpenVDB volume loader (requires OpenVDB) |
| **Group3D** | `fr.inria.built-in.Group3D` | Registered | Groups 3D objects with unified transform |

## 3D Scene & Render (5 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Scene3D** | `fr.inria.built-in.Scene3D` | Registered | Aggregates 3D objects for rendering |
| **RenderPass** | `fr.inria.built-in.RenderPass` | Registered | Multi-pass filter: object visibility, holdout, shadow catcher, light selection |
| **ScanlineRender** | `fr.inria.built-in.ScanlineRender` | Registered | CPU rasterizer, outputs 2D |
| **CyclesRender** | `fr.inria.built-in.CyclesRender` | Registered | Cycles path tracer (requires NATRON_CYCLES) |
| **Project3D** | `fr.inria.built-in.Project3D` | Registered | Camera projection onto geometry |

## Camera & Lighting (3 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Camera3D** | `fr.inria.built-in.Camera3D` | Registered | Camera with T/R, focal length, aperture, DOF |
| **ReadAlembicCamera** | `fr.inria.built-in.ReadAlembicCamera` | Registered | Import animated camera from .abc |
| **Light3D** | `fr.inria.built-in.Light3D` | Registered | Point/Spot/Area/Distant/Dome light with HDRI |

## Materials (2 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **Material3D** | `fr.inria.built-in.Material3D` | Registered | Standalone PBR material with texture map inputs |
| **Volume3D** | `fr.inria.built-in.Volume3D` | Registered | Procedural volume (sphere, noise, box) |

## Deep Compositing — Tier 1+2 (16 nodes, registered)

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
| **DeepToPoints** | `fr.inria.built-in.DeepToPoints` | Registered | Convert deep to point cloud |
| **DeepTransform** | `fr.inria.built-in.DeepTransform` | Registered | Transform deep images |
| **DeepExpression** | `fr.inria.built-in.DeepExpression` | Registered | Expression-based deep processing |
| **DeepColorCorrect** | `fr.inria.built-in.DeepColorCorrect` | Registered | Color correct deep samples |
| **DeepDefocus** | `fr.inria.built-in.DeepDefocus` | Registered | Defocus deep images |

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

## Particles (2 nodes)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **ParticleEmitter** | `fr.inria.built-in.ParticleEmitter` | Registered | Emitter with Point/Sphere/Box/Disc shapes |
| **ParticleGravity** | `fr.inria.built-in.ParticleGravity` | Registered | Gravity force for particles |

## Channel (1 node)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **DevShuffle** | `fr.inria.built-in.DevShuffle` | Registered | Channel shuffle node |

## Transform (1 node)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **SphericalTransform** | `fr.inria.built-in.SphericalTransform` | Registered | 8 projection types (equirect, cubemap, fisheye, etc.), rotation, interpolation |

## Color (1 node)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **ColorChartMatch** | `fr.inria.built-in.ColorChartMatch` | Registered | Color chart matching with matrix export (ACEScg default) |

## Other (1 node)

| Node | Plugin ID | Status | Description |
|------|-----------|--------|-------------|
| **CameraTracker** | `fr.inria.built-in.CameraTracker` | Disabled (WIP) | 3D camera solve from 2D tracks (uses libmv, excluded from build — GCC 15 Eigen issues) |

---

## Summary

| Category | Registered | Not Registered | Total |
|----------|-----------|----------------|-------|
| 3D Geometry | 7 | 0 | 7 |
| 3D Scene & Render | 5 | 0 | 5 |
| Camera & Lighting | 3 | 0 | 3 |
| Materials | 2 | 0 | 2 |
| Deep (Tier 1+2) | 16 | 0 | 16 |
| Deep (Tier 3) | 0 | 19 | 19 |
| Particles | 2 | 0 | 2 |
| Channel | 1 | 0 | 1 |
| Transform | 1 | 0 | 1 |
| Color | 1 | 0 | 1 |
| Other | 1 | 0 | 1 |
| **Total** | **39** | **19** | **58** |
