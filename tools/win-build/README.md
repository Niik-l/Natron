# Natron Windows build scripts (MSYS2)

`tools/win-build/` — push-button build of Natron RB-2.6 (Qt6 + Cycles + OFX plugins +
PyPlugs) into a clean, relocatable install folder. Run everything in the **MSYS2 MINGW64**
shell. (Distinct from `tools/jenkins/` and `tools/buildmaster/`, which are the Linux/CI
`build-natron.sh` scripts.)

> **`BUILDING.md` is the canonical reference.** These scripts are derived from it — every
> command, patch, and version pin in here should match what `BUILDING.md` documents. If the
> two ever disagree, **trust `BUILDING.md`** and fix the scripts. The scripts are convenience
> automation; the manual guide is the source of truth.

## Before you start (first time only)

1. **Install MSYS2** from https://www.msys2.org/ — accept the defaults (installs to `C:\msys64`).
2. **Open the *MINGW64* terminal.** From the Start menu choose **"MSYS2 MINGW64"** — not
   "MSYS2 MSYS" or "UCRT64". The prompt must show **MINGW64** in purple. (The build fails in
   the other shells.)
3. **Get the code** (need ~15 GB free on the build drive). In the MINGW64 shell:
   ```bash
   pacman -S --needed git          # if git isn't installed yet
   cd /d                           # pick a drive: /c = C:, /d = D:, ...
   git clone --branch RB-2.6 https://github.com/Niik-l/Natron.git
   cd Natron/tools/win-build
   ```

You don't edit any paths — `NATRON_ROOT` auto-detects from where you cloned.

## Quick start

From `Natron/tools/win-build` in the MINGW64 shell:

```bash
bash build-all.sh
```

This does everything: installs dependencies, clones the helper repos next to Natron, applies
patches, builds Cycles + Natron + plugins, and stages a ready-to-run install. Time:
**~30 min on a fast multi-core machine with dependencies already installed; 1–2 hours on a
cold setup** (first-time `pacman` downloads of Qt6/Boost/etc. on slower hardware). A clean run
leaves **~14 GB on the build drive** (the install you keep is **~6 GB**; the rest is the
build tree + sources, deletable afterward — see "Cleanup" below). `WITH_CYCLES=0` is well
under half that. When it finishes it prints:

```
Launch: .../Natron-install/App/Natron.exe     ← double-click this
```

**Only optional tweak:** to skip the long Cycles build, set `WITH_CYCLES=0` in `config.sh`
first (`nano config.sh`; Ctrl+O saves, Ctrl+X exits).

**If a phase fails**, it stops with a red message naming the phase. Fix the cause and re-run
`bash build-all.sh` — finished phases are skipped, so it resumes where it stopped.

## Phases (each idempotent and individually runnable)

| Phase | Does |
|---|---|
| `00-deps.sh`    | `pacman -Syu` + install all deps (`--needed`, so a no-op if present) |
| `01-clone.sh`   | clone Natron (branch tip) + cycles/openfx-misc/openfx-io/natron-plugins (pinned) |
| `02-patch.sh`   | apply `patches/*.patch` (openfx) + Cycles seds (FindTBB, M_PI) |
| `03-cycles.sh`  | configure + build Cycles (skipped if `WITH_CYCLES=0`) |
| `04-natron.sh`  | configure (Cycles flags conditional) + build Natron |
| `05-plugins.sh` | build openfx-misc (Misc.ofx, CImg.ofx) + openfx-io (IO.ofx) |
| `06-install.sh` | stage a clean `$INSTALL_DIR`: exes + bundled DLLs + Python + plugins + PyPlugs + OCIO |
| `07-verify.sh`  | launch the staged binary headless with a clean PATH; assert version + plugins + Cycles |

`build-all.sh` runs 00→07, tee-ing each phase to `logs/<phase>.log`, and stops loudly on failure.

## After building — two folders, two purposes

A finished build leaves **two copies of `Natron.exe` / `NatronRenderer.exe`**:

