# JavaScript

This document covers `src/js`: the QuickJS-ng runtime wrapper, how
script runs in pages, the prelude and its bytecode, limits, `Date` and
the clock, and the bindings that have rules of their own (Range,
Selection, CSSOM, getComputedStyle, crypto, postMessage). Read it
before changing a binding or the runtime, or when a site's script
fails. Requests made by script are in
[`script-requests-and-cors.md`](script-requests-and-cors.md), workers
in [`workers.md`](workers.md), windows of nested documents in
[`frames.md`](frames.md), the canvas in [`canvas.md`](canvas.md) and
form controls in [`forms.md`](forms.md).

The engine is QuickJS-ng (`third_party/quickjs`), reached only through
`src/js`; no other folder sees a QuickJS type. `src/js/js.h` is the
interface: one `AuJs` per document, holding a context whose global
object is the window, the DOM bindings and the event loop. The
documents of one top-level page share one runtime (see
[`frames.md`](frames.md)).

## To run a script on the host

- `build/host/src/platform/host/aura-host js FILE.js...` runs scripts
  (`--html PAGE.html` loads a document first, `--stats` prints start-up
  time and memory).
- `aura-host page PAGE.html` loads a scripted page through the document
  view (`--click ID` clicks an element, `--chunk N` feeds the parser N
  bytes at a time, `--no-script` turns script off).
- The `tests/js` scripts and pages run under ctest.

## To debug a script on a live page

Use `aura-host chrome` actions:

| Action | Does |
|---|---|
| `--do js:CODE` | evaluates CODE in the active tab and prints the result |
| `--do save:FILE` | writes the page source |
| `--do console` | prints the console |
| `--do js:aura:memory`, `--do js:aura:gc` | script heap accounting, the collector ([`memory.md`](memory.md)) |

- Uncaught errors, errors in event listeners and unhandled promise
  rejections go to the console at error level; one that the page's
  `error` or `unhandledrejection` listener cancels (Google's
  `window.onerror` returns true) goes there at debug level, marked
  "handled by the page".
- To work on a site's scripts offline, save the page and the scripts it
  loads and serve them from a local HTTP server (the origin must be
  http for origin checks to behave as on the site).
- In the application, the Console window shows the same lines and takes
  expressions ([`developer-tools.md`](developer-tools.md)).

## To run script in a document from C

Create an `AuJs` with `au_js_new`, run scripts with `au_js_run_classic`
or the module calls, and call `au_js_run_tasks` and `au_js_run_frame`
from the idle loop. Free it with `au_js_free` before the document goes,
never from inside a hook.

## To change QuickJS-ng

Add or edit a patch in `third_party/quickjs-patches` and rebuild the
tree with `apply.sh` (see `third_party/README.md`); never edit
`third_party/quickjs` alone.

Patches this document refers to:

| Patch | Does |
|---|---|
| 0004 | pads every `alloca` |
| 0005 | makes the byte-swap flag work in the bytecode writer |
| 0006 | a function and the functions nested in it share one copy of their source text |
| 0007 | `Date` reads the embedder's clock |

## To test on Mac OS 9

Build with `./build.sh ppc` and push
`build/ppc/src/platform/macos9/jstest/JSTest.bin` with
`tools/guest-push.sh -n AuraTest`, then `tools/guest-qmp.py open
AuraTest JSTest` ([`guest-testing.md`](guest-testing.md)).

JSTest prints its start-up time and memory, an "ok" or "FAIL" line per
self-test, the benchmark times of `tests/js/bench/bench.js` at
`BENCH_SCALE` 0.1, and the heap left. To compare with the host, run
`aura-host js --stats` on a file that sets `globalThis.BENCH_SCALE =
0.1` followed by `tests/js/bench/bench.js`, with a Release build.

**To run JSTest on a real G4**, copy `JSTest.bin` to the machine (a CD
made from the image `guest-push.sh` writes, or a file share), decode it
with StuffIt Expander if the Finder shows a MacBinary file, and open
it. Its SIZE resource asks for 24 MB (20 MB at least); raise it in Get
Info if the self-test reports out-of-memory errors. Quit other
applications first so the timings are not disturbed. QEMU timings do
not predict a G4's: the guest's speed depends on the host's TCG
translation, so report G4 and guest numbers separately.

