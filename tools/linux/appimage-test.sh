#!/bin/bash
# Tests the portable AppImage on a bare distro container, the way a user's
# desktop would see it. Run as root inside `docker run <distro-image>` with the
# AppImage + debug-symbols tarball in the current directory and this script's
# directory mounted (for xwd2png.py):
#
#   bash /tools/appimage-test.sh <label> <outdir>
#
# Steps, each written to <outdir>/status.txt as "<step>=ok|FAIL":
#   1. install a desktop baseline (GL, X11, fonts, and the libraries on the
#      AppImage excludelist), with the distro's own package manager;
#   2. libs:     every bundled ELF file resolves (ldd through the bundled RPATHs);
#   3. headless: `Natron --version` starts and exits normally;
#   4. gui:      the full GUI comes up under Xvfb on Mesa's software GL;
#                <outdir>/screenshot.png is grabbed after a settle time, and a
#                gdb backtrace is taken if it crashed.
# Exit status is non-zero if any of 2-4 failed.
set -uo pipefail

LABEL=${1:?label}
OUT=${2:?outdir}
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$OUT"
STATUS="$OUT/status.txt"
: > "$STATUS"
note() { echo "$1=$2" >> "$STATUS"; echo "== $1: $2"; }

echo "== $LABEL: $(. /etc/os-release && echo "$PRETTY_NAME")"
ldd --version 2>/dev/null | sed -n 1p || true
sed "s/^/AppImage needs /" ./*.glibc.txt 2>/dev/null || true

# ---- 1. desktop baseline ----------------------------------------------------
echo "== Desktop baseline"
common="file binutils findutils tar gzip gdb"
if command -v dnf >/dev/null; then
    pkgs="$common python3 xorg-x11-server-Xvfb mesa-dri-drivers mesa-libGL mesa-libEGL
          libglvnd-glx libglvnd-egl libglvnd-opengl fontconfig freetype harfbuzz dejavu-sans-fonts
          libX11 libxcb libxkbcommon libxkbcommon-x11 xcb-util-wm xcb-util-image xcb-util-keysyms
          xcb-util-renderutil xcb-util-cursor libSM libICE fribidi alsa-lib"
    if dnf --version 2>/dev/null | head -1 | grep -q '^5'; then
        skip=--skip-unavailable            # dnf5 (Fedora)
    else
        skip=--setopt=strict=0             # dnf4 (Rocky/Alma): unknown names are not fatal
    fi
    # shellcheck disable=SC2086
    dnf install -y -q --setopt=install_weak_deps=False $skip $pkgs >/dev/null
elif command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    want="$common python3 xvfb libgl1-mesa-dri libgl1 libegl1 libopengl0 libglx0 fontconfig
          libfreetype6 libharfbuzz0b fonts-dejavu-core libx11-6 libxcb1 libxkbcommon0
          libxkbcommon-x11-0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0
          libxcb-cursor0 libxcb-shape0 libxcb-xinerama0 libxcb-randr0 libxcb-xfixes0
          libsm6 libice6 libfribidi0 libasound2 libasound2t64"
    pkgs=""
    for p in $want; do                     # names differ per release; take what exists
        apt-cache show "$p" >/dev/null 2>&1 && pkgs="$pkgs $p"
    done
    # shellcheck disable=SC2086
    apt-get install -y -qq --no-install-recommends $pkgs >/dev/null
elif command -v pacman >/dev/null; then
    # shellcheck disable=SC2086
    pacman -Syu --noconfirm --needed -q $common python xorg-server-xvfb mesa libglvnd fontconfig \
        freetype2 harfbuzz ttf-dejavu libx11 libxcb libxkbcommon libxkbcommon-x11 xcb-util-cursor \
        xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm alsa-lib e2fsprogs \
        libgpg-error libsm libice fribidi >/dev/null
elif command -v zypper >/dev/null; then
    # shellcheck disable=SC2086
    zypper -n -q install --no-recommends $common python3 xorg-x11-server-Xvfb Mesa-dri Mesa-libGL1 \
        Mesa-libEGL1 libOpenGL0 libGLX0 fontconfig libfreetype6 libharfbuzz0 dejavu-fonts \
        libX11-6 libxcb1 libxkbcommon0 libxkbcommon-x11-0 libxcb-icccm4 libxcb-image0 \
        libxcb-keysyms1 libxcb-render-util0 libxcb-cursor0 libSM6 libICE6 libfribidi0 \
        libasound2 >/dev/null || [ $? -eq 104 ]   # 104: some names not found, rest installed
else
    echo "::error::no known package manager"; exit 2
fi

# ---- 2. every bundled binary resolves ---------------------------------------
echo "== Every bundled binary resolves"
chmod +x ./*.AppImage
./*.AppImage --appimage-extract >/dev/null
bad=0
while IFS= read -r f; do
    # Plain ldd resolves through the bundled RPATHs, as a launch does; ldd can
    # crash on some ELF files, which must not end the scan.
    missing=$({ ldd "$f" 2>/dev/null || true; } | awk '/not found/ {print $1}')
    if [ -n "$missing" ]; then echo "$f: $missing"; bad=1; fi
done < <(find squashfs-root/usr -type f -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print)
if [ "$bad" -eq 0 ]; then note libs ok; else note libs FAIL; fi

# ---- 3. headless start ------------------------------------------------------
echo "== Headless start"
if QT_QPA_PLATFORM=offscreen timeout 120 squashfs-root/AppRun --version > "$OUT/headless.log" 2>&1; then
    cat "$OUT/headless.log"; note headless ok
else
    rc=$?; cat "$OUT/headless.log"; echo "exit $rc"; note headless FAIL
fi

# ---- 4. GUI under Xvfb -------------------------------------------------------
echo "== GUI under Xvfb (Mesa software GL)"
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 XDG_RUNTIME_DIR=/tmp/xdg HOME=/tmp/home
mkdir -p "$XDG_RUNTIME_DIR" "$HOME" "$OUT/fb"; chmod 700 "$XDG_RUNTIME_DIR"
Xvfb :99 -screen 0 1600x1000x24 -fbdir "$OUT/fb" -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
XVFB=$!
sleep 2
gui=FAIL
if ! kill -0 $XVFB 2>/dev/null; then
    echo "Xvfb did not start:"; cat "$OUT/xvfb.log"
else
    squashfs-root/AppRun > "$OUT/gui.log" 2>&1 &
    NATRON=$!
    settle=45; alive=1
    for ((i = 0; i < settle; i++)); do
        sleep 1
        if ! kill -0 $NATRON 2>/dev/null; then alive=0; break; fi
    done
    if [ $alive -eq 1 ]; then
        cp "$OUT/fb/Xvfb_screen0" "$OUT/screenshot.xwd"
        python3 "$TOOLS/xwd2png.py" "$OUT/screenshot.xwd" "$OUT/screenshot.png" && rm -f "$OUT/screenshot.xwd"
        kill $NATRON 2>/dev/null; sleep 5; kill -9 $NATRON 2>/dev/null
        wait $NATRON; rc=$?
        # 0 or SIGTERM (143) = it was still running fine when we stopped it.
        if [ $rc -eq 0 ] || [ $rc -eq 143 ]; then gui=ok; fi
        echo "GUI ran ${settle}s, stopped with exit $rc"
    else
        wait $NATRON; rc=$?
        echo "GUI exited early with $rc"
    fi
    tail -60 "$OUT/gui.log"
    if [ $gui != ok ] && ls Natron-*-debug-symbols.tar.gz >/dev/null 2>&1; then
        echo "== Backtrace"
        tar -xzf Natron-*-debug-symbols.tar.gz
        cp debug/Natron.debug squashfs-root/usr/bin/
        # "info symbol" names the library holding the crash address;
        # "info sharedlibrary" shows which copies (bundled or host) loaded.
        APPDIR="$PWD/squashfs-root" timeout 300 gdb -q -batch -ex run -ex 'thread apply all bt 30' \
            -ex 'info symbol $pc' -ex 'info sharedlibrary' \
            --args squashfs-root/usr/bin/Natron 2>&1 | tail -250 | tee "$OUT/backtrace.log" || true
    fi
fi
kill $XVFB 2>/dev/null; rm -rf "$OUT/fb"
note gui $gui

grep -q FAIL "$STATUS" && exit 1
exit 0
