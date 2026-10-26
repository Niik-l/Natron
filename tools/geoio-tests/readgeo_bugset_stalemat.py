# Bug 3 verify: same ReadGeo in single mode — swap mixamo.fbx (textured,
# multi-material) to the material-less liquid FBX sequence and render: the
# previous file's texture must NOT ride along.
import os, sys, time

T0 = [time.time()]
def emit(m):
    sys.stderr.write("BG3 %7.2fs %s\n" % (time.time() - T0[0], m))
    sys.stderr.flush()

ASSET = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\assets"
SEQS = r"C:\Users\filmc\Documents\OpenCode_realflow-mcp\spikes\liquid_out\out"
OUTD = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\out\bugset"
os.makedirs(OUTD, exist_ok=True)

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

def render_one(tag, fr, samples=16):
    p = os.path.join(OUTD, "%s_f%04d.png" % (tag, fr))
    if os.path.isfile(p): os.remove(p)
    w = app.createWriter(p)
    w.connectInput(0, cyc)
    app.render(w, fr, fr)
    size0 = -1
    for _ in range(480):
        if os.path.isfile(p) and os.path.getsize(p) > 4096 and os.path.getsize(p) == size0:
            break
        if os.path.isfile(p): size0 = os.path.getsize(p)
        time.sleep(0.25)
    time.sleep(0.5)
    ok = os.path.isfile(p) and os.path.getsize(p) > 0
    emit("%s: %s (lit=%d)" % (tag, "ok" if ok else "MISSING", litness(p) if ok else -1))
    return p

try:
    n = app.createNode("fr.inria.built-in.ReadGeo")
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
    cyc.getParam("samples").setValue(24)
    cyc.connectInput(1, scene)
    cyc.connectInput(2, camera)

    n.getParam("filename").set(os.path.join(ASSET, "mixamo.fbx"))
    time.sleep(1.0)
    try:
        emit("before swap tex knob: ##:%s" % (str(n.getParam("textureFile").get()),))
    except Exception: pass
    render_one("bg3_mixamo", 1, 24)

    # THE SWAP: same node, material-less FBX sequence.
    n.getParam("filename").set(os.path.join(SEQS, "fbx_seq", "liquid_#####.fbx"))
    time.sleep(1.0)
    emit("after swap info: %s" % (str(n.getParam("info").get()),))
    render_one("bg3_swap_liquid", 10, 24)
except Exception as e:
    emit("EXCEPTION: %r" % e)
emit("BG3 RESULT DONE_OK")
