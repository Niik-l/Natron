# Gather Sequences (one-file-per-frame geo) end-to-end check.
# For each format (abc/fbx/obj) x node class (ReadGeoArchive, ReadGeo):
#   - filename = '#' pattern (the file dialog's Sequence: mode result): renders
#     at frames 5 / 20 / 40 must DIFFER (the sequence plays), out-of-range
#     frames 1 / 100 must MATCH frames 2 / 50 (native clamp)
#   - filename = literal single file (File: mode): frames 5 vs 40 must MATCH
#     (static file), even though the file has one-file-per-frame siblings
# Prints GSEQ PASS/FAIL lines and "GSEQ RESULT DONE_OK" on success.
import os, re, sys, time

T0 = [time.time()]
def emit(m):
    sys.stderr.write("GSEQ %7.2fs %s\n" % (time.time() - T0[0], m))
    sys.stderr.flush()

BASE = r"C:\Users\filmc\Documents\OpenCode_Natron3D\realflow-mcp\spikes\liquid_out\out"
OUTD = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\out\gatherseq"
os.makedirs(OUTD, exist_ok=True)

FAILS = []

def check(label, cond):
    emit("%s: %s" % (label, "PASS" if cond else "FAIL"))
    if not cond:
        FAILS.append(label)

def seq_pattern_of(literal_path):
    """Dialog practice: '#' per digit of the padded frame number
    (liquid_00010.abc -> liquid_#####.abc)."""
    name = re.search(r'(\d+)(\.[^.]+)$', literal_path)
    if not name:
        raise RuntimeError("no digit run: " + literal_path)
    return literal_path[:name.start(1)] + "#" * len(name.group(1)) + name.group(2)

SEQUENCES = [
    ("abc", BASE + r"\abc_seq\liquid_00010.abc", ["geo"]),
    ("fbx", BASE + r"\fbx_seq\liquid_00010.fbx", ["geo", "arch"]),
    ("obj", BASE + r"\obj_seq\liquid_00010.obj", ["geo"]),
]
NODES = {
    "arch": "fr.inria.built-in.ReadGeoArchive",
    "geo": "fr.inria.built-in.ReadGeo",
}

FR_SEQ = [5, 20, 40]        # inside the file range 2..50
FR_CLAMPS = [(1, 2), (100, 50)]  # (render frame, expected-equals frame)

def rig(tag, nodeclass, literal_path, use_pattern):
    """Build ReadGeo(X)Archive -> Scene3D -> CyclesRender(+light+camera)."""
    geo = app.createNode(nodeclass)
    # Dialog practice: Sequence: mode stores the '#' pattern; File: mode the
    # literal single path.
    geo.getParam("filename").set(seq_pattern_of(literal_path) if use_pattern
                                 else literal_path)
    try:
        emit("%s info: %s" % (tag, str(geo.getParam("info").get())))
    except Exception as e:
        emit("%s info read failed: %r" % (tag, e))
    emit("%s file set (%s)" % (tag, "pattern" if use_pattern else "literal"))
    scene = app.createNode("fr.inria.built-in.Scene3D")
    scene.connectInput(0, geo)
    light = app.createNode("fr.inria.built-in.Light3D")
    scene.connectInput(1, light)
    light.getParam("intensity").setValue(25.0)
    light.getParam("translateX").setValue(3.0)
    light.getParam("translateY").setValue(4.0)
    light.getParam("translateZ").setValue(5.0)
    camera = app.createNode("fr.inria.built-in.Camera3D")
    camera.getParam("translateY").setValue(0.5)
    camera.getParam("translateZ").setValue(4.5)
    cyc = app.createNode("fr.inria.built-in.CyclesRender")
    cyc.getParam("samples").setValue(8)
    cyc.connectInput(1, scene)
    cyc.connectInput(2, camera)
    return geo, cyc

