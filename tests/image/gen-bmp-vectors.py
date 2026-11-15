#!/usr/bin/env python3
"""Writes the BMP and ICO test vectors in tests/image/vectors/bmp with
reference decodes (.pam, 8-bit RGBA) computed here from the same pixels.

BMP: 1, 4 and 8 bits with palettes, 16 bits (555 and 565 bit fields),
24 bits, 32 bits (plain, whose fourth byte is padding, and bit fields
with an alpha mask in V4 and V5 headers), top-down rows, RLE8 and RLE4
(runs, absolute runs, deltas, end of line and end of bitmap), and the
OS/2 12-byte header. ICO: several BMP entries (1, 4, 8, 24 and 32 bits,
AND masks) of different sizes and a PNG entry; NAME.ico decodes its
largest entry (the reference); the test also checks that the entry
closest to a target size is chosen.

usage: gen-bmp-vectors.py [OUTDIR]"""
import os
import struct
import sys
import zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors", "bmp")


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


def write_pam(path, w, h, px):
    hdr = "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (w, h)
    with open(path, "wb") as f:
        f.write(hdr.encode())
        for row in px:
            for p in row:
                f.write(bytes(p))


def pack_bits(vals, bpp):
    out = bytearray()
    per = 8 // bpp
    for i in range(0, len(vals), per):
        b = 0
        for k in range(per):
            v = vals[i + k] if i + k < len(vals) else 0
            b |= v << (8 - bpp * (k + 1))
        out.append(b)
    return bytes(out)


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def info_header(w, h, bpp, comp, hs=40, npal=0, masks=None, size_image=0):
    hdr = struct.pack("<IiiHHIIiiII", hs, w, h, 1, bpp, comp, size_image, 2835, 2835, npal, 0)
    if hs > 40:
        m = list(masks or (0, 0, 0, 0))
        hdr += struct.pack("<IIII", *m)
        hdr += b"\0" * (hs - len(hdr))
    return hdr


def file_wrap(dib, pal_and_masks_len, hs):
    off = 14 + hs + pal_and_masks_len
    return b"BM" + struct.pack("<IHHI", 14 + len(dib), 0, 0, off) + dib


