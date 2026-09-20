#!/usr/bin/env python3
"""Convert an XWD dump (Xvfb -fbdir framebuffer, or `xwd` output) to PNG.

Standard library only, so it runs on any distro's stock python3. Handles the
ZPixmap TrueColor layouts Xvfb writes (16/24/32 bits per pixel, either byte
order); anything else is reported rather than mis-decoded.

    xwd2png.py Xvfb_screen0 shot.png
"""
import struct
import sys
import zlib


def read_xwd(path):
    with open(path, 'rb') as f:
        data = f.read()
    fields = struct.unpack('>25I', data[:100])
    (header_size, version, pixmap_format, depth, width, height, _xoff,
     byte_order, _unit, _bit_order, _pad, bpp, bytes_per_line, visual_class,
     rmask, gmask, bmask, _bits_per_rgb, _cmap_entries, ncolors) = fields[:20]
    if version != 7 or pixmap_format != 2:
        raise SystemExit(f'unsupported XWD: version {version}, format {pixmap_format}')
    if visual_class not in (4, 5) or bpp not in (16, 24, 32):   # TrueColor/DirectColor
        raise SystemExit(f'unsupported visual class {visual_class} / {bpp} bpp')
    offset = header_size + ncolors * 12
    return data, offset, width, height, byte_order, bpp, bytes_per_line, (rmask, gmask, bmask)


def shift_scale(mask):
    if mask == 0:
        return 0, 0
    shift = (mask & -mask).bit_length() - 1
    bits = (mask >> shift).bit_length()
    return shift, bits


def to_rgb_rows(data, offset, width, height, byte_order, bpp, bytes_per_line, masks):
    bpx = bpp // 8
    conv = []
    for m in masks:
        shift, bits = shift_scale(m)
        conv.append((m, shift, (1 << bits) - 1))
    rows = []
    for y in range(height):
        base = offset + y * bytes_per_line
        row = bytearray(width * 3)
        for x in range(width):
            p = base + x * bpx
            px = int.from_bytes(data[p:p + bpx], 'big' if byte_order else 'little')
            for c, (m, shift, maxv) in enumerate(conv):
                v = (px & m) >> shift
                row[x * 3 + c] = v * 255 // maxv if maxv else 0
        rows.append(bytes(row))
    return rows


def write_png(path, width, height, rows):
    raw = b''.join(b'\x00' + r for r in rows)

    def chunk(tag, payload):
        c = tag + payload
        return struct.pack('>I', len(payload)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    data, offset, w, h, bo, bpp, bpl, masks = read_xwd(sys.argv[1])
    write_png(sys.argv[2], w, h, to_rgb_rows(data, offset, w, h, bo, bpp, bpl, masks))
    print(f'{sys.argv[2]}: {w}x{h}, {bpp} bpp')


if __name__ == '__main__':
    main()
