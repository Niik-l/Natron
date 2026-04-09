# Template: VDB Fire / Smoke
# Creates: ReadVDB + Light (dome) + Scene + Camera + CyclesRender + Viewer
# Run from Natron Script Editor (Edit > Script Editor, paste, Ctrl+Enter)

app = app1

vdb = app.createNode("fr.inria.built-in.ReadVDB")
light = app.createNode("fr.inria.built-in.Light3D")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
cycles = app.createNode("fr.inria.built-in.CyclesRender")
viewer = app.createNode("fr.inria.built-in.Viewer")

# Connect: vdb + light -> scene, camera + scene -> cycles -> viewer
scene.connectInput(0, vdb)
scene.connectInput(1, light)
cycles.connectInput(0, camera)
cycles.connectInput(1, scene)
viewer.connectInput(0, cycles)

# Position nodes in the node graph
vdb.setPosition(-100, -200)
light.setPosition(100, -200)
scene.setPosition(0, -50)
camera.setPosition(-200, 100)
cycles.setPosition(0, 100)
viewer.setPosition(0, 250)

# Dome light for even illumination
light.getParam("lightType").setValue(4)  # 4 = Dome
light.getParam("intensity").setValue(1.0)

# Camera pulled back
camera.getParam("translateZ").setValue(5.0)

print("[Template] VDB Fire scene created: ReadVDB + Dome Light + CyclesRender")
print("[Template] Load a .vdb file in the ReadVDB node's File param to render")
