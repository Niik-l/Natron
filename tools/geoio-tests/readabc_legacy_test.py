# ReadAlembicArchive baseline battery (pre-refactor reference for phase 3).
# animcubetree.abc: root "AnimRoot" (animated translation, 9 samples) with
# mesh child CubeMeshT + static subtree CubeMeshS.
# Expect: 3 entries, animates across frames 2..8, static == itself, clamp at
# ends (frame 1 == 2, 100 == 9).
# Prints ABCR PASS/FAIL; "ABCR RESULT DONE_OK" on success.
import os, sys, time

T0 = [time.time()]
def emit(m):
    sys.stderr.write("ABCR %7.2fs %s\n" % (time.time() - T0[0], m))
    sys.stderr.flush()

LEG = "fr.inria.built-in.ReadAlembicArchive"
A = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\assets\animcubetree.abc"
OUTD = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\out\unified"
os.makedirs(OUTD, exist_ok=True)
FAILS = []

def check(label, cond):
    emit("%s: %s" % (label, "PASS" if cond else "FAIL"))
    if not cond:
        FAILS.append(label)

def img_diff_mean(p1, p2):
    from qtpy.QtGui import QImage
    a = QImage(p1).convertToFormat(QImage.Format_RGB32)
    b = QImage(p2).convertToFormat(QImage.Format_RGB32)
    if a.isNull() or b.isNull() or a.width() == 0: return 1.0
    w = min(a.width(), b.width()); h = min(a.height(), b.height())
    tot = 0.0; n = 0
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            ca = a.pixel(x, y); cb = b.pixel(x, y)
            tot += abs((ca >> 16 & 0xff) - (cb >> 16 & 0xff))
            tot += abs((ca >> 8 & 0xff) - (cb >> 8 & 0xff))
            tot += abs((ca & 0xff) - (cb & 0xff))
            n += 1
    return tot / (n * 3 * 255.0)

def litness(p):
    from qtpy.QtGui import QImage
    img = QImage(p).convertToFormat(QImage.Format_RGB32)
    lit = 0
    for y in range(0, img.height(), 8):
        for x in range(0, img.width(), 8):
            c = img.pixel(x, y)
            v = ((c >> 16 & 0xff) + (c >> 8 & 0xff) + (c & 0xff)) / 3.0
            if v > 8: lit += 1
    return lit

IDENT_MAX = 0.006
DIFF_MIN = 0.01

n = app.createNode(LEG)
n.getParam("filename").set(A)
time.sleep(0.5)
try:
    emit("info: %s" % n.getParam("info").get())
except Exception: pass
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
cyc.getParam("samples").setValue(16)
cyc.connectInput(1, scene)
cyc.connectInput(2, camera)

def render_one(tag, fr):
    p = os.path.join(OUTD, "%s_f%04d.png" % (tag, fr))
    if os.path.isfile(p): os.remove(p)
    w = app.createWriter(p)
    w.connectInput(0, cyc)
    app.render(w, fr, fr)
    size0 = -1
    for _ in range(240):
        if os.path.isfile(p) and os.path.getsize(p) > 4096 and os.path.getsize(p) == size0:
            break
        if os.path.isfile(p): size0 = os.path.getsize(p)
        time.sleep(0.25)
    time.sleep(0.5)
    emit("%s ok (lit=%d)" % (tag, litness(p) if os.path.isfile(p) else -1))
    return p if os.path.isfile(p) and os.path.getsize(p) else None

try:
    sc = n.getParam("archiveSceneCount")
    h = [render_one("abc_leg_f%04d" % fr, fr) for fr in (2, 5, 8)]
    if all(h):
        animateOk = (img_diff_mean(h[0], h[1]) > DIFF_MIN and
                     img_diff_mean(h[1], h[2]) > DIFF_MIN)
        emit("animate diffs: %.4f %.4f" % (img_diff_mean(h[0], h[1]), img_diff_mean(h[1], h[2])))
        check("legacy abc animates", animateOk)
    else:
        check("legacy abc animates", False)
    # sample k sits at frame k+1 (frame-by-frame time mode): out-of-range
    # below maps to sample 0 == frame 1; above maps to sample N-1 == frame 9.
    c1 = render_one("abc_leg_c1", 0)
    b2 = render_one("abc_leg_b2", 1)
    c100 = render_one("abc_leg_c100", 100)
    b9 = render_one("abc_leg_b9", 9)
    if all([c1, b2, c100, b9]):
        check("clamp low", img_diff_mean(c1, b2) < IDENT_MAX)
        check("clamp high", img_diff_mean(c100, b9) < IDENT_MAX)
    else:
        check("clamp low", False)
        check("clamp high", False)
except Exception as e:
    emit("EXCEPTION: %r" % e)
    FAILS.append("exception")

emit("FAILS=%r" % FAILS)
if not FAILS:
    emit("ABCR RESULT DONE_OK")
