# Universal ReadGeo — session handoff state (2026-10-25)

Carry-over document for the ReadGeo uber-node effort. Read this before touching
`ReadGeo.*`, the consumers, or the batteries.

## Objective
One format-agnostic geometry reader node (ReadGeo): "Output Single Mesh" knob
(default ON) switches between classic single-mesh emission (MeshProvider) and
object-hierarchy multi-emit (ArchiveProvider). Legacy nodes
(ReadGeoArchive / ReadAlembicArchive) stay registered untouched.

Locked decisions:
- ODR policy: NO shared emission helpers across the unified and the archive
  nodes. ReadGeo.cpp carries its own distinctly-named copies
  (RGEntryMaterialAdapter, rgGlobMatch, obj helpers, rgAbc* engine). No new
  .cpp files for this work; consumers only hold MaterialProvider*/ArchiveProvider*.
- Bake Transforms knob applies in both modes, but means different passes:
  single = classic build bake (rt.bakeMeshWorld is NOT set in single —
  buildSceneMeshCore bakes itself), multi = the archive bake post-pass.
- abc multi-emit: no file materials (entry provider = nullptr), no bake pass,
  no Time-mode knob (effective timeMode 0 / native frame mapping).

## Commits (branch RB-2.6-niikl, agents commit, user pushes; no embedded
double quotes in messages)
- `ea3b2a3c1` geoio: fast OBJ parse — zero-allocation buffer scan (viewport scrub speed)
- `d27aa6538` ReadGeo: dual-mode uber node, phase 1 compatible with glb/gltf/fbx
- `afa0cd19f` ReadGeo: OBJ multi-emit (phase 2)

Phase 3 (abc multi-emit) is IN PROGRESS, UNCOMMITTED in ReadGeo.cpp/ReadGeo.h.

## Build / verify loop (from workspace root)
```
$env:MSYSTEM='MINGW64'; $env:LLVM_INSTALL_DIR='C:/msys64/mingw64'
& C:\msys64\usr\bin\bash.exe -lc "cd /c/Users/filmc/Documents/OpenCode_Natron3D/Natron/build-qt6 && mingw32-make -j8"
cmd /c "build.cmd 06-install"
cmd /c '"...\Natron-install\Renderer\NatronRenderer.exe" "<test>.py" > "%TEMP%\out.txt" 2>&1'
```
New source files ONLY → `cmake .. -G "MinGW Makefiles"` first (regenerate globs).
NOTE: if a build appears to change nothing after an edit, force `touch` the
.cpp (mtime-based staleness was observed) and confirm a "Building CXX object"
line appears. Image checks: qtpy QImage sampling grid, IDENT_MAX=0.006.

## Phase 1+2 — DONE, all batteries green
Batteries (Natron/tools/geoio-tests/):
- readgeo_universal_test.py — unified vs legacy pixel parity per file; live
  mode flip; fbx# seq both modes; abc-multi render (no crash).
- readgeo_obj_multi_test.py — obj multi vs merged single; two-group synthetic
  obj (twoboxes_uni.obj, authored inline); Object Filter multi; obj# seq
  multi animates + clamps; multi==single per frame.
- readgeo_gatherseq_test.py / readgeo_lru_sweep.py — legacy regressions, green.

KEY consumer lesson (the multi-emission black-render bug): SceneGraph's
MeshProvider branch `continue`s the NODE LOOP when getMeshData is empty —
there is NO structural fall-through. The dual node in multi mode must have
`meshProv` nulled BEFORE the branch:

```cpp
MeshProvider* meshProv = dynamic_cast<MeshProvider*>(effect.get());
if (meshProv) {
    if (ArchiveProvider* apDual = dynamic_cast<ArchiveProvider*>(effect.get())) {
        if (apDual->isMultiEmit()) meshProv = nullptr;  // fall to archive branch
    }
}
```
(SceneGraph.cpp mesh branch; fixed in `d27aa6538`.)
Other consumers: ScanlineRender::extractGeometry (dual+multi → return false)
and extractGeometries (archive branch gated `abcArchive->isMultiEmit()`
else falls to single path); CyclesRenderer.cpp eSceneNodeMesh fallback guard
(~2619) same nulling. Cycles renders multi via sn.meshData set by SceneGraph
emission + archiveEntryIdx only for motion blur (no change needed there).

Engine internals completed in ReadGeo.cpp (phase 1+2):
- Dispatch in loadGeoFromFile: multi → gltf/glb/fbx loadArchiveGeo, obj →
  loadObjArchiveGeo (parseObjArchiveMulti: one root-level mesh node per OBJ
  group, per-group VERTEX PRUNE with face+edge remap), abc → loadAbcArchiveGeo
  (phase 3, see below).
