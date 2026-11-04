"""Headless check of the Cycles Cryptomatte ENCODER, run by run_encode_check.sh:

    CRYPTO_TEST_DIR=<this folder> NatronRenderer -t encode_check.py

Builds a small Cycles scene: a Sphere3D inside a Group3D (Crypto Asset "HeroAsset", Crypto Custom
"hero"), a Cube3D with its own Material3D and Crypto Custom "hero", an untagged Card3D as ground,
a light and a camera. CyclesRender renders the four Cryptomatte layer types; Cryptomatte nodes on
its output decode object / material / asset / custom mattes to out/*.exr (alpha), the render node
saves its multi-layer EXR (out/cycles_crypto.exr) and that file is decoded again through a Read.
run_encode_check.sh compares the results with oiiotool and checks the EXR header.
"""
import os, sys, time

HERE = os.environ.get("CRYPTO_TEST_DIR")
if not HERE:
    print("CRYPTO_TEST_DIR not set"); sys.stdout.flush(); os._exit(2)
OUT = HERE + "/out"
a = app1 if 'app1' in globals() else app

def log(*m):
    print("ENC", *m); sys.stdout.flush()

def setp(node, name, value, dim=None):
    p = node.getParam(name)
    if p is None:
        log("NOPARAM", node.getScriptName(), name); return
    if dim is None:
        p.setValue(value)
    else:
        p.setValue(value, dim)

def connect_material(geo, mat):
    for i in range(geo.getMaxInputCount()):
        lbl = (geo.getInputLabel(i) or "").lower()
        if "mat" in lbl:
            ok = geo.connectInput(i, mat); log("material input", i, lbl, ok); return
    log("NO MATERIAL INPUT on", geo.getScriptName(), [geo.getInputLabel(i) for i in range(geo.getMaxInputCount())])

# ---- scene
sphere = a.createNode("fr.inria.built-in.Sphere3D")
setp(sphere, "translateX", -1.2)
group = a.createNode("fr.inria.built-in.Group3D")
log("group connect", group.connectInput(0, sphere))
setp(group, "cryptoAsset", "HeroAsset")
setp(group, "cryptoCustom", "hero")

cube = a.createNode("fr.inria.built-in.Cube3D")
setp(cube, "translateX", 1.2)
setp(cube, "cryptoCustom", "hero")
mat = a.createNode("fr.inria.built-in.Material3D")
setp(mat, "baseColor", 0.8, 0); setp(mat, "baseColor", 0.1, 1); setp(mat, "baseColor", 0.1, 2)
connect_material(cube, mat)

card = a.createNode("fr.inria.built-in.Card3D")
setp(card, "translateZ", -2.5)   # a wall behind the sphere and cube, facing the camera
setp(card, "scaleX", 8.0); setp(card, "scaleY", 8.0)

light = a.createNode("fr.inria.built-in.Light3D")
setp(light, "intensity", 25.0); setp(light, "translateX", 3.0); setp(light, "translateY", 4.0); setp(light, "translateZ", 5.0)
camera = a.createNode("fr.inria.built-in.Camera3D")
setp(camera, "translateY", 0.5); setp(camera, "translateZ", 6.0)

scene = a.createNode("fr.inria.built-in.Scene3D")
for i, n in enumerate([group, cube, card, light]):
    log("scene input", i, n.getScriptName(), scene.connectInput(i, n))

cyc = a.createNode("fr.inria.built-in.CyclesRender")
setp(cyc, "samples", 16)
cyc.connectInput(1, scene)
cyc.connectInput(2, camera)
for k in ("aovCryptoObject", "aovCryptoMaterial", "aovCryptoAsset", "aovCryptoCustom"):
    setp(cyc, k, True)
setp(cyc, "cryptoLevels", 6)
log("scene nodes", sphere.getScriptName(), cube.getScriptName(), card.getScriptName(), group.getScriptName(), mat.getScriptName())

