#!/bin/bash
# Cycles Cryptomatte encoder check. Usage (MSYS2 / Linux shell, OFX_PLUGIN_PATH set):
#   tools/cryptomatte-tests/run_encode_check.sh <path to NatronRenderer[.exe]>
# Needs oiiotool and exrheader on PATH and a Cycles-enabled build.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RENDERER="${1:?path to NatronRenderer}"
export CRYPTO_TEST_DIR="$HERE"
O="$HERE/out"; mkdir -p "$O"
rm -f "$O"/obj_*.exr "$O"/custom_*.exr "$O"/asset_*.exr "$O"/mat_*.exr "$O"/beauty.exr "$O"/file_*.exr "$O"/cycles_crypto.exr
"$RENDERER" -t "$HERE/encode_check.py" 2>&1 | grep -a "^ENC\|rror\|Cryptomatte flags" | grep -v "Rendering started"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
same() { oiiotool "$1" --ch A "$2" --ch A --diff --fail "$3" --failpercent 0 --hardfail "$3" >/dev/null 2>&1; }
amax() { oiiotool "$1" --ch A --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}'; }
# sum of two mattes == a third (A channels)
sum2() { oiiotool "$1" --ch A "$2" --ch A --add -o "$O/_sum.exr" >/dev/null 2>&1 && same "$O/_sum.exr" "$3" "$4"; }
for f in obj_sphere obj_cube obj_card obj_all custom_hero custom_default asset_hero asset_cube mat_cube beauty; do
  [ -s "$O/$f.exr" ] || { bad "missing output $f.exr"; }
done
[ $fail = 1 ] && { echo "CYCLES_CRYPTO_CHECK FAILED (renders missing)"; exit 1; }
# 1. each object matte is non-empty and the three are disjoint-ish: their sum == the all-objects matte
m=$(amax "$O/obj_sphere.exr"); awk -v m="$m" 'BEGIN{exit !(m>0.9)}' && ok "sphere matte present (max $m)" || bad "sphere matte max=$m"
m=$(amax "$O/obj_cube.exr");   awk -v m="$m" 'BEGIN{exit !(m>0.9)}' && ok "cube matte present (max $m)"   || bad "cube matte max=$m"
m=$(amax "$O/obj_card.exr");   awk -v m="$m" 'BEGIN{exit !(m>0.9)}' && ok "card matte present (max $m)"   || bad "card matte max=$m"
oiiotool "$O/obj_sphere.exr" --ch A "$O/obj_cube.exr" --ch A --add "$O/obj_card.exr" --ch A --add -o "$O/_sum3.exr" >/dev/null 2>&1
same "$O/_sum3.exr" "$O/obj_all.exr" 1e-5 && ok "sphere + cube + card == all objects" || bad "object mattes do not sum to all objects"
# 2. all objects == beauty alpha (same samples; lights are not hit)
same "$O/obj_all.exr" "$O/beauty.exr" 0.02 && ok "all objects == beauty alpha (tol 0.02)" || bad "all objects != beauty alpha"
# 3. custom layer: "hero" == sphere (inherited from the group) + cube (own tag); "default" == card
sum2 "$O/obj_sphere.exr" "$O/obj_cube.exr" "$O/custom_hero.exr" 1e-5 && ok "custom hero == sphere + cube" || bad "custom hero != sphere + cube"
same "$O/custom_default.exr" "$O/obj_card.exr" 1e-5 && ok "custom default == card" || bad "custom default != card"
# 4. asset layer: group's Crypto Asset covers the sphere; the ungrouped cube is its own asset
same "$O/asset_hero.exr" "$O/obj_sphere.exr" 1e-5 && ok "asset HeroAsset == sphere" || bad "asset HeroAsset != sphere"
same "$O/asset_cube.exr" "$O/obj_cube.exr" 1e-5 && ok "asset Cube3D == cube" || bad "asset Cube3D != cube"
# 5. material layer: the Material3D on the cube
same "$O/mat_cube.exr" "$O/obj_cube.exr" 1e-5 && ok "material Material3D == cube" || bad "material Material3D != cube"
# 6. the saved multi-layer EXR: manifests in the header, layers decode identically through a Read
if [ -s "$O/cycles_crypto.exr" ]; then
  n=$(exrheader "$O/cycles_crypto.exr" 2>/dev/null | grep -c "cryptomatte/.*/manifest")
  [ "$n" = "4" ] && ok "EXR header has 4 manifests" || bad "EXR header manifests = $n"
  exrheader "$O/cycles_crypto.exr" 2>/dev/null | grep "cryptomatte/.*/manifest" | cut -c1-160
  # the render node upsamples its native render to the project format; the file is native size
  W=$(oiiotool --info "$O/cycles_crypto.exr" 2>/dev/null | grep -o "[0-9]* x *[0-9]*" | head -1 | tr -d ' ')
  oiiotool "$O/obj_sphere.exr" --ch A --resample "$W" -o "$O/_live_sphere.exr" >/dev/null 2>&1
  oiiotool "$O/custom_hero.exr" --ch A --resample "$W" -o "$O/_live_hero.exr" >/dev/null 2>&1
  same "$O/file_obj_sphere.exr" "$O/_live_sphere.exr" 0 && ok "file CryptoObject sphere == live (bit exact at $W)" || bad "file CryptoObject sphere != live"
  same "$O/file_custom_hero.exr" "$O/_live_hero.exr" 0 && ok "file CryptoCustom hero == live (bit exact at $W)" || bad "file CryptoCustom hero != live"
else
  bad "cycles_crypto.exr was not written"
fi
[ $fail = 0 ] && echo "CYCLES_CRYPTO_CHECK DONE_OK" || echo "CYCLES_CRYPTO_CHECK FAILED"
exit $fail
