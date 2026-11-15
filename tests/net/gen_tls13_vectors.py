#!/usr/bin/env python3
"""Write tests/net/tls13_vectors.h for test_tls13_kat.c.

Usage: python3 -I tests/net/gen_tls13_vectors.py rfc8448.txt tests/net/tls13_vectors.h

The first argument is the text of RFC 8448 ("Example Handshake Traces for
TLS 1.3", https://www.rfc-editor.org/rfc/rfc8448.txt). The values of its
section 3, "Simple 1-RTT Handshake", are copied out by their labels. The
script also makes two RSA-PSS signatures with the openssl command (RSA
2048 with SHA-256 and RSA 3072 with SHA-384, salt length equal to the
hash length, as TLS 1.3 requires) over a fixed message; the keys are made
on the spot, so those two vectors change on every run.
"""
import re
import subprocess
import sys
import tempfile
import os


def section3(text):
    a = text.index("\n3.  Simple 1-RTT Handshake")
    b = text.index("\n4.  Resumed 0-RTT Handshake")
    lines = []
    for ln in text[a:b].split("\n"):
        if ln.startswith("Thomson ") or ln.startswith("RFC 8448 ") or ln == "\f":
            continue
        lines.append(ln.replace("\f", ""))
    return lines


def blobs(lines):
    """Yield (heading, label, bytes) for every "label (N octets): hex" item."""
    heading = ""
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = re.match(r"^   \{(client|server)\}  (.*)$", ln)
        if m:
            heading = m.group(1) + " " + m.group(2).strip()
            j = i + 1
            while j < len(lines) and lines[j].startswith("      ") and not re.match(r"^      \S.*:", lines[j]) and lines[j].strip():
                heading += " " + lines[j].strip()
                j += 1
            i += 1
            continue
        m = re.match(r"^      (\S.*?) \((\d+) octets\):\s*(.*)$", ln)
        if m:
            label, n, rest = m.group(1), int(m.group(2)), m.group(3)
            hexs = rest.split()
            j = i + 1
            while len(hexs) < n and j < len(lines):
                if lines[j].strip():
                    hexs += lines[j].split()
                j += 1
            data = bytes(int(h, 16) for h in hexs[:n]) if n else b""
            assert len(data) == n, (heading, label)
            yield heading, label, data
            i = j
            continue
        i += 1


def carr(name, data):
    out = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 16):
        out.append("    " + " ".join("0x%02x," % b for b in data[i:i + 16]))
    out.append("};")
    return "\n".join(out)


def pss_vector(bits, md, msg):
    d = tempfile.mkdtemp()
    key = os.path.join(d, "k.pem")
    sig = os.path.join(d, "sig")
    m = os.path.join(d, "msg")
    with open(m, "wb") as f:
        f.write(msg)
    subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:%d" % bits, "-out", key],
                   check=True, capture_output=True)
    subprocess.run(["openssl", "dgst", "-" + md, "-sign", key, "-sigopt", "rsa_padding_mode:pss", "-sigopt",
                    "rsa_pss_saltlen:-1", "-out", sig, m], check=True)
    mod = subprocess.run(["openssl", "rsa", "-in", key, "-noout", "-modulus"], check=True, capture_output=True,
                         text=True).stdout.strip().split("=")[1]
    with open(sig, "rb") as f:
        s = f.read()
    return bytes.fromhex(mod), s


def main():
    text = open(sys.argv[1], encoding="ascii").read()
    want = {
        # (heading prefix, label) -> C name; the first match is taken
        ("client create an ephemeral x25519 key pair", "private key"): "v_client_priv",
        ("client construct a ClientHello handshake message", "ClientHello"): "v_client_hello",
        ("server construct a ServerHello handshake message", "ServerHello"): "v_server_hello",
        ("server extract secret \"handshake\"", "secret"): "v_handshake_secret",
        ("server derive secret \"tls13 c hs traffic\"", "expanded"): "v_c_hs_traffic",
        ("server derive secret \"tls13 s hs traffic\"", "expanded"): "v_s_hs_traffic",
        ("server extract secret \"master\"", "secret"): "v_master_secret",
        ("server derive write traffic keys for handshake data", "key expanded"): "v_s_hs_key",
        ("server derive write traffic keys for handshake data", "iv expanded"): "v_s_hs_iv",
        ("server derive secret \"tls13 c ap traffic\"", "expanded"): "v_c_ap_traffic",
        ("server derive secret \"tls13 s ap traffic\"", "expanded"): "v_s_ap_traffic",
        ("client derive secret \"tls13 res master\"", "expanded"): "v_res_master",
        ("server generate resumption secret \"tls13 resumption\"", "expanded"): "v_resumption_psk",
        ("client send application_data record", "payload"): "v_app_payload",
    }
    got = {}
    records = []  # complete records in order
    for heading, label, data in blobs(section3(text)):
        if label == "complete record":
            records.append((heading, data))
        for (h, l), name in want.items():
            if heading.startswith(h) and label == l and name not in got:
                got[name] = data
    missing = [n for n in want.values() if n not in got]
    assert not missing, missing
    names = ["v_rec_client_hello", "v_rec_server_hello", "v_rec_server_flight", "v_rec_client_finished",
             "v_rec_ticket", "v_rec_client_app", "v_rec_server_app", "v_rec_client_alert", "v_rec_server_alert"]
    assert len(records) == len(names), [h for h, _ in records]
    out = ["/* Generated by gen_tls13_vectors.py from RFC 8448 section 3 (Simple 1-RTT",
           " * Handshake) and, for the v_pss_* values, by the openssl command. Do not edit. */", ""]
    for name in sorted(got):
        out.append(carr(name, got[name]))
    for name, (_, data) in zip(names, records):
        out.append(carr(name, data))
    msg = b"Aura TLS 1.3 feasibility: RSA-PSS vector"
    out.append(carr("v_pss_msg", msg))
    for bits, md in ((2048, "sha256"), (3072, "sha384")):
        mod, sig = pss_vector(bits, md, msg)
        out.append(carr("v_pss_%s_mod" % md, mod))
        out.append(carr("v_pss_%s_sig" % md, sig))
    with open(sys.argv[2], "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
