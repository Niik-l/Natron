#!/usr/bin/env bash
# Assemble the Linux install tree from a CI build, strip it, and write the
# tarballs. Layout mirrors tools/win-build/06-install.sh; Natron resolves its
# bundled plugins as <exe dir>/../Plugins/{OFX/Natron,PyPlugs}.
#
# Usage: package.sh <workspace> <name-suffix>
#   <workspace> holds natron/build, openfx-misc/build, openfx-io/build and
#   natron-plugins/.
# Produces:
#   <workspace>/dist/Natron                          stripped install tree
#   <workspace>/Natron-<suffix>.tar.gz               that tree
#   <workspace>/Natron-<suffix>-debug-symbols.tar.gz the stripped debug info
set -euo pipefail

[ $# -eq 2 ] || { echo "Usage: $0 <workspace> <name-suffix>"; exit 2; }
R="$(readlink -f "$1")"
V="$2"
B="$R/natron/build"; D="$R/dist/Natron"; G="$R/dist/debug"
cd "$R"

rm -rf "$R/dist"
mkdir -p "$D/bin" "$D/Plugins/OFX/Natron" "$D/Plugins/PyPlugs" "$D/Resources" "$G"
cp "$B/App/Natron" "$B/Renderer/NatronRenderer" "$B/PythonBin/natron-python" "$D/bin/"

for p in Misc CImg IO; do
    src=$(find openfx-misc/build openfx-io/build -name "$p.ofx" -type f | head -1)
    d="$D/Plugins/OFX/Natron/$p.ofx.bundle/Contents/Linux-x86-64"
    mkdir -p "$d"; cp "$src" "$d/"
done

cp -r "$B/Plugins/PyPlugs/." "$D/Plugins/PyPlugs/"
cp -r natron-plugins/. "$D/Plugins/PyPlugs/"
rm -rf "$D/Plugins/PyPlugs/.git" "$D/Plugins/PyPlugs/.github"

curl -fsSL https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v2.4.tar.gz \
    | tar -xz -C "$D/Resources"
mv "$D/Resources/OpenColorIO-Configs-Natron-v2.4" "$D/Resources/OpenColorIO-Configs"

# Strip every binary; keep its debug info in a separate .debug file linked
# back by name (gdb finds it when placed next to the binary).
while IFS= read -r f; do
    n=$(basename "$f")
    objcopy --only-keep-debug "$f" "$G/$n.debug"
    strip --strip-debug --strip-unneeded "$f"
    objcopy --add-gnu-debuglink="$G/$n.debug" "$f"
done < <(find "$D/bin" "$D/Plugins/OFX" -type f \( -path '*/bin/*' -o -name '*.ofx' \))
du -sh "$D" "$G"

tar -C "$R/dist" -czf "$R/Natron-$V.tar.gz" Natron
tar -C "$R/dist" -czf "$R/Natron-$V-debug-symbols.tar.gz" debug
ls -la "$R"/Natron-"$V"*.tar.gz
