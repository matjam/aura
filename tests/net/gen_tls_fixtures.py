#!/usr/bin/env python3
"""Generates tests/net/tls_fixtures.c: test certificates and keys for the
in-process TLS server of the network tests.

    python3 tests/net/gen_tls_fixtures.py

Needs the openssl command (3.4 or later, for -not_before/-not_after). All
keys are EC P-256. Certificates:

  ca          the trusted test root (CA:true), 2020-01-01 to 2099-12-31
  other_ca    a root the tests do not trust
  valid       tls.test, issued by ca, 2020 to 2099
  expired     tls.test, issued by ca, 2020-01-01 to 2021-01-01
  wronghost   other.test, issued by ca
  untrusted   tls.test, issued by other_ca

For Authority Information Access (servers that send an incomplete chain):

  inter         "Aura Test Intermediate", a CA issued by ca
  inter2        "Aura Test Intermediate 2", a CA issued by inter, whose
                caIssuers URL is http://ca.test/inter.crt
  aia_leaf      tls.test, issued by inter, caIssuers http://ca.test/inter.crt
  aia_leaf2     tls.test, issued by inter2, caIssuers http://ca.test/inter2.p7c
  noaia_leaf    tls.test, issued by inter, no AIA extension
  untrusted_aia tls.test, issued by other_ca, caIssuers http://ca.test/other.crt
  expired_aia   tls.test, issued by inter, expired, caIssuers inter.crt
  wronghost_aia other.test, issued by inter, caIssuers inter.crt

and inter2_p7, inter2 in a PKCS#7 "certs-only" SignedData (DER), as some
CAs serve their caIssuers files.

The output is deterministic only in shape: keys are fresh on each run, so
regenerate only when the set changes, and commit the result.
"""
import os
import subprocess
import sys
import tempfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tls_fixtures.c")


def run(*args, cwd):
    subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def key(name, cwd):
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", name + ".key", cwd=cwd)


