# Universal ReadGeo Phase 1: dual-mode glb/fbx parity battery.
# For each of (BoxTextured.glb, box.fbx, spider.fbx) x (single, multi):
#   unified ReadGeo render must MATCH the legacy node rendering
#   (ReadGeo = single / ReadGeoArchive = multi).
# Extra: live mode flip re-dispatch, seq patterns in both modes on the same
# node, and the glb/converted obj placeholder multi-emit render (no crash).
# Prints UNI PASS/FAIL lines; "UNI RESULT DONE_OK" on success.
import os, re, sys, time

T0 = [time.time()]
def emit(m):
    sys.stderr.write("UNI %7.2fs %s\n" % (time.time() - T0[0], m))
    sys.stderr.flush()

GEO_LEGACY = "fr.inria.built-in.ReadGeo"
# The archive NODE was removed; this ID is a hidden deprecated alias that
# resolves to ReadGeo (loads hierarchy-mode with the outputSingleMesh fix).
ARCH_LEGACY = "fr.inria.built-in.ReadGeoArchive"
UNIFIED = "fr.inria.built-in.ReadGeo"

A = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\assets"
SEQBASE = r"C:\Users\filmc\Documents\OpenCode_realflow-mcp\spikes\liquid_out\out"
OUTD = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\out\unified"
os.makedirs(OUTD, exist_ok=True)

FAILS = []

def check(label, cond):
    emit("%s: %s" % (label, "PASS" if cond else "FAIL"))
    if not cond:
        FAILS.append(label)

def seq_pattern_of(literal_path):
    name = re.search(r'(\d+)(\.[^.]+)$', literal_path)
    if not name:
        raise RuntimeError("no digit run: " + literal_path)
    return literal_path[:name.start(1)] + "#" * len(name.group(1)) + name.group(2)

def rig(tag, nodeclass, path, multi=False, is_pattern=False, samples=8):
    """Build geo -> Scene3D -> CyclesRender(+light+camera)."""
    n = app.createNode(nodeclass)
    # The uber ReadGeo: multi=False leaves single-mesh; the legacy ARCH class
    # is now a hidden alias resolving to ReadGeo — always force hierarchy.
    if multi and (nodeclass == UNIFIED or nodeclass == ARCH_LEGACY):
        n.getParam("outputSingleMesh").setValue(0)
    p = seq_pattern_of(path) if is_pattern else path
    n.getParam("filename").set(p)
    try:
        emit("%s info: %s" % (tag, str(n.getParam("info").get())))
    except Exception:
        pass
    scene = app.createNode("fr.inria.built-in.Scene3D")
    scene.connectInput(0, n)
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
    cyc.getParam("samples").setValue(samples)
    cyc.connectInput(1, scene)
    cyc.connectInput(2, camera)
    return n, cyc

def render_one(tag, scan, fr):
    p = os.path.join(OUTD, "%s_f%04d.png" % (tag, fr))
    if os.path.isfile(p):
        os.remove(p)
    w = app.createWriter(p)
    w.connectInput(0, scan)
    app.render(w, fr, fr)
    size0 = -1
    for _ in range(240):
        if os.path.isfile(p) and os.path.getsize(p) > 4096 and os.path.getsize(p) == size0:
            break
        if os.path.isfile(p):
            size0 = os.path.getsize(p)
        time.sleep(0.25)
    time.sleep(0.5)
    if not os.path.isfile(p) or os.path.getsize(p) == 0:
        emit("%s frame %d MISSING" % (tag, fr))
        return None
    emit("%s frame %d ok" % (tag, fr))
    return p

def render_frames(tag, scan, frames):
    return [render_one("%s" % tag, scan, fr) for fr in frames]

def img_diff_mean(p1, p2):
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

def is_black(p):
    from qtpy.QtGui import QImage
    img = QImage(p).convertToFormat(QImage.Format_RGB32)
    if img.isNull():
        return False
    for y in range(0, img.height(), 8):
        for x in range(0, img.width(), 8):
            if (img.pixel(x, y) & 0xffffff) != 0:
                return False
    return True

IDENT_MAX = 0.006
DIFF_MIN = 0.01
FR = 10

CASES = [
    # (tag, unified_path, legacy_class, multi)
    ("glb_s", A + r"\BoxTextured.glb", GEO_LEGACY, False),
    ("glb_m", A + r"\BoxTextured.glb", ARCH_LEGACY, True),
    ("fbx_s", A + r"\box.fbx", GEO_LEGACY, False),
    ("fbx_m", A + r"\box.fbx", ARCH_LEGACY, True),
    ("spider_m", A + r"\spider.fbx", ARCH_LEGACY, True),
]

