# =====================================================================
# win-build — configuration.  EDIT THIS FILE, then run ./build-all.sh
# (run everything in the MSYS2 *MINGW64* shell)
# =====================================================================

# Parent dir holding your Natron checkout + sibling builds (cycles/, openfx-*/, etc.).
# Auto-detected from this file's location: ../../.. = the folder that contains Natron/.
# Advanced: override by exporting NATRON_ROOT before running.
_CFG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATRON_ROOT="${NATRON_ROOT:-$(cd "$_CFG_DIR/../../.." && pwd)}"

# Clean, self-contained, relocatable install folder (built fresh from outputs).
INSTALL_DIR="$NATRON_ROOT/Natron-install"

# 1 = build Cycles path tracer + CyclesRender node;  0 = skip Cycles entirely.
WITH_CYCLES=1

# 1 = also stage a standalone NatronRenderer.exe;  0 = GUI only.
STAGE_RENDERER=1

# Parallel jobs.  "auto" = nproc.  Natron's moc/autogen phase is capped (see lib.sh).
JOBS="auto"
NATRON_JOBS=""          # leave empty to derive from JOBS (capped at 8 for moc safety)

# ---- Source refs -----------------------------------------------------
# Natron tracks the branch tip so a re-run picks up the latest RB-2.6.
NATRON_BRANCH="RB-2.6"
# Dependencies are PINNED to the 2026-05-25 known-good build so the patches apply.
# Niik-l fork branch, NOT an upstream tag: Natron's CyclesRenderer requires the
# deep-EXR output API (deep_output_driver.h etc.) that only exists there
# (= v5.0.0 + deep port 787be0655 + MinGW fixes committed).
CYCLES_REF="feature/cycles-deep"
OPENFX_MISC_REF="0abd46b5a8cbc98fa24579042129460d0aa87b8f"
OPENFX_IO_REF="31ebb488d0b4aec52e92ec94cdc30da277091d30"
# natron-plugins is content only (no patch); tracks tip. Built-with: b0c499fb6391024f54be9f26ed41b5cf7475d574

# ---- Toolchain location ---------------------------------------------
MINGW="/c/msys64/mingw64"
