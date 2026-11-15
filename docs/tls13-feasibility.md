# TLS 1.3 for Aura: feasibility study

Studied on 2026-11-15, on branch `tls13`. Nothing here is merged; the
experiments are in this branch and are listed under "Experiments".

The question was how feasible TLS 1.3 is and what the best way to add it
is. During the study the question widened: Aura's TLS library, BearSSL
0.6, dates from 2018, and the working assumption became that it should be
replaced by a maintained library rather than extended. The facts support
that assumption. They also show its costs, which are listed.

## Summary

- **Recommendation: replace BearSSL with Mbed TLS 4.1 LTS** (Apache-2.0,
  maintained until March 2029, TLS 1.2 and 1.3). It compiled for PowerPC
  Mac OS 9 with Retro68 without a source change, and a test program
  completed verified TLS 1.3 and TLS 1.2 handshakes in the Mac OS 9 guest
  over Aura's own Open Transport sockets, including to a site that Aura
  cannot reach today.
- **Need.** Of 54 sites tested, 1 refuses TLS 1.2 (68kmla.org, a retro
  Mac forum), 3 offer no TLS 1.3, and 50 accept both. TLS 1.3 is not yet
  required by the acceptance sites; the reason to act is the age of the
  library more than the protocol version.
- **Costs of the replacement.** About 180 KB more code in the
  application (278 KB linked against BearSSL's 101 KB). X25519 is about
  six times slower than BearSSL's (about 25 ms more per handshake in the
  guest); the other primitives are within a factor of two either way.
  Servers that offer only RSA key exchange or DHE in TLS 1.2
  (www.fsf.org) stop working: Mbed TLS 4 removed both. The library keeps
  global state (the PSA key store and its hooks), which Aura's network
  stack otherwise avoids.
- **Not preferred: writing TLS 1.3 in Aura on BearSSL's primitives.** It
  is feasible (the proof below passed), but it leaves Aura maintaining
  protocol code alone on top of a library whose last release is eight
  years old.

## 1. BearSSL's state

Checked against https://bearssl.org/ and a clone of
`https://www.bearssl.org/git/BearSSL` on 2026-11-15.

| Fact | Value |
|---|---|
| Release Aura carries | 0.6, tagged 2018-08-14; still "Current version is 0.6" on the site, described there as beta-quality |
| Commits after 0.6 | 39, the last on 2026-04-06; one author |
| TLS 1.3 | Not in the tree. The site lists it under "Not Yet Implemented" and says it will change the context structures |
| DHE | Not in the tree |
| RSA-PSS | Added to the tree on 2018-08-17 (`br_rsa_pss_vrfy`), three days after 0.6; a verification bug in it was fixed on 2022-06-08. Not in 0.6, and not wired into the X.509 engine (`x509_minimal.t0` still says "TODO: add support for RSA/PSS") |
| Ed25519 | Not in the tree |
| Post-quantum key exchange | Not in the tree |

Fixes made after 0.6 that touch code Aura links:

