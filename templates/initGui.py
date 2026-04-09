# Natron Templates — Menu Commands
# Copy this file to ~/.Natron/initGui.py to register templates in the menu bar.
# Templates appear under: Menu bar > Templates > ...

from NatronGui import *
from NatronEngine import *


def _createTemplate3DBasic():
    app = natron.getGuiInstance(0)
    if not app:
        return

    sphere = app.createNode("fr.inria.built-in.Sphere3D")
    light = app.createNode("fr.inria.built-in.Light3D")
    scene = app.createNode("fr.inria.built-in.Scene3D")
    camera = app.createNode("fr.inria.built-in.Camera3D")
    cycles = app.createNode("fr.inria.built-in.CyclesRender")
    viewer = app.createNode("fr.inria.built-in.Viewer")

    scene.connectInput(0, sphere)
    scene.connectInput(1, light)
    cycles.connectInput(0, camera)
    cycles.connectInput(1, scene)
    viewer.connectInput(0, cycles)

    sphere.setPosition(-100, -200)
    light.setPosition(100, -200)
    scene.setPosition(0, -50)
    camera.setPosition(-200, 100)
    cycles.setPosition(0, 100)
    viewer.setPosition(0, 250)

    camera.getParam("translateZ").setValue(5.0)


def _createTemplateVDBFire():
    app = natron.getGuiInstance(0)
    if not app:
        return

    vdb = app.createNode("fr.inria.built-in.ReadVDB")
    light = app.createNode("fr.inria.built-in.Light3D")
    scene = app.createNode("fr.inria.built-in.Scene3D")
    camera = app.createNode("fr.inria.built-in.Camera3D")
    cycles = app.createNode("fr.inria.built-in.CyclesRender")
    viewer = app.createNode("fr.inria.built-in.Viewer")

    scene.connectInput(0, vdb)
    scene.connectInput(1, light)
    cycles.connectInput(0, camera)
    cycles.connectInput(1, scene)
    viewer.connectInput(0, cycles)

    vdb.setPosition(-100, -200)
    light.setPosition(100, -200)
    scene.setPosition(0, -50)
    camera.setPosition(-200, 100)
    cycles.setPosition(0, 100)
    viewer.setPosition(0, 250)

    light.getParam("lightType").setValue(4)
    light.getParam("intensity").setValue(1.0)
    camera.getParam("translateZ").setValue(5.0)


def _createTemplateParticles():
    app = natron.getGuiInstance(0)
    if not app:
        return

    emitter = app.createNode("fr.inria.built-in.ParticleEmitter")
    gravity = app.createNode("fr.inria.built-in.ParticleGravity")
    solver = app.createNode("fr.inria.built-in.ParticleSolver")
    scene = app.createNode("fr.inria.built-in.Scene3D")
    camera = app.createNode("fr.inria.built-in.Camera3D")
    scanline = app.createNode("fr.inria.built-in.ScanlineRender")
    viewer = app.createNode("fr.inria.built-in.Viewer")

    gravity.connectInput(0, emitter)
    solver.connectInput(0, gravity)
    scene.connectInput(0, solver)
    scanline.connectInput(0, camera)
    scanline.connectInput(1, scene)
    viewer.connectInput(0, scanline)

    emitter.setPosition(0, -350)
    gravity.setPosition(0, -200)
    solver.setPosition(0, -50)
    scene.setPosition(0, 100)
    camera.setPosition(-200, 250)
    scanline.setPosition(0, 250)
    viewer.setPosition(0, 400)

    emitter.getParam("translateY").setValue(2.0)
    camera.getParam("translateZ").setValue(8.0)


# Register menu commands
natron.addMenuCommand("Templates/Basic 3D Scene", "_createTemplate3DBasic")
natron.addMenuCommand("Templates/VDB Fire-Smoke", "_createTemplateVDBFire")
natron.addMenuCommand("Templates/Particle System", "_createTemplateParticles")
