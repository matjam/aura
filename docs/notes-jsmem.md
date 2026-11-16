# Notes from the jsmem branch

These notes belong in the topic documents under `docs/`. Each heading
names the topic it goes to. Dated 2026-11-15; all host figures are from
a 64-bit macOS host, RelWithDebInfo.

## JavaScript memory and limits

### To see what script memory a page uses

- Run `aura-host chrome --tab URL --do wait:3000 --do perf` (add `--net`
  for network pages), or choose Develop > Performance. Under Memory the
  report has one line per script runtime of the page with the bytes it
  holds now, the most it has held (peak) and its limit; under it one
  line per document in that runtime (the page and each frame); then one
  line per worker with its own bytes, peak and limit. The code is
  `au_js_memory_summary` (`src/js/runtime.c`), reached through
  `au_page_script_summary`.
- The per-document figures are what was allocated while that document's
  context was entered. The allocator (`qa_malloc` in `runtime.c`) writes
  the entered context's slot into each block's header. QuickJS takes
  blocks of up to 512 bytes from 4 KB arenas of its own, so a small
  block is charged to the document that made its arena: read the
  per-document lines as approximate and the runtime line as exact.
- `--do js:aura:memory` and `aura-host js --stats FILE` print QuickJS's
  accounting by kind. New lines: "in use" (what the engine's blocks
  hold), "free inside the engine's arenas" (the rest of the heap) and
  "source text" (each text counted once).
- `au_js_stats` reports `memory_used` as the bytes the allocator gave
  the engine, arenas' free blocks included (it used QuickJS's count of
  used blocks, about 10% less, and walked the heap to get it). The
  kept-document estimate uses this figure. It also reports
  `memory_peak`, `worker_peak` and `worker_limit`.

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
  (`au_memory_low`, see below), the line says "Aura has too little
  memory left" and suggests closing tabs or raising Aura's partition.
- An allocation refused inside the parser used to surface as a syntax
  error at the place the parser happened to be. `eval_classic` now
  reports it as out of memory.
- A worker that runs out of memory ends, and its `Worker` object in the
  page gets an `error` event ("out of memory: the worker reached its
  script memory limit of N KB"). The page goes on running
  (`js_worker_fatal` in `worker.c`). There is no page-level notice: the
  console line is the report.

### Where the memory goes

One document that loads an 860 KB minified script (one outer function
around about 3,600 functions) holds 6.6 MB after the script has run. A
document's context with the prelude alone is 1.7 MB (1.4 MB in use), a
worker's 0.6 MB. The script's share, by QuickJS's accounting:

| Part | KB | Shared by the documents of a page |
|---|---|---|
| Source text kept for `Function.prototype.toString` | 840 | yes, one copy per runtime (was one per document) |
| Bytecode | 517 | no (a function's bytecode belongs to one realm) |
| Function records (variables, closure tables, constant pools) | 400 | no |
| Line tables | 153 | no |
| Atoms (names) | 257 | yes, atoms belong to the runtime |
| Objects, properties, shapes, strings, arrays the script made | 960 | no |
| Block headers (8 bytes each) and parts not itemised (closure cells) | 740 | no |
| Free blocks inside the engine's arenas | 630 | no |

Compiling costs more than it keeps. For this script the parser held
4.0 MB beyond the result at its peak: a 544-byte record per function
(1.9 MB), every function's first-pass bytecode (2.7 MB, kept until the
whole script is parsed, plus 0.7 MB of unused buffer), and a copy of
every function's text, nested text once per level (2.2 MB). Compiling
took 116 to 144 ms; writing the compiled script as bytecode took 3 ms
(1.2 MB) and reading it 3 to 4 ms with no working memory.

Settled and peak script memory, in KB, for N documents that each load
the script (`run/mem` pages: the page plus N-1 same-origin frames;
"worker" adds a dedicated worker that loads it):

| Case | Before: settled | Before: peak | After: settled | After: peak |
|---|---|---|---|---|
| N = 1 | 6,722 | 11,596 | 6,889 | 9,089 |
| N = 2 | 12,450 | 17,324 | 11,629 | 11,912 |
| N = 3 | 18,218 | 23,040 | 16,449 | 16,735 |
| N = 3, the worker's runtime (script through `importScripts`) | 5,646 | 13,034 | 5,498 | 8,009 |
| N = 3, the worker's runtime (script as the worker's own) | not measured | not measured | 5,250 | 5,533 |

