#!/usr/bin/env python3
"""Generates the inflate test vectors in tests/codec/inflate-vectors/.

The originals are the uncompressed files in tests/codec/brotli-vectors/
(shared with the brotli tests). For each original this writes, with
Python's zlib and gzip modules (the system zlib):

    NAME.LEVEL.z      zlib format at levels 0, 1, 6, 9
    NAME.6.gz         gzip
    NAME.6.raw        raw deflate
    NAME.fixed.z      zlib with fixed Huffman codes only (Z_FIXED)
    NAME.w9.z         zlib with a 512-byte window
    NAME.flush.z      zlib with a sync flush every 1000 bytes (stored
                      empty blocks mid-stream)

plus a gzip file of two members and one with every optional header
field. The test (test_inflate.c) finds the original by the name before
the first '.' and decodes each vector against it.

Run: python3 -I tests/codec/gen-inflate-vectors.py
"""
import gzip
import os
import struct
import zlib

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "brotli-vectors")
dst = os.path.join(here, "inflate-vectors")
os.makedirs(dst, exist_ok=True)

names = ["empty", "one", "text", "page.html", "random", "repeat", "utf8", "samples"]


def write(name, data):
    with open(os.path.join(dst, name), "wb") as f:
        f.write(data)


def zcompress(data, level=6, wbits=15, strategy=zlib.Z_DEFAULT_STRATEGY, flush_every=0):
    c = zlib.compressobj(level, zlib.DEFLATED, wbits, 9, strategy)
    out = b""
    if flush_every:
        for i in range(0, len(data), flush_every):
            out += c.compress(data[i:i + flush_every]) + c.flush(zlib.Z_SYNC_FLUSH)
    else:
        out += c.compress(data)
    return out + c.flush()


for n in names:
    with open(os.path.join(src, n), "rb") as f:
        data = f.read()
    base = n.replace(".", "_")
    for lvl in (0, 1, 6, 9):
        write("%s.%d.z" % (base, lvl), zcompress(data, lvl))
    write("%s.6.gz" % base, gzip.compress(data, 6, mtime=0))
    write("%s.6.raw" % base, zcompress(data, 6, -15))
    write("%s.fixed.z" % base, zcompress(data, 6, 15, zlib.Z_FIXED))
    write("%s.w9.z" % base, zcompress(data, 6, 9))
    write("%s.flush.z" % base, zcompress(data, 6, 15, flush_every=1000))

# Two gzip members: text then page.html; the expected output is both.
with open(os.path.join(src, "text"), "rb") as f:
    t = f.read()
with open(os.path.join(src, "page.html"), "rb") as f:
    p = f.read()
write("multi.gz", gzip.compress(t, 6, mtime=0) + gzip.compress(p, 6, mtime=0))
with open(os.path.join(dst, "multi"), "wb") as f:
    f.write(t + p)

# Every optional gzip header field: FTEXT|FHCRC|FEXTRA|FNAME|FCOMMENT.
body = zcompress(p, 6, -15)
hdr = b"\x1f\x8b\x08" + bytes([0x1F]) + struct.pack("<I", 0) + b"\x00\x03"
hdr += struct.pack("<H", 6) + b"AB\x02\x00xy"
hdr += b"page.html\x00" + b"a comment\x00"
hdr += struct.pack("<H", zlib.crc32(hdr) & 0xFFFF)
write("page_html.hdr.gz", hdr + body + struct.pack("<II", zlib.crc32(p), len(p) & 0xFFFFFFFF))
