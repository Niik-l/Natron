#!/usr/bin/env bash
# Build the OFX Lab plugins (GridWrap, UVGen, SharpenZ, TimeWarp, ZDefocusZ).
#
# Usage: build.sh <openfx-dir> <out-dir>
#   <openfx-dir>  checkout of AcademySoftwareFoundation/openfx (the plugins use the upstream
#                 Support library, not Natron's fork of it; ref in tools/win-build/config.sh)
#   <out-dir>     receives <Name>.ofx for each plugin, plus obj/
#
# Works with GCC on Linux and with MinGW-w64 GCC on Windows (MSYS2). No dependencies besides the
# OpenFX headers: GridWrap opens the system OpenGL library and ZDefocusZ the OpenCL loader at run
# time, by name, and both carry on without them.
set -euo pipefail

[ $# -eq 2 ] || { echo "Usage: $0 <openfx-dir> <out-dir>"; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
OFX="$(cd "$1" && pwd)"
mkdir -p "$2/obj"
OUT="$(cd "$2" && pwd)"
[ -f "$OFX/Support/Library/ofxsImageEffect.cpp" ] || { echo "no OpenFX Support library in $OFX"; exit 1; }

JOBS="${OFXLAB_JOBS:-$( (nproc || echo 2) 2>/dev/null)}"
CXX="${CXX:-g++}"
INC="-I$OFX/include -I$OFX/Support/include -I$OFX/Support/Library -I$OFX/Support/Plugins/include -I$HERE/common"
CXXFLAGS="-O2 -std=c++17 -DNDEBUG -DOFX_SUPPORTS_OPENGLRENDER=0 -fvisibility=hidden -Wno-deprecated-declarations"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    INC="$INC -I$HERE/common/win"   # dlfcn.h shim over LoadLibrary
    LINK="-static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic" ;;
  *)
    CXXFLAGS="$CXXFLAGS -fPIC -pthread"
    LINK="-pthread -ldl -Wl,--no-undefined" ;;
esac

cd "$OUT/obj"
pids=()
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done; }
compile() { # obj src
  if [ ! -f "$1" ] || [ "$2" -nt "$1" ]; then
    throttle
    echo "CC  ${2#"$HERE"/}"
    $CXX $CXXFLAGS $INC -c "$2" -o "$1" &
    pids+=($!)
  fi
}

SUP=""
for f in ofxsCore ofxsImageEffect ofxsInteract ofxsLog ofxsMultiThread ofxsParams ofxsProperty ofxsPropertyValidation; do
  compile "sup_$f.o" "$OFX/Support/Library/$f.cpp"
  SUP="$SUP sup_$f.o"
done

declare -A SRCS=(
  [GridWrap]="gridwrap:gridwrap grid resample render interact gl"
  [UVGen]="uvgen:uvgen uvcore"
  [SharpenZ]="sharpenz:sharpenz"
  [TimeWarp]="timewarp:timewarp flow motion"
  [ZDefocusZ]="zdefocusz:zdefocusz zd bokeh layered gpu"
)
NAMES="GridWrap UVGen SharpenZ TimeWarp ZDefocusZ"
for name in $NAMES; do
  dir="${SRCS[$name]%%:*}"
  for s in ${SRCS[$name]#*:}; do
    compile "${name}_$s.o" "$HERE/$dir/$s.cpp"
  done
done

fail=0
for p in "${pids[@]:-}"; do [ -z "$p" ] || wait "$p" || fail=1; done
[ $fail -eq 0 ] || { echo "ofxlab: compile failed"; exit 1; }

for name in $NAMES; do
  objs=""
  for s in ${SRCS[$name]#*:}; do objs="$objs ${name}_$s.o"; done
  echo "LD  $name.ofx"
  $CXX -shared -o "$OUT/$name.ofx" $objs $SUP $LINK
done
echo "ofxlab: built $NAMES"