## Script in pages

Script in pages is driven by `src/page/script.c`: parser-blocking
scripts pause the parser, `document.write` feeds it, defer, async,
module and nomodule scripts run in their order, and the document view
fires DOMContentLoaded and load.

- The runtime starts only when a page has a script or an inline event
  handler.
- Scripting is off in a bare document view; `au_docview_set_scripting`
  turns it on from the next load.
- In the browser, `page_doc.c` sets it from the `content.javascript`
  preference (on by default) at each load, runs
  `au_docview_run_scripts` for up to 20 ms in every `au_page_idle` and
  follows the navigations it reports through `open_url`, and keeps the
  last 300 console lines (`au_page_console_line`) for the Console
  window.

**Response heads.** A loader that knows a response's status and headers
passes them with `au_docview_resource_head` before the data; script
then sees them in `fetch` and `XMLHttpRequest`, and an error status
fails a script element's load. Without it script sees status 200, no
headers and a type guessed from the URL; `page_doc.c` passes the final
URL and content type, and status 200 until `AuFetchInfo` has a status.

**`document.open()`** on a loaded document followed by `write` replaces
the document's contents with the parse of what was written (the same
Document and window; scripts in the written text do not run; a `write`
without `open` after load is still ignored).
`DOMImplementation.createDocument` and `createDocumentType` make plain
documents and doctypes.

## Ownership and limits

**Ownership.** A node's wrapper holds one reference on the node; the
node holds its wrapper while the node is in a tree or otherwise
referenced; a node no script has touched has no wrapper. `js_int.h` has
the details.