- finishArchiveGeo(path): shared tail (bake flag, subtree filter, info line,
  morph-knob sync) split out of loadArchiveGeo.
- ensureSequenceFrameArch: obj branch parses frames via parseObjArchiveMulti.
- Accessors (getSceneNodeCount/EntryTree/getSceneNodeAt/getMeshDataAt/
  getEntryWorldMatrix/getRootMatrix/getEntryMaterialProvider) act on
  _imp->visible/_imp->rt (runtime) — abc branch added in phase 3 (below).
- getMaterial* getters: multi → getConnectedMaterial(), else first
  rt.data.materials[0] if present, else defaults; getMaterialDiffuseColorspace
  multi → "sRGB".
- getPreferredMetadata multi: seq or rt.hasAnimation → frame-varying; plus
  abc entry scan (below); single mode untouched.

## Phase 3 — abc multi-emit: DONE (2026-10-25), all abc batteries green

ROOT CAUSE of the black renders: `loadAbcArchiveGeo` populates
`_imp->abcVisible` only — `_imp->visible` (the glTF/FBX/obj runtime list)
stays EMPTY for a plain .abc load, and every ArchiveProvider accessor guarded
on `idx >= _imp->visible.size()`, so all 5 entries returned
false/empty/nullptr to SceneGraph (count=5 correct, emission dropped every
entry). Yesterday's "trace never fires" was a red herring — the ENTER
fprintf is env-gated and `NATRON_DEBUG_GEOIO` was never set in the renderer.

Fix: the guards in getSceneNodeAt / getMeshDataAt / getEntryWorldMatrix now
bound on `abcVisible.size()` when `_imp->abcLoaded` (else `visible.size()`);
getEntryMaterialProvider already early-returned nullptr for abc.

Verification (readabc_unified_test.py): parity frames 2/5 bit-identical
(diff=0.00000, lit=8171/4139), animates, clamp low (0==1, lit>0), clamp high
(100==9 pixel match + differs from mid — tail frames are dark in the asset),
single-mode unified == legacy single. readabc_legacy_test.py DONE_OK. All
static sections of readgeo_universal_test.py + readgeo_obj_multi_test.py
PASS (glb/fbx/spider parity, twobox, filters).

ENVIRONMENT (2026-10-25): the realflow fixtures LIVE at
`C:\Users\filmc\Documents\OpenCode_realflow-mcp\spikes\liquid_out\out\{abc,fbx,obj}_seq`
(frames 2-50) — all five batteries DONE_OK (universal / obj multi /
gatherseq / lru sweep / the readabc pair).

