#!/usr/bin/env bash
# Phase 00 — install MSYS2/MINGW64 dependencies (idempotent; --needed skips installed).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw

log "pacman -Syu (refresh + core update)"
pacman -Syu --noconfirm || warn "pacman -Syu returned non-zero — if it asked to CLOSE THE TERMINAL, reopen MINGW64 and re-run ./00-deps.sh before continuing."

log "Installing core build + runtime dependencies"
pacman -S --needed --noconfirm \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake git \
  mingw-w64-x86_64-qt6-base mingw-w64-x86_64-pyside6 mingw-w64-x86_64-shiboken6 \
  mingw-w64-x86_64-python mingw-w64-x86_64-python-qtpy mingw-w64-x86_64-boost \
  mingw-w64-x86_64-cairo mingw-w64-x86_64-expat mingw-w64-x86_64-openvdb \
  mingw-w64-x86_64-alembic \
  mingw-w64-x86_64-openimageio mingw-w64-x86_64-openexr mingw-w64-x86_64-opencolorio \
  mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-libraw mingw-w64-x86_64-libpng

if [ "${WITH_CYCLES:-1}" = "1" ]; then
  log "Installing Cycles dependencies"
  pacman -S --needed --noconfirm \
    mingw-w64-x86_64-embree mingw-w64-x86_64-openimagedenoise mingw-w64-x86_64-openpgl \
    mingw-w64-x86_64-opensubdiv mingw-w64-x86_64-pugixml mingw-w64-x86_64-libepoxy
fi
ok "Phase 00 (deps) complete"
