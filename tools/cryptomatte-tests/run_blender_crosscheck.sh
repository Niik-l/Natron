#!/bin/bash
# Cross-check: Blender's compositor decodes the EXR our Cycles wrote (out/cycles_crypto.exr,
# from run_encode_check.sh) and must produce the same matte as our Cryptomatte node.
#   tools/cryptomatte-tests/run_blender_crosscheck.sh [blender.exe]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; O="$HERE/out"
BLENDER="${1:-/c/Program Files/Blender Foundation/Blender 4.5/blender.exe}"
[ -s "$O/cycles_crypto.exr" ] || { echo "run run_encode_check.sh first"; exit 1; }
fail=0
check() { # layer name ours
  rm -f "$O/blender_matte_0001.exr"
  "$BLENDER" -b --python "$HERE/blender_decode.py" -- "$O/cycles_crypto.exr" "$O" "$1" "$2" 2>&1 | grep -a "^BLENDER\|Error\|Traceback" | head -5
  if [ -s "$O/blender_matte_0001.exr" ]; then
    oiiotool "$O/blender_matte_0001.exr" --ch 0 --chnames A -o "$O/_blender_$1.exr" >/dev/null 2>&1
    if oiiotool "$O/_blender_$1.exr" "$3" --ch A --diff --fail 1e-4 --failpercent 0 --hardfail 1e-4 >/dev/null 2>&1; then
      echo "  OK   Blender decodes $1 '$2' == ours"
    else
      echo "  FAIL Blender decodes $1 '$2' differs from ours"; oiiotool "$O/_blender_$1.exr" "$3" --ch A --diff 2>&1 | grep -i "max error\|pixels" | head -2; fail=1
    fi
  else
    echo "  FAIL Blender wrote nothing for $1 '$2'"; fail=1
  fi
}
# our live decodes are at the project format; Blender works at the file's native size
W=$(oiiotool --info "$O/cycles_crypto.exr" 2>/dev/null | grep -o "[0-9]* x *[0-9]*" | head -1 | tr -d ' ')
oiiotool "$O/obj_sphere.exr" --ch A --resample "$W" -o "$O/_live_sphere.exr" >/dev/null 2>&1
oiiotool "$O/custom_hero.exr" --ch A --resample "$W" -o "$O/_live_hero.exr" >/dev/null 2>&1
check CryptoObject "$(exrheader "$O/cycles_crypto.exr" | grep -o '"Sphere3D[0-9]*"' | head -1 | tr -d '"')" "$O/_live_sphere.exr"
check CryptoCustom hero "$O/_live_hero.exr"
[ $fail = 0 ] && echo "BLENDER_CROSSCHECK DONE_OK" || echo "BLENDER_CROSSCHECK FAILED"
exit $fail
