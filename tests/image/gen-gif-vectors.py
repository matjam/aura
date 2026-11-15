#!/usr/bin/env python3
"""Writes the GIF test vectors in tests/image/vectors/gif with reference
decodes (.pam, 8-bit RGBA) computed here from the same indices, using the
browsers' rules: disposal to background clears to transparent, disposal
to previous restores the area under the frame. Animations get one
reference per frame (NAME.fN.pam) and a .txt with the frame count, loop
count (0 = forever, else plays) and delays.

The GIFs are written by this script's own LZW encoder (with clear codes
part way through, and a full 4096-entry table for the larger image), so
every feature is exercised deliberately. With ImageMagick present, also
run: magick NAME.gif -coalesce frame%d.pam and compare by eye; the
generator does not depend on it.

usage: gen-gif-vectors.py [OUTDIR]"""
import os
import struct
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors", "gif")


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


def lzw_encode(indices, min_code, clear_every=0):
    clear, eoi = 1 << min_code, (1 << min_code) + 1
    out = bytearray()
    acc = nb = 0

    def emit(code, size):
        nonlocal acc, nb
        acc |= code << nb
        nb += size
        while nb >= 8:
            out.append(acc & 255)
            acc >>= 8
            nb -= 8

    size = min_code + 1
    table = {}
    nxt = eoi + 1
    emit(clear, size)
    w = None
    count = 0
    for k in indices:
        if w is None:
            w = (k,)
            continue
        wk = w + (k,)
        if wk in table or len(wk) == 1:
            w = wk
            continue
        emit(table[w] if len(w) > 1 else w[0], size)
        if nxt < 4096:
            table[wk] = nxt
            nxt += 1
            if nxt == (1 << size) + 1 and size < 12:
                pass
            if nxt - 1 == (1 << size) and size < 12:
                size += 1
        w = (k,)
        count += 1
        if clear_every and count % clear_every == 0:
            emit(table[w] if len(w) > 1 else w[0], size)
            w = None
            emit(clear, size)
            size = min_code + 1
            table = {}
            nxt = eoi + 1
    if w is not None:
        emit(table[w] if len(w) > 1 else w[0], size)
    emit(eoi, size)
    if nb:
        out.append(acc & 255)
    return bytes(out)


def sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        part = data[i:i + 255]
        out.append(len(part))
        out += part
    out.append(0)
    return bytes(out)


def interlace_order(h):
    rows = []
    for start, step in ((0, 8), (4, 8), (2, 4), (1, 2)):
        rows.extend(range(start, h, step))
    return rows


def pal_bits(n):
    b = 0
    while (2 << b) < n:
        b += 1
    return b


def pal_bytes(pal, bits):
    out = bytearray()
    for i in range(2 << bits):
        out += bytes(pal[i] if i < len(pal) else (0, 0, 0))
    return bytes(out)


def write_pam(path, w, h, rgba):
    hdr = "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (w, h)
    with open(path, "wb") as f:
        f.write(hdr.encode())
        f.write(bytes(rgba))


def make_gif(name, sw, sh, gpal, frames, loop=None, version=b"GIF89a"):
    """frames: dicts with x, y, w, h, idx (rows of indices), lpal (or None),
    trans (index or None), dispose, delay (cs), interlace, min_code,
    clear_every."""
    out = bytearray(version)
    gbits = pal_bits(len(gpal)) if gpal else 0
    out += struct.pack("<HHBBB", sw, sh, (0x80 | (gbits << 4) | gbits) if gpal else 0, 0, 0)
    if gpal:
        out += pal_bytes(gpal, gbits)
    if loop is not None:
        out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\x00"
    # A comment extension, skipped by the decoder.
    out += b"\x21\xFE" + sub_blocks(b"test vector")
    canvas = [[(0, 0, 0, 0)] * sw for _ in range(sh)]
    refs, delays = [], []
    prev = None
    for f in frames:
        if prev:
            px, py, pw, ph, pdis, saved = prev
            if pdis == 2:
                for yy in range(max(0, py), min(sh, py + ph)):
                    for xx in range(max(0, px), min(sw, px + pw)):
                        canvas[yy][xx] = (0, 0, 0, 0)
            elif pdis == 3:
                for (yy, xx), v in saved.items():
                    canvas[yy][xx] = v
        x, y, w, h = f["x"], f["y"], f["w"], f["h"]
        saved = {}
        if f["dispose"] == 3:
            for yy in range(max(0, y), min(sh, y + h)):
                for xx in range(max(0, x), min(sw, x + w)):
                    saved[(yy, xx)] = canvas[yy][xx]
        trans = f.get("trans")
        packed = (f["dispose"] << 2) | (1 if trans is not None else 0)
        out += b"\x21\xF9\x04" + struct.pack("<BHB", packed, f["delay"], trans or 0) + b"\x00"
        lpal = f.get("lpal")
        lbits = pal_bits(len(lpal)) if lpal else 0
        flags = (0x80 | lbits) if lpal else 0
        if f.get("interlace"):
            flags |= 0x40
        out += b"\x2C" + struct.pack("<HHHHB", x, y, w, h, flags)
        if lpal:
            out += pal_bytes(lpal, lbits)
        rows = interlace_order(h) if f.get("interlace") else range(h)
        stream = [i for r in rows for i in f["idx"][r]]
        mc = f["min_code"]
        out.append(mc)
        out += sub_blocks(lzw_encode(stream, mc, f.get("clear_every", 0)))
        pal = lpal or gpal
        for yy in range(h):
            for xx in range(w):
                cx, cy = x + xx, y + yy
                if cx >= sw or cy >= sh:
                    continue
                i = f["idx"][yy][xx]
                if trans is not None and i == trans:
                    continue
                c = pal[i] if i < len(pal) else (0, 0, 0)
                canvas[cy][cx] = (c[0], c[1], c[2], 255)
        rgba = bytearray()
        for row in canvas:
            for v in row:
                rgba += bytes(v)
        refs.append(rgba)
        delays.append(f["delay"] * 10)
        prev = (x, y, w, h, f["dispose"], saved)
    out.append(0x3B)
    with open(os.path.join(OUT, name + ".gif"), "wb") as fh:
        fh.write(out)
    if len(frames) == 1:
        write_pam(os.path.join(OUT, name + ".pam"), sw, sh, refs[0])
    else:
        for i, r in enumerate(refs):
            write_pam(os.path.join(OUT, "%s.f%d.pam" % (name, i)), sw, sh, r)
        plays = 1 if loop is None else (0 if loop == 0 else loop + 1)
        with open(os.path.join(OUT, name + ".txt"), "w") as fh:
            fh.write("frames %d loop %d delays %s\n" % (len(frames), plays, " ".join(str(d) for d in delays)))


