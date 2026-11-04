"""Headless acceptance test for the Cryptomatte node, run by run_check.sh:

    NatronRenderer -t decode_check.py

Decodes the Blender reference EXR (ref/blender_crypto_0001.exr, made by make_ref.py) in several
ways and writes 32-bit float EXRs to out/; run_check.sh then compares them with oiiotool.
Paths are absolute because NatronRenderer -t scripts have no __file__.
"""
import os, sys

HERE = os.environ.get("CRYPTO_TEST_DIR")
if not HERE:
    print("CRYPTO_TEST_DIR not set"); sys.stdout.flush(); os._exit(2)
REF = HERE + "/ref/blender_crypto_0001.exr"
OUT = HERE + "/out"
a = app1 if 'app1' in globals() else app

def log(*m):
    print("CHECK", *m); sys.stdout.flush()

read = a.createReader(REF)
layers = [L.getLayerName() for L in read.getAvailableLayers(-1)]
log("read layers", len(layers), [l for l in layers if "Crypto" in l])

def writer(node, name):
    w = a.createWriter(OUT + "/" + name + ".exr")
    w.connectInput(0, node)
    w.getParam("bitDepth").set("32f")
    w.getParam("ocioOutputSpaceIndex").set("linear")
    return w

def crypto(layer, matte, output="matteAlpha"):
    c = a.createNode("fr.inria.built-in.Cryptomatte")
    c.connectInput(0, read)
    lk = c.getParam("cryptoLayer")
    opts = [lk.getOption(i) for i in range(lk.getNumOptions())]
    if layer not in opts:
        log("LAYER MISSING", layer, "options:", opts)
    lk.set(layer)
    c.getParam("matteList").set(matte)
    c.getParam("output").set(output)
    return c, opts

cases = []
c, opts = crypto("ViewLayer.CryptoObject", "Cube_Left, Sphere_Mid, Suzanne_Right, Ground, Sun")
log("layer options", opts)
log("manifest info", c.getParam("manifestInfo").get())
log("manifest names", c.getParam("manifestNames").get().replace("\n", "|"))
cases.append(writer(c, "all_objects"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left")[0], "cube"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "<2ce615c5>")[0], "cube_hex"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left", "matteRGBA")[0], "cube_rgba"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Sphere_Mid, Ground")[0], "sphere_ground"))
cases.append(writer(crypto("ViewLayer.CryptoMaterial", "GreenMat")[0], "greenmat"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left\nSphere_Mid")[0], "cube_sphere"))
cases.append(writer(crypto("ViewLayer.CryptoAsset", "HeroGroup")[0], "herogroup"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left", "preview")[0], "preview"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "NoSuchObject")[0], "nothing"))

for w in cases:
    a.render(w, 1, 1)
log("RENDERS_DONE", len(cases))
sys.stdout.flush()
os._exit(0)
