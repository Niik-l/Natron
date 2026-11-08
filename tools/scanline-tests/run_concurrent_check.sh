#!/bin/bash
# ScanlineRender concurrent-render check: three 4K_DCP High-AA ScanlineRender Writes rendered at
# once must all produce a full-size image (2026-10-21: one of them failed at the multisample FBO;
# the render now steps the sample count down when the driver refuses the framebuffer). Usage
# (MSYS2 / Linux shell, OFX_PLUGIN_PATH set, a GPU + GL driver available):
#   tools/scanline-tests/run_concurrent_check.sh <path to NatronRenderer[.exe]>
# Needs oiiotool on PATH.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RENDERER="${1:?path to NatronRenderer}"
export SCANLINE_TEST_DIR="$HERE"
O="$HERE/out"; mkdir -p "$O"
rm -f "$O"/slr_*.exr
"$RENDERER" -t "$HERE/concurrent_check.py" > "$O/_run.log" 2>&1
grep -a "^SLR\|\[ScanlineRender\]\|Rendering Failed\|rror\|Traceback" "$O/_run.log" | grep -v "Rendering started" | cut -c1-300
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
amax() { oiiotool "$1" --ch A --printstats 2>/dev/null | grep "Stats Max" | awk '{print $3}'; }
size() { oiiotool --info "$1" 2>/dev/null | grep -o "[0-9]* x *[0-9]*" | head -1 | tr -d ' '; }
for i in 0 1 2; do
  f="$O/slr_$i.exr"
  if [ ! -s "$f" ]; then bad "slr_$i.exr missing (the render failed)"; continue; fi
  s=$(size "$f"); [ "$s" = "4096x2160" ] && ok "slr_$i is 4096x2160" || bad "slr_$i size $s (expected 4096x2160)"
  m=$(amax "$f"); awk -v m="$m" 'BEGIN{exit !(m>0.9)}' && ok "slr_$i has geometry (alpha max $m)" || bad "slr_$i empty (alpha max $m)"
done
grep -aq "Rendering Failed" "$O/_run.log" && bad "a render reported Rendering Failed" || ok "no Rendering Failed in the log"
n=$(grep -ac "\[ScanlineRender\].*refused" "$O/_run.log")
echo "  info: $n framebuffer fallback line(s) in the log (0 = the driver accepted 8 samples for all three)"
[ $fail = 0 ] && echo "SCANLINE_CONCURRENT_CHECK DONE_OK" || echo "SCANLINE_CONCURRENT_CHECK FAILED"
exit $fail
