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

# 1 = Cycles can also render on NVIDIA GPUs (CyclesRender Device = GPU (CUDA)); 0 = CPU only (the
# GPU choice then falls back to the CPU with a warning). Needs the CUDA Toolkit (13.x) and Visual
# Studio 2022's C++ tools on the build machine (nvcc uses cl.exe for the GPU kernels only); users
# need nothing but an NVIDIA driver, 580 or newer. Adds ~20 min to the Cycles build, ~35 MB.
WITH_CUDA=0
# GPU kernels: sm_75 GTX 16 / RTX 20, sm_86 RTX 30, sm_89 RTX 40, sm_120 RTX 50; compute_75 = PTX that
# newer cards compile on first use (needs a driver at least as new as the toolkit). CUDA 13 can't
# build sm_50..sm_70 (GTX 900 / 1000, Volta): those cards render on the CPU.
CUDA_ARCHS="sm_75;sm_86;sm_89;sm_120;compute_75"
CUDA_DIR="/c/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.4"
VCVARS='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'

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
# openfx-arena (Arena.ofx: ReadSVG, ReadPSD, ReadKrita, ReadCDR, ReadPDF, Text, Magick
# filters) — master @ 2026-07-16; patched to C++20 for MSYS2 poppler (patches/openfx-arena.patch).
OPENFX_ARENA_REF="49e4d5fc197a637196c1a7decab7f97751b867f6"
# nik-kit-natron (NikKit.ofx: this fork's own OpenFX tools, menu group "Nik Kit"; its pyplugs/
# ship under Plugins/PyPlugs/NikKit). No patch. https://github.com/Niik-l/nik-kit-natron
NIK_KIT_REF="692201dde7e891895044a7ed224ffe6e37c0c66a"
# natron-plugins is content only (no patch); tracks tip. Built-with: b0c499fb6391024f54be9f26ed41b5cf7475d574

# ---- Toolchain location ---------------------------------------------
MINGW="/c/msys64/mingw64"
