#!/usr/bin/env python3
"""Write a PEM certificate bundle as length-prefixed DER for trial_client.

Usage: python3 -I make-roots.py BUNDLE.pem OUT
"""
import base64
import re
import struct
import sys

pem = open(sys.argv[1], encoding="ascii", errors="ignore").read()
n = 0
with open(sys.argv[2], "wb") as out:
    for m in re.finditer(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem, re.S):
        der = base64.b64decode(m.group(1))
        out.write(struct.pack(">I", len(der)) + der)
        n += 1
print("%d certificates" % n)
