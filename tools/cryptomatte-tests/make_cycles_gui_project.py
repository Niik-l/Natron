"""Builds a Cycles + Cryptomatte project for a GUI pass (same scene as encode_check.py).
Run: CRYPTO_TEST_DIR=<this folder> NatronRenderer -t make_cycles_gui_project.py"""
import os, sys
HERE = os.environ["CRYPTO_TEST_DIR"]
a = app1 if 'app1' in globals() else app
def setp(node, name, value, dim=None):
    p = node.getParam(name)
    if p is not None:
        p.setValue(value) if dim is None else p.setValue(value, dim)
sphere = a.createNode("fr.inria.built-in.Sphere3D"); setp(sphere, "translateX", -1.2)
group = a.createNode("fr.inria.built-in.Group3D"); group.connectInput(0, sphere)
setp(group, "cryptoAsset", "HeroAsset"); setp(group, "cryptoCustom", "hero")
cube = a.createNode("fr.inria.built-in.Cube3D"); setp(cube, "translateX", 1.2); setp(cube, "cryptoCustom", "hero")
mat = a.createNode("fr.inria.built-in.Material3D")
setp(mat, "baseColor", 0.8, 0); setp(mat, "baseColor", 0.1, 1); setp(mat, "baseColor", 0.1, 2)
cube.connectInput(1, mat)
card = a.createNode("fr.inria.built-in.Card3D"); setp(card, "translateZ", -2.5); setp(card, "scaleX", 8.0); setp(card, "scaleY", 8.0)
light = a.createNode("fr.inria.built-in.Light3D"); setp(light, "intensity", 25.0); setp(light, "translateX", 3.0); setp(light, "translateY", 4.0); setp(light, "translateZ", 5.0)
camera = a.createNode("fr.inria.built-in.Camera3D"); setp(camera, "translateY", 0.5); setp(camera, "translateZ", 6.0)
# scattered instances with one Cryptomatte ID each (Crypto Per Instance on the ParticleInstance)
pad = a.createNode("fr.inria.built-in.Card3D")
setp(pad, "translateZ", -2.3); setp(pad, "translateY", 1.0); setp(pad, "scaleX", 1.5); setp(pad, "scaleY", 1.0)
proto = a.createNode("fr.inria.built-in.Sphere3D")
scatter = a.createNode("fr.inria.built-in.Scatter3D"); scatter.connectInput(0, pad)
setp(scatter, "count", 8); setp(scatter, "seed", 3)
inst = a.createNode("fr.inria.built-in.ParticleInstance"); inst.connectInput(0, scatter); inst.connectInput(1, proto)
setp(inst, "scaleMultiplier", 0.25); setp(inst, "cryptoCustom", "dots"); setp(inst, "cryptoPerInstance", True)
scene = a.createNode("fr.inria.built-in.Scene3D")
for i, n in enumerate([group, cube, card, inst, light]):
    scene.connectInput(i, n)
cyc = a.createNode("fr.inria.built-in.CyclesRender"); setp(cyc, "samples", 16)
cyc.connectInput(1, scene); cyc.connectInput(2, camera)
for k in ("aovCryptoObject", "aovCryptoMaterial", "aovCryptoAsset", "aovCryptoCustom"):
    setp(cyc, k, True)
crypto = a.createNode("fr.inria.built-in.Cryptomatte"); crypto.connectInput(0, cyc)
crypto.getParam("matteList").set("hero")
x, y = cyc.getPosition(); crypto.setPosition(x, y + 120)
a.saveProjectAs(HERE + "/out/cryptomatte_cycles_gui.ntp")
print("SAVED"); sys.stdout.flush(); os._exit(0)
