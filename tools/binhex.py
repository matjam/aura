#!/usr/bin/env python3
"""Convert between MacBinary and BinHex 4.0.

Usage:
  tools/binhex.py IN.bin OUT.hqx            encode a MacBinary file
  tools/binhex.py --decode IN.hqx OUT.bin   decode to MacBinary II
  tools/binhex.py --compare IN.hqx REF.bin  exit 0 when the BinHex file
                                            holds the same file as REF.bin
  tools/binhex.py --self-test

BinHex 4.0 carries a Macintosh file (name, type, creator, Finder flags and
both forks) as 7-bit text, so it survives mail, text-mode transfers and
web servers that know nothing of resource forks. It carries no dates;
--decode writes them as zero, and --compare ignores them.

The format, from the outside in:

  1. Text. A line that starts "(This file must be converted with BinHex",
     then the encoded data between two colons, in lines of 64 characters
     (the first colon counts in its line). A decoder skips everything
     before the first colon after that line and ignores line ends.
  2. Six bits per character, most significant bits first, with the
     alphabet in ALPHABET below; the last character is padded with zero
     bits.
  3. Run-length encoding. 0x90 is the marker: "B 90 N" is N copies of B
     (N from 2 to 255, the B before the marker included), and "90 00" is
     one literal 0x90.
  4. The file: name length, name, a zero byte, type, creator, flags
     (16 bits), data fork length, resource fork length (32 bits each),
     CRC of those; the data fork, its CRC; the resource fork, its CRC.
     Numbers are big-endian. Each CRC is CRC-16 with polynomial 0x1021
     and initial value 0 (as XMODEM uses) over its part.

Python 3 standard library only (the binhex module left it in 3.11).
"""

import argparse
import binascii
import struct
import sys

BANNER = "(This file must be converted with BinHex 4.0)"
ALPHABET = ("!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`"
            "abcdefhijklmpqr")
MARK = 0x90
LINE = 64
EOLS = {"lf": "\n", "cr": "\r", "crlf": "\r\n"}


class Error(Exception):
    pass


def crc16(data):
    return binascii.crc_hqx(bytes(data), 0)


# ---- MacBinary -----------------------------------------------------------

class MacFile:
    """A Macintosh file: name (bytes, Mac Roman), type, creator, Finder
    flags, dates (Mac OS seconds) and the two forks."""

    def __init__(self, name, ftype, creator, flags, data, rsrc,
                 created=0, modified=0):
        if not 1 <= len(name) <= 63:
            raise Error("the name must have 1 to 63 bytes")
        self.name, self.type, self.creator = bytes(name), ftype, creator
        self.flags, self.data, self.rsrc = flags, bytes(data), bytes(rsrc)
        self.created, self.modified = created, modified

    def same(self, other):
        return (self.name, self.type, self.creator, self.flags, self.data,
                self.rsrc) == (other.name, other.type, other.creator,
                               other.flags, other.data, other.rsrc)


def macbinary_read(b, path="input"):
    if len(b) < 128 or b[0] != 0 or b[74] != 0 or not 1 <= b[1] <= 63:
        raise Error("%s: not a MacBinary file" % path)
    if crc16(b[:124]) != struct.unpack(">H", b[124:126])[0]:
        raise Error("%s: MacBinary header CRC does not match" % path)
    dlen, rlen, created, modified = struct.unpack(">IIII", b[83:99])
    dpad = (dlen + 127) // 128 * 128
    if len(b) < 128 + dpad + rlen:
        raise Error("%s: shorter than its header says" % path)
    return MacFile(b[2:2 + b[1]], b[65:69], b[69:73], b[73] << 8 | b[101],
                   b[128:128 + dlen], b[128 + dpad:128 + dpad + rlen],
                   created, modified)


def macbinary_write(f):
    h = bytearray(128)
    h[1] = len(f.name)
    h[2:2 + len(f.name)] = f.name
    h[65:69], h[69:73] = f.type, f.creator
    h[73], h[101] = f.flags >> 8, f.flags & 0xFF
    h[83:99] = struct.pack(">IIII", len(f.data), len(f.rsrc), f.created,
                           f.modified)
    h[122], h[123] = 129, 129            # MacBinary II
    h[124:126] = struct.pack(">H", crc16(h[:124]))
    out = bytes(h)
    for fork in (f.data, f.rsrc):
        out += fork + bytes(-len(fork) % 128)
    return out


# ---- BinHex --------------------------------------------------------------