"Before" is `main` with only the counters added. The settled figure of
N = 1 rose by 250 KB because the compiled script is kept for 20 seconds
(see below). The generated script of `tests/browser/test_script_memory.c`
(830 KB, 6,000 functions) with two frames and a worker peaked at
20,625 KB in the page's runtime and 10,581 KB in the worker with each
context compiling for itself, and at 15,468 KB and 4,535 KB now.

### What was changed, largest first

- **A large script is compiled once per page.** `runtime.c` ("Compiled
  scripts") keeps the compiled form of the last four classic scripts of
  64 KB or more. When another document of the page runs the same URL
  and bytes, the kept script is written as bytecode (`JS_WriteObject`)
  and read into that document's context (`JS_ReadObjectSource`) instead
  of being compiled. When the page starts a worker whose own script is
  one it has compiled, it writes the bytecode once more and hands it to
  the worker with the script (`js_code_for_worker`, `JsWorker.code`).
  Functions keep their text: the bytecode names positions in the
  script's text (QuickJS patch 0009). A kept script holds the code that
  ran once while the script loaded (250 KB for the 860 KB script), so an
  entry that nothing has read for 20 seconds is dropped
  (`js_code_expire`, called while the page is idle); a frame added later
  compiles again. Not covered: module scripts, a script a worker loads
  with `importScripts`, a script that is mostly data (its functions have
  texts of their own and the bytecode cannot name positions), and a
  script the worker compiled before the page did.
- **One source text per script, shared by the documents of a page**
  (QuickJS patch 0008). The parser no longer copies each function's
  text while parsing. When a script's outermost functions make up half
  of it or more, one text holds the whole script and every function
  points into it; otherwise each outermost function gets a text and its
  nested functions share it, as before. A whole-script text of 1 KB or
  more is found again by content, so every context of a runtime that
  compiles or reads the script shares it. `toString` returns the same
  characters as before (no text is stripped at any size).
- **A script's text is held once while it compiles.** The response
  buffer becomes the script's text (`au_docscript_resource_done` takes
  the buffer) and the engine frees it when the compile ends, before the
  script runs (`au_js_run_classic_take`). Before, the response buffer, a
  copy for the pending script and a copy for the engine existed during
  the run. A worker's own script stays in C until `N.runScript(null)`
  takes it; the bytes fetched for a worker and for `importScripts` go to
  the native as a typed array, which it empties
  (`js_take_script_bytes`). Before, a worker held the bytes, a string
  and a copy while it compiled.
- **The collector runs before the limit is reached.** QuickJS collects
  when the heap is half as large again as after its last collection,
  which near a limit is past the limit. The allocator lowers the
  engine's threshold when the heap passes seven eighths of the limit
  (`gc_pressure`), so the engine collects at its next object creation;
  it does not collect inside the allocator, where the engine may be in
  the middle of building an object. `eval_classic` also collects before
  compiling a script of 64 KB or more when the heap has grown by 512 KB,
  because a compile creates no objects and so triggers no collection.
- **Parser buffers are trimmed** (patch 0009, `js_fd_trim`): 0.3 to
  0.9 MB of a compile's peak.
- `js_workers_memory` counted a worker's script text after it was freed.

Not done:

- Line tables are kept (153 KB per document for the script above, 3%).
  Error locations in the Console depend on them.
- The prelude is still created in every context (1.4 MB per document,
  0.6 MB per worker). Its classes and prototypes must be per realm
  (`frame.contentWindow.Node !== Node`), and a function's bytecode
  belongs to one realm, so only the 109 KB of prelude bytecode and its
  function records could be shared, and only by teaching QuickJS to
  share bytecode between realms. The cheaper step is the one already
  listed under Memory: create the CSSOM, Range and observer parts on
  first use (an estimated 150 to 250 KB per context).
- The compile itself still holds about 4 MB beyond its result for an
  860 KB script (a record per function and all first-pass bytecode
  until the script ends). That is how QuickJS resolves variables.

