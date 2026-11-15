#!/usr/bin/env python3
"""Build Aura's self-extracting archive for Mac OS 9.

Usage:
  tools/make-sea.py DISTFOLDER OUT.bin [--stub AuraSEA.bin] [--date SECONDS]
  tools/make-sea.py --list OUT.bin
  tools/make-sea.py --self-test

Packs the files of DISTFOLDER (build/ppc/dist/Aura) into an archive, joins
it to the installer application (src/platform/macos9/sea, built as
AuraSEA.bin) and writes the result as MacBinary. Opened on Mac OS 9, the
application extracts the folder; src/platform/macos9/sea/sea.c describes
the archive format and reads it. tools/binhex.py turns the MacBinary file
into BinHex.

--list reads a finished file back, inflates every fork, checks the CRCs
and prints the entries and a SHA-256 of the unpacked contents. Two builds
with the same digest install the same folder, whatever their compressed
bytes are (zlib versions differ in what they emit).

Files become Macintosh files as tools/guest-push.sh makes them:

  *.bin             MacBinary: decoded, with its name, type, creator, Finder
                    flags and both forks (the Aura application)
  *.ttf, *.ttc      type tfil, creator AURA
  *.idx             type TEXT, creator ttxt, bytes unchanged
  LICENSE*, OFL*, README*, *.txt, *.md
                    SimpleText documents: Mac Roman, carriage returns
  *.html, *.htm, *.css, *.js, *.svg
                    type TEXT, creator AURA, bytes unchanged
  anything else     type BINA, creator AURA, bytes unchanged

The archive goes after the application's code in the data fork, and the
'cfrg' resource, which says the code runs to the end of the fork, is
changed to give the code's length, so that Mac OS does not load the
archive with it.

Every date in the output is --date (seconds since 1970; default
SOURCE_DATE_EPOCH, else now), so a build is reproducible from the same
inputs.

Python 3 standard library only.
"""

import argparse
import binascii
import hashlib
import os
import re
import struct
import sys
import time
import unicodedata
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(
    __file__)), ".."))
MAGIC = b"AuraSEA1"
ENTRY = 0x4175456E              # 'AuEn'
MAC_EPOCH = 2082844800          # 1904 to 1970, seconds
HAS_BUNDLE, HAS_CUSTOM_ICON, HAS_BEEN_INITED = 0x2000, 0x0400, 0x0100
MAX_PATH = 240


class Error(Exception):
    pass


def crc16(data):
    return binascii.crc_hqx(bytes(data), 0)


def version():
    with open(os.path.join(ROOT, "src", "browser", "credits.h")) as f:
        m = re.search(r'^#define AU_VERSION "([^"]*)"', f.read(), re.M)
    if not m:
        raise Error("no AU_VERSION in src/browser/credits.h")
    return m.group(1)


# ---- MacBinary -----------------------------------------------------------

def macbinary_read(b, path):
    if len(b) < 128 or b[0] != 0 or b[74] != 0 or not 1 <= b[1] <= 63:
        raise Error("%s: not a MacBinary file" % path)
    if crc16(b[:124]) != struct.unpack(">H", b[124:126])[0]:
        raise Error("%s: MacBinary header CRC does not match" % path)
    dlen, rlen = struct.unpack(">II", b[83:91])
    dpad = (dlen + 127) // 128 * 128
    if len(b) < 128 + dpad + rlen:
        raise Error("%s: shorter than its header says" % path)
    return {"name": bytes(b[2:2 + b[1]]), "type": bytes(b[65:69]),
            "creator": bytes(b[69:73]), "flags": b[73] << 8 | b[101],
            "data": bytes(b[128:128 + dlen]),
            "rsrc": bytes(b[128 + dpad:128 + dpad + rlen])}


def macbinary_write(name, ftype, creator, flags, data, rsrc, date):
    h = bytearray(128)
    h[1] = len(name)
    h[2:2 + len(name)] = name
    h[65:69], h[69:73] = ftype, creator
    h[73], h[101] = flags >> 8, flags & 0xFF
    h[83:99] = struct.pack(">IIII", len(data), len(rsrc), date, date)
    h[122], h[123] = 129, 129            # MacBinary II
    h[124:126] = struct.pack(">H", crc16(h[:124]))
    return bytes(h) + data + bytes(-len(data) % 128) + rsrc + \
        bytes(-len(rsrc) % 128)