def rand_pal(rng, n):
    return [(rng.next() & 255, rng.next() & 255, rng.next() & 255) for _ in range(n)]


def pattern(rng, w, h, n, smooth=True):
    if smooth:
        return [[((x // 3 + y // 2) + (rng.next() % 2)) % n for x in range(w)] for y in range(h)]
    return [[rng.next() % n for x in range(w)] for y in range(h)]


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = Rng(7)
    for bits in (1, 2, 4, 8):
        n = 2 << (bits - 1) if bits > 1 else 2
        n = 1 << bits
        pal = rand_pal(rng, n)
        for il in (False, True):
            name = "p%d%s" % (bits, "i" if il else "")
            make_gif(name, 41, 27, pal, [dict(x=0, y=0, w=41, h=27, idx=pattern(rng, 41, 27, n), dispose=0, delay=0,
                                              interlace=il, min_code=max(2, bits))], version=b"GIF87a" if bits == 4 else b"GIF89a")
    # Transparency, a local palette, a frame smaller than the screen.
    pal = rand_pal(rng, 16)
    make_gif("trans", 30, 20, pal, [dict(x=3, y=2, w=20, h=15, idx=pattern(rng, 20, 15, 16, False), trans=5,
                                         lpal=rand_pal(rng, 16), dispose=0, delay=0, min_code=4)])
    # Clear codes part way, and a big noisy image that fills the table.
    pal = rand_pal(rng, 256)
    make_gif("clears", 50, 40, pal, [dict(x=0, y=0, w=50, h=40, idx=pattern(rng, 50, 40, 256), dispose=0, delay=0,
                                          min_code=8, clear_every=300)])
    make_gif("full", 120, 90, pal, [dict(x=0, y=0, w=120, h=90, idx=pattern(rng, 120, 90, 256, False), dispose=0,
                                         delay=0, min_code=8)])
    # A frame reaching past the screen (clipped), and tiny sizes.
    pal = rand_pal(rng, 4)
    make_gif("clip", 16, 16, pal, [dict(x=8, y=8, w=16, h=16, idx=pattern(rng, 16, 16, 4), dispose=0, delay=0,
                                        min_code=2)])
    make_gif("one", 1, 1, pal, [dict(x=0, y=0, w=1, h=1, idx=[[2]], dispose=0, delay=0, min_code=2)])
    # Animation: every disposal method, offsets, transparency, delays
    # (including 0 and 1 cs, which play at the browsers' minimum), a loop.
    pal = rand_pal(rng, 8)
    frames = []
    for i, (dis, d) in enumerate(((1, 10), (2, 0), (3, 1), (0, 25), (3, 5), (2, 7))):
        w, h = 12 + i * 2, 10 + i
        frames.append(dict(x=(i * 5) % 20, y=(i * 3) % 12, w=w, h=h, idx=pattern(rng, w, h, 8, False),
                           trans=i % 8 if i % 2 else None, dispose=dis, delay=d, min_code=3))
    make_gif("anim", 40, 30, pal, frames, loop=0)
    frames = [dict(x=0, y=0, w=20, h=20, idx=pattern(rng, 20, 20, 8), dispose=1, delay=50, min_code=3),
              dict(x=5, y=5, w=10, h=10, idx=pattern(rng, 10, 10, 8), trans=0, dispose=3, delay=50, min_code=3,
                   interlace=True),
              dict(x=0, y=10, w=20, h=10, idx=pattern(rng, 20, 10, 8), dispose=2, delay=50, min_code=3)]
    make_gif("anim-loop2", 20, 20, pal, frames, loop=2)
    make_gif("anim-once", 20, 20, pal, frames[:2])


if __name__ == "__main__":
    main()
