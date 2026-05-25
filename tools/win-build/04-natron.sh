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
  "${CYCLES_FLAGS[@]}"

log "Building Natron (-j$(natron_jobs_n)) — moc-capped for stability"
mingw32-make -j"$(natron_jobs_n)"
[ -f App/Natron.exe ] || die "Natron build incomplete (App/Natron.exe missing)"
ok "Phase 04 (natron) complete"
