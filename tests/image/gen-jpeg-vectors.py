#!/usr/bin/env python3
"""Generates the JPEG test vectors in tests/image/vectors/jpeg.

Every image is synthesized here (gradients, edges and noise), encoded with
cjpeg (libjpeg-turbo) or ImageMagick, and decoded with djpeg -nosmooth
(replicated chroma, as Aura upsamples) for the reference. Orientation
references are the djpeg output rotated here.

To regenerate, run from the repository root:
    python3 tests/image/gen-jpeg-vectors.py
To make the large speed-test images (not committed) instead:
    python3 tests/image/gen-jpeg-vectors.py --big build/jpeg
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "vectors", "jpeg")


def pattern(w, h, seed=1):
    """A colourful test pattern: gradients, a ring, hard edges and noise."""
    s = seed * 2654435761 % 2**32 or 1
    px = bytearray()
    for y in range(h):
        for x in range(w):
            s ^= (s << 13) & 0xFFFFFFFF
            s ^= s >> 17
            s ^= (s << 5) & 0xFFFFFFFF
            n = (s & 31) - 16
            r = x * 255 // max(1, w - 1)
            g = y * 255 // max(1, h - 1)
            dx, dy = x - w / 2, y - h / 2
            d2 = dx * dx + dy * dy
            b = 230 if (w * h / 16) < d2 < (w * h / 7) else 40
            if (x // 6 + y // 6) % 2 and x > w * 2 // 3:
                r, g, b = 255 - r, 255 - g, 255 - b
            if y > h * 3 // 4 and x < w // 3:
                r = g = b = (x * 37) & 255
            px += bytes(max(0, min(255, v + n)) for v in (r, g, b))
    return px


def write_ppm(path, w, h, px):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(px)


def read_pnm(path):
    data = open(path, "rb").read()
    parts = data.split(maxsplit=4)
    kind, w, h = parts[0], int(parts[1]), int(parts[2])
    return kind, w, h, parts[4]


def write_pnm(path, kind, w, h, px):
    with open(path, "wb") as f:
        f.write(kind + b"\n%d %d\n255\n" % (w, h))
        f.write(px)


def run(cmd, out=None):
    with open(out, "wb") if out else open(os.devnull, "wb") as f:
        subprocess.run(cmd, stdout=f, check=True)


def djpeg(jpg, ref, scale=None):
    cmd = ["djpeg", "-nosmooth", "-pnm"]
    if scale:
        cmd += ["-scale", scale]
    run(cmd + [jpg], ref)


def exif_app1(orient, big_endian):
    """An APP1 Exif segment whose IFD0 holds Orientation and one other tag."""
    e = ">" if big_endian else "<"
    tiff = (b"MM" if big_endian else b"II") + struct.pack(e + "HI", 42, 8)
    entries = [
        struct.pack(e + "HHI", 0x010F, 2, 4) + b"Aur\0",  # Make
        struct.pack(e + "HHIHH", 0x0112, 3, 1, orient, 0),
    ]
    tiff += struct.pack(e + "H", len(entries)) + b"".join(entries) + struct.pack(e + "I", 0)
    body = b"Exif\0\0" + tiff
    return b"\xff\xe1" + struct.pack(">H", len(body) + 2) + body


def orient_pixels(w, h, px, ch, o):
    """The coded image px (w x h) as displayed with EXIF orientation o."""
    ow, oh = (h, w) if o >= 5 else (w, h)
    out = bytearray(ow * oh * ch)
    for y in range(h):
        for x in range(w):
            if o == 1:
                X, Y = x, y
            elif o == 2:
                X, Y = w - 1 - x, y
            elif o == 3:
                X, Y = w - 1 - x, h - 1 - y
            elif o == 4:
                X, Y = x, h - 1 - y
            elif o == 5:
                X, Y = y, x
            elif o == 6:
                X, Y = h - 1 - y, x
            elif o == 7:
                X, Y = h - 1 - y, w - 1 - x
            else:
                X, Y = y, w - 1 - x
            i, k = (y * w + x) * ch, (Y * ow + X) * ch
            out[k:k + ch] = px[i:i + ch]
    return ow, oh, out


# name, width, height, cjpeg arguments, scaled references
VECTORS = [
    ("base444", 48, 32, ["-quality", "90", "-sample", "1x1"], True),
    ("base422", 48, 32, ["-quality", "90", "-sample", "2x1"], False),
    ("base420", 48, 32, ["-quality", "90"], True),
    ("base411", 48, 32, ["-quality", "90", "-sample", "4x1,1x1,1x1"], False),
    ("base440", 48, 32, ["-quality", "90", "-sample", "1x2,1x1,1x1"], False),
    ("base_odd", 48, 32, ["-quality", "90", "-sample", "2x1,1x2,1x1"], False),
    ("grey", 48, 32, ["-quality", "85", "-grayscale"], True),
    ("prog420", 48, 32, ["-quality", "90", "-progressive"], True),
    ("prog444", 48, 32, ["-quality", "90", "-progressive", "-sample", "1x1"], True),
    ("prog422", 40, 24, ["-quality", "80", "-progressive", "-sample", "2x1"], False),
    ("prog411", 40, 24, ["-quality", "80", "-progressive", "-sample", "4x1,1x1,1x1"], False),
    ("prog_grey", 40, 24, ["-quality", "80", "-progressive", "-grayscale"], True),
    ("rst1", 48, 32, ["-quality", "85", "-restart", "1"], False),
    ("rst2b", 48, 32, ["-quality", "85", "-restart", "2B"], False),
    ("prog_rst", 48, 32, ["-quality", "85", "-progressive", "-restart", "2B"], False),
    ("multiscan", 40, 24, ["-quality", "85", "-scans", "SEQ"], True),
    ("opt", 48, 32, ["-quality", "75", "-optimize"], False),
    ("q5", 48, 32, ["-quality", "5"], False),
    ("q100", 40, 24, ["-quality", "100", "-sample", "1x1"], False),
    ("s1x1", 1, 1, ["-quality", "90"], False),
    ("s7x5", 7, 5, ["-quality", "90"], True),
    ("s33x17", 33, 17, ["-quality", "90"], True),
    ("s100x75", 100, 75, ["-quality", "60"], True),
    ("p33x17", 33, 17, ["-quality", "90", "-progressive"], True),
]

SEQ_SCANS = "0;\n1;\n2;\n"


def make_vectors():
    os.makedirs(OUT, exist_ok=True)
    tmp = tempfile.mkdtemp()
    seq = os.path.join(tmp, "seq.txt")
    with open(seq, "w") as f:
        f.write(SEQ_SCANS)
    for i, (name, w, h, args, scaled) in enumerate(VECTORS):
        src = os.path.join(tmp, name + ".ppm")
        write_ppm(src, w, h, pattern(w, h, i + 1))
        jpg = os.path.join(OUT, name + ".jpg")
        args = [seq if a == "SEQ" else a for a in args]
        run(["cjpeg"] + args + ["-outfile", jpg, src])
        djpeg(jpg, os.path.join(OUT, name + ".ppm"))
        if scaled:
            for s in (2, 4, 8):
                djpeg(jpg, os.path.join(OUT, "%s.s%d.ppm" % (name, s)), "1/%d" % s)

    # CMYK: ImageMagick writes YCCK with an Adobe marker; the same file
    # with the Adobe transform set to 0 is plain (Adobe inverted) CMYK.
    src = os.path.join(tmp, "cmyk.ppm")
    write_ppm(src, 40, 24, pattern(40, 24, 99))
    ycck = os.path.join(OUT, "ycck.jpg")
    run(["magick", src, "-quality", "90", "-colorspace", "CMYK", ycck])
    djpeg(ycck, os.path.join(OUT, "ycck.ppm"))
    data = bytearray(open(ycck, "rb").read())
    a = data.find(b"Adobe")
    assert a > 0 and data[a + 11] == 2
    data[a + 11] = 0
    cmyk = os.path.join(OUT, "cmyk.jpg")
    open(cmyk, "wb").write(data)
    djpeg(cmyk, os.path.join(OUT, "cmyk.ppm"))

    # EXIF orientations: one coded image (wider than tall), eight files.
    src = os.path.join(tmp, "orient.ppm")
    w, h = 40, 24
    write_ppm(src, w, h, pattern(w, h, 7))
    base = os.path.join(tmp, "orient.jpg")
    run(["cjpeg", "-quality", "90", "-outfile", base, src])
    ref = os.path.join(tmp, "orient_ref.ppm")
    djpeg(base, ref)
    kind, rw, rh, px = read_pnm(ref)
    data = open(base, "rb").read()
    for o in range(1, 9):
        jpg = data[:2] + exif_app1(o, o % 2 == 0) + data[2:]
        open(os.path.join(OUT, "orient%d.jpg" % o), "wb").write(jpg)
        ow, oh, opx = orient_pixels(rw, rh, px, 3, o)
        write_pnm(os.path.join(OUT, "orient%d.ppm" % o), kind, ow, oh, opx)

    # Unsupported codings (expected to fail cleanly).
    src = os.path.join(tmp, "small.ppm")
    write_ppm(src, 16, 16, pattern(16, 16, 5))
    run(["cjpeg", "-arithmetic", "-outfile", os.path.join(OUT, "arith.jpg"), src])
    run(["cjpeg", "-precision", "12", "-outfile", os.path.join(OUT, "p12.jpg"), src])
    run(["cjpeg", "-lossless", "1", "-outfile", os.path.join(OUT, "lossless.jpg"), src])


def make_big(folder):
    os.makedirs(folder, exist_ok=True)
    w, h = 2048, 1536
    # A large pattern built from tiles of the small one (fast in Python).
    tw, th = 256, 192
    tile = pattern(tw, th, 3)
    rows = []
    for y in range(h):
        ty = y % th
        line = tile[ty * tw * 3:(ty + 1) * tw * 3]
        rows.append(line * (w // tw))
    src = os.path.join(folder, "big.ppm")
    write_ppm(src, w, h, b"".join(rows))
    run(["cjpeg", "-quality", "85", "-outfile", os.path.join(folder, "big420.jpg"), src])
    run(["cjpeg", "-quality", "85", "-progressive", "-outfile", os.path.join(folder, "bigprog.jpg"), src])


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--big":
        make_big(sys.argv[2])
    else:
        make_vectors()
