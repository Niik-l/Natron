# GeoMaterialOverride — per-part material assignment for Alembic archives

**Status:** Phase 1 (render pipeline) + Phase 2 (surface picker) **done & verified**;
uncommitted (commit prep in progress 2026-06-06).
**Date:** 2026-06-04, updated 2026-06-06.

**Update 2026-06-06:** Phase 2 picker built (`Gui/MaterialOverridePickerWidget`,
no Q_OBJECT, injected via `NodeSettingsPanel::initializeExtraGui`). Also landed:
matching is now exact / ancestor / **bare-leaf-name** (so tree names work);
**disabled node bypasses** the override (`isNodeDisabled()` skipped in the
pre-scan); the **CyclesRender scene hash** now includes the per-part override so
edits re-render; and a **"Refresh Passes"** button on CyclesRender. The
right-click "Split Geo from tree selection" idea was superseded by the picker.

## Problem

A `ReadAlembicArchive` emits many sub-objects (e.g. an industrial wall lamp =
12 `polySurface*` meshes), but a Material3D wired to the archive's `mat` input
applies **one** material to **all** of them (`ReadAlembicArchive.h:152` — *"every
archive mesh entry shares a single material"*). We need a *different* material on
specific sub-objects (glass dome vs metal cage vs bulb).

Name/glob matching is useless here — every mesh is `polySurfaceShapeN`. So the
selection must be **visual** (pick the surfaces), and the assignment is keyed on
the **full archive path** (unique even when leaf names collide:
`industrial_wall_lamp/polySurface2/polySurfaceShape2`).

## Chosen approach: A — override-in-place by path

A new node, **`GeoMaterialOverride`** (label "Material Override"), sits in the geo
chain: `ReadAlembicArchive → GeoMaterialOverride → Scene → CyclesRender`.

- **Input 0 ("Geo")** — the upstream geo (the archive, or another override).
- **Input 1 ("Mat")** — a Material3D.
- **Knob `surfaces`** — newline-separated list of full archive paths this override
  applies to. (Phase 2: a right-click "Split Geo / Assign Material" action in the
  Alembic tree populates this from the ctrl-click selection.)

It creates **no new geometry** — it tells the renderer "for these paths, use *my*
`Mat` instead of the archive's." Chain several for several materials; delete one
and those surfaces cleanly revert to the archive's base material. No exclusion
bookkeeping, no double-geo (the rejected approach B re-emitted geo and had to keep
the archive's `excludedPaths` in sync — fragile).

### Material precedence (highest first)
1. **Downstream / per-pass `materialOverride`** (clay / shadow-pass) — still wins.
2. **Per-part `GeoMaterialOverride`** (this feature).
3. **Archive / source node's own `mat` input** — the base material.

Matches the user's "assignment lives on the archive, unless there's an override at
the end."

## How it threads through the pipeline

1. **`SceneNode` gains `NodeWPtr materialNode`** (`SceneGraph.h`) — the per-part
   material for this scene node; null = none.
2. **`SceneGraph::rebuild`** (`SceneGraph.cpp`) pre-scans `allNodes` for
   `GeoMaterialOverride` nodes, resolving each to *(target archive, path-set,
   material node)*. During the archive's per-entry emission, an entry whose
   `fullPath` is listed gets `sn.materialNode` set. (Runs for both the Cycles path
   and the 3D viewport, since both call `rebuild`.)
3. **`collectSceneNodes`** (`CyclesPassRender.cpp`) recurses through a
   `GeoMaterialOverride` (input 0) so the archive *behind* it is still collected;
   the override node itself is also pushed so `rebuild`'s pre-scan sees it.
4. **`CyclesRenderer` per-node shader pick** (`CyclesRenderer.cpp:~2039`) applies
   the precedence above using `sn.materialNode`.

The override node is **not** a `MaterialProvider`; `rebuild` reads its `Mat` input
(input 1) directly and stores that material node on the matched `SceneNode`. The
renderer resolves it exactly like any other source material via
`createMaterialShader` (which redirects through `getConnectedMaterial`).

## Files changed (Phase 1)

| File | Change |
|------|--------|
| `Engine/EffectInstance.h` | + `PLUGINID_NATRON_GEOMATERIALOVERRIDE` define |
| `Engine/Dev/Scene3D/GeoMaterialOverride.h` | **new** — node class |
| `Engine/Dev/Scene3D/GeoMaterialOverride.cpp` | **new** — node impl |
| `Engine/AppManager.cpp` | + include + `registerBuiltInPlugin<GeoMaterialOverride>` |
| `Engine/Dev/Scene3D/SceneGraph.h` | + `NodeWPtr materialNode` on `SceneNode` |
| `Engine/Dev/Scene3D/SceneGraph.cpp` | override pre-scan + per-entry tagging in `rebuild` |
| `Engine/Dev/Cycles/CyclesPassRender.cpp` | `collectSceneNodes` recurses through the override |
| `Engine/Dev/Cycles/CyclesRenderer.cpp` | shader-pick precedence (`sn.materialNode`) |

New files → run `cmake .. -G "MinGW Makefiles"` once before `mingw32-make`.

## Revert plan

Phase 1 is self-contained and additive — nothing existing is rewritten, only added
to. To fully back it out:

1. **Delete** `Engine/Dev/Scene3D/GeoMaterialOverride.{h,cpp}` and this doc.
2. **`EffectInstance.h`** — remove the `PLUGINID_NATRON_GEOMATERIALOVERRIDE` line.
3. **`AppManager.cpp`** — remove the `#include "Dev/Scene3D/GeoMaterialOverride.h"`
   and the `registerBuiltInPlugin<GeoMaterialOverride>(...)` line.
4. **`SceneGraph.h`** — remove `NodeWPtr materialNode;` from `SceneNode`.
5. **`SceneGraph.cpp`** — remove the `GeoMaterialOverride` include, the
   `MatOverrideRule` pre-scan block, and the per-entry `sn.materialNode` tagging in
   the archive-emission loop.
6. **`CyclesPassRender.cpp`** — remove the `GeoMaterialOverride` include and the
   recursion branch in `collectSceneNodes`.
7. **`CyclesRenderer.cpp`** — revert the shader-pick block to the original
   `if (materialOverride) … else if (srcNode) …` (drop the `matOverrideNode`
   branch).
8. `cmake .. -G "MinGW Makefiles"` + rebuild.

Because the only data-model change is one nulled-by-default `NodeWPtr` on
`SceneNode`, existing scenes without a `GeoMaterialOverride` behave identically
whether or not this feature is present (the pre-scan finds no overrides → no
tagging → original shader pick).

## Phase 2 (not started)

- Right-click "Split Geo / Assign Material" in the `AlembicTreeWidget`: ctrl-click
  surfaces → create a `GeoMaterialOverride` pre-filled with their paths, wired to
  the archive.
- A material dropdown / Material3D auto-create convenience.
- Overlapping-path precedence is currently "last matching override wins" in
  traversal order — refine to "closest-to-scene wins" if it matters in practice.