Reference/baseline (green, pre-refactor):
- Asset: Natron/tools/geoio-tests/assets/animcubetree.abc — Blender-generated
  (roundtrip/blender_make_animabc.py; Blender 5.2 at
  "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"; alembic_export
  kwargs for that version: xsamples/gsamples/subdiv_schema/apply_subdiv/
  init_scene_frame_range — pack_uvs & facesets & scene_frame_range don't exist).
  Content: root AnimRoot (x-translation keyframes, 9 samples) → mesh
  CubeMeshT; static sibling subtree CubeMeshS (5 entries total).
- Battery: readabc_legacy_test.py → ReadAlembicArchive animates (2/5/8) +
  clamps (frame 0 == frame 1; frame 100 == frame 9) → DONE_OK.
  Sample mapping fact: sample k sits at frame k+1 (timeMode 0);
  getSceneNodeAt uses NEAREST sampling; getEntryWorldMatrix/vertex updates
  interpolate brackets for sub-frame motion blur.

Aborted extraction: GeoAbcArchiveEngine.{h,cpp} + ReadAlembicArchive rewiring
was attempted and REVERTED (git checkout). Lessons (do not repeat):
- `class MaterialProvider` is GLOBAL scope in MaterialProvider.h; a
  `class MaterialProvider;` forward inside namespace Natron creates a
  SHADOWING Natron::MaterialProvider and breaks every TU (invalid use of
  incomplete type). Don't forward-declare global-scope classes inside Natron::.
- Natron gcc refuses `<ImathMatrix.h>` bare — use `<Imath/ImathMatrix.h>`.
- Alembic archive open needs `#include <Alembic/AbcCoreFactory/All.h>`.

Current implementation (all in ReadGeo.cpp + tiny ReadGeo.h additions):
- ReadGeo.h: fwd decls (UNCONDITIONAL — GUI TUs lack NATRON_HAVE_ALEMBIC):
  `struct RgAbcEntry;` `struct RgAbcFrame;`. Declares loadAbcArchiveGeo,
  ensureAbcSequenceFrame, loadObjArchiveGeo, finishArchiveGeo.
- ReadGeo.cpp namespace scope (after first anon-exit ~line 383, #ifdef-gated):
  struct RgAbcEntry (fullPath/name/parentIndex/isMesh/sampleTimes/
  sampleMatrices(Imath::M44d)/meshData/hasAnimatedVerts/vertexSampleTimes/
  vertexSamples) and struct RgAbcFrame {std::vector<RgAbcEntry> entries;
  double sourceFps}.
- ReadGeo.cpp anon ns (~line 2020-2390): rgAbcReadPolyMeshSample0,
  rgAbcSampleIndexContinuous, rgAbcEntryLocalAt (NOTE: interpolates brackets
  even for xform locals in timeMode 0 — identical to legacy at integer
  frames), rgAbcMeshAt (in-place vertex mutation, documented G1 rule),
  rgAbcEntryWorldAt (chain), rgAbcWalkObject, rgAbcParse, and — from phase 2 —
  parseObjArchiveMulti + obj helpers in the SAME-or-earlier anon ns.
  LAYOUT GOTCHA: the obj-helpers anon ns closes right after
  parseObjArchiveMulti ("} // anon (obj helpers + obj archive parse)") — keep
  brace balance intact; the file previously broke every member definition
  into an anon scope when a close went missing.
- ReadGeoPrivate: abcEntries, abcVisible, abcSourceFps, abcLoaded,
  abcSeqRt (GeoSequenceLruCache<RgAbcFrame>{72} — T is the VALUE type, LRU
  hands back shared_ptr<T>).
- loadAbcArchiveGeo: seq reference resolution → rgAbcParse → subtree filter
  (includeMeshes + rgGlobMatch + excludedLines) → legacy-format info line
  ("Total entries: %d (xforms: X, meshes: M) | Visible after filter: V |
  Source FPS: F" + seq suffix) → sceneLoaded=true, abcLoaded=true.
- Dispatch: multi + ".abc" → loadAbcArchiveGeo (placeholder branch removed).
- Accessors branch on `_imp->abcLoaded` (each verified present):
  getSceneNodeCount → abcVisible.size(); getEntryTree → engine-scan;
  getSceneNodeAt → rgAbcEntryLocalAt + visible-ancestor/-folding walk;
  getMeshDataAt → rgAbcMeshAt (+ seq → ensureAbcSequenceFrame, frame file
  sampled at its own time 0); getEntryWorldMatrix → rgAbcEntryWorldAt;
  getRootMatrix → identity/false; getEntryMaterialProvider → nullptr.
- ensureAbcSequenceFrame: per-frame file → RgAbcFrame via rgAbcParse,
  count-equality hierarchy check, abcSeqRt LRU, hold-reference fallback.
- Clear paths (loadGeoFromFile prologue AND knobChanged mode-flip branch)
  now also clear abcEntries/abcVisible/abcSeqRt + abcLoaded=false.

## CLOSED — black-abc bug diagnosis trail (2026-10-24, resolved 10-25)
The "trace never fires" observation was misleading (the ENTER fprintf is
env-gated; whether NATRON_DEBUG_GEOIO was truly set in the renderer env is
uncertain — an unconditional count trace DID fire, ~count-per-second flood).
Real diagnosis: count fired, ENTER ran 10x per render (5 pivot-walk + 5
emission) — the defect was the false return path (see ROOT CAUSE at Phase 3).
Battery flaws found in diagnosis are FIXED: clamp checks require lit>0 (low)
and a clamp-vs-mid difference check (high, tail frames are dark in the
asset); dark parity frames are skipped as "trivial" and logged; the
readgeo_universal placeholder check flipped to "abc unified multi renders
content". The obj seq battery's clamp checks are lit-guarded so a missing
fixture fails loudly.
Next for phase 4: NODE_REGISTRY.md note + full format x mode matrix +
dead placeholder text removal (grep "hierarchy import is not implemented" —
currently nothing matches, already clean).

## Asset notes
- Assets dir (untracked): animcubetree.abc, two-box obj generation is inline
  in readgeo_obj_multi_test.py. Realflow seq fixtures at
  realflow-mcp/spikes/liquid_out/out/{abc,fbx,obj}_seq (frames 2-50).
- Do NOT commit binary assets; commit generators + batteries only.