# ---- decoders on the live render
def writer(node, name):
    w = a.createWriter(OUT + "/" + name + ".exr")
    w.connectInput(0, node)
    w.getParam("bitDepth").set("32f")
    w.getParam("ocioOutputSpaceIndex").set("linear")
    return w

def crypto(layer, matte):
    c = a.createNode("fr.inria.built-in.Cryptomatte")
    c.connectInput(0, cyc)
    lk = c.getParam("cryptoLayer")
    opts = [lk.getOption(i) for i in range(lk.getNumOptions())]
    if layer not in opts:
        log("LAYER MISSING", layer, "options:", opts)
    lk.set(layer)
    c.getParam("matteList").set(matte)
    c.getParam("previewMattes").set(False)
    return c, opts

cases = []
c0, opts = crypto("CryptoObject", sphere.getScriptName()); log("layer options", opts)
cases.append(("obj_sphere", c0))
cases.append(("obj_cube", crypto("CryptoObject", cube.getScriptName())[0]))
cases.append(("obj_card", crypto("CryptoObject", card.getScriptName())[0]))
cases.append(("obj_all", crypto("CryptoObject", ", ".join([sphere.getScriptName(), cube.getScriptName(), card.getScriptName()]))[0]))
cases.append(("custom_hero", crypto("CryptoCustom", "hero")[0]))
cases.append(("custom_default", crypto("CryptoCustom", "default")[0]))
cases.append(("asset_hero", crypto("CryptoAsset", "HeroAsset")[0]))
cases.append(("asset_cube", crypto("CryptoAsset", cube.getScriptName())[0]))
cases.append(("mat_cube", crypto("CryptoMaterial", mat.getScriptName())[0]))
writers = [(name, writer(c, name)) for name, c in cases]
# the beauty alpha, for the all-objects comparison
writers.append(("beauty", writer(cyc, "beauty")))
for name, w in writers:
    a.render(w, 1, 1)
    log("rendered", name)

# manifests the decoders got from the render node
for name, c in cases:
    if name in ("obj_sphere", "custom_hero", "asset_hero", "mat_cube"):
        c.getParam("reloadManifest").trigger()   # the render just produced the manifests
        log("manifest", name, "|", c.getParam("manifestInfo").get(), "|", c.getParam("manifestNames").get().replace("\n", ","))

# ---- the render node's multi-layer EXR, decoded again through a Read
exr = OUT + "/cycles_crypto.exr"
if os.path.isfile(exr):
    os.remove(exr)
setp(cyc, "exrOutputPath", exr)
cyc.getParam("saveExr").trigger()
for _ in range(100):
    if os.path.isfile(exr) and os.path.getsize(exr) > 1024:
        break
    time.sleep(0.2)
log("saved exr", os.path.isfile(exr), os.path.getsize(exr) if os.path.isfile(exr) else 0)
if os.path.isfile(exr):
    rd = a.createReader(exr)
    layers = [L.getLayerName() for L in rd.getAvailableLayers(-1)]
    log("file layers", [l for l in layers if "Crypto" in l])
    c = a.createNode("fr.inria.built-in.Cryptomatte")
    c.connectInput(0, rd)
    c.getParam("cryptoLayer").set("CryptoObject")
    c.getParam("matteList").set(sphere.getScriptName())
    c.getParam("previewMattes").set(False)
    w = writer(c, "file_obj_sphere")
    a.render(w, 1, 1)
    log("file manifest |", c.getParam("manifestInfo").get(), "|", c.getParam("manifestNames").get().replace("\n", ","))
    c2 = a.createNode("fr.inria.built-in.Cryptomatte")
    c2.connectInput(0, rd)
    c2.getParam("cryptoLayer").set("CryptoCustom")
    c2.getParam("matteList").set("hero")
    c2.getParam("previewMattes").set(False)
    a.render(writer(c2, "file_custom_hero"), 1, 1)
log("ENCODE_DONE")
sys.stdout.flush()
os._exit(0)