- 2026-04-04: a chunked decoder must not be entered again after a
  failure (`br_x509_decoder_push` and the key decoder). Aura pushes each
  certificate into `br_x509_decoder` once (`tls.c`, the user's exception),
  so by reading the code this path is not reached. This is a reading, not
  an audit.
- 2026-04-06: incoming CBC records with an invalid length. Aura enables
  no CBC suite.
- 2019-07-20: a buffer overflow in private key decoding. Aura decodes no
  private keys outside its tests.
- 2019-12-14 and 2020-11-18: carry bugs in the 64-bit P-256 code, which
  was added after 0.6 and is not used on a 32-bit target.
- 2018-12-08: the byte order of the Curve25519 scalar changed. It has no
  effect on interoperability, but code that passes a known scalar must
  follow it (the proof below depends on the 0.6 order).

No known unfixed vulnerability applies to Aura's configuration of 0.6, as
far as these commit messages and diffs show. The concern is different:
there has been no release since 2018, the fixes above are only in git, the
project has one maintainer, and the features the web is moving to
(TLS 1.3, RSA-PSS in the handshake, post-quantum key exchange) are absent
with no sign of work on them.

## 2. Need: which sites require TLS 1.3

Each site got one HEAD request through `aura-host fetch -v` (TLS 1.2
only) and one TLS 1.3 handshake with `openssl s_client -tls1_3 -groups
X25519:P-256:P-384` (OpenSSL 4.1.0; the classical groups a client
without post-quantum support would offer). A TLS 1.2 handshake with
OpenSSL was made only where Aura failed. The last column is the
signature scheme of the server's CertificateVerify in TLS 1.3.

Headline numbers, 54 sites:

- 53 accept TLS 1.2 and work in Aura today; 1 refuses it
  (68kmla.org answers a TLS 1.2 ClientHello with a `protocol_version`
  alert).
- 51 offer TLS 1.3; 3 do not (arstechnica.com, bsky.app, www.fsf.org).
- In TLS 1.3, 32 of the 51 sign with `rsa_pss_rsae_sha256` and 19 with
  ECDSA (17 P-256, 2 P-384). RSA-PSS verification is therefore required
  for any TLS 1.3 client; none used Ed25519.
- No site failed in Aura for a cipher reason. www.fsf.org works only
  through the plain RSA key exchange that `tls.c` keeps as a last resort
  (it offers RSA and DHE, no ECDHE).
- Every acceptance site of `docs/v1-scope.md` accepts TLS 1.2.

| Site | TLS 1.2 | Cipher negotiated by Aura | TLS 1.3 suite | TLS 1.3 signature |
|---|---|---|---|---|
| old.reddit.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.reddit.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| en.wikipedia.org | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | ecdsa_secp256r1_sha256 |
| news.ycombinator.com | yes | ECDHE-ECDSA-AES256-GCM-SHA384 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| duckduckgo.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| html.duckduckgo.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| github.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | ecdsa_secp256r1_sha256 |
| accounts.google.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| www.youtube.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| x.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.amazon.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.apple.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.microsoft.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| stackoverflow.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| archive.org | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.bing.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| www.bbc.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.nytimes.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.theguardian.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| arstechnica.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | **not offered** | - |
| lite.cnn.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | ecdsa_secp384r1_sha384 |
| text.npr.org | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| apnews.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| www.ebay.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.etsy.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.walmart.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| www.chase.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.bankofamerica.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.wellsfargo.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.paypal.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.usa.gov | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.irs.gov | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.gov.uk | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.nasa.gov | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | ecdsa_secp256r1_sha256 |
| www.cloudflare.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| www.fastly.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| developer.mozilla.org | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| docs.python.org | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.w3.org | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| www.debian.org | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.fsf.org | yes | RSA-AES128-GCM-SHA256 (no ECDHE offered) | **not offered** | - |
| lobste.rs | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | ecdsa_secp256r1_sha256 |
| www.facebook.com | yes | ECDHE-ECDSA-AES128-GCM-SHA256 | TLS_CHACHA20_POLY1305_SHA256 | ecdsa_secp256r1_sha256 |
| www.linkedin.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.netflix.com | yes | ECDHE-RSA-AES128-GCM-SHA256 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| gitlab.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| bsky.app | yes | ECDHE-RSA-AES128-GCM-SHA256 | **not offered** | - |
| mastodon.social | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| 68kmla.org | **no** | fails: "the server sent a fatal alert" | TLS_CHACHA20_POLY1305_SHA256 | ecdsa_secp384r1_sha384 |
| lowendmac.com | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | rsa_pss_rsae_sha256 |
| www.macintoshrepository.org | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |
| www.macrumors.com | yes | ECDHE-ECDSA-CHACHA20-POLY1305 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| www.rfc-editor.org | yes | ECDHE-ECDSA-AES128-GCM-SHA256 | TLS_AES_256_GCM_SHA384 | ecdsa_secp256r1_sha256 |
| curl.se | yes | ECDHE-RSA-CHACHA20-POLY1305 | TLS_AES_128_GCM_SHA256 | rsa_pss_rsae_sha256 |

Notes on the survey:

- arstechnica.com and bsky.app answer a TLS 1.3-only ClientHello by
  closing the connection and negotiate TLS 1.2 when both are offered.
  A client must therefore keep TLS 1.2.
- Some HEAD requests returned 403 or 405; the TLS result is what was
  recorded.
- macintoshgarden.org and www.google.com were not contacted.

**Trend.** No browser has removed TLS 1.2, and the large sites all still
accept it. Secondary reports of Cloudflare Radar data put TLS 1.3 and
QUIC together at about 94% of traffic in the third quarter of 2026
(https://technologychecker.io/blog/https-adoption; a summary by a third
party, not checked against Radar itself). How's My SSL announced that
from 2026-12-14 it rates TLS 1.2 connections, and TLS 1.3 connections
without post-quantum key agreement, as "Bad"
(https://blog.howsmyssl.com/2026/05/18/tls-1.3-1.2-and-post-quantum-ranking-changes/).
Sites that are TLS 1.3 only are still rare (1 in 54 here), and they are
found among small, recently configured servers, which includes the retro
Mac sites Aura is for. No figure for the share of TLS 1.3-only servers
was found.

## 3. Replacement candidates

Requirements: C, permissive licence compatible with MIT distribution (no
GPL), TLS 1.3 and TLS 1.2 client, no OS services, builds for 32-bit
big-endian with GCC and newlib, no threads.

| Library | Licence | TLS versions | Maintained | Fits | Notes |
|---|---|---|---|---|---|
| **Mbed TLS 4.1 LTS** (with TF-PSA-Crypto 1.1) | Apache-2.0 OR GPL-2.0-or-later (the user picks; take Apache-2.0) | 1.2, 1.3 | Yes: 4.1.1 on 2026-07-07, LTS until March 2029 (`BRANCHES.md`), security fixes with CVEs in each release | **Yes: built and run on the target** | Replaces BearSSL entirely. Details below |
| Mbed TLS 3.6 LTS | same | 1.2, 1.3 | Until March 2027 only | Likely (not tried) | Still has RSA key exchange and DHE-RSA, which 4.x removed. Too short a support period to choose now |
| picotls | MIT; minicrypto parts CC0 and BSD-2 | 1.3 only | Yes (commits in November 2026) | Partly | No TLS 1.2, so BearSSL stays for 1.2. Its minicrypto backend has no RSA and "cannot verify a signature sent by the peer" (its README), so certificate verification would be written by Aura on BearSSL. Its other backend is OpenSSL |
| wolfSSL | GPL-3.0 (or a commercial licence) | 1.2, 1.3 | Yes | No | Excluded by the licence unless a commercial licence is bought |
| BearSSL development tree | MIT | 1.2 | Slowly (section 1) | No | No TLS 1.3 |
| s2n-tls | Apache-2.0 | 1.2, 1.3 | Yes | No | Needs a separate libcrypto (OpenSSL or AWS-LC) and POSIX; from memory, not checked |
| OpenSSL, LibreSSL, BoringSSL, AWS-LC | Apache-2.0 or similar | 1.2, 1.3 | Yes | No | Megabytes of code and a POSIX or Windows platform layer; not tried |
| rustls | Apache-2.0/MIT | 1.2, 1.3 | Yes | No | Rust |

Sources: https://github.com/Mbed-TLS/mbedtls/releases and the release's
`LICENSE`, `BRANCHES.md` and `ChangeLog`; https://github.com/h2o/picotls;
the GitHub licence field of wolfSSL/wolfssl (GPL-3.0).

Mbed TLS is the only candidate that meets every requirement. It is used
widely in embedded systems and is developed under TrustedFirmware.org
(Arm and Linaro), with a published security process (`SECURITY.md`).

## 4. Mbed TLS on the target: what was tried

All of this used the unmodified 4.1.1 release tarball (SHA-256
`3359a349e23d...75c98c`, matching the published checksum), compiled by
`tests/net/mbedtls-trial/build.sh`, which runs none of the release's own
build scripts.

**Configuration** (`tests/net/mbedtls-trial/aura_mbedtls_config.h` and
`aura_crypto_config.h`): client only; TLS 1.2 with ECDHE-ECDSA and
ECDHE-RSA; TLS 1.3 with ephemeral and PSK-ephemeral key exchange and the
middlebox compatibility mode; AES-GCM and ChaCha20-Poly1305; X25519,
P-256, P-384, P-521; RSA PKCS#1 v1.5 and PSS verification; SHA-256, 384,
512 (SHA-1 only so that 41 of 128 root certificates with SHA-1
self-signatures parse; the default verification profile refuses SHA-1 in
a chain); SNI, ALPN, session tickets; X.509 with the trusted-certificate
callback. No sockets, files, threads, PEM, DTLS, server, renegotiation.

**Compiling with Retro68** (GCC 16.1, `powerpc-apple-macos`): every file
compiled on the first attempt with `-std=c99` and no warnings. No patch
was needed.

**What the library asks of the embedder**, all supplied by the test
programs with no OS call inside the library:

| Service | How |
|---|---|
| Transport | Two callbacks (`mbedtls_ssl_set_bio`) that return "want read" or "want write". The Mac OS 9 test maps them onto `AuNetOps.send` and `recv` |
| Memory | `mbedtls_platform_set_calloc_free`. It needs an allocator: there is no static-buffer mode worth using here. `au_alloc` and `au_free` fit; a failed allocation in the library is handled as an error, so `au_try_alloc` could be used to avoid the fatal handler |
| Random numbers | `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG` (one function) in the trial. A port should instead let the library's own generator take its seed through `MBEDTLS_PSA_DRIVER_GET_ENTROPY` from `AuNetOps.entropy` |
| Clocks | `MBEDTLS_PLATFORM_MS_TIME_ALT`, `mbedtls_platform_set_time`, `MBEDTLS_PLATFORM_GMTIME_R_ALT`, from `AuNetOps.now_ms` and `wall_time`; `AURA_FAKE_TIME` keeps working |
| Trust anchors | `mbedtls_ssl_conf_ca_cb`: the library asks for candidates by issuer, and the embedder parses only the matching root (1 of 128 per handshake in the tests) |

These hooks are process-wide, and `psa_crypto_init` creates a global key
store. Aura's stack has no global state today; with Mbed TLS it has this
much.

**Host results** (`tests/net/mbedtls-trial/trial_client.c`, non-blocking
socket): TLS 1.3 and TLS 1.2 against `openssl s_server -tls1_3` and
`-tls1_2` with a local CA, each with a full and a resumed handshake; a
wrong root store gives "not correctly signed by the trusted CA" with the
chain still readable; live handshakes to 68kmla.org (TLS 1.3 only),
www.reddit.com (TLS 1.3, RSA-PSS) and en.wikipedia.org (TLS 1.2, ECDSA),
each verified against the system's root bundle, full then resumed.

**Guest results** (`src/platform/macos9/tlstrial`, QEMU mac99, stock
Mac OS 9.2.2, over Open Transport through `AuNetOps`): TLS 1.3 handshakes
with certificate verification to example.com, www.reddit.com,
en.wikipedia.org and 68kmla.org, TLS 1.2 to github.com, HEAD requests
answered, TLS 1.3 sessions resumed. This shows the big-endian
32-bit build is correct for X25519, P-256 and P-384 ECDSA, RSA-PSS,
ChaCha20-Poly1305, SHA-256/384 and HKDF. AES-GCM was exercised only by
the timing loop, not against a server (the servers chose ChaCha20).

**One failure found:** with the Everest X25519 implementation
(`MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED`), every X25519 handshake in the
guest failed with `MBEDTLS_ERR_SSL_INVALID_MAC`: the shared secret is
wrong on this target. The cause was not investigated (its README names
only x86 as supported). Do not enable it.

### Size

From the linker maps of the two Mac OS 9 test programs (`-O2`, code and
read-only data that the linker kept):

| Library | Code | Read-only data | Total |
|---|---|---|---|
| BearSSL 0.6, as NetTest links it (the whole Aura network stack) | 87.6 KB | 13.1 KB | 101 KB |
| Mbed TLS 4.1.1, trial configuration, as TlsTrial links it | 250.9 KB | 23.9 KB (+3.0 KB writable) | 278 KB |

The application (`Aura.bin`) is 4.07 MB, so the replacement adds about
180 KB, or 4%. Compiling the library with `-Os` made the whole archive a
third smaller (256 KB of code against 378 KB); the linked size at `-Os`
was not measured. `v1-scope.md` sets no size target.

### Memory per connection

Bytes the library allocated, measured with a counting allocator (16 KB
input buffer, 4 KB output buffer):

| | Peak in the handshake | While open |
|---|---|---|
| TLS 1.3 full, 2 to 4 certificates | 35 to 46 KB | 28 to 33 KB |
| TLS 1.3 resumed | 28 KB | 23.5 KB |
| TLS 1.2 full | 32 to 48 KB | 23 to 33 KB |
| Library at rest, no connection | 1.8 KB | |

The open figure includes the peer's certificate chain, which
`MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` keeps; Aura copies the chain for the
certificate viewer anyway and could release the library's copy. For
comparison, `struct AuTls` today is about 42 KB (BearSSL's 33 KB record
buffer, 3.7 KB client context, 3.2 KB X.509 engine) plus the recorded
certificates. Memory use is the same or lower. A saved session is 254 to
314 bytes in TLS 1.3 and 1.0 to 2.0 KB in TLS 1.2 (it includes the
peer's certificate); `AuTlsSession.data` is 128 bytes today and must
grow or become allocated.

### Speed

Timings in the guest, both programs run one after the other in one boot
with the host otherwise idle. QEMU's speed is not a G4's, and repeated
runs differed by up to a factor of two when the host was busy, so read
the ratios, not the values. A run on a real G4 is still needed
(unverified).

| Operation | BearSSL 0.6 (`m31`, `i31`, `ct`) | Mbed TLS 4.1.1 |
|---|---|---|
| SHA-256 | 20,320 KB/s | 62,624 KB/s |
| AES-128-GCM | 7,040 KB/s | 5,472 KB/s |
| ChaCha20-Poly1305 | 39,888 KB/s | 40,992 KB/s |
| X25519, make key pair | 2 ms | 12.1 ms |
| X25519, shared secret | 2 ms (same routine) | 13.8 ms |
| P-256, make key pair | 5 ms | 5.0 ms |
| P-256, shared secret | 7.2 ms | 12.0 ms |
| ECDSA P-256 verify | 13.2 ms | 6.6 ms |
| RSA-2048 verify | 2.8 ms | 3.4 ms |
| RSA-4096 verify | 13.3 ms | 10.2 ms |

Handshake time inside the library, guest: TLS 1.3 to www.reddit.com
(RSA chain of 2) 73 ms, to example.com (ECDSA chain of 4 with P-384) 237
ms, to 68kmla.org 289 ms; resumed TLS 1.3 43 to 76 ms. BearSSL in the
same boot: TLS 1.2 to example.com 436 ms and to Wikipedia 388 ms from
request to response headers including the network, 27 to 62 ms resumed.
The two sets are not the same measurement and the full handshakes are of
the same order.

What the numbers mean:

- **X25519 is the one clear loss**: about 26 ms per handshake against 4
  ms. Mbed TLS runs the Montgomery ladder on its general bignum code;
  BearSSL has dedicated 31-bit field arithmetic. A TLS 1.3 resumption
  with forward secrecy repeats the key exchange, so it costs about 45 ms
  of processor time where a TLS 1.2 session-ID resumption costs almost
  none.
- **The saved round trip.** TLS 1.3 completes the handshake in one round
  trip instead of two. On a connection with 50 to 150 ms of latency that
  saves 50 to 150 ms per new connection, which is more than the X25519
  cost in the guest. On a real 1 GHz G4 the X25519 cost may be several
  times the guest's; if it reaches 100 ms the two cancel. Offering P-256
  first is not better (17 ms for both steps in the guest against 26 ms
  is a small gain, and Mbed TLS's P-256 is not constant-time table-free
  code either). The better fix, if G4 measurements call for it, is an
  X25519 driver for the PSA interface built from BearSSL's
  `ec_c25519_m31.c` (MIT, 800 lines), which the PSA driver model allows
  without patching the library.
- **Bulk encryption** is at parity. Mbed TLS's AES uses lookup tables and
  is not constant-time; BearSSL's `aes_ct` is. On a single-user machine
  without other tenants the cache-timing attack this protects against
  does not apply in practice, and servers mostly choose ChaCha20 when
  Aura prefers it.
- **AltiVec.** Neither library has AltiVec code for these primitives
  (BearSSL's POWER8 AES needs instructions a G4 lacks). No option
  benefits from it without new code. ChaCha20 and Poly1305 would be the
  candidates.

## 5. Migration plan

The steps are ordered so that each leaves a working tree.

1. **Decide and import.** Record the decision (Conventions in
   `CLAUDE.md` names BearSSL; this replaces it). Import Mbed TLS 4.1.x
   and its TF-PSA-Crypto under `third_party/mbedtls`: only `library/`,
   `include/`, and from `tf-psa-crypto` the `core`, `extras`,
   `platform`, `utilities`, `include` and `drivers/builtin` folders,
   with `LICENSE`. Add our `CMakeLists.txt` and the two configuration
   headers (start from the trial's). Keep changes, if any are ever
   needed, as patches in `third_party/mbedtls-patches`, as for
   QuickJS-ng. Add the Apache-2.0 notice to `credits.c`
   (about:licences) and `third_party/README.md`. *Delivers:* the library
   builds for host and PowerPC beside BearSSL. *Test:* `./build.sh all`.
2. **Platform glue** (`src/net/tls_platform.c`): allocator, clocks,
   entropy through `MBEDTLS_PSA_DRIVER_GET_ENTROPY`, `psa_crypto_init`
   once per process. *Test:* a unit test that draws random bytes and
   checks the clock hooks with `AURA_FAKE_TIME`.
3. **Root store.** `gen_roots.py` writes full DER certificates instead
   of BearSSL trust anchors (about 140 KB instead of 51 KB);
   `roots.c` keeps an index of subject names and hands matching roots
   to the library's callback. *Test:* the existing root store tests and
   `fuzz_net`, ported.
4. **`src/net/tls.c` on Mbed TLS**, behind the unchanged `tls.h`
   interface where possible:
   - The interface is buffer-based (`au_tls_in`, `au_tls_out`); Mbed
     TLS calls back for bytes. Put two small staging buffers in
     `AuTls` and have the callbacks read and write them, or change
     `http.c` to pass the socket calls. The first keeps `http.c`
     unchanged.
   - Verification mode `MBEDTLS_SSL_VERIFY_OPTIONAL`: the handshake
     completes, `tls.c` reads the verify flags and the peer chain
     before any application data is sent, and decides. This is where
     the present behaviours go: the certificate viewer's chain
     (`au_cert_parse` on each `raw`), the error text from the flags
     (expired, wrong name, not trusted, weak key), the user's
     exception for one leaf fingerprint, and chain completion.
   - Chain completion and AIA: when the flags say "not trusted", build
     the path as `complete_chain` does today (server certificates, then
     the intermediates pool), parse it into a new chain and call
     `mbedtls_x509_crt_verify_with_ca_cb` with the host name. Every
     check then runs on the completed chain. The AIA fetch in `http.c`
     is unchanged. The trial showed the hook (incomplete-chain.badssl.com
     completes the handshake with the "not trusted" flag and the leaf
     available); the completion itself was not written.
   - Sessions: `mbedtls_ssl_get_session` and `mbedtls_ssl_session_save`
     into a larger `AuTlsSession`; in TLS 1.3 the ticket arrives after
     the handshake (`MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET` from
     the read call), so save when it arrives, not when the handshake
     ends. Keep sessions in memory only: the library's documentation
     requires the application to protect serialized sessions.
   - `au_tls_security`: version and suite names from the library.
   *Test:* `tests/net/test_tls.c` ported (below), `aura-host fetch -v`
   against the survey list, badssl.com's failure hosts, the guest.
5. **The test server.** `tests/net/fakenet.c` runs an in-process
   BearSSL server. Replace it with an in-process Mbed TLS server built
   from the same sources with a second configuration that adds
   `MBEDTLS_SSL_SRV_C` (host tests only; the application's library
   stays client-only). Run each TLS test at TLS 1.2 and at TLS 1.3.
   Regenerate the fixtures (`gen_tls_fixtures.py`) and add RSA-PSS and
   P-384 chains. This was not tried in the study.
6. **Other BearSSL users.** `auth.c` (digest authentication: MD5 and
   SHA-256), `certinfo.c` (fingerprints), `http.c`, `entropy.c` (the
   pool's SHA-256), NetTest: move to `psa_hash_*` or keep a 200-line
   SHA-256 of our own. Digest authentication needs `PSA_WANT_ALG_MD5`.
7. **Remove BearSSL** from `third_party`, the build and the credits.
   *Test:* everything above, sanitizer build, `fuzz_net`, the guest
   with NetTest's URL list, and a timing run on a real G4.

Behaviours kept: SNI, ALPN, resumption, AIA fetching, the certificate
error pages with the chain, the user's exception, HTTPS-first causes.

Behaviours lost or changed:

- **RSA key exchange and DHE are gone** (Mbed TLS 4.0 removed both from
  TLS 1.2; confirmed: the trial fails against www.fsf.org with a
  handshake failure alert). Aura would show its "secure connection
  failed" page for such servers. In the survey that is 1 site of 54.
  Mbed TLS 3.6 still has both but is supported only until March 2027.
- **No global-state-free stack**, as described above.
- **Root store on disk and in memory grows** by about 90 KB.

## 6. Why not write TLS 1.3 on BearSSL

This was the original framing and it is feasible; it is not preferred.

**The proof.** `tests/net/test_tls13_kat.c` (ctest `net_tls13_kat`)
plays the client side of the RFC 8448 "Simple 1-RTT Handshake" trace
with BearSSL 0.6 primitives only: X25519, the whole key schedule checked
against the RFC's values, opening the server's encrypted flight,
verifying the RSA-PSS CertificateVerify with the certificate's key and
the server Finished, producing the client's Finished and application
records byte for byte, deriving the resumption PSK from the
NewSessionTicket, and RSA-PSS with 2048-bit SHA-256 and 3072-bit SHA-384
vectors made by OpenSSL, with negative cases. It passed on the second
run; the first failed on the X25519 scalar's byte order (0.6 takes it
little-endian; the development tree changed that). It is about 450
lines and took one short sitting.

What BearSSL 0.6 provides and lacks for TLS 1.3:

| Piece | In 0.6 | Missing |
|---|---|---|
| Hashes, HMAC | SHA-256, SHA-384, `br_hmac` | |
| HKDF | `br_hkdf_*` runs Extract then Expand in one context | Expand from a given secret (15 lines on `br_hmac`), HKDF-Expand-Label |
| AEAD | `br_gcm` with `br_aes_ct_ctr`, `br_poly1305_ctmul_run` with ChaCha20 | The TLS 1.3 record layer (nonce from the sequence number, inner content type, padding): the 1.2 record code cannot be reused |
| Key exchange | X25519 and P-256 (`br_ec_c25519_m31`, `br_ec_p256_m31`) | |
| Signatures | ECDSA verify, RSA PKCS#1 v1.5 verify, the RSA public operation | RSA-PSS verify (70 lines over `br_rsa_i31_public`; in the proof), Ed25519 (no server in the survey used it) |
| Certificates | The minimal X.509 engine, usable outside the TLS engine as `tls.c` already does for chain completion | RSA-PSS certificate signatures (rare on the web) |
| Handshake | Nothing reusable: BearSSL's engine is a compiled T0 state machine for TLS 1.0 to 1.2 | ClientHello with supported_versions, key_share, signature_algorithms, psk_key_exchange_modes; HelloRetryRequest; EncryptedExtensions; Certificate; CertificateVerify; Finished; NewSessionTicket; KeyUpdate; the compatibility ChangeCipherSpec and legacy session id; downgrade sentinel checks; PSK binders for resumption |

**Two engines, one ClientHello.** BearSSL's API cannot be handed a
ServerHello received by other code, so one ClientHello offering both
versions is not possible while BearSSL handles TLS 1.2. The workable
arrangement is to try TLS 1.3 first with Aura's code and connect again
with BearSSL after a `protocol_version` alert, a TLS 1.2 ServerHello or
a closed connection (arstechnica.com and bsky.app close). That costs an
extra connection to every TLS 1.2-only server, needs a per-host memory
of the result, and the fallback is the weakest point: an attacker who
can drop packets can force TLS 1.2. The standard defence (the downgrade
sentinel in the server random) only works inside one handshake, which
this arrangement does not have.

**Size of the work:** about 2,500 to 3,500 lines of new protocol code in
`src/net` plus tests, vectors and a fuzzer. **Risk:** Aura alone would
maintain a TLS 1.3 handshake; state machine and parser errors in such
code are the classic source of TLS vulnerabilities, and nobody else
would review or test it. **Maintenance:** permanent, and BearSSL beneath
it stays at 2018 with no post-quantum path. Patching BearSSL itself
(the T0 handshake programs) has the same cost with less freedom, and the
site says TLS 1.3 will need the context structures changed.

If this route were ever taken, limit the risk this way: a strict
table-driven state machine that rejects any message out of order; no
optional features (no 0-RTT, no client certificates, no KeyUpdate
initiation, no post-handshake authentication); the RFC 8448 vectors and
OpenSSL `s_server -tls1_3` in the tests; a fuzzer on the handshake
parser under the sanitizers; and the downgrade checks above.

## 7. Related gaps

| Item | Today | With Mbed TLS 4.1 | Advice |
|---|---|---|---|
| Session tickets | TLS 1.2 session IDs only (BearSSL has no ticket support) | TLS 1.2 tickets and TLS 1.3 PSK resumption, shown working | Comes with the replacement |
| OCSP stapling, revocation | None: `tls.c` and BearSSL's minimal engine check no revocation | No stapling support in the client either (no `status_request` code found in the library; from a search of the source) | Leave out. Browsers have moved to short-lived certificates and pushed revocation lists; neither fits here yet |
| Certificate Transparency | Not checked | Not provided | Skip |
| Encrypted Client Hello | No | No | Skip |
| Post-quantum hybrid key exchange (X25519MLKEM768) | No | Not in 4.1.1 (no ML-KEM in the source; ML-DSA only). Whether a later release adds it was not established | Not needed yet: all 51 TLS 1.3 servers accepted classical groups. It adds about 1.2 KB to the ClientHello and a slow key generation on a G4. Follow the library |
| 0-RTT | No | Available, off | Leave off (replay risk, little gain) |
| HTTP/2 over ALPN | No; ALPN offers `http/1.1` | ALPN works the same | A separate large item with no dependency on the TLS library |
| Ed25519 certificates | No | PSA identifiers exist; no built-in implementation was found | Not needed: none seen |

## 8. Recommendation

**Replace BearSSL with Mbed TLS 4.1 LTS, following section 5.**

Comparison of the two real options:

| | Keep BearSSL 0.6, write TLS 1.3 in Aura | Replace with Mbed TLS 4.1 LTS |
|---|---|---|
| Security maintenance, coming years | Aura alone maintains the TLS 1.3 code; BearSSL fixes arrive only in git, with no release since 2018 | A funded project with a security process, CVE handling and an LTS branch to March 2029; Aura updates by replacing a folder |
| Protocol future (post-quantum, new extensions) | Written by Aura each time | Arrives with library updates, when the library has it |
| Work | 2,500 to 3,500 lines of new protocol code, tests, fuzzing; two engines and a fallback path | Rewrite `tls.c` (600 lines) and the test server, change the root store format, move six small BearSSL uses; no protocol code |
| Risk | High: home-made handshake, downgrade weakness in the two-engine fallback | Moderate: a port, with the verification decision in `tls.c` as today |
| Code size on Mac OS 9 | About +30 KB (estimate) | About +180 KB (+4% of the application) |
| Memory per connection | About +2 KB (estimate) | Same or lower than today |
| Speed on a G4 (from guest ratios) | Unchanged primitives; one round trip saved | Parity except X25519, about 6 times slower; one round trip saved |
| Compatibility | Keeps RSA key exchange for servers like www.fsf.org | Loses RSA key exchange and DHE servers |
| Licence | MIT throughout | Apache-2.0 for the library (compatible with distributing Aura under MIT; add its notice). Do not enable Everest (Apache-only is fine, but it fails on this target) |
| Third-party rule | Unchanged | The rule's text changes from BearSSL to Mbed TLS: still two libraries |

Reasons for the choice: the protocol and certificate code that faces
hostile servers should be code that many others run, test and fix; the
library was shown to build and work on the target without changes; and
the costs (180 KB, one slow primitive with a known remedy, a few legacy
servers) are small against that.

What would change the recommendation:

- A real G4 showing handshakes that are unacceptably slow with Mbed TLS
  even with a dedicated X25519 driver.
- A need to keep RSA-key-exchange or DHE-only servers working. Then
  either stay on Mbed TLS 3.6 until March 2027, or accept the loss.
- Memory pressure in the application that makes 180 KB matter.
- A BearSSL release with TLS 1.3 (no sign of one).

What should not be done:

- Do not write a TLS 1.3 handshake in Aura while a maintained library
  fits.
- Do not run two TLS engines with a version fallback between them.
- Do not enable 0-RTT, renegotiation, the Everest X25519 code, or
  SHA-1 in the verification profile.
- Do not persist TLS sessions to disk.
- Do not take the library under its GPL option or bundle wolfSSL.
- Do not merge before the timing run on a real G4 and the full test
  port; BearSSL can stay in the tree until step 7.

## Experiments in this branch

| Path | What it is |
|---|---|
| `tests/net/test_tls13_kat.c`, `tls13_vectors.h`, `gen_tls13_vectors.py` | The RFC 8448 and RSA-PSS proof on BearSSL primitives; runs under ctest as `net_tls13_kat` |
| `tests/net/mbedtls-trial/` | The Mbed TLS trial: configuration headers, `build.sh` (host or `ppc`), `trial_client.c` (host harness), `make-roots.py`. Not built by CMake; the library is not in the repository |
| `src/platform/macos9/tlstrial/` | TlsTrial, the Mac OS 9 console program; built only with `-DAURA_MBEDTLS_DIR=... -DAURA_MBEDTLS_LIB=...` |
| `src/platform/macos9/nettest/nettest.c` | NetTest also times P-256 ECDH, ECDSA verification, RSA-2048 and RSA-4096 and HMAC; its digest buffer was 16 bytes for 32-byte digests and is now 32 |

To repeat the trial: unpack `mbedtls-4.1.1.tar.bz2` from the project's
GitHub releases, run `tests/net/mbedtls-trial/build.sh DIR run/mbed-host
host` and `... run/mbed-ppc ppc -O2`, compile `trial_client.c` against
the host library (the include paths are those in `build.sh`), and
configure `build/ppc` with the two variables to build `TlsTrial.bin`.
Push it with a `trial-roots` file made by `make-roots.py` and open it
in the guest.

## Not verified

- Speed on a real G4. All timings are from QEMU and vary with host load.
- The in-process Mbed TLS test server, chain completion through
  `mbedtls_x509_crt_verify_with_ca_cb`, and the staging-buffer adapter
  for `tls.h` are designs, not code.
- AES-GCM against a live server on the target, and `-Os` linked size.
- Why Everest X25519 fails on big-endian PowerPC.
- Whether BearSSL 0.6 as Aura uses it has a reachable defect among the
  post-0.6 fixes: judged from commit messages and diffs only.
- Mbed TLS's lack of OCSP stapling and Ed25519, and the state of ML-KEM
  after 4.1.1: from source searches, not from its documentation.
- s2n-tls, OpenSSL-family and Mbed TLS 3.6 were not built.
- The traffic-share figures are from third-party summaries.
