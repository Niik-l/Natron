# Headless check that the bundled openfx-arena (Arena.ofx) works from the
# AppImage on a bare distro: ReadSVG -> Write renders ARENA_SVG to ARENA_OUT.
# Run by tools/linux/appimage-test.sh as `NatronRenderer -t <this file>`.
# Prints "RESULT DONE_OK" on success; any failure prints "RESULT FAILED".
import os
import sys


def out(*a):
    print(*a)
    sys.stdout.flush()


try:
    import NatronEngine
    ids = NatronEngine.natron.getPluginIDs("net.fxarena.")
    out("ARENA_PLUGIN_COUNT", len(ids))
    if not ids:
        raise RuntimeError("no net.fxarena.* plugins loaded (Arena.ofx missing or failed to load)")
    r = app.createNode("net.fxarena.openfx.ReadSVG")
    if r is None:
        raise RuntimeError("ReadSVG could not be created")
    r.getParam("filename").set(os.environ["ARENA_SVG"])
    w = app.createNode("fr.inria.built-in.Write")
    w.connectInput(0, r)
    w.getParam("filename").set(os.environ["ARENA_OUT"])
    app.render(w, 1, 1)
    dest = os.environ["ARENA_OUT"]
    size = os.path.getsize(dest) if os.path.exists(dest) else -1
    out("RENDERED_PNG_BYTES", size)
    if size <= 0:
        raise RuntimeError("no PNG written")
    # Every bundled PyPlug creates nodes by plugin ID; each must exist in this AppImage.
    # (2026-10-07: 35 PyPlugs needed the SeExpr nodes IO.ofx only has with the SeExpr library.)
    import re, collections
    pyplugs = os.environ.get("PYPLUGS_DIR")
    if pyplugs and os.path.isdir(pyplugs):
        have = set(NatronEngine.natron.getPluginIDs(""))
        needs = collections.defaultdict(set)
        rx = re.compile(r'createNode\(\s*["\']([^"\']+)["\']')
        for dp, dn, fn in os.walk(pyplugs):
            for f in fn:
                if f.endswith(".py"):
                    try:
                        txt = open(os.path.join(dp, f), encoding="utf-8", errors="ignore").read()
                    except Exception:
                        continue
                    for pid in rx.findall(txt):
                        needs[pid].add(f)
        missing = sorted(pid for pid in needs if pid not in have)
        out("PYPLUG_DEPS checked=%d missing=%d" % (len(needs), len(missing)))
        for pid in missing:
            out("PYPLUG_MISSING %s (%d pyplugs)" % (pid, len(needs[pid])))
    out("RESULT DONE_OK")
except Exception as e:  # noqa: BLE001
    import traceback
    traceback.print_exc()
    sys.stdout.flush()
    out("RESULT FAILED", repr(e))
os._exit(0)
