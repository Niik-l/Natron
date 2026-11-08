"""GUI check for concurrent ScanlineRender renders, the way a user does it: load the
three-ScanlineRender project, trigger Render > Render All Writers and click OK on each "same
image being overwritten" question, so the three Writes start a few hundred ms apart and finish
at different times (the case that killed the shared GL contexts' DCs, 2026-11-07, see
Engine/Dev/TODO.md). Pass = all three Writes report "Rendering finished" in the stderr log and no
"[Render] ... failed" / "[OSGLContext_win] ... failed" lines.

    SCANLINE_TEST_DIR=<this folder> NATRON_DEBUG_GLCTX=1 Natron.exe gui_render_all_check.py > log 2>&1

The project is <dir>/out/scanline_concurrent_gui.ntp (made by make_delorean-style scripts; any
project with several ScanlineRender Writes works, set SCANLINE_GUI_PROJECT to use another).
Progress of this script goes to <dir>/out/_gui_check.log because GUI print() lands in the
Script Editor. Close Natron yourself when the Progress panel shows the three Writes done.
"""
import os, sys, traceback

HERE = os.environ.get("SCANLINE_TEST_DIR") or os.getcwd()
PROJ = os.environ.get("SCANLINE_GUI_PROJECT") or (HERE + "/out/scanline_concurrent_gui.ntp")
_f = open(HERE + "/out/_gui_check.log", "a")

def log(*m):
    _f.write("GUICHECK " + " ".join(str(x) for x in m) + "\n")
    _f.flush()

try:
    from PySide6 import QtCore, QtWidgets
    log("loading", PROJ)
    a = app1.loadProject(PROJ)
    log("loadProject ->", a)

    state = {"clicked": 0, "triggered": False, "ticks": 0}

    def find_action():
        # the visible main window's menu bar: Render > Render All Writers
        for w in QtWidgets.QApplication.topLevelWidgets():
            if not isinstance(w, QtWidgets.QMainWindow) or not w.isVisible():
                continue
            mb = w.menuBar()
            if mb is None:
                continue
            for top in mb.actions():
                if top.text().replace("&", "") != "Render" or top.menu() is None:
                    continue
                for act in top.menu().actions():
                    if act.text().replace("&", "") == "Render All Writers":
                        return act
        return None

    def tick():
        state["ticks"] += 1
        try:
            if not state["triggered"]:
                act = find_action()
                if act is None:
                    return
                log("triggering Render All Writers")
                state["triggered"] = True
                act.trigger()
                return
            m = QtWidgets.QApplication.activeModalWidget()
            if m is not None:
                for b in m.findChildren(QtWidgets.QPushButton):
                    if b.text().replace("&", "").strip().lower() == "ok":
                        state["clicked"] += 1
                        log("clicking OK on dialog", state["clicked"], "|", m.windowTitle())
                        b.click()
                        return
            if state["clicked"] >= 3 or state["ticks"] > 600:
                log("done, dialogs answered:", state["clicked"])
                timer.stop()
        except Exception:
            log("tick EXC", traceback.format_exc())

    timer = QtCore.QTimer()
    timer.timeout.connect(tick)
    timer.start(500)
    log("timer armed")
except Exception:
    log("EXC", traceback.format_exc())
