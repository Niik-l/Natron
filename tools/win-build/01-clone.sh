#!/usr/bin/env bash
# Phase 01 — clone Natron (branch tip) + dependencies (pinned). Idempotent: skips existing clones.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw
mkdir -p "$NATRON_ROOT"; cd "$NATRON_ROOT"

# clone_at <url> <dir> <ref>   (full clone, then checkout pinned ref)
clone_at() {
  local url="$1" dir="$2" ref="$3"
  if [ -d "$dir/.git" ]; then ok "$dir already present (skipping clone)"; return; fi
  log "Cloning $dir @ $ref"
  git clone "$url" "$dir"
  git -C "$dir" checkout -q "$ref"
}

# Natron — track the branch tip (a re-run picks up latest RB-2.6).
if [ -d Natron/.git ]; then ok "Natron already present (skipping clone)"
else log "Cloning Natron (branch $NATRON_BRANCH)"; git clone --branch "$NATRON_BRANCH" https://github.com/Niik-l/Natron.git Natron; fi
log "Natron submodules"; git -C Natron submodule update --init --recursive

if [ "${WITH_CYCLES:-1}" = "1" ]; then
  if [ -d cycles/.git ]; then ok "cycles already present (skipping clone)"
  else log "Cloning cycles @ $CYCLES_REF (shallow)"; git clone --branch "$CYCLES_REF" --depth 1 https://github.com/Niik-l/cycles.git cycles; fi
fi

clone_at https://github.com/NatronGitHub/openfx-misc.git openfx-misc "$OPENFX_MISC_REF"
log "openfx-misc submodules"; git -C openfx-misc submodule update --init --recursive
clone_at https://github.com/NatronGitHub/openfx-io.git   openfx-io   "$OPENFX_IO_REF"
log "openfx-io submodules";  git -C openfx-io  submodule update --init --recursive

if [ -d natron-plugins/.git ]; then ok "natron-plugins already present (skipping clone)"
else log "Cloning natron-plugins (tip, shallow)"; git clone --depth 1 https://github.com/NatronGitHub/natron-plugins.git natron-plugins; fi

ok "Phase 01 (clone) complete"
