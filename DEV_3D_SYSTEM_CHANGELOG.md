# Natron 3D System — Initial Port Snapshot

> **Status: snapshot of the initial 3D-port milestone (≈ 2026-03-30).**
>
> This doc captures the *original* port — adding a 3D viewport, Cycles renderer,
> material system, Alembic I/O, lighting, deep compositing, and the first two
> particle nodes. It is intentionally not kept live-updated.
>
> For the **current** state of the fork, see:
> - [`NODE_REGISTRY.md`](NODE_REGISTRY.md) — full list of registered + unregistered nodes
> - [`GIT_WORKFLOW.md`](GIT_WORKFLOW.md) — branch / commit conventions
> - `git log --oneline RB-2.6` — full chronological history

## Overview

This document covers the changes that added a 3D viewport, Cycles renderer,
material system, Alembic I/O, lighting, deep compositing, and the first
particle nodes to Natron. All on the `RB-2.6` branch.

---

## New Node Types Registered

All registered in `Engine/AppManager.cpp` via `registerBuiltInPlugin<>()`:

### 3D Geometry (6 nodes)
| Node | Plugin ID | Inputs | Description |
|------|-----------|--------|-------------|
| **Sphere3D** | `fr.inria.built-in.Sphere3D` | 2 (img, material) | Tessellated sphere with equirectangular UVs |
| **Card3D** | `fr.inria.built-in.Card3D` | 2 (img, material) | Flat textured quad, aspect from image |
| **Cube3D** | `fr.inria.built-in.Cube3D` | 2 (img, material) | 24-vertex cube with per-face UVs |
| **Cylinder3D** | `fr.inria.built-in.Cylinder3D` | 2 (img, material) | Tessellated cylinder with caps |
| **ReadGeo** | `fr.inria.built-in.ReadGeo` | 1 (material) | Alembic .abc mesh loader with UVs |
| **Group3D** | `fr.inria.built-in.Group3D` | 8 (3D objects) | Groups 3D objects with unified transform |

### 3D Scene & Render (5 nodes)
| Node | Plugin ID | Inputs | Description |
|------|-----------|--------|-------------|
| **Scene3D** | `fr.inria.built-in.Scene3D` | 8 (3D objects) | Aggregates 3D objects for rendering |
| **RenderPass** | `fr.inria.built-in.RenderPass` | 1 (scene) | Multi-pass filter: object visibility, holdout, shadow catcher, light selection |
| **ScanlineRender** | `fr.inria.built-in.ScanlineRender` | 3 (bg, obj/scn, cam) | CPU rasterizer, outputs 2D |
| **CyclesRender** | `fr.inria.built-in.CyclesRender` | 3 (bg, obj/scn, cam) | Cycles path tracer, outputs 2D |
| **Project3D** | `fr.inria.built-in.Project3D` | 4 (img, projCam, geo, renderCam) | Camera projection onto geometry |

### Camera & Lighting (3 nodes)
| Node | Plugin ID | Inputs | Description |
|------|-----------|--------|-------------|
| **Camera3D** | `fr.inria.built-in.Camera3D` | 0 | Camera with T/R, focal length, aperture |
| **ReadAlembicCamera** | `fr.inria.built-in.ReadAlembicCamera` | 0 | Import animated camera from .abc |
| **Light3D** | `fr.inria.built-in.Light3D` | 0 | Point/Spot/Area/Distant/Dome light |

### Materials (2 nodes)
| Node | Plugin ID | Inputs | Description |
|------|-----------|--------|-------------|
| **Material3D** | `fr.inria.built-in.Material3D` | 5 (diffuse, metallic, roughness, emission, normal) | Standalone PBR material with texture map inputs |
| **Volume3D** | `fr.inria.built-in.Volume3D` | 0 | Procedural volume (sphere, noise, box) |

### Deep Compositing (17 nodes)
DeepRead, DeepWrite, DeepFlatten, DeepMerge, DeepSlice, DeepColorCorrect, DeepGrade, DeepTransform, DeepCrop, DeepDefocus, DeepExpression, DeepHoldout, DeepRecolor, DeepFromImage, DeepToPoints, DeepImage

### Particles (2 nodes)
ParticleEmitter, ParticleGravity

### Channel (1 node)
DevShuffle

---

## File Inventory

### Modified Existing Files

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Qt6 compat, optional NATRON_CYCLES flag |
| `App/CMakeLists.txt` | Link flags |
| `Engine/CMakeLists.txt` | Dev/ source globs, Alembic/Cycles deps, NATRON_HAVE_ALEMBIC, NATRON_CYCLES defines |
| `Engine/AppManager.cpp` | `registerBuiltInPlugin<>()` for all new nodes |
| `Engine/EffectInstance.h` | Extended interface |
| `Gui/CMakeLists.txt` | ImGui/ImGuizmo sources, NATRON_CYCLES define |
| `Gui/Viewport3D.cpp` | Replaced internals with DevViewport3D |
| `Gui/Viewport3D.h` | Replaced internals with DevViewport3D |
| `Gui/Viewport3DTab.cpp` | Updated to use DevViewport3D, timeline integration |
| `Gui/Viewport3DTab.h` | Updated member types |

### New Files — Engine/Dev/Scene3D/ (34 files)

