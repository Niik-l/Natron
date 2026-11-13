"""Writes a 3-frame PNG sequence whose content slides by a known amount per frame, for
node_check.py. Needs PySide6 (MSYS2: mingw-w64-x86_64-pyside6).

    python make_frames.py <outdir>

The picture moves 6 pixels right and 3 pixels DOWN per frame. Natron's image origin is the
bottom-left corner, so the expected forward vector is (+6, -3).
"""
import math, os, random, sys
from PySide6.QtGui import QImage, QColor, QGuiApplication

app = QGuiApplication(sys.argv[:1])
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
W, H, DX, DY = 640, 360, 6, 3
rnd = random.Random(3)
waves = [(rnd.uniform(0.01, 0.25), rnd.uniform(0, math.pi), rnd.uniform(0, 6.28), rnd.uniform(0.3, 1.0)) for _ in range(40)]


def tex(x, y):
    v = 0.0
    s = 0.0
    for f, ang, ph, amp in waves:
        v += amp * math.sin(f * (x * math.cos(ang) + y * math.sin(ang)) + ph)
        s += amp
    return 0.5 + 0.5 * v / (0.35 * s)


# one wide strip of texture, cropped at a different offset for each frame
PAD = 16
strip = [[tex(x, y) for x in range(-PAD, W + PAD)] for y in range(-PAD, H + PAD)]
for frame in range(1, 4):
    img = QImage(W, H, QImage.Format_RGB32)
    ox, oy = PAD - DX * (frame - 1), PAD - DY * (frame - 1)
    for y in range(H):
        row = strip[y + oy]
        for x in range(W):
            g = max(0, min(255, int(row[x + ox] * 255)))
            img.setPixelColor(x, y, QColor(g, g, g))
    img.save(os.path.join(out, "slide.%04d.png" % frame))
print("wrote 3 frames to", out)
