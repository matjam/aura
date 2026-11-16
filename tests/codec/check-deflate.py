#!/usr/bin/env python3
"""Checks Aura's deflate encoder against another decoder: runs
test_deflate --emit on the given files and decodes every stream it
wrote with Python's zlib (raw, zlib and gzip framing).

usage: check-deflate.py TEST_DEFLATE FILE...
"""
import os
import subprocess
import sys
import tempfile
import zlib


def decode(data, kind):
    wbits = {"raw": -15, "z": 15, "gz": 31}[kind]
    d = zlib.decompressobj(wbits)
    out = d.decompress(data) + d.flush()
    if not d.eof:
        raise ValueError("the stream does not end")
    if d.unused_data:
        raise ValueError("bytes after the stream")
    return out


def main():
    tool, files = sys.argv[1], sys.argv[2:]
    bad = 0
    count = 0
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([tool, "--emit", tmp] + files, check=True)
        for path in files:
            with open(path, "rb") as f:
                want = f.read()
            base = os.path.basename(path)
            for level in (0, 1, 2):
                for kind in ("raw", "z", "gz"):
                    name = "%s.%d.%s" % (base, level, kind)
                    with open(os.path.join(tmp, name), "rb") as f:
                        data = f.read()
                    count += 1
                    try:
                        got = decode(data, kind)
                    except Exception as e:  # zlib.error or ValueError
                        print("FAIL %s: %s" % (name, e))
                        bad += 1
                        continue
                    if got != want:
                        print("FAIL %s: decodes to different bytes" % name)
                        bad += 1
    print("check-deflate: %d streams, %d failures" % (count, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
