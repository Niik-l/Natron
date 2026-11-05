"""Three ScanlineRender Writes rendered at the same time (the 2026-10-21 report: start two or three
together and one fails "Rendering Failed" at the multisample framebuffer). Each concurrent render
owns a GL context and an RGBA32F multisample target of the output size, so this uses the 4K_DCP
format, Antialiasing High (8 samples) and every AOV to make the three renders as heavy as possible.
ScanlineRender now steps the sample count down when the driver refuses the framebuffer; all three
Writes must produce a full-size image. The shell script checks the outputs and reports any
"[ScanlineRender]" fallback lines from the log.

The background renderer has OpenGL rendering off by default (Preferences > GPU rendering =
Disabled If Background), so the script turns the preference on for this process first.

    SCANLINE_TEST_DIR=<this folder> NatronRenderer -t concurrent_check.py
"""
import os, sys, traceback

HERE = os.environ.get("SCANLINE_TEST_DIR") or os.getcwd()
OUT = HERE + "/out"
a = app1 if 'app1' in globals() else app

def log(*m):
    print("SLR", *m)
    sys.stdout.flush()

def setp(node, name, value):
    p = node.getParam(name)
    if p is None:
        log("no param", node.getScriptName(), name)
        return False
    p.set(value)
    return True

try:
    import NatronEngine
    gl = NatronEngine.natron.getSettings().getParam("enableOpenGLRendering")
    log("gl pref before |", gl.get())
    gl.set("enabled")
    log("gl pref after |", gl.get())
    mc = NatronEngine.natron.getSettings().getParam("maxOpenGLContexts")
    log("max GL contexts |", mc.get() if mc else "?")

    fmt = a.getProjectParam("outputFormat")
    opts = [fmt.getOption(i) for i in range(fmt.getNumOptions())]
    big = [o for o in opts if o.startswith("4K_DCP")]
    if big:
        fmt.set(big[0])
    log("format |", fmt.get(), "| 4K option", big)

    sphere = a.createNode("fr.inria.built-in.Sphere3D")
    card = a.createNode("fr.inria.built-in.Card3D")
    setp(card, "translateZ", -2.0)
    setp(card, "uniformScale", 4.0) or setp(card, "scaleX", 4.0)
    light = a.createNode("fr.inria.built-in.Light3D")
    setp(light, "intensity", 20.0)
    setp(light, "translateY", 4.0)
    setp(light, "translateZ", 4.0)
    cam = a.createNode("fr.inria.built-in.Camera3D")
    setp(cam, "translateZ", 6.0)
    scene = a.createNode("fr.inria.built-in.Scene3D")
    for i, n in enumerate([sphere, card, light]):
        log("scene input", i, n.getScriptName(), scene.connectInput(i, n))

    writers = []
    for i in range(3):
        slr = a.createNode("fr.inria.built-in.ScanlineRender")
        log("slr connect", i, slr.connectInput(1, scene), slr.connectInput(2, cam))
        setp(slr, "antialiasing", 3)   # High = 8 samples
        setp(slr, "outputWidth", 4096)  # the render size is the node's own, not the project format
        setp(slr, "outputHeight", 2160)
        for k in ("outputNormal", "outputUV", "outputPref", "outputVelocity"):
            setp(slr, k, True)
        w = a.createWriter(OUT + "/slr_%d.exr" % i)
        w.connectInput(0, slr)
        w.getParam("bitDepth").set("32f")
        writers.append(w)
    log("rendering 3 writers at once")
    a.render([(w, 1, 1) for w in writers])   # one task list = the three renders run together
    log("RENDERS_DONE")
except Exception:
    traceback.print_exc()
sys.stdout.flush()
os._exit(0)
