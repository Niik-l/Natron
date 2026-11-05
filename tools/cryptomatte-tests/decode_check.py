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

def crypto(layer, matte, preview=False, channel="alpha"):
    c = a.createNode("fr.inria.built-in.Cryptomatte")
    c.connectInput(0, read)
    lk = c.getParam("cryptoLayer")
    opts = [lk.getOption(i) for i in range(lk.getNumOptions())]
    if layer not in opts:
        log("LAYER MISSING", layer, "options:", opts)
    lk.set(layer)
    c.getParam("matteList").set(matte)
    c.getParam("previewMattes").set(preview)
    c.getParam("matteChannel").set(channel)
    return c, opts

cases = []
c, opts = crypto("ViewLayer.CryptoObject", "Cube_Left, Sphere_Mid, Suzanne_Right, Ground, Sun")
log("layer options", opts)
log("manifest info", c.getParam("manifestInfo").get())
log("manifest names", c.getParam("manifestNames").get().replace("\n", "|"))
cases.append(writer(c, "all_objects"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left")[0], "cube"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "<2ce615c5>")[0], "cube_hex"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left", channel="red")[0], "cube_red"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Sphere_Mid, Ground")[0], "sphere_ground"))
cases.append(writer(crypto("ViewLayer.CryptoMaterial", "GreenMat")[0], "greenmat"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left\nSphere_Mid")[0], "cube_sphere"))
cases.append(writer(crypto("ViewLayer.CryptoAsset", "HeroGroup")[0], "herogroup"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_Left", preview=True)[0], "preview"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "NoSuchObject")[0], "nothing"))
# wildcards expand through the manifest
cases.append(writer(crypto("ViewLayer.CryptoObject", "Cube_*")[0], "glob_cube"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "?ube_Left")[0], "glob_q_cube"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "*")[0], "glob_all"))
cg, _ = crypto("ViewLayer.CryptoObject", "S*")
cases.append(writer(cg, "glob_s"))
cases.append(writer(crypto("ViewLayer.CryptoObject", "Sphere_Mid, Suzanne_Right, Sun")[0], "s_names"))
# the generated expression: one term pair per ID per rank plane, the glob expanded to 3 IDs
import struct
cube_f = "%.9g" % struct.unpack(">f", bytes.fromhex("2ce615c5"))[0]
log("expr cube |", crypto("ViewLayer.CryptoObject", "Cube_Left")[0].getParam("expression").get().replace("\n", " "))
log("expr cube float", cube_f)
log("expr glob terms", cg.getParam("expression").get().count("ViewLayer.CryptoObject00.red =="))
log("expr empty |", crypto("ViewLayer.CryptoObject", "")[0].getParam("expression").get())

for w in cases:
    a.render(w, 1, 1)
log("RENDERS_DONE", len(cases))
sys.stdout.flush()
os._exit(0)
