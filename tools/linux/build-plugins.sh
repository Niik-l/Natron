#!/usr/bin/env bash
# Build the OFX plugins (Misc.ofx, CImg.ofx from openfx-misc; IO.ofx from
# openfx-io) for a Linux CI build. Mirrors tools/win-build/02-patch.sh +
# 05-plugins.sh: same pinned sources (checked out by the workflow at the refs
# in tools/win-build/config.sh) and the same patches.
#
# Usage: build-plugins.sh <workspace> <build-type>
#   <workspace> holds natron/, openfx-misc/ and openfx-io/ checkouts.
# Writes <workspace>/plugins.log. Exits non-zero if any plugin wasn't built,
# after trying both repos, so one run reports every failure.
set -euo pipefail

[ $# -eq 2 ] || { echo "Usage: $0 <workspace> <build-type>"; exit 2; }
W="$(readlink -f "$1")"
BUILD_TYPE="$2"
P="$W/natron/tools/win-build/patches"
cd "$W"

for repo in openfx-misc openfx-io; do
    if git -C "$repo" apply --reverse --check "$P/$repo.patch" 2>/dev/null; then
        echo "$repo: patch already applied"
    else
        git -C "$repo" apply "$P/$repo.patch"
        echo "$repo: applied $repo.patch"
    fi
done

# openfx-io forces C++14 when it detects OIIO >= 2.3, but OIIO 3's headers
# need C++17 (std::byte). MSYS2's find module doesn't report the version, so
# Windows keeps GCC's C++17 default; Linux config files do report it.
sed -i 's/set(CMAKE_CXX_STANDARD 14)/set(CMAKE_CXX_STANDARD 17)/' openfx-io/CMakeLists.txt
grep -q 'set(CMAKE_CXX_STANDARD 17)' openfx-io/CMakeLists.txt

# openfx-misc fetches its pinned CImg.h (and patches inpaint.h) at build time.
make -C openfx-misc/CImg CImg.h

# CMAKE_POLICY_VERSION_MINIMUM: the plugin CMakeLists predate CMake 4.
fail=0
: > "$W/plugins.log"
for repo in openfx-misc openfx-io; do
    { cmake -S "$repo" -B "$repo/build" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      && ninja -C "$repo/build" -k 0 -j"$(nproc)"; } 2>&1 \
      | tee -a "$W/plugins.log" || fail=1
done

for p in Misc CImg IO; do
    find openfx-misc/build openfx-io/build -name "$p.ofx" -type f | grep -q . \
        || { echo "::error::$p.ofx was not built"; fail=1; }
done
exit $fail