### The limits

The limits are preferences, `content.script_memory_kb` (one page's
runtime, shared by its frames) and `content.worker_memory_kb` (each
worker). The value 0 is Automatic and is the default; it is stored as 0,
so it follows the policy of the Aura that reads it. Any other value is
the user's own limit in KB, clamped to 4 MB .. 256 MB
(`au_script_memory_clamp`). `src/browser/scriptmem.h` holds the policy:

| | Automatic | Unknown machine (host tools, tests) | Custom range |
|---|---|---|---|
| Page | half of the memory free, at least 8 MB, at most 96 MB | 64 MB | 4 to 256 MB |
| Worker | a quarter of the memory free, at least 4 MB, at most 32 MB | 32 MB | 4 to 256 MB |

- "Memory free" is `AuBrowserHost.memory_free_kb`: on Mac OS 9 the free
  application heap plus free temporary memory (`mac_memory_free_kb`),
  read when a document begins (`begin_doc` in `page_doc.c`, which
  calls `au_docview_set_script_memory`). A change of preference applies
  to pages loaded afterwards.
- Why half and a quarter: the page's documents, styles, layout and
  images need the rest, and tabs share it. The limits are caps, not
  reservations.
- Why the ceilings: they bound what one runaway page takes on a machine
  with much memory. The widget page that prompted this held 16 MB in
  its runtime with a 19 MB peak before the widget ran (64-bit host),
  and reached the old fixed 32 MB when it ran; 64 MB and 96 MB leave
  two to three times that.
- Why the floors: a document's context is 1.7 MB before any page script,
  and compiling a 1 MB script needs about 9 MB.
- The stock test machine (60 MB of heap and 400 MB of temporary memory
  free) gets 96 MB and 32 MB. A Mac with 20 MB of heap and 5 MB of
  temporary memory free gets 12 MB and 6 MB, and a heavy page fails
  there with the console line above.
- The engine's own defaults (`au_js_settings_default`: 32 MB, and
  24 MB for a worker) apply only to an embedder that sets nothing.
- **A second guard.** The engine asks `au_memory_low(n)` (`core.h`)
  before it takes an arena or a larger block. The Mac shell answers yes
  when less than 3 MB would be left (`low_memory` in `main.c`), and the
  script then gets the out-of-memory error while there is still memory
  for the page itself. `au_alloc` running out is still fatal.
- **Workers count.** `au_js_stats` adds each worker's heap, stack,
  pending script and bytecode to the document's figure, so
  `au_docview_memory_estimate` and the kept-document budget include
  them (checked; `test_shared_script` asserts it).
- **To set the limits on the host**, pass `--script-mem MB` and
  `--worker-mem MB` to `aura-host chrome`.
- **In Preferences > Content** (`prefswin.c`) each limit is a pop-up
  (Automatic, Custom) with an MB field that is active for Custom, and a
  line that says what Automatic gives on this Mac now. The pane's note
  says that changes apply to pages loaded afterwards.

### Where script memory comes from on Mac OS 9

The script heap is `au_try_alloc`, so it came from the application heap
only: QuickJS's 4 KB arenas and larger blocks were one `NewPtr` each
(they are over the pooled allocator's 2 KB), and nothing used temporary
memory.

`heap_get` in `main.c` now takes a platform block from temporary memory
when the application heap is short: when the block would leave less
than 2 MB free there (`HEAP_FLOOR`), or when `NewPtr` fails. Temporary
memory is taken 256 KB at a time (one locked handle); blocks under
128 KB are cut from the chunk in order and the chunk is returned when
its last block is freed. Larger blocks get a handle each. Every platform
block has an 8-byte header that says which kind it is. This applies to
every allocation, not only to script. Develop > Performance shows how
much temporary memory the heap has taken.

Not verified: the temporary memory path has been compiled for PowerPC
and Aura ran in the stock guest with the new block headers, but the
guest's heap never ran short (47 MB free), so no block came from
temporary memory there. To exercise it, lower Aura's partition in Get
Info to 12 MB and load a heavy page; Develop > Performance then shows
"KB taken by Aura's heap" above 0.

### Measured in the guest

Stock base, 2026-11-15, the generated 830 KB script of
`tests/browser/test_script_memory.c` loaded by a page, two `srcdoc`
frames and a worker (`run/guest/mem.html`, pushed with the
application): the page, both frames and the worker computed the same
value, so bytecode written by one context and read by another, and by
a worker's runtime, works on PowerPC. Develop > Performance showed the
page's runtime at 10,766 KB with a peak of 12,585 KB (limit 98,304 KB:
Automatic with 47 MB of heap and 404 MB of temporary memory free), the
page's document at 4,555 KB, each frame at about 3,100 KB with a peak
of 4,500 KB, and the worker at 3,631 KB with a peak of 3,785 KB (limit
32,768 KB). The load took 1.35 s. With "Script memory per page" set to
Custom, 48 MB, the next load showed a limit of 49,152 KB.

