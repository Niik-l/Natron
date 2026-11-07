#!/usr/bin/env python
"""Generate the synthetic equirectangular fixtures for the VR tools check (pure stdlib,
16-bit PNG via struct+zlib — same trick as tools/geoio-tests/probe_iso.py).

All fixtures are 1024x512 (2:1 equirectangular):
  band_seam.png  vertical white band on the 0/360 wrap seam (columns 0..15)
  band_mid.png   the same band in the middle (columns 496..511)
  glow_src.png   a centered white square (the glow/threshold source)
  depth_src.png  two bands: one at the focus depth (x~200), one far (x~824)
  depth_ramp.png depth ramp value = x/W (0..1 left to right)
  lw_fg.png      RGBA: opaque dark square centered, rest transparent (light-wrap fg)
  lw_bg.png      RGB: flat bright background (the light to wrap)

Usage: python make_pano_ref.py <out dir>
"""
import os
import struct
import sys
import zlib

W, H = 1024, 512


def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png(path, width, height, color_type, bit16_rows):
    # color_type: 0 = gray, 2 = RGB, 6 = RGBA; bit depth 16.
    # bit16_rows: list of rows, each a bytes of big-endian 16-bit samples.
    raw = bytearray()
    for row in bit16_rows:
        raw.append(0)  # filter: none
        raw += row
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 16, color_type, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def gray_row(fn):
    row = bytearray()
    for x in range(W):
        v = int(max(0.0, min(1.0, fn(x))) * 65535.0 + 0.5)
        row += struct.pack(">H", v)
    return bytes(row)


def rgba_row(fn):
    row = bytearray()
    for x in range(W):
        r, g, b, a = fn(x)
        for v in (r, g, b, a):
            row += struct.pack(">H", int(max(0.0, min(1.0, v)) * 65535.0 + 0.5))
    return bytes(row)


def band_columns(lo, hi):  # [lo, hi) white
    return lambda x: 1.0 if lo <= x < hi else 0.0


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(out):
        os.makedirs(out)

    # Seam-centered band: 8 columns on each side of the 0/360 wrap, so the blur blob
    # is symmetric around the seam and the two edge strips are comparable.
    def seam_band(x):
        return 1.0 if (x >= W - 8 or x < 8) else 0.0
    rows = [gray_row(seam_band) for _ in range(H)]
    write_png(os.path.join(out, "band_seam.png"), W, H, 0, rows)

    rows = [gray_row(band_columns(496, 512)) for _ in range(H)]
    write_png(os.path.join(out, "band_mid.png"), W, H, 0, rows)

    def glow_square(x):
        return 1.0 if 480 <= x < 544 else 0.0
    rows = [gray_row(glow_square) for _ in range(H)]
    # limit the square vertically: rows outside 224..287 are black
    rows = [row if 224 <= y < 288 else gray_row(lambda x: 0.0) for y, row in enumerate(rows)]
    write_png(os.path.join(out, "glow_src.png"), W, H, 0, rows)

    def depth_bands(x):
        v = 0.0
        if 192 <= x < 208:
            v = 1.0
        elif 816 <= x < 832:
            v = 1.0
        return v
    rows = [gray_row(depth_bands) for _ in range(H)]
    write_png(os.path.join(out, "depth_src.png"), W, H, 0, rows)

    rows = [gray_row(lambda x: float(x) / W) for _ in range(H)]
    write_png(os.path.join(out, "depth_ramp.png"), W, H, 0, rows)

    def fg(x, y):
        inside = (448 <= x < 576) and (192 <= y < 320)
        return (0.1, 0.1, 0.1, 1.0) if inside else (0.0, 0.0, 0.0, 0.0)
    rows = [rgba_row(lambda x: fg(x, y)) for y in range(H)]
    write_png(os.path.join(out, "lw_fg.png"), W, H, 6, rows)

    # RGB, not RGBA: write as color type 2
    rows = []
    for _ in range(H):
        row = bytearray()
        for x in range(W):
            for _c in range(3):
                row += struct.pack(">H", int(0.9 * 65535.0 + 0.5))
        rows.append(bytes(row))
    write_png(os.path.join(out, "lw_bg.png"), W, H, 2, rows)

    print("PANO_FIXTURES_OK " + out)


if __name__ == "__main__":
    main()
