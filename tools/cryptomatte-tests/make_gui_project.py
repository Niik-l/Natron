"""Builds a project for a GUI pass: Read(reference EXR) -> Cryptomatte, saved next to the EXR.
Run: CRYPTO_TEST_DIR=<this folder> NatronRenderer -t make_gui_project.py"""
import os, sys
HERE = os.environ["CRYPTO_TEST_DIR"]
a = app1 if 'app1' in globals() else app
r = a.createReader(HERE + "/ref/blender_crypto_0001.exr")
c = a.createNode("fr.inria.built-in.Cryptomatte")
c.connectInput(0, r)
c.getParam("matteList").set("Sphere_Mid")
c.setPosition(r.getPosition()[0], r.getPosition()[1] + 120)
a.saveProjectAs(HERE + "/out/cryptomatte_gui.ntp")
print("SAVED", HERE + "/out/cryptomatte_gui.ntp"); sys.stdout.flush(); os._exit(0)
