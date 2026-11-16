# Web storage

This document covers `localStorage` and `sessionStorage`: the storage
service in `src/net/web_storage.c`, which areas a document gets, how
they follow the cookie preferences, the limits, the files, how script
reaches the service, and what the Cookies window and System Status
show. It also says what the storage interfaces Aura does not have
would take. Read it before changing any of these, or when a site
forgets a choice it should keep. Cookies are in
[`networking.md`](networking.md); the Session file and the other files
Aura keeps are in [`browser-chrome.md`](browser-chrome.md).

## To try it

- Open `tests/page/samples/storage.html`: each load adds one to a
  counter kept in `localStorage` and to one kept in `sessionStorage`.
  Reload, open it in a second tab, quit and open Aura again.
- On the host, run `aura-host chrome --profile DIR --tab PAGE --do
  reload --do storage`: `--profile DIR` keeps the storage files in
  `DIR/local-storage` (so a second run finds them), `--do storage`
  prints the service's counts and its areas. `--net` keeps them in
  `$AURA_PROFILE` (default `~/.aura-host`), beside the cookies. Without
  either the service lives in memory for the run.
- `aura-host page PAGE.html` gives the page and its frames a service in
  memory; `aura-host js` has no service, and each script context keeps
  two areas of its own.
- To look at a site's areas in the application, open Window > Cookies
  (below); to see the counts, Develop > System Status, "Web storage".
- To start clean in the guest, delete the `Local Storage` folder in the
  Aura preferences folder.

## Which area a document gets

One service per application holds every area (`AuWebStorage`; the
browser makes one in memory, and a shell that keeps files replaces it
with `au_browser_set_storage` before the first window opens). An area
is named by a storage key, worked out by `au_web_storage_key` from the
document's origin and, for a nested document, the tab's URL:

| Document | Storage key |
|---|---|
| a page, or a frame of the same site as the tab's page | its origin: `https://example.com`, `http://a.test:8080` |
| a frame of another site than the tab's page (a third-party context, decided as for cookies: the registrable domains differ) | origin, `^`, the top-level site: `https://widget.example^https://news.example` |
| any `file:` document | `file://` (one area for all local files) |
| a sandboxed frame without `allow-same-origin`, a `data:` document, an internal page | none: `localStorage` and `sessionStorage` throw SecurityError |

- `http` and `https` of one host are different origins and so
  different areas.
- An `about:blank` or `srcdoc` frame has its parent's origin and area
  (not in a `file:` page: it has none there).
- **localStorage** is one area per storage key, shared by every tab.
- **sessionStorage** is one area per tab and storage key. The tab owns
  a namespace of the service (`AuTab.storage_ns`), not the document, so
  the area outlives navigations, reloads, Back and Forward, a kept
  document and a discarded page. A tab opened from another (a link
  opened in a new tab, `window.open`, `target=_blank`, a duplicated
  tab, Command-click on Back) starts with a copy of the opener's
  namespace (`au_bw_open_from`, `au_web_storage_ns_copy`); a tab opened
  from the menu or the address field starts empty. Reopen Closed Tab
  brings the tab's namespace back, and the Session file carries it (see
  "Files").

## Cookie preferences

The service reads the cookie jar's policy and per-site rules
(`au_web_storage_set_cookie_jar`) at each access, so a change applies
at once:

| Setting | Effect on storage |
|---|---|
| Cookies: none | SecurityError, except for a site with an "Always allow" rule |
| a site's rule "Block" | SecurityError; setting the rule in the Cookies window removes what the site stored |
| a site's rule "Session only" | `localStorage` works but is never written to disk, and the site's `sessionStorage` is left out of the Session file |
| Block third-party cookies (the default), or Cookies: same site | a third-party frame's `localStorage` is kept for the session only (it is partitioned by the top-level site whatever the setting) |
| Clear cookies at quit | every area is removed at quit, and `sessionStorage` is not written into the Session file |
| Clear Cookies in Preferences; Remove All in the Cookies window | removes every area, local and session |

An area that turns from persistent to session only keeps its contents
in memory and loses its file at the next save.

## Semantics

- Keys and values are strings of UTF-16 code units, and every sequence
  comes back as it went in, lone surrogates and NUL included. The
  service stores the script engine's UTF-8, in which a lone surrogate
  is three bytes (QuickJS-ng converts both ways without loss), with
  explicit lengths.
- Keys keep their insertion order; a changed value keeps its place.
- `length`, `key(n)`, `getItem`, `setItem`, `removeItem`, `clear`, and
  named properties through a Proxy (`storageHandler` in
  `src/js/prelude_window.js`): a string key that is not a member of the
  interface reads the item, any string key written, defined or deleted
  acts on the items (`localStorage.length = 7` stores an item named
  `length`), and `in`, `Object.keys`, `for...in` and `JSON.stringify`
  see the items.
