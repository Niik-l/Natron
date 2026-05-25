#!/usr/bin/env bash
# Phase 02 — apply MinGW/toolchain patches. Idempotent: skips already-applied patches.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw
PATCHES="$HERE/patches"

# apply_patch <repo-dir> <patch-file>
apply_patch() {
  local repo="$NATRON_ROOT/$1" pf="$2"
  if git -C "$repo" apply --reverse --check "$pf" 2>/dev/null; then ok "$1: patch already applied (skipping)"; return; fi
  git -C "$repo" apply --check "$pf" 2>/dev/null || die "$1: $(basename "$pf") does not apply cleanly — upstream likely moved. Regenerate it (see README) or pin the matching ref."
  git -C "$repo" apply "$pf"; ok "$1: applied $(basename "$pf")"
}

apply_patch openfx-io   "$PATCHES/openfx-io.patch"
apply_patch openfx-misc "$PATCHES/openfx-misc.patch"

if [ "${WITH_CYCLES:-1}" = "1" ]; then
  cd "$NATRON_ROOT/cycles"
  # (a) Natron's bundled MinGW patch.
  # Idempotency via a content sentinel rather than `git apply --reverse --check`:
  # that check is EOL/whitespace-sensitive and behaves differently across git
  # builds (MSYS2 git vs Git-for-Windows). The patch removes the
  # `${PYTHON_LIBRARIES}` line from macros.cmake — use its absence as the marker.
  CP="$NATRON_ROOT/Natron/patches/cycles-mingw.patch"
  if grep -q 'PYTHON_LIBRARIES' src/cmake/macros.cmake; then
    git apply "$CP"; ok "cycles: applied cycles-mingw.patch"
  else
    ok "cycles: mingw patch already applied"
  fi
  # (b) FindTBB: MSYS2 ships only libtbb12 (NAMES/tbb are on separate lines)
  if grep -q 'tbb tbb12' src/cmake/Modules/FindTBB.cmake; then ok "cycles: FindTBB already patched"
  else sed -i 's/^    tbb$/    tbb tbb12/' src/cmake/Modules/FindTBB.cmake; ok "cycles: FindTBB -> tbb12"; fi
  # (c) M_PI must be defined BEFORE add_subdirectory(src) so src/subd inherits it
  if grep -q '_USE_MATH_DEFINES' CMakeLists.txt; then ok "cycles: M_PI already patched"
  else sed -i '/^add_subdirectory(src)/i\
# Niik-l fork: ensure MinGW exposes M_PI for OpenSubdiv-using sources.\
if(WIN32)\
  add_compile_definitions(_USE_MATH_DEFINES)\
endif()\
' CMakeLists.txt; ok "cycles: M_PI inserted before add_subdirectory(src)"; fi
else
  ok "WITH_CYCLES=0 — skipping Cycles patches"
fi
ok "Phase 02 (patch) complete"