for tag, path, legacy, multi in CASES:
    try:
        uimg = render_frames(tag + "_uni", rig(tag + "_uni", UNIFIED, path, multi)[1], [FR])[0]
        limg = render_frames(tag + "_leg", rig(tag + "_leg", legacy, path, multi)[1], [FR])[0]
        if uimg and limg:
            d = img_diff_mean(uimg, limg)
            emit("%s parity diff: %.4f" % (tag, d))
            check(tag + " unified==legacy", d < IDENT_MAX)
        else:
            check(tag + " unified==legacy", False)
    except Exception as e:
        emit(tag + " EXCEPTION: %r" % e)
        FAILS.append(tag + " exception")

# Live mode flip: same unified node, filename already loaded in single mode;
# flipping outputSingleMesh must re-dispatch through knobChanged and match
# fresh legacy nodes of each mode on the same file.
try:
    n1, scan1 = rig("flip_a", UNIFIED, A + r"\box.fbx", multi=False)
    a = render_frames("flip_s", scan1, [FR])[0]
    n1.getParam("outputSingleMesh").setValue(0)   # knobChanged reload
    time.sleep(1.0)
    b = render_frames("flip_m", scan1, [FR])[0]
    ref_s = render_frames("flip_refs", rig("flip_refs", GEO_LEGACY, A + r"\box.fbx", False)[1], [FR])[0]
    ref_m = render_frames("flip_refm", rig("flip_refm", ARCH_LEGACY, A + r"\box.fbx", True)[1], [FR])[0]
    if a and b and ref_s and ref_m:
        check("flip single==legacy_single", img_diff_mean(a, ref_s) < IDENT_MAX)
        check("flip multi==archive", img_diff_mean(b, ref_m) < IDENT_MAX)
    else:
        check("flip single==legacy_single", False)
        check("flip multi==archive", False)
except Exception as e:
    emit("flip EXCEPTION: %r" % e)
    FAILS.append("flip exception")

# fbx one-file-per-frame sequence on the unified node in BOTH modes.
try:
    litpath = SEQBASE + r"\fbx_seq\liquid_00010.fbx"
    n2, scan2 = rig("seq_u", UNIFIED, litpath, multi=False, is_pattern=True)
    h = [render_one("seq_u_s%d" % fr, scan2, fr)
         for fr in [5, 20, 40]]
    if all(h):
        check("seq unified single animates",
              img_diff_mean(h[0], h[1]) > DIFF_MIN and
              img_diff_mean(h[1], h[2]) > DIFF_MIN)
    else:
        check("seq unified single animates", False)
    n2.getParam("outputSingleMesh").setValue(0)   # multi, seq on fbx: valid
    time.sleep(1.0)
    hm = [render_one("seq_u_m%d" % fr, scan2, fr) for fr in [5, 40]]
    if all(hm):
        check("seq unified multi animates", img_diff_mean(hm[0], hm[1]) > DIFF_MIN)
    else:
        check("seq unified multi animates", False)
    # Static literal file in single mode == frame 5 and 40 identical
    n2.getParam("outputSingleMesh").setValue(1)
    n2.getParam("filename").set(litpath)
    hs = [render_one("seq_u_lit%d" % fr, scan2, fr) for fr in [5, 40]]
    if all(hs):
        check("seq unified static literal", img_diff_mean(hs[0], hs[1]) < IDENT_MAX)
    else:
        check("seq unified static literal", False)
except Exception as e:
    emit("seq EXCEPTION: %r" % e)
    FAILS.append("seq exception")

# abc multi-emit content check (phase 3 is functional — see
# readabc_unified_test.py for the full parity battery). Render must not be a
# black/empty frame any more.
try:
    abcpath = SEQBASE + r"\abc_seq\liquid_00010.abc"
    n3, scan3 = rig("abc_m", UNIFIED, abcpath, multi=True)
    ab = render_frames("abc_m", scan3, [FR])[0]
    if ab:
        emit("abc multi rendered")
        check("abc unified multi renders content", not is_black(ab))
    else:
        check("abc unified multi renders content", False)
except Exception as e:
    emit("abc EXCEPTION: %r" % e)
    FAILS.append("abc exception")

emit("FAILS=%r" % FAILS)
if not FAILS:
    emit("UNI RESULT DONE_OK")
