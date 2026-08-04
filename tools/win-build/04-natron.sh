#!/usr/bin/env bash
# Phase 04 — configure + build Natron (Cycles flags added only when WITH_CYCLES=1).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw
cd "$NATRON_ROOT/Natron"; mkdir -p build-qt6 && cd build-qt6

CYCLES_FLAGS=()
if [ "${WITH_CYCLES:-1}" = "1" ]; then
  CYCLES_FLAGS=( -DNATRON_CYCLES=ON
                 -DNATRON_CYCLES_DIR="$NATRON_ROOT/cycles"
                 -DNATRON_CYCLES_BUILD_DIR="$NATRON_ROOT/cycles/build" )
fi

# FastVolumeRender (GPU VDB node) needs wgpu-native, which isn't an MSYS2
# package and isn't vendored (see BUILDING.md "FastVolumeRender"). Auto-enable
# when a wgpu/ dir with the headers + DLL sits next to the Natron checkout —
# without it the node is silently absent from the build (bit the 2026-08-02
# release).
FASTVOLUME_FLAGS=()
if [ -f "$NATRON_ROOT/wgpu/wgpu_native.dll" ] && [ -f "$NATRON_ROOT/wgpu/webgpu.h" ]; then
  FASTVOLUME_FLAGS=( -DNATRON_FASTVOLUME=ON -DNATRON_WGPU_DIR="$NATRON_ROOT/wgpu" )
  ok "FastVolumeRender enabled (wgpu at $NATRON_ROOT/wgpu)"
else
  warn "FastVolumeRender disabled — no $NATRON_ROOT/wgpu (headers + wgpu_native.dll). See BUILDING.md."
fi

log "Configuring Natron (Cycles: ${WITH_CYCLES:-1})"
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNATRON_QT6=ON -DNATRON_BUILD_TESTS=OFF \
  -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/mingw32-make.exe \
  -DPython3_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe \
  -DPython_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe \
  -DPython3_ROOT_DIR=/c/msys64/mingw64 -DPython_ROOT_DIR=/c/msys64/mingw64 \
  -DNATRON_LLVM_INSTALL_DIR=C:/msys64/mingw64 \
  "${CYCLES_FLAGS[@]}" "${FASTVOLUME_FLAGS[@]}"

log "Building Natron (-j$(natron_jobs_n)) — moc-capped for stability"
mingw32-make -j"$(natron_jobs_n)"
[ -f App/Natron.exe ] || die "Natron build incomplete (App/Natron.exe missing)"
ok "Phase 04 (natron) complete"
