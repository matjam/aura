# Networking

This document covers the portable network stack in `src/net`: the HTTP
client, the loader, the HTTP cache and disk cache, the DNS cache,
persisted state, HTTPS-first, and how to try the stack on the host and
on Mac OS 9. Read it before changing how Aura fetches, caches or
stores cookies. TLS and certificates are in [`tls.md`](tls.md); the
Open Transport side, the entropy pool and the Mac's clock are in
[`mac-os-9-shell.md`](mac-os-9-shell.md); blocklists are in
[`content-blocking.md`](content-blocking.md); requests made by script
are in [`script-requests-and-cors.md`](script-requests-and-cors.md).

The network stack is portable engine code that never touches a socket
itself. `src/net` talks to the platform through `AuNetOps`
(`src/platform/platform.h`): DNS, non-blocking TCP, a monotonic and a
wall clock, and an entropy source. The host implements it with BSD
sockets (`src/platform/host/net_host.c`); Mac OS 9 with Open Transport
(`src/platform/macos9/net_ot.c`). One `AuNet` per application holds the
connection pool, the TLS sessions and the policy stores the embedder
attaches. The only process-wide state is the TLS library's
(`src/net/tls_platform.c`: its allocator hook, key store, random
generator and clocks).

The protocol code is sans I/O: response parsing, TLS, cookies, HSTS and
the cache are state machines over bytes and values, and only
`au_net_pump` talks to `AuNetOps`. All network work runs on the main
task ([`mac-os-9-shell.md`](mac-os-9-shell.md), "Everything runs on the
main task").

## To try a URL on the host

- `build/host/src/platform/host/aura-host fetch -v URL` prints the
  response headers, the TLS version, cipher and certificate chain, and
  timings.
- `aura-host render URL -w 800 -o out.png` loads a page and its
  resources over the network and renders it.
- Set `AURA_FAKE_TIME` (seconds since 1970) to test the clock check.
- `aura-host fetch` keeps `cookies` and `hsts` in `$AURA_PROFILE`
  (default `~/.aura-host`).

## To try the stack on Mac OS 9

Build with `./build.sh ppc` and push
`build/ppc/src/platform/macos9/nettest/NetTest.bin` with a copy of
`assets/net/aura-roots.bin` named `aura-roots` (`guest-push.sh` would
decode a `.bin` file as MacBinary). See
[`guest-testing.md`](guest-testing.md).

NetTest waits for the entropy pool, times the TLS library's primitives
(hashing, the two ciphers, key exchange on X25519 and P-256, ECDSA and
RSA verification), a DNS lookup and a TCP connection, then fetches a
URL list twice and prints status, TLS version, cipher, the handshake's
time and the other timings, and last the memory the TLS library holds
and the most it held; a `NetTest URLs` file beside it replaces the
list. It also writes `NetTest Log` in the Aura preferences folder.

## To fetch with the engine

Create the platform's ops, then `au_net_new(ops)`, attach a root store
(`au_roots_load` on `assets/net/aura-roots.bin`), and optionally a
cookie jar, HSTS store and cache (`au_net_set_cookie_jar`,
`au_net_set_hsts`, `au_net_set_cache`). Start requests with
`au_http_start` and call `au_net_pump` on every pass of the event loop;
callbacks run only from the pump. `au_net_apply_prefs` copies the
browser preferences in.

## To load pages and resources

Use the loader (`src/net/loader.h`): it adds mixed content blocking,
MIME sniffing with `nosniff`, encoding detection and conversion to
UTF-8, `data:` URLs, HTTPS-first, and the download hand-off
(`au_net_download_*`, shaped for `AuTransferSource`).

`src/net/page_fetcher.c` adapts it to the browser's `AuPageFetcher`:
`au_net_page_fetcher_interface` fills one, and the shell registers it
for `http` and `https` with `au_browser_set_fetcher`
([`browser-chrome.md`](browser-chrome.md)).

## To persist state

Serialize the stores with `au_cookie_jar_serialize`,
`au_hsts_serialize` and the cache's `AuCacheStorage`, and parse them
back at start. The Mac OS 9 application saves cookies and HSTS as
`Cookies` and `HSTS` 5 seconds after a change and at quit, and keeps
the disk cache in the `Cache` folder
([`mac-os-9-shell.md`](mac-os-9-shell.md)).

## Cookies and `document.cookie`

Script reads and sets cookies through `au_net_document_cookie_get` and
`_set` (`http.c`), which call the jar's `au_cookie_jar_document_get`
and `_set`. [`javascript.md`](javascript.md), "document.cookie", lists
the layers between the native and these calls.

- **The URL** is the document's own. A `srcdoc` or `about:blank` frame
  uses its parent's (it has its parent's origin).
- **Frames.** For a nested document the tab's URL is the site for
  cookies, so third-party blocking and SameSite apply: a cross-site
  frame reads and sets nothing while third-party cookies are blocked,
  and with blocking off it still cannot set a Lax cookie (the default)
  from a cross-site frame.
- **A sandboxed frame** without `allow-same-origin` reads `""` and sets
  nothing (the standard throws SecurityError).
- **A kept document** (hidden for Back and Forward) reads `""`.
- **Rules the jar applies**, as for response headers: `Secure` is
  refused on http, except on a loopback address, which is a trustworthy
  origin; an `HttpOnly` cookie is not readable, cannot be set by script
  and cannot be replaced by script; `SameSite=None` needs `Secure`;
  `Path`, `Max-Age` and `Expires` behave as in a response.
- **To try it**, serve a page from a local HTTP server and run
  `aura-host chrome --net --tab URL --do wait:300 --do
  js:document.cookie`.