def render_frames(tag, scan, frames):
    outs = []
    for fr in frames:
        p = os.path.join(OUTD, "%s_f%04d.png" % (tag, fr))
        if os.path.isfile(p):
            os.remove(p)
        w = app.createWriter(p)
        w.connectInput(0, scan)
        app.render(w, fr, fr)
        # app.render dispatches on the background queue; poll for the file.
        for _ in range(120):
            if os.path.isfile(p) and os.path.getsize(p) > 0:
                break
            time.sleep(0.5)
        emit("%s frame %d rendered" % (tag, fr))
        if not os.path.isfile(p) or os.path.getsize(p) == 0:
            emit("%s frame %d: MISSING OUTPUT" % (tag, fr))
            outs.append(None)
        else:
            outs.append(p)
    return outs

def img_diff_mean(p1, p2):
    """Mean |diff| of RGB over a pixel grid, normalized 0..1. CPU Cycles renders
    of identical content are NOT bit-identical (worker-tile scheduling), so
    comparisons must be tolerance-based."""
    from qtpy.QtGui import QImage
    a = QImage(p1).convertToFormat(QImage.Format_RGB32)
    b = QImage(p2).convertToFormat(QImage.Format_RGB32)
    if a.isNull() or b.isNull() or a.width() == 0 or b.width() == 0:
        return 1.0
    w = min(a.width(), b.width())
    h = min(a.height(), b.height())
    tot = 0.0
    n = 0
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            ca = a.pixel(x, y)
            cb = b.pixel(x, y)
            tot += abs((ca >> 16 & 0xff) - (cb >> 16 & 0xff))
            tot += abs((ca >> 8 & 0xff) - (cb >> 8 & 0xff))
            tot += abs((ca & 0xff) - (cb & 0xff))
            n += 1
    return tot / (n * 3 * 255.0)

IDENT_MAX = 0.006   # <= ~1.5/255 mean difference == same content (render noise)
DIFF_MIN = 0.01     # >= mean difference == visually different content

for ext, litpath, which in SEQUENCES:
    for nshort in which:
        nodeclass = NODES[nshort]
        tag = nshort + "_" + ext
        try:
            geo, scan = rig(tag + "_seq", nodeclass, litpath, True)
            hs = render_frames(tag + "_seq", scan, FR_SEQ)
            if not all(hs):
                check(tag + " seq animates", False)
                FAILS.append(tag + " missing seq frames")
            else:
                d01 = img_diff_mean(hs[0], hs[1])
                d12 = img_diff_mean(hs[1], hs[2])
                d02 = img_diff_mean(hs[0], hs[2])
                emit("%s seq diffs: 5v20=%.4f 20v40=%.4f 5v40=%.4f"
                     % (tag, d01, d12, d02))
                check(tag + " seq animates",
                      d01 > DIFF_MIN and d12 > DIFF_MIN and d02 > DIFF_MIN)
            cl = render_frames(tag + "_seq_clamp", scan,
                               [c[0] for c in FR_CLAMPS])
            base = render_frames(tag + "_seq_base", scan, [2, 50])
            if all(cl) and all(base):
                dlow = img_diff_mean(cl[0], base[0])
                dhigh = img_diff_mean(cl[1], base[1])
                emit("%s clamp diffs: 1v2=%.4f 100v50=%.4f" % (tag, dlow, dhigh))
                check(tag + " clamp low (frame 1 == 2)", dlow < IDENT_MAX)
                check(tag + " clamp high (frame 100 == 50)", dhigh < IDENT_MAX)
            else:
                check(tag + " clamp low (frame 1 == 2)", False)
                check(tag + " clamp high (frame 100 == 50)", False)
                FAILS.append(tag + " missing clamp/base frames")
            # File: mode on the same data (literal single file) -> static.
            geo.getParam("filename").set(litpath)
            hstat = render_frames(tag + "_static", scan, [FR_SEQ[0], 40])
            if all(hstat):
                dstat = img_diff_mean(hstat[0], hstat[1])
                emit("%s static diff: 5v40=%.4f" % (tag, dstat))
                check(tag + " static (File: mode: 5 == 40)", dstat < IDENT_MAX)
            else:
                check(tag + " static (File: mode: 5 == 40)", False)
                FAILS.append(tag + " missing static frames")
            emit(tag + " done")
        except Exception as e:
            emit(tag + " EXCEPTION: %r" % e)
            FAILS.append(tag + " exception")

emit("FAILS=%r" % FAILS)
if not FAILS:
    emit("GSEQ RESULT DONE_OK")
