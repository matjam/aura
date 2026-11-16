# TLS

This document covers TLS in Aura: Mbed TLS behind `src/net/tls.c`, what
Aura offers, certificate checking, the root store, incomplete chains,
old servers, sessions, and the library's memory, clocks and entropy.
Read it before changing `tls.c`, the library's configuration or
patches, or the root store. [`tls13-feasibility.md`](tls13-feasibility.md)
is the historical study that records why BearSSL was replaced; it does
not describe the current code.

TLS is Mbed TLS 4.1 (`third_party/mbedtls`; `third_party/README.md`
says how it is carried and updated), used only through `src/net/tls.c`:

- TLS 1.3 and TLS 1.2;
- X25519 (the key share sent), P-256 and P-384;
- ChaCha20-Poly1305 first and then AES-GCM;
- ECDSA and RSA certificates (RSA-PSS and PKCS#1 v1.5);
- SNI, ALPN `http/1.1`;
- session resumption (tickets and the TLS 1.2 session id, TLS 1.3
  tickets);
- certificate errors reported with the chain (`au_http_cert_info`).

The library has no DHE, and no RSA key exchange except through Aura's
patch (see "Old servers" below).

## To change what Aura offers

To change what Aura offers (versions, cipher suites, groups, signature
algorithms), edit the tables at the top of `tls.c`. To change what the
library is built with, edit `third_party/mbedtls-config`; the Mac OS 9
library is a client only, and the host's adds the server half for the
tests.

## To regenerate the root store

Download `https://curl.se/ca/cacert.pem` and run:

```bash
python3 -I src/net/gen_roots.py cacert.pem assets/net/aura-roots.bin
```

Never add intermediates to it.

The root store (`assets/net/aura-roots.bin`, 127 KB) was made from
curl's Mozilla bundle dated 2026-11-05 (118 roots, checked current on
2026-11-15); it holds the USERTrust and Sectigo Public Server
Authentication R46/E46 roots and ISRG Root X1/X2. It holds whole DER
certificates (format version 2, `roots.h`); a version 1 file from an
older Aura is refused with a message that says so.

## To update Mbed TLS

Follow `third_party/README.md`. Expect to fix
`third_party/mbedtls-patches/0001-tls12-rsa-key-transport.patch` by
hand if the TLS 1.2 handshake files changed, and run `test_legacy_kex`
and `fuzz_net tls`. If `0002-bignum-yield.patch` no longer applies, put
the two yield points back in `mbedtls_mpi_gen_prime` and
`mpi_miller_rabin` and run `js_page_subtle_rsa_gen`.

The library also serves script's crypto.subtle (`src/net/webcrypto.c`,
[`javascript.md`](javascript.md), "crypto.subtle"); the options it needs
are marked in `aura_crypto_config.h`. Script's keys share the library's
key store and random generator with TLS, and its allocations count in
`au_tls_memory`.

## To see a connection's TLS details

`aura-host fetch -v URL` prints the TLS version, cipher and certificate
chain, says when certificates came from caIssuers URLs, and says "RSA
key exchange (no forward secrecy)" for a legacy connection. In the
application the Network window's details and the certificate viewer
show the same ([`developer-tools.md`](developer-tools.md)). NetTest
times handshakes in the guest ([`networking.md`](networking.md)).

## The interface

The interface (`tls.h`) passes buffers: the stack asks for room for
ciphertext from the socket (`au_tls_in`), for ciphertext to send
(`au_tls_out`), for decrypted data (`au_tls_read`) and for room to
write (`au_tls_write`). The library instead calls back for bytes, so
`AuTls` holds staging buffers (4 KB in, 4.5 KB out, 4 KB of decrypted
data, and 4 KB to encrypt while a request is being sent) and `step`
runs the library as far as they allow. `http.c` did not change shape
when the library replaced BearSSL; it gained `session_keep`.

## Certificates

The library verifies the chain during the handshake in its "optional"
mode, which never stops the handshake for a certificate error;
`finish_handshake` reads the verdict and decides before any application
data moves in either direction.

One error is reported per chain, in this order: expired or not yet
valid, wrong host, untrusted, weak key, unsupported algorithm, key
usage.

Do not change the mode to "none": the library then checks nothing,
including key usage.

**Trust anchors** come from the root store through `ca_cb`: the library
asks who may have signed a certificate, and `roots.c` offers the roots
with that subject name, which are parsed then (one per handshake,
usually). Nothing else is ever offered as an anchor.

**The user's exception** (`au_tls_allow_cert`, Continue Anyway) applies
in `finish_handshake` to a chain whose leaf has the accepted
fingerprint; the connection is marked `overridden` and its session is
not kept.

## Incomplete chains

Some servers send their certificate without the intermediate
(macintoshgarden.org, incomplete-chain.badssl.com).

- When the chain as sent does not reach a root, `complete_chain` in
  `tls.c` builds a path from the leaf over the server's certificates
  and the intermediates pool (`certpool.c`) and has the library verify
  that path, so every check runs on the completed chain as if the
  server had sent it. Pool certificates are never trust anchors.
- When the path stops for want of an issuer, `AuCertInfo` names the
  last certificate's caIssuers URL and the transfer (`aia_begin`,
  `aia_step` in `http.c`) fetches it over plain http as a child
  transfer (no cookies, referrer or cache, 16 KB, 10 s, at most 4 per
  transfer, each URL once, failures remembered for the session), adds
  the DER, PKCS#7 or PEM certificates (`au_cert_bundle_parse`,
  `certinfo.c`) to the pool and connects again.
