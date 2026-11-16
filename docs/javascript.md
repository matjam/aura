# JavaScript

This document covers `src/js`: the QuickJS-ng runtime wrapper, how
script runs in pages, the prelude and its bytecode, limits, `Date` and
the clock, `Intl`, and the bindings that have rules of their own
(`document.cookie`, postMessage, Range, Selection, CSSOM,
getComputedStyle, crypto). Read it
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
| `--do perf` | the Performance report, with each runtime's, document's and worker's script memory, peak and limit ([`memory.md`](memory.md)) |
| `--script-mem MB`, `--worker-mem MB` (options, not actions) | set the script memory limits ("Script memory limits" below) |

- Uncaught errors, errors in event listeners and unhandled promise
  rejections go to the console at error level; one that the page's
  `error` or `unhandledrejection` listener cancels (Google's
  `window.onerror` returns true) goes there at debug level, marked
  "handled by the page".
- To work on a site's scripts offline, save the page and the scripts it
  loads and serve them from a local HTTP server (the origin must be
  http for origin checks to behave as on the site).
- A script that is refused memory stops with a console line that names
  the limit and the preference to change ("What an out-of-memory line
  says" below).
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
| 0008 | one source text per script (`0008-one-source-text-per-script`): the parser copies no function text, and the contexts of a runtime share a script's text |
| 0009 | bytecode that names positions in the script's text (`0009-bytecode-source-positions`: `JS_WRITE_OBJ_SOURCE_REF`, `JS_ReadObjectSource`), and a parser that trims each function's buffers (`js_fd_trim`) |
| 0010 | a module graph is resolved again without compiling its modules again (`0010-module-retry-without-recompiling`; "Module scripts" below) |
| 0011 | the parser records what its bracket look-ahead found instead of repeating it for nested brackets (`0011-parse-lookahead-memo`); the bytecode is unchanged |
| 0012 | the parser finds a scoped variable through the variable hash table instead of walking the scope chain (`0012-scope-lookup-hash`); the bytecode is unchanged |

`third_party/README.md` lists every patch. After changing any of them,
run JSTest in the guest.

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

## Module scripts

`src/js/module.c` and `runModuleScript` in `prelude_web.js` load module
graphs. QuickJS's loader is synchronous, so loading goes in rounds:
`N.moduleEval` compiles the module and resolves its imports; when
imports are missing it returns their URLs, the prelude fetches them all
at once (`fetchModules`) and calls again with the module's name.

- Each module is compiled once (patch 0010). A compiled module stays
  loaded in its document's context whether or not its imports resolved,
  and the next round resolves it again by name
  (`JS_FindLoadedModule`). One pass reports every missing import, so a
  graph takes one round per level of imports.
- A module's text is freed when it compiles; the engine keeps its own
  copy (patch 0008).
- A module script whose URL is already loaded is that module: it runs
  once, as the standard's module map says.
- To change the protocol, keep `tests/js/modules.js` passing: it covers
  shared imports, two script elements of one URL, `import.meta.url` of
  modules loaded in later rounds, inline modules and syntax errors.

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
`au_alloc`'s fatal handler. The memory limits are set by the policy in
"Script memory limits" below.
On Mac OS 9 script may use 512 KB of the application's 1 MB stack
([`mac-os-9-shell.md`](mac-os-9-shell.md)). A native that can take long
must check the time limit itself ([`fuzzing.md`](fuzzing.md), "Script
that does not end").

**Script memory.** See [`memory.md`](memory.md) for what a page's
script heap holds and how to measure it, and "How script memory is kept
down" below for what the runtime does about it. `au_js_run_tasks` runs
the collector when script has been idle for a second and the heap has
grown by an eighth (at least 512 KB) since the last collection; a
hidden document is collected when it is hidden.

## Script memory limits

To change the limits or the policy, edit `src/browser/scriptmem.h` and
`scriptmem.c`. The limits are two preferences:

| Preference | Limits | In Preferences > Content |
|---|---|---|
| `content.script_memory_kb` | one page's runtime, shared by its frames | Script memory per page |
| `content.worker_memory_kb` | each worker | Script memory per worker |

The value 0 is Automatic and is the default; it is stored as 0, so it
follows the policy of the Aura that reads it. Any other value is the
user's own limit in KB, clamped to the custom range
(`au_script_memory_clamp`). `au_script_memory_limits` gives the limits
in force. The policy:

| | Automatic | Unknown machine (host tools, tests) | Custom range |
|---|---|---|---|
| Page | half of the memory free, at least 8 MB, at most 96 MB | 64 MB | 4 to 256 MB |
| Worker | a quarter of the memory free, at least 4 MB, at most 32 MB | 32 MB | 4 to 256 MB |

- **"Memory free"** is `AuBrowserHost.memory_free_kb`: on Mac OS 9 the
  free application heap plus free temporary memory
  (`mac_memory_free_kb`), read when a document begins (`begin_doc` in
  `page_doc.c`, which calls `au_docview_set_script_memory`). A change
  of preference applies to pages loaded afterwards.
- **Half and a quarter:** the page's documents, styles, layout and
  images need the rest, and tabs share it. The limits are caps, not
  reservations.
- **The ceilings** bound what one runaway page takes on a machine with
  much memory. The widget page that prompted the policy held 16 MB in
  its runtime with a 19 MB peak before the widget ran (64-bit host),
  and reached the earlier fixed 32 MB when it ran; 64 MB and 96 MB
  leave two to three times that.
- **The floors:** a document's context is 1.7 MB before any page
  script, and compiling a 1 MB script needs about 9 MB.
- **Examples.** The stock test machine (60 MB of heap and 400 MB of
  temporary memory free) gets 96 MB and 32 MB. A Mac with 20 MB of heap
  and 5 MB of temporary memory free gets 12 MB and 6 MB, and a heavy
  page fails there with the out-of-memory console line.
- **The engine's own defaults** (`au_js_settings_default`: 32 MB for a
  page's runtime; `AU_JS_WORKER_MEMORY`, 24 MB, for a worker) apply
  only to an embedder that sets nothing.
- **To set the limits on the host**, pass `--script-mem MB` and
  `--worker-mem MB` to `aura-host chrome`.
- **In Preferences > Content** (`prefswin.c`) each limit is a pop-up
  (Automatic, Custom) with an MB field that is active for Custom, and a
  line that says what Automatic gives on this Mac now. The pane's note
  says that changes apply to pages loaded afterwards.

**The low-memory check.** The engine asks `au_memory_low(n)` (`core.h`)
before it takes an arena or a larger block. The Mac shell answers yes
when less than 3 MB would be left (`low_memory` in `main.c`), and the
script then gets the out-of-memory error while there is still memory
for the page itself. `au_alloc` running out is still fatal. On
Mac OS 9 the script heap can also come from temporary memory
([`memory.md`](memory.md), "Temporary memory for the heap").

**Workers count.** `au_js_stats` adds each worker's heap, stack,
pending script and bytecode to the document's figure, so
`au_docview_memory_estimate` and the kept-document budget include them
(`test_shared_script` asserts it).

### What an out-of-memory line says

A script that is refused memory stops with a console line at error
level that names the runtime, its limit, its use, what it was doing and
the preference to change:

    Script stopped: out of memory. The page http://a.test/ reached its
    script memory limit of 4096 KB (4087 KB in use, 829 KB more asked
    for) while compiling http://a.test/big.js. To allow more, raise
    "Script memory per page" in Preferences > Content (the
    content.script_memory_kb preference).

- "The worker URL" replaces "The page URL" for a worker's runtime, with
  "Script memory per worker" and `content.worker_memory_kb`.
- "while running script" replaces "while compiling URL" when no compile
  was under way; "in the frame URL" is added when a frame's script was
  running.
- When the limit was not reached but the machine is short of memory
  (`au_memory_low`), the line says "Aura has too little memory left"
  and suggests closing tabs or raising Aura's partition.
- An allocation refused inside the parser is reported as out of memory
  by `eval_classic`. (It used to surface as a syntax error at the place
  the parser happened to be.)
- A worker that runs out of memory ends, and its `Worker` object in the
  page gets an `error` event ("out of memory: the worker reached its
  script memory limit of N KB"). The page goes on running
  (`js_worker_fatal` in `worker.c`). There is no page-level notice: the
  console line is the report.

### How script memory is kept down

Largest saving first. The measurements are in [`memory.md`](memory.md),
"Where script memory goes".

- **A large script is compiled once per page.** `runtime.c` ("Compiled
  scripts") keeps the compiled form of the last four classic scripts of
  64 KB or more. When another document of the page runs the same URL
  and bytes, the kept script is written as bytecode (`JS_WriteObject`)
  and read into that document's context (`JS_ReadObjectSource`) instead
  of being compiled. When the page starts a worker whose own script is
  one it has compiled, it writes the bytecode once more and hands it to
  the worker with the script (`js_code_for_worker`, `JsWorker.code`).
  Functions keep their text: the bytecode names positions in the
  script's text (patch 0009). A kept script holds the code that ran
  once while the script loaded (250 KB for an 860 KB script), so an
  entry that nothing has read for 20 seconds is dropped
  (`js_code_expire`, called while the page is idle); a frame added
  later compiles again. Not covered: module scripts, a script a worker
  loads with `importScripts`, a script that is mostly data (its
  functions have texts of their own and the bytecode cannot name
  positions), and a script the worker compiled before the page did.
- **One source text per script, shared by the documents of a page**
  (patch 0008, which extends 0006). The parser does not copy each
  function's text while parsing. When a script's outermost functions
  make up half of it or more, one text holds the whole script and every
  function points into it; otherwise each outermost function gets a
  text and its nested functions share it. A whole-script text of 1 KB
  or more is found again by content, so every context of a runtime that
  compiles or reads the script shares it. `toString` returns the same
  characters as before (no text is stripped at any size).
- **A script's text is held once while it compiles.** The response
  buffer becomes the script's text (`au_docscript_resource_done` takes
  the buffer) and the engine frees it when the compile ends, before the
  script runs (`au_js_run_classic_take`). A worker's own script stays
  in C until `N.runScript(null)` takes it; the bytes fetched for a
  worker and for `importScripts` go to the native as a typed array,
  which it empties (`js_take_script_bytes`). (Before, the response
  buffer, a copy for the pending script and a copy for the engine
  existed during the run, and a worker held the bytes, a string and a
  copy while it compiled.)
- **The collector runs before the limit is reached.** QuickJS collects
  when the heap is half as large again as after its last collection,
  which near a limit is past the limit. The allocator lowers the
  engine's threshold when the heap passes seven eighths of the limit
  (`gc_pressure`), so the engine collects at its next object creation;
  it does not collect inside the allocator, where the engine may be in
  the middle of building an object. `eval_classic` also collects before
  compiling a script of 64 KB or more when the heap has grown by
  512 KB, because a compile creates no objects and so triggers no
  collection.
- **Parser buffers are trimmed** (patch 0009, `js_fd_trim`): 0.3 to
  0.9 MB of a compile's peak.
- `js_workers_memory` no longer counts a worker's script text after it
  is freed.

**Tests**

- `tests/browser/test_script_memory.c` (ctest `browser_script_memory`):
  a page, two same-origin frames and a worker that load one generated
  830 KB script finish under limits of 20 MB and 8 MB with stated bounds
  on both peaks; a worker that runs out of memory is reported as such,
  its `Worker` object gets `error`, and the page keeps running; a page
  that runs out while compiling names the script.
- `test_script_memory_prefs` in `tests/browser/test_browser.c`: the two
  preferences (default, round trip, clamping, Automatic stored as 0)
  and the policy function.

## Retro68 compiler points

QuickJS is built with `-fno-strict-aliasing` (with `uint32_t` as
`unsigned long`, string iterators never advanced), and patch 0004 pads
every `alloca`, because Retro68's GCC places spilled locals in the top
8 bytes of alloca space. Do not use `alloca` or variable-length arrays
in Aura's own code.

## The prelude

The prelude is one function expression split over
`src/js/prelude*.js`, joined in order (base, dom, html, window, web,
net, lazy, workers, obs). The build compiles it to QuickJS bytecode and
the runtime loads that.

- To add a file, add it to the list in `third_party/quickjs.cmake` and
  its `#embed` line in `prelude.c` (the source build).
- A name declared twice across the files stops the runtime from
  starting ("invalid redefinition").
- `Intl` is not part of the prelude: `src/js/intl.js` is compiled to
  bytecode of its own and read on first use (see "Intl" below). So are
  `src/js/streams.js` (the Streams API) and `src/js/eventsource.js`
  (see "Streams and EventSource" below), and `src/js/range.js`,
  `cssom.js` and `canvas.js` ("Modules read on first use" below).
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

### Modules read on first use

To move a part of the prelude out of every context's start-up heap,
make it a module like `src/js/range.js`: a function of `G` (the global
object) and `H` (a bag of prelude helpers) that returns its globals and
whatever the prelude's stubs call. Then:

- add its name to the `foreach(mod range cssom canvas)` list in
  `third_party/quickjs.cmake` (it is compiled to `build/*/gen/NAME.bc`),
  its two `#embed` blocks to `prelude.c` and its index to
  `n_lazy_module` in `runtime.c` (`N.lazy(i)`);
- in `prelude_lazy.js`, call `lazyModule(index, globals, helpers)`
  (`prelude.js`): each global becomes an accessor, one function per
  name serving as getter and setter, and the first read loads the
  module and turns every global into a plain data property (unless the
  page assigned one first). `name in window`, the window's own property
  names, `typeof` and `instanceof` see the global before and after, and
  the globals stay non-enumerable as they were;
- give the members of other interfaces that reach the module (a canvas
  element's `getContext`, `element.sheet`, `document.createRange`) a stub
  in `prelude_lazy.js` with the same name and length that calls the
  module;
- pass the helpers as a function (`() => ({ N, doc, ... })`), so that the
  bag is made at the first read and may name anything the prelude
  defines after `prelude_lazy.js`.

The three modules, on 2026-11-16 (bytecode sizes on the host):

| Module | Bytecode | Globals | Members that read it |
|---|---|---|---|
| `range.js` | 15,744 bytes | `AbstractRange`, `Range`, `StaticRange`, `Selection`, `getSelection` | `document.createRange`; `document.getSelection` through `getSelection` |
| `cssom.js` | 19,382 bytes | the 19 CSSOM classes (`CSSRule` to `StyleSheetList`) | `sheet` of style and link elements, `styleSheets` and `adoptedStyleSheets` of documents and shadow roots |
| `canvas.js` | 9,236 bytes | `CanvasRenderingContext2D`, `CanvasGradient`, `CanvasPattern`, `ImageBitmap`, `TextMetrics`, `Path2D`, `ImageData`, `DOMMatrixReadOnly`, `DOMMatrix`, `OffscreenCanvas`, `createImageBitmap` | a canvas element's `getContext`, `toDataURL`, `toBlob` |

The canvas element's `width` and `height` setters stay in the prelude
(`prelude_lazy.js`); they touch the context only when one exists, which
means `canvas.js` is loaded. The context's C functions stay on
`N.c2dProto` from the start: QuickJS makes a function in a function list
only when it is first read, so they cost a property each.

Measured on 2026-11-16, base `49d5f6e`, host (RelWithDebInfo, Apple
silicon, the machine busy with fuzzers, so the times are rough):

| | Before | After |
|---|---|---|
| A context's heap at start-up (`aura-host js --stats`, empty script) | 1,803,990 bytes | 1,635,883 bytes (-164 KB, -9.3%) |
| In use at start-up | 1462 KB | 1317 KB |
| Functions at start-up | 1851, 356 KB (115 KB of bytecode) | 1571, 301 KB (97 KB of bytecode) |
| Objects, properties, atoms at start-up | 3841, 11,230, 4115 | 3603, 10,517, 3801 |
| `prelude.bc` | 281,540 bytes | 240,780 bytes (and 44,362 in the three modules) |
| The heap after a script that touches all three modules | 1,803,990 bytes | 1,767,774 bytes |
| Start-up time (median of 15) | 6.80 ms | 6.20 ms |

A page's script runtime (Develop > Performance, `aura-host chrome --net
--tab URL --do wait:6000 --do perf`, two loads each):

| Page | Before | After | Modules the page read |
|---|---|---|---|
| Hacker News front page | 1783, 1783 KB | 1615, 1615 KB | none |
| Wikipedia article (PowerPC) | 9758, 9738 KB | 9605, 9569 KB | none |
| GitHub repository page (torvalds/linux) | 39,608, 39,615 KB | 39,648, 39,550 KB | `cssom.js` |

Left in the prelude: `IntersectionObserver` and `ResizeObserver` (most
script-heavy pages create one while they load, and both are run from
layout's entry points), `MutationObserver` and custom elements (the DOM
hooks call them), XMLHttpRequest and fetch (shared with the worker
prelude).

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

## Intl

To change `Intl`, edit `src/js/intl.js` and run `tests/js/intl.js`
(ctest `js_intl`); the local-zone cases are in `tests/js/clock.js`
(ctest `js_clock`). The tests' expected strings are Chrome's output for
en-US: check a new case in Chrome before you write it down.

QuickJS-ng has no `Intl`, and sites call it while their modules load:
GitHub's code view threw "Intl is not defined" and left the file list's
commit messages and dates, Releases, Contributors and Languages as grey
placeholders. Aura's `Intl` is written in JavaScript for English only,
with no locale data beyond small tables in the file:

| Object | What it does |
|---|---|
| `DateTimeFormat` | `format`, `formatToParts`, `formatRange`, `formatRangeToParts`, `resolvedOptions`; `dateStyle`, `timeStyle` and the component options (`weekday`, `era`, `year`, `month`, `day`, `hour`, `minute`, `second`, `fractionalSecondDigits`, `dayPeriod`, `timeZoneName` in all six styles), `hour12` and `hourCycle` |
| `NumberFormat` | `format`, `formatToParts`, `resolvedOptions`; decimal, percent, currency (symbols for 16 currencies, the code for others; `accounting`, `code`, `name`, `narrowSymbol`), unit (the 45 units ECMA-402 sanctions and their `-per-` pairs), compact, scientific and engineering notation, the digit and rounding options, `useGrouping`, `signDisplay`; numbers, BigInts and decimal strings, which keep all their digits |
| `RelativeTimeFormat` | `format`, `formatToParts`, `numeric: "auto"`, the long, short and narrow styles |
| `PluralRules` | English cardinal and ordinal rules, with the digit options |
| `Collator` | `compare`, with `sensitivity`, `numeric`, `caseFirst`, `ignorePunctuation` |
| `ListFormat`, `Segmenter`, `DisplayNames`, `Locale` | the common cases; `Segmenter` does graphemes, words and sentences |
| `Intl` | `getCanonicalLocales`, `supportedValuesOf`, and `supportedLocalesOf` on each constructor (the `en` locales) |

- **Locales.** Every locale tag is checked and canonicalized as the
  standard says (an invalid one throws RangeError), and every one is
  formatted as en-US; `resolvedOptions().locale` is `en-US`.
- **Time zones.** With no `timeZone` option, dates are formatted with
  `Date`'s local fields, so they follow the embedder's clock and zone
  ("Date and the clock" above). The local zone is named by its offset
  (`Etc/GMT+8`, or `+05:30` for an offset that is not whole hours),
  because Mac OS 9 has no zone database; GitHub sends this name to its
  server as the `tz` cookie. `UTC`, the `Etc/GMT` zones, offsets and 67
  named zones with their daylight saving rules (`ZONES` in the file) are
  known; any other name throws RangeError, as the standard says.
- **The locale methods.** `Number.prototype.toLocaleString`,
  `BigInt.prototype.toLocaleString`, `Date.prototype.toLocaleString`,
  `toLocaleDateString`, `toLocaleTimeString` and
  `String.prototype.localeCompare` go through the same code, so they
  agree with `Intl`; with no arguments they reuse one formatter each.
- **Loaded on first use.** `intl.js` is compiled to its own bytecode
  (`build/*/gen/intl.bc`, 92,641 bytes on the host) and is not part of the
  prelude. `prelude.js` defines `Intl` as a getter and the locale
  methods as small wrappers; the first time script touches either,
  `N.intl()` (`runtime.c`) reads the bytecode and the getter becomes a
  plain data property. A page that never uses `Intl` holds none of it.

What `Intl` costs, measured on 2026-11-15 on the host (RelWithDebInfo,
Apple silicon) with `aura-host js --stats`:

| | Before | After |
|---|---|---|
| A context's heap at start-up | 1,743,317 bytes | 1,751,437 bytes (+8 KB, including the SVG `style` properties added at the same time) |
| Start-up time (median of 20) | 3.3 ms | 3.3 ms |
| The heap after a script that formats one date and one number | | 1,933,999 bytes (+182 KB over start-up) |
| Reading `intl.bc` on first use | | 0.3 ms |

What is left out is in [`known-gaps.md`](known-gaps.md#javascript).

## Streams and EventSource

What the two modules do is in
[`script-requests-and-cors.md`](script-requests-and-cors.md); this
section is how they load. Each is a function of the global object and a
bag of prelude helpers (`H`), compiled to its own bytecode
(`build/*/gen/streams.bc`, 59,288 bytes on the host;
`eventsource.bc`, 4,376 bytes) and read by `N.streams()` or
`N.eventSource()` (`n_lazy` in `runtime.c`), which also adds the codec
and parser natives of `natives_streams.c` to `N` at that moment.

- `prelude.js` defines the 17 stream globals as accessors, one function
  per name serving as getter and setter; the first read loads the
  module and every one becomes a plain data property. `EventSource` is
  done the same way in `prelude_net.js`.
- A body's stream is made only when script reads `body`
  (`streams().bodyFromFeed` and `streamFromBytes`); a network body
  until then is a plain record (`newFeed`, `feedEvent` in
  `prelude_net.js`), so the prelude holds no stream code.
- To add a helper the modules need from the prelude, add it to the `H`
  bag in `streams()` or `eventSources()`.

What they cost, measured on 2026-11-16 on the host (RelWithDebInfo,
Apple silicon, the machine busy with another build, so the times are
higher than in the `Intl` table) with `aura-host js --stats`:

| | Before | After |
|---|---|---|
| A context's heap at start-up | 1,751,437 bytes | 1,772,088 bytes (+20 KB) |
| In use at start-up | 1423 KB | 1438 KB |
| Start-up time (median of 21, alternating runs) | 6.90 ms | 6.91 ms |
| The heap after a script that reads a stream and touches `EventSource` | | 1,954,490 bytes (+182 KB over start-up) |
| Reading `streams.bc` on first use | | 0.6 to 1.2 ms |
| Reading `eventsource.bc` on first use | | 0.1 to 0.2 ms |

Most of the 20 KB at start-up is the stream code in `prelude_net.js`
(feeds, bodies that may be streams, the XHR progress throttle) and the
17 accessors.

## document.cookie

`document.cookie` reads and sets the cookies of the document's own URL
through the network layer's jar. The rules the jar applies, what frames
and sandboxed documents get, and the tests are in
[`networking.md`](networking.md), "Cookies and `document.cookie`". The
path through the layers:

| Layer | Functions |
|---|---|
| The native | `n_cookie` (`natives2.c`), which calls `AuJsHooks.get_cookie` and `set_cookie` |
| The page's script glue | `h_get_cookie` and `h_set_cookie` (`src/page/script.c`) |
| The document view | `AuDocViewCallbacks.get_cookie` and `set_cookie`; nested views pass them up through `fc_get_cookie` in `frames.c` |
| The browser | `dv_get_cookie` in `page_doc.c`, then `AuPageFetcher.get_cookie` and `set_cookie` |
| The network stack | `au_net_document_cookie_get` and `_set` (`http.c`), then the jar's `au_cookie_jar_document_get` and `_set` |

An embedder that implements neither hook gets an empty string and
dropped writes. (That was the browser's state until 2026-11-15:
requests carried cookies, because the network layer has its own path to
the jar, but no test read the getter in a page.)

`document.referrer` is the Referer value the navigation carried, for a
top-level document; in a frame it is `""`
([`known-gaps.md`](known-gaps.md#javascript)). Web storage
(`localStorage`, `sessionStorage`) is described in
[`web-storage.md`](web-storage.md).

## window.postMessage

`window.postMessage` takes any absolute URL as the target origin and
compares only its origin (between frames: see
[`frames.md`](frames.md)). Google's search box defers its submit with
`postMessage("sbox.df", location.href)`; comparing the string with the
origin dropped the message and Return did nothing
(`tests/js/pages/search_defer.html`, `test_search_box_defer` in
`tests/forms`).

## Range

`range.js` (read on first use, "Modules read on first use" above),
`ranges.c`. Boundary points live in C so that the
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

`cssom.js` (read on first use, "Modules read on first use" above).
`element.sheet` and `document.styleSheets` give a `CSSStyleSheet` per
style or link element.

- Rules are read from the sheet's text (`AuJsHooks.sheet_text`,
  `au_dv_sheet_text` in `docview.c`) by a splitter in script.
- `insertRule`, `deleteRule` and changes to a rule write the whole text
  back (`au_dv_sheet_set_text`), and the document view parses it again
  into `SheetSrc.om_sheet`, which keeps that text, so the element's own
  text does not change.
- `sheet.disabled` sets `SheetSrc.disabled`.
- A rule's declarations are the `style` attribute of an element that is
  never inserted, so they share the inline style code.
- The text script writes back is parsed into `SheetSrc.om_sheet`, which
  replaces the sheet's own in the cascade; the resolver borrows sheets,
  so `au_dv_sheet_set_text` detaches them before it frees the old one.
  This is what keeps a parsed sheet that several documents share
  ([`css.md`](css.md), "Sheets shared between documents") unchanged:
  script never edits the shared sheet, it gets a sheet of its own.
- A link element's sheet works like a style element's: the parsed sheet
  keeps the text it was fetched as ([`css.md`](css.md), "How a parsed
  sheet is stored"), and `au_dv_sheet_text` reads it from there. The
  rules of a cross-origin sheet throw SecurityError.
- A cross-origin sheet's rules throw SecurityError.

**Constructed sheets.** `new CSSStyleSheet(options)` takes an id from a
counter in the prelude and makes the sheet in the document view
(`N.sheetConstruct`, `au_dv_sheet_construct`, `AuDocView.csheets`).
`replace`, `replaceSync`, `insertRule`, `deleteRule`, `media` and
`disabled` send the new text or flags the same way; `@import` rules are
dropped. A `FinalizationRegistry` tells the view when script lets go of
a sheet (`au_dv_sheet_destroy`); a sheet that a tree still adopts stays
until the document goes.

**`adoptedStyleSheets`** on a document or a shadow root is an array
behind a `Proxy`: each write sends the list of ids
(`N.sheetsAdopt`, `au_dv_sheets_adopt`). The view adds adopted sheets
after the tree's own sheets and restyles only that tree when the list
or an adopted sheet changes (`sheets_changed`,
[`css.md`](css.md), "Shadow trees and scoped styles").

## Shadow DOM

`natives.c` (attachShadow, slots), `prelude.js` (event paths),
`prelude_obs.js` (`slotchange`), `prelude_dom.js` (focus),
`src/dom/shadow.c` (assignment and the flat tree).

- **Declarative shadow roots.** The HTML parser makes them when
  `AuHtmlOptions.declarative_shadow` is set: the document view sets it
  for pages, `setHTMLUnsafe` and `Document.parseHTMLUnsafe` for their
  fragments and documents; `innerHTML` and `DOMParser` do not. The
  template goes on the stack of open elements only and its contents are
  the shadow root (`declarative_template` in `treebuilder.c`).
- **Slots.** Named assignment is worked out on demand
  (`au_node_assigned_slot`). `slot.assign()` stores node and slot pairs
  in the document (`AuDocument.manual`, no references; a node in a pair
  carries `AU_NODEF_MANUAL`, and freeing it drops its pairs).
  `assignedNodes({flatten: true})` replaces nested slots with what they
  offer, with an explicit stack.
- **Events** are retargeted across shadow boundaries; `composed`
  decides whether an event leaves its shadow root, and `composedPath()`
  hides nodes of closed trees from listeners outside them.
- **Focus.** `focus()` on a host whose shadow root has `delegatesFocus`
  focuses the first focusable element of its shadow tree.
  `document.activeElement` and `shadowRoot.activeElement` are the
  focused element retargeted to that tree. A focused element inside a
  shadow tree gives its hosts `:focus` and `:focus-within`.
- **Hit testing.** `document.elementFromPoint` asks layout
  (`AuJsHooks.hit_test`, `h_hit_test` in `script.c`) and retargets the
  element to the document; a shadow root's `elementFromPoint` answers
  for its own tree.

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

### crypto.subtle

To change `crypto.subtle`, work in `src/js/subtle.js` (algorithm
normalization, key types, usages and extractability, and the key
formats), `src/js/natives_crypto.c` (key handles and the natives) and
`src/net/webcrypto.c` (the cryptography, through TF-PSA-Crypto's PSA
interface; [`tls.md`](tls.md) says how the library is built). Run
`tests/js/subtle.js` (ctest `js_subtle`), `js_page_subtle_rsa_gen`,
`js_subtle_context` and `net_webcrypto_fuzz`, and the same under the
sanitizers.

| Algorithm | Operations | Key formats |
|---|---|---|
| SHA-1, SHA-256, SHA-384, SHA-512 | `digest` | |
| HMAC | `sign`, `verify`, `generateKey` | raw, jwk |
| AES-GCM, AES-CBC, AES-CTR | `encrypt`, `decrypt`, `wrapKey`, `unwrapKey`, `generateKey` | raw, jwk |
| AES-KW | `wrapKey`, `unwrapKey`, `generateKey` | raw, jwk |
| RSASSA-PKCS1-v1_5, RSA-PSS | `sign`, `verify`, `generateKey` | spki, pkcs8, jwk |
| RSA-OAEP | `encrypt`, `decrypt`, `wrapKey`, `unwrapKey`, `generateKey` | spki, pkcs8, jwk |
| ECDSA (P-256, P-384, P-521) | `sign`, `verify`, `generateKey` | raw (public), spki, pkcs8, jwk |
| ECDH (P-256, P-384, P-521), X25519 | `deriveBits`, `deriveKey`, `generateKey` | raw (public), spki, pkcs8, jwk |
| PBKDF2, HKDF | `deriveBits`, `deriveKey` | raw |

- **Where it exists.** `crypto.subtle`, `SubtleCrypto` and `CryptoKey`
  exist only in secure contexts, in pages and workers: https:, file:,
  about:, localhost and 127.0.0.1, and blob: URLs of those (a blob: URL
  of a file: page is `blob:null/...`). The prelude decides when the
  context starts (`prelude_web.js`); elsewhere `'subtle' in crypto` is
  false.
- **Loaded on first use.** `subtle.js` is compiled to its own bytecode
  (`build/*/gen/subtle.bc`) and read by `N.subtle()` the first time
  script touches `crypto.subtle`, `SubtleCrypto` or `CryptoKey`, as
  `Intl` is ("Intl" above).
- **Keys.** A `CryptoKey` holds a handle (a class in
  `natives_crypto.c`); the key material is in the library's key store,
  or for secret keys in a block that is zeroed when the handle is
  collected. Script never sees it unless it exports an extractable key.
  `type`, `extractable`, `algorithm` and `usages` follow the
  specification; `usages` are in its order, once each. What the live
  keys hold outside the script heap is capped at 4 MB
  (`AU_WC_HELD_MAX`); past it, making a key rejects with
  OperationError until the collector frees unused ones.
- **Errors** are the specification's: NotSupportedError for an
  algorithm, format or curve that is not there, TypeError for a missing
  member, SyntaxError for usages a key cannot have, InvalidAccessError
  for a key of another algorithm or without the usage or not
  extractable, DataError for bad key data, OperationError when the
  operation fails (a tag or padding that does not check, a length out
  of range).
- **Key formats.** `subtle.js` reads and writes spki, pkcs8,
  ECPrivateKey and the RSA structures in DER (definite, minimal lengths,
  nothing after the end) and converts JWKs; the library gets PKCS#1 DER
  for RSA, the scalar or the uncompressed point for EC keys and 32 bytes
  for X25519, and checks that RSA keys are consistent and EC points are
  on the curve. A JWK or ECPrivateKey whose public key does not match
  its private key is a DataError. Exported JWKs have Chrome's members in
  Chrome's (alphabetical) order, and pkcs8 EC keys carry the public key
  and no parameters, as Chrome's do.
- **Lengths.** Every length script passes is checked in `subtle.js` and
  again in C: AES keys of 16, 24 or 32 bytes, GCM tags of 32, 64, 96 and
  104 to 128 bits, a 16-byte CBC iv and CTR counter, KW data in 8-byte
  steps (16 bytes or more to wrap, 24 to unwrap), secret keys up to
  64 KB, RSA moduli up to 4096 bits, HKDF up to 255 hash blocks and
  PBKDF2 up to 1 MB of output.
- **RSA key generation** takes 1024 to 4096 bits in whole bytes and an
  odd public exponent below 2^31. It must not stop the browser, so it
  runs a slice at a time: in a page, on a coroutine of its own
  (`AuJsHooks.coro`), resumed for 8 ms by each step of `finishJob` in
  `subtle.js`, which runs each step as a task and the next as a timer
  so page timers, input and painting run between steps; in a worker,
  inline, leaving the worker's stack when its slice ends. Prime
  generation calls back between candidates and Miller-Rabin rounds
  through `au_tls_set_yield` (`third_party/mbedtls-patches/0002-bignum-yield.patch`).
  A page that goes away, or a worker that is terminated, abandons the
  generation at its next yield. Without coroutines it runs to the end
  in one step.
- **The entropy.** The library's random generator starts from
  `AuJsHooks.random` the first time script uses a key, if no https
  request has started it ([`tls.md`](tls.md), "Memory, clocks and
  entropy"); until the platform's pool is ready the calls reject with
  OperationError.
- The digests are `src/net/digest.c`: SHA-256 is the engine's own, the
  others come from the TLS library's PSA hash interface. HMAC is
  `au_hmac` over them.
- A `CryptoKey` cannot be cloned: `structuredClone` and `postMessage`
  of one throw DataCloneError.

Measured on 2026-11-16, base 6cd3ddf, on an Apple Silicon host under
load (`aura-host js`, averages; RSA generation time varies a lot from
key to key). The G4 column is an estimate, 30 to 50 times the host,
not a measurement: the library's bignum code uses 32-bit limbs there.

| Operation | Host | 1 GHz G4 (estimate) |
|---|---|---|
| RSA-1024 `generateKey` | 242 ms | 7 to 12 s |
| RSA-2048 `generateKey` | 761 ms | 25 to 40 s |
| RSA-4096 `generateKey` | 3.1 s | 2 to 3 min |
| RSA-2048 `sign` | 6.3 ms | 0.2 to 0.3 s |
| RSA-2048 `verify` | 0.1 ms | 5 ms |
| ECDSA P-256 `sign` / `verify` / `generateKey` | 0.3 / 0.9 / 0.2 ms | 10 / 30 / 8 ms |
| PBKDF2-SHA-256, 100,000 iterations | 165 ms | 5 to 8 s |
| AES-128-GCM `encrypt`, 1 MB | 8.9 ms | 0.3 to 0.5 s |

Only RSA generation runs in slices; the other operations, PBKDF2
included, run to the end in one step.

The PowerPC build (`Aura.pef`) grew by 76,198 bytes, measured against
889cb4a:

| Part | Bytes |
|---|---|
| AES-CBC, AES-CTR and AES-ECB, with the cipher module | 15,816 |
| PBKDF2 with HMAC | 2,224 |
| HKDF as one algorithm (TLS already has its halves) | 32 |
| RSA key pair import, export and generation, with the yield points | 5,696 |
| `webcrypto.c` | 12,520 |
| `natives_crypto.c` | 14,160 |
| `subtle.js` bytecode in the prelude | 26,336 |

The library's rows are the difference in its object code with each
option turned off.

What is left out is in [`known-gaps.md`](known-gaps.md#javascript).

## Tests

`tests/js` holds binding, loop, limit, lifetime, security and worker
scripts for `aura-host js`, scripted pages for `aura-host page`
(`tests/js/pages`) and the benchmark (`tests/js/bench/bench.js`).
`tests/js/lazy_prelude_a.js`, `_b.js` and `_c.js` (ctest
`js_lazy_prelude_*`, with `lazy_common.js`) check the modules read on
first use: each file touches every module first in another way
(`typeof`, `instanceof`, a member of another interface) in a fresh
window, and checks that feature detection sees the globals before, that
nothing else is read, and that a global the page replaced stays
replaced.

## Not done

See [`known-gaps.md`](known-gaps.md#javascript).
