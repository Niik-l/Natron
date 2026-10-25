import os, re, sys, time
OUTD = r"C:\Users\filmc\Documents\OpenCode_Natron3D\Natron\tools\geoio-tests\out\gseqmini"
os.makedirs(OUTD, exist_ok=True)

OBJ = r"C:\Users\filmc\Documents\OpenCode_Natron3D\realflow-mcp\spikes\liquid_out\out\obj_seq\liquid_00010.obj"
# '#' per digit of the padded frame number (the dialog's Sequence: mode value)
seq_pat = re.sub(r'(\d+)(\.[^.]+)$', lambda m: '#' * len(m.group(1)) + m.group(2), OBJ)

geo = app.createNode("fr.inria.built-in.ReadGeo")
geo.getParam("cacheFrames").setValue(2)
geo.getParam("filename").set(seq_pat)
scene = app.createNode("fr.inria.built-in.Scene3D")
scene.connectInput(0, geo)
light = app.createNode("fr.inria.built-in.Light3D")
scene.connectInput(1, light)
light.getParam("intensity").setValue(25.0)
camera = app.createNode("fr.inria.built-in.Camera3D")
camera.getParam("translateZ").setValue(4.5)
cyc = app.createNode("fr.inria.built-in.CyclesRender")
cyc.getParam("samples").setValue(4)
cyc.connectInput(1, scene)
cyc.connectInput(2, camera)

sizes = []
for fr in range(2, 51):
    p = os.path.join(OUTD, "lrucheck_f%04d.png" % fr)
    if os.path.isfile(p):
        os.remove(p)
    w = app.createWriter(p)
    w.connectInput(0, cyc)
    app.render(w, fr, fr)
    for i in range(60):
        if os.path.isfile(p) and os.path.getsize(p) > 0:
            sizes.append(os.path.getsize(p))
            os.remove(p)
            break
        time.sleep(0.5)
    else:
        sizes.append(None)
    sys.stderr.write("LRUCHECK f%d done (%d)\n" % (fr, sizes[-1] or -1))
    sys.stderr.flush()

ok = sum(1 for s in sizes if s)
sys.stderr.write("LRUCHECK RESULT %d/49 frames rendered, monotonic-ish=%s, DONE_OK\n"
                 % (ok, ok))
