"""Headless check of the VR tool nodes, run by run_vr_check.sh:

    VR_TEST_DIR=<this folder> NatronRenderer -t vr_check.py

Builds Read -> VR node -> Write(EXR) graphs over synthetic equirectangular fixtures
(make_pano_ref.py): blur wrap continuity (band on the seam vs the same band mid-frame),
sharpen overshoot, glow/expoglow threshold isolation, depth-driven blur (sharp at the
focus depth, blurred far) and light wrap (brighter than the plain composite at the fg
edge). run_vr_check.sh asserts the outputs with oiiotool.
"""
import os
import sys

HERE = os.environ.get("VR_TEST_DIR")
if not HERE:
    print("VR_TEST_DIR not set")
    sys.stdout.flush()
    os._exit(2)
OUT = HERE + "/out"

a = app

def log(*m):
    print("VR", *m)
    sys.stdout.flush()

def setp(node, name, value):
    p = node.getParam(name)
    if p is None:
        log("NOPARAM", node.getScriptName(), name)
        return
    p.setValue(value)

def reader(name):
    r = a.createReader(OUT + "/" + name)
    if r is None:
        log("FAILED to create reader for", name)
    return r

def writer(node, name):
    w = a.createWriter(OUT + "/" + name + ".exr")
    w.connectInput(0, node)
    w.getParam("bitDepth").set("16f")
    try:
        w.getParam("ocioOutputSpaceIndex").set("linear")
    except Exception:
        pass
    return w

renders = []

def render(w, name):
    renders.append((name, w))

# ---- 1. Blur VR: seam band vs mid band (wrap continuity)
rSeam = reader("band_seam.png")
blurSeam = a.createNode("fr.inria.built-in.VRBlur")
blurSeam.connectInput(0, rSeam)
setp(blurSeam, "size", 25.0)
setp(blurSeam, "sizeXFactor", 1.0)
setp(blurSeam, "quality", 1)
setp(blurSeam, "latitudeComp", True)
setp(blurSeam, "mix", 1.0)
render(writer(blurSeam, "blur_seam"), "blur_seam")

rMid = reader("band_mid.png")
blurMid = a.createNode("fr.inria.built-in.VRBlur")
blurMid.connectInput(0, rMid)
setp(blurMid, "size", 25.0)
setp(blurMid, "mix", 1.0)
render(writer(blurMid, "blur_mid"), "blur_mid")

# ---- 2. Blur VR flat (latitude compensation off) must still wrap
blurFlat = a.createNode("fr.inria.built-in.VRBlur")
blurFlat.connectInput(0, rSeam)
setp(blurFlat, "size", 25.0)
setp(blurFlat, "latitudeComp", False)
render(writer(blurFlat, "blur_seam_flat"), "blur_seam_flat")

# ---- 3. Sharpen VR: overshoot on the band edge
shp = a.createNode("fr.inria.built-in.VRSharpen")
shp.connectInput(0, rMid)
setp(shp, "amount", 2.0)
setp(shp, "size", 5.0)
render(writer(shp, "sharpen"), "sharpen")

# ---- 4. Glow VR / ExpoGlow VR: threshold isolation
rGlow = reader("glow_src.png")
glow = a.createNode("fr.inria.built-in.VRGlow")
glow.connectInput(0, rGlow)
setp(glow, "size", 20.0)
setp(glow, "threshold", 0.5)
setp(glow, "softness", 0.0)
setp(glow, "gain", 1.0)
setp(glow, "glowOnly", False)
render(writer(glow, "glow"), "glow")

exglow = a.createNode("fr.inria.built-in.VRExpoGlow")
exglow.connectInput(0, rGlow)
setp(exglow, "size", 20.0)
setp(exglow, "threshold", 0.5)
setp(exglow, "softness", 0.0)
render(writer(exglow, "expoglow"), "expoglow")

# ---- 5. Depth Blur VR: sharp at the focus depth, blurred far
rDepth = reader("depth_src.png")
rRamp = reader("depth_ramp.png")
dblur = a.createNode("fr.inria.built-in.VRDepthBlur")
dblur.connectInput(0, rDepth)
dblur.connectInput(1, rRamp)
setp(dblur, "depthSource", 1)  # Depth input
setp(dblur, "size", 30.0)
setp(dblur, "focus", 0.1)
setp(dblur, "range", 0.8)
setp(dblur, "mix", 1.0)
render(writer(dblur, "depthblur"), "depthblur")

# ---- 6. Light Wrap VR: wrap vs no-wrap composite
rFg = reader("lw_fg.png")
rBg = reader("lw_bg.png")
lw = a.createNode("fr.inria.built-in.VRLightWrap")
lw.connectInput(0, rFg)
lw.connectInput(1, rBg)
setp(lw, "wrapAmount", 0.5)
setp(lw, "wrapSize", 40.0)
setp(lw, "edgeSoftness", 10.0)
render(writer(lw, "lightwrap"), "lightwrap")

lwOff = a.createNode("fr.inria.built-in.VRLightWrap")
lwOff.connectInput(0, rFg)
lwOff.connectInput(1, rBg)
setp(lwOff, "wrapAmount", 0.0)
render(writer(lwOff, "lightwrap_off"), "lightwrap_off")

for name, w in renders:
    log("render", name)
    a.render(w, 1, 1)

log("DONE_OK")
sys.stdout.flush()
os._exit(0)