**Limits.** Each `AuJs` has a memory limit enforced by its allocator (a
refused allocation is an uncatchable out-of-memory error, with a 64 KB
reserve so the error can be reported), a C stack limit (deep recursion
is a RangeError) and a time budget (`AuJsHooks.long_script` decides
whether a long script goes on). Allocation failures never reach
`au_alloc`'s fatal handler. A page's runtime may allocate 32 MB.
On Mac OS 9 script may use 512 KB of the application's 1 MB stack
([`mac-os-9-shell.md`](mac-os-9-shell.md)). A native that can take long
must check the time limit itself ([`fuzzers.md`](fuzzers.md), "Script
that does not end").

**Script memory.** See [`memory.md`](memory.md) for what a page's
script heap holds and how to measure it. A function and the functions
nested in it share one copy of their source text (patch 0006).
`au_js_run_tasks` runs the collector when script has been idle for a
second and the heap has grown by an eighth (at least 512 KB) since the
last collection; a hidden document is collected when it is hidden.

## Retro68 compiler points

QuickJS is built with `-fno-strict-aliasing` (with `uint32_t` as
`unsigned long`, string iterators never advanced), and patch 0004 pads
every `alloca`, because Retro68's GCC places spilled locals in the top
8 bytes of alloca space. Do not use `alloca` or variable-length arrays
in Aura's own code.

## The prelude

The prelude is one function expression split over
`src/js/prelude*.js`, joined in order (base, dom, html, range, cssom,
window, web, net, canvas, workers, obs). The build compiles it to
QuickJS bytecode and the runtime loads that.

- To add a file, add it to the list in `third_party/quickjs.cmake` and
  its `#embed` line in `prelude.c` (the source build).
- A name declared twice across the files stops the runtime from
  starting ("invalid redefinition").
- A worker has a second, smaller prelude built from `prelude.js`,
  `prelude_web.js`, `prelude_net.js` and `prelude_worker.js`
  (`prelude_worker.bc`): code in the first three must run without a
  document (`doc` is null there, and only the natives of
  `js_natives_shared_init`, the loop, fetch, module and worker natives
  exist), so put anything that touches the DOM in another file.

### Prelude bytecode

`tools/jsprelude/jsprelude.c` compiles the prelude with
`JS_EVAL_FLAG_COMPILE_ONLY` and writes it with `JS_WriteObject`,
without source text or line tables; `third_party/quickjs.cmake` runs it
at build time into `build/*/gen/prelude.bc`, `prelude.c` embeds the
file, and `load_prelude` in `runtime.c` loads it with `JS_ReadObject`
and `JS_EvalFunction`.

- **Byte order.** QuickJS-ng bytecode is in the byte order of the
  machine that reads it, and v0.18.0 swaps nowhere (its
  `JS_WRITE_OBJ_BSWAP` was defined as 0). Patch 0005 makes the flag
  work in the writer: the Mac OS 9 build compiles the tool with the
  host's C compiler (`AURA_HOST_CC`, found as `cc`) and runs it with
  `--swap`, so no PowerPC code runs during the build.
- **Same sources.** The tool must be built from the same QuickJS
  sources as the engine (the bytecode names built-in atoms by number);
  the build does that.
- **From source.** To load the prelude from source instead (errors in
  it then carry line numbers), configure with
  `-DAURA_JS_PRELUDE_SOURCE=ON`.
- Prelude functions print as `function name() { [native code] }` from
  bytecode.
- After changing QuickJS or the tool, run JSTest in the guest: its
  "prelude regexp" and "prelude forms" self-tests use regular
  expression literals, numbers and strings from the swapped bytecode.

### Start-up cost

`aura-host js --stats` prints the start-up time, the heap, and what the
heap holds by kind.

| Where | From source | From bytecode |
|---|---|---|
| Host (RelWithDebInfo) | 30 ms, 1.69 MB | 5 ms, 1.28 MB |
| Stock guest (JSTest) | 1148 ms, 1430 KB | 747 ms, 1002 KB |

The 226 KB of bytecode sit in the application's data instead of 299 KB
of source. (A later measurement, in [`workers.md`](workers.md), has the
page's prelude at 258 KB of bytecode.)

## Date and the clock

Script's `Date` reads the embedder's clock when one is installed with
`au_js_set_clock` (`src/js/clock.h`, process-wide; QuickJS patch 0007):
UTC milliseconds for `Date.now()`, `new Date()` and
`performance.timeOrigin`, and the zone offset for the local-time
methods (`getHours`, `toString`, `getTimezoneOffset`,
`toLocaleString`).

- The Mac OS 9 application installs the network stack's clock
  (`au_mac_wall_ms`: the system clock's seconds with milliseconds from
  the time since startup) and the Date & Time control panel's offset
  (`mac_net_init` in `network.c`).
- Mac OS 9 has no zone database, so every date, past or future, gets
  the offset set now, daylight saving hour included; the offset is read
  again at most once a minute.
- Without a clock (the host tools, JSTest) `Date` reads the C library,
  which on Mac OS 9 knows neither the Mac's clock nor its zone.
- `performance.now()` and timers use the monotonic `AuJsHooks.now_ms`.
- To test with a fixed time and zone, run `aura-host js --clock MS
  --zone SECONDS` (UTC milliseconds since 1970; seconds east of UTC),
  as `tests/js/clock.js` does (ctest `js_clock`).

[`mac-os-9-shell.md`](mac-os-9-shell.md) describes the Mac's clock.

## window.postMessage

`window.postMessage` takes any absolute URL as the target origin and
compares only its origin (between frames: see
[`frames.md`](frames.md)). Google's search box defers its submit with
`postMessage("sbox.df", location.href)`; comparing the string with the
origin dropped the message and Return did nothing
(`tests/js/pages/search_defer.html`, `test_search_box_defer` in
`tests/forms`).

## Range

`prelude_range.js`, `ranges.c`. Boundary points live in C so that the
DOM mutation hooks can move them without running script:
`js_ranges_inserted` and `js_ranges_removed` follow the DOM standard's
insert and remove steps.

- Offsets in text are UTF-16 code units, which only script knows, so
  `replaceData` and `splitText` in `prelude_dom.js` report their edits
  with `N.rangeText` and `N.rangeSplit`; the `data`, `nodeValue` and
  `textContent` setters and the parser are handled in `js_ranges_text`
  (data only appended moves nothing, anything else puts the node's
  points at offset 0).
- To change a text mutation path in the prelude, keep it going through
  `replaceData`.
- `getClientRects` and `getBoundingClientRect` give the rectangles of
  the selected part of each text node, one per line, through
  `N.textRects` and the `text_rects` hook (`h_text_rects` in
  `script.c`, on layout's `au_layout_text_pos_from_dom` and
  `au_layout_range_rects`); a collapsed range in text gives one
  rectangle of no width.
- Not exact: `normalize()` moves points in merged text nodes to their
  parent.

## Selection

Selection is script state with one range and a direction (`extend`,
`setBaseAndExtent`, `selectionchange`). It is not the selection the
user makes with the mouse (`page_doc.c`, see
[`browser-chrome.md`](browser-chrome.md)), and it is not painted. To
join them, convert each boundary point with
`au_layout_text_pos_from_dom` and `au_layout_text_pos_to_dom`
(`src/layout/selection.h`); that is not wired yet.

## CSSOM

`prelude_cssom.js`. `element.sheet` and `document.styleSheets` give a
`CSSStyleSheet` per style or link element.

- Rules are read from the sheet's text (`AuJsHooks.sheet_text`,
  `au_dv_sheet_text` in `docview.c`) by a splitter in script.
- `insertRule`, `deleteRule` and changes to a rule write the whole text
  back (`au_dv_sheet_set_text`, kept in `SheetSrc.om_text`), and the
  document view parses it again, so the element's own text does not
  change.
- `sheet.disabled` sets `SheetSrc.disabled`.
- A rule's declarations are the `style` attribute of an element that is
  never inserted, so they share the inline style code.
- The text script writes back is parsed into `SheetSrc.om_sheet`, which
  replaces the sheet's own in the cascade; the resolver borrows sheets,
  so `au_dv_sheet_set_text` detaches them before it frees the old one.
- A link element's sheet is listed and can be disabled, but its rules
  throw NotSupportedError: the document view frees a fetched sheet's
  text once it is parsed (`parse_sheet`), so there is nothing to read
  them from.
- A cross-origin sheet's rules throw SecurityError.

## getComputedStyle

`getComputedStyle` returns the used width and height in pixels
(`computedValue` in `prelude_dom.js`, through `N.rect`, which forces a
layout), takes `::before`, `::after` and `::marker`, and reads
`white-space` as one keyword. Margins, insets and percentages in other
properties are still computed values, `transform` is not a matrix, and
the `border` and `font` shorthands serialize every part.

## crypto

`crypto` (`getRandomValues`, `randomUUID`, `subtle`) exists only when
the embedder passes `AuJsHooks.random` (`AuDocViewCallbacks.random` for
a document view).

- In the browser the shell passes `AuBrowserHost.random`, which reaches
  the page through `AuPageEnv.random` and `dv_random` in `page_doc.c`.
- On Mac OS 9 it is `mac_net_random`, the entropy pool the network
  stack uses; until the pool has gathered 128 bits it returns false and
  the calls throw OperationError (they never get weaker bytes).
- `aura-host chrome` and `aura-host js` read `/dev/urandom`; `aura-host
  chrome --no-entropy` answers as an unready pool.
- `tests/browser/test_crypto.c` covers a ready source, an unready one
  and a host without one (no `crypto` object).
- `crypto.subtle` has `digest` (SHA-1, SHA-256, SHA-384, SHA-512) and
  HMAC with raw keys (`importKey`, `generateKey`, `exportKey`, `sign`,
  `verify`); every other algorithm and operation rejects with
  NotSupportedError (AES, RSA, ECDSA, PBKDF2 and HKDF are what pages
  ask for next).
- The digests are `src/net/digest.c`: SHA-256 is the engine's own, the
  others come from the TLS library's PSA hash interface, which `src/js`
  reaches only through that file.

## Tests

`tests/js` holds binding, loop, limit, lifetime, security and worker
scripts for `aura-host js`, scripted pages for `aura-host page`
(`tests/js/pages`) and the benchmark (`tests/js/bench/bench.js`).

## Not done

See [`known-gaps.md`](known-gaps.md#javascript).