- The pool (32 certificates) is saved as `Intermediates` in the Aura
  preferences folder and `intermediates` in the `aura-host` profile
  (`au_net_intermediates_serialize`); a hostile file cannot grant
  trust.
- `aura-host fetch -v` says when certificates came from caIssuers URLs.

## Old servers

`security.legacy_tls`, "Allow old servers without forward secrecy" in
Preferences > Privacy, on by default.

Some servers offer TLS 1.2 with RSA key exchange and nothing Aura has
with forward secrecy (www.fsf.org: RSA or DHE only). Mbed TLS 4 removed
RSA key exchange, so
`third_party/mbedtls-patches/0001-tls12-rsa-key-transport.patch` puts
the client side back (and the server side for the tests): the suites
`TLS_RSA_WITH_AES_128_GCM_SHA256` and `..._256_GCM_SHA384`, TLS 1.2
only.

- `au_tls_allow_legacy_kex` adds them at the end of the ClientHello's
  list, so a server with ECDHE or TLS 1.3 never takes them; `http.c`
  calls it while `AuNetConfig.legacy_tls` is set.
- A connection that used them has a cipher name starting `RSA-` and
  `AuSecurityInfo.legacy_kex`; `aura-host fetch -v`, the Network
  window's details and the certificate viewer say "RSA key exchange (no
  forward secrecy)", and a document loaded that way shows the amber
  lock that mixed content gets (`AU_SECURITY_MIXED`).
- With the preference off such a server fails with "Secure connection
  failed", and the page names the preference.
- No CBC suites and no DHE were added: no server found needed CBC, and
  the library's finite-field Diffie-Hellman takes only the RFC 7919
  groups, not a server's own parameters.
- To drop the patch, follow the steps in its header.
- When updating Mbed TLS, expect to fix the patch by hand if the TLS
  1.2 handshake files changed, and run `test_legacy_kex` and `fuzz_net
  tls`.

## Sessions

`au_tls_save_session` gives the library's serialized session when there
is a new one: after a full TLS 1.2 handshake, and each time a TLS 1.3
server sends a ticket, which happens after the handshake.
`session_keep` in `http.c` asks after the handshake and on every pass
while the connection is open.

A resumed connection has no chain of its own (`AuCertInfo.count` is 0):
the session entry keeps the chain of the full handshake for the
certificate viewer, and a new ticket replaces the old one in the entry.

Sessions hold key material and the library trusts their contents: keep
them in memory only. A session over 1 KB is not kept.

## Memory, clocks and entropy

`tls_platform.c`:

- **Memory.** The library allocates through `au_try_alloc`; a failed
  allocation fails the handshake with "not enough memory for the secure
  connection" and never reaches the out-of-memory handler
  (`test_alloc_failure`).
- **Clocks.** Certificate dates are checked against the time given to
  `au_tls_new` (so `AURA_FAKE_TIME` works); ticket ages use
  `AuNetOps.now_ms` through `au_tls_tick`, which `au_net_pump` calls.
- **Entropy.** The library's random generator (HMAC-DRBG) takes its
  seed from a pool that `http.c` feeds with `AuNetOps.entropy` output,
  at the first https request and again every 1000 uses; the library
  starts at the first feed, so nothing in it runs before the platform
  has entropy. [`mac-os-9-shell.md`](mac-os-9-shell.md) describes the
  Mac's entropy pool.

## Tests

`tests/net/test_tls.c` runs against the library's server half in
`tests/net/fakenet.c` (`FakeServer.tls_min`, `tls_max`, `no_resume`,
`p256_only` for a HelloRetryRequest, `strip_tls13` for a downgrade
attempt, `mangle` to damage bytes): certificate errors and exceptions,
the clock, HSTS, entropy, the caIssuers cases, both protocol versions,
RSA and P-384 keys, resumption, the downgrade sentinel and allocation
failures. Fixtures come from `tests/net/gen_tls_fixtures.py`.

`fuzz_net tls ITERATIONS SEED SECONDS` fuzzes handshakes through
`tls.c` (ctest `net_tls_fuzz`, 20 seconds at most; it prints how many
it ran); run it longer under the sanitizer build after changing `tls.c`
or updating the library.

## Measured

Measured on 2026-11-15.

| What | Figure |
|---|---|
| `Aura.pef` | grew from 4,041,788 to 4,287,026 bytes (6%) |
| the installer archive | grew by 187 KB, 76 KB of it the root store |
| an open connection | about 26 KB in the library and 13 KB of staging, about what BearSSL's 42 KB was |
| a handshake | about 22 KB more |

In the stock guest (QEMU, not a G4; times vary with the host's load),
by NetTest:

| Handshake | Time |
|---|---|
| full TLS 1.3, RSA-PSS certificate (www.reddit.com) | 112 ms |
| TLS 1.2, ECDSA (github.com) | 211 ms |
| TLS 1.2, RSA (duckduckgo.com) | 231 ms |
| resumed TLS 1.3 | 76 to 83 ms |

X25519 costs about 26 ms per handshake where BearSSL's took 4 ms; that
is not tuned yet. No real G4 has been measured.

## Limits and not done

See [`known-gaps.md`](known-gaps.md#networking-and-tls).