def make_palette_image(rng, w, h, n):
    pal = [(rng.next() & 255, rng.next() & 255, rng.next() & 255) for _ in range(n)]
    idx = [[(x // 2 + y + rng.next() % 2) % n for x in range(w)] for y in range(h)]
    return pal, idx


def bmp_palette(name, w, h, bpp, top_down=False, rng=None, npal_used=0):
    n = npal_used or (1 << bpp)
    pal, idx = make_palette_image(rng, w, h, n)
    rows = []
    for y in range(h):
        rows.append(pad4(pack_bits(idx[y], bpp) if bpp < 8 else bytes(idx[y])))
    if not top_down:
        rows = rows[::-1]
    palb = b"".join(bytes((b, g, r, 0)) for (r, g, b) in pal)
    dib = info_header(w, -h if top_down else h, bpp, 0, npal=npal_used) + palb + b"".join(rows)
    with open(os.path.join(OUT, name + ".bmp"), "wb") as f:
        f.write(file_wrap(dib, len(palb), 40))
    write_pam(os.path.join(OUT, name + ".pam"), w, h, [[pal[i] + (255,) for i in r] for r in idx])


def channel(v, mask):
    if not mask:
        return None
    s = 0
    while not (mask >> s) & 1:
        s += 1
    n = 0
    while (mask >> (s + n)) & 1:
        n += 1
    x = (v & mask) >> s
    if n >= 8:
        return (x >> (n - 8)) & 255
    # Fewer bits are replicated into the low bits, as browsers do.
    v, got = 0, 0
    while got < 8:
        v = (v << n) | x
        got += n
    return (v >> (got - 8)) & 255


def bmp_fields(name, w, h, bpp, masks, hs, comp, rng, top_down=False):
    rows, ref = [], []
    for y in range(h):
        raw = bytearray()
        rr = []
        for x in range(w):
            v = rng.next() & ((1 << bpp) - 1)
            raw += struct.pack("<H" if bpp == 16 else "<I", v)
            r, g, b = (channel(v, m) or 0 for m in masks[:3])
            a = channel(v, masks[3]) if len(masks) > 3 and masks[3] else 255
            rr.append((r, g, b, a))
        rows.append(pad4(bytes(raw)))
        ref.append(rr)
    if not top_down:
        rows = rows[::-1]
    extra = b""
    if hs == 40 and comp == 3:
        extra = struct.pack("<III", *masks[:3])
    dib = info_header(w, -h if top_down else h, bpp, comp, hs=hs, masks=masks if hs > 40 else None) + extra + b"".join(rows)
    with open(os.path.join(OUT, name + ".bmp"), "wb") as f:
        f.write(file_wrap(dib, len(extra), hs))
    write_pam(os.path.join(OUT, name + ".pam"), w, h, ref)


def bmp_rgb(name, w, h, bpp, rng, top_down=False, os2=False):
    rows, ref = [], []
    for y in range(h):
        raw = bytearray()
        rr = []
        for x in range(w):
            r, g, b = (x * 7 + rng.next() % 4) & 255, (y * 5) & 255, (x * y) & 255
            if bpp == 24:
                raw += bytes((b, g, r))
            else:
                raw += bytes((b, g, r, rng.next() & 255))  # padding byte, ignored
            rr.append((r, g, b, 255))
        rows.append(pad4(bytes(raw)))
        ref.append(rr)
    if not top_down:
        rows = rows[::-1]
    if os2:
        hdr = struct.pack("<IHHHH", 12, w, h, 1, bpp)
        dib = hdr + b"".join(rows)
        data = file_wrap(dib, 0, 12)
    else:
        dib = info_header(w, -h if top_down else h, bpp, 0) + b"".join(rows)
        data = file_wrap(dib, 0, 40)
    with open(os.path.join(OUT, name + ".bmp"), "wb") as f:
        f.write(data)
    write_pam(os.path.join(OUT, name + ".pam"), w, h, ref)


def bmp_rle(name, w, h, bpp, rng):
    """RLE8 (bpp 8) or RLE4 (bpp 4) with runs, absolute runs, a delta and
    an early end of bitmap."""
    n = 1 << bpp
    pal = [(rng.next() & 255, rng.next() & 255, rng.next() & 255) for _ in range(n)]
    img = [[None] * w for _ in range(h)]  # None: not written (transparent)
    data = bytearray()
    y = 0  # file row (bottom-up)
    while y < h:
        x = 0
        if y == 2 and h > 6:
            # A delta: skip 3 pixels right and 2 rows up.
            data += bytes((0, 2, 3, 2))
            y += 2
            x = 3
        while x < w:
            left = w - x
            if rng.next() % 2 and left >= 3:
                k = min(left, 3 + rng.next() % 12)
                if bpp == 8:
                    vals = [rng.next() % n for _ in range(k)]
                    body = bytes(vals)
                else:
                    vals = [rng.next() % n for _ in range(k)]
                    body = pack_bits(vals, 4)
                data += bytes((0, k)) + body + (b"\0" if len(body) % 2 else b"")
                for i, v in enumerate(vals):
                    img[h - 1 - y][x + i] = v
                x += k
            else:
                k = min(left, 1 + rng.next() % 20)
                if bpp == 8:
                    c = rng.next() % n
                    data += bytes((k, c))
                    for i in range(k):
                        img[h - 1 - y][x + i] = c
                else:
                    c1, c2 = rng.next() % n, rng.next() % n
                    data += bytes((k, c1 << 4 | c2))
                    for i in range(k):
                        img[h - 1 - y][x + i] = c1 if i % 2 == 0 else c2
                x += k
        if y == h - 2:
            data += bytes((0, 1))  # end of bitmap: the last row is left empty
            break
        data += bytes((0, 0))
        y += 1
    palb = b"".join(bytes((b, g, r, 0)) for (r, g, b) in pal)
    dib = info_header(w, h, bpp, 1 if bpp == 8 else 2, size_image=len(data)) + palb + bytes(data)
    with open(os.path.join(OUT, name + ".bmp"), "wb") as f:
        f.write(file_wrap(dib, len(palb), 40))
    write_pam(os.path.join(OUT, name + ".pam"), w, h,
              [[(pal[v] + (255,)) if v is not None else (0, 0, 0, 0) for v in r] for r in img])


def png_bytes(w, h, px):
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in row) for row in px)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def ico_bmp_entry(w, h, bpp, rng):
    """Returns (entry bytes, reference rows)."""
    ref = []
    mask_rows = []
    xor_rows = []
    pal = []
    if bpp <= 8:
        n = 1 << bpp
        pal = [(rng.next() & 255, rng.next() & 255, rng.next() & 255) for _ in range(n)]
    any_alpha = False
    for y in range(h):
        vals, rr, mbits = [], [], []
        for x in range(w):
            masked = (x - w // 2) ** 2 + (y - h // 2) ** 2 > (w // 2) ** 2  # transparent corners
            mbits.append(1 if masked else 0)
            if bpp <= 8:
                v = (x + y) % len(pal)
                vals.append(v)
                c = pal[v] + (255,)
            elif bpp == 24:
                c = ((x * 9) & 255, (y * 9) & 255, 128, 255)
                vals.append(c)
            else:
                a = 0 if masked else (255 if x < w // 2 else 128)
                c = ((x * 9) & 255, (y * 9) & 255, 64, a)
                vals.append(c)
            rr.append(c)
        if bpp <= 8:
            xor_rows.append(pad4(pack_bits(vals, bpp) if bpp < 8 else bytes(vals)))
        elif bpp == 24:
            xor_rows.append(pad4(b"".join(bytes((c[2], c[1], c[0])) for c in vals)))
        else:
            xor_rows.append(b"".join(bytes((c[2], c[1], c[0], c[3])) for c in vals))
        mask_rows.append(pad4(pack_bits(mbits, 1)))
        if bpp < 32:
            rr = [(0, 0, 0, 0) if m else c for c, m in zip(rr, mbits)]
        else:
            rr = [c if c[3] else (0, 0, 0, 0) for c in rr]
        ref.append(rr)
    palb = b"".join(bytes((b, g, r, 0)) for (r, g, b) in pal)
    hdr = info_header(w, h * 2, bpp, 0)
    entry = hdr + palb + b"".join(xor_rows[::-1]) + b"".join(mask_rows[::-1])
    return entry, ref


def make_ico(name, entries):
    """entries: list of (w, h, bytes, bpp). The largest is the reference."""
    data = struct.pack("<HHH", 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    body = b""
    for (w, h, e, bpp) in entries:
        data += struct.pack("<BBBBHHII", w & 255, h & 255, 0, 0, 1, bpp, len(e), off + len(body))
        body += e
    with open(os.path.join(OUT, name + ".ico"), "wb") as f:
        f.write(data + body)


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = Rng(11)
    for bpp in (1, 4, 8):
        bmp_palette("pal%d" % bpp, 23, 13, bpp, rng=rng)
    bmp_palette("pal8td", 17, 9, 8, top_down=True, rng=rng)
    bmp_palette("pal8used", 17, 9, 8, rng=rng, npal_used=20)
    bmp_rgb("rgb24", 21, 11, 24, rng)
    bmp_rgb("rgb24td", 21, 11, 24, rng, top_down=True)
    bmp_rgb("rgb32", 19, 7, 32, rng)
    bmp_rgb("os2rgb24", 13, 5, 24, rng, os2=True)
    bmp_fields("f555", 15, 9, 16, (0x7C00, 0x03E0, 0x001F), 40, 3, rng)
    bmp_fields("f565", 15, 9, 16, (0xF800, 0x07E0, 0x001F), 40, 3, rng)
    bmp_fields("v4argb", 15, 9, 32, (0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000), 108, 3, rng)
    bmp_fields("v5rgba", 15, 9, 32, (0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF), 124, 3, rng, top_down=True)
    bmp_rle("rle8", 40, 12, 8, rng)
    bmp_rle("rle4", 37, 12, 4, rng)
    # ICO with BMP entries of each depth, and one with a PNG entry.
    ents = []
    refs = {}
    for (s, bpp) in ((16, 4), (16, 8), (32, 32), (24, 24), (48, 1)):
        e, ref = ico_bmp_entry(s, s, bpp, rng)
        ents.append((s, s, e, bpp))
        refs[(s, bpp)] = ref
    make_ico("icons", ents)
    write_pam(os.path.join(OUT, "icons.pam"), 48, 48, refs[(48, 1)])
    # The 16 x 16 8-bit entry for the target-size test.
    write_pam(os.path.join(OUT, "icons-16.pam.ref"), 16, 16, refs[(16, 8)])
    px = [[((x * 4) & 255, (y * 4) & 255, 200, 255 if (x + y) % 5 else 0) for x in range(64)] for y in range(64)]
    e16, r16 = ico_bmp_entry(16, 16, 32, rng)
    make_ico("pngicon", [(16, 16, e16, 32), (0, 0, png_bytes(64, 64, px), 32)])
    # The PNG entry is 64 x 64 (directory says 256 for 0); reference it.
    write_pam(os.path.join(OUT, "pngicon.pam"), 64, 64, [[(p if p[3] else (0, 0, 0, 0)) for p in row] for row in px])


if __name__ == "__main__":
    main()