# ---- The files -----------------------------------------------------------

def mac_text(b):
    """UTF-8 text as a SimpleText document: Mac Roman, carriage returns.
    A character Mac Roman lacks becomes its base letter, or '?'."""
    text = b.decode("utf-8", "replace").replace("\r\n", "\n")
    out = bytearray()
    for ch in text:
        if ch == "\n":
            out.append(13)
            continue
        try:
            out += ch.encode("mac_roman")
        except UnicodeEncodeError:
            base = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore")
            out += base or b"?"
    return bytes(out)


def mac_name(name, path):
    try:
        b = name.encode("mac_roman")
    except UnicodeEncodeError:
        raise Error("%s: the name has characters Mac Roman lacks" % path)
    if not 1 <= len(b) <= 31 or b":" in b:
        raise Error("%s: a Mac name has 1 to 31 characters and no colon"
                    % path)
    return b


def mac_file(path):
    """The Macintosh file a host file becomes: (name, type, creator, flags,
    data fork, resource fork)."""
    base = os.path.basename(path)
    with open(path, "rb") as f:
        b = f.read()
    low = base.lower()
    if low.endswith(".bin"):
        m = macbinary_read(b, path)
        return (mac_name(m["name"].decode("mac_roman"), path), m["type"],
                m["creator"], m["flags"] & ~HAS_BEEN_INITED, m["data"],
                m["rsrc"])
    name = mac_name(base, path)
    if low.endswith((".ttf", ".ttc")):
        return name, b"tfil", b"AURA", 0, b, b""
    if low.endswith(".idx"):
        return name, b"TEXT", b"ttxt", 0, b, b""
    if base.startswith(("LICENSE", "OFL", "README")) or \
            low.endswith((".txt", ".md")):
        return name, b"TEXT", b"ttxt", 0, mac_text(b), b""
    if low.endswith((".html", ".htm", ".css", ".js", ".svg")):
        return name, b"TEXT", b"AURA", 0, b, b""
    return name, b"BINA", b"AURA", 0, b, b""


def deflate(b):
    if not b:
        return b""
    z = zlib.compressobj(9, zlib.DEFLATED, -15)
    return z.compress(b) + z.flush()


def entry(kind, path, ftype, creator, flags, date, data, rsrc):
    if not 1 <= len(path) <= MAX_PATH:
        raise Error("%r: the path is longer than %d bytes" % (path, MAX_PATH))
    cdata, crsrc = deflate(data), deflate(rsrc)
    head = struct.pack(">IBBH4s4sHHII", ENTRY, kind, 0, len(path), ftype,
                       creator, flags, 0, date, date)
    head += struct.pack(">III", len(data), len(cdata), zlib.crc32(data))
    head += struct.pack(">III", len(rsrc), len(crsrc), zlib.crc32(rsrc))
    head += path
    return head + struct.pack(">I", zlib.crc32(head)) + cdata + crsrc


def pack(folder, name, date):
    """The archive of `folder`: header and entries, without the trailer.
    Entries are in sorted order, a folder before what it holds."""
    entries = []
    total = 0

    def walk(host, prefix):
        nonlocal total
        names = sorted(n for n in os.listdir(host) if not n.startswith("."))
        seen = set()
        for n in names:
            p = os.path.join(host, n)
            if os.path.isdir(p):
                mname = mac_name(n, p)
                entries.append(entry(0, prefix + mname, b"\0" * 4, b"\0" * 4,
                                     0, date, b"", b""))
                walk(p, prefix + mname + b":")
            else:
                mname, ftype, creator, flags, data, rsrc = mac_file(p)
                entries.append(entry(1, prefix + mname, ftype, creator,
                                     flags, date, data, rsrc))
                total += len(data) + len(rsrc)
            # HFS names are case-insensitive.
            if mname.lower() in seen:
                raise Error("%s: two items named %r" % (host, mname))
            seen.add(mname.lower())

    walk(folder, b"")
    if not entries:
        raise Error("%s: nothing to pack" % folder)
    head = MAGIC + struct.pack(">II", len(entries), total) + \
        bytes((len(name),)) + name + bytes(31 - len(name))
    return head + b"".join(entries)


# ---- The installer application -------------------------------------------

