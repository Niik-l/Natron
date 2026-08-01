#!/usr/bin/env bash
# Run every phase in order. Edit config.sh first. Run in the MSYS2 MINGW64 shell.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/config.sh"

PHASES=(00-deps 01-clone 02-patch 03-cycles 04-natron 05-plugins 06-install 07-verify)
mkdir -p "$HERE/logs"
START=$(date +%s)
for ph in "${PHASES[@]}"; do
  printf '\n\033[1;35m==================== %s ====================\033[0m\n' "$ph"
  if ! bash "$HERE/$ph.sh" 2>&1 | tee "$HERE/logs/$ph.log"; then
    printf '\n\033[1;31mFAILED at %s — see %s/logs/%s.log. Fix, then re-run ./%s.sh (phases are idempotent) or ./build-all.sh.\033[0m\n' "$ph" "$HERE" "$ph" "$ph"
    exit 1
  fi
done
printf '\n\033[1;32mALL PHASES COMPLETE in %ss. Install: %s\033[0m\n' "$(( $(date +%s) - START ))" "$INSTALL_DIR"
echo "Launch: $INSTALL_DIR/App/Natron.exe   (the install is self-contained — NOT build-qt6/App)"
echo "Optional: ./08-cleanup.sh deletes the source/build trees (~10 GB) once you're happy."
