#!/usr/bin/env bash
# Build an AppImage from the Linux CI install tree (the `dist/Natron` folder that
# .github/workflows/linux-build.yml packages: bin/, Plugins/, Resources/).
#
# Unlike make-appimage.sh next to it (which wraps an official Natron tarball
# that already bundles its libraries), this bundles everything from the build
# machine: Qt + its plugins, the embedded Python (stdlib + PySide6/shiboken6/
# qtpy), and every shared library Natron, the OFX plugins and the Python
# extension modules need.
#
# The result runs on distros whose glibc is at least as new as the build
# machine's; the script prints that minimum at the end.
#
# Usage: build-appimage.sh <install-dir> <output-dir> <appimage-file-name>
set -euo pipefail

[ $# -eq 3 ] || { echo "Usage: $0 <install-dir> <output-dir> <appimage-file-name>"; exit 2; }
SRC="$(readlink -f "$1")"
OUT="$(readlink -f "$2")"
NAME="$3"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
[ -x "$SRC/bin/Natron" ] || { echo "error: $SRC/bin/Natron not found"; exit 1; }

# Tool pins. Each download is checked against its SHA-256.
LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
PLUGIN_QT_URL=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage
PLUGIN_QT_SHA=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

# PySide6 modules to keep. The rest would drag in all of Qt (WebEngine, 3D...).
PYSIDE_KEEP="QtCore QtGui QtWidgets QtOpenGL QtOpenGLWidgets QtNetwork QtConcurrent"

# The tools are AppImages themselves; containers have no FUSE to mount them.
export APPIMAGE_EXTRACT_AND_RUN=1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
TOOLS="$WORK/tools"
APPDIR="$WORK/AppDir"
mkdir -p "$TOOLS" "$APPDIR/usr" "$OUT"

fetch() { # url sha256 dest
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c -
    chmod +x "$3"
}
echo "== Fetching pinned tools"
fetch "$LINUXDEPLOY_URL"  "$LINUXDEPLOY_SHA"  "$TOOLS/linuxdeploy-x86_64.AppImage"
fetch "$PLUGIN_QT_URL"    "$PLUGIN_QT_SHA"    "$TOOLS/linuxdeploy-plugin-qt-x86_64.AppImage"
fetch "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA" "$TOOLS/appimagetool-x86_64.AppImage"
fetch "$RUNTIME_URL"      "$RUNTIME_SHA"      "$TOOLS/runtime-x86_64"
export PATH="$TOOLS:$PATH"   # linuxdeploy finds its plugins on PATH

# unresolved <elf>: prints the libraries ldd can't find (empty if none).
# ldd can itself crash on some ELF files; that must not abort the script.
unresolved() { { ldd "$1" 2>/dev/null || true; } | awk '/not found/ {print $1}'; }

echo "== Natron tree"
cp -a "$SRC/." "$APPDIR/usr/"

echo "== Python"
# Natron's setupPythonEnv (Global/PythonUtils.cpp) sets PYTHONHOME to
# <bin>/.. and adds lib/pythonX.Y, its lib-dynload and site-packages.
PYV="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
STDLIB="$(python3 -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')"
DYNLOAD="$(python3 -c 'import sysconfig, os; print(os.path.join(sysconfig.get_path("platstdlib"), "lib-dynload"))')"
PYDST="$APPDIR/usr/lib/python$PYV"
mkdir -p "$PYDST/site-packages"
tar -C "$STDLIB" \
    --exclude=./site-packages --exclude=./test --exclude=./idlelib \
    --exclude=./tkinter --exclude=./turtledemo --exclude=./ensurepip \
    -cf - . | tar -C "$PYDST" -xf -
if [ ! -d "$PYDST/lib-dynload" ]; then
    cp -a "$DYNLOAD" "$PYDST/lib-dynload"
fi
for mod in PySide6 shiboken6 qtpy packaging; do
    d="$(python3 -c "import $mod, os; print(os.path.dirname($mod.__file__))")"
    cp -a "$d" "$PYDST/site-packages/"
    echo "  $mod <- $d"
done
for f in "$PYDST"/site-packages/PySide6/Qt*.so "$PYDST"/site-packages/PySide6/Qt*.pyi; do
    [ -e "$f" ] || continue
    m="$(basename "$f")"; m="${m%%.*}"
    case " $PYSIDE_KEEP " in *" $m "*) ;; *) rm -f "$f" ;; esac
done
# Fedora's Python uses platlibdir=lib64; make both spellings resolve.
ln -sfn lib "$APPDIR/usr/lib64"

# Drop Python extension modules whose system libraries aren't installed
# (e.g. _tkinter without Tk); linuxdeploy would otherwise abort on them.
while IFS= read -r f; do
    missing="$(unresolved "$f")"
    if [ -n "$missing" ]; then
        echo "  dropping $(basename "$f") (needs: $(echo $missing))"
        rm -f "$f"
    fi