def set_cfrg_length(rsrc, length):
    """Returns the resource fork with the 'cfrg' (0) member for the data
    fork given `length` instead of "to the end of the fork"."""
    r = bytearray(rsrc)
    data_off, map_off = struct.unpack(">II", r[:8])
    types = map_off + struct.unpack(">H", r[map_off + 24:map_off + 26])[0]
    count = struct.unpack(">H", r[types:types + 2])[0] + 1
    for i in range(count):
        rtype, n, refs = struct.unpack(">4sHH",
                                       r[types + 2 + 8 * i:types + 10 + 8 * i])
        if rtype != b"cfrg":
            continue
        for j in range(n + 1):
            ref = types + refs + 12 * j
            if struct.unpack(">h", r[ref:ref + 2])[0] != 0:
                continue
            at = data_off + int.from_bytes(r[ref + 5:ref + 8], "big") + 4
            members = struct.unpack(">I", r[at + 28:at + 32])[0]
            m = at + 32
            arch, usage, where, offset, old = struct.unpack(
                ">4s18xBBII", r[m:m + 32])
            if members != 1 or arch != b"pwpc" or where != 1 or offset != 0 \
                    or old not in (0, length):
                raise Error("the stub's 'cfrg' is not the one Retro68 "
                            "writes: %d member(s), %r, where %d, offset %d, "
                            "length %d" % (members, arch, where, offset, old))
            r[m + 28:m + 32] = struct.pack(">I", length)
            return bytes(r)
    raise Error("the stub has no 'cfrg' (0) resource")


def build(stub_path, folder, date):
    with open(stub_path, "rb") as f:
        stub = macbinary_read(f.read(), stub_path)
    if stub["type"] != b"APPL":
        raise Error("%s: not an application" % stub_path)
    code = stub["data"]
    code += bytes(-len(code) % 4)
    archive = pack(folder, b"Aura", date)
    trailer = MAGIC + struct.pack(">III", len(code), len(archive),
                                  zlib.crc32(archive[:48]))
    name = ("Aura %s Installer" % version()).encode("mac_roman")
    if len(name) > 31:
        raise Error("the name %r is longer than 31 characters" % name)
    return macbinary_write(name, b"APPL", stub["creator"],
                           HAS_BUNDLE | HAS_CUSTOM_ICON,
                           code + archive + trailer,
                           set_cfrg_length(stub["rsrc"], len(code)), date)


# ---- Reading an archive back ---------------------------------------------

def unpack(data):
    """Parses the archive at the end of a data fork the way sea.c does.
    Returns (folder name, [(kind, path, type, creator, flags, date, data,
    rsrc)])."""
    if len(data) < 68 or data[-20:-12] != MAGIC:
        raise Error("no archive at the end of the data fork")
    offset, length, want = struct.unpack(">III", data[-12:])
    if offset + length + 20 != len(data) or length < 48:
        raise Error("the trailer does not match the fork's length")
    a = data[offset:offset + length]
    if a[:8] != MAGIC or zlib.crc32(a[:48]) != want:
        raise Error("the archive header is damaged")
    count, total = struct.unpack(">II", a[8:16])
    name = a[17:17 + a[16]]
    pos = 48
    out = []
    seen = 0
    for _ in range(count):
        (magic, kind, _, plen, ftype, creator, flags, _, created, modified,
         dlen, dclen, dcrc, rlen, rclen, rcrc) = struct.unpack(
            ">IBBH4s4sHHIIIIIIII", a[pos:pos + 52])
        if magic != ENTRY or kind > 1 or not 1 <= plen <= MAX_PATH:
            raise Error("entry %d is damaged" % len(out))
        path = a[pos + 52:pos + 52 + plen]
        end = pos + 52 + plen
        if zlib.crc32(a[pos:end]) != struct.unpack(">I", a[end:end + 4])[0]:
            raise Error("%r: the entry's CRC does not match" % path)
        if path.startswith(b":") or path.endswith(b":") or b"::" in path:
            raise Error("%r: not a relative path" % path)
        pos = end + 4
        forks = []
        for flen, clen, crc in ((dlen, dclen, dcrc), (rlen, rclen, rcrc)):
            raw = zlib.decompress(a[pos:pos + clen], -15) if clen else b""
            if len(raw) != flen or zlib.crc32(raw) != crc:
                raise Error("%r: a fork does not match its length or CRC"
                            % path)
            forks.append(raw)
            pos += clen
        seen += dlen + rlen
        out.append((kind, path, ftype, creator, flags, created, forks[0],
                    forks[1]))
    if pos != len(a) or seen != total:
        raise Error("the archive's length or total does not match")
    return name, out


