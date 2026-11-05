#!/bin/bash
# Cryptomatte node acceptance check. Usage (MSYS2 / Linux shell):
#   tools/cryptomatte-tests/run_check.sh <path to NatronRenderer[.exe]>
# Needs oiiotool on PATH and ref/blender_crypto_0001.exr (make it with
#   blender -b --python make_ref.py -- ref/blender_crypto_0001.exr   if missing).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RENDERER="${1:?path to NatronRenderer}"
export CRYPTO_TEST_DIR="$HERE"
mkdir -p "$HERE/out"; rm -f "$HERE/out"/*.exr
"$RENDERER" -t "$HERE/decode_check.py" > "$HERE/out/_run.log" 2>&1
grep -a "^CHECK\|Rendering\|rror" "$HERE/out/_run.log" | grep -v "Rendering started"
REF="$HERE/ref/blender_crypto_0001.exr"; O="$HERE/out"; fail=0
ok()   { echo "  OK   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
# same(a,b,tol): every pixel of channel A within tol
same() { oiiotool "$1" --ch A "$2" --ch A --diff --fail "$3" --failpercent 0 --hardfail "$3" >/dev/null 2>&1; }
alpha_of_ref() { oiiotool "$REF" --ch "ViewLayer.Combined.A" --chnames A -o "$O/ref_alpha.exr" >/dev/null 2>&1; }
alpha_of_ref
# 1. every manifest object selected == the beauty alpha (both come from the same samples; the
#    beauty is denoised, so allow a little slack at the edges)
same "$O/all_objects.exr" "$O/ref_alpha.exr" 0.02 && ok "all objects == Combined alpha (tol 0.02)" || bad "all objects != Combined alpha"
# 2. hex ID == name, bit exact
same "$O/cube.exr" "$O/cube_hex.exr" 0 && ok "<2ce615c5> == Cube_Left" || bad "<2ce615c5> != Cube_Left"
# 3. Matte Output into rgba.red puts the same matte in R
oiiotool "$O/cube.exr" --ch A --chnames R "$O/cube_red.exr" --ch R --diff --fail 0 --hardfail 0 >/dev/null 2>&1 && ok "Matte Output rgba.red" || bad "Matte Output rgba.red"
# 4. material and asset layers are unions of object mattes
same "$O/greenmat.exr" "$O/sphere_ground.exr" 1e-6 && ok "GreenMat == Sphere_Mid + Ground" || bad "GreenMat != Sphere_Mid + Ground"
same "$O/herogroup.exr" "$O/cube_sphere.exr" 1e-6 && ok "HeroGroup == Cube_Left + Sphere_Mid" || bad "HeroGroup != Cube_Left + Sphere_Mid"
# 5. the cube is on the left: nothing right of x=200, something on the left
rmax=$(oiiotool "$O/cube.exr" --ch A --crop 120x240+200+0 --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}')
lmax=$(oiiotool "$O/cube.exr" --ch A --crop 200x240+0+0 --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}')
[ "${rmax:-1}" = "0.000000" ] && ok "cube matte empty right of x=200" || bad "cube matte right of x=200 max=$rmax"
awk -v m="${lmax:-0}" 'BEGIN{exit !(m>0.99)}' && ok "cube matte reaches 1 on the left (max $lmax)" || bad "cube matte max on the left = $lmax"
# 6. an unknown name gives an empty matte; the preview has colour
nmax=$(oiiotool "$O/nothing.exr" --ch A --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}')
[ "${nmax:-1}" = "0.000000" ] && ok "unknown name -> empty matte" || bad "unknown name matte max=$nmax"
pmax=$(oiiotool "$O/preview.exr" --ch R,G,B --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}')
awk -v m="${pmax:-0}" 'BEGIN{exit !(m>0.1)}' && ok "preview colours present (max R $pmax)" || bad "preview max R = $pmax"
# 7. wildcards: Cube_* and ?ube_Left == Cube_Left, * == every object, S* == the three S names
same "$O/glob_cube.exr" "$O/cube.exr" 0 && ok "Cube_* == Cube_Left" || bad "Cube_* != Cube_Left"
same "$O/glob_q_cube.exr" "$O/cube.exr" 0 && ok "?ube_Left == Cube_Left" || bad "?ube_Left != Cube_Left"
same "$O/glob_all.exr" "$O/all_objects.exr" 0 && ok "* == all manifest objects" || bad "* != all manifest objects"
same "$O/glob_s.exr" "$O/s_names.exr" 0 && ok "S* == Sphere_Mid + Suzanne_Right + Sun" || bad "S* != the three S names"
# 8. the Expression knob holds the Nuke-style terms for the layer's three planes
cube_f=$(grep -a "^CHECK expr cube float" "$HERE/out/_run.log" | awk '{print $NF}')
expr=$(grep -a "^CHECK expr cube |" "$HERE/out/_run.log")
echo "$expr" | grep -q "((ViewLayer.CryptoObject00.red == $cube_f) ? ViewLayer.CryptoObject00.green : 0) + ((ViewLayer.CryptoObject00.blue == $cube_f) ? ViewLayer.CryptoObject00.alpha : 0)" && ok "expression term for Cube_Left ($cube_f)" || bad "expression term for Cube_Left missing: $expr"
echo "$expr" | grep -q "CryptoObject02.alpha" && ok "expression covers plane 02" || bad "expression lacks plane 02"
[ "$(grep -a "^CHECK expr glob terms" "$HERE/out/_run.log" | awk '{print $NF}')" = "3" ] && ok "S* expression has 3 IDs" || bad "S* expression ID count: $(grep -a "^CHECK expr glob terms" "$HERE/out/_run.log")"
[ "$(grep -a "^CHECK expr empty |" "$HERE/out/_run.log" | awk '{print $NF}')" = "0" ] && ok "empty list -> expression 0" || bad "empty list expression"
[ $fail = 0 ] && echo "CRYPTOMATTE_CHECK DONE_OK" || echo "CRYPTOMATTE_CHECK FAILED"
exit $fail
