#!/usr/bin/env bash
# Phase 08 (OPTIONAL, destructive) — reclaim ~10 GB by deleting the source and
# build trees after a verified install. Natron-install/ is fully self-contained
# (binaries, DLLs, Python stdlib, OFX plugins, PyPlugs and OCIO configs are all
# COPIES), so the clones are only needed again if you want to rebuild.
#
# NOT run by build-all.sh — invoke explicitly:  ./08-cleanup.sh [--yes]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"

I="$INSTALL_DIR"

# Refuse to delete the sources unless the staged install looks complete —
# a binary plus its runtime DLLs is the minimum proof 06-install ran through.
[ -f "$I/App/Natron.exe" ] && [ -f "$I/App/libgcc_s_seh-1.dll" ] \
  || die "install at $I looks incomplete — run ./build-all.sh (through 07-verify) first"

DOOMED=(Natron cycles openfx-misc openfx-io natron-plugins)

echo "About to DELETE these source/build trees under $NATRON_ROOT:"
FOUND=0
for d in "${DOOMED[@]}"; do
  [ -d "$NATRON_ROOT/$d" ] && { du -sh "$NATRON_ROOT/$d"; FOUND=1; }
done
[ "$FOUND" = "1" ] || die "nothing to clean — no source trees found under $NATRON_ROOT"
echo "Kept (self-contained): $I"
echo "To rebuild later you re-clone from scratch (./build-all.sh, ~40 min)."

if [ "${1:-}" != "--yes" ]; then
  read -r -p "Proceed? [y/N] " a
  case "$a" in y|Y|yes|YES) ;; *) die "aborted — nothing deleted";; esac
fi

# This script lives INSIDE Natron/ (one of the doomed trees) and Windows can't
# delete a file that's in use — so write a tiny worker at $NATRON_ROOT (outside
# every doomed tree) and exec it, then nothing running lives in what's deleted.
W="$NATRON_ROOT/.cleanup-worker.sh"
{
  printf 'set -u\ncd "%s" || exit 1\n' "$NATRON_ROOT"
  for d in "${DOOMED[@]}"; do
    printf 'rm -rf -- "%s" && echo "  deleted %s"\n' "$d" "$d"
  done
  printf 'echo "Cleanup done. Install: %s"\n' "$I"
  printf 'rm -f -- "$0" 2>/dev/null || true\n'
} > "$W"
exec bash "$W"
