# Content blocking

This document covers blocking by domain: the compiled list format and
matching, the parser, where requests are checked, the catalogue and its
updates, the preferences and the Mac OS 9 storage. Read it before
changing `src/net/blocklist.c`, `src/net/blocklist_store.c` or the
Blocking preferences.

Aura blocks by domain: a request to a host on a blocklist is never made
(no DNS lookup, no connection). Aura carries no list. It downloads the
lists the user turns on from their publishers and keeps a compiled copy
(HaGeZi's lists are GPL-3.0; do not add a list to the repository or the
application).

## To try it on the host

| Command | Does |
|---|---|
| `aura-host blocklist compile LIST.txt OUT` | compiles a list; prints entries, time and peak memory |
| `aura-host blocklist check FILE HOST...` | checks hosts against a list |
| `aura-host blocklist bench LIST.txt` | lookups per second |
| `aura-host blocklist update DIR` | runs the manager against a folder, over the network |
| `aura-host chrome --net --blocklist FILE`, `aura-host render URL --blocklist FILE` | check requests against a compiled or text list and print the counts |

## Matching and the file format

To change matching or the file format, work in `src/net/blocklist.c`
(`blocklist.h` describes both).

- A compiled list is a sorted array of 48-bit hashes of lower-case
  domains, searched by bisection.
- A host is listed when it or a parent domain down to its registrable
  domain is in a list (never a bare public suffix), unless an exception
  covers it.
- Order: the personal allow entries, the personal block entries, any
  list's exceptions, the lists.
- Verdicts are cached per host (512 slots).
- With n entries a probe of an unlisted name hits by chance with
  probability n / 2^48 (3.2e-10 for Light), and a host takes two or
  three probes; the personal allow entries are kept as text and clear a
  false hit.

## The parser

The parser (`AuBlockBuilder`) takes plain domains, hosts files, adblock
domain rules (`||domain^`, `@@||domain^`), `*.domain` and dnsmasq rules
in any mix, streaming, with lines capped at 512 bytes and names
validated; non-ASCII names go through `au_url_host_parse`. It sorts in
chunks of 16384 entries and merges in slices
(`au_block_builder_finish` with a budget), so a shell compiles at idle
time. Every allocation may fail; the builder then fails and the old
list stays.

## Where it applies

`au_net_loader_start` (`loader.c`) asks `au_blocklist_check` before
anything else, and again for each redirect (`t_redirect`).

- A request with a top-level URL (subresources, fetch, XMLHttpRequest,
  frames) is blocked when its host is listed, unless it is first-party
  (the page's registrable domain) or the page's site is switched off.
- A document request without one is a navigation: it ends with
  `AU_NET_ERR_BLOCKED` and `page_doc.c` shows the blocked-site page
  (`show_net_error`), whose Visit Anyway calls
  `AuPageFetcher.allow_blocked` (the site is then allowed as a
  navigation until Aura quits) and whose Go Back calls
  `AuPageCallbacks.go_back`.
- A blocked request reports `AuLoadInfo.blocked_reason` and
  `AuFetchInfo.blocked_reason` ("blocked: LIST NAME"); the page counts
  them in `AuPageStats.blocked` and the status bar shows a shield and
  the count (`AuChromeState.blocked`).
- The Network window lists a page's blocked requests with the list that
  stopped each ([`developer-tools.md`](developer-tools.md)).

## Lists and updates

Lists and updates are `src/net/blocklist_store.c` (`AuBlockManager`):

- the catalogue (HaGeZi Light, Normal, Pro, Pro++ and the Threat
  Intelligence Feeds in three sizes, in the wildcard domains format, at
  the jsDelivr URLs the project's README gives);
- the lists added by https URL;
- conditional requests (ETag, Last-Modified) at most once per list per
  interval, started only from `au_block_manager_idle` while nothing
  loads, outside the HTTP cache (`AuHttpRequest.no_cache`) and without
  cookies.

A download that fails, leaves https, holds no domains or under a
quarter of the old (or expected) count keeps the old list and is tried
again after 6 hours (10 minutes when there is no old list). The
platform supplies an `AuBlockStore` (files) and an `AuBlockMem` (large
blocks).

## Preferences

| Preference | Holds |
|---|---|
| `privacy.block_trackers` | the switch, on |
| `blocking.lists` | catalogue ids, "light" |
| `blocking.custom` | https URLs |
| `blocking.allow`, `blocking.block` | the personal entries |
| `blocking.sites_off` | registrable domains, set by Turn Off Blocking for This Site in the page's contextual menu (`au_bw_toggle_site_blocking` in `context.c`) |
| `blocking.update` | daily, weekly, never |

## Mac OS 9

`blocking_mac.c` keeps the lists in temporary memory (`TempNewHandle`,
else the heap through `au_try_alloc`; a list that fits in neither is
not loaded and the Preferences pane says so) and the compiled files in
the `Blocklists` folder of the Aura preferences folder (one file per
list, named by its id, and `Index`). Downloads start 4 seconds after
launch at the earliest.

Preferences > Blocking (`prefs_blocking.c`; the switch and the three
fields are in `prefswin.c`) lists the catalogue with each list's size
and date or what the manager is doing; its checkboxes and Update Now
act at once. Develop > Network Status shows the totals, where the list
memory came from and what the last compile cost.

## Tests

`tests/blocklist`:

- `test_blocklist`: formats and junk, matching, the public suffix
  guard, exceptions, collisions, damaged files, allocation failures,
  the request decision.
- `test_blocklist_net`: the loader, a browser window and the update
  manager on the fake transport.
- `fuzz_blocklist ITERATIONS SEED [SECONDS]`: the parser, compiled
  files and the store's index; it prints the iterations done.

## Measured

Host, lists of 2026-11-15:

| List | Compiled size | Entries | Compile time |
|---|---|---|---|
| Light (1.96 MB of text) | 549 KB | 91,414 | 24 ms |
| Normal | 1.34 MB | 223,018 | |
| Pro | 1.42 MB | 237,247 | |
| Pro++ | 1.56 MB | 259,753 | 63 ms |
| the full threat feeds | 8.7 MB | 1,455,489 | 387 ms |

Compiling peaks at twice the compiled size. About 600,000 uncached
lookups per second.

| Site, with scripts | Without blocking | With Light |
|---|---|---|
| arstechnica.com | 77 requests, 9.8 MB | 61 requests, 4.4 MB |
| theguardian.com | 118 requests | the same 118 |

Guest (stock base): the first launch fetched Light 10 seconds after the
download began (653 KB on the wire; the time includes the wait for the
entropy pool), merged it in 30 ms over 5 idle calls and held it in
535 KB of temporary memory; Normal took 0.8 s to fetch and parse and
73 ms to merge. Both lists use 1.8 MB.

## Not done

See [`known-gaps.md`](known-gaps.md#content-blocking).