- **The storage event** (`key`, `oldValue`, `newValue`, `url`,
  `storageArea`) is queued as a task in every other document that has
  the changed area: other tabs and frames, never the writer, and not
  documents kept for Back. `clear()` on an area that held something
  fires one event with a null key; a write that changes nothing fires
  none. A change to `sessionStorage` reaches the same-key frames of the
  same tab.
- Workers have no `localStorage` or `sessionStorage`, as the standard
  says.
- `navigator.storage.estimate()` resolves with the usage and quota of
  the document's local area; `persisted()` and `persist()` resolve true
  where storage is allowed, since Aura never drops a persistent area.
- `document.referrer` is the Referer value the navigation carried (the
  default policy: the full URL within an origin, the origin across
  origins, nothing from https to http). A typed address, a bookmark and
  a frame's document give `""`.

## Limits

| Limit | Value | When it is reached |
|---|---|---|
| per area | 5 MB, counted as two bytes per UTF-16 code unit of keys and values (`AU_WEB_STORAGE_AREA_BYTES`) | `setItem` throws QuotaExceededError |
| over all areas, on disk and in memory | 50 MB by the same count (`AU_WEB_STORAGE_TOTAL_BYTES`) | the least recently used session-only areas are dropped to make room; if that is not enough, QuotaExceededError. A persistent area is never dropped |
| area data in memory | 16 MB; 6 MB in the Mac OS 9 application (`au_web_storage_set_limits`) | persistent areas that are saved are unloaded, least recently used first, and read back when next used; changed ones are written first; then session-only areas are dropped; if only tabs' session areas are left, QuotaExceededError. One area may always be in memory |
| areas | 4096 | a session-only area is dropped, else QuotaExceededError |
| an area file read | 12 MB | larger is damaged |
| `sessionStorage` in the Session file | 256 KB per tab (`AU_WEB_STORAGE_SESSION_SAVE_BYTES`); 64 storage lines read per tab | an area that does not fit is left out of the file (it stays in memory) |

The low-memory release handler of the Mac OS 9 application also unloads
the saved areas (`au_web_storage_release`).

## Files

The Mac OS 9 application keeps a `Local Storage` folder in the Aura
preferences folder (`network.c`, "Web storage's files"):

| File | Holds |
|---|---|
| `Index` | text: `# Aura local storage 1`, then one line per area, `file`, `bytes`, `items`, `storage key`, separated by tabs |
| 8 hex digits (`df2654e9`) | one persistent area: the name is a hash of the storage key (the next free number when two keys collide), since an HFS name holds 31 characters |

An area file is binary, integers big-endian: `AuLS`, a version (1),
the storage key with its length, the item count, each item as length
and bytes of the key then of the value, and an FNV-1a sum of all
before it. It needs no escaping.

- **Reading.** The index is read at start; an area's file is read when
  script first uses the area, and reads from script never touch the
  disk after that (unless the area was unloaded for memory).
- **Writing.** Script's writes change memory and mark the area. The
  shell writes marked areas, one file per event-loop pass while nothing
  loads, 5 seconds after the last change (30 seconds after the first,
  for a page that never stops writing), and all of them at quit
  (`mac_net_save`, on `au_web_storage_pending` and
  `au_web_storage_change_gen`). Each file is written as `NAME.tmp` and
  renamed over the old one.
- **Damage.** Every length is checked against what is left of the
  file, and the file must name the area's key, end where its items end
  and carry the right sum. A file that fails is renamed `NAME
  (damaged)` and the area starts empty; a damaged index keeps the lines
  before the damage. `au_web_storage_parse_file` is the reader.
- **The Session file** gets, under each tab, one line per origin of
  the tab's `sessionStorage`: `storage`, the storage key, then keys and
  values, separated by tabs, with `%`, tab, CR, LF and NUL written as
  `%25`, `%09`, `%0D`, `%0A`, `%00` (`au_web_storage_ns_serialize`).
  Restore Session reads them back before the tab's first document
  runs. A change to `sessionStorage` puts the file out of date as a
  scroll position does (saved within 30 seconds).

On the host the same files are in `local-storage` of the profile
folder, and a damaged file is renamed `NAME.damaged`.

## How script reaches the service

A storage call is one `AuJsStorageCall` (`src/js/storage_call.h`: the
kind, the operation, key, value, and the caller) passed down four
layers, as `document.cookie` is:

| Layer | Does |
|---|---|
| `src/js/natives2.c` (`N.storage*`) | fills the document's origin and URL; refuses an opaque origin; without the hook keeps two maps of its own (`AuJs.own_storage`) |
| `AuJsHooks.storage`, `h_storage` in `src/page/script.c` | adds the writing view and, for a nested document, the top-level URL |
| `AuDocViewCallbacks.storage` (`fc_storage` in `frames.c` for frames) | reaches the top-level view's embedder |
| `dv_storage` in `src/browser/page_doc.c` | calls `au_web_storage_call` with the tab's namespace |

The prelude's `Storage` objects hold only their kind; nothing is cached
in script, so a write is visible at once to every document.