### Tests

- `tests/browser/test_script_memory.c` (ctest `browser_script_memory`):
  a page, two same-origin frames and a worker that load one generated
  830 KB script finish under limits of 20 MB and 8 MB with stated bounds
  on both peaks; a worker that runs out of memory is reported as such,
  its `Worker` object gets `error`, and the page keeps running; a page
  that runs out while compiling names the script.
- `test_script_memory_prefs` in `tests/browser/test_browser.c`: the
  two preferences (default, round trip, clamping, Automatic stored as
  0) and the policy function.

## Cookies

`document.cookie` read back empty and writes were dropped: the script
natives called `AuJsHooks.get_cookie` and `set_cookie`, and no embedder
implemented them. Requests carried cookies because the network layer
has its own path to the jar. No test read the getter in a page.

- **The path now:** `n_cookie` (`natives2.c`), `h_get_cookie` and
  `h_set_cookie` (`src/page/script.c`), `AuDocViewCallbacks.get_cookie`
  and `set_cookie` (nested views pass them up through `fc_get_cookie`
  in `frames.c`), `dv_get_cookie` in `page_doc.c`,
  `AuPageFetcher.get_cookie` and `set_cookie`,
  `au_net_document_cookie_get` and `_set` (`http.c`), and the jar's
  `au_cookie_jar_document_get` and `_set`.
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
- **Seen and not changed:** `document.referrer` is always `""`.

## Frames

- **A frame's window exists as soon as its element does.** A script
  that follows an `<iframe>` in the same parser chunk now finds
  `frames.length`, `window[0]`, `window["name"]` and `contentWindow`
  (`run_classic` in `script.c` brings the frame list up to date before a
  parser script runs; the parser's insertions do not mark the list, so
  the check walks the document, and is made only while the parser
  runs). The window is given its indexed and named frames before the
  frames' documents start (`au_frames_sync`), so the script of a
  `srcdoc` frame finds an earlier sibling through `parent.frames[name]`
  while it loads.
- **The initial document.** A frame whose response has not arrived gets
  an `about:blank` document and a script context when script first asks
  for its window (`au_frames_view_of`, which already did this); no
  context is made for a frame nobody asks about.
- **What differs from the standard.** When the frame's real document
  begins, the frame gets a new window object. The standard keeps one
  WindowProxy per frame for its whole life, and reuses the Window of
  the initial `about:blank` document for a first same-origin document.
  In Aura, `frames[i]`, `window["name"]` and `contentWindow` read again
  give the new window, and a reference taken earlier is not equal to
  it. Through the old reference, `document` is still the blank
  document and properties script put on the window are still there;
  `location` answers for the top-level document; a message posted to it
  does not reach the new window. Keeping the identity needs a context
  that can take a new document (the prelude captures the document when
  it starts), which is not a small change.
- **A `srcdoc` or `about:blank` frame resolves URLs against its
  parent's base URL** (`au_docview_begin`); it used `about:srcdoc`, so
  a relative script or image in a `srcdoc` document did not load.
- **Tests:** `tests/js/pages/frame_initial.html`.

## Third-party code

`third_party/quickjs-patches` gained `0008-one-source-text-per-script`
and `0009-bytecode-source-positions`; `third_party/README.md` lists
them. After changing either, run JSTest in the guest, as for any QuickJS
change.
