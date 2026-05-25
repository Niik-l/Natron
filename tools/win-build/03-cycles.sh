#!/usr/bin/env bash
# Phase 03 — configure + build Cycles standalone (skipped when WITH_CYCLES=0).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"

if [ "${WITH_CYCLES:-1}" != "1" ]; then ok "WITH_CYCLES=0 — skipping Cycles build"; exit 0; fi
require_mingw
cd "$NATRON_ROOT/cycles"; mkdir -p build && cd build

log "Configuring Cycles"
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_CYCLES_STANDALONE=ON -DWITH_CYCLES_STANDALONE_GUI=OFF \
  -DWITH_CYCLES_USD=OFF -DWITH_USD=OFF -DWITH_CYCLES_OSL=OFF \
  -DWITH_CYCLES_DEVICE_CUDA=OFF -DWITH_CYCLES_DEVICE_OPTIX=OFF \
  -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_DEVICE_ONEAPI=OFF \
  -DWITH_CYCLES_EMBREE=ON -DWITH_CYCLES_OPENIMAGEDENOISE=ON \
  -DWITH_CYCLES_OPENCOLORIO=ON -DWITH_CYCLES_OPENSUBDIV=ON \
  -DWITH_CYCLES_OPENVDB=ON -DWITH_CYCLES_ALEMBIC=OFF \
  -DWITH_CYCLES_LOGGING=OFF -DWITH_LIBS_PRECOMPILED=OFF \
  -DTBB_ROOT_DIR=/c/msys64/mingw64

log "Building Cycles (-j$(jobs_n)) — this is the long one"
mingw32-make -j"$(jobs_n)"
[ -f lib/libcycles_subd.a ] || die "Cycles build incomplete (libcycles_subd.a missing — check the M_PI patch)"
ok "Phase 03 (cycles) complete"
