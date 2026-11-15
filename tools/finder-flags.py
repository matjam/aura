#!/usr/bin/env python3
"""Set Finder flags in MacBinary files.

Retro68's Rez writes MacBinary files with every Finder flag clear. The
Finder reads an application's bundle (its 'BNDL', and so its icon) only
when hasBundle is set, and the file's own custom icon ('icns' and the other
icon resources at ID -16455) only when hasCustomIcon is set.

Usage:
  tools/finder-flags.py --bundle [--custom-icon] FILE.bin...
  tools/finder-flags.py --show FILE.bin...
  tools/finder-flags.py --self-test

The Finder flags are a big-endian word whose high byte is at offset 73 of
the MacBinary header (hasBundle is bit 13, hasCustomIcon bit 10) and whose
low byte, in MacBinary II and III, is at offset 101. Bytes 124-125 hold the
CRC-16 (CCITT polynomial 0x1021, initial value 0, as in XMODEM) of the
header's first 124 bytes; the script checks it before writing and
recomputes it after.

Python 3 standard library only.
"""

import argparse
import binascii
import sys

FLAGS_HIGH = 73          # Finder flags, high byte
FLAGS_LOW = 101          # Finder flags, low byte (MacBinary II)
HAS_BUNDLE = 0x20        # bit 13 of the Finder flags
HAS_CUSTOM_ICON = 0x04   # bit 10
CRC = 124                # CRC-16 of bytes 0-123


def crc16(data):
    """CRC-16/XMODEM, as MacBinary II uses."""
    return binascii.crc_hqx(bytes(data), 0)


def check(header, path="header"):
    if len(header) < 128 or header[0] != 0 or header[74] != 0 or \
            not 1 <= header[1] <= 63:
        raise ValueError("%s: not a MacBinary file" % path)
    if crc16(header[:CRC]) != int.from_bytes(header[CRC:CRC + 2], "big"):
        raise ValueError("%s: header CRC does not match (not MacBinary II?)"
                         % path)


def set_flags(header, flags):
    """Return the header with the flags in `flags` (high byte) set and the
    CRC recomputed."""
    check(header)
    h = bytearray(header)
    h[FLAGS_HIGH] |= flags
    h[CRC:CRC + 2] = crc16(h[:CRC]).to_bytes(2, "big")
    return bytes(h)


def describe(header):
    flags = header[FLAGS_HIGH] << 8 | header[FLAGS_LOW]
    names = [n for n, b in (("hasBundle", 13), ("hasCustomIcon", 10),
                            ("isInvisible", 14), ("isAlias", 15),
                            ("isStationery", 11), ("nameLocked", 12))
             if flags >> b & 1]
    return "type %s creator %s flags 0x%04X %s" % (
        header[65:69].decode("mac_roman"), header[69:73].decode("mac_roman"),
        flags, " ".join(names) or "(none)")


def self_test():
    # The CRC routine against the published check value of CRC-16/XMODEM.
    assert crc16(b"123456789") == 0x31C3, "CRC-16/XMODEM check value"
    # A synthetic MacBinary II header: name "Aura", APPL/AURA, no flags.
    h = bytearray(128)
    h[1] = 4
    h[2:6] = b"Aura"
    h[65:73] = b"APPLAURA"
    h[122], h[123] = 129, 129          # MacBinary II version fields
    h[CRC:CRC + 2] = crc16(h[:CRC]).to_bytes(2, "big")
    check(h)
    out = set_flags(h, HAS_BUNDLE | HAS_CUSTOM_ICON)
    check(out)
    assert out[FLAGS_HIGH] == 0x24
    assert out[:FLAGS_HIGH] == h[:FLAGS_HIGH]
    assert out[FLAGS_HIGH + 1:CRC] == h[FLAGS_HIGH + 1:CRC]
    assert out[CRC:] != h[CRC:]
    # Setting the flags again changes nothing.
    assert set_flags(out, HAS_BUNDLE) == out
    # A damaged header is refused.
    bad = bytearray(out)
    bad[10] ^= 1
    try:
        check(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("a damaged header passed the CRC check")
    print("self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bundle", action="store_true", help="set hasBundle")
    ap.add_argument("--custom-icon", action="store_true",
                    help="set hasCustomIcon")
    ap.add_argument("--show", action="store_true",
                    help="print the type, creator and flags only")
    ap.add_argument("--self-test", action="store_true",
                    help="test the CRC and flag code")
    ap.add_argument("files", nargs="*", metavar="FILE.bin")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.files:
        ap.error("no files")
    flags = (HAS_BUNDLE if args.bundle else 0) | \
        (HAS_CUSTOM_ICON if args.custom_icon else 0)
    if not flags and not args.show:
        ap.error("give --bundle, --custom-icon or --show")
    for path in args.files:
        with open(path, "r+b") as f:
            header = f.read(128)
            try:
                if args.show:
                    check(header, path)
                    print("%s: %s" % (path, describe(header)))
                    continue
                new = set_flags(header, flags)
            except ValueError as e:
                sys.exit(str(e))
            f.seek(0)
            f.write(new)
            print("%s: %s" % (path, describe(new)))


if __name__ == "__main__":
    main()