def rle_encode(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        run = 1
        while run < 255 and i + run < n and data[i + run] == b:
            run += 1
        if b == MARK:
            # Every 0x90 is written as a literal.
            out += bytes((MARK, 0)) * run
        elif run >= 3:
            out += bytes((b, MARK, run))
        else:
            out += bytes((b,)) * run
        i += run
    return bytes(out)


def rle_decode(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        i += 1
        if b != MARK:
            out.append(b)
            continue
        if i >= n:
            raise Error("the data ends inside a run")
        count = data[i]
        i += 1
        if count == 0:
            out.append(MARK)
        elif not out:
            raise Error("a run with nothing to repeat")
        else:
            out += bytes((out[-1],)) * (count - 1)
    return bytes(out)


def sixbit_encode(data):
    chars = []
    acc = bits = 0
    for b in data:
        acc = acc << 8 | b
        bits += 8
        while bits >= 6:
            bits -= 6
            chars.append(ALPHABET[acc >> bits & 63])
        acc &= (1 << bits) - 1
    if bits:
        chars.append(ALPHABET[acc << (6 - bits) & 63])
    return "".join(chars)


def sixbit_decode(text):
    values = {c: i for i, c in enumerate(ALPHABET)}
    out = bytearray()
    acc = bits = 0
    for c in text:
        if c not in values:
            raise Error("a character outside the BinHex alphabet: %r" % c)
        acc = acc << 6 | values[c]
        bits += 6
        if bits >= 8:
            bits -= 8
            out.append(acc >> bits & 0xFF)
            acc &= (1 << bits) - 1
    return bytes(out)


def binhex_encode(f, eol="\n"):
    head = bytes((len(f.name),)) + f.name + b"\0" + f.type + f.creator + \
        struct.pack(">HII", f.flags, len(f.data), len(f.rsrc))
    raw = b"".join(part + struct.pack(">H", crc16(part))
                   for part in (head, f.data, f.rsrc))
    text = ":" + sixbit_encode(rle_encode(raw)) + ":"
    lines = [BANNER] + [text[i:i + LINE] for i in range(0, len(text), LINE)]
    return (eol.join(lines) + eol).encode("ascii")


def binhex_decode(b):
    text = b.decode("latin-1")
    at = text.find(BANNER[:40])
    if at < 0:
        raise Error("no BinHex 4.0 line")
    start = text.find(":", text.find(")", at))
    if start < 0:
        raise Error("no data after the BinHex 4.0 line")
    body = "".join(text[start + 1:].split())
    end = body.find(":")
    if end < 0:
        raise Error("the data has no closing colon (cut short?)")
    raw = rle_decode(sixbit_decode(body[:end]))

    pos = 0

    def part(n, what):
        nonlocal pos
        if pos + n + 2 > len(raw):
            raise Error("the data ends inside the %s" % what)
        p = raw[pos:pos + n]
        if crc16(p) != struct.unpack(">H", raw[pos + n:pos + n + 2])[0]:
            raise Error("the CRC of the %s does not match" % what)
        pos += n + 2
        return p

    if not raw or not 1 <= raw[0] <= 63:
        raise Error("bad name length")
    head = part(1 + raw[0] + 19, "header")
    n = head[0]
    flags, dlen, rlen = struct.unpack(">HII", head[n + 10:n + 20])
    data = part(dlen, "data fork")
    rsrc = part(rlen, "resource fork")
    return MacFile(head[1:1 + n], head[n + 2:n + 6], head[n + 6:n + 10],
                   flags, data, rsrc)


# ---- Self-test -----------------------------------------------------------

# A file worked out by hand from the format: name "a", type TEXT, creator
# ttxt, no flags, data fork "Hi\n", no resource fork.
#   header  01 61 00 54 45 58 54 74 74 78 74 00 00 00 00 00 03 00 00 00 00
#           CRC 6A 2D
#   data    48 69 0A, CRC A4 F4;  resource fork empty, CRC 00 00
# The run-length pass changes only the zeros after the creator: five,
# then 03, then four become 00 90 05, 03, 00 90 04. The 27 bytes that
# result make 36 characters.
VECTOR = (b"(This file must be converted with BinHex 4.0)\n"
          b":!@%!9%9B9(4dH(3!N!8$!*!%DLe)D3UNp!!!:\n")


def self_test():
    assert crc16(b"123456789") == 0x31C3, "CRC-16/XMODEM check value"
    # Six-bit coding: the first, last and a middle alphabet character.
    assert sixbit_encode(b"\x00") == "!!"
    assert sixbit_encode(b"\xFF\xFF\xFF") == "rrrr"
    assert sixbit_decode("!!") == b"\x00"
    # Run-length coding, the cases of the format description.
    assert rle_decode(bytes.fromhex("00 11 22 33 44 55 66 77")) == \
        bytes.fromhex("00 11 22 33 44 55 66 77")
    assert rle_decode(bytes.fromhex("11 22 22 22 22 22 22 33")) == \
        rle_decode(bytes.fromhex("11 22 90 06 33"))
    assert rle_decode(bytes.fromhex("11 22 90 00 33")) == \
        bytes.fromhex("11 22 90 33")
    assert rle_decode(bytes.fromhex("2B 90 00 90 05")) == \
        bytes.fromhex("2B 90 90 90 90 90")
    assert rle_encode(bytes.fromhex("11 22 22 22 22 22 22 33")) == \
        bytes.fromhex("11 22 90 06 33")
    assert rle_encode(b"\x90\x90\x90") == b"\x90\x00" * 3
    assert rle_encode(b"\x07" * 600) == \
        bytes.fromhex("07 90 FF 07 90 FF 07 90 5A")
    for sample in (b"", b"\x90", b"a\x90", b"\x00" * 1000, b"ab" * 40,
                   bytes(range(256)) * 3, b"\x90" * 300 + b"\x00\x90\x90"):
        assert rle_decode(rle_encode(sample)) == sample

    # The hand-made vector decodes, and encoding gives it back.
    f = binhex_decode(VECTOR)
    assert (f.name, f.type, f.creator, f.flags, f.data, f.rsrc) == \
        (b"a", b"TEXT", b"ttxt", 0, b"Hi\n", b""), "vector"
    assert binhex_encode(f) == VECTOR, "vector encoding"
    # Decoders skip text before the data and accept any line ending.
    mail = b"From: someone\r\n\r\n" + VECTOR.replace(b"\n", b"\r\n") + b"--\r\n"
    assert binhex_decode(mail).same(f)
    assert binhex_decode(binhex_encode(f, "\r")).same(f)

    # Round trips through MacBinary and BinHex, with both forks, flags,
    # a long run, the marker byte and more than one line.
    rsrc = bytes((i * 7 + 3) & 0xFF for i in range(5000)) + b"\x90" * 9
    g = MacFile(b"Aura Installer", b"APPL", b"AuSX", 0x2400,
                b"\x00" * 700 + b"data \x90 fork" + bytes(range(256)), rsrc)
    mb = macbinary_write(g)
    assert len(mb) % 128 == 0
    assert macbinary_read(mb).same(g)
    hqx = binhex_encode(g)
    lines = hqx.decode("ascii").split("\n")
    assert lines[0] == BANNER and lines[1][0] == ":" and lines[-1] == ""
    assert all(len(x) == LINE for x in lines[1:-2]) and lines[-2][-1] == ":"
    assert all(ord(c) < 127 for c in hqx.decode("ascii"))
    back = binhex_decode(hqx)
    assert back.same(g)
    assert macbinary_write(back) == mb, "round trip is byte for byte"

    # Damage is found: a changed character, and a file cut short.
    bad = bytearray(hqx)
    at = len(BANNER) + 200
    bad[at] = ord("!") if bad[at] != ord("!") else ord("#")
    for broken in (bytes(bad), hqx[:len(hqx) // 2]):
        try:
            binhex_decode(broken)
        except Error:
            pass
        else:
            raise AssertionError("damage went unnoticed")
    print("self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--decode", action="store_true",
                    help="IN is BinHex; write OUT as MacBinary")
    ap.add_argument("--compare", action="store_true",
                    help="IN is BinHex, OUT a MacBinary file to compare with")
    ap.add_argument("--eol", choices=sorted(EOLS), default="lf",
                    help="line ending when encoding (default lf)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("input", nargs="?", metavar="IN")
    ap.add_argument("output", nargs="?", metavar="OUT")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.input or not args.output:
        ap.error("give IN and OUT")
    try:
        with open(args.input, "rb") as fh:
            src = fh.read()
        if args.compare:
            with open(args.output, "rb") as fh:
                ref = macbinary_read(fh.read(), args.output)
            f = binhex_decode(src)
            if not f.same(ref):
                sys.exit("%s and %s hold different files"
                         % (args.input, args.output))
            print("%s holds the same file as %s (\"%s\", %d + %d bytes)" % (
                args.input, args.output, f.name.decode("mac_roman"),
                len(f.data), len(f.rsrc)))
            return
        if args.decode:
            out = macbinary_write(binhex_decode(src))
        else:
            out = binhex_encode(macbinary_read(src, args.input),
                                EOLS[args.eol])
        with open(args.output, "wb") as fh:
            fh.write(out)
    except Error as e:
        sys.exit("%s: %s" % (args.input, e))


if __name__ == "__main__":
    main()