done < <(find "$PYDST" -name '*.so*' -type f)

echo "== Runtime-loaded libraries"
# OIDN loads its CPU device module with dlopen, so no dependency scan finds it.
oidn_cpu="$(find /usr/lib64 /usr/lib -maxdepth 1 -name 'libOpenImageDenoise_device_cpu.so*' 2>/dev/null)"
[ -n "$oidn_cpu" ] || { echo "error: OIDN CPU device module not found (Cycles denoising would fail)"; exit 1; }
echo "$oidn_cpu" | xargs -I{} cp -a {} "$APPDIR/usr/lib/"

echo "== Desktop entry, icon, environment hook"
cp "$REPO/Gui/Resources/Applications/fr.natron.Natron.desktop" "$WORK/fr.natron.Natron.desktop"
cp "$REPO/Gui/Resources/Images/natronIcon256_linux.png" "$WORK/natronIcon256_linux.png"
mkdir -p "$APPDIR/usr/share/metainfo" "$APPDIR/usr/share/mime/packages"
cp "$REPO/Gui/Resources/Metainfo/fr.natron.Natron.appdata.xml" "$APPDIR/usr/share/metainfo/"
cp "$REPO/Gui/Resources/Mime/x-natron.xml" "$APPDIR/usr/share/mime/packages/"
# OFX plugins and Python extension modules are dlopen'd; their dependencies
# live in usr/lib. linuxdeploy's AppRun sources every script in apprun-hooks/.
mkdir -p "$APPDIR/apprun-hooks"
# $APPDIR is set by the AppImage runtime; fall back to AppRun's own location
# when the extracted AppDir is run directly.
cat > "$APPDIR/apprun-hooks/natron-env.sh" <<'EOF'
_natron_appdir="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
export LD_LIBRARY_PATH="$_natron_appdir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset _natron_appdir
EOF

# Our own AppRun: linuxdeploy otherwise makes AppRun a plain symlink to
# Natron, and the apprun-hooks (ours + linuxdeploy-plugin-qt's) never run.
cat > "$WORK/AppRun" <<'EOF'
#!/bin/sh
this_dir="$(dirname "$(readlink -f "$0")")"
export APPDIR="${APPDIR:-$this_dir}"
for hook in "$this_dir"/apprun-hooks/*.sh; do
    [ -f "$hook" ] && . "$hook"
done
exec "$this_dir/usr/bin/Natron" "$@"
EOF
chmod +x "$WORK/AppRun"

echo "== linuxdeploy"
export QMAKE="$(command -v qmake6 || echo /usr/lib64/qt6/bin/qmake)"
# linuxdeploy's bundled strip is too old for current distro libraries (it
# can't parse .relr.dyn and fails). Nothing needs it: distro libraries ship
# stripped, and Natron + the plugins are stripped before this script runs.
export NO_STRIP=1
args=( --appdir "$APPDIR"
       --executable "$APPDIR/usr/bin/Natron"
       --executable "$APPDIR/usr/bin/NatronRenderer"
       --executable "$APPDIR/usr/bin/natron-python"
       --desktop-file "$WORK/fr.natron.Natron.desktop"
       --icon-file "$WORK/natronIcon256_linux.png"
       --custom-apprun "$WORK/AppRun"
       --plugin qt )
# Everything loaded at runtime rather than linked: OFX plugins, Python
# extension modules, the OIDN device module.
while IFS= read -r f; do
    args+=( --deploy-deps-only "$f" )
done < <(find "$APPDIR/usr/Plugins/OFX" -name '*.ofx' -type f
         find "$PYDST" -name '*.so*' -type f
         ls "$APPDIR"/usr/lib/libOpenImageDenoise_device_cpu.so*)
linuxdeploy-x86_64.AppImage "${args[@]}"

# This only proves nothing is missing on the build machine, where the system
# copies are also present; the workflow's appimage-test job checks the real
# thing on a different distro.
echo "== Unresolved libraries on the build machine (should be none)"
bad=0
while IFS= read -r f; do
    missing="$(LD_LIBRARY_PATH="$APPDIR/usr/lib" unresolved "$f")"
    if [ -n "$missing" ]; then echo "  $f: $missing"; bad=1; fi
done < <(find "$APPDIR/usr" -type f \( -name '*.so*' -o -name '*.ofx' -o -perm -u+x \) -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print)
[ "$bad" -eq 0 ] || { echo "error: unresolved libraries in the AppDir"; exit 1; }

echo "== Minimum glibc"
find "$APPDIR/usr" -type f -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print0 \
    | xargs -0 objdump -T 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV | tail -1 \
    | tee "$OUT/$NAME.glibc.txt"

echo "== appimagetool"
ARCH=x86_64 appimagetool-x86_64.AppImage --runtime-file "$TOOLS/runtime-x86_64" \
    "$APPDIR" "$OUT/$NAME"
ls -la "$OUT/$NAME"