| Folder | Role | Who needs it |
|---|---|---|
| `Natron/build-qt6/` | **develop + run here.** Edit code, `mingw32-make`, launch `build-qt6/App/Natron.exe` from the MINGW64 shell. Incremental rebuilds = seconds to minutes. | developers |
| `Natron-install/` | **shippable, relocatable bundle.** A deliberate self-contained copy with DLLs + Python + plugins staged in. Phase 07 proves it launches with a clean PATH. | distribution / non-dev users / standalone QA |

They're **different deliverables, not redundant builds.** For a developer iterating on code,
`Natron-install/` is the deployment artifact and not needed for day-to-day work — you can
delete it and re-stage when you next want a clean bundle.

## Cleanup — reclaim disk after a successful build

After a clean script run the drive holds ~14 GB. Only `Natron-install/` (~6 GB) is needed to
*run* Natron — phase 07 proves it's self-contained.

**If you only want to run Natron and won't touch the code** — delete everything except
`Natron-install/` (reclaims ~8 GB):

```bash
rm -rf Natron/build-qt6 cycles openfx-misc openfx-io natron-plugins
# Or even the whole source checkout if you keep Natron-install/ elsewhere:
# rm -rf Natron
```

`Natron-install/` can be moved, copied to another drive, or zipped and run on another machine.

**If you plan to edit the code** — keep `Natron/build-qt6/`. It enables fast incremental
rebuilds (`mingw32-make` recompiles just what changed, seconds to minutes, vs. ~30 min from
scratch). After rebuilding, re-run `06-install.sh` to re-stage the new exes into
`Natron-install/`. If you later delete `build-qt6/` and want to develop again, regenerate it
with `cmake .. -G "MinGW Makefiles"` (the first rebuild is then a full one).

> **Why the duplicate?** `06-install.sh` *copies* `Natron.exe` / `NatronRenderer.exe` from
> `build-qt6/` into `Natron-install/` and stages DLLs + Python + plugins around them — that's
> the relocatable bundle. The duplicate (~2 GB) is the cost of having a shippable copy
> separate from the dev build tree.

## config.sh knobs

- `NATRON_ROOT` — parent dir for sources + builds
- `INSTALL_DIR` — clean deployable output (default `$NATRON_ROOT/Natron-install`)
- `WITH_CYCLES` — `1` full Cycles build / `0` skip (faster, no CyclesRender node)
- `STAGE_RENDERER` — `1` also stage standalone `NatronRenderer.exe`
- `JOBS` / `NATRON_JOBS` — parallelism (`auto`=nproc; Natron capped at 8 for moc safety)
- Pinned dep refs (`CYCLES_REF`, `OPENFX_*_REF`) — keep the patches applying

## Patches

`patches/openfx-io.patch` and `patches/openfx-misc.patch` are git diffs against the
pinned refs (OIIO 3.x / FFmpeg 8.x fixes; empty `CMAKE_SYSTEM_PROCESSOR` fix). Cycles
fixes are `sed`s in `02-patch.sh`, mirroring BUILDING.md.

**If a patch ever stops applying** (you bumped a pinned ref and upstream moved the lines),
02 fails loudly. Regenerate from a hand-fixed tree:

```bash
git -C $NATRON_ROOT/openfx-io diff > tools/win-build/patches/openfx-io.patch
```

## Notes / gotchas baked in

- Runs under **MSYS2 git** (mingw64 on PATH). Idempotency for `cycles-mingw.patch` uses a
  content sentinel, not `git apply --reverse --check` (that check differs across git builds).
- `pacman -Syu` may demand you reopen the MINGW64 terminal; `00-deps.sh` warns if so — reopen and re-run it.
- Cycles statically links into Natron, so the exes are large (debug info); the install
  folder is ~6 GB with Cycles, far less without (the build tree is ~9 GB on top of that).

## Tested (2026-05-25)

Validated live: patches apply to pristine source under both Git-for-Windows and MSYS2 git;
`02-patch` idempotent; `06-install` stages a correct 6 GB folder; `07-verify` passes
standalone (Natron 2.6, 539 plugins, CyclesRender, openfx-io/CImg, qtpy OK). Build phases
00/01/03/04/05 are the exact commands from the verified 2026-05-25 manual build, wrapped
with config + logging.
