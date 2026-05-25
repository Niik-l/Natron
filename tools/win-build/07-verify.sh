#!/usr/bin/env bash
# Phase 07 — verify the STAGED install runs standalone (clean PATH) and loads all plugins.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw
I="$INSTALL_DIR"
SCRIPT="$HERE/verify_natron.py"
[ -f "$SCRIPT" ] || die "verify_natron.py not found next to the scripts"

# Prefer the headless renderer; fall back to the GUI binary in interpreter mode.
if [ -f "$I/Renderer/NatronRenderer.exe" ]; then BIN_DIR="$I/Renderer"; BIN="NatronRenderer.exe"
elif [ -f "$I/App/Natron.exe" ];            then BIN_DIR="$I/App";      BIN="Natron.exe"
else die "no staged binary found in $I (run 06-install.sh first)"; fi

log "Verifying $BIN_DIR/$BIN with a CLEAN PATH (proves standalone — DLLs must come from the bundle)"
out="$(cd "$BIN_DIR" && PATH="/usr/bin:/c/Windows/System32:/c/Windows" \
      timeout 300 ./"$BIN" -t "$SCRIPT" </dev/null 2>&1 || true)"

# Print the signal lines. (Use here-strings below, NOT `echo | grep -q`: under
# `set -o pipefail`, grep -q exits early and SIGPIPEs echo -> false failure.)
grep -E "RESULT|Failed to import qtpy|DLL load failed" <<<"$out" || true

grep -q "RESULT DONE_OK" <<<"$out" || die "verification did not reach DONE_OK"
ok "engine + Python init OK (DONE_OK)"

if grep -q "fr.inria.built-in.CyclesRender" <<<"$out"; then ok "Cycles node present"
elif [ "${WITH_CYCLES:-1}" = "1" ]; then die "CyclesRender node missing (WITH_CYCLES=1)"
else ok "Cycles intentionally absent (WITH_CYCLES=0)"; fi

grep -q "WriteOIIO" <<<"$out" || die "openfx-io (WriteOIIO) not loaded"
grep -q "eu.cimg."  <<<"$out" || die "CImg.ofx not loaded"
ok "OFX plugins loaded (openfx-io + CImg)"

if grep -qi "Failed to import qtpy" <<<"$out"; then warn "qtpy import warning present (check DLL bundling in App/ and Renderer/)"
else ok "qtpy OK"; fi
ok "Phase 07 (verify) complete — install at $I is good"
