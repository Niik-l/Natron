# Template: Basic 3D Scene
# Creates: Sphere + Light + Scene + Camera + CyclesRender + Viewer
# Run from Natron Script Editor (Edit > Script Editor, paste, Ctrl+Enter)

app = app1

sphere = app.createNode("fr.inria.built-in.Sphere3D")
light = app.createNode("fr.inria.built-in.Light3D")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
cycles = app.createNode("fr.inria.built-in.CyclesRender")
viewer = app.createNode("fr.inria.built-in.Viewer")

# Connect: sphere + light -> scene, camera + scene -> cycles -> viewer
scene.connectInput(0, sphere)
scene.connectInput(1, light)
cycles.connectInput(0, camera)
cycles.connectInput(1, scene)
viewer.connectInput(0, cycles)

# Position nodes in the node graph
sphere.setPosition(-100, -200)
light.setPosition(100, -200)
scene.setPosition(0, -50)
camera.setPosition(-200, 100)
cycles.setPosition(0, 100)
viewer.setPosition(0, 250)

# Set camera back so sphere is in frame
camera.getParam("translateZ").setValue(5.0)

print("[Template] Basic 3D scene created: Sphere + Light + CyclesRender")
