#!/usr/bin/env python3
"""Generates Aura's root store file (see src/net/roots.h) from a PEM bundle.

Usage: python3 -I src/net/gen_roots.py cacert.pem assets/net/aura-roots.bin

The bundle is Mozilla's root set as published by the curl project
(https://curl.se/ca/cacert.pem); its "Certificate data from Mozilla as
of:" line gives the store's date, which Aura's clock check uses. The PEM
is not kept in git. For each certificate this extracts the subject DN
(DER) and the public key (RSA modulus and exponent, or EC curve and
point) with a small DER reader; certificates with other key types are
skipped and reported.
"""
import base64
import email.utils
import re
import struct
import sys

OID_RSA = bytes.fromhex("2a864886f70d010101")
OID_EC = bytes.fromhex("2a8648ce3d0201")
CURVES = {
    bytes.fromhex("2a8648ce3d030107"): 23,  # P-256
    bytes.fromhex("2b81040022"): 24,  # P-384
    bytes.fromhex("2b81040023"): 25,  # P-521
}


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


def strip_int(b):
    while len(b) > 1 and b[0] == 0:
        b = b[1:]
    return b


def parse(der):
    _, cs, ce = tlv(der, 0)
    tbs = children(der, cs, ce)[0]
    fields = children(der, tbs[2], tbs[3])
    i = 0
    if fields[0][0] == 0xA0:
        i = 1
    # serial, sigalg, issuer, validity, subject, spki
    subject = fields[i + 4]
    spki = fields[i + 5]
    subject_der = der[subject[1]:subject[3]]
    alg, bits = children(der, spki[2], spki[3])
    algf = children(der, alg[2], alg[3])
    oid = der[algf[0][2]:algf[0][3]]
    key = der[bits[2] + 1:bits[3]]  # skip the unused-bits byte
    if oid == OID_RSA:
        _, ks, ke = tlv(key, 0)
        n, e = children(key, ks, ke)[:2]
        return subject_der, ("rsa", strip_int(key[n[2]:n[3]]), strip_int(key[e[2]:e[3]]))
    if oid == OID_EC and len(algf) > 1:
        curve = CURVES.get(der[algf[1][2]:algf[1][3]])
        if curve:
            return subject_der, ("ec", curve, key)
    return subject_der, None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="ascii", errors="replace").read()
    m = re.search(r"Certificate data from Mozilla as of: (.+)", text)
    if not m:
        raise SystemExit("no date line in " + src)
    date = int(email.utils.parsedate_to_datetime(m.group(1).strip() + " +0000").timestamp()) \
        if "GMT" not in m.group(1) else int(email.utils.parsedate_to_datetime(m.group(1).strip()).timestamp())
    pems = re.findall(r"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----", text, re.S)
    out = bytearray()
    count = skipped = 0
    for p in pems:
        der = base64.b64decode("".join(p.split()))
        dn, key = parse(der)
        if not key:
            skipped += 1
            continue
        if len(dn) > 1024:
            skipped += 1
            continue
        out += bytes([1]) + struct.pack(">H", len(dn)) + dn
        if key[0] == "rsa":
            out += bytes([1]) + struct.pack(">H", len(key[1])) + key[1] + struct.pack(">H", len(key[2])) + key[2]
        else:
            out += bytes([2, key[1]]) + struct.pack(">H", len(key[2])) + key[2]
        count += 1
    head = b"AURR" + bytes([1, 0]) + struct.pack(">H", count) + struct.pack(">q", date)
    with open(dst, "wb") as f:
        f.write(head + out)
    print("%s: %d anchors (%d skipped), %d bytes, date %s (%d)" % (dst, count, skipped, len(head) + len(out),
                                                                   m.group(1).strip(), date))


main()
