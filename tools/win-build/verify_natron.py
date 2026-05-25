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
                "cimg", "cycles", "grade", "shuffle", "roto"]:
        print("RESULT CHECK %-10s -> %s" % (key, find(key)))
    print("RESULT DONE_OK")
except Exception as e:
    import traceback
    print("RESULT ERROR:", e)
    traceback.print_exc()

# Exit cleanly so interpreter mode does not block on stdin.
try:
    sys.exit(0)
except SystemExit:
    pass
