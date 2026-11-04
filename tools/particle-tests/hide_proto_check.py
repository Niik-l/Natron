"""Headless check of ParticleInstance's Hide Instanced Geo, run by run_hide_proto_check.sh:

    PARTICLE_TEST_DIR=<this folder> NatronRenderer -t hide_proto_check.py

A Sphere3D prototype sits at the origin, right in front of the camera, and is wired BOTH into a
ParticleInstance (five small instances scattered on a pad above) and straight into the Scene3D,
the one case where a render would otherwise draw the source geo at its own place. CyclesRender
renders the scene with Hide Instanced Geo on (default) and off; the shell script checks the alpha
at the image centre: empty with the toggle on, covered by the source sphere with it off, while the
instances are present either way. ScanlineRender has no headless GL path (Rendering Failed under
NatronRenderer) and is covered by the GUI pass; the 3D viewport goes through the same SceneGraph
visibility flag as Cycles.
"""
import os, sys

HERE = os.environ.get("PARTICLE_TEST_DIR")
if not HERE:
    print("PARTICLE_TEST_DIR not set"); sys.stdout.flush(); os._exit(2)
OUT = HERE + "/out"
a = app1 if 'app1' in globals() else app

def log(*m):
    print("HIDE", *m); sys.stdout.flush()

def setp(node, name, value, dim=None):
    p = node.getParam(name)
    if p is None:
        log("NOPARAM", node.getScriptName(), name); return
    if dim is None:
        p.setValue(value)
    else:
        p.setValue(value, dim)

proto = a.createNode("fr.inria.built-in.Sphere3D")          # the source geo, at the origin
pad = a.createNode("fr.inria.built-in.Card3D")
setp(pad, "translateZ", -2.3); setp(pad, "translateY", 1.6); setp(pad, "scaleX", 1.5); setp(pad, "scaleY", 0.6)
scatter = a.createNode("fr.inria.built-in.Scatter3D")
log("scatter connect", scatter.connectInput(0, pad))
setp(scatter, "count", 5); setp(scatter, "seed", 3)
inst = a.createNode("fr.inria.built-in.ParticleInstance")
log("instance connect", inst.connectInput(0, scatter), inst.connectInput(1, proto))
setp(inst, "scaleMultiplier", 0.3)
log("hideInstancedGeo default", inst.getParam("hideInstancedGeo").get())

light = a.createNode("fr.inria.built-in.Light3D")
setp(light, "intensity", 25.0); setp(light, "translateX", 3.0); setp(light, "translateY", 4.0); setp(light, "translateZ", 5.0)
camera = a.createNode("fr.inria.built-in.Camera3D")
setp(camera, "translateY", 0.5); setp(camera, "translateZ", 6.0)
scene = a.createNode("fr.inria.built-in.Scene3D")
for i, n in enumerate([inst, light, proto]):   # proto also in the scene directly
    log("scene input", i, n.getScriptName(), scene.connectInput(i, n))

cyc = a.createNode("fr.inria.built-in.CyclesRender")
setp(cyc, "samples", 8)
log("cyc connect", cyc.connectInput(1, scene), cyc.connectInput(2, camera))

def writer(node, name):
    w = a.createWriter(OUT + "/" + name + ".exr")
    w.connectInput(0, node)
    w.getParam("bitDepth").set("32f")
    w.getParam("ocioOutputSpaceIndex").set("linear")
    return w

for state in ("on", "off"):
    inst.getParam("hideInstancedGeo").set(state == "on")
    for tag, node in (("cyc", cyc),):
        w = writer(node, tag + "_" + state)
        a.render(w, 1, 1)
        log("rendered", tag, state)
log("HIDE_DONE")
sys.stdout.flush()
os._exit(0)