def listing(path):
    with open(path, "rb") as f:
        m = macbinary_read(f.read(), path)
    name, entries = unpack(m["data"])
    digest = hashlib.sha256()
    print("%s: \"%s\", %s/%s, flags 0x%04X, code %d bytes, archive %d bytes"
          % (path, m["name"].decode("mac_roman"),
             m["type"].decode("mac_roman"), m["creator"].decode("mac_roman"),
             m["flags"], struct.unpack(">I", m["data"][-12:-8])[0],
             struct.unpack(">I", m["data"][-8:-4])[0]))
    print("folder \"%s\", %d entries" % (name.decode("mac_roman"),
                                         len(entries)))
    for kind, p, ftype, creator, flags, date, data, rsrc in entries:
        digest.update(struct.pack(">BH", kind, len(p)) + p + ftype +
                      creator + struct.pack(">HII", flags, len(data),
                                            len(rsrc)) + data + rsrc)
        if kind == 0:
            print("  %-44s folder" % (p.decode("mac_roman") + ":"))
        else:
            print("  %-44s %s/%s 0x%04X %9d %8d" % (
                p.decode("mac_roman"), ftype.decode("mac_roman"),
                creator.decode("mac_roman"), flags, len(data), len(rsrc)))
    print("date %s" % time.strftime(
        "%Y-%m-%d %H:%M:%S UTC", time.gmtime(entries[0][5] - MAC_EPOCH)))
    print("contents sha256 %s" % digest.hexdigest())


# ---- Self-test -----------------------------------------------------------

def rsrc_fork(resources):
    """A resource fork holding `resources`, a list of (type, id, bytes)."""
    data = b"".join(struct.pack(">I", len(d)) + d for _, _, d in resources)
    types = sorted({t for t, _, _ in resources})
    type_list = struct.pack(">H", len(types) - 1)
    refs = b""
    ref_at = 2 + 8 * len(types)
    off = 0
    offsets = {}
    for t, i, d in resources:
        offsets[(t, i)] = off
        off += 4 + len(d)
    for t in types:
        mine = [(i, d) for tt, i, d in resources if tt == t]
        type_list += struct.pack(">4sHH", t, len(mine) - 1,
                                 ref_at + len(refs))
        for i, d in mine:
            refs += struct.pack(">hhI4x", i, -1, offsets[(t, i)])
    body = type_list + refs
    rmap = bytes(24) + struct.pack(">HH", 28, 28 + len(body)) + body
    head = struct.pack(">IIII", 256, 256 + len(data), len(data), len(rmap))
    return head + bytes(240) + data + rmap


