#!/usr/bin/env python3
"""Compare a region of a guest screenshot with a host-rendered PNG.

Usage:
  tools/png-compare.py GUEST.png X Y HOST.png [DIFF.png]

Compares HOST.png with the same-sized region of GUEST.png whose top-left
corner is X,Y, and prints the mean and maximum channel difference and the
share of pixels that differ by more than 8 levels. DIFF.png, when given,
gets an image of the differences (amplified 4x).

Pure Python (zlib only), for 8-bit RGB and RGBA PNGs.
"""

import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat = 8, b""
    width = height = ctype = None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                raise SystemExit(f"{path}: only 8-bit RGB/RGBA")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    # Return RGB triples per row.
    out = []
    for line in rows:
        out.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(width)])
    return width, height, out


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main(argv):
    if len(argv) not in (5, 6):
        raise SystemExit(__doc__)
    gw, gh, guest = read_png(argv[1])
    x0, y0 = int(argv[2]), int(argv[3])
    hw, hh, host = read_png(argv[4])
    if x0 + hw > gw or y0 + hh > gh:
        raise SystemExit("the host image does not fit in the guest image there")
    total = maxd = big = 0
    diff_rows = []
    for y in range(hh):
        drow = []
        for x in range(hw):
            g = guest[y0 + y][x0 + x]
            h = host[y][x]
            d = max(abs(g[0] - h[0]), abs(g[1] - h[1]), abs(g[2] - h[2]))
            total += d
            maxd = max(maxd, d)
            if d > 8:
                big += 1
            v = min(255, d * 4)
            drow.append((v, v, v))
        diff_rows.append(drow)
    n = hw * hh
    print(f"{hw}x{hh} at {x0},{y0}: mean diff {total / n:.2f}, max {maxd}, "
          f"{100.0 * big / n:.2f}% of pixels differ by more than 8")
    if len(argv) == 6:
        write_png(argv[5], hw, hh, diff_rows)


if __name__ == "__main__":
    main(sys.argv)
