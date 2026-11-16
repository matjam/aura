# Third-party code

Aura carries two third-party libraries, QuickJS-ng and Mbed TLS. Each entry
records where the code came from and how to update it.

## QuickJS-ng (JavaScript engine)

- **Version:** v0.18.0, commit d7215b0ad3fc2c562e8317fd9bca058fd4863804
  (2026-11-01).
- **Source:** https://github.com/quickjs-ng/quickjs, a shallow clone of
  the tag `v0.18.0`.
- **Licence:** MIT, `quickjs/LICENSE`.
- **What is kept:** the engine only: `quickjs.c`, `libregexp.c`,
  `libunicode.c`, `dtoa.c` and their headers (`quickjs-patches/apply.sh`
  lists them). No CLI (`qjs`, `qjsc`), no `quickjs-libc`, no test262
  harness; a subset of the tests (below).
- **Build:** `quickjs.cmake` builds the static library `aura_quickjs`
  with QuickJS-ng's own warning flags, `-funsigned-char` and
  `-fno-strict-aliasing` (QuickJS passes `uint32_t *` as `int *`, and
  `uint32_t` is `unsigned long` in Retro68's newlib), and links it into
  `aura_engine`. Aura reaches it only through `src/js`.
- **Changes:** every change is a patch in `quickjs-patches/`, applied in
  name order. To update QuickJS-ng, fetch the new release and run
  `third_party/quickjs-patches/apply.sh PRISTINE_DIR`, which copies the
  engine files into `third_party/quickjs` and applies the patches; fix any
  patch that no longer applies; then run `./build.sh test` and the guest
  self-test (`src/platform/macos9/jstest`). The patches are:
  `0001` newlib portability (no pthreads or C11 atomics, big-endian
  checks), `0002` a retry hook in module resolution so the embedder can
  fetch a missing module and resolve again, `0003` a GC mark for
  WeakMap values with symbol keys (fixes a realm leak), `0004` 16
  bytes of padding on every `alloca` on Mac OS 9, because Retro68's GCC
  places spilled locals in the top 8 bytes of alloca space (the patch
  header has a reproduction), `0005` a working `JS_WRITE_OBJ_BSWAP`:
  bytecode is in the byte order of the machine that reads it and
  v0.18.0 swaps nowhere, so the build tool `tools/jsprelude` writes the
  prelude's bytecode byte-swapped for the PowerPC build, `0006`
  one shared copy of the source text for a function and the functions
  nested in it (each used to keep its own copy), `0007`
  `JS_SetTimeSource`, through which the embedder supplies Date's clock
  and time zone offset (`src/js/clock.h`), and `0008` one source text
  per script: the parser copies no function text while parsing, a
  script made mostly of functions keeps one text that all its functions
  point into, and the contexts of a runtime that compile the same
  script share that text, and `0009` bytecode that names positions in
  the script's text (`JS_WRITE_OBJ_SOURCE_REF`, `JS_ReadObjectSource`),
  with which `src/js/runtime.c` compiles a large script once per page,
  and a parser that trims each function's bytecode buffer, and `0010`
  module graphs resolved again without compiling their modules again:
  resolution reports every missing import in one pass,
  `JS_EVAL_FLAG_NO_RESOLVE` compiles a module without resolving it,
  unresolved modules stay loaded, and `JS_FindLoadedModule` finds one
  by name (`src/js/module.c`), and `0011` a parser that records what its
  bracket look-ahead (`js_parse_skip_parens_token`) found for each
  bracket it passed, instead of scanning nested brackets once per level,
  and `0012` a parser that finds a scoped variable through the variable
  hash table instead of walking the scope chain (`scope_chain_lookup`),
  and `0013` less memory while compiling a large script (5-byte source
  positions in phase 1 bytecode, the phase 2 and 3 state out of
  `JSFunctionDef`, one trim after each function, the compiler's blocks
  outside the small-block arenas) and `JS_EVAL_FLAG_NO_SOURCE`, which
  keeps no source text (`Function.prototype.toString` then gives the
  native function form with the function's kind, or `class C { }`),
  and `0014` correctly typed realloc wrappers for `unicode_normalize`
  (the casts it replaces were undefined behaviour that UBSan reports).
- **Tests:** `quickjs/tests` holds the subset of upstream's `tests/`
  folder that needs no `quickjs-libc` (no `qjs:std`, `qjs:os`, `bjson`,
  workers or test262 harness); the microbenchmark is left out.
  `tests/js/CMakeLists.txt` runs each through `aura-host js`. Copy them
  again from the new release when updating.

## Mbed TLS (TLS and X.509), with TF-PSA-Crypto

- **Version:** Mbed TLS 4.1.1, the 4.1 long-term support branch
  (supported until March 2029), released 2026-07-07. It carries
  TF-PSA-Crypto 1.1.1, its cryptography library, in `tf-psa-crypto/`:
  in Mbed TLS 4 the two are separate trees and both are needed.
- **Source:**
  https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2
- **SHA-256 of the tarball:**
  3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
  (the release's `mbedtls-4.1.1-sha256sum.txt` gives the same).
- **Imported:** 2026-11-15, replacing BearSSL 0.6
  (`docs/tls13-feasibility.md` records why).
- **Licence:** Apache-2.0 OR GPL-2.0-or-later, at the user's choice
  (`mbedtls/LICENSE`, `mbedtls/tf-psa-crypto/LICENSE`). Aura takes it
  under Apache-2.0. The distribution carries the licence text as
  `Licences/MbedTLS.txt` (`build.sh`), and about:licences shows the
  notice (`src/browser/credits.c`).
- **What is kept:** `library/` and `include/`, and from `tf-psa-crypto/`
  the folders `core`, `dispatch`, `extras`, `platform`, `utilities`,
  `include` and `drivers/builtin`, with the licences and READMEs
  (`mbedtls-patches/apply.sh` lists them). Left out: tests, programs,
  documentation, scripts, the framework, the release's build files, and
  the Everest, p256-m and post-quantum drivers.
- **Build:** `mbedtls.cmake` builds the static library `aura_mbedtls`
  from those sources with Aura's configuration,
  `mbedtls-config/aura_mbedtls_config.h` and `aura_crypto_config.h`: a
  TLS 1.2 and 1.3 client; ECDHE with X25519, P-256 and P-384; ECDSA and
  RSA (PKCS#1 v1.5 and PSS) verification; AES-GCM and ChaCha20-Poly1305;
  SHA-256, SHA-384 and SHA-512 (SHA-1 only to parse root certificates);
  HKDF; X.509 parsing and verification with the trusted-certificate
  callback; session tickets, SNI and ALPN. For script's crypto.subtle
  (`src/net/webcrypto.c`) it also has AES-CBC with PKCS#7 padding,
  AES-CTR and AES-ECB (which bring in the cipher module), PBKDF2 with
  HMAC, HKDF as one algorithm, and RSA private keys (import, export,
  generation). No server, no DTLS, no
  sockets, files, threads, operating system time or entropy: the
  library gets memory, clocks and entropy from `src/net/tls_platform.c`.
  The library is compiled with `-O2` and without warnings; Aura reaches
  it only from `src/net/tls.c`, `src/net/tls_platform.c`, NetTest and the
  tests.
- **Host builds add the server half** (`AURA_MBEDTLS_SERVER`, set by
  `mbedtls.cmake` for every build except Mac OS 9): the in-process TLS
  server of `tests/net/fakenet.c` needs it. The library's structures
  depend on the configuration, so the definition is public to
  everything that includes its headers.
- **Do not enable `MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED`.** With it,
  X25519 gave wrong shared secrets on big-endian PowerPC and every
  handshake failed its first integrity check.
- **Changes:** two patches. `mbedtls-patches/0001-tls12-rsa-key-transport.patch`
  restores RSA key transport for TLS 1.2 (the two
  `TLS_RSA_WITH_AES_*_GCM_*` suites), which Mbed TLS 4.0 removed, for
  old servers that offer nothing with forward secrecy. It is 250 added
  lines in seven files, all under the option
  `AURA_MBEDTLS_KEY_EXCHANGE_RSA`, and follows Mbed TLS 3.6's code.
  `mbedtls-patches/0002-bignum-yield.patch` adds a call to the
  embedder's `aura_mbedtls_yield` before each prime candidate and each
  Miller-Rabin round, under the option `AURA_MBEDTLS_YIELD`, so that RSA
  key generation for crypto.subtle can leave its stack between steps (a
  nonzero return abandons it). Each patch's header says why it exists
  and how to drop it. The vendored tree is
  the release with the patches applied. Any change goes in
  `mbedtls-patches/` as a patch, applied in name order by `apply.sh`. To update Mbed TLS, download the
  new release of the 4.1 branch, check its SHA-256, and run
  `third_party/mbedtls-patches/apply.sh UNPACKED_DIR`, which replaces
  `third_party/mbedtls` and applies the patches; update this file and
  the version in `src/browser/credits.c`; then run `./build.sh all`,
  the sanitizer build's tests, and NetTest in the guest. `mbedtls.cmake`
  leaves out the source files that Aura's configuration turns off (they
  compile to nothing); if a new release or a configuration change needs
  one of them, the link fails with undefined symbols, and the file comes
  off that list.

## Data from other projects

These are not code, but are generated from other projects' files:

- `src/codec/brotli_dict_data.c`: the Brotli static dictionary (RFC 7932
  appendix A), compressed. Generated by `src/codec/gen_brotli_dict.py`
  from libbrotlicommon 1.2.0. Brotli is MIT licensed, "Copyright (c)
  2009, 2010, 2013-2016 by the Brotli Authors."
- `src/net/psl_data.c`: the Public Suffix List (Mozilla Public License
  2.0), generated by `src/net/gen_psl.py`. The list version is recorded
  in the file.
- `assets/net/aura-roots.bin`: the root certificates of the curl CA
  bundle (Mozilla's root store, MPL 2.0), whole, in DER, generated by
  `src/net/gen_roots.py`.

