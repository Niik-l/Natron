"""Converts an image to a binary 8-bit grey PGM for flo_eval (weights 0.3 / 0.6 / 0.1, the
VectorGenerator defaults). Needs PySide6.

    python to_pgm.py <in.png> <out.pgm>
"""
import sys
from PySide6.QtGui import QImage, QGuiApplication

app = QGuiApplication(sys.argv[:1])
img = QImage(sys.argv[1]).convertToFormat(QImage.Format_RGB888)
w, h = img.width(), img.height()
out = bytearray(w * h)
for y in range(h):
    row = bytes(img.constScanLine(y))[:w * 3]
    for x in range(w):
        out[y * w + x] = min(255, int(0.3 * row[3 * x] + 0.6 * row[3 * x + 1] + 0.1 * row[3 * x + 2] + 0.5))
with open(sys.argv[2], "wb") as f:
    f.write(b"P5\n%d %d\n255\n" % (w, h))
    f.write(out)
