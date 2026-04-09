# Template: Particle System
# Creates: Emitter + Gravity + Solver + Scene + Camera + ScanlineRender + Viewer
# Run from Natron Script Editor (Edit > Script Editor, paste, Ctrl+Enter)

app = app1

emitter = app.createNode("fr.inria.built-in.ParticleEmitter")
gravity = app.createNode("fr.inria.built-in.ParticleGravity")
solver = app.createNode("fr.inria.built-in.ParticleSolver")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
scanline = app.createNode("fr.inria.built-in.ScanlineRender")
viewer = app.createNode("fr.inria.built-in.Viewer")

# Connect: emitter -> gravity -> solver -> scene, camera + scene -> scanline -> viewer
gravity.connectInput(0, emitter)
solver.connectInput(0, gravity)
scene.connectInput(0, solver)
scanline.connectInput(0, camera)
scanline.connectInput(1, scene)
viewer.connectInput(0, scanline)

# Position nodes in the node graph
emitter.setPosition(0, -350)
gravity.setPosition(0, -200)
solver.setPosition(0, -50)
scene.setPosition(0, 100)
camera.setPosition(-200, 250)
scanline.setPosition(0, 250)
viewer.setPosition(0, 400)

# Emitter above ground, shooting upward
emitter.getParam("translateY").setValue(2.0)

# Camera pulled back
camera.getParam("translateZ").setValue(8.0)

print("[Template] Particle scene created: Emitter + Gravity + Solver + ScanlineRender")
print("[Template] Press play to simulate particles. View from Solver for full sim.")
