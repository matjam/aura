#!/usr/bin/env python3
"""Build Aura's application icon from its pixel art.

The art lives in assets/icon/<concept>/icon.txt as text grids, one
character per pixel, with a palette that gives each character its 32-bit
colour, its alpha, its index in the 16-colour palette and its 1-bit value.
See assets/icon/README.txt for the format.

Usage:
  tools/make-icon.py               write src/platform/macos9/aura_icon.r
                                   and assets/icon/preview/
  tools/make-icon.py -c platypus   the same for another concept
  tools/make-icon.py --sheet       also write assets/icon/concepts.png,
                                   the contact sheet of every concept
  tools/make-icon.py --custom-icon also store the 'icns' at ID -16455,
                                   the file's custom icon

To switch the application's icon to another concept, change CONCEPT below
and run the script again.

aura_icon.r holds, at ID 128: 'ICN#', 'icl4', 'icl8' (32x32), 'ics#',
'ics4', 'ics8' (16x16), and 'icns' (Mac OS 8.5 and later), which holds
those again plus 32-bit colour with 8-bit masks: 'il32'+'l8mk' (32x32),
'is32'+'s8mk' (16x16), and 'ih32'+'h8mk' with 'ich#', 'ich4', 'ich8'
(48x48). With -DAURA_ICON_BUNDLE it also defines the Finder bundle: the
signature resource 'AURA' (0), 'FREF' (128) for 'APPL' and 'BNDL' (128).

The Finder shows a bundle's icon from its desktop database, which keeps at
most 256 colours and a 1-bit mask. With --custom-icon the 'icns' is also
written at ID -16455, the file's custom icon, which Icon Services draws in
32-bit colour with the 8-bit masks; the file then needs its hasCustomIcon
flag set (tools/finder-flags.py --custom-icon).

The previews are decoded from the bytes written into aura_icon.r, not from
the art, so they show what the Mac will draw.

Python 3 standard library only.
"""

import argparse
import os
import re
import struct
import sys
import zlib

CONCEPT = "kookaburra"

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(
    __file__)), ".."))
ART = os.path.join(ROOT, "assets", "icon")
REZ_OUT = os.path.join(ROOT, "src", "platform", "macos9", "aura_icon.r")
PREVIEW = os.path.join(ART, "preview")
CONCEPTS = ["frill", "kookaburra", "platypus", "quoll"]
CREATOR = "AURA"

GREY = (0xDD, 0xDD, 0xDD)    # Platinum
WHITE = (0xFF, 0xFF, 0xFF)
ZOOM = 8

# Palettes ---------------------------------------------------------------


