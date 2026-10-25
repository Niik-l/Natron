# Alembic unified ReadGeo multi-emit battery (phase 3).
# animcubetree.abc parity: unified ReadGeo (multi, bake ignored) vs legacy
# ReadAlembicArchive — same pixels per frame; animates; clamps 0==1, 100==9;
# single-mode unified abc still equals legacy ReadGeo abc single.
# Prints ABCU PASS/FAIL; " ABCU RESULT DONE_OK" on success.
import os, sys, time

T0 = [time.time()]
def emit(m):
    sys.stderr.write(" ABCU 6.2fs %s\n" % (time.time() - T0[0], 0)) if False else None
    sys.stderr.write("ABCU %7.2fs %s\n" % (time.time() - T0[0], m))
    sys.stderr.flush()

UNIFIED = "fr.inria.built-in.ReadGeo"
LEGA = "fr.inria.built-in.ReadAlembicArchive"
LEGY = "fr.inria.built-in.ReadGeo"
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

def rig(tag, nodeclass, path, multi=False, samples=16):
    n = app.createNode(nodeclass)
    if multi:
        n.getParam("outputSingleMesh").setValue(0)
    n.getParam("filename").set(path)
    time.sleep(0.5)
    try:
        emit("%s info: %s" % (tag, str(n.getParam("info").get())))
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
    cyc.getParam("samples").setValue(samples)
    cyc.connectInput(1, scene)
    cyc.connectInput(2, camera)
    return n, cyc

def render_one(tag, scan, fr):
    p = os.path.join(OUTD, "%s_f%04d.png" % (tag, fr))
    if os.path.isfile(p): os.remove(p)
    w = app.createWriter(p)
    w.connectInput(0, scan)
    app.render(w, fr, fr)
    size0 = -1
    for _ in range(240):
        if os.path.isfile(p) and os.path.getsize(p) > 4096 and os.path.getsize(p) == size0:
            break
        if os.path.isfile(p): size0 = os.path.getsize(p)
        time.sleep(0.25)
    time.sleep(0.5)
    if not os.path.isfile(p) or os.path.getsize(p) == 0:
        emit("%s MISSING" % tag); return None
    emit("%s ok (lit=%d)" % (tag, litness(p)))
    return p

try:
    # parity per frame, unified multi vs legacy
    for nshort, path, leg, multi in [("u_m", A, LEGA, True)]:
        nu, su = rig("abcu_" + nshort, UNIFIED, path, multi)
        nl, sl = rig("abcu_leg", leg, path, False)
        for fr in (2, 5, 8):
            pu = render_one("abcu_m_f%04d" % fr, su, fr)
            pl = render_one("abcu_l_f%04d" % fr, sl, fr)
            if pu and pl:
                litu, litl = litness(pu), litness(pl)
                d = img_diff_mean(pu, pl)
                emit("frame %d parity: diff=%.5f lit=%d/%d" % (fr, d, litu, litl))
                if litu == 0 and litl == 0:
                    # Trivial parity (both dark) — not evidence of a match.
                    emit("frame %d skipped: both dark" % fr)
                    continue
                check("parity frame %d" % fr, d < IDENT_MAX)
            else:
                check("parity frame %d" % fr, False)
    # clamp parity on the unified node
    c = render_one("abcu_c0", su, 0)
    b = render_one("abcu_b1", su, 1)
    # Require content — identical dark frames clamp trivially.
    check("clamp low (0 == 1)",
          c and b and litness(c) > 0 and litness(b) > 0 and
          img_diff_mean(c, b) < IDENT_MAX)
    c100 = render_one("abcu_c100", su, 100)
    b9 = render_one("abcu_b9", su, 9)
    # Tail frames are dark in this asset (robust parity proof must come from
    # the clamp itself: 100 must match 9 AND stay different from mid anim).
    mid = render_one("abcu_mid", su, 5)
    check("clamp mid (100 ~ 9) pixel match",
          c100 and b9 and img_diff_mean(c100, b9) < IDENT_MAX)
    check("clamp high differs from mid (100 != 5)",
          c100 and mid and img_diff_mean(c100, mid) > DIFF_MIN)
    # animated content on the unified node
    pu5 = render_one("abcu_m_f0005", su, 5)
    pu2 = render_one("abcu_m_f0002", su, 2)
    check("unified abc animates", pu5 and pu2 and img_diff_mean(pu5, pu2) > DIFF_MIN)
    # single-mode unified abc == legacy ReadGeo single (untouched path)
    us, ss_ = rig("abcu_single", UNIFIED, A, False)
    ls, ssl = rig("abcu_legy", LEGY, A, False)
    p1 = render_one("abcu_s", ss_, 5)
    p2 = render_one("abcu_legy_s", ssl, 5)
    check("unified single == legacy single", p1 and p2 and img_diff_mean(p1, p2) < IDENT_MAX)
except Exception as e:
    emit("EXCEPTION: %r" % e)
    FAILS.append("exception")

emit("FAILS=%r" % FAILS)
if not FAILS:
    emit("ABCU RESULT DONE_OK")
