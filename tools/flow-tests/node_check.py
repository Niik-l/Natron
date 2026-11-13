"""Headless check of the VectorGenerator node on the sliding sequence from make_frames.py:

    FLOW_TEST_DIR=<folder holding slide.####.png> NatronRenderer -t node_check.py

Writes forward.0001.exr (expected vector +6, -3) and backward.0002.exr (expected -6, +3) to
$FLOW_TEST_DIR; compare with `oiiotool --stats` over an interior crop.
NatronRenderer -t scripts have no __file__, hence the environment variable.
"""
import os, sys

HERE = os.environ.get("FLOW_TEST_DIR")
if not HERE:
    print("FLOW_TEST_DIR not set"); sys.stdout.flush(); os._exit(2)
a = app1 if 'app1' in globals() else app


def log(*m):
    print("CHECK", *m); sys.stdout.flush()


read = a.createReader(HERE + "/slide.####.png")
log("read", read.getScriptName())


def case(name, output, frame, detail):
    v = a.createNode("fr.inria.built-in.VectorGenerator")
    if v is None:
        log("NODE MISSING"); os._exit(3)
    v.connectInput(0, read)
    v.getParam("output").set(output)
    v.getParam("vectorDetail").set(detail)
    w = a.createWriter(HERE + "/" + name + ".####.exr")
    w.connectInput(0, v)
    w.getParam("bitDepth").set("32f")
    w.getParam("ocioOutputSpaceIndex").set("linear")
    a.render(w, frame, frame)
    log("rendered", name, frame)


case("forward", "forward", 1, 1.0)
case("forward_half", "forward", 1, 0.5)
case("backward", "backward", 2, 1.0)
log("DONE")