def raw_scalar(name, cwd):
    der = subprocess.run(["openssl", "ec", "-in", name + ".key", "-outform", "DER"], cwd=cwd, check=True,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    i = der.find(b"\x02\x01\x01\x04\x20")
    if i < 0:
        sys.exit("unexpected EC key encoding")
    return der[i + 5:i + 5 + 32]


def self_signed(name, cn, cwd):
    key(name, cwd)
    with open(os.path.join(cwd, name + ".ext"), "w") as f:
        f.write("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n"
                "subjectKeyIdentifier=hash\n")
    run("openssl", "req", "-new", "-key", name + ".key", "-subj", "/O=Aura Tests/CN=" + cn, "-out", name + ".csr",
        cwd=cwd)
    run("openssl", "x509", "-req", "-in", name + ".csr", "-signkey", name + ".key", "-extfile", name + ".ext",
        "-not_before", "20200101000000Z", "-not_after", "20991231235959Z", "-outform", "DER", "-out",
        name + ".der", cwd=cwd)


def leaf(name, host, issuer, nb, na, cwd, aia=None):
    key(name, cwd)
    with open(os.path.join(cwd, name + ".ext"), "w") as f:
        f.write("basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n"
                "subjectAltName=DNS:%s\n" % host)
        if aia:
            f.write("authorityInfoAccess=OCSP;URI:http://ocsp.test/,caIssuers;URI:%s\n" % aia)
    run("openssl", "req", "-new", "-key", name + ".key", "-subj", "/CN=" + host, "-out", name + ".csr", cwd=cwd)
    run("openssl", "x509", "-req", "-in", name + ".csr", "-CA", issuer + ".pem", "-CAkey", issuer + ".key",
        "-CAcreateserial", "-extfile", name + ".ext", "-not_before", nb, "-not_after", na, "-outform", "DER",
        "-out", name + ".der", cwd=cwd)


def intermediate(name, cn, issuer, cwd, aia=None):
    key(name, cwd)
    with open(os.path.join(cwd, name + ".ext"), "w") as f:
        f.write("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n"
                "subjectKeyIdentifier=hash\n")
        if aia:
            f.write("authorityInfoAccess=caIssuers;URI:%s\n" % aia)
    run("openssl", "req", "-new", "-key", name + ".key", "-subj", "/O=Aura Tests/CN=" + cn, "-out", name + ".csr",
        cwd=cwd)
    run("openssl", "x509", "-req", "-in", name + ".csr", "-CA", issuer + ".pem", "-CAkey", issuer + ".key",
        "-CAcreateserial", "-extfile", name + ".ext", "-not_before", "20200101000000Z", "-not_after",
        "20991231235959Z", "-outform", "DER", "-out", name + ".der", cwd=cwd)
    run("openssl", "x509", "-inform", "DER", "-in", name + ".der", "-out", name + ".pem", cwd=cwd)


def c_array(name, data):
    lines = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    with tempfile.TemporaryDirectory() as d:
        self_signed("ca", "Aura Test Root", d)
        self_signed("other_ca", "Untrusted Test Root", d)
        for ca in ("ca", "other_ca"):
            run("openssl", "x509", "-inform", "DER", "-in", ca + ".der", "-out", ca + ".pem", cwd=d)
        leaf("valid", "tls.test", "ca", "20200101000000Z", "20991231235959Z", d)
        leaf("expired", "tls.test", "ca", "20200101000000Z", "20210101000000Z", d)
        leaf("wronghost", "other.test", "ca", "20200101000000Z", "20991231235959Z", d)
        leaf("untrusted", "tls.test", "other_ca", "20200101000000Z", "20991231235959Z", d)
        always = ("20200101000000Z", "20991231235959Z")
        intermediate("inter", "Aura Test Intermediate", "ca", d)
        intermediate("inter2", "Aura Test Intermediate 2", "inter", d, aia="http://ca.test/inter.crt")
        leaf("aia_leaf", "tls.test", "inter", *always, d, aia="http://ca.test/inter.crt")
        leaf("aia_leaf2", "tls.test", "inter2", *always, d, aia="http://ca.test/inter2.p7c")
        leaf("noaia_leaf", "tls.test", "inter", *always, d)
        leaf("untrusted_aia", "tls.test", "other_ca", *always, d, aia="http://ca.test/other.crt")
        leaf("expired_aia", "tls.test", "inter", "20200101000000Z", "20210101000000Z", d,
             aia="http://ca.test/inter.crt")
        leaf("wronghost_aia", "other.test", "inter", *always, d, aia="http://ca.test/inter.crt")
        run("openssl", "crl2pkcs7", "-nocrl", "-certfile", "inter2.pem", "-outform", "DER", "-out", "inter2.p7c",
            cwd=d)
        names = ("ca", "other_ca", "valid", "expired", "wronghost", "untrusted", "inter", "inter2", "aia_leaf",
                 "aia_leaf2", "noaia_leaf", "untrusted_aia", "expired_aia", "wronghost_aia")
        parts = ["/* Generated by gen_tls_fixtures.py; do not edit. Test-only keys. */",
                 '#include "tls_fixtures.h"', ""]
        for n in names:
            der = open(os.path.join(d, n + ".der"), "rb").read()
            parts.append(c_array(n + "_der", der))
            parts.append(c_array(n + "_key", raw_scalar(n, d)))
            parts.append("")
        p7 = open(os.path.join(d, "inter2.p7c"), "rb").read()
        parts.append(c_array("inter2_p7_data", p7))
        parts.append("const unsigned char *const tls_fix_inter2_p7 = inter2_p7_data;")
        parts.append("const size_t tls_fix_inter2_p7_len = sizeof inter2_p7_data;")
        parts.append("")
        parts.append("const TlsFixture tls_fixtures[TLS_FIX_COUNT] = {")
        for n in names:
            parts.append("    {{(unsigned char *)%s_der, sizeof %s_der}, {BR_EC_secp256r1, (unsigned char *)%s_key, "
                         "sizeof %s_key}}," % (n, n, n, n))
        parts.append("};")
        with open(OUT, "w") as f:
            f.write("\n".join(parts) + "\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
