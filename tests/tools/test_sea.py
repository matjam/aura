#!/usr/bin/env python3
"""Test the installer's archive reader against the packer.

Usage: tests/tools/test_sea.py SEA_EXTRACT [INSTALLER.bin]

Packs a folder with tools/make-sea.py (fork sizes at the edges of the
reader's buffers, compressible and incompressible data, both forks, empty
files and folders), extracts it with sea_extract, which runs the
installer's own reader (src/platform/macos9/sea/sea_archive.c), and
compares every fork. Then damages the archive in many places and checks
that the reader refuses each copy without crashing. With INSTALLER.bin,
also extracts that file and compares it with the packer's reading of it.
"""

import importlib.util
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "make_sea", os.path.join(HERE, "..", "..", "tools", "make-sea.py"))
make_sea = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_sea)


def run(extract, installer, dest):
    return subprocess.run([extract, installer, dest], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def read_back(installer):
    with open(installer, "rb") as f:
        m = make_sea.macbinary_read(f.read(), installer)
    return make_sea.unpack(m["data"])[1]


def compare(extract, installer, tmp, label, entries=None):
    """Extracts `installer` and compares with the packer's reading of it
    (or with `entries`). Returns the number of entries, or None when the
    reader refused the file."""
    if entries is None:
        entries = read_back(installer)
    dest = os.path.join(tmp, label)
    r = run(extract, installer, dest)
    assert r.returncode in (0, 1), (label, "crash", r.returncode, r.stderr)
    if r.returncode == 1:
        return None
    lines = r.stdout.decode("latin-1").splitlines()
    assert len(lines) == 1 + len(entries), (label, lines[:3])
    expected = set()
    for (kind, path, ftype, creator, flags, _, data, rsrc), line in \
            zip(entries, lines[1:]):
        want = "%d %s %s %s %04x %d %d" % (
            kind, path.decode("latin-1"), ftype.hex(), creator.hex(), flags,
            len(data), len(rsrc))
        assert line == want, (label, line, want)
        host = os.path.join(dest, *path.decode("mac_roman").split(":"))
        if kind == 0:
            assert os.path.isdir(host), host
            continue
        with open(host, "rb") as f:
            assert f.read() == data, (label, path, "data fork")
        expected.add(host)
        if rsrc:
            with open(host + ".rsrc", "rb") as f:
                assert f.read() == rsrc, (label, path, "resource fork")
            expected.add(host + ".rsrc")
    found = {os.path.join(d, n) for d, _, names in os.walk(dest)
             for n in names}
    assert found == expected, (label, found ^ expected)
    return len(entries)


def main():
    extract = sys.argv[1]
    rng = random.Random(20261115)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "src")
        os.makedirs(os.path.join(src, "Fonts", "deep", "deeper"))
        os.makedirs(os.path.join(src, "Empty"))
        # The reader inflates through a 16 KB input and a 32 KB output
        # buffer: sizes around both, in data that does not compress (the
        # input runs out first) and data that does (the output fills
        # first, and the stream can end exactly on a full buffer).
        n = 0
        for size in (0, 1, 2, 16383, 16384, 16385, 32767, 32768, 32769,
                     65535, 65536, 65537, 98304, 300001):
            for kind in ("noise", "zeros", "text"):
                if kind == "noise":
                    b = rng.randbytes(size)
                elif kind == "zeros":
                    b = bytes(size)
                else:
                    b = (b"The quick brown fox. " * (size // 21 + 1))[:size]
                n += 1
                with open(os.path.join(src, "Fonts", "f%02d-%s.ttf"
                                       % (n, kind)), "wb") as f:
                    f.write(b)
        with open(os.path.join(src, "Fonts", "deep", "deeper", "x.html"),
                  "wb") as f:
            f.write(b"<p>hello</p>\n")
        with open(os.path.join(src, "README.md"), "wb") as f:
            f.write("Licence text, café.\nSecond line.\n".encode())
        with open(os.path.join(src, "App.bin"), "wb") as f:
            f.write(make_sea.macbinary_write(
                b"App", b"APPL", b"AURA", 0x2400, rng.randbytes(70000),
                bytes(32768) + rng.randbytes(32768), 0))
        stub = os.path.join(tmp, "stub.bin")
        cfrg = bytes(28) + struct.pack(">I", 1) + b"pwpc" + bytes(18) + \
            bytes((1, 1)) + bytes(16) + b"\x04Stub" + bytes(3)
        with open(stub, "wb") as f:
            f.write(make_sea.macbinary_write(
                b"AuraSEA", b"APPL", b"AuSX", 0, b"Joy!peff" + bytes(1001),
                make_sea.rsrc_fork([(b"cfrg", 0, cfrg)]), 0))
        good = make_sea.build(stub, src, make_sea.MAC_EPOCH)
        installer = os.path.join(tmp, "installer.bin")
        with open(installer, "wb") as f:
            f.write(good)
        entries = read_back(installer)
        count = compare(extract, installer, tmp, "out", entries)
        assert count == len(entries)

        # Damage. The data fork starts at 128; the archive follows the
        # stub's code. Flip one bit at random places and at every byte of
        # the header, the first entry and the trailer; cut the fork short.
        dlen = struct.unpack(">I", good[83:87])[0]
        start = 128 + struct.unpack(">I", good[128 + dlen - 12:
                                               128 + dlen - 8])[0]
        end = 128 + dlen
        places = list(range(start, start + 48 + 52 + 16)) + \
            list(range(end - 20, end)) + \
            [rng.randrange(start, end) for _ in range(150)]
        refused = harmless = 0
        bad_path = os.path.join(tmp, "bad.bin")
        for k, at in enumerate(places):
            bad = bytearray(good)
            bad[at] ^= 1 << rng.randrange(8)
            with open(bad_path, "wb") as f:
                f.write(bad)
            # Refused, or (a bit deflate ignores, such as the padding
            # after a stored block's header) extracted with every fork
            # still right: compare() checks them.
            if compare(extract, bad_path, tmp, "bad%d" % k, entries) is None:
                refused += 1
            else:
                assert at >= start + 48 + 52, ("header damage accepted", at)
                harmless += 1
        assert refused > len(places) * 3 // 4, (refused, harmless)
        for k, cut in enumerate((1, 20, 21, 1000, dlen // 2, dlen - 30)):
            bad = bytearray(good[:128 + dlen - cut])
            bad[83:87] = struct.pack(">I", dlen - cut)
            bad[124:126] = struct.pack(">H", make_sea.crc16(bad[:124]))
            with open(bad_path, "wb") as f:
                f.write(bad)
            r = run(extract, bad_path, os.path.join(tmp, "cut%d" % k))
            assert r.returncode == 1, ("cut accepted", cut, r.returncode)
            refused += 1
        extra = ""
        if len(sys.argv) > 2:
            extra = ", %s: %d entries match" % (
                os.path.basename(sys.argv[2]),
                compare(extract, sys.argv[2], tmp, "real"))
        print("%d entries match, %d damaged copies refused, %d flipped bits "
              "without effect%s" % (count, refused, harmless, extra))


if __name__ == "__main__":
    main()
