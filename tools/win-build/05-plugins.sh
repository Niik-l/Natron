#!/usr/bin/env bash
# Phase 05 — build OFX plugins: openfx-misc (Misc.ofx, CImg.ofx) and openfx-io (IO.ofx).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw

CMAKE_COMMON=( -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/mingw32-make.exe
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 )

log "openfx-misc: fetch pinned CImg.h"
( cd "$NATRON_ROOT/openfx-misc/CImg" && mingw32-make CImg.h )
log "openfx-misc: configure + build"
cd "$NATRON_ROOT/openfx-misc"; mkdir -p build && cd build
cmake .. "${CMAKE_COMMON[@]}"
mingw32-make -j"$(jobs_n)"
[ -f Misc.ofx ] && [ -f CImg.ofx ] || die "openfx-misc incomplete (Misc.ofx/CImg.ofx missing)"
ok "openfx-misc built (Misc.ofx, CImg.ofx)"

log "openfx-io: configure + build"
cd "$NATRON_ROOT/openfx-io"; mkdir -p build && cd build
cmake .. "${CMAKE_COMMON[@]}"
mingw32-make -j"$(jobs_n)"
[ -f IO.ofx ] || die "openfx-io incomplete (IO.ofx missing)"
ok "openfx-io built (IO.ofx)"
ok "Phase 05 (plugins) complete"
