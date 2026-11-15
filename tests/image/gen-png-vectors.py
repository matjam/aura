#!/usr/bin/env python3
"""Writes the PNG test vectors in tests/image/vectors/png: every colour
type and bit depth, with and without Adam7 interlacing, palette and tRNS
transparency, every filter type, and APNG animations, each with a
reference decode (.pam, 8-bit RGBA, not premultiplied) computed here from
the same source pixels. 16-bit samples are referenced by their high byte,
as the decoder keeps them.

APNG vectors get one reference per frame (NAME.fN.pam) and a .txt with
the frame count, loop count and delays.

Only the Python standard library is used (zlib for the compression).

usage: gen-png-vectors.py [OUTDIR]"""
import os
import struct
import sys
import zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors", "png")

ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


class Rng:
    def __init__(self, s):
        self.s = s or 1

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s


def chunk(t, data):
    c = struct.pack(">I", len(data)) + t + data
    return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)


def pack_row(samples, depth):
    """samples: list of ints, packed at depth bits each, big-endian."""
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        out = bytearray()
        for v in samples:
            out += struct.pack(">H", v)
        return bytes(out)
    out = bytearray()
    per = 8 // depth
    for i in range(0, len(samples), per):
        b = 0
        for k in range(per):
            v = samples[i + k] if i + k < len(samples) else 0
            b |= v << (8 - depth * (k + 1))
        out.append(b)
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_row(ft, row, prev, bpp):
    out = bytearray([ft])
    for i in range(len(row)):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if prev and i >= bpp else 0
        x = row[i]
        if ft == 0:
            v = x
        elif ft == 1:
            v = x - a
        elif ft == 2:
            v = x - b
        elif ft == 3:
            v = x - ((a + b) >> 1)
        else:
            v = x - paeth(a, b, c)
        out.append(v & 255)
    return bytes(out)


def encode_image(px, w, h, depth, channels, interlace, rng):
    """px[y][x] = tuple of channel samples. Returns the zlib stream."""
    bpp = max(1, channels * depth // 8)
    raw = bytearray()
    passes = ADAM7 if interlace else [(0, 0, 1, 1)]
    for (x0, y0, dx, dy) in passes:
        pw = (w - x0 + dx - 1) // dx if w > x0 else 0
        ph = (h - y0 + dy - 1) // dy if h > y0 else 0
        if pw == 0 or ph == 0:
            continue
        prev = None
        for py in range(ph):
            y = y0 + py * dy
            samples = []
            for pxi in range(pw):
                samples.extend(px[y][x0 + pxi * dx])
            row = pack_row(samples, depth)
            ft = rng.next() % 5
            raw += filter_row(ft, row, prev, bpp)
            prev = row
    return zlib.compress(bytes(raw), 9)


def write_png(path, w, h, depth, ctype, idat, extra=b"", interlace=0, split=0):
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))
    data += extra
    if split:
        for i in range(0, len(idat), split):
            data += chunk(b"IDAT", idat[i:i + split])
    else:
        data += chunk(b"IDAT", idat)
    data += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def write_pam(path, w, h, rgba):
    hdr = "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (w, h)
    with open(path, "wb") as f:
        f.write(hdr.encode())
        f.write(bytes(rgba))