def clut8():
    """The Mac OS 8-bit system palette ('clut' 8): the 6x6x6 colour cube
    from white down, without black, then ramps of red, green, blue and
    grey, then black at 255."""
    levels = [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    clut = [(r, g, b) for r in levels for g in levels for b in levels][:215]
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    clut += [(v, 0, 0) for v in ramp]
    clut += [(0, v, 0) for v in ramp]
    clut += [(0, 0, v) for v in ramp]
    clut += [(v, v, v) for v in ramp]
    clut.append((0, 0, 0))
    assert len(clut) == 256
    return clut


CLUT8 = clut8()

# The Mac OS 4-bit system palette ('clut' 4).
CLUT4 = [(0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x02),
         (0xDD, 0x08, 0x06), (0xF2, 0x08, 0x84), (0x46, 0x00, 0xA5),
         (0x00, 0x00, 0xD4), (0x02, 0xAB, 0xEA), (0x1F, 0xB7, 0x14),
         (0x00, 0x64, 0x11), (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A),
         (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40),
         (0x00, 0x00, 0x00)]


def nearest(clut, c):
    """The index of the colour in clut closest to c, by a distance that
    weights the channels roughly as the eye does."""
    r, g, b = c

    def dist(k):
        p = clut[k]
        rm = (r + p[0]) / 2
        dr, dg, db = r - p[0], g - p[1], b - p[2]
        return ((2 + rm / 256) * dr * dr + 4 * dg * dg +
                (2 + (255 - rm) / 256) * db * db)
    return min(range(len(clut)), key=dist)


# Source art -------------------------------------------------------------


class Entry:
    def __init__(self, ch, rgb, alpha, c4, bw):
        self.ch, self.rgb, self.alpha, self.c4, self.bw = \
            ch, rgb, alpha, c4, bw
        self.c8 = nearest(CLUT8, rgb) if rgb else 0


class Art:
    """One concept's source: a palette and grids by size."""

    def __init__(self, concept):
        self.concept = concept
        self.path = os.path.join(ART, concept, "icon.txt")
        self.name = concept
        self.palette = {}
        self.grids = {}      # size -> rows of characters
        self.bwgrids = {}    # size -> rows of '#' and '.', optional
        self.parse()

    def fail(self, n, msg):
        sys.exit("%s:%d: %s" % (self.path, n, msg))

    def parse(self):
        with open(self.path) as f:
            lines = f.read().split("\n")
        n = 0
        while n < len(lines):
            line = lines[n]
            n += 1
            words = line.split("#", 1)[0].split() if not \
                line.startswith("#") else []
            if not words:
                continue
            if words[0] == "name":
                self.name = line.split(None, 1)[1].strip()
            elif words[0] == "palette":
                while lines[n].strip() != "end":
                    w = lines[n].split()
                    if w and not lines[n].startswith("#"):
                        if len(w) < 5:
                            self.fail(n + 1, "palette entry needs 5 fields")
                        ch = w[0]
                        rgb = None if w[1] == "-" else tuple(
                            bytes.fromhex(w[1]))
                        c4 = 0 if w[3] == "-" else int(w[3])
                        self.palette[ch] = Entry(ch, rgb, int(w[2]), c4, w[4])
                    n += 1
                n += 1
            elif words[0] in ("icon", "bw"):
                size = int(words[1])
                rows = []
                while lines[n].strip() != "end":
                    rows.append(lines[n].rstrip())
                    n += 1
                n += 1
                if len(rows) != size or any(len(r) != size for r in rows):
                    self.fail(n, "%s %d grid is not %dx%d" %
                              (words[0], size, size, size))
                for r in rows:
                    for ch in r:
                        if words[0] == "icon" and ch not in self.palette:
                            self.fail(n, "%r is not in the palette" % ch)
                        if words[0] == "bw" and ch not in "#.":
                            self.fail(n, "bw grids use # and . only")
                (self.grids if words[0] == "icon" else self.bwgrids)[size] = \
                    rows
            else:
                self.fail(n, "unknown line: %s" % line)


class Icon:
    """One size of a concept, in every depth Mac OS draws."""

    def __init__(self, art, size):
        self.size = size
        rows = art.grids[size]
        pal = art.palette
        self.entries = [[pal[ch] for ch in row] for row in rows]
        # Pixels at least half opaque are in the 1-bit mask; the fainter
        # ones (a glow) show only in the 32-bit icons.
        self.mask = [[1 if e.alpha >= 128 else 0 for e in row]
                     for row in self.entries]
        if size in art.bwgrids:
            self.bits = [[1 if ch == "#" else 0 for ch in row]
                         for row in art.bwgrids[size]]
        else:
            self.bits = [[pattern(e.bw, i, j) for i, e in enumerate(row)]
                         for j, row in enumerate(self.entries)]
            self.outline()
        for j in range(size):
            for i in range(size):
                if not self.mask[j][i]:
                    self.bits[j][i] = 0

    def outline(self):
        """Black on every masked pixel next to an unmasked one, so the
        1-bit icon keeps its edge on any background."""
        s = self.size
        for j in range(s):
            for i in range(s):
                if self.mask[j][i] and any(
                        not (0 <= i + di < s and 0 <= j + dj < s) or
                        not self.mask[j + dj][i + di]
                        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                    self.bits[j][i] = 1

    def bitmap(self, rows):
        out = bytearray()
        for row in rows:
            for k in range(0, self.size, 8):
                v = 0
                for b in row[k:k + 8]:
                    v = v << 1 | b
                out.append(v)
        return bytes(out)

    def icn(self):
        """'ICN#', 'ics#', 'ich#': the icon's bits, then the mask."""
        return self.bitmap(self.bits) + self.bitmap(self.mask)

    def ic8(self):
        return bytes(e.c8 if m else 0
                     for row, mrow in zip(self.entries, self.mask)
                     for e, m in zip(row, mrow))

    def ic4(self):
        out = bytearray()
        for row, mrow in zip(self.entries, self.mask):
            v = [e.c4 if m else 0 for e, m in zip(row, mrow)]
            for k in range(0, self.size, 2):
                out.append(v[k] << 4 | v[k + 1])
        return bytes(out)

    def channel(self, k):
        return bytes(e.rgb[k] if e.rgb and e.alpha else 0
                     for row in self.entries for e in row)

    def rgb32(self):
        """'il32', 'is32', 'ih32': each channel run-length coded."""
        return b"".join(rle(self.channel(k)) for k in range(3))

    def mask8(self):
        return bytes(e.alpha for row in self.entries for e in row)

    def rgba(self):
        """The 32-bit art as rows of (r, g, b, a), for the contact sheet."""
        return [[(e.rgb or (0, 0, 0)) + (e.alpha,) for e in row]
                for row in self.entries]

    def bw_rgba(self):
        return [[((0, 0, 0) if b else WHITE) + (255 if m else 0,)
                 for b, m in zip(brow, mrow)]
                for brow, mrow in zip(self.bits, self.mask)]


def pattern(bw, i, j):
    """A 1-bit pixel: 0 white, 1 black, l light grey (1 in 4 black),
    m mid grey (checkerboard), d dark grey (3 in 4 black)."""
    if bw == "l":
        return 1 if (j % 2 == 0 and (i + j // 2 * 2) % 4 == 0) else 0
    if bw == "m":
        return (i + j) % 2
    if bw == "d":
        return 0 if (j % 2 == 0 and (i + j // 2 * 2) % 4 == 2) else 1
    return int(bw)


def rle(data):
    """The run-length coding of 32-bit icon channels: a byte n below 0x80
    is followed by n + 1 literal bytes; a byte n of 0x80 or more by one
    byte, repeated n - 0x80 + 3 times."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 130 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes((0x80 + run - 3, data[i]))
            i += run
            continue
        j = i
        while j < n and j - i < 128:
            if j + 2 < n and data[j] == data[j + 1] == data[j + 2]:
                break
            j += 1
        out.append(j - i - 1)
        out += data[i:j]
        i = j
    return bytes(out)


def unrle(data, count):
    out = bytearray()
    i = 0
    while len(out) < count:
        n = data[i]
        i += 1
        if n < 0x80:
            out += data[i:i + n + 1]
            i += n + 1
        else:
            out += bytes([data[i]]) * (n - 0x80 + 3)
            i += 1
    if len(out) != count:
        raise ValueError("run-length data overruns its channel")
    return bytes(out), i


# Rez output -------------------------------------------------------------


def icns(icons):
    parts = []
    large = icons[32]
    parts += [(b"ICN#", large.icn()), (b"icl4", large.ic4()),
              (b"icl8", large.ic8()), (b"il32", large.rgb32()),
              (b"l8mk", large.mask8())]
    small = icons[16]
    parts += [(b"ics#", small.icn()), (b"ics4", small.ic4()),
              (b"ics8", small.ic8()), (b"is32", small.rgb32()),
              (b"s8mk", small.mask8())]
    if 48 in icons:
        huge = icons[48]
        parts += [(b"ich#", huge.icn()), (b"ich4", huge.ic4()),
                  (b"ich8", huge.ic8()), (b"ih32", huge.rgb32()),
                  (b"h8mk", huge.mask8())]
    body = b"".join(t + struct.pack(">I", len(d) + 8) + d for t, d in parts)
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def rez_data(rtype, rid, data, name=None):
    head = "data '%s' (%d%s) {" % (rtype, rid,
                                   ', "%s"' % name if name else "")
    lines = [head]
    for k in range(0, len(data), 32):
        lines.append('\t$"%s"' % data[k:k + 32].hex().upper())
    lines.append("};")
    return "\n".join(lines)


BUNDLE = """\
#ifdef AURA_ICON_BUNDLE
/* The Finder bundle: the signature (owner) resource for creator '%(c)s',
   the file reference for the application's type and the bundle that maps
   it to icon family 128. Defined only with -DAURA_ICON_BUNDLE, for when
   aura.r does not define them itself. The Finder reads the bundle only
   when the file's hasBundle flag is set. */
#include "Finder.r"

data '%(c)s' (0, "Owner resource") {
\t$"0441757261"            /* "Aura" */
};

resource 'FREF' (128) {
\t'APPL', 0, ""
};

resource 'BNDL' (128) {
\t'%(c)s', 0,
\t{
\t\t'ICN#', { 0, 128 };
\t\t'FREF', { 0, 128 }
\t}
};
#endif
"""


def write_rez(art, icons, path, custom_icon=False):
    sizes = sorted(icons)
    family = icns(icons)
    res = [("ICN#", 128, icons[32].icn()), ("icl4", 128, icons[32].ic4()),
           ("icl8", 128, icons[32].ic8()), ("ics#", 128, icons[16].icn()),
           ("ics4", 128, icons[16].ic4()), ("ics8", 128, icons[16].ic8()),
           ("icns", 128, family)]
    if custom_icon:
        res.append(("icns", -16455, family))
    with open(path, "w") as f:
        f.write("/* Aura's application icon: %s (concept \"%s\").\n"
                "   Written by tools/make-icon.py from "
                "assets/icon/%s/icon.txt.\n"
                "   Change the art and run the script again instead of "
                "editing this file.\n\n"
                "   Icon family 128, sizes %s.%s */\n\n" %
                (art.name, art.concept, art.concept,
                 ", ".join("%dx%d" % (s, s) for s in sizes),
                 "\n   The 'icns' is also the file's custom icon (ID -16455);"
                 "\n   set the file's hasCustomIcon flag with"
                 "\n   tools/finder-flags.py --custom-icon." if custom_icon
                 else ""))
        f.write(BUNDLE % {"c": CREATOR} + "\n")
        for rtype, rid, data in res:
            f.write(rez_data(rtype, rid, data) + "\n\n")


def read_rez(path):
    """The data resources in a .r file this script wrote, as
    {(type, id): bytes}."""
    with open(path) as f:
        text = f.read()
    out = {}
    for m in re.finditer(r"data '(.{4})' \((-?\d+)[^)]*\) \{(.*?)\};",
                         text, re.S):
        hexes = re.findall(r'\$"([0-9A-Fa-f]*)"', m.group(3))
        out[(m.group(1), int(m.group(2)))] = bytes.fromhex("".join(hexes))
    return out


# Decoding the resources back into pixels -------------------------------
# Each decoder returns rows of (r, g, b, a) or None for transparent.


def dec_bits(data, size):
    rb = size // 8
    n = rb * size
    if len(data) != 2 * n:
        raise ValueError("1-bit icon of %d bytes for %dx%d" %
                         (len(data), size, size))

    def bit(base, i, j):
        return data[base + j * rb + i // 8] >> (7 - i % 8) & 1
    rows = []
    for j in range(size):
        row = []
        for i in range(size):
            b, m = bit(0, i, j), bit(n, i, j)
            if m:
                row.append(((0, 0, 0) if b else WHITE) + (255,))
            elif b:
                row.append((255, 0, 255, 255))  # inverts the screen: a bug
            else:
                row.append(None)
        rows.append(row)
    return rows


def mask_of(icn, size):
    n = size * size // 8
    return dec_bits(bytes(n) + icn[n:], size)


def dec_indexed(data, size, depth, icn):
    """'icl8'/'icl4' and the small and huge ones, shown through the 1-bit
    mask as the Finder draws them."""
    if len(data) != size * size * depth // 8:
        raise ValueError("%d-bit icon of %d bytes" % (depth, len(data)))
    mask = mask_of(icn, size)
    rows = []
    for j in range(size):
        row = []
        for i in range(size):
            if depth == 8:
                c = CLUT8[data[j * size + i]]
            else:
                b = data[(j * size + i) // 2]
                c = CLUT4[b >> 4 if i % 2 == 0 else b & 15]
            row.append(c + (255,) if mask[j][i] else None)
        rows.append(row)
    return rows


def dec_32(data, mask8, size):
    n = size * size
    if len(mask8) != n:
        raise ValueError("8-bit mask of %d bytes" % len(mask8))
    chans = []
    pos = 0
    for _ in range(3):
        ch, used = unrle(data[pos:], n)
        chans.append(ch)
        pos += used
    if pos != len(data):
        raise ValueError("%d bytes left after the 32-bit channels" %
                         (len(data) - pos))
    return [[(chans[0][j * size + i], chans[1][j * size + i],
              chans[2][j * size + i], mask8[j * size + i])
             for i in range(size)] for j in range(size)]


def parse_icns(data):
    if data[:4] != b"icns" or struct.unpack(">I", data[4:8])[0] != len(data):
        raise ValueError("bad icns header")
    out = {}
    pos = 8
    while pos < len(data):
        t = data[pos:pos + 4].decode("latin-1")
        n = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        out[t] = data[pos + 8:pos + n]
        pos += n
    if pos != len(data):
        raise ValueError("icns elements overrun")
    return out


def decode_all(res):
    """Every icon in the resources, as {label: rows}."""
    out = {}
    for t, size in (("ICN#", 32), ("ics#", 16)):
        out["%s 1-bit" % t] = dec_bits(res[(t, 128)], size)
    out["icl4"] = dec_indexed(res[("icl4", 128)], 32, 4, res[("ICN#", 128)])
    out["icl8"] = dec_indexed(res[("icl8", 128)], 32, 8, res[("ICN#", 128)])
    out["ics4"] = dec_indexed(res[("ics4", 128)], 16, 4, res[("ics#", 128)])
    out["ics8"] = dec_indexed(res[("ics8", 128)], 16, 8, res[("ics#", 128)])
    fam = parse_icns(res[("icns", 128)])
    for t in ("ICN#", "icl4", "icl8", "ics#", "ics4", "ics8"):
        if fam[t] != res[(t, 128)]:
            raise ValueError("icns %s differs from the '%s' resource" %
                             (t, t))
    out["il32+l8mk"] = dec_32(fam["il32"], fam["l8mk"], 32)
    out["is32+s8mk"] = dec_32(fam["is32"], fam["s8mk"], 16)
    if "ih32" in fam:
        out["ich# 1-bit"] = dec_bits(fam["ich#"], 48)
        out["ich4"] = dec_indexed(fam["ich4"], 48, 4, fam["ich#"])
        out["ich8"] = dec_indexed(fam["ich8"], 48, 8, fam["ich#"])
        out["ih32+h8mk"] = dec_32(fam["ih32"], fam["h8mk"], 48)
    return out


# Images -----------------------------------------------------------------


class Canvas:
    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.px = [list(bg) * w for _ in range(h)]

    def put(self, x, y, c, a=255):
        if not (0 <= x < self.w and 0 <= y < self.h):
            return
        row = self.px[y]
        k = x * 3
        if a >= 255:
            row[k:k + 3] = c[:3]
        elif a > 0:
            for q in range(3):
                row[k + q] = (c[q] * a + row[k + q] * (255 - a) + 127) // 255

    def rect(self, x, y, w, h, c):
        for v in range(y, y + h):
            for u in range(x, x + w):
                self.put(u, v, c)

    def icon(self, rows, x, y, zoom=1):
        for j, row in enumerate(rows):
            for i, p in enumerate(row):
                if p is None or p[3] == 0:
                    continue
                for v in range(zoom):
                    for u in range(zoom):
                        self.put(x + i * zoom + u, y + j * zoom + v, p[:3],
                                 p[3])

    def text(self, s, x, y, c=(0, 0, 0), scale=2):
        for ch in s.upper():
            glyph = FONT.get(ch, FONT["?"])
            for j, line in enumerate(glyph):
                for i, b in enumerate(line):
                    if b == "#":
                        self.rect(x + i * scale, y + j * scale, scale, scale,
                                  c)
            x += 4 * scale

    def save(self, path):
        raw = b"".join(b"\0" + bytes(row) for row in self.px)
        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2,
                                          0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(raw, 9))
        png += chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)


def chunk(t, data):
    return (struct.pack(">I", len(data)) + t + data +
            struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF))


# A 3x5 font for labels.
FONT = {k: v.split("/") for k, v in {
    "A": "###/#.#/###/#.#/#.#", "B": "##./#.#/##./#.#/##.",
    "C": "###/#../#../#../###", "D": "##./#.#/#.#/#.#/##.",
    "E": "###/#../##./#../###", "F": "###/#../##./#../#..",
    "G": "###/#../#.#/#.#/###", "H": "#.#/#.#/###/#.#/#.#",
    "I": "###/.#./.#./.#./###", "J": "..#/..#/..#/#.#/###",
    "K": "#.#/#.#/##./#.#/#.#", "L": "#../#../#../#../###",
    "M": "#.#/###/###/#.#/#.#", "N": "##./#.#/#.#/#.#/#.#",
    "O": "###/#.#/#.#/#.#/###", "P": "###/#.#/###/#../#..",
    "Q": "###/#.#/#.#/###/..#", "R": "##./#.#/##./#.#/#.#",
    "S": "###/#../###/..#/###", "T": "###/.#./.#./.#./.#.",
    "U": "#.#/#.#/#.#/#.#/###", "V": "#.#/#.#/#.#/#.#/.#.",
    "W": "#.#/#.#/###/###/#.#", "X": "#.#/#.#/.#./#.#/#.#",
    "Y": "#.#/#.#/.#./.#./.#.", "Z": "###/..#/.#./#../###",
    "0": "###/#.#/#.#/#.#/###", "1": ".#./##./.#./.#./###",
    "2": "###/..#/###/#../###", "3": "###/..#/.##/..#/###",
    "4": "#.#/#.#/###/..#/..#", "5": "###/#../###/..#/###",
    "6": "###/#../###/#.#/###", "7": "###/..#/..#/..#/..#",
    "8": "###/#.#/###/#.#/###", "9": "###/#.#/###/..#/###",
    " ": ".../.../.../.../...", "-": ".../.../###/.../...",
    "+": ".../.#./###/.#./...", "#": "#.#/###/#.#/###/#.#",
    "?": "###/..#/.##/.../.#.", "/": "..#/..#/.#./#../#..",
}.items()}


def tile(canvas, rows, x, y, label=None):
    """An icon enlarged with crisp pixels, on white and on grey, with the
    actual size beside each. Returns the width used."""
    size = len(rows)
    big = size * ZOOM
    pad = 8
    for k, bg in enumerate((WHITE, GREY)):
        bx = x + k * (big + size + 3 * pad)
        canvas.rect(bx, y, big + size + 3 * pad, big + 2 * pad, bg)
        canvas.icon(rows, bx + pad, y + pad, ZOOM)
        canvas.icon(rows, bx + 2 * pad + big, y + pad, 1)
    if label:
        canvas.text(label, x, y + big + 2 * pad + 4)
    return 2 * (big + size + 3 * pad)


def write_previews(decoded, outdir):
    os.makedirs(outdir, exist_ok=True)
    for label, rows in decoded.items():
        size = len(rows)
        w = 2 * (size * ZOOM + size + 24)
        c = Canvas(w, size * ZOOM + 16, WHITE)
        tile(c, rows, 0, 0)
        name = re.sub(r"[^a-z0-9]+", "-", label.lower().replace("#", "n"))
        c.save(os.path.join(outdir, name.strip("-") + ".png"))
    # Everything on one sheet.
    order = [k for k in ("il32+l8mk", "icl8", "icl4", "ICN# 1-bit",
                         "is32+s8mk", "ics8", "ics4", "ics# 1-bit",
                         "ih32+h8mk", "ich8", "ich4", "ich# 1-bit")
             if k in decoded]
    w = 2 * (48 * ZOOM + 48 + 24) + 16
    h = sum(len(decoded[k]) * ZOOM + 16 + 30 for k in order) + 16
    c = Canvas(w, h, (0x99, 0x99, 0xCC))
    y = 8
    for k in order:
        tile(c, decoded[k], 8, y, k)
        y += len(decoded[k]) * ZOOM + 16 + 30
    c.save(os.path.join(outdir, "family.png"))


def write_sheet(path):
    """The contact sheet: for each concept, the 32x32 and 16x16 enlarged
    and at actual size on white and grey, then the 1-bit versions."""
    arts = [Art(c) for c in CONCEPTS
            if os.path.exists(os.path.join(ART, c, "icon.txt"))]
    row_h = 32 * ZOOM + 16 + 40
    w = 2 * (32 * ZOOM + 32 + 24) + 2 * (16 * ZOOM + 16 + 24) + \
        (32 * ZOOM + 32 + 24) + (16 * ZOOM + 16 + 24) + 6 * 16 + 16
    h = len(arts) * row_h + 16
    c = Canvas(w, h, (0x99, 0x99, 0xCC))
    y = 8
    for art in arts:
        large, small = Icon(art, 32), Icon(art, 16)
        x = 8
        c.text("%d %s" % (CONCEPTS.index(art.concept) + 1, art.name), x, y,
               scale=3)
        yy = y + 22
        x += tile(c, large.rgba(), x, yy) + 16
        x += tile(c, small.rgba(), x, yy) + 16
        bw32, bw16 = large.bw_rgba(), small.bw_rgba()
        for rows in (bw32, bw16):
            size = len(rows)
            big = size * ZOOM
            c.rect(x, yy, big + size + 24, big + 16, WHITE)
            c.icon(rows, x + 8, yy + 8, ZOOM)
            c.icon(rows, x + 16 + big, yy + 8, 1)
            x += big + size + 24 + 16
        y += row_h
    c.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-c", "--concept", default=CONCEPT, choices=CONCEPTS)
    ap.add_argument("--sheet", action="store_true",
                    help="also write the contact sheet of all concepts")
    ap.add_argument("--custom-icon", action="store_true",
                    help="also store the icns at ID -16455 (custom icon)")
    ap.add_argument("-o", default=REZ_OUT, help="the .r file to write")
    args = ap.parse_args()

    art = Art(args.concept)
    for s in (32, 16):
        if s not in art.grids:
            sys.exit("%s: no %dx%d icon" % (art.path, s, s))
    icons = {s: Icon(art, s) for s in art.grids}
    write_rez(art, icons, args.o, args.custom_icon)
    res = read_rez(args.o)
    if args.custom_icon and res[("icns", -16455)] != res[("icns", 128)]:
        sys.exit("icns -16455 differs from icns 128")
    decoded = decode_all(res)
    write_previews(decoded, PREVIEW)
    print("wrote %s and %s/" % (os.path.relpath(args.o, ROOT),
                                os.path.relpath(PREVIEW, ROOT)))
    if args.sheet:
        sheet = os.path.join(ART, "concepts.png")
        write_sheet(sheet)
        print("wrote %s" % os.path.relpath(sheet, ROOT))


if __name__ == "__main__":
    main()
