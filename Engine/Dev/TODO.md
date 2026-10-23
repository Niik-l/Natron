# Dev Nodes — Known Issues & TODO

- [x] **Mixamo FBX: Cycles material shading wrong around the face** — RESOLVED 2026-10-22 (late). With a FULL rebuild + fresh `build.cmd 06-install` staging, the writer path renders the scene CORRECTLY: hair-only (`%TEMP%\opencode\arc_haironly.py`, `objectFilter='*Hair*'` + reverseNormals) = ginger-brown textured with warm two-point-light shading, and full model (`arc_single.py`) at 8 spp (`arc_f249.png`) AND 256 spp (`arc_f249_256spp.png`). Earlier same-night runs that looked black-glossy were a STAGING ARTIFACT, not an engine bug: `build-qt6\Renderer\NatronRenderer.exe` cannot run standalone (0xC0000135 — DLLs exist only in the 06-install stage), so the partially-restaged `Natron-install\Renderer` carried a stale pre-fix Renderer binary and the A/B comparisons mixed two engines. LESSON (agent workflow): after touching any Engine source, run `mingw32-make -C Natron/build-qt6 -j2 && cmd /c build.cmd 06-install` BEFORE headless writer-path A/B renders (earlier in the TODO string: UVs verify correct END TO END and the texturing was there all along: (a) ground-truth standalone ufbx dump (probes in `%TEMP%\opencode\ufbx_dump.c`) shows per-part UVs clean (per-corner, full 0..1 range, no degenerate runs); (b) merged ReadGeo [GEOIO] dump matches it exactly (first uv values identical, uvslots=72978 = per-part corner sum: Shoes 13778, Tops 9244, Bottoms 9626, Hair 9334, Body 27662, Eyelashes 214, Eyes 3120); (c) `[Cycles GEO]` debug (NATRON_DEBUG_SC=1) shows `uv == tri*3` exact for every archive entry; (d) fresh headless writer renders show BOTH paths TEXTURED — archive (`tools/geoio-tests/out/mixamo_face/wr_sc.png`) and Re-built group (`rr_face.png`/`face2.png`: brows, eyelids, nose, black nails, white shirt); head materials NOT corrupt: "shattered hair" is the asset's actual plate-shell hairstyle design — ufbx skin-eval truth (probes `ufbx_eval.c`): Hair is single-bone (head), all weights 1.0, eval == rest exactly (max deviation 0)). VERIFYING evidence for the resolution: (a) `[CYCMAT]` stderr (env-gated in `CyclesRenderer.cpp`) prints all 7 bindMaterial entries with correct per-part texture paths (e.g. `Remy_Hair_Diffuse_04a6398c66d62b04.png`) + base/rough/metal — material binding verified end-to-end; (b) `[Cycles LIGHTS]` prints light object tfm from the SceneGraph world matrix (components at indices [12][13][14], NOT [3][7][11] — the initial debug print had wrong indices and suggested a false lights-at-origin scare); (c) standalone Cycles referee `%TEMP%\opencode\cycprobe.exe` (link via `build_cycprobe.sh`: g++ against libcycles_*.a + NatronEngine CXX defines/includes rsp wrapped in `--start-group/--end-group` for archive scan order) renders the engine's own `dump_hair_t249.obj` BRIGHT with the same two lights — parity with Blender. Debug env gates added (all default-off): `NATRON_CYCLES_NOTEX` (skip base texture), `NATRON_CYCLES_NOSHADOW` (lights cast_shadow=false), `NATRON_CYCLES_TRANSPBG0` (force background opaque), `NATRON_CYCLES_NOTREE` (skip light tree). Remaining sub-items: (e) residual fine speckle at 256 spp still noticeably noisy (fine orange fireflies over the whole head vs the EEVEE referee) — suspect sampling/noise tuning (integrator already has Cycles' default sample_clamp_indirect=10), NOT material binding; (f) `rr_face`/`rr_full` rendered IDENTICAL from one script after a camera param change — the stale-camera-cache note below applies to the headless writer path too: CyclesRender does not re-render when knobs change after its first render — one camera per headless launch until fixed; (g) `objectPath` set headless did not scope a per-part ReadGeo (stays "All parts (merged)") — possibly needs '/Hair' full path or a reload trigger (probe when needed). Repro asset `tools/geoio-tests/assets/mixamo.fbx` (81 entries: 74 xform + 7 mesh).
- [x] **Mixamo FBX: skeleton/mesh scale mismatch (~10×/100×)** — FIXED 2026-10-19 (`c3d7b47d7`, user-verified "the scale matches"): ufbx emits skinned vertices in WORLD space (unit/axes normalization baked in) while entry geometry and every downstream transform compose in GEOMETRY space, so the ×100 cm normalization leaked into the skin path only; the FBX entry builder now remaps ufbx world-space skinned vertices to geometry space so skin output composes through the same chain as the rest pose. ORIGINAL NOTE (2026-10-19, user report): an animated FBX exported from Mixamo loads with the mesh and the skeleton disagreeing on scale (one of them ~10× or ~100× off), so the rig renders unassembled / giant / tiny. Sample asset: `tools/geoio-tests/assets/mixamo.fbx`.
- [x] **ReadGeoArchive: shading changes when the rig is toggled in the archive tree** — FIXED 2026-10-20 (headlessly verified, user GUI pass still welcome). `ReadGeoArchive::getEntryMaterialProvider` resolved the material for the incoming VISIBLE entry index directly in the FULL node list — as soon as the rig subtree was ticked off (74 skeleton transforms dropped out of `visible`), every mesh entry's visible index shifted and it shaded with whatever material sat at that position in the full list, falling through to the node-level material (reads as the bright default; in Cycles textures toggled on/off). Fix: remap the visible index to the stable FULL node index (`rt.data.nodes[visible[idx]]`, `visible[idx]` bounded) before the lookup AND key the per-entry material adapter cache by the full index so shading survives any visibility change. Other consumers already sat in this index space (`getSceneNodeAt` / `getMeshDataAt` / `getEntryWorldMatrix` map internally; Viewport3D and CyclesRenderer pass `SceneNode::archiveEntryIdx` in visible space). Sample asset: `tools/geoio-tests/assets/mixamo.fbx` (81 entries: 74 xform + 7 mesh; rig root `/mixamorig:Hips`). Verify: `Natron/tools/geoio-tests/readgeo_rigshade_verify.py` — Cycles renders rig-visible vs rig-excluded are bit-identical (deterministic base render x2 checked; EXR capDate timestamp zeroed before SHA so identical pixels hash equal); mesh count intact under the filter, silhouette unchanged. ORIGINAL NOTE (2026-10-19, user report): unticking the rig subtree in the archive contents tree visibly changes the mesh look — much brighter in the 3D viewport; in Cycles textures toggle on/off.
- [x] **ReadGeoArchive: Re-build Materials button** — FIXED 2026-10-21 (headlessly verified; user GUI pass still welcome). `ReadGeoArchive` gained a `rebuildMaterials` button (File page, next to Reload): `rebuildMaterialsSet()` snapshots each visible mesh entry (FULL node walk order under `archiveMutex`), then builds a standalone node set beside the archive that **replicates its output 100%** — nothing connects back: a root `Group3D` named+labeled `Group3d_RebuiltMaterials_NN` (NN counts previous rebuilds in the project via a `getProject()->getNodes()` label scan, set through `kCreateNodeArgsPropNodeInitialName`) placed RIGHT of the generated chain (data reads left to right: Material3D @px+460 -> part ReadGeo @px+620 -> sub Group3D @px+780 -> root @px+940) carrying the archive's user TRS (rotationPivot not mirrored — single-part ReadGeo has no knob for it), one **ReadGeo** per part scoped via its Object dropdown rank (`objectPath` = 1-based mesh rank; label = part full path) + its **Material3D** (label `<part>_mat`, knob mirror via `setRebuildMaterialKnobs`) wired into the ReadGeo's Material input, sub-`Group3D`s chaining every 8 parts (`GROUP3D_MAX_INPUTS`) labeled with the root name, persistent message on completion. Mesh parts WITHOUT a file material still get a plain ReadGeo (no Material3D) so no geometry is dropped. FBX **embedded textures stage into a same-named `.fbm` side folder next to the source file** (DCC convention — `mixamo.fbx` -> `mixamo.fbm/`): `importWriteEmbeddedTexture` gains optional `outDir`/`outName` (TEMP fallback preserved; name sanitized + 16-hex content dedup), threaded through `resolveUfbxTextureArc`/`mapUfbxMaterialArc`/`loadFbxArchiveImport` (`ReadGeoArchiveFbx.cpp`); glTF mirrors this with a same-named **`.glm`** folder (`model.glb` -> `model.glm/` — `resolveCgltfImage` gains `sideDir`/`sideName`, `loadGltfArchiveImport` stages EVERY embedded image up front (GLB buffer views + data URIs, incl. textures no material references), external file URIs stay untouched). NOTE: `Node::setPosition/getPosition` are NodeGui-backed — headless probes read back (0,0); positions only materialize in the GUI. Two subtleties that made replication exact: (a) the Material3D's **texture colorspace** must be set explicitly — EntryMaterialProvider hardcodes `getMaterialDiffuseColorspace()="sRGB"`, but a fresh Material3D's default is the first sRGB-ish OCIO entry whose id (e.g. `rrt_srgb`) passes through `materialColorspaceToCycles()` verbatim, decoding the albedo texture differently (small-but-systematic rim/terminator shading errors, mean err 0.006 stable up to 4096spp — a REAL delta, not MC noise: alpha masks matched to sub-pixel AA, sampling stays deterministic per graph); `setRebuildMaterialKnobs` now selects the exact "sRGB" entry for both colorspaces. (b) `archiveFps()/frameOffset` have no ReadGeo counterpart — noted as non-replicable edge cases. Verify: `Natron/tools/geoio-tests/readgeo_rebuild_materials_verify.py` — archive rendered twice (determinism PASS) then the rebuilt set pixel-compared with idiff (`REBUI_PASS`, 1024spp; also PASS at 4096spp in `out/rebuild_noisetest`). TESTING LESSON: never `app.deleteNode(writer)` after `app.render()` mid-session — it poisons subsequent renders in the same session (zeroed/missed EXR outputs); keep writers alive and pixel-compare (EXR metadata differs across files, so byte/SHA equality is unattainable). Original spec (2026-10-19 user request, clarified 2026-10-20): for EACH material-bearing part TWO nodes — a **ReadGeo** scoped to that part via its Object dropdown + its **Material3D** wired into the ReadGeo's Material input (images: diffuse, normals, …,) — assembled hierarchically under **Group3D**. A per-part **Material Override is NOT part of the set**: the override can only fire with an ArchiveProvider upstream (SceneGraph consumes `matOverrides` solely in the archive emission branch, SceneGraph.cpp:613) so it would be inert in a per-part plain-ReadGeo chain — with the standalone set the Material3D binds through ReadGeo's own material input instead. Material Override FBX/glTF support was fixed separately 2026-10-20 (`e0b450477`: the GUI picker resolves any ArchiveProvider; the SceneGraph rule engine was already archive-agnostic — verified `OVERRIDE_FBX_PASS` headlessly on mixamo.fbx: rule fires on listed paths, no false match on unlisted). User decision 2026-10-19: assemble with **Group3D** (it already provides the SceneGraph parent-under-a-transformed-group machinery); the dedicated **TransformGeo and MergeGeo nodes are PARKED** — Group3D has the needed functions. Grounding: SceneGraph PASS 2 handles group parenting, Scene3D collects multi-input scenes, per-entry file materials (`ArchiveProvider::getEntryMaterialProvider`) supply the values the Material3D pair must carry across; ReadGeoArchive holds a single-part Object dropdown today, ReadGeo already has the per-part one.
- [ ] **Geo file sequences (FBX/GLB/GLTF/OBJ)** — TOP PRIORITY (2026-10-18, user decision; .abc explicitly EXCLUDED — that's ReadAlembicArchive's domain). Today all geo nodes read the file knob with plain `getValue()` — no per-frame pattern expansion, so `sim.####.fbx` style pointcache sequences don't work; all timeline motion comes from file-internal animation only. Design (ReadVDB template, with its 2026-08-21 lessons baked in): pattern-ness decided AT FILE-CHOICE time (does the pattern match >1 file on disk? — never unconditional substitution, that broke single-frame VDBs), resolved-path substitution per frame, reload keyed by the resolved path, and the per-frame file swap serialized on the node data mutex (`_meshMutex` / `archiveMutex`) like any other load. Frame Offset/FPS/Freeze semantics interplay needs a decision: do they remap the SEQUENCE index, or only the file-internal time? Applies to ReadGeo (merged + single) and ReadGeoArchive (per-entry load — decide: whole-archive swap per frame vs per-entry; whole-file swap is the simple correct v1).
- [ ] **USD read via tinyusdz** — SECOND (2026-10-18): .usd/.usda/.usdc into the existing archive contract on ReadGeoArchive (format-neutral node). Stage 1 = static hierarchy + meshes + materials (tinyusdz write-side also exists — note for the parked WriteGeo). Animation/skin later; composition arcs (references/instancing/variants) out of scope or last. OpenUSD proper is a no-go under MinGW for now. WriteGeo (OBJ/GLB) PARKED behind these two by user decision 2026-10-18.
- [ ] **Merge ReadGeo + ReadGeoArchive into one node** — LATER (2026-10-21, idea accepted, not prioritized). One `ReadGeo` implementing `EffectInstance + MaterialProvider + MeshProvider + ArchiveProvider` over the already-shared `GeoArchiveRuntime`, with an **Output: [Hierarchy | Flattened mesh]** knob: Hierarchy = today's archive behavior (entry tree, Frame Offset/FPS, locators, bbox, per-entry materials, node material knobs as override); Flattened = today's ReadGeo behavior (Object dropdown merged/single, Bake Transforms, Freeze at Frame, material pages). Feasibility notes: the flatten already exists (`ReadGeo::buildSceneMesh`); the REAL work is the consumer contract — `SceneGraph::rebuild`, `ScanlineRender::extractGeometry` and `CyclesRenderer` each `dynamic_cast` MeshProvider/ArchiveProvider in separate branches and would double-emit a node that is both; needs a shared precedence resolver (ArchiveProvider+flatten-off → multi-emit, else single mesh) plus mode-scoped knob pages, a unified material story (node knobs replace the Material3D-input override), and aliasing the old `ReadGeoArchive` plugin ID for project-load compat. Estimate 2–4 focused days including re-verification of both modes across viewport/Scanline/Cycles with the `tools/geoio-tests` batteries. Do after the current verified-fix batch lands.
- [ ] **Known limitation (confirmed 2026-10-18): merged/file meshes render single-material** — merged ReadGeo meshes and archive entries shade with one flat color in ScanlineRender; the file's per-part materials / baked face colors don't show. The 3D viewport (per-part tint) and Cycles (per-entry materials + face colors) consume them correctly; ScanlineRender's GLSL pipeline has no face-color / per-part-material input yet. Fix shape when picked up: plumb `MeshData::vertexColors` (per-corner, parallel to faceIndices) + the entry material's diffuse map through `kBeautyFrag` — the same data the Cycles `geo_face_color` attribute consumes. Left as a limitation by user decision (2026-10-18).
Tracking known bugs, incomplete features, and planned improvements.

---

## My TODO (user priorities)

Pruned 2026-10-13: finished items moved to **Done (2026)** at the end of this section with their dates. The long diagnostic notes of the fixed bugs live in git history (`git log -p -- Engine/Dev/TODO.md`).

### Waiting on a GUI test

- [ ] **TEST the FastVolumeRender particle light** (quick commit 2026-09-17, NOT GUI-tested; PARKED by user 2026-10-18 — unrelated to the geo work, revisit when touching particles+volumes): Particle Light knobs + Particles As Lights / Max Particle Lights / Intensity / Range; the light budget went 8 → 128 for every scene, so also re-check an ordinary lit VDB/Volume3D scene renders as before. Test rig: emitter → solver → Scene3D beside a Volume3D haze, camera in front; compare with a Light3D point light in the same fog.
- [x] **Verify the two Cycles-render crash fixes in the GUI** (template 3D scene load repeated) — DONE 2026-10-18 by user: loads fine, no crashes. Covers `d070af460` (render-mutex race destroying the active renderer mid-render) and `2cd63abc8` (G1 copy-on-read against the in-place mesh mutation — the recurring template-load AV in `syncSceneWithCamera`).
- [x] **Per-part color pass (merged meshes)** — DONE 2026-10-18 by user: merged-file colors are visible in the 3D viewport and Cycles. **ScanlineRender renders merged meshes single-material (one flat color) — known limitation**, documented below (face-color GLSL plumbing not built there; per-part materials also absent).
- [ ] **GUI-test the new Preferences > 3D Viewport page** (built 2026-09-21, headless-verified compile only): each nav preset on the free camera AND while looking through an editable Camera3D; box select / point pick / gizmo drag under every preset; right-click Blast menu appears on a plain click and never after a nav drag; background gradient + grid colour apply live to every open 3D viewport and survive a restart; two 3D viewports at once (ImGuizmo file-static caveat); unclamped orbit over the top; Blender preset on the Linux AppImage under a WM that grabs Alt+drag. Not done from the plan: zoom-towards-cursor, gizmo size, point size, per-axis grid colours.
- [ ] **Test the 3D viewport mesh cache in the GUI** (COMMITTED + PUSHED 2026-10-15: 17f221c60; GUI-probe verified 925K tris 86.8 -> 18.6 ms/paint, same frame; user's first look: "pretty good update"): a 1.5M+ scan in Shaded / Shaded+Wire / Flat / Wireframe while orbiting; a Megascans asset (diffuse texture on the fast path); an animated Alembic + a deforming ReadGeo scrubbed (version stamp re-upload); Reverse Normals toggled; a Project3D / UVProject / MergeMat scene (still the old path, must look identical); two 3D viewports open at once; closing/reopening a viewport pane (context recreation). Then: fast navigate while dragging (skip wire / bbox over a triangle budget), projection layers on the buffer path, ParticleInstance prototypes through the cache.
- [ ] **Finish the Scatter3D + ParticleInstance GUI test** (COMMITTED + PUSHED 2026-10-15: 1b9950ab4 + 10b40d98a; first GUI pass done 2026-10-14: flat Card3D → Scatter3D → ParticleInstance with Cube3D/Sphere3D prototypes works in the 3D viewport and ScanlineRender. Fixes since the first build, all in the current dev exe: a vertical surface at the default Slope Limit 90 no longer rejects every sample (cos(90) float edge), Size min/max clamped ≥0, 3D-viewport bare-particle points now sized from the real projection (world-space diameter, like SLR/Cycles/the instancer; 512 px cap), new Scatter3D **Display Size** knob = viewport-only point multiplier). Still to check: ReadGeo terrain (or a Card3D laid flat) → Scatter3D → ParticleInstance with a Megascans ReadGeo+Material3D on geo A (Apply Prototype Transform on, so the loader's cm→m scale holds), Random Rotation y=360, Random Scale 0.7–1.3; check the textures come through in CyclesRender, ScanlineRender and the 3D viewport; Align to Normal on a slope (and Up Axis Z on a Blender export); Slope Limit 30 on a hill; a painted density map on Scatter3D.density (needs UVs on the terrain); Min Distance vs Count in the Info line; an archive and a GeoBuilder card as prototypes; an old project with cube/sphere instances still looks the same (prototype transforms are now applied — a prototype sitting away from the origin shifts its instances).
- [ ] **Colour picker (2026-10-20)**: open the picker on a Grade, click HSV — the page must switch; type V = 2.5 or R = 3 and confirm the knob takes the value and the dialog reopens showing it (not clamped to 1); triangle / hex still show the clamped colour.
- [ ] **GUI check of the LensDefocus GPU / Cycles Device / L-layout merge (2026-10-24, headless-verified)**: RB-2.6 b84b6d484 + Cycles fork 3a20cc0b1 (`complex_tools/zdefocus/MERGE.md`). Press **L** with nothing selected on `probes/out/CyclesDeepLayers.ntp` (whole graph laid out, shared inputs clean); open `probes/out/GPU_demo.ntp`: Device knobs on CyclesRender (Settings), CyclesRenderSettings (Render), RenderPass (Preview), LensDefocus / DeepDefocus (Quality), the "Render Settings" info line on CyclesRender_Settings; LensDefocus Device = GPU renders the same as CPU (headless: 15 probe pairs within 7.5e-6); CyclesRender Device = GPU in this CPU-only Cycles build falls back to CPU with a warning banner (amber); Deep Max Samples default is now 8 (old projects load as 8). Headless done: testbed 37/37 CPU + 37/37 GPU-compare (worst mean 1.3e-4), cycles_deep_check 3e-5 at DEEP_MAX 64 and 2.
- [ ] **GUI pass of the deep LensDefocus work (merged 2026-10-23, headless-verified only)**: the LensDefocus stack (`complex_tools/zdefocus/HANDOFF.md`, outside the repo) was fast-forwarded into RB-2.6 at 62d278bbb with the Cycles fork at 0bb878c59; sections 1-8 were user-approved in the GUI, sections 9-10 were not: LensDefocus **Use Deep Data** on a DeepRead / CyclesRender deep source (`probes/out/LensDefocus_deep_reveal.ntp`: the orange card blurs and the scene shows through its rim; DeepDefocus gives the same image), CyclesRender **Deep Layers** 2 (`probes/out/CyclesDeepLayers.ntp`: the checker stays straight under the sphere's rim), Cycles deep edge samples red / blue not purple, and CyclesRender Preview Mode deep output aligned with the flat image. Post-merge headless checks (HANDOFF section 4 items 5 / 8 / 10, sections 8-10 probes) all passed on the merged build, pixel-identical to the branch build.
- [ ] **ParticleInstance in a Scene3D through ScanlineRender (2026-10-21)**: the user's Scatter3D → ParticleInstance (mesh prototype) → Scene3D (+ Light3D) → ScanlineRender graph must show the instances like Cycles does (headless-verified with a cube prototype; check a ReadGeo / Megascans prototype with its material texture, and motion blur samples > 1).
- [ ] **ScanlineRender concurrent renders (2026-10-21, open)**: two or three Write nodes on ScanlineRenders started together → one fails "Rendering Failed" at the multisample FBO (`glCheckFramebufferStatus` incomplete, ScanlineRender.cpp ~2835). Reproduce with the Progress panel; candidate fix = retry with samples=1 / non-MSAA FBO when incomplete.
- [ ] **Crop Reformat (2026-10-20)**: Crop with Extent Size + Reformat ticked → the viewer's format / a downstream Write must be the crop size, not the project format; also with Extent Default (input region). Needs the rebuilt Misc.ofx in the dev plugin folder (installed 2026-10-20).
- [ ] **Many-input arrow length (2026-10-20)**: Group3D / Material3D disconnected arrows now 1.5x instead of 2.5x — check the labels still read as two rows.
- [ ] **Test Transform3D + the Group3D axis input in the GUI** (built 2026-10-19, headless-verified in Cycles): Cube3D → Transform3D (translate / rotate with a Pivot — the gizmo handle should sit on the pivot and rotation turn around it) → Scene3D in the 3D viewport, ScanlineRender and Cycles; a ReadAlembicTransform on the axis input driving a ReadGeo; Transform3D → Transform3D chains (axis) for a hierarchy; a Camera3D on axis (keyed position only — Motion-page handheld / push-in are NOT read); an old project with a Group3D still loads with its transform where it was (page retitled "Transform"); **a Light3D under a Transform3D** lights from the moved position in ScanlineRender, Cycles and FastVolumeRender (spot / area orientation too), and a dome under a rotated group spins its HDRI; **a Camera3D under a Transform3D** renders from the moved position in ScanlineRender and Cycles, projects from it as a Project3D projector, and the viewport look-through matches (orbit / pan of a grouped camera is a known gap: writes knobs as if ungrouped); **group marker** = dashed orange box around the children (world space) + pivot cross + small origin tripod; the box must wrap the sphere wherever it is, nested groups and an archive included, and a parented child must be clickable where it is drawn; **Move Pivot to Centroid / Move Centroid to Origin** on a translated cube, a nested Transform3D and an Alembic archive (pivot lands on the object, origin button parks it at 0 and turns Enable Transform on; pressing with nothing connected shows the warning); **the gizmo now sits on the parented object** — drag a cube that is under a rotated Transform3D / Group3D and under an axis, check the knobs come back sane and the object follows the handle; **ScanlineRender** now renders geometry under a Group3D / Transform3D (it never did before) — compare with the viewport and Cycles; a Blender / Maya **Alembic with a rotated or scaled parent xform** now lands where the DCC had it in the viewport and Cycles (it used to be composed in reverse; ScanlineRender was already right), re-check any existing archive project that relied on a group/parent rotation.
- [ ] **Test ContentFill in the GUI** (2026-10-14, a6b59c73e, headless-verified): Roto over an object on texture (foliage, brick, gravel) and on the bridge-pillar plate; a Sample mask (the grids) on the part to borrow from; Patch Size 7 vs 13; Grow / Feather; Search Radius vs render time on a 4K plate.
- [ ] **Test PatchFill in the GUI** (built + headless-verified 2026-10-13, commits 360aa15fa + Detail tab): Roto → PatchFill.Mask over an HDRI with a light; Grow Mask past the halo; Fill / Matte / Detail outputs; a shape touching the image border; Lattice 8x8 vs 64x64; Detail Amount 1 with Band on a textured wall (Stamp Size vs the texture's feature size, Rotation Variation 0 on brick), Nearest on a grainy plate, Region on a clean patch, a grain plate on the Detail input.

### Reports and parked work

- [ ] **Camera3D keyframes land on the wrong frame** (user report 2026-09-08: keys set for f300 ended up on f50; unreproduced, deferred at the user's request — needs a repro: which knob, set via panel or viewport gizmo, timeline vs project frame range, autokey state).
- [ ] **GeoBuilder slice 2 — bake** (PARKED by user 2026-10-18 — never intentionally planned): write a placed grid card out as OBJ → ReadGeo, and a one-button Project3D + FrameHold projection setup on the card. Slices 3–4 = vertex editing / multi-frame alignment. The card is already a usable mesh everywhere ReadGeo is.
- [ ] DeepExpression 2.0 follow-up polish (2026-07-25): iterate on the Nuke-style expression node after first day of use
- [ ] Refine the particle template presets (Sparks / Jet Exhaust / Heat Distortion / Rain — values are a first pass, 2026-07-25)
- [ ] Fill the Project3D / Deep / HDRI template categories (placeholders in the menu; candidate list in session notes — projection patch rig, DCM comp starter, Blast cleanup rig, look-dev spheres, HDRI backplate starter)

### Engineering

- [ ] Particle audit fixes (2026-07-24 audit, details local in PARTICLE_AUDIT_NOTES.md):
  - [ ] P0 safety: emitter/spawn/ReadAlembic provider locking + immutable snapshots (same fix as solver cc05a8ac2); ScanlineRender motion blur must copy, not mutate the provider's data; solver cache-status labels must not evaluate from render threads
  - [ ] P1 determinism — batch A DONE 2026-07-25 (collided OR-across-substeps; dead-ID tombstones; solver fast-path after hash check + live-slot reset). REMAINING: per-frame emitter knob eval (fixes animated knobs + transform trail + dead Size Variance / Color From Image / Start Alpha knobs); monotonic IDs + Merge rekey; force physics (drag exp, gravity mass, attract softening, wind hash-by-id); forces-through-Merge (needs branch-scoping decision, see P2)
  - [ ] P2 subframe: forces take (time, dt); solver subframe lerp; sub-frame emission. (Cycles shutter samples: SUPERSEDED 2026-07-27 — velocity-based motion steps from the center frame, no re-sim, no ID matching needed)
  - [ ] P3 perf: DONE 2026-07-27 — collision knob hoist (per-frame CollisionEnv), all 7 force loops + ParticleAttribute multithreaded (ParticleParallel.h), Turbulence2D 2x dedup, preVel scratch reuse, curve LUTs, emitter colorVariance hoist. REMAINING: cache eviction (cacheMaxMB is currently a no-op); persistent viewport VBO; ScanlineRender shader/VBO reuse (per-sample vertex regen is the render-side bottleneck at 100k+)
  - [ ] P4 features: instance materials, ParticleExpression (reuse DeepExpression RPN engine), sprite textures, per-particle rotation, KnobGradient ramps in emitter, force falloff/mask framework, viewport force gizmos, Alembic interpolation
- [ ] **Linux follow-ups** (Linux port verified 2026-09-20: 9-distro CI matrix, Linux attached to numbered releases, RTX 4090 pod session):
  - [ ] **Attach the Linux AppImage to each numbered release** (same commit as the Windows package; run Linux Portable on that commit, download the AppImage + debug-symbols from the run or from the pre-release while it still points at that commit, `gh release upload <tag> ...`). (The rolling pre-release was dropped 2026-09-21: releases are numbered only, both platforms, same commit.) Noted in BUILDING.md (Linux section) and the local PACKAGING_NOTES.md.
  - [ ] **openfx-gmic (both platforms, optional)** — openfx-arena DONE on both platforms 2026-10-02; SeExpr nodes DONE 2026-10-07 (584 plugins); only G'MIC (GMIC.ofx) remains unbundled vs upstream. Upstream Natron bundles both (Text, ReadSVG, ReadPSD, G'MIC filters). Windows: MSYS2 has ImageMagick/poppler/librsvg packages, so arena is a normal 05-plugins addition; G'MIC is a very long compile. Linux: build the same deps into /usr/local in the portable image (cached under DEPS_KEY) and add both to `tools/linux/build-plugins.sh`. Do both platforms together so the packages stay identical.
  - [ ] **GPU Cycles (CUDA/OptiX)** — every build passes `-DWITH_CYCLES_DEVICE_CUDA/OPTIX=OFF`. Linux: CUDA toolkit + OptiX SDK in the portable build image, kernels compiled at build time, test on the GPU pod. Windows: harder, nvcc needs MSVC (the MSYS2/MinGW toolchain cannot drive it) — would need a separate MSVC kernel build step. Not required for parity between platforms; a feature decision.
- [ ] **Discord `#testing` forum for pre-release builds (2026-10-13)** — create the forum (Create Posts restricted to the maintainer, replies open; tags Needs testers / Windows / Linux / Confirmed / Still broken; guidelines: link the GitHub pre-release, say what changed and what to check, testers reply with OS + GPU + result). Then use it in the release flow: one post per test build (e.g. the OCIO `natron-2.6-test-issue4` pre-release for issue #4), flip the tag to Confirmed when enough replies say it works, close the issue, delete the pre-release, and edit the post to point at the numbered release. Companion to `#community-projects` (forum, created 2026-10-13: Nodes / Scripts / Templates / Integration / Help wanted).
- [ ] **Scatter node** (scatter points over geometry / a mask) and ParticleInstance driven by it. Point clouds themselves are done (PointsToParticles, see Done).
- [ ] **Deep sample picker** — pick the samples under the cursor in the viewer (DeepSampleFilter exists unregistered in Tier 3 and is a filter, not a picker).
- [ ] Material ramps / expressions / variations (Material3D side; particles have ParticleAttribute v2 ramps already)
- [ ] Shader updates for volumes (WIP — PrincipledVolume path works, remap curves in, needs more iteration)
- [ ] **Test all deep nodes** in the GUI — the 2026-07-24 audit was code-level (DEEP_AUDIT_NOTES.md, local); the Tier 1+2 "Needs Testing" lists below are still the checklist.
- [ ] **Deep validation gates** carried from the Blender-deep port: DeepFlatten-vs-Combined parity, VDB volumetric deep, Karma DCM comparison.
- [ ] **ParticleMaterial Phase 2** — temperature / blackbody mode (design in session notes); instance materials are in P4 above.

### Done (2026)

- [x] Denoise — CyclesRenderPass / CyclesRenderSettings Denoise knobs via OIDN (+ the CPU device-module fix). Ticked 2026-10-13.
- [x] Point clouds + ParticleInstance on point clouds — PointsToParticles adapter 2026-07-26 (any PointCloudProvider → ScanlineRender / Cycles / ParticleInstance), DeepToPoints camera unprojection, Blast round trip. Ticked 2026-10-13.
- [x] Use backplate in Cycles renders — shadow-catcher comp pass (`plate*(1-A) + RGB`) 2026-06. Ticked 2026-10-13.
- [x] Next release must ship the rebuilt `IO.ofx` — every package since 2026.09.15 is built from `tools/win-build/patches/openfx-io.patch` (the SeExpr release depends on it). Ticked 2026-10-13.
- [x] Generic per-knob lock (right-click → Lock Parameter) — 2026-07-27 (KnobHelper::setUserLocked, serialization v15).
- [x] Read: an empty Read given a file never guessed its colorspace — 2026-09-07 (`ReadNode.cpp`, decoder knobs held by the wrapper so the file change never reached the plugin).
- [x] Read/Write under an ACES config failed with `Color space 'default' could not be found` — 2026-09-09 (openfx-io patch, GenericOCIO fallback).
- [x] Project saved under one OCIO config failed under another (`'sRGB' could not be found`) — 2026-09-09 (synonym table + resolve on load).
- [x] PNG sequence in a fresh Natron still said `'default' could not be found` — 2026-09-13 (canonicalizeColorSpace fix in the patch).
- [x] Linux: intermittent abort on SIGTERM at shutdown — 2026-09-20 (self-pipe + QSocketNotifier; the CI matrix `shutdown` column watches it).
- [x] Material assignment for particles — ParticleMaterial 2026-07-27.
- [x] Fully test VDBs — EmberGen + Houdini fire/smoke via Cycles.
- [x] Blender deep — 2026-07-26, Blender PR #154410 ported into the Cycles fork + CyclesRender/CyclesRenderPass deep output (validation gates carried above).
- [x] Uniform Scale on ReadVDB — 2026-09-07.
- [x] ReadVDB frame-offset UX + single-frame VDB frame substitution + VDB error reporting — 2026-08-21 (a6ed2a97c, shipped in 08.24).

---

## ScanlineRender — Nuke parity

Matching Nuke's ScanlineRender controls.
Done + pushed: Antialiasing level (None/Low/Medium/High = MSAA), Overscan,
Projection Mode (Perspective / Orthographic / UV / Spherical), Ambient,
Transparency (`ffea831c4`), bg-input resolution/format conform (`ffea831c4` —
a connected bg/Reformat drives the output res AND format, Nuke-style).

Remaining:

- [ ] **Revisit Transparency semantics** — the `transparency` knob (on = respect surface alpha, off = force opaque) shipped 2026-06-17 in `ffea831c4` (rode along with the Project3D projective-texturing commit, disclosed in the message). It's committed + working; what's still open is whether the intended behaviour vs Nuke is right — our renderer already alpha-blends, so confirm the desired semantics before relying on it.
- [ ] **Tessellation max** — adaptive screen-space subdivision of polygons. Also the fix for the Spherical-projection pole/±180° seam distortion (coarse geo smears without it). Cycles got this on 2026-10-13 (per-mesh `Subdivision (Cycles)` group via `SubdivisionProvider.h`, Cycles' own OpenSubdiv dicer); ScanlineRender still needs a tessellation step of its own.
- [ ] **Z-blend (mode + range)** — blend intersecting / coplanar surfaces so they don't z-fight flicker (Nuke: none / smooth / linear + a range).
- [ ] **Depth of field** — Nuke does it on the MultiSample tab via `focus diameter` + `samples` (orbit the camera around the focal distance per sample; no f-stop knob). Needs the camera's focal distance.

## New nodes

- [x] **Project3D (camera-projection material shader)** — DONE, pushed 2026-06-17 (`b1d83b512`). Rebuilt **Project3D itself** as the material shader (plugs into a geo's mat input; projected by ScanlineRender; live preview in the 3D viewport for all geo types), matching Nuke's Project3D. **Project On** (front/back/both, default both), **Crop**, **Near/Far Clip** all work — and as of 2026-06-24 (`79d1fb988`) Crop + Project-On are honored in **both** the 3D viewport AND ScanlineRender (render discards culled/cropped fragments; viewport uses clamp-to-border + alpha-test + per-triangle facing cull). The separate "Project3DShader" node was dropped — Nuke's Project3D vs Project3DShader is just legacy-3D vs USD-3D (same function); we have one 3D system → one node. **REMAINING:** the **Occlusion** mode (none/self/world) knob + shader compare are wired, but the projector **depth pre-pass isn't built**, so occlusion currently projects through regardless of the setting ("self" first, then "world"). The matte-painting lock workflow uses a FrameHold on the projection camera.

- [x] **CameraTracker matchmove suite (ingested 2026-07-07)** — CameraTracker (full detect/track/solve at reference parity, planar quads, manual tracks + magnifier + nested table, focal from EXIF/self-calibration, scene orientation) + **PointCloudGenerator** (dense cloud from a solved camera) + **LensWarp** (undistort/redistort with the solve's lens model) + new **KnobTracksTable** knob type + libmv/ceres solver-core upgrades + the overlay-group null-deref core fix. **REMAINING (known-open list):** one hard forward-motion clip collapses at init (init-pair investigation queued); planar quads with corners leaving frame oscillate (designed fix: delta composition + hysteresis — fully-visible quads are production-usable); planar regions table (reuse the KnobTracksTable pattern); UHD >4k-track crash; plate-color the point clouds; distortion-at-render is via the LensWarp workflow (solved K1/K2 are not auto-applied to Camera3D/ScanlineRender).

- [x] **MergeMat (combine two materials — classic Nuke)** — DONE, pushed 2026-06-24 (`79d1fb988`). `Engine/Dev/Scene3D/MergeMat.{h,cpp}`: a `MaterialProvider` with A (foreground) + B (background) inputs, an Operation knob (none/replace/over/stencil/mask/plus/max/min) + Mix, that layers multiple Project3D projections onto one geo and **chains** (MergeMat→MergeMat) like classic Nuke. ScanlineRender flattens the tree into ≤4 layers composited per-fragment in the GLSL shader; the 3D viewport mirrors it multi-pass (one blended pass per layer) across all 5 geo types. A lone Project3D = 1 layer (single-projection unchanged). **LIMITS:** 4-layer cap; viewport blend ops over-exact / replace+plus approximated / rest→over (render does them properly); Cycles falls back to the foreground material (no layered projection yet — future MixShader combine).

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

- Basic deep pipeline (DeepRead, DeepFlatten, DeepMerge, DeepToPoints, etc.) tested and working. DeepRead reads `####` sequences (2026-07-26); Dots/DevStamps on deep wires resolve centrally in `getDeepImageFromEffect`.
- 2026-07-24 full-suite audit (details local in DEEP_AUDIT_NOTES.md): crash-class trio FIXED+pushed; remaining tier-2 items — ~25 nodes keep stale deep output on input disconnect (blanket onInputChanged sweep), DCM (A/Z/ZBack-only) no-ops in Relight/ContactShadow/Normalize/Grade.

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

- ~~DeepWrite needs testing with actual deep EXR output~~ — exercised by the Cycles deep output (2026-07, user-confirmed deep EXRs)
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
- **~~Tracks and solve lost on save~~** — FIXED 2026-09-20 (see TODO 9). Big solves add a few MB of text to the .ntp; compress the blob if that ever matters.
- **~~F in the 3D viewer never framed an object while the solve cloud was shown~~** — FIXED 2026-09-20 in `Viewport3D` (selection before cloud; outlier-tolerant cloud fit).

### TODO

1. ~~Fix viewer overlay crash~~ DONE
2. ~~Mask region for feature detection~~ DONE — ROI knobs + yellow dashed overlay
3. ~~Manual tracker points~~ DONE — "Add Track Mode" checkbox + click to place
4. **Camera path smoothing** — post-solve filter to remove jumps on early frames
5. Improve solve quality further (homography decomposition fallback)
6. Move tracking to background thread with progress dialog
7. Track management (select/delete individual tracks, bidirectional tracking)
8. Test with lateral-motion footage (should produce much better solves)
9. ~~Persist tracks / solve / planars / orientation with the project~~ DONE 2026-09-20 — hidden `trackerState` knob, rewritten after every button and mutating viewer edit, restored in `onKnobsLoaded`; Status line reports what came back.

### Files
- `Engine/CameraTrackerNode.h/cpp`
- See also: CameraTracker STATUS.md for detailed status

---

## 3D System / Scene3D

### Known Issues

- None currently — Camera3D, Card3D, Sphere3D, Scene, ScanlineRender all working in 3D viewport.

### Completed (2026-06-18) — FastVolumeRender ingest + Volume3D overhaul + viewport previews + ReadAlembicArchive fixes

- **FastVolumeRender** — real-time GPU VDB volume node (wgpu-native compute ray-marcher) ingested as a built-in sibling to CyclesRender; scene-driven lighting from Light3D (Distant=sun, Dome=ambient, multi-light Point/Spot/Area-beam each self-shadowed, fire-as-light), per-frame compression + uniform-buffer cache, multi-plane AOVs incl. per-light-group. Gated `NATRON_FASTVOLUME` (external wgpu dep — see BUILDING.md). Verified NVIDIA-only. **Follow-ups:** test AMD/Intel GPUs; brick compression is single-threaded (cold-frame cost); beauty-only / async readback to cut the ~45 ms draw floor.
- **Volume3D** — procedural-cloud overhaul (Sphere/Box + Enable-Noise: warp / coverage / erosion / edge-detail / seed / wind; vertical profile: base/top flatness + height-falloff; presets). Feeds FastVolumeRender (GPU, density-only). Possible polish: soft box edges, density-remap curve.
- **3D-viewport previews** — soft fire/smoke splats for Volume3D + ReadVDB (trilinear sampling, cached per path/frame/res, `viewportDisplayRes` slider).
- **ReadAlembicArchive viewport** — fixed the giant locator gizmo on baked-unit-scale archives (`drawTransformNode` was re-applying `localMatrix` on top of the dispatch's already-applied `worldMatrix` + inheriting the baked scale → now drawn once at a constant per-axis world size); added **Show Locators** + **Locator Size** knobs (Display page); **F (frame selected)** now fits the selection's real world-space mesh AABB so it tracks the user scale.

### Completed (2026-06-06) — Material override + archive transform + viewport isolate + fixes

- **GeoMaterialOverride ("Material Override") node** — per-sub-object materials on a
  ReadAlembicArchive without duplicating geo. `archive → Material Override → Scene`;
  Geo (input 0) passthrough + Mat (input 1). `surfaces` knob lists archive paths
  (exact / ancestor / bare-leaf match). Decorator: `SceneGraph::rebuild` tags
  `SceneNode.materialNode`; `collectSceneNodes` descends the Geo input; renderer
  precedence = downstream override > per-part > archive base. Disabled node bypasses.
- **Surface picker** — `Gui/MaterialOverridePickerWidget` (checkable tree from the
  connected archive, writes the `surfaces` knob), injected via
  `NodeSettingsPanel::initializeExtraGui`. No Q_OBJECT (lambda connects).
- **Render-cache fix** — CyclesRender scene hash now includes the per-part override
  (flag + override material params) so Surfaces/material edits re-render.
- **CyclesRender "Refresh Passes" button** — clears cache + refreshes metadata +
  re-renders viewers, for when a toggled AOV doesn't propagate without scrubbing.
- **AlembicTreeWidget crash fix** — unticking a surface freed the clicked item mid
  `itemChanged` (self-induced reload → `_tree->clear()`); guarded with
  `_applyingExcluded`. Files: `Gui/MaterialOverridePickerWidget.{h,cpp}`,
  `Engine/Dev/Scene3D/GeoMaterialOverride.{h,cpp}`, `GEO_MATERIAL_OVERRIDE_DESIGN.md`.
- **ReadAlembicArchive Transform tab** — Translate/Rotate/Scale + Uniform Scale on
  the archive root SceneNode → scales/moves the whole archive (worldMatrix
  propagation). `ReadAlembicArchive.cpp` + `SceneGraph.cpp`.
- **3D viewport "Isolate Selected"** — new second toolbar row + toggle; draws only
  the selected node + descendants. `Gui/Viewport3DTab.cpp`, `Gui/Viewport3D.{h,cpp}`.
- **Read node grow-on-scrub fix** — `NodeGui::adjustSizeToContent` fits width to
  content deterministically (no `boundingRect()` pen-margin creep). `Gui/NodeGui.cpp`.

### Completed (2026-06-04) — Geo auto-load on project open + viewport / review QoL

- **Geometry & Alembic nodes load on project open** — `ReadGeo`,
  `ReadAlembicArchive`, `ReadAlembicCamera`, `ReadAlembicTransform` only read
  their file inside `knobChanged` (file change / **Reload** click); restoring a
  saved knob value on project load doesn't fire `knobChanged`, so every loaded
  scene needed a manual Reload per geo node. Each node now overrides
  `onKnobsLoaded()` (post-deserialization hook) and re-runs its existing load
  path on the restored file. `ReadGeo`'s `.obj`/`.abc` dispatch + `isLoading`
  guard + metadata refresh was factored into a shared `loadGeoFromFile()`;
  `ReadAlembicArchive` also refreshes metadata. Empty path = no-op.
- **Backdrop renders as a flat color** — `NodeGraphRectItem` painted a
  top-to-bottom gradient over every node; added an opt-in flat mode
  (`setFlat`) enabled for backdrops only in `NodeGui::createGui`, so a Backdrop
  shows the picked color exactly (Nuke-style) while other nodes keep the
  gradient. (`Gui/NodeGraphRectItem.{h,cpp}`, `Gui/NodeGui.cpp`)
- **Read "Open in RV" button** — mirrors the Write node: "RV Executable" knob
  (pre-filled from `NATRON_RV_PATH`) + "Open in RV" button launching RV/OpenRV
  detached on the source pattern for fast input review. (`Engine/ReadNode.cpp`)

### Completed (2026-05-30) — CyclesRenderSettings node + wiring

- **`Engine/Dev/Cycles/CyclesRenderSettings.{h,cpp}` (new)** — sink node
  with ~13 getters covering samples + denoise + integrator bounces + DOF
  (enabled/focus/blades/rotation in degrees) + motion blur
  (enabled/shutter/position). Knob layout: Render (samples, denoise) /
  Integrator (4 bounce counts) / Depth of Field (4 knobs) / Motion Blur
  (3 knobs). Defaults match CyclesRender's existing knob defaults
  verbatim so swapping the source of truth produces identical output.
  Originally scoped with a `CyclesSettingsProvider` abstract interface
  (mirroring `CameraProvider` / `MaterialProvider`), but Qt6's AUTOMOC
  silently skipped Q_OBJECT under multi-inheritance with a non-QObject
  second base — fell back to duck typing on the concrete class.
- **Registration** — `PLUGINID_NATRON_CYCLESRENDERSETTINGS` added to
  `Engine/EffectInstance.h`; `registerBuiltInPlugin<CyclesRenderSettings>`
  added next to the other Cycles plugins in `AppManager.cpp`.
- **CyclesRender wiring.** Input count 3 → 4; slot 3 = "settings",
  optional. New `onInputChanged` hides the Render / Integrator / DOF /
  Motion Blur knobs via `setSecret(true)` when a CyclesRenderSettings is
  wired (Output / AOV / Focus helper / EXR knobs stay visible since they
  remain CyclesRender-only). `render()` resolves the provider once at the
  top of the function and threads each consumer through a ternary:
  `settings ? settings->getX() : _imp->X.lock()->getValue()`. The
  integrator resolution moved above the cache hash (was inside the
  cache-miss branch); changing bounce counts now invalidates the hash and
  triggers a re-render (was a latent bug).
- **CyclesRenderPassManager wiring.** Input count 3 → 4; slot 3 =
  "settings", optional. Provider resolved once in
  `parseAndDumpActivePasses` and threaded into `renderFrameForBatches`
  via a new parameter. Per batch, DOF / Motion Blur / Integrator structs
  are populated from the provider when connected and set on
  `CyclesPassRequest`. DOF apertureSize derives from the active camera's
  focal length + F-Stop (cameraOverride first, then input 2). When the
  Settings input isn't connected, the renderer falls back to its own
  defaults (today's behavior). Per-pass JSON `samples` still wins over
  the provider for that pass. New stderr line "Settings input: connected
  / not connected" announces which path is taken.

### Completed (2026-05-30) — CyclesRenderPassManager — light-group / object / camera scoping + non-EXR output

- **`@all_light_groups` magic AOV token.** Drops the
  separate-file `lg_key_example` from the seed JSON in favor of the
  production relight pattern: beauty bundles every light group as a
  layer. `parseAndDumpActivePasses` enumerates scene lights up front
  (one `enumerateSceneLights` call, two consumers: the diagnostic dump
  and the AOV expander). The per-pass AOV parser dedupes via a
  `seen` set so explicit `Combined_<group>` + the token can co-exist.
- **Per-pass object scoping** — `candidateObjects` / `excludeObjects` /
  `soloObject` mirror the light-scoping JSON fields.
  `CyclesPassRender.{h,cpp}` gains `SceneGeoInfo` + `enumerateSceneGeo`
  (same input-1 → optional CyclesRenderPass → Scene3D/Group3D walk as
  `enumerateSceneLights`, just collects everything that isn't a
  Light3D). `resolveActiveObjects` honors solo > candidates >
  "all-minus-excludes". When scoped, a full `ObjectVisibility` map
  covering every scene node is built (visible = `rayVisibility=0x7FF`,
  scoped-out = `isExcluded=true`) and routed through
  `CyclesPassRequest::visMap`. Unscoped batches pass `nullptr` so the
  renderer skips the visibility code path entirely.
  `parseLightList` renamed to `parseNameList` (now used by both
  scopings).
- **Per-pass camera override** — `cameraOverride` takes a fully-
  qualified script name of any `CameraProvider`-derived node anywhere
  in the project. `CyclesPassRequest` grows a `const CameraProvider*
  cameraOverride` (header pulls in `CameraProvider.h`); the helper
  consults it first, falls back to input slot 2, then to the
  renderer's hard-coded defaults. Resolution in the manager goes
  `getApp()->getNodeByFullySpecifiedName` → `dynamic_cast`; a
  non-empty-but-unresolvable name aborts the batch with a clear
  stderr line instead of silently rendering the wrong angle.
  `enumerateProjectCameras` (uses `getProject()->getNodes_recursive`)
  dumps every candidate node so users can match what to type.
- **PNG / TIFF / JPEG output.** `CyclesRenderer::saveSingleImage`
  joins `saveMultiLayerEXR`, dispatching on file extension:
  PNG = 8/16-bit + deflate; TIFF = 8/16/32-bit + ZIP/LZW/None; JPEG =
  8-bit RGB only, `compression` reinterpreted as quality 1-100. Beauty
  `Combined` writes RGBA in PNG/TIFF; other AOVs write RGB so
  scalar-broadcast AOVs (Depth/AO) don't end up with alpha=0. The
  manager's per-pass save loop dispatches by extension on
  `resolvedPaths[idx]`: `.exr` → existing multi-layer; other formats
  loop the spec's AOVs and inject `_<AOVName>` before the extension on
  multi-AOV passes so AOV files don't overwrite each other. Single-AOV
  passes write to the path as-is.
- **Batching key** now spans samples + active-lights set +
  visible-objects set + camera-override name. Specs with identical
  scoping still batch together; any difference forces a new Cycles
  session. Per-batch stderr line includes
  `lights=[...] objects=[...] cam=<name|<input2>>` so each batch's
  scope is visible at a glance.

### Completed (2026-05-29) — CyclesRenderPassManager — real Cycles output + batching + frame range + per-pass output

- **`Engine/Dev/Cycles/CyclesPassRender.{h,cpp}` (new)** — exposes
  `renderCyclesPassesForEffect(effect, CyclesPassRequest, outBuffers,
  errOut)`. Builds the scene graph from the effect's input 1 (walking
  through optional `CyclesRenderPass`, then `Scene3D`/`Group3D` containers),
  bakes Material3D textures, pulls camera params from input 2, and
  calls `CyclesRenderer::renderToBufferWithCameraMultiPass`. Logic
  intentionally duplicates the corresponding block of
  `CyclesRender::render()` for this phase (5A.1); 5A.2 will refactor
  CyclesRender to call the helper too and delete the duplicate.
- **Batching engine (step 6).** Active passes grouped by hash of
  `samples` (the only per-pass override wired today; future per-pass
  camera / visibility / lights / shader keys extend the bucket). Per
  batch: render the union of all batch members' AOVs in ONE Cycles
  session, then demux per-pass via a filtered buffer map and call
  `saveMultiLayerEXR` once per pass. Common case (passes share
  scene state): N passes from 1 Cycles session instead of N sessions.
- **Frame range support.** New knobs: `frameMode`
  (Current / Range / Range No Re-render), `frameStart` (default 1),
  `frameEnd` (default 100), `frameIncrement` (default 1). Render to
  Disk wraps the batching engine in a frame loop; each frame
  re-resolves `####` etc. and runs the batches independently. No
  Re-render mode skips frames whose ALL output files already exist on
  disk — useful for resume after interrupt or for incremental updates.
- **Project format auto-detect.** Resolves output width/height via
  `effect->getApp()->getProject()->getProjectDefaultFormat()` and uses
  that for every Cycles request. Falls back to 1920×1080 only if no
  project is available. Logged in stderr alongside the frame mode
  banner (`Frame mode: ..., output=WxH`).
- **Per-pass output settings.** JSON fields `bitDepth` (`16-bit Half` /
  `32-bit Full`) and `compression` (ZIP / ZIPS / PIZ / DWAA / DWAB /
  RLE / PXR24 / B44 / B44A / None) are now parsed and threaded through
  to a new `CyclesRenderer::saveMultiLayerEXR` 5-arg overload taking
  `ExrOutputOptions { bitDepth, compression }`. Case-insensitive
  matching with safe fallback to float32 + ZIP on anything unknown.
  Existing 3-arg overload retained for backward compat with
  CyclesRender's manual EXR button. `format` field is parsed too but
  PNG/TIFF/JPEG remain on the future-work list — MVP supports
  multi-layer EXR only.

### Completed (2026-05-29) — CyclesRenderPassManager MVP 1-5C (new sink node)

- **`Engine/Dev/Cycles/CyclesRenderPassManager.{h,cpp}`** — new sink node
  registered as `fr.inria.built-in.CyclesRenderPassManager` in the 3D
  category. Mirrors CyclesRender's input layout (bg / obj / cam) so it
  drops into existing scene wiring.
- **JSON-backed pass list.** Single `passesJson` `KnobString`
  (multi-line) seeded with a 2-pass default (beauty Combined + data
  Depth/Normal/UV). Persists in project files via normal knob
  serialization — verified round-trip on save/load.
- **Reset to Default button** — restores the seed JSON without
  re-creating the node.
- **Render to Disk button (5C scope)** — parses the JSON via Qt's
  `QJsonDocument`, filters by the data.js active rule
  (`enabled && output && !mute && (!soloActive || solo)`), resolves
  dollar-token paths (`$PASS` from the spec, `$SHOT` / `$RENDER` from
  env vars with `"shot"` / `"/tmp"` fallbacks), then frame-pattern
  resolves (`####` / `%04d` / trailing digit group, same logic as
  `CyclesRenderer::resolveTextureFrame`), and writes synthetic 64×64
  multi-layer EXRs at the resolved paths. Layer/channel names match
  the existing `CyclesRenderer::saveMultiLayerEXR` convention so files
  open in Read with the expected layer dropdown.
- **Architecture + open questions** documented in
  `RENDER_PASS_MANAGER_DESIGN.md` (local-only). Tracks the batching
  rule (group passes by shared scene state — camera+vis+light+samples+
  overrides — and run one Cycles session per batch with the union of
  AOVs), open questions (preview output, custom widget priority,
  token vocabulary, material override scope), and the MVP slice that
  validates each.
- **Audits + catalog** in `CYCLES_PASS_AUDIT.md` and `CYCLES_PASS_CATALOG.md`
  (local-only). The audit documents the current pass plumbing in
  `CyclesRender.cpp` (12 enabled passes via 12 knobs, single-Cycles-call
  multi-pass via `NatronMultiPassOutputDriver`, what controls already
  exist via the upstream `CyclesRenderPass` node, what's missing for the
  spec). The catalog enumerates all 49 `PassType` enums from
  `D:\_vfx_claude_2025\cycles\src\kernel\types.h:497-581` bucketed
  across the 6 categories the UI uses (Beauty / Shadows / Additive /
  Data / Reflection-Refraction / Matte-ID), plus the non-enum mechanisms
  (cryptomatte bitmask, light groups, AOVs, shadow catcher, holdout,
  denoising auto-allocation).

### Pending — CyclesRenderPassManager nice-to-haves

- **Shadow Catcher AOV (raw accumulator + sample count)** — we
  expose `ShadowCatcherMatte` (`PASS_SHADOW_CATCHER_MATTE`), which in
  approximate mode contains "non-catcher objects + catcher with
  shadow baked in" — the practical beauty pass when shadow catchers
  are in the scene. The other two passes in Cycles' canonical trio
  (`PASS_SHADOW_CATCHER` raw accumulator + divide-by-Combined in
  accessor + `PASS_SHADOW_CATCHER_SAMPLE_COUNT`) would let users
  composite shadow against an external plate explicitly. Exposing
  those requires registering all three passes and documenting the
  comp formula `final = (Combined / SHADOW_CATCHER) * plate +
  SHADOW_CATCHER_MATTE over plate`. First attempt returned uniform
  1.0 because the accessor's internal divide bottoms out at the
  default when only `PASS_SHADOW_CATCHER` is populated.
- **Custom UI widget for the pass table** — DONE (2026-06-01,
  uncommitted). New `KnobPassTable` (type `"CyclesPassTable"`) +
  `Gui/PassTableWidget` + `Gui/KnobGuiPassTable`, mirroring the
  DevShuffle custom-knob pattern (GUI matched by knob typeName). Backing
  store stays the same pass-list JSON string, so the renderer / save /
  load / undo are unchanged. Increments: (1) spreadsheet table
  (Name/Type/On/Samples/AOVs/File + Add/Remove), (2) readability
  styling, (3) per-pass "Selected Pass" detail panel for the non-column
  fields, (4) discovered-name dropdown pickers for the object/light
  scoping fields + a "Refresh Objects" button, backed by a new
  `CyclesRenderPassManager::discoverSceneObjects()`, (5) AOV checkbox
  grid (ticks ↔ the pass `aovs` array; custom/token AOVs preserved),
  (6) live multi-plane **preview**: "Preview Selected" renders the chosen
  pass to a connected Viewer with each ticked AOV as a Viewer layer —
  `render()` rewritten from a transparent-black sink, secret
  `previewPassIndex` knob, node made `isMultiPlanar()` +
  `getComponentsNeededAndProduced`. Deferred polish: checkable
  multi-select dropdowns, camera/material pickers, collapse the tall
  detail panel into sub-groups, "clear preview" affordance. See
  `PASS_TABLE_WIDGET_NOTES.md` (repo-root-adjacent, not committed).

### Completed (2026-05-30) — Per-pass material override + visibility promotions

- **JSON model.** Four new fields per pass: `materialOverride` (script
  name of a Material3D-like node), `shadowCatcherObjects`,
  `holdoutObjects`, `traceObjects` (semicolon-separated script names).
- **`CyclesRenderer::renderToBufferWithCameraMultiPass` + `syncSceneWithCamera`**
  gain a `MaterialProvider* materialOverride` param. The per-mesh
  `createMaterialShader` call site substitutes the override when set
  so every mesh in the batch uses the same shader. Particles + volumes
  keep their own shader paths (no override).
- **`CyclesPassRequest::materialOverride`** threads through
  `executeCyclesPasses` to the renderer.
- **`enumerateProjectMaterials`** mirrors `enumerateProjectCameras` and
  prints every `MaterialProvider`-derived non-geo-wrapper node in the
  project for the diagnostic dump. `hasMaterialInput()` filters out
  geo nodes (Sphere3D, Card3D, …) so only genuine material sources
  appear.
- **`renderFrameForBatches` visMap construction** extended: builds a
  full map covering every scene object when ANY of object scoping,
  shadowCatcherObjects, holdoutObjects, traceObjects is set. Per
  object: excluded → `isExcluded=true` (rayVis 0); shadow catcher
  → rayVis 0x7FF + `isShadowCatcher=true`; holdout → rayVis 0x7FF +
  `isHoldout=true`; trace → rayVis 0x7FE (ALL & ~CAMERA); else default
  visible. Priority: excluded > SC > HO > TR.
- **`batchKeyFor`** extends with `|mat=…|sc=…|ho=…|tr=…` segments so
  different overrides / promotions force separate Cycles sessions.
  Same override + same promotions still batch together.
- **Per-batch stderr line** prints `mat=`, `sc=[…]`, `ho=[…]`, `tr=[…]`
  alongside the existing scoping summaries. Material/SC/HO/TR
  resolution that fails the lookup aborts that batch loudly.

### Completed (2026-05-30) — CyclesPassRender prepare/execute split (5A.2)

- **`Engine/Dev/Cycles/CyclesPassRender.{h,cpp}`** — refactored from a
  single `renderCyclesPassesForEffect()` into a two-step API:
  - `prepareCyclesPasses(effect, req, out, err)` walks input 1 (through
    optional CyclesRenderPass into Scene3D/Group3D), builds the scene graph,
    bakes Material3D input textures, and resolves the camera
    (override > input 2 > defaults). Fills a new `CyclesPassPrepared`
    struct with the resolved `sceneGraph`, `camTX..camVA`, and the
    discovered `CyclesRenderPass*`.
  - `executeCyclesPasses(renderer, prepared, req, outBuffers, err)`
    invokes `renderToBufferWithCameraMultiPass` on a caller-owned
    `CyclesRenderer&`. CyclesRender uses `_imp->activeRenderer` for
    cross-frame `cancelRender()`; PassManager hands in a fresh local
    instance per batch via the wrapper.
  - `renderCyclesPassesForEffect()` retained as a thin wrapper for the
    PassManager (prepare + local renderer + execute).
- **`Engine/Dev/Cycles/CyclesRender.cpp`** — `render()` swaps out two
  large duplicated blocks for `prepareCyclesPasses` + `executeCyclesPasses`
  calls. The cache hash block reads `sceneGraph` / `renderPass` via
  local aliases bound to `prepared`, so hash inputs are bit-for-bit
  identical pre/post-refactor and existing cache behavior is preserved.
  CyclesRenderPass visibility resolution stays in the cache-miss branch but
  writes into `req.visMap` / `req.activeLights` rather than separate
  pointer locals. CyclesRender::render() drops ~70 lines net.

### Completed (2026-05-29) — CyclesRender Mist AOV + single-value display fix

- **`Engine/Dev/Cycles/CyclesRender.cpp` + `CyclesRenderer.cpp`** — added
  PASS_MIST end-to-end via the now-canonical 5-step add-pass recipe: knob
  → `getEnabledPasses` → `passNameToPlane` → `standardPasses[]` table →
  per-pixel broadcast block. Proves the pattern is reusable for the ~33
  remaining unwired Cycles passes (cryptomatte, motion vector, position,
  denoising aux, volume direct/indirect, etc.).
- **Display fix for single-value AOVs.** Depth / AO / Mist were defined as
  1-channel "A" planes, which Natron's viewer in default RGB mode renders
  as black (R/G/B undefined). Switched all three to 3-channel R/G/B
  planes with the scalar broadcast in the per-pixel blit. Bonus: AO now
  displays correctly too.
- **Recipe + trap saved as memory entries** — `reference_cycles_add_pass_recipe.md`
  (5-step pattern) and `feedback_natron_single_channel_plane_display.md`
  (the 1-ch "A" display trap).

### Completed (2026-05-29) — SphericalTransform Faces format

- **`Engine/Dev/Transform/SphericalTransform.{cpp,h}`** — input count 1 → 7;
  slot 0 is the legacy single-image source (`img`), slots 1-6 are per-face
  cubemap inputs labeled `-Z, +Z, -X, +X, -Y, +Y` (canonical / Nuke order).
  When `Input Projection = Cubemap` AND `Input Format = Faces`, render()
  pre-fetches each connected face into a `FaceSource` array and per output
  pixel computes its 3D direction → `cubeFaceFromDirection()` → looks up
  the internal-face → slot mapping via `kInternalFaceToFaceSlot[]` →
  reseats sampling state at that face. Missing faces output black per spec.
- **`getRegionOfDefinition` + `getPreferredMetadata` fallback** — slot 0
  may legitimately be empty in Faces mode; both fall through to the first
  connected face input as the size reference instead of returning failure
  (an early failure poisons the engine cache for subsequent knob changes).
- **`isInputOptional` returns `true` for all slots.** The root-cause bug
  fix — when slot 0 is mandatory and disconnected, the viewer's
  `checkTreeCanRender_internal` (`Engine/ViewerInstance.cpp:702-730`)
  silently bails out before any RoD/render call. Matches the pattern in
  Scene3D / DevShuffle / ScanlineRender / ParticleInstance / ParticleMerge
  / Group3D / CyclesRender.
- **Trademark scrub** — replaced 9 references to a competing compositing
  host with neutral phrasing (`"canonical order"`, `"compositing DCCs"`,
  `"standard optical models"`, etc.) across `SphericalTransform.{cpp,h}`
  and `SphericalProjections.h`.

### Completed (2026-05-28) — Scene-wide motion blur + GLSL closeout + 3D viewport `F` in look-through

- **Scene-wide multi-sample motion blur** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. New "Motion Blur" group on the Output tab: `Samples`, `Shutter`, `Shutter Offset` (Centered / Start / End / Custom — mirrors Nuke's ScanlineRender), `Custom Offset`, `Temporal Jitter` (deterministic per-(sample, frame) hash). Per-sample loop re-evaluates the camera (view + proj + projView), re-extracts every animated geo at sub-frame time, and re-queries instances with manual `pos + vel * sampleDt` extrapolation (the particle sim only runs once per frame, so getInstances/getParticles at sampleTime return frame-time data — extrapolation is required). Particle velocity-stretch cheat mode preserved for `motionSamples == 1`.
- **Animated Alembic sub-frame interpolation** — `Engine/Dev/Scene3D/ReadAlembicArchive.cpp` + `Engine/Dev/Scene3D/ReadGeo.cpp`. `getMeshDataAt` / `getEntryWorldMatrix` / `updateTransformAtTime` / `updateVerticesAtTime` all lerp between bracketing samples instead of snapping `floor(time + 0.5)`. Both timeMode 0 (per-frame) and timeMode 1 (source-time) paths use continuous sample indices. `ReadAlembicCamera` already correct (samples stored as Natron knob keyframes which interpolate natively).
- **Cycles vertex motion blur for Alembic** — `Engine/Dev/Cycles/CyclesRenderer.cpp`. Added `int archiveEntryIdx = -1` to `SceneNode` (`Engine/Dev/Scene3D/SceneGraph.{h,cpp}`) so the renderer can re-query the upstream archive at sub-times. Hoisted shutter open/close outside the per-node loop. Mesh path now sets `mesh->set_use_motion_blur(true)` + `set_motion_steps(3)` and populates `ATTR_STD_MOTION_VERTEX_POSITION` with sub-time vertex positions for both ReadAlembicArchive and ReadGeo sources. Vertex motion blur only — xform motion deferred (requires SceneGraph parent-chain re-eval at sub-time).
- **GLSL volume rendering migration** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. ReadVDB + procedural Volume3D ray-march migrated from legacy fixed-function (`gl_ModelViewMatrix` / `gl_Vertex` / `gl_FragColor`) to GLSL 3.30 core with explicit `u_modelView` / `u_projView` uniforms. New `drawVolumeProxyCube` helper builds a transient 36-vertex VBO + glDrawArrays, replacing the `glBegin(GL_QUADS)` immediate-mode draw. `GL_LUMINANCE` → `GL_R32F` / `GL_RED` for core-profile compatibility (shader still reads `.r`, no behavior change).
- **GLSL ParticleInstance migration** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. New `kInstanceVert` / `kInstanceFrag` shader pair (modeled on `kBeautyVert/Frag` but trimmed: per-instance color uniform, no texture/STW, stretch-fade uniforms for cheat-mode MB). One VBO per geo type built once per render with interleaved pos3 + normal3 + uv2 (stride 8); Cube3D forwards `CubeVertex.u/v`, Sphere3D gets synthesized spherical UVs. Per-vertex flat normals derived from triangle cross-product. Per-instance loop builds `localMatrix` (T·R·S) + `normalMatrix` (transpose(inverse(3x3))), uploads uniforms, `glDrawArrays`. Instances now participate in MRT — Normal / UV / Pref / Velocity AOVs all written (legacy immediate-mode path skipped MRT entirely). Multi-sample MB uses per-instance velocity extrapolation as above.
- **Normal AOV fix for file-loaded meshes** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. ReadGeo and ReadAlembicArchive paths weren't populating `geo.normals` → GLSL `normalize(vec3(0))` returns NaN → renders white in float AOV. Added `computeVertexNormalsFromTris` helper (area-weighted face-normal averaging per vertex), called from both mesh paths. Procedural shapes (Sphere3D etc.) already had explicit normals — unchanged.
- **Non-cubic VDB 3D texture** — `Engine/Dev/Scene3D/ReadVDB.{h,cpp}` + `Engine/Dev/Scene3D/ScanlineRender.cpp`. `VDBVolumeData::resolution` → `resX/resY/resZ`. Sampling loop maps the largest VDB axis to `maxResolution`, scales other axes proportionally (min 8 per axis), preserving the VDB's voxel aspect. Non-cubic smoke / fire no longer renders as a soft blob filling the bounding box.
- **Legacy fixed-function stack removed** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. With volume + instance now GLSL, the per-sample `glMatrixMode` / `glLoadMatrixf` calls inside the multi-sample loop have no remaining consumers and are gone. Every draw path in ScanlineRender is now GLSL 3.30 core, uniform-driven.
- **3D viewport `F` works in look-through** — `Gui/Viewport3D.cpp`. F (Frame Selected / point cloud / reset) now also handles look-through-camera mode: if the look-through source is an editable Camera3D, the camera node's translate knobs get rewritten so the framed target sits at framing distance along the camera's view direction, preserving its rotation. ReadAlembicCamera (read-only) falls back to perspective-mode framing so the user still gets a useful view.

### Completed (2026-05-27) — ParticleAttribute (V2 three-section design) + KnobGradient + ScanlineRender Solid

- **ParticleAttribute** — `Engine/Dev/Particles/ParticleAttribute.{h,cpp}`. ParticleModifier passthrough with **three always-visible sections** (Color / Pscale / Alpha). Each section: Enable, Isolate (radio across sections), Source (Age/Lifetime / Speed / Velocity / Position / BounceCount / SpawnIndex), Source Min/Max + Fit button, editor (Gradient for Color, KnobParametric Curve for Pscale + Alpha), Mix, Reset, and a live read-only Summary label. Top-level **Reset All** button restores every section. `applyForce` iterates particles once, applying all enabled sections (Color → RGBA via gradient sample; Pscale → size via curve; Alpha → a via curve). Defaults: warm fire gradient for Color, ease-out (ending at 0.1 so dying particles stay visible) for Pscale, bell for Alpha. Placement: downstream of `ParticleSolver` (the solver's step-6 appearance-resync overwrites the modifications otherwise).
- **KnobGradient + KnobGuiGradient + GradientWidget** — new custom knob infrastructure backing ParticleAttribute's Color section. Engine: `Engine/Dev/Particles/KnobGradient.{h,cpp}` stores stops as a comma-separated YAML-safe string (`"0,ffd14aff,0.45,ff5a00ff,1,1a0010ff"` — `:` and `|` separators both broke project load, comma matched the KnobShuffle precedent). Gui: `Gui/KnobGuiGradient.{h,cpp}` + `Gui/GradientWidget.{h,cpp}` is a draggable gradient bar with click-to-add stops, drag-to-move, double-click-to-edit (QColorDialog), right-click delete. Registered in both `KnobFactory.cpp` (Engine) and `KnobGuiFactory.cpp` (Gui) mirroring how KnobShuffle is wired.
- **ScanlineRender Solid mode** — `Engine/Dev/Scene3D/ScanlineRender.cpp`. New `Solid` knob on the Particles page. When on: blend func switches to `GL_ONE / GL_ZERO` (replace), depth-write enabled so particles occlude each other and the scene, and disc/sphere/sprite fan rings use full alpha at edges instead of fading to 0. Closes the "I want opaque hard-edged particles" gap (default behavior was soft fade-to-transparent for compositor-style blending).
- **Phase 7 polish** — Reset All button at top of ParticleAttribute, live Summary labels per section, refined tooltips on Source/Source Min/Source Max/Mix with concrete examples and unit hints.

### Completed (2026-05-27) — Alembic particle round-trip + ParticleSolver multi-frame cache

- **WriteAlembicParticles** — new node, `Engine/Dev/Particles/WriteAlembicParticles.{h,cpp}`. Passthrough particle node (extends `ParticleModifier` with a no-op `applyForce`, particles flow through unchanged). Knobs: `File` (KnobOutputFile, .abc path) and `Bake` button. Clicking Bake iterates the project's `getFrameRange()` at `getProjectFrameRate()` FPS, pulls particles from input 0 each frame via `ParticleProvider::getParticleData()`, and writes an OPoints sample per frame containing positions, IDs, velocities (as the standard velocities slot), plus RGBA color (`Cd` C4f) and per-particle size (float) as arbGeomParams. Uses `Alembic::AbcCoreOgawa::WriteArchive` + `Alembic::AbcGeom::OPoints`. Synchronous on the UI thread (no progress dialog yet — per-frame status logged to stdout). Registered as `fr.inria.built-in.WriteAlembicParticles`. **First disk-writing node in the 3D system.** Verified end-to-end: baked file loads cleanly in Houdini's particle view.
- **ReadAlembicParticles** — companion read node, `Engine/Dev/Particles/ReadAlembicParticles.{h,cpp}`. Inherits `EffectInstance + ParticleProvider` (a source, no inputs). Knobs: `File` (KnobFile), `Reload`, `Info` (read-only label showing "N samples, M-K particles"). On file change, opens the archive via `Alembic::AbcCoreFactory::IFactory`, walks to find the first `IPoints` (depth-first), and pre-loads every sample into RAM (positions, IDs, velocities, optional `Cd` color + `size` from arbGeomParams). `getParticleData(time)` maps Natron-time → Alembic seconds via project FPS, snaps to nearest sample via `TimeSampling::getNearIndex`, builds a `ParticleData` from the pre-loaded arrays. Marks itself frame-varying via `getPreferredMetadata` when sample count > 1 (mirrors the `ReadAlembicArchive` precedent — without it Scene3D/ScanlineRender would freeze on a single frame). V1 picks the first IPoints; multi-points dropdown is future work. Closes the round-trip: Solver → WriteAlembicParticles → (edit in Houdini) → ReadAlembicParticles → downstream.
- **ParticleSolver multi-frame RAM cache** (Phase A skeleton + Phase B wiring landed together):

- **Phase A — skeleton** (landed first): added "Cache" page on ParticleSolver with knobs `Cache Simulation` (bool, default on), `Max Cache (MB)` (default 1024, range 64-16384), read-only labels `Cached Frames` and `Cache RAM`, and a `Clear Cache` button. Backing data in `ParticleSolverPrivate`: `std::map<int, CachedFrame>` keyed by frame, `std::mutex` for thread safety, `frameCacheHash` U64 sentinel for upstream-input invalidation, `frameCacheBytes` for memory accounting. Render path unchanged in A.
- **Phase B — wired into the render path**: `getParticleData(time)` now consults the multi-frame cache. (1) Hash invalidation — XOR of input0/input1 `getHash()` + own knob values (elasticity, friction, maxBounces, substeps); mismatch with stored `frameCacheHash` wipes the cache before simulating. (2) Exact-frame hit returns immediately (deep-copies state + `knownIDs` out, skips the integration loop entirely). (3) Cache miss finds the nearest cached frame F ≤ endFrame via `std::map::upper_bound`, restores from F, integrates from F+1 to endFrame, caches each integrated frame as it goes. Live status labels update on every miss. **No LRU eviction yet** — cache grows unbounded (capped manually via `Clear Cache` or natural `Max Cache (MB)` ignored). Phase C adds eviction.
- **Debug coloring (showCollisions tint) factored out** so the cache stores uncolored state. Toggling the knob no longer requires a cache flush.
- **Existing single-slot cache (`cachedData`/`cachedFrame`/`knownIDs`) preserved** — acts as the fast path when the previous call's time matches exactly.

### Completed (2026-05-24) — Animated Alembic + viewport lighting + camera default

- **ReadGeo + ReadAlembicArchive: animated vertex (deforming) meshes** — was loading sample 0 only, freezing on the rest pose. Both nodes now pre-load all vertex samples at file-open into per-entry caches; `getMeshData(time)` / `getMeshDataAt(idx, time)` memcpy the matching sample into `mesh->vertices` before returning. Topology stays constant across samples (the standard Maya/Blender/Houdini export). Archive time-to-sample mapping mirrors `getEntryWorldMatrix` (timeMode 0 per-frame + Frame Offset, timeMode 1 nearest source-time).
- **ReadGeo + ReadAlembicArchive: `setIsFrameVarying(true)`** — declared via new `getPreferredMetadata` override when the loaded file carries animated content (xform OR vertex samples). Without this, Scene3D never saw a time-varying upstream — it only checks knob animation via `inp->getHasAnimation()`, not embedded `.abc` data — so ScanlineRender's image cache froze on a single frame even with animated geo. CyclesRender always worked because it sets frame-varying on its own node; the 3D viewport bypassed the cache via direct `getMeshData(time)` calls. Both nodes also call `refreshMetadata_public(true)` after a load.
- **ReadGeo: user TRS knobs honored in ScanlineRender + Cycles + viewport** — was silently dropped (only `mesh->transform` was applied). Both `ScanlineRender::extractGeometry` and `SceneGraph` now compose `userTRS × embedded`, so animated `.abc` xforms still play through alongside user offsets.
- **3D viewport shaded mode on imported meshes** — was uniformly dark because `ViewportFaceLitFactor` assumed CCW-from-outside winding. Imported meshes have unpredictable winding (CW, CCW, mixed triangulated soups), so the cross-product face normal could point inward → N.L clamped to 0. Switched to two-sided `abs(N.L)` on the cross-product path; procedural primitives (Sphere/Card/Cube/Cyl) keep `ViewportLitFromVertexNormals` untouched.
- **Camera3DNode: V Aperture default matches project aspect** — was hardcoded 18.672mm (Academy 35mm), forcing users to click "Match Project Aspect" on every new camera. Default is now computed in `initializeKnobs` as `24.576 * (projectH / projectW)`, so a fresh Camera3D on a 1920×1080 project drops in at ~13.824mm and reports "Sensor aspect matches project" immediately. Fallback to 18.672mm when no project is available.

### Completed (2026-05-23) — Phase 3 GLSL/MRT migration + Shading Modes

- **ScanlineRender GLSL 3.3 + MRT pipeline** — fixed-function `glBegin`/`glEnd` mesh + particle paths retired. Single VAO/VBO/IBO + `kBeautyVert`/`kBeautyFrag` shader pair drives every geo draw; particle draws use a parallel `kParticleVert`/`kParticleFrag` pair. UVProject's STW projective texturing handled in-shader via `u_hasTexture == 2` branch.
- **6 per-pixel AOVs** — Depth (linear camera-space), World Position (reconstructed via inverse(MVP)), Normal (world-space), UV, Pref (object-space ref position), Velocity (screen-pixels-per-frame). Declared on plane -1 via `isMultiPlanar()` + `getComponentsNeededAndProduced`. MRT attachments allocated lazily per-AOV. AOV blend overridden to `GL_ONE / GL_ZERO` (replace) via `glBlendFunci` so values don't accumulate across overlapping fragments.
- **Particle AOVs** — Normal/UV/Pref/Velocity work for all four particle modes (Point/Disc/Sphere/Sprite + motion-blur stretch variants). Per-vertex AOV defaults set via a `fillAovs` lambda (normal = +fwd camera-facing, pref = particle world pos, velocity = per-frame displacement). Sphere static overrides normal/pref with real per-vertex sphere values; Sprite static overrides UVs with quad-corner layout. Depth/WorldPos for particles is by-design only contributed by static Sphere (other modes disable depth writes for translucency). Motion-blur stretch is documented as beauty-only — for AOV-correct motion blur use Motion Samples > 1.
- **Shading modes** — new `Shading Mode` knob on ScanlineRender (default Shaded). Shaded does per-pixel N.L diffuse + 0.15 ambient against a `Light3D` if connected, otherwise a camera-relative headlight (Maya default convention). Flat preserves the legacy unlit behavior. Wireframe renders solid white GL_LINES derived from triangle indices.
- **3D viewport shading parity** — `Viewport3D` adds `eFlat` to `ShadingMode` enum (legacy unlit). `eShaded` + `eShadedWire` now compute per-face flat N.L using averaged per-vertex normals on primitives (Sphere/Card/Cube/Cylinder — winding-agnostic) and cross-product face normals on ReadGeo/Alembic (CCW-from-outside assumption). Light is camera-locked top-right-eye, orbits with viewer.

### Completed (2026-04-08)

- **ReadVDB OpenVDB support enabled** — `NATRON_HAVE_OPENVDB` defined under `NATRON_CYCLES` build. VDB grids load directly via `VDBImageLoader` (no dense conversion).
- **ReadVDB PrincipledVolume fire rendering** — density, temperature, flame, color grids bound to Cycles PrincipledVolumeNode. Absorption color, density/temperature remap curves (FloatCurveNode). Defaults dialed for EmberGen/Houdini fire VDBs.
- **ReadVDB viewport wireframe bbox** — dynamic bounds from `getVDBBounds()` instead of hardcoded cube. Cached per resolved frame path.
- **Volume3D rotation** — Rotate X/Y/Z knobs added, SceneGraph reads them into `buildTRS()`. Also added stepSize and volumeBounces knobs.
- **Cycles CPU device for NanoVDB** — explicit CPU device selection required for volume rendering. Without it, volumes render empty.

### TODO

- ReadGeo/ReadAlembicCamera require Alembic library (optional, not tested)
- ScanlineRender shadows not implemented
- Light3D shadow ray marching is functional but slow for dense volumes
- Volume shader workflow still WIP — needs more iteration on user-facing controls

---

## Particles

**Status: Phase 2 complete (13 nodes). Solver architecture refactored. Collision with geometry working.**

### Architecture (2026-04-06 refactor)

- **Force nodes are stateless** — `applyForce()` modifies velocities in-place, no cached positions. Viewing from a force node shows emitter positions with velocity tweaks (preview only, no accumulation).
- **ParticleSolver is the solver** — walks upstream with `collectUpstreamForces()` to find the emitter and all force nodes. Runs the single simulation loop each frame: spawn → forces → integrate → collide → expire. Only node that owns position/velocity state.
- **Works without geo** — if no geometry is connected to the `geo` input, ParticleSolver acts as a pure solver (forces + integration, no collision). Always put ParticleSolver at the end of the chain.
- **OBB collision** — reads rotateX/Y/Z from connected Cube3D, transforms particles into local space for AABB test, transforms back. Standard technique (Unity/Unreal/Blender).
- This matches the standard pattern used by Blender, Unity, Unreal, and Houdini. See `Research_Particle_Sim_Loop.md`.

### Known Issues

- **Force preview is velocity-only** — viewing from a force node (e.g. Gravity) shows emitter positions, not accumulated force positions. Must view from ParticleSolver for proper simulation.
- **Node renamed** — ParticleCollide → ParticleSolver (2026-04-06). Plugin ID is now `ParticleSolver`.
- **Animated colliders snap per-frame, not per-substep** — `applyCollision` is called per substep but with frame-level time. A keyframed-rotating Cube3D collider effectively uses the frame-N pose for all substeps. Visible as "popping" only on fast-rotating colliders. Fix would be `applyCollision(p, frame + sub/numSubsteps, dt)` plus per-substep knob caching. Low priority.
- **Per-particle knob reads in `applyCollision`** — 10 knob lookups per particle per substep per frame on the collision geo. At 10k particles × 4 substeps = 400k knob reads/frame. Should hoist the reads outside the inner loop. Perf, not correctness.

### Completed (2026-04-06)

- **Solver architecture refactor** — force nodes stateless, ParticleSolver owns simulation loop. Bounced velocity persists correctly (particles roll off surfaces during playback).
- **OBB collision** — rotation support for Cube3D colliders (inverse-transform to local space)
- **Show Collisions debug** — checkbox tints collided particles red
- **Removed built-in shape knobs** — no more Plane/Box/Sphere dropdown. Collision shape comes from connected Cube3D/Sphere3D geo.
- **Collision functions** — `bounceParticle`, `rayAABB`, `collideGeoBox`, `collideGeoSphere`, `pushOutOfBox`, `pushOutOfSphere`
- **`prevPx/prevPy/prevPz`** on Particle struct, `size` knob fix
- **Shiboken crash recovery**
- **ParticleCollide → ParticleSolver rename** — node, class, plugin ID, all references
- **ParticleSpawn "On Collision" working** — Emitter → Gravity → Solver (Cube3D geo) → Spawn (On Collision) → Merge pipeline tested and working
- **ReadAlembicTransform** — reads IXform (null/locator) from Alembic files. Dropdown to select transform, FPS knob, frame offset. Outputs animated translateX/Y/Z, rotateX/Y/Z, scaleX/Y/Z.
- **ParticleEmitter transform input** — new "transform" input (input 1). Connect ReadAlembicTransform directly — emitter position and emit direction automatically follow the null. No expression linking needed.
- **Substeps knob** on ParticleSolver (default 4, range 1-32)
- **Full particle node audit** — all 12 nodes clean, no issues found
- **3D viewport axis handle** for ReadAlembicTransform — RGB axis cross + yellow diamond at null position, rotates with animation, gizmo suppressed (read-only)

### Completed (2026-04-07)

- **ScanlineRender particle modes** — Point, Disc, Sphere, Sprite + Additive/Over blend + global scale knob
- **ScanlineRender 4x MSAA** — multisampled FBO + blit-to-resolve pattern for proper anti-aliasing
- **Multi-sample motion blur (ScanlineRender)** — physically-accurate via sub-frame accumulation. Works on all particle modes and instances. Samples + Shutter knobs. Stretch cheat mode is mutually exclusive.
- **ParticleInstance** — instances Cube3D/Sphere3D geo at particle positions. Working in both ScanlineRender and CyclesRender.
- **Cycles particle instancing** — native Cycles instancing (shared ccl::Mesh, one ccl::Object per instance). Motion blur via set_motion() with 3 transform steps extrapolated from velocity.
- **maxBounces** — bounceCount on Particle struct, kills particle when exceeded
- **Color variance** — per-particle random color variation on emitter (deterministic by particle ID)
- **Sphere/instance smoothing** — bumped sphere tessellation (16×24 instance, 10×14 particle mode), disc segments (32), added back-face culling for 3D geo

### Completed (2026-05-22) — Audit pass: A1 / A2 / A3 fixes

- **A1: Guarded `dynamic_cast` on collision-geo knob lookups** — `ParticleSolver::applyCollision` and `ParticleEmitter::getParticleData` (transform input path) previously checked the `KnobIPtr` was non-null but blindly dereferenced the `dynamic_cast<KnobDouble*>` result. Future node types exposing `translateX` / `rotateX` etc. as non-`KnobDouble` would null-deref. Replaced inline casts with a `readDouble()` local helper that guards both checks.
- **A2: Threaded `dt` through `bounceParticle` / collide helpers** — at `Substeps > 1` (default 4), the post-bounce continuation displacement (`p.vx * remaining`) was using the full-frame velocity instead of scaling by the substep size. Particles overshot the bounce point by ×4 at default substeps — visible as a "spring" effect on fresh bounces. `bounceParticle`, `collidePlane`, `collideGeoBox`, `collideGeoSphere`, and `ParticleSolver::applyCollision` now all take `dt` and apply `p.vx * remaining * dt` for the continuation.
- **A3: Stopped resyncing `p.life` from the emitter in solver step 6** — the per-frame appearance-sync loop overwrote `p.life` with the emitter's value, undoing the kill that `bounceParticle` set when a particle settled (`speed < 0.0001f` → `p.life = p.age`). Settled particles accumulated indefinitely on collider surfaces. Now sync only `r/g/b/a/size`; `life` is assigned at emission and stays put.

### Still TODO

- **Material system redesign** — see `Research_Material_System.md`. Current state is further along than expected:
  - `Material3D` already exists as authoring node with 5 texture inputs
  - Cube3D etc. already have `mat` input (input 1) for direct Material3D connection
  - CyclesRenderer uses `MaterialProvider` to build shader graphs
  - ScanlineRender does NOT consume materials (raw image textures only)
  - **Gaps:** ParticleInstance is hardcoded orange, no AssignMaterial node, no Cycles shader caching, ScanlineRender doesn't do PBR
  - **Recommended: Hybrid (Option C)** — direct input for simple cases + AssignMaterial node for scene-wide/pattern-based
  - **Phase 1:** `resolveMaterial()` helper + `MaterialBindingTable` on MaterialProvider.h
  - **Phase 2 (biggest win):** Extend ParticleInstance to 9 inputs (4 geo + 4 mat + 1 master override). Precedence: `masterOverride > pairedMat > geo->getConnectedMaterial() > geo inline knobs`. Kill the orange hardcode. Add per-material shader cache in Cycles.
  - **Phase 3:** `AssignMaterial` node (scene passthrough + material input + name pattern). Downstream overrides upstream.
  - **Phase 4:** Face groups / collections / ScanlineRender PBR (deferred)
- **Per-instance color in Cycles shader** — use `ObjectInfoNode` to pipe per-particle color from `obj->set_color()` into the Principled BSDF base color (partial fix for the hardcoded orange, before full material system)
- **Particle caching** — simulation re-runs on every frame scrub (should cache per-frame)
- **Color ramp UI** — pick a gradient of colors for per-particle random sampling (see `Research_Particle_Color_And_MotionBlur.md`)
- **ReadAlembicTransform animation path** — draw the motion trail in the viewport
- **Mesh collision** — per-triangle ray intersection for Alembic geo (requires ReadAlembicGeo)
- **Vertex selection in 3D viewport** — click on mesh vertex (see `Research_Vertex_Selection_3DViewport.md`)
- **ParticleExpression** — per-particle scripting for custom forces/behaviors

---

## 3D Viewport

ImGuizmo-based viewport with orbit camera, translate/rotate/scale gizmos (W/E/R keys), undo support, light-type icons. Toolbar exposes: gizmo space toggle (world/local), Grid show/hide, **View** dropdown (look through any Camera3D / ReadAlembicCamera), **Shading** dropdown (Wireframe / Shaded / Shaded+Wire), and (NATRON_CYCLES builds) a Render button.

### Known Issues (Fixed)
- ~~Point cloud persists after deleting DeepToPoints node~~ — Fixed
- ~~No undo/redo for gizmo transforms~~ — Fixed: GizmoTransformUndoCommand

### Completed (2026-05-21)
- **Look-through camera** — pick any CameraProvider node from the View dropdown to drive the viewport. While looking through a Camera3D, `Alt+Left` orbit / `Middle` pan / `Alt+Right` dolly write back to the camera's translate/rotate knobs (Alembic cameras are read-only and skip this).
- **Camera gate** — when looking through a camera, the 3D draw is letterboxed to the camera's sensor aspect so the wireframe matches the ScanlineRender output bit-perfect. A thin warm-orange outline marks the gate edge.
- **Shading dropdown** — Wireframe / Shaded / Shaded+Wire (default). Meshes render as solid grey + edges by default. Shapes carrying textures (Sphere3D/Cube3D/Card3D/Cylinder3D) still display them in Shaded / Shaded+Wire; Wireframe suppresses textures for a clean schematic view.

### TODO
- Selection highlight could be more visible
- Performance with very large point clouds (>10M points) untested
- Tumble-pivot indicator (small dot at orbit centre while orbiting) — Maya/Blender convention
- "Show Camera Gate" toggle to hide the orange outline when not wanted

---

## Cycles Renderer

CyclesRender node with path tracing, PBR materials, lights, animation. Optional build with `-DNATRON_CYCLES=ON`.

### Files
- `Engine/Dev/Cycles/CyclesRender.h/cpp` — Natron node interface
- `Engine/Dev/Cycles/CyclesRenderer.h/cpp` — Cycles bridge (scene sync, materials, lights, volumes)
- `patches/cycles-mingw.patch` — 2 MinGW fixes for upstream Cycles source
- `App/CMakeLists.txt` — Cycles library linking (10 Cycles libs + 17 deps)

### Completed (2026-04-08)

- **PrincipledVolume VDB rendering** — ReadVDB grids loaded via `VDBImageLoader`, bound to Cycles standard attributes (density/temperature/flame/color). PrincipledVolumeNode with ValueNode density (never `set_density()` — blocks attribute reads). FloatCurveNode remap for density and temperature.
- **Volume3D procedural shader** — sphere/box shapes via Cycles shader graph (TextureCoordinate → shape math → ScatterVolume + AbsorptionVolume). No dense ccl::Volume needed.
- **Explicit CPU device** — required for NanoVDB volume support. Set automatically in session creation.

### TODO
- GPU rendering (CUDA/OptiX) — not built (see Linux follow-ups in My TODO; Windows needs an MSVC kernel step)
- ~~Deep EXR output~~ — DONE 2026-07-26 (Blender PR #154410 port, see My TODO Done)
- Viewport rendered mode (live progressive) — future phase
- Volume shader workflow WIP — absorption/scatter/blackbody/remap exposure needs iteration

### Known Issues

- **Morph-target (shape-key / blend-shape) assets render blank in Cycles — DISTANCE-DEPENDENT** —
  isolated 2026-10-17, deferred by user decision (do NOT patch Cycles for now — this is the
  handoff note for whoever picks the Cycles side up). Known state after the 10-17 bisect
  (GLB surgery via `make_glb_variants*.py`, probes `readgeo_glbvar*_probe.py`,
  `readgeo_ab_probe.py`, `readgeo_dist_probe.py`, `readgeo_caminside_probe.py`):
  - AnimatedMorphCube.glb ("k") renders and animates correctly in the 3D viewport (GL) AND via
    ScanlineRender — the ReadGeo mesh data is correct end-to-end. Cycles is the only consumer
    that loses it.
  - **Distance-dependent**: k renders with the camera at <=1.5 m (inside / near-wall views
    confirmed by pixels), ZERO pixels at 6 m — on independent fresh rigs. Non-morph cubes at
    the same distance render fine.
  - The ccl scene holds the k object with sane bytes after sync (NATRON_DEBUG_SC scene walk:
    24 verts +-1, no NaN, bbox sane, 12 triangles in-range, identity tfm, vis=0xFFFFFFFF, pass
    executes, CPU device): the triangles never intersect rays from any angle while sitting in
    the scene graph. The poison is inside Cycles' geometry/BVH/device processing — not visible
    from Natron's sync walk.
  - Ruled out one-by-one (variant still black unless noted): TANGENT attr, morph targets +
    weights animation themselves, material, POSITION accessor min/max, Reverse Normals,
    winding + the w=0 rotation quat (proper rotation), small-verts x big-scale numerics,
    the face-color shader chain (`NATRON_GEOIO_NO_VCOL=1` still black), and session/init races
    (same-process A/B with BoxAnimated: box renders, k does not).
  - Next steps for the Cycles side: (a) instrument Cycles' geometry/BVH path — dump
    Mesh::triangles post-update, or compare a working mesh attribute-by-attribute at the ccl
    level; (b) test another Khronos morph sample, or a hand-rebuilt cube with k's exact values
    but morphcube's arrangement; (c) the close-camera repro (<=1.5 m) is a useful knob.
  - Note for rig authors: `readgeo_dist_probe.py` also showed that camera knob changes AFTER a
    render don't re-sync in CyclesRender — create cameras at their final position.
  - UPDATE 2026-10-18: user GUI pass with the rebuilt exe — AnimatedMorphCube.glb now renders
    in 3D viewport, ScanlineRender AND CyclesRender; the morph-weight override path is ALSO
    user-verified (Use File Morph Weights off + dragging the weight knobs, both nodes, all
    three render paths). The earlier "always blank" observation was confounded by a stale
    staged exe (the session's recurring 06-install gotcha); the distance-dependence reported
    on 10-17 may still be real but is unconfirmed against the current build. If a blank Cycles
    render of a morph asset reappears, re-check camera distance + exe freshness first, then
    this entry applies as written.

- **Two Cycles render-path defects (GUI sessions, isolated 2026-10-21 while verifying the
  glTF ORM/normal material wiring on the cobra asset)** — probes in
  `tools/geoio-tests/mcmp_cycles_*.py`:
  - **Writer/`renderBlocking` path renders pure black** with a correctly connected scene
    (Scene3D → CyclesRender → Writer; scene, light, camera verified by the viewer path
    rendering the same graph). PNG comes out 0.00 mean luma, no stderr errors. This is the
    long-standing "zero-sample flood" — reproduce headless with `NatronRenderer` too.
    The interactive **viewer** path renders the same graph fine, so the defect is in the
    offline render/batch path (CyclesPassRender / multipass tile handling), not in scene sync
    or materials. Trail: `CyclesRenderer.cpp` multipass ~3088, driver tiles ~140-190,
    `CyclesPassRender.cpp` ~247. UPDATE 2026-10-22: the mixamo writer path with the CURRENT
    staged engine produces a normal textured PNG (`writer_sc2.py` → `wr_sc.png`) using
    `app.createWriter(path)` + two wired Light3D + camera at render time — so the generic
    writer path is fine; re-confirmed late same day after a FULL rebuild + fresh 06-install
    (`arc_single.py` → `arc_f249.png` / 256 spp, no black flood). The earlier "black" results
    tonight were a stale `Natron-install\Renderer` staging artifact — see the mixamo entry at
    the top of this file. If the cobra repro still goes black, the defect is in that graph's
    specifics, not the generic writer path.
  - **Viewer Cycles engine goes stale after its first render** — subsequent scene/knob changes
    (tree swaps, light changes, material edits) and even Viewer input disconnect/reconnect do
    not restart the render; the pane keeps showing the first completed frame. Creating a NEW
    Viewer node connected to the CyclesRender node renders fresh (the workaround used for the
    material A/B/C verification). Likely a missing invalidation/re-render trigger from graph
    changes into the viewer-side Cycles session.

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
