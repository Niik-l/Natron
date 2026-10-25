# OBJ multi-emit phase 2 battery.
# 1. unified obj multi vs unified obj single (merged, bake ON): identical
#    (flat identity hierarchy == merged render).
# 2. synthetic two-group obj: multi emits 2 entries; excluding one via
#    Object Filter equals the single-mode Object-selection render.
# 3. obj seq multi: animates, native clamps (1==2, 100==50).
# Prints OBJM PASS/FAIL; "OBJM RESULT DONE_OK" on success.
import os, re, sys, time

T0 = [time.time()]
def emit(m):
    sys.stderr.write("OBJM %7.2fs %s\n" % (time.time() - T0[0], m))
    sys.stderr.flush()

UNIFIED = "fr.inria.built-in.ReadGeo"
SEQBASE = r"C:\Users\filmc\Documents\OpenCode_Natron3D\realflow-mcp\spikes\liquid_out\out"
A = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\assets"
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

# --- synthetic two-group obj -------------------------------------------------
BOX_VTX = [(0,0,0),(1,0,0),(1,1,0),(0,1,0),(0,0,1),(1,0,1),(1,1,1),(0,1,1)]
BOX_FACES = [(1,2,3,4),(5,6,7,8),(1,2,6,5),(2,3,7,6),(3,4,8,7),(4,1,5,8)]
def box_obj(path):
    lines = []
    for off in (0.0, 4.0):
        base = len(BOX_VTX) * (1 if off else 0)
        for (x, y, z) in BOX_VTX:
            lines.append("v %g %g %g" % (x + off, y, z))
    lines.append("g CubeA")
    for f in BOX_FACES:
        lines.append("f " + " ".join(str(i + 1) for i in f))
    lines.append("g CubeB")
    for f in BOX_FACES:
        lines.append("f " + " ".join(str(i + 9) for i in f))
    open(path, "w").write("\n".join(lines) + "\n")
    return path

TWOBOX = box_obj(A + r"\twoboxes_uni.obj")

def rig(tag, path, multi=False, extra=None):
    n = app.createNode(UNIFIED)
    if multi:
        n.getParam("outputSingleMesh").setValue(0)
    n.getParam("filename").set(path)
    time.sleep(0.3)
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
    cyc.getParam("samples").setValue(16)
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
    emit("%s ok" % tag)
    return p

# --- 1: liquid obj (single group): multi vs merged single ---------------------
try:
    lit = SEQBASE + r"\obj_seq\liquid_00010.obj"
    nu, su = rig("m1_uni_m", lit, multi=True)
    pu = render_one("m1_uni_m", su, 10)
    nl, sl = rig("m1_uni_s", lit, multi=False)   # bake ON default: merged world bake
    pl = render_one("m1_uni_s", sl, 10)
    if pu and pl:
        d = img_diff_mean(pu, pl)
        emit("m1 (single vs multi) diff=%.5f lit=%d/%d" % (d, litness(pu), litness(pl)))
        check("liquid multi == merged single", d < IDENT_MAX,
              )
    else:
        check("liquid multi == merged single", False)
except Exception as e:
    emit("m1 EXCEPTION: %r" % e); FAILS.append("m1 exception")

