#!/usr/bin/env bash
# Phase 03 — configure + build Cycles standalone (skipped when WITH_CYCLES=0).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"

if [ "${WITH_CYCLES:-1}" != "1" ]; then ok "WITH_CYCLES=0 — skipping Cycles build"; exit 0; fi
require_mingw
cd "$NATRON_ROOT/cycles"; mkdir -p build && cd build

# NVIDIA GPU (WITH_CUDA=1): the CUDA device loads the driver at runtime (cuew), the kernels are
# compiled ahead for CUDA_ARCHS by nvcc, which needs MSVC's environment.
CUDA_FLAGS=(-DWITH_CYCLES_DEVICE_CUDA=OFF -DWITH_CYCLES_CUDA_BINARIES=OFF)
if [ "${WITH_CUDA:-0}" = "1" ]; then
  [ -x "$CUDA_DIR/bin/nvcc.exe" ] || die "WITH_CUDA=1 but no nvcc at $CUDA_DIR (install the CUDA Toolkit or fix CUDA_DIR)"
  msvc_env
  CUDA_M="$(cygpath -m "$CUDA_DIR")"
  CUDA_FLAGS=(-DWITH_CYCLES_DEVICE_CUDA=ON -DWITH_CUDA_DYNLOAD=ON -DWITH_CYCLES_CUDA_BINARIES=ON
              "-DCYCLES_CUDA_BINARIES_ARCH=$CUDA_ARCHS" "-DCUDA_TOOLKIT_ROOT_DIR=$CUDA_M"
              "-DCUDA_NVCC_EXECUTABLE=$CUDA_M/bin/nvcc.exe" "-DCUDA_HOST_COMPILER=$CL_EXE")
  ok "CUDA: $CUDA_M, kernels $CUDA_ARCHS, host compiler $CL_EXE"
fi

log "Configuring Cycles"
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_CYCLES_STANDALONE=ON -DWITH_CYCLES_STANDALONE_GUI=OFF \
  -DWITH_CYCLES_USD=OFF -DWITH_USD=OFF -DWITH_CYCLES_OSL=OFF \
  "${CUDA_FLAGS[@]}" -DWITH_CYCLES_DEVICE_OPTIX=OFF \
  -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_DEVICE_ONEAPI=OFF \
  -DWITH_CYCLES_EMBREE=ON -DWITH_CYCLES_OPENIMAGEDENOISE=ON \
  -DWITH_CYCLES_OPENCOLORIO=ON -DWITH_CYCLES_OPENSUBDIV=ON \
  -DWITH_CYCLES_OPENVDB=ON -DWITH_CYCLES_ALEMBIC=OFF \
  -DWITH_CYCLES_LOGGING=OFF -DWITH_LIBS_PRECOMPILED=OFF \
  -DTBB_ROOT_DIR=/c/msys64/mingw64

log "Building Cycles (-j$(jobs_n)) — this is the long one"
mingw32-make -j"$(jobs_n)"
[ -f lib/libcycles_subd.a ] || die "Cycles build incomplete (libcycles_subd.a missing — check the M_PI patch)"
if [ "${WITH_CUDA:-0}" = "1" ]; then
  n="$(ls src/kernel/kernel_*.zst 2>/dev/null | wc -l)"
  [ "$n" -gt 0 ] || die "WITH_CUDA=1 but no GPU kernels in build/src/kernel (see the nvcc output above)"
  ok "CUDA kernels: $n"
fi
ok "Phase 03 (cycles) complete"