A change travels back through the service's listener: `storage_changed`
in `browser.c` passes it to every tab's page (`au_page_storage_event`),
the document view to its frames (`au_docview_storage_event`), and each
document's `au_js_storage_event` asks the service whether the changed
area is its own (`AU_JS_STORAGE_MATCH`) before it queues the event.

To give another embedder storage, set `AuDocViewCallbacks.storage` to a
function that calls `au_web_storage_call`, and pass changes to
`au_docview_storage_event` (`pagerun.c` is the short example).

## The Cookies window and System Status

- **Window > Cookies** lists a site that holds storage even when it
  has no cookie. The site's row adds the size (`2 cookies, 12 KB of
  storage`). An expanded site shows, after its cookies, one row per
  storage area: `Local storage`, `Local storage (until Aura quits)` or
  `Session storage (a tab)`, with the storage key, the size and the
  item count. Details on such a row names the origin, the embedding
  site of a partition, the size and where it is kept; it never shows
  contents. Remove on an area's row removes that area; Remove on a site
  removes its cookies and its storage; Remove All removes everything.
  The model is `src/net/cookie_view.c` (`AuCookieView.storage`); no
  column was added, because the list view has one detail column.
- **Develop > System Status** has four lines under Network: `Web
  storage` (origins, KB, writes pending), `Storage areas` (session
  only, of tabs, KB in memory), `Storage files` (read, written,
  damaged) and `Largest write` (bytes and milliseconds). The bug report
  takes these counts with the rest of System Status and nothing else
  about storage: no keys, no values, no origins.

## Measurements

2026-11-15. Host: Apple silicon, RelWithDebInfo. Guest: QEMU mac99,
Enforcer base, `run/stime.html` (a loop of `setItem` on 100 keys).

| What | Host | Guest |
|---|---|---|
| 1000 `setItem` calls | 1 ms | 13 ms (669 ms for 50,000) |
| 1000 `getItem` calls | 1 ms | 10 ms (501 ms for 50,000) |
| saving a 1 MB area (1,000,113 bytes) | | 56 ms |
| `setItem` of a 1,000,000 character value | | 11 ms |

## Tests

- `tests/webapi/test_web_storage.c` (ctest `webapi_web_storage`): the
  map; persistence across loads and a restart; origins and schemes; two
  tabs and the storage event; the tab's session storage (navigation,
  Back to a kept document, reload, opened, duplicated, closed and
  reopened tabs, the Session file and its cap); third-party frames,
  sandbox, `data:`; the cookie preferences and rules; the Cookies
  window's model; quotas and dropped session-only areas; strings and
  named properties; files (every truncation, every flipped byte, lying
  lengths, a damaged index, unloading); `document.referrer`.
- `tests/browser/fuzz_files.c`, kind `storage` (ctest
  `browser_files_fuzz`): mutated area files (half with the sum set
  right, so the items are reached), indexes and session lines through
  the service. For a long run: `build/asan/tests/browser/fuzz_files
  100000000 SEED 600`; to replay, `fuzz_files replay storage FILE`.
- `tests/js/window.js` covers the areas a script context keeps without
  a service.

## Not built, and what each would take

| Interface | Do sites need it | What it would take |
|---|---|---|
| IndexedDB | Many applications test for it and fall back to `localStorage` or memory; some (offline mail and document editors, some single-page applications' caches, Firebase persistence) require it. Aura has no `indexedDB`, which those that test take as "absent" | Object stores with keys ordered as the standard orders them, indexes, cursors, transactions tied to the event loop, version upgrades, and structured clone to bytes for values. The storage key, partitioning, policy and quota code here can be reused; the storage itself cannot (records must be found by key range without loading a database into memory). A separate piece of work the size of this service several times over |
| Cache API (`caches`) and service workers | Only for offline use and push; pages work without them, and feature tests see them absent | A service worker is a worker with its own life cycle that answers the page's fetches: registration, install and activate events, the fetch event in the loader's path, and the Cache API as a store of request and response pairs on disk. Not planned for version 1 |
| `navigator.storage.getDirectory` (the origin private file system) | Rare | A per-origin folder and file handles; not planned |
| `document.referrer` in frames | Some embedded widgets read it to learn the embedding page | Frames have the requesting document's URL already (`AuFrame`, the request's `document_url`): compute the Referer value at the frame's load and pass it with `au_docview_set_referrer` |
| the storage event in a document kept for Back | No: browsers do not deliver it there either | nothing |
| `noopener` for the session storage copy | Rarely matters | `window.open` with `noopener` and links with `rel=noopener` should not copy the opener's `sessionStorage`; the open path would need to carry the flag to `au_bw_open_from` |
| `sessionStorage` of closed tabs across a quit | No | The closed-tab list is not in the Session file at all |

## Not done

- A `srcdoc` or `about:blank` frame in a `file:` page has no storage
  (its inherited origin is opaque).
- The total limit counts what the index says for areas not loaded; an
  index edited by hand can understate it until the areas are read.
- Files in the `Local Storage` folder that the index does not name are
  left alone (they are not removed).