```
Camera3DNode.h / Camera3DNode.cpp
CameraProvider.h
Card3D.h / Card3D.cpp
Cube3D.h / Cube3D.cpp
Cylinder3D.h / Cylinder3D.cpp
Group3D.h / Group3D.cpp
Light3D.h / Light3D.cpp
Material3D.h / Material3D.cpp
MaterialProvider.h
Project3D.h / Project3D.cpp
ReadAlembicCamera.h / ReadAlembicCamera.cpp
ReadGeo.h / ReadGeo.cpp
ReadVDB.h / ReadVDB.cpp
ScanlineRender.h / ScanlineRender.cpp
Scene3D.h / Scene3D.cpp
SceneGraph.h / SceneGraph.cpp
Sphere3D.h / Sphere3D.cpp
Volume3D.h / Volume3D.cpp
```

### New Files — Engine/Dev/Cycles/ (4 files)

```
CyclesRender.h / CyclesRender.cpp      # Natron node wrapper
CyclesRenderer.h / CyclesRenderer.cpp  # Cycles API bridge
```

### New Files — Engine/Dev/Deep/ (~30 files)

```
DeepImage.h/cpp, DeepRead.h/cpp, DeepWrite.h/cpp, DeepFlatten.h/cpp,
DeepMerge.h/cpp, DeepSlice.h/cpp, DeepColorCorrect.h/cpp, DeepGrade.h/cpp,
DeepTransform.h/cpp, DeepCrop.h/cpp, DeepDefocus.h/cpp, DeepExpression.h/cpp,
DeepHoldout.h/cpp, DeepRecolor.h/cpp, DeepFromImage.h/cpp, DeepToPoints.h/cpp,
PointCloudData.h, DeepUtils.h/cpp
```

### New Files — Engine/Dev/Particles/ (4 files)

```
ParticleEmitter.h / ParticleEmitter.cpp
ParticleGravity.h / ParticleGravity.cpp
ParticleData.h
```

### New Files — Gui/ (6 files)

```
DevViewport3D.h / DevViewport3D.cpp    # ImGuizmo-based 3D viewport
Viewport3DTab.h / Viewport3DTab.cpp    # Panel wrapper (toolbar + viewport + timeline)
ShuffleWidget.h / ShuffleWidget.cpp    # DevShuffle UI
KnobGuiShuffle.h / KnobGuiShuffle.cpp # Shuffle knob GUI
```

### New Files — Gui/ImGuizmo/ (MIT licensed, ~10 files)

```
imgui.h / imgui.cpp
imgui_draw.cpp
imgui_widgets.cpp
imgui_tables.cpp
imgui_internal.h
ImGuizmo.h / ImGuizmo.cpp
imconfig.h
imstb_rectpack.h, imstb_textedit.h, imstb_truetype.h
```

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

### Undo
- Natron has TWO undo stacks: per-node and global (NodeGraph)
- Gizmo transforms push `GizmoTransformUndoCommand` to NodeGraph's global stack for Ctrl+Z

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

## Testing Checklist

### Basic 3D Viewport
- [ ] Open 3D Viewport tab
- [ ] Grid and axes visible
- [ ] Orbit (middle-mouse), pan (shift+middle), zoom (scroll)
- [ ] Create Sphere3D — appears in viewport
- [ ] Select sphere — ImGuizmo handles appear
- [ ] W/E/R — translate/rotate/scale gizmos
- [ ] F — frame to selected object
- [ ] Ctrl+Z — undo gizmo transforms

### Alembic Import
- [ ] Create ReadGeo → set .abc file path
- [ ] Object dropdown populates with geometry objects
- [ ] Mesh appears in 3D viewport as wireframe
- [ ] T/R/S gizmo works on imported mesh
- [ ] Info field shows vertex/face count

### Materials
- [ ] Material3D node → connect to ReadGeo's Material input
- [ ] Set Base Color → renders with color in Cycles
- [ ] Set Diffuse Map texture → renders with texture
- [ ] Set Normal Map → surface detail visible
- [ ] Set Roughness Map → per-pixel roughness variation
- [ ] Set Metallic Map → metal/dielectric variation

### Lighting
- [ ] Light3D node → Point type → illuminates scene
- [ ] Switch to Spot → cone visible in viewport
- [ ] Adjust Spot Angle/Smooth → cone updates
- [ ] Switch to Area → rectangle visible
- [ ] Adjust Area Width/Height → rectangle updates
- [ ] Dome + HDRI file → environment lighting

### Cycles Rendering
- [ ] Camera3D + Scene3D + CyclesRender → Viewer shows rendered image
- [ ] Adjust Samples → quality changes
- [ ] Adjust Max Bounces → GI changes
- [ ] Cache works (same frame doesn't re-render)
- [ ] Animation: keyframed objects update per frame

---

## Known Issues / TODO

1. **Material3D 2D input baking** — experimental; renders connected 2D nodes to temp .hdr files via `renderRoI()`. Works but needs more testing with complex node chains.
2. **Deep EXR** — Deep nodes exist but Cycles deep output is WIP upstream.
3. **ReadVDB** — Requires OpenVDB library (optional dependency).
4. **OFX plugins** — When running from build directory, set `OFX_PLUGIN_PATH` to load Read/Write nodes.
5. **Viewport3DTab signal warning** — `frameChanged(SequenceTime,int)` vs `onFrameChanged(double)` mismatch (cosmetic, doesn't affect functionality).

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
