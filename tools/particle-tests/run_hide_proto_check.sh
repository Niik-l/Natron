#!/bin/bash
# ParticleInstance "Hide Instanced Geo" check (CyclesRender; ScanlineRender needs the GUI). Usage (MSYS2 /
# Linux shell, OFX_PLUGIN_PATH set):
#   tools/particle-tests/run_hide_proto_check.sh <path to NatronRenderer[.exe]>
# Needs oiiotool on PATH and a Cycles-enabled build.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RENDERER="${1:?path to NatronRenderer}"
export PARTICLE_TEST_DIR="$HERE"
O="$HERE/out"; mkdir -p "$O"
rm -f "$O"/cyc_*.exr
"$RENDERER" -t "$HERE/hide_proto_check.py" > "$O/_run.log" 2>&1; grep -a "^HIDE\|Error\|Traceback" "$O/_run.log"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
# alpha max over the whole image / over the centre (where the source sphere sits)
amax()   { oiiotool "$1" --ch A --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}'; }
cmax()   { oiiotool "$1" --ch A --cut "$(centre "$1")" --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}'; }
centre() { oiiotool --info "$1" 2>/dev/null | grep -o "[0-9]* x *[0-9]*" | head -1 | awk '{printf "%dx%d+%d+%d", $1/10, $3/10, $1*0.45, $3*0.45}'; }
for f in cyc_on cyc_off; do
  [ -s "$O/$f.exr" ] || bad "missing output $f.exr"
done
[ $fail = 1 ] && { echo "HIDE_PROTO_CHECK FAILED (renders missing)"; exit 1; }
for r in cyc; do
  m=$(cmax "$O/${r}_off.exr"); awk -v m="$m" 'BEGIN{exit !(m>0.9)}' && ok "$r off: source sphere at the centre (max $m)" || bad "$r off: centre empty (max $m)"
  m=$(cmax "$O/${r}_on.exr");  awk -v m="$m" 'BEGIN{exit !(m<0.01)}' && ok "$r on: centre empty (max $m)" || bad "$r on: source sphere still rendered (max $m)"
  m=$(amax "$O/${r}_on.exr");  awk -v m="$m" 'BEGIN{exit !(m>0.9)}' && ok "$r on: instances present (max $m)" || bad "$r on: nothing rendered (max $m)"
done
[ $fail = 0 ] && echo "HIDE_PROTO_CHECK DONE_OK" || echo "HIDE_PROTO_CHECK FAILED"
exit $fail
