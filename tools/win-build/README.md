# Natron Windows build scripts (MSYS2)

`tools/win-build/` — push-button build of Natron RB-2.6 (Qt6 + Cycles + OFX plugins +
PyPlugs) into a clean, relocatable install folder. Run everything in the **MSYS2 MINGW64**
shell. (Distinct from `tools/jenkins/` and `tools/buildmaster/`, which are the Linux/CI
`build-natron.sh` scripts.)

> **`BUILDING.md` is the canonical reference.** These scripts are derived from it — every
> command, patch, and version pin in here should match what `BUILDING.md` documents. If the
> two ever disagree, **trust `BUILDING.md`** and fix the scripts. The scripts are convenience
> automation; the manual guide is the source of truth.

## Quick start

```bash
# 1. edit config.sh  (at minimum: NATRON_ROOT, INSTALL_DIR, WITH_CYCLES)
# 2. run it all:
./build-all.sh
# ...or run/resume a single phase:
./04-natron.sh
```

On success the install is at `$INSTALL_DIR` (default `$NATRON_ROOT/Natron-install`).
Launch `$INSTALL_DIR/App/Natron.exe` (double-clickable — DLLs are bundled).

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