def gen_still(name, w, h, ctype, depth, interlace, seed, trns=False, split=0):
    rng = Rng(seed)
    maxv = (1 << depth) - 1
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    px = []
    extra = b""
    pal = []
    if ctype == 3:
        n = min(1 << depth, 1 + rng.next() % (1 << depth))
        pal = [(rng.next() & 255, rng.next() & 255, rng.next() & 255) for _ in range(n)]
        alpha = [rng.next() & 255 for _ in range(n)]
        extra += chunk(b"PLTE", b"".join(bytes(c) for c in pal))
        if trns:
            # Fewer alpha entries than palette entries: the rest are opaque.
            na = max(1, n // 2)
            extra += chunk(b"tRNS", bytes(alpha[:na]))
            alpha = alpha[:na] + [255] * (n - na)
        else:
            alpha = [255] * n
    for y in range(h):
        row = []
        for x in range(w):
            if ctype == 3:
                row.append((rng.next() % len(pal),))
            else:
                # A gradient with noise so every filter has work to do.
                base = [(x * 37 + y * 11 + c * 50) for c in range(channels)]
                row.append(tuple(min(maxv, (b * maxv // 255 + (rng.next() % 3)) % (maxv + 1)) for b in base))
        px.append(row)
    key = None
    if trns and ctype == 0:
        key = px[h // 2][w // 2]
        extra += chunk(b"tRNS", struct.pack(">H", key[0]))
    if trns and ctype == 2:
        key = px[h // 2][w // 2]
        extra += chunk(b"tRNS", struct.pack(">HHH", *key))
    idat = encode_image(px, w, h, depth, channels, interlace, rng)
    write_png(os.path.join(OUT, name + ".png"), w, h, depth, ctype, idat, extra, interlace, split)
    rgba = bytearray()

    def to8(v):
        if depth == 16:
            return v >> 8
        return v * 255 // maxv

    for y in range(h):
        for x in range(w):
            s = px[y][x]
            if ctype == 3:
                r, g, b = pal[s[0]]
                a = alpha[s[0]]
            elif ctype in (0, 4):
                r = g = b = to8(s[0])
                a = to8(s[1]) if ctype == 4 else (0 if key is not None and s == key else 255)
            else:
                r, g, b = to8(s[0]), to8(s[1]), to8(s[2])
                a = to8(s[3]) if ctype == 6 else (0 if key is not None and s == key else 255)
            rgba += bytes((r, g, b, a))
    write_pam(os.path.join(OUT, name + ".pam"), w, h, rgba)


def gen_apng(name, w, h, frames, plays, idat_is_frame=True, seed=5):
    """frames: list of (x, y, fw, fh, delay_num, delay_den, dispose, blend).
    Pixels are opaque or fully transparent, so compositing is exact."""
    rng = Rng(seed)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    out += chunk(b"acTL", struct.pack(">II", len(frames), plays))
    seq = 0
    canvas = [[(0, 0, 0, 0)] * w for _ in range(h)]
    refs = []
    delays = []
    prev = None
    if not idat_is_frame:
        # A default image that is not part of the animation.
        px = [[(255, 0, 255, 255)] * w for _ in range(h)]
        out += chunk(b"IDAT", encode_image(px, w, h, 8, 4, 0, rng))
    for i, (x, y, fw, fh, dn, dd, dispose, blend) in enumerate(frames):
        if prev is not None:
            px0, py0, pw0, ph0, pdis, saved = prev
            if pdis == 1:
                for yy in range(py0, py0 + ph0):
                    for xx in range(px0, px0 + pw0):
                        canvas[yy][xx] = (0, 0, 0, 0)
            elif pdis == 2:
                for yy in range(py0, py0 + ph0):
                    for xx in range(px0, px0 + pw0):
                        canvas[yy][xx] = saved[yy - py0][xx - px0]
        if i == 0 and dispose == 2:
            dispose_eff = 1
        else:
            dispose_eff = dispose
        saved = [[canvas[yy][xx] for xx in range(x, x + fw)] for yy in range(y, y + fh)] if dispose_eff == 2 else None
        col = (rng.next() & 255, rng.next() & 255, rng.next() & 255)
        px = []
        for yy in range(fh):
            row = []
            for xx in range(fw):
                hole = (xx + yy + i) % 4 == 0
                row.append((0, 0, 0, 0) if hole else (col[0], (xx * 9) & 255, (yy * 13) & 255, 255))
            px.append(row)
        fctl = struct.pack(">IIIIIHHBB", seq, fw, fh, x, y, dn, dd, dispose, blend)
        seq += 1
        out += chunk(b"fcTL", fctl)
        z = encode_image(px, fw, fh, 8, 4, 0, rng)
        if i == 0 and idat_is_frame:
            out += chunk(b"IDAT", z)
        else:
            # Split frame data over two fdAT chunks.
            half = len(z) // 2
            for part in (z[:half], z[half:]):
                out += chunk(b"fdAT", struct.pack(">I", seq) + part)
                seq += 1
        for yy in range(fh):
            for xx in range(fw):
                s = px[yy][xx]
                if blend == 0 or s[3] == 255:
                    canvas[y + yy][x + xx] = s
        rgba = bytearray()
        for yy in range(h):
            for xx in range(w):
                rgba += bytes(canvas[yy][xx])
        refs.append(rgba)
        delays.append((dn * 1000 + (dd or 100) // 2) // (dd or 100))
        prev = (x, y, fw, fh, dispose_eff, saved)
    out += chunk(b"IEND", b"")
    with open(os.path.join(OUT, name + ".png"), "wb") as f:
        f.write(out)
    for i, rgba in enumerate(refs):
        write_pam(os.path.join(OUT, "%s.f%d.pam" % (name, i)), w, h, rgba)
    with open(os.path.join(OUT, name + ".txt"), "w") as f:
        f.write("frames %d loop %d delays %s\n" % (len(frames), plays, " ".join(str(d) for d in delays)))


def main():
    os.makedirs(OUT, exist_ok=True)
    seed = 1
    for ctype, depths in ((0, (1, 2, 4, 8, 16)), (2, (8, 16)), (3, (1, 2, 4, 8)), (4, (8, 16)), (6, (8, 16))):
        for depth in depths:
            for interlace in (0, 1):
                seed += 1
                name = "c%dd%02d%s" % (ctype, depth, "i" if interlace else "")
                gen_still(name, 37, 23, ctype, depth, interlace, seed)
                if ctype in (0, 2, 3):
                    gen_still(name + "t", 37, 23, ctype, depth, interlace, seed + 100, trns=True)
    # Sizes that stress Adam7 (passes left empty) and row packing.
    for (w, h) in ((1, 1), (2, 1), (1, 9), (5, 3), (9, 9)):
        seed += 1
        gen_still("s%dx%di" % (w, h), w, h, 6, 8, 1, seed)
        gen_still("s%dx%dg1" % (w, h), w, h, 0, 1, 0, seed)
    # IDAT split into many small chunks, and one byte each.
    gen_still("split", 64, 40, 2, 8, 0, 77, split=97)
    gen_still("split1", 16, 8, 6, 8, 1, 78, split=1)
    # A larger image for the speed and scaling tests.
    gen_still("big", 160, 120, 2, 8, 0, 79)
    gen_apng("apng-basic", 40, 30, [
        (0, 0, 40, 30, 1, 10, 0, 0),
        (5, 5, 20, 10, 50, 1000, 1, 1),
        (10, 2, 15, 20, 0, 0, 2, 1),
        (0, 0, 10, 10, 1, 100, 0, 0),
    ], 3)
    gen_apng("apng-hidden", 24, 16, [
        (0, 0, 24, 16, 2, 10, 0, 0),
        (4, 4, 8, 8, 2, 10, 0, 1),
    ], 0, idat_is_frame=False)


if __name__ == "__main__":
    main()