def self_test():
    import tempfile
    cfrg = bytes(28) + struct.pack(">I", 1) + b"pwpc" + bytes(18) + \
        bytes((1, 1)) + bytes(8) + bytes(8) + b"\x04Stub" + bytes(3)
    stub_rsrc = rsrc_fork([(b"SIZE", -1, bytes(10)), (b"cfrg", 0, cfrg),
                           (b"vers", 1, b"\0" * 8)])
    code = b"Joy!peffpwpc" + bytes(range(256)) * 3 + b"x"   # not 4-aligned
    patched = set_cfrg_length(stub_rsrc, 784)
    assert len(patched) == len(stub_rsrc)
    diff = [i for i in range(len(patched)) if patched[i] != stub_rsrc[i]]
    assert diff and diff[-1] - diff[0] < 4, "only the length changes"
    assert set_cfrg_length(patched, 784) == patched

    app = macbinary_write(b"Aura", b"APPL", b"AURA",
                          HAS_BUNDLE | HAS_CUSTOM_ICON | HAS_BEEN_INITED,
                          b"code" * 5000, b"\x90resources" * 300, 1)
    with tempfile.TemporaryDirectory() as tmp:
        d = os.path.join(tmp, "Aura")
        os.makedirs(os.path.join(d, "Fonts", "inter"))
        os.makedirs(os.path.join(d, "Empty"))
        files = {
            "Aura.bin": app,
            "aura-roots": bytes(range(256)) * 9,
            "Fonts/fonts.idx": b"one\ntwo\n",
            "Fonts/README.md": "Café — naïve ł 中\n".encode(),
            "Fonts/inter/Inter.ttf": b"\0\1\0\0" + bytes(5000),
            "Fonts/inter/empty.txt": b"",
        }
        for rel, b in files.items():
            with open(os.path.join(d, rel), "wb") as f:
                f.write(b)
        stub_path = os.path.join(tmp, "stub.bin")
        with open(stub_path, "wb") as f:
            f.write(macbinary_write(b"AuraSEA", b"APPL", b"AuSX", 0, code,
                                    stub_rsrc, 0))
        out = build(stub_path, d, 1700000000 + MAC_EPOCH)
        assert out == build(stub_path, d, 1700000000 + MAC_EPOCH), \
            "the same inputs give the same bytes"
        m = macbinary_read(out, "out")
        assert m["type"] == b"APPL" and m["creator"] == b"AuSX"
        assert m["flags"] == HAS_BUNDLE | HAS_CUSTOM_ICON
        assert m["name"].startswith(b"Aura ") and \
            m["name"].endswith(b" Installer")
        assert m["data"].startswith(code + bytes(3))
        assert m["rsrc"] == set_cfrg_length(stub_rsrc, len(code) + 3)
        name, entries = unpack(m["data"])
        assert name == b"Aura"
        got = {e[1]: e for e in entries}
        assert [e[1] for e in entries] == [
            b"Aura", b"Empty", b"Fonts", b"Fonts:README.md",
            b"Fonts:fonts.idx", b"Fonts:inter", b"Fonts:inter:Inter.ttf",
            b"Fonts:inter:empty.txt", b"aura-roots"], [e[1] for e in entries]
        a = got[b"Aura"]
        assert a[2:5] == (b"APPL", b"AURA", HAS_BUNDLE | HAS_CUSTOM_ICON)
        assert a[6] == b"code" * 5000 and a[7] == b"\x90resources" * 300
        assert got[b"Fonts"][0] == 0 and got[b"Empty"][0] == 0
        assert got[b"Fonts:inter:Inter.ttf"][2:4] == (b"tfil", b"AURA")
        assert got[b"Fonts:fonts.idx"][2:4] == (b"TEXT", b"ttxt")
        assert got[b"Fonts:fonts.idx"][6] == b"one\ntwo\n"
        assert got[b"Fonts:README.md"][6] == \
            b"Caf\x8e \xd1 na\x95ve ? ?\r", got[b"Fonts:README.md"][6]
        assert got[b"aura-roots"][2:4] == (b"BINA", b"AURA")
        assert got[b"Fonts:inter:empty.txt"][6] == b""
        # Damage anywhere in the archive is found.
        for at in (len(code) + 3 + 9, len(code) + 3 + 60, len(m["data"]) - 30,
                   len(m["data"]) - 5):
            bad = bytearray(m["data"])
            bad[at] ^= 0x40
            try:
                unpack(bytes(bad))
            except (Error, zlib.error):
                pass
            else:
                raise AssertionError("damage at %d went unnoticed" % at)
        # A name over 31 characters is refused.
        with open(os.path.join(d, "x" * 32), "wb") as f:
            f.write(b"")
        try:
            build(stub_path, d, 0)
        except Error:
            pass
        else:
            raise AssertionError("a 32-character name was accepted")
    print("self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--stub", metavar="AuraSEA.bin", default=os.path.join(
        ROOT, "build", "ppc", "src", "platform", "macos9", "sea",
        "AuraSEA.bin"), help="the installer application (MacBinary)")
    ap.add_argument("--date", type=int, metavar="SECONDS",
                    help="the date of every file, in seconds since 1970")
    ap.add_argument("--list", action="store_true",
                    help="check and list a finished file")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("paths", nargs="*", metavar="PATH")
    args = ap.parse_args()
    try:
        if args.self_test:
            self_test()
        elif args.list:
            if len(args.paths) != 1:
                ap.error("--list takes one file")
            listing(args.paths[0])
        else:
            if len(args.paths) != 2:
                ap.error("give DISTFOLDER and OUT.bin")
            date = args.date
            if date is None:
                date = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
            out = build(args.stub, args.paths[0], date + MAC_EPOCH)
            with open(args.paths[1], "wb") as f:
                f.write(out)
            print("%s: %d KB" % (args.paths[1], len(out) // 1024))
    except (Error, zlib.error) as e:
        sys.exit("make-sea: %s" % e)


if __name__ == "__main__":
    main()