# --- 2: two-group obj ---------------------------------------------------------
try:
    nu2, su2 = rig("m2_uni_m", TWOBOX, multi=True)
    pm = render_one("m2_uni_m", su2, 10)
    # filter: only CubeA (subtree rule keeps its children, none here)
    nu2.getParam("objectFilter").setValue("*CubeA*")
    time.sleep(0.5)
    pf = render_one("m2_flt", su2, 10)
    # single merged reference (all groups via mesh bake)
    nl2, sl2 = rig("m2_uni_s", TWOBOX, multi=False)
    ps = render_one("m2_uni_s", sl2, 10)
    # single Object=CubeA reference
    nl2.getParam("objectPath").setValue(0)   # dropdown rebuilt on load
    time.sleep(0.5)
    psa = render_one("m2_sA", sl2, 10)
    if pm and ps:
        d = img_diff_mean(pm, ps)
        emit("m2 full multi vs merged single diff=%.5f lit=%d/%d" % (d, litness(pm), litness(ps)))
        check("twobox multi == merged single", d < IDENT_MAX)
        lk = litness(pf) if pf else 0
        check("twobox filter still renders", bool(pf) and lk > 0)
        if pf and psa:
            df = img_diff_mean(pf, psa)
            emit("m2 filter vs Object=CubeA diff=%.5f lit=%d/%d" % (df, litness(pf), litness(psa)))
            check("twobox filter == Object CubeA", df < IDENT_MAX)
        else:
            check("twobox filter == Object CubeA", False)
    else:
        check("twobox multi == merged single", False)
        check("twobox filter == Object CubeA", False)
except Exception as e:
    emit("m2 EXCEPTION: %r" % e); FAILS.append("m2 exception")

# --- 3: obj sequence multi: animates + clamps ---------------------------------
try:
    litseq = SEQBASE + r"\obj_seq\liquid_00010.obj"
    patseq = re.sub(r'(\d+)(\.obj)$', lambda m: '#' * len(m.group(1)) + m.group(2), litseq)
    n3, s3 = rig("m3_seq_m", patseq, multi=True)
    h = [render_one("m3_f%04d" % fr, s3, fr) for fr in (5, 20, 40)]
    if all(h):
        # Content guard: trivial equal matches on dark/missing-fixture frames
        # must not read as an animated sequence.
        check("obj seq multi animates",
              litness(h[0]) > 0 and litness(h[1]) > 0 and
              img_diff_mean(h[0], h[1]) > DIFF_MIN and img_diff_mean(h[1], h[2]) > DIFF_MIN)
        if litness(h[0]) == 0 or litness(h[1]) == 0:
            emit("m3 fixture gap: frames dark (seq files %s)" %
                 ("present" if os.path.isfile(litseq) else "MISSING"))
    else:
        check("obj seq multi animates", False)
    c = [render_one("m3_c%04d" % fr, s3, fr) for fr in (1, 100)]
    b = [render_one("m3_b%04d" % fr, s3, fr) for fr in (2, 50)]
    if all(c) and all(b):
        check("obj seq multi clamp low",
              litness(c[0]) > 0 and img_diff_mean(c[0], b[0]) < IDENT_MAX)
        check("obj seq multi clamp high",
              litness(c[1]) > 0 and img_diff_mean(c[1], b[1]) < IDENT_MAX)
    else:
        check("obj seq multi clamp low", False)
        check("obj seq multi clamp high", False)
except Exception as e:
    emit("m3 EXCEPTION: %r" % e); FAILS.append("m3 exception")

# --- 4: obj sequence multi == single sequence per frame -----------------------
try:
    # m4: flip the SAME node to single; the seq pattern still drives frames.
    n3.getParam("outputSingleMesh").setValue(1)
    time.sleep(0.5)
    hs = render_one("m4_s0005", s3, 5)
    hm = render_one("m4_m0005", s3, 5) if False else None
    # re-render the multi reference from a fresh multi node
    nu4, su4 = rig("m4_multi_ref", patseq, multi=True)
    hmu = render_one("m4_m0005", su4, 5)
    if hs and hmu:
        d = img_diff_mean(hs, hmu)
        emit("m4 multi-vs-single frame 5 diff=%.5f" % d)
        check("obj seq frame5 multi == single", d < IDENT_MAX)
    else:
        check("obj seq frame5 multi == single", False)
except Exception as e:
    emit("m4 EXCEPTION: %r" % e); FAILS.append("m4 exception")

emit("FAILS=%r" % FAILS)
if not FAILS:
    emit("OBJM RESULT DONE_OK")