- **Tests:** `tests/webapi/test_doc_cookie.c` (ctest
  `webapi_doc_cookie`): response and script cookies read back, `Path`,
  `Max-Age` and the clock, `Secure` on http, `HttpOnly`, SameSite,
  deletion, what a later request carries, a same-site frame, a
  cross-site frame with and without third-party blocking, a `srcdoc`
  frame, a sandboxed frame, and cookies turned off.

## The HTTP cache

`src/net/cache.h` describes what is stored and when it is fresh. The
cache is used through the page's cache mode (`AuFetchCacheMode`):

| Action | Cache use |
|---|---|
| a navigation | as HTTP says |
| Reload | revalidates the document |
| Shift- or Option-Reload | bypasses it |
| a history navigation (Back, Forward, the history pop-up, reopened or duplicated tabs, session restore) | `AU_FETCH_CACHE_HISTORY` for the document and its subresources |

- **The history mode** serves any stored response as it is, stale,
  no-cache or must-revalidate, without a request
  (`au_cache_lookup_history`, RFC 9111 section 6). Responses with
  neither freshness nor a validator are stored for this alone. no-store
  responses are never stored, so history fetches them again, and Vary
  (Cookie included) still selects the variant.
- **Encoded bodies.** A response that arrived gzip, deflate or brotli
  encoded is stored as it arrived, in memory and on disk, with its
  coding list in the entry (`au_cache_writer_set_coding`,
  `au_cache_entry_coding`), and `deliver_cached` in `http.c` decodes it
  each time it is served: an article's 46 entries take 0.9 MB instead
  of 4.4 MB. Other bodies (images) are stored as they are. The stored
  response fields carry neither Content-Encoding nor Content-Length, as
  before, so readers of a cached response see a decoded body either
  way. Revalidation (a 304 keeps the stored body and its coding), Vary
  and the history mode work on the entry and do not look at the body;
  requests with Range never use the cache.
- **The entry format.** An entry held decoded is serialized as `AURC1`,
  as before; one held encoded as `AURC2`, which adds the coding list
  and the decoded length before the body. `au_cache_entry_parse` reads
  both, so cache files written by earlier versions stay valid; an
  earlier version of Aura takes an `AURC2` file for damaged, removes it
  and fetches again. A stored body that fails to decode (a damaged
  file) fails that request with a decode error and the entry is
  removed, so the next request fetches it.
- **Entry size.** An entry may take a quarter of the memory cache (8 MB
  by default), counted as stored, so a large article's HTML fits.
- **Resident bytes.** The cache holds response bodies in memory up to
  its budget. With the disk cache on, the Mac application keeps 1 MB of
  them resident (`au_cache_set_resident`); the rest are read back from
  disk when asked for.
- **Memory held.** Each entry costs 256 bytes, its headers and its
  body, and the newest entry is always kept. A stored body is copied
  into a block of its size (`au_cache_commit`), so the cache's blocks
  match its count; on the host it plateaus at about 8 MB of `src/net`
  after ten pages ([`memory.md`](memory.md), "What grows from page to
  page"). Its blocks are in the long-lived zone.
- **To count hits on the host**, run `aura-host chrome --net
  --no-doc-cache --tab URL --do cache --do go:URL2 --do back --do
  cache` (without `--no-doc-cache`, Back shows the kept document and
  the HTTP cache is not asked; see
  [`browser-chrome.md`](browser-chrome.md)). `au_cache_stats` counts
  lookups, 304s, stores and disk reads.

### stale-while-revalidate

RFC 5861: inside the window `au_cache_lookup` returns
`AU_CACHE_STALE_USABLE`. For a subresource, `cache_try` in `http.c`
delivers the stale entry at once and starts a transfer of its own to
revalidate it (`revalidate_in_background`, one per entry at a time, no
callbacks); a document waits for the revalidation. must-revalidate,
no-cache and a request's no-cache or max-age turn it off.
`tests/net/test_swr.c` covers it.

### Disk cache writes are queued

With `au_disk_cache_set_deferred`, a store only keeps the file's bytes;
`au_disk_cache_idle` writes them oldest first and `au_disk_cache_flush`
(also `au_disk_cache_free`) writes the rest. A lookup for a queued key
is answered from the queue, and more than 2 MB of queued bytes are
written at once. The Mac OS 9 shell turns it on and writes one file per
event loop pass while no transfer is under way (`mac_net_idle`); a
write takes about 9 ms there.

## The DNS cache

The DNS cache (`src/net/dns.c`) sits between the HTTP client and
`AuNetOps.resolve_*`: an answer is used for 5 minutes and a failure for
10 seconds (the platforms give no TTL), at most 64 hosts, and lookups
of one host that overlap share one platform request. A refused or
failed connection forgets the host's answer. `au_net_clear_dns` clears
it (Clear Cache in Preferences does); `au_net_dns_stats` counts
lookups, and `aura-host chrome --do cache` prints them.
`tests/net/test_dns.c` covers it.

## HTTPS-first failures

`au_http_fallback_cause` says why the https attempt of an upgraded
navigation failed, and reaches the page as `AuFetchInfo.cause`.

- Refused, timed out, closed or plain-text connections give "This site
  does not support HTTPS".
- Certificate and TLS failures give "This site's secure connection
  failed" with the reason and certificate, Continue to the HTTP Site,
  and for certificate problems the session exception
  (`show_https_fallback` in `page_doc.c`).

## The request trace

Every transfer stamps the start of each phase (`au_http_trace` in
`net.h`) for the Network window
([`developer-tools.md`](developer-tools.md)).

## Tests

`tests/net` runs the stack against the fake transport
(`tests/net/fakenet.c`); [`tls.md`](tls.md) lists the TLS tests.
`tests/net/test_swr.c` and `tests/net/test_dns.c` are named above.

## Limits and not done

See [`known-gaps.md`](known-gaps.md#networking-and-tls).
