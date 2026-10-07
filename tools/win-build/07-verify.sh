#!/usr/bin/env bash
# Phase 07 — verify the STAGED install runs standalone (clean PATH) and loads all plugins.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw
I="$INSTALL_DIR"
SCRIPT="$HERE/verify_natron.py"
[ -f "$SCRIPT" ] || die "verify_natron.py not found next to the scripts"
export NATRON_PYPLUGS_DIR="$I/Plugins/PyPlugs"   # verify_natron.py checks every PyPlug's node IDs exist

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
grep -q "net.fxarena.openfx.ReadSVG" <<<"$out" || die "Arena.ofx not loaded (ReadSVG missing)"
# SeExpr/SeExprSimple/SeNoise/SeGrain are built into IO.ofx only when openfx-io finds the
# SeExpr 2.11 library (MSYS2 package mingw-w64-x86_64-seexpr); 35 bundled PyPlugs need them.
grep -q "fr.inria.openfx.SeExprSimple" <<<"$out" || die "IO.ofx was built without SeExpr (SeExprSimple missing): is mingw-w64-x86_64-seexpr installed?"
grep -q "net.sf.openfx.SeNoise" <<<"$out" || die "IO.ofx was built without SeNoise"
ok "OFX plugins loaded (openfx-io + CImg + Arena + SeExpr nodes)"

# PyPlug dependencies: every node ID the bundled PyPlugs create must exist. Known exception:
# OpenFX.Yo.ResolveMath is a third-party plugin upstream does not bundle either (2 PyPlugs).
missing_deps="$(grep "RESULT PYPLUG_MISSING" <<<"$out" | grep -v "OpenFX.Yo.ResolveMath" || true)"
if [ -n "$missing_deps" ]; then
  echo "$missing_deps"
  die "bundled PyPlugs reference plugins this install does not have (see above)"
fi
ok "PyPlug dependencies satisfied ($(grep -o 'RESULT PYPLUG_DEPS checked=[0-9]*' <<<"$out" | cut -d= -f2) node IDs)"

# Qt image-format plugins (imageformats/qjpeg.dll etc.): jpg, tif, webp must be writable.
fmts="$(grep "RESULT QT_IMAGE_WRITE_FORMATS:" <<<"$out" || true)"
for f in jpg tiff webp; do
  grep -qw "$f" <<<"$fmts" || die "Qt image format '$f' missing (imageformats/ plugins not installed): $fmts"
done
ok "Qt image-format plugins present (jpg, tiff, webp)"

if grep -qi "Failed to import qtpy" <<<"$out"; then warn "qtpy import warning present (check DLL bundling in App/ and Renderer/)"
else ok "qtpy OK"; fi
ok "Phase 07 (verify) complete — install at $I is good"
