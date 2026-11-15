#!/usr/bin/env python3
"""Generates Aura's root store file (see src/net/roots.h) from a PEM bundle.

Usage: python3 -I src/net/gen_roots.py cacert.pem assets/net/aura-roots.bin

The bundle is Mozilla's root set as published by the curl project
(https://curl.se/ca/cacert.pem); its "Certificate data from Mozilla as
of:" line gives the store's date, which Aura's clock check uses. The PEM
is not kept in git. Each certificate goes into the file whole, in DER.
Certificates whose key is neither RSA nor an EC key on P-256, P-384 or
P-521 are skipped and reported: Aura's TLS library configuration cannot
verify with them.
"""
import base64
import email.utils
import re
import struct
import sys

OID_RSA = bytes.fromhex("2a864886f70d010101")
OID_EC = bytes.fromhex("2a8648ce3d0201")
CURVES = {
    bytes.fromhex("2a8648ce3d030107"),  # P-256
    bytes.fromhex("2b81040022"),  # P-384
    bytes.fromhex("2b81040023"),  # P-521
}
MAX_CERT = 8192


def tlv(buf, pos):
    tag = buf[pos]
    length = buf[pos + 1]
    pos += 2
    if length & 0x80:
        n = length & 0x7F
        length = int.from_bytes(buf[pos:pos + n], "big")
        pos += n
    return tag, pos, pos + length


def children(buf, start, end):
    out = []
    pos = start
    while pos < end:
        tag, cs, ce = tlv(buf, pos)
        out.append((tag, pos, cs, ce))
        pos = ce
    return out


def key_ok(der):
    _, cs, ce = tlv(der, 0)
    tbs = children(der, cs, ce)[0]
    fields = children(der, tbs[2], tbs[3])
    i = 1 if fields[0][0] == 0xA0 else 0
    spki = fields[i + 5]  # serial, sigalg, issuer, validity, subject, spki
    alg = children(der, spki[2], spki[3])[0]
    algf = children(der, alg[2], alg[3])
    oid = der[algf[0][2]:algf[0][3]]
    if oid == OID_RSA:
        return True
    return oid == OID_EC and len(algf) > 1 and der[algf[1][2]:algf[1][3]] in CURVES


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="ascii", errors="replace").read()
    m = re.search(r"Certificate data from Mozilla as of: (.+)", text)
    if not m:
        raise SystemExit("no date line in " + src)
    when = m.group(1).strip()
    date = int(email.utils.parsedate_to_datetime(when if "GMT" in when else when + " +0000").timestamp())
    pems = re.findall(r"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----", text, re.S)
    out = bytearray()
    count = skipped = 0
    for p in pems:
        der = base64.b64decode("".join(p.split()))
        if len(der) > MAX_CERT or not key_ok(der):
            skipped += 1
            continue
        out += struct.pack(">H", len(der)) + der
        count += 1
    head = b"AURR" + bytes([2, 0]) + struct.pack(">H", count) + struct.pack(">q", date)
    with open(dst, "wb") as f:
        f.write(head + out)
    print("%s: %d roots (%d skipped), %d bytes, date %s (%d)" % (dst, count, skipped, len(head) + len(out), when,
                                                                  date))


main()
