#!/usr/bin/env bash
# Phase 06 — assemble a clean, relocatable install into $INSTALL_DIR.
# Layout mirrors what Natron resolves relative to its binary:
#   App/Natron.exe + DLLs + platforms/ ;  lib/pythonX.Y ;  Plugins/ ;  Resources/
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw

B="$NATRON_ROOT/Natron/build-qt6"
I="$INSTALL_DIR"
PYV="$(py_ver)"   # e.g. 3.14
[ -f "$B/App/Natron.exe" ] || die "build output missing ($B/App/Natron.exe) — run 04-natron.sh first"

log "Staging install into $I (python $PYV)"
mkdir -p "$I/App/platforms" "$I/lib" \
         "$I/Plugins/OFX/Natron" "$I/Plugins/PyPlugs" "$I/Resources"

# --- GUI binary + bundled DLLs + Qt platform plugin ---
log "App: binary + DLLs"
cp "$B/App/Natron.exe" "$I/App/"
cp "$MINGW"/bin/*.dll "$I/App/"
cp "$MINGW/share/qt6/plugins/platforms/qwindows.dll" "$I/App/platforms/"

# --- Headless renderer (optional) ---
if [ "${STAGE_RENDERER:-1}" = "1" ]; then
  log "Renderer: binary + DLLs"
  mkdir -p "$I/Renderer/platforms"
  cp "$B/Renderer/NatronRenderer.exe" "$I/Renderer/"
  cp "$MINGW"/bin/*.dll "$I/Renderer/"
  cp "$MINGW/share/qt6/plugins/platforms/qwindows.dll" "$I/Renderer/platforms/"
fi

# --- Python standard library (+ PySide6, qtpy live in its site-packages) ---
log "Python stdlib (lib/python$PYV)"
rm -rf "$I/lib/python$PYV"
cp -r "$MINGW/lib/python$PYV" "$I/lib/python$PYV"

# --- OFX plugins into .ofx.bundle/Contents/Win64 ---
log "OFX plugins (Misc, CImg, IO)"
install_ofx() { # name srcpath
  local n="$1" src="$2" d="$I/Plugins/OFX/Natron/$1.ofx.bundle/Contents/Win64"
  mkdir -p "$d"; cp "$src" "$d/$1.ofx"
}
install_ofx Misc "$NATRON_ROOT/openfx-misc/build/Misc.ofx"
install_ofx CImg "$NATRON_ROOT/openfx-misc/build/CImg.ofx"
install_ofx IO   "$NATRON_ROOT/openfx-io/build/IO.ofx"

# --- PyPlugs: built-ins (from the build's POST_BUILD) + community pack ---
log "PyPlugs (built-in + community)"
[ -d "$B/Plugins/PyPlugs" ] && cp -r "$B/Plugins/PyPlugs/." "$I/Plugins/PyPlugs/" || warn "no built-in PyPlugs found in build tree"
cp -r "$NATRON_ROOT/natron-plugins/." "$I/Plugins/PyPlugs/"

# --- OpenColorIO configs: reuse local copy if present, else download (non-fatal) ---
if [ -d "$I/Resources/OpenColorIO-Configs" ]; then
  ok "OCIO configs already present"
elif [ -d "$B/Resources/OpenColorIO-Configs" ]; then
  log "OCIO configs (reusing local copy)"; cp -r "$B/Resources/OpenColorIO-Configs" "$I/Resources/OpenColorIO-Configs"
else
  log "OCIO configs (downloading Natron-v2.4)"
  if ( cd "$I/Resources" \
       && curl -L -s -S https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v2.4.tar.gz -o ocio.tgz \
       && tar xzf ocio.tgz && mv OpenColorIO-Configs-Natron-v2.4 OpenColorIO-Configs && rm -f ocio.tgz ); then
    ok "OCIO configs installed"
  else
    warn "OCIO download failed — Natron still runs (color presets unavailable). Re-run 06 to retry."
  fi
fi

ok "Phase 06 (install) complete — staged at $I"
