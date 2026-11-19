import os
import sys
try:
    import NatronEngine
    n = NatronEngine.natron
    print("RESULT NATRON_VERSION:", n.getNatronVersionString())
    ids = n.getPluginIDs()
    print("RESULT TOTAL_PLUGINS:", len(ids))

    def find(sub):
        m = [p for p in ids if sub.lower() in p.lower()]
        return m[:5]

    for key in ["read", "write", "blur", "merge", "transform",
                "cimg", "cycles", "grade", "shuffle", "roto", "oiio",
                "readsvg", "seexprsimple", "senoise", "softthreshold"]:
        print("RESULT CHECK %-10s -> %s" % (key, find(key)))

    # Qt image-format plugins (imageformats/): PySide6 scripts write JPEG/TIFF/WebP
    # through these; PNG alone means the folder is missing from the install.
    from PySide6.QtGui import QImageWriter
    fmts = sorted(bytes(f).decode() for f in QImageWriter.supportedImageFormats())
    print("RESULT QT_IMAGE_WRITE_FORMATS:", " ".join(fmts))

    # Every bundled PyPlug is a script that creates plugin nodes by ID; each ID must exist in
    # this install or the PyPlug fails on creation (2026-10-07: 35 PyPlugs silently needed the
    # SeExpr nodes that IO.ofx only has when the SeExpr library is found at build time).
    import os, re, collections
    pyplugs = os.environ.get("NATRON_PYPLUGS_DIR")
    if pyplugs and os.path.isdir(pyplugs):
        have = set(ids)
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
        print("RESULT PYPLUG_DEPS checked=%d missing=%d" % (len(needs), len(missing)))
        for pid in missing:
            print("RESULT PYPLUG_MISSING %s (%d pyplugs)" % (pid, len(needs[pid])))
    print("RESULT DONE_OK")
except Exception as e:
    import traceback
    print("RESULT ERROR:", e)
    traceback.print_exc()

# Exit cleanly so interpreter mode does not block on stdin. sys.exit() is
# swallowed by the embedded interpreter, which then starts the Python REPL;
# with stdin redirected (07-verify uses </dev/null) CPython 3.14's _pyrepl
# fails in GetConsoleScreenBufferInfo and loops, so leave the process for real.
try:
    sys.stdout.flush()
    sys.stderr.flush()
except Exception:
    pass
os._exit(0)
