# Memory

This document covers Aura's memory model: `au_alloc` and what happens
when memory runs out, the pooled allocator on Mac OS 9, the budgets of
each cache, how to measure on the host and in the guest, and what
things cost. Read it when Aura runs out of memory, before adding a
cache, or before changing `src/core/heap.c`.

The Mac OS 9 application runs in a 64 MB partition (12 MB at least,
`aura.r`). Font data, the window's GWorld and the blocklists go to
temporary memory when there is room; everything else comes from the
application heap through `au_alloc`, which takes temporary memory when
the heap is short ("Temporary memory for the heap" below).

## To see which code holds memory on the host

Configure a tree with the allocation counter, build `aura-host`, and
run it with `AURA_MEM=1`:

```bash
cmake -S . -B build/mem -G Ninja -DAURA_MEM_TRACK=ON
AURA_MEM=1 build/mem/src/platform/host/aura-host chrome --bundled-only --tab PAGE -o run/a.png
```

- After the paint it prints the bytes live per source folder and per
  source line, with what each held when the total was at its peak.
- `--do mem` prints the same at any point, and `--do closetab --do mem`
  shows what a closed tab leaves behind.
- `AURA_MEM_MIN=BYTES` sets the smallest line listed.
- The counter (`src/core/core.h`) records the file and line of every
  allocating call through macros and puts a 16-byte header on each
  block; a buffer is counted for the line that first allocated it. A
  helper that allocates for its callers takes the caller's line when it
  calls `(au_realloc)(...)` in parentheses and has a macro of its own,
  as `css_grow` does.

**Use `--bundled-only` for figures that should resemble the Mac.**
Without it the host tool reads 22 macOS fonts whole at start (19.6 MB),
which the Mac application never does: there a face's data is read on
first use, into temporary memory. The host's PNG writer takes 11 MB at
the end and the tool's own window surface 3 MB (1024 x 768); the Mac's
GWorld is in temporary memory.

## To see what a script heap holds

Run `aura-host js --stats FILE` or, for a page, `aura-host chrome --net
--tab URL --do wait:4000 --do js:aura:memory` (QuickJS's accounting:
atoms, strings, objects, properties, shapes, function records,
bytecode, line tables). `--do js:aura:gc` runs the collector.

- The accounting also prints "in use" (what the engine's blocks hold),
  "free inside the engine's arenas" (the rest of the heap) and "source
  text" (each text counted once).
- **Per runtime, document and worker.** Run `aura-host chrome --tab URL
  --do wait:3000 --do perf` (add `--net` for network pages), or choose
  Develop > Performance in the application. The report has one line per
  script runtime of the page with the bytes it holds now, the most it
  has held (peak) and its limit; under it one line per document in that
  runtime (the page and each frame); then one line per worker with its
  own bytes, peak and limit. The code is `au_js_memory_summary`
  (`src/js/runtime.c`), reached through `au_page_script_summary`.
- **Read the per-document lines as approximate and the runtime line as
  exact.** The per-document figures are what was allocated while that
  document's context was entered: the allocator (`qa_malloc` in
  `runtime.c`) writes the entered context's slot into each block's
  header. QuickJS takes blocks of up to 512 bytes from 4 KB arenas of
  its own, so a small block is charged to the document that made its
  arena.
- `au_js_stats` reports `memory_used` as the bytes the allocator gave
  the engine, arenas' free blocks included, and `memory_peak`,
  `worker_peak` and `worker_limit`. The kept-document estimate uses
  `memory_used`. (It used QuickJS's count of used blocks, about 10%
  less, and walked the heap to get it.)
- The limits and the out-of-memory console line are in
  [`javascript.md`](javascript.md), "Script memory limits".

To see what parsed style sheets hold, and where, run `css_bench --mem`,
`--where` or `--page` ([`css.md`](css.md), "To measure").

## To measure the Mac application

Choose Develop > Performance or Develop > System Status: both show the
free heap, the largest free block and free temporary memory, and
Performance shows the temporary memory the heap has taken. The HTTP
cache section of System Status shows the bytes the HTTP cache holds in
memory and on disk ([`developer-tools.md`](developer-tools.md)). Take
the first
reading after a page has loaded once: the network stack, the root store
and the blocklists start after launch.

## Allocation

Allocate with `au_alloc`, `au_realloc`, `au_free` (`src/core/core.h`).
They do not return NULL. When malloc fails they first call the release
handler (`au_set_release_handler`), which frees caches, and try again;
the out-of-memory handler runs when nothing is left to free.

Code that must survive a failed allocation uses `au_try_alloc`: the TLS
library ([`tls.md`](tls.md)), canvas bitmaps
([`canvas.md`](canvas.md)), blocklist memory
([`content-blocking.md`](content-blocking.md)). Script has its own
limit and never reaches the fatal handler
([`javascript.md`](javascript.md)).

In the Mac application the release handler is `release_caches` in
`main.c`, which drops the kept documents and, when there are none, the
background tabs that can be unloaded (`au_browser_release_memory`); the out-of-memory handler
closes Open Transport before ExitToShell
([`mac-os-9-shell.md`](mac-os-9-shell.md)). Command-Option-M is the
out-of-memory test.

## The pooled allocator

On Mac OS 9, `au_alloc` takes every block of up to 16 KB from the
pooled allocator (`src/core/heap.c`, turned on by `au_heap_enable` at
the start of `main`), and calls `NewPtr` only for a slab or a block
over 16 KB.

To see what the pool holds on the host, run `aura-host` with
`AURA_HEAP=1` (the pool then runs over `malloc`, as on the Mac) and add
`--do heap`: it prints the blocks in use, the bytes held from the
platform, the slabs and large blocks, the blocks over 2 KB and their
slabs, and the empty slabs kept. For example, `AURA_HEAP=1 aura-host
chrome --net --tab about:blank --tab URL --active 1 --do wait:8000 --do
heap --do closetab --do wait:3000 --do heap` shows what a page leaves
behind when its tab closes.

**What a block costs there.** Every block has a header of 8 bytes (the
slab and the size asked for), and its size is rounded up to its class:

| Size | Classes | Slab |
|---|---|---|
| up to 128 bytes | every 8 bytes | 32 KB, shared by the small classes |
| 129 bytes to 2 KB | four per doubling (a 130-byte block takes a 160-byte slot) | 32 KB, shared by the small classes |
| over 2 KB to 16 KB | 2560, 3072, 3584, 4112, 5120, 6144, 7168, 8208, 10240, 12288, 14336, 16400 bytes | about 32 KB, a whole number of blocks and at least 4: 13 blocks of 2560, 8 of 4112, 4 of 16400 |
| over 16400 bytes | none: a `NewPtr` of its own, with the same header and the Memory Manager's | |

The classes at a power of two are 16 bytes over it so that the script
engine's 4 KB arenas fit: QuickJS takes its blocks of up to 512 bytes
from arenas of at most 4096 bytes, and `qa_malloc`
(`src/js/runtime.c`) adds a 16-byte header to each.

**Empty slabs.** A slab is returned only when its last block is freed.
The pool then keeps up to 8 empty small slabs and, for the classes
over 2 KB, one empty slab per class and 4 in all; the others go back to
the platform at once. `au_heap_trim` gives back the kept slabs too.
`au_alloc` and `au_try_alloc` call it when the platform has no memory,
before the release handler, and again after the handler has run.

**Long-lived blocks pin slabs.** One block that outlives a page keeps
its whole slab, so many small long-lived blocks hold more memory than
they use ("Blocks over 2 KB" below has the figures). Prefer one
allocation for records that live and die together: a parsed style
sheet keeps its rules and selectors in an arena for this reason
([`css.md`](css.md), "How a parsed sheet is stored").

Retro68's `malloc` is one `NewPtr` per block, and the Memory Manager
took tens of seconds per layout with the 90,000 blocks of a large page.
Do not call `malloc` in engine code, and enable the pool before the
first allocation in a new Mac OS 9 program. `tests/core/test_heap.c`
covers it on the host.

## Temporary memory for the heap

On Mac OS 9, `heap_get` in `main.c` takes a platform block from
temporary memory when the application heap is short: when the block
would leave less than 2 MB free there (`HEAP_FLOOR`), or when `NewPtr`
fails. This applies to every allocation, script included: the script
heap is `au_try_alloc`, its 4 KB arenas come from the pool's slabs, and
its blocks over 16 KB are one platform block each.

- Temporary memory is taken 256 KB at a time (one locked handle);
  blocks under 128 KB are cut from the chunk in order and the chunk is
  returned when its last block is freed. Larger blocks get a handle
  each.
- Every platform block has an 8-byte header that says which kind it is.
- Develop > Performance shows how much temporary memory the heap has
  taken (the "taken by Aura" line under Temporary memory).
- To exercise the path, lower Aura's partition in Get Info to 12 MB and
  load a heavy page; the "taken by Aura" line then shows above 0.
- Not verified: the path has been compiled for PowerPC and Aura ran in
  the stock guest with the new block headers, but the guest's heap
  never ran short (47 MB free), so no block came from temporary memory
  there ([`known-gaps.md`](known-gaps.md#not-verified)).

## Budgets

| What | Budget | Document |
|---|---|---|
| The application partition | 64 MB (12 MB at least) | |
| Kept documents | 3 per tab, 24 MB over all tabs; the oldest is dropped while less than 6 MB of heap is free | [`browser-chrome.md`](browser-chrome.md) |
| Background tabs | unloaded, least recently shown first, while less than 6 MB is free (heap and temporary memory together), or after 30 minutes in the background while less than 16 MB is free | [`browser-chrome.md`](browser-chrome.md) |
| The image cache | 8 MB per document by default; one image at most half of it | [`images.md`](images.md) |
| A decoded image | 16 MB | [`images.md`](images.md) |
| The HTTP memory cache | 8 MB by default; 1 MB resident with the disk cache on | [`networking.md`](networking.md) |
| A page's script runtime, shared by its frames | Automatic: half of the memory free, at least 8 MB, at most 96 MB (64 MB where the machine is unknown); or the `content.script_memory_kb` preference, 4 to 256 MB | [`javascript.md`](javascript.md), "Script memory limits" |
| A worker | Automatic: a quarter of the memory free, at least 4 MB, at most 32 MB (32 MB where the machine is unknown); or the `content.worker_memory_kb` preference, 4 to 256 MB. 256 KB of stack | [`javascript.md`](javascript.md), "Script memory limits"; [`workers.md`](workers.md) |
| Script, when the machine is short | refused when less than 3 MB would be left (`au_memory_low`) | [`javascript.md`](javascript.md), "Script memory limits" |
| A document's canvases | 32 MB; one canvas 16 MB | [`canvas.md`](canvas.md) |
| Images script holds for a canvas | 4 MB each, 12 MB together | [`canvas.md`](canvas.md) |

**The HTTP cache** holds response bodies in memory up to its budget
(8 MB by default). With the disk cache on, the Mac application keeps
1 MB of them resident (`au_cache_set_resident`); the rest are read back
from disk when asked for.

## What things cost

Host, 64-bit, bundled fonts only, 1024 x 768, KB live after the paint,
before and after the work of 2026-11-15. A PowerPC build has 4-byte
pointers, so pointer-heavy rows are smaller there.

| Subsystem | Blank tab | Wikipedia article, no script (1 MB of HTML, 9300 elements, 18,000 boxes) | GitHub home page (2.5 MB of CSS, scripts) |
|---|---|---|---|
| Layout: boxes, fragments, lines | 66, 66 | 6693, 3357 | 2175, 972 |
| DOM: nodes, attributes, text | 1, 1 | 3849, 3657 | 1178, 1090 |
| CSS: parsed sheets, rule index, computed styles | 152, 146 | 2238, 2188 | 25638, 24688 |
| Page source kept for Save Page As | 6, 7 | 2057, 1082 | 521, 535 |
| Script heap | 0, 0 | 0, 0 | 2088, 1548 |
| Atoms and other core | 60, 60 | 269, 269 | 724, 724 |
| Font tables, shaping caches, glyph cache | 43, 43 | 278, 278 | 776, 753 |
| HTML parser, document view, image cache | 11, 12 | 92, 94 | 107, 108 |
| Host tool only: font file data | 1024, 1024 | 5376, 5376 | 3840, 3840 |
| Host tool only: window surface | 3072, 3072 | 3072, 3072 | 3072, 3072 |
| Total live | 4453, 4449 | 23942, 19389 | 40138, 37348 |

The style sheet work that followed (below, "Parsed style sheets")
changed the GitHub column: CSS is 6036 KB, the sheets' text adds
2499 KB to the document view's row until it is packed (about 600 KB
then), and the total live is 19,126 KB before packing.

With the system fonts, `/usr/bin/time -l` reports a peak footprint of
(before the style sheet work):

| Page | Now | Before |
|---|---|---|
| a blank tab (fonts, PNG and surface are all of it) | 31.7 MB | 31.3 MB |
| the Wikipedia article | 49.2 MB | 52.2 MB |
| the GitHub page without its sheets | 38.7 MB | 42.1 MB |
| the GitHub page with them | 78.2 MB | 80.2 MB |

### Parsed style sheets

`css_bench --where --style PAGE` after the work of 2026-11-15 on style
sheets: what the sheets of a page hold once the page is styled and
their texts are packed, on the host and, from the struct layouts, with
32-bit pointers. Before that
work the GitHub home page's sheets (28 linked, 20 distinct) were
300,000 blocks: 17.6 MB on the host, and 13.9 MB with 32-bit pointers
plus 2.4 MB of block headers in the pooled allocator. The Wikipedia
article's were 10,200 blocks, 0.74 MB and 0.59 MB.

| What | GitHub, host | 32-bit | Wikipedia, host | 32-bit |
|---|---|---|---|---|
| Rules | 655 KB | 328 KB | 44 KB | 22 KB |
| Selector lists and selectors | 461 KB | 312 KB | 45 KB | 31 KB |
| Components and filter hashes | 735 KB | 376 KB | 119 KB | 62 KB |
| Attribute values, An+B, names that differ in case | 96 KB | 82 KB | 19 KB | 15 KB |
| Declaration blocks of the rules that matched | 166 KB | 137 KB | 44 KB | 36 KB |
| Media lists, layer names, condition text | 38 KB | 36 KB | 8 KB | 7 KB |
| The arena that holds the rows above | 2240 KB in 194 chunks | 1323 KB | 308 KB in 27 chunks | 192 KB |
| The sheets' text, packed (2559 KB and 220 KB as it arrived) | 596 KB | 596 KB | 48 KB | 48 KB |
| Value trees | 20 KB | 16 KB | 14 KB | 12 KB |
| Custom property and `var()` token lists | 473 KB | 396 KB | 48 KB | 41 KB |
| Sheet records, keyframes, font faces | 29 KB | 21 KB | 0.4 KB | 0.2 KB |
| Total | 3.4 MB in 4300 blocks | 2.4 MB | 0.41 MB in 480 blocks | 0.29 MB |

Of GitHub's 16,400 rules, 14,200 blocks of declarations and 280
`@media` blocks are still text after the page is styled. For the page
as the browser loads it (`AURA_MEM=1`, host), `src/css` went from
24.7 MB in 347,000 blocks to 6.0 MB in 19,600, plus the sheets' text
(which the document view used to free: 2.5 MB until it is packed,
0.6 MB after); the picture painted is the same, byte for byte. [`css.md`](css.md) has the figures step by step
and the times.

### A live Wikipedia article's script heap

It was 10.6 MB when the load ended: function records 3.9 MB (bytecode
0.4 MB, the rest mostly source text held once per nesting level),
objects 1.6 MB, properties 1.2 MB, atoms 0.8 MB, the prelude's own
objects about 1 MB. Sharing the source text brought it to 8.7 MB; four
seconds later the page's lazy modules have raised it to 11.3 MB, and
the idle collection takes 0.6 MB off. The kept-document estimate for
the article fell from 15.3 MB to 12.9 MB.

### Where script memory goes

Measured on 2026-11-15, 64-bit macOS host, RelWithDebInfo. What the
runtime does to keep these figures down is in
[`javascript.md`](javascript.md), "How script memory is kept down".

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
"worker" adds a dedicated worker that loads it). "Before" is `main`
with only the counters added; "after" has the compile-once cache, the
shared source text and the other changes:

| Case | Before: settled | Before: peak | After: settled | After: peak |
|---|---|---|---|---|
| N = 1 | 6,722 | 11,596 | 6,889 | 9,089 |
| N = 2 | 12,450 | 17,324 | 11,629 | 11,912 |
| N = 3 | 18,218 | 23,040 | 16,449 | 16,735 |
| N = 3, the worker's runtime (script through `importScripts`) | 5,646 | 13,034 | 5,498 | 8,009 |
| N = 3, the worker's runtime (script as the worker's own) | not measured | not measured | 5,250 | 5,533 |

- The settled figure of N = 1 rose by 250 KB because the compiled
  script is kept for 20 seconds.
- The generated script of `tests/browser/test_script_memory.c` (830 KB,
  6,000 functions) with two frames and a worker peaked at 20,625 KB in
  the page's runtime and 10,581 KB in the worker with each context
  compiling for itself, and at 15,468 KB and 4,535 KB after.

**In the guest.** Stock base, 2026-11-15, the same generated script
loaded by a page, two `srcdoc` frames and a worker
(`run/guest/mem.html`, pushed with the application). The page, both
frames and the worker computed the same value, so bytecode written by
one context and read by another, and by a worker's runtime, works on
PowerPC. The load took 1.35 s. Develop > Performance showed:

| Runtime or document | Holds | Peak | Limit |
|---|---|---|---|
| The page's runtime | 10,766 KB | 12,585 KB | 98,304 KB (Automatic with 47 MB of heap and 404 MB of temporary memory free) |
| The page's document | 4,555 KB | | |
| Each frame | about 3,100 KB | 4,500 KB | |
| The worker | 3,631 KB | 3,785 KB | 32,768 KB |

With "Script memory per page" set to Custom, 48 MB, the next load
showed a limit of 49,152 KB.

### Measured in the guest

Stock base, merged build, live Wikipedia:

| Reading | Free heap | Largest block | Free temporary memory |
|---|---|---|---|
| about:home after launch | 63,718 KB | | 405,265 KB |
| the article loaded | 45,531 KB | 35,885 KB | 401,738 KB |
| after closing its tab | 60,058 KB | 42,523 KB | |
| after loading and closing it a second time | 59,973 KB | | |

The 3.7 MB between the first and third readings is the network stack
that started in between (root store, cookies, blocklist state, 975 KB
of HTTP cache in memory) and slabs of the pooled allocator that still
hold a live block; a second load returned the heap to within 85 KB, so
pages are freed. The 3.5 MB of temporary memory is font data, which
stays loaded.

Stock base, 2026-11-15, a copy of the GitHub home page without its
scripts and with its 20 style sheets (2.5 MB), opened from the disc;
`main` at 1971abb against the style sheet work (Develop > Performance):

| Reading | `main` | With the style sheet work |
|---|---|---|
| Free heap, about:home after launch | 61.29 MB | 61.29 MB |
| Free heap, the page loaded | 33.64 MB | 48.94 MB |
| Largest free block | 29.58 MB | 46.63 MB |
| Free heap after 45 s of mouse moves (the texts packed) | 33.54 MB | 51.50 MB |
| Page load | 3.05 s | 1.57 s |
| Style | 683 ms in 4 passes | 349 ms in 3 passes |

The page takes 27.7 MB of heap on `main` and 9.8 MB with the work.
With the live page and its scripts the script heap (36 MB) decides the
reading, and the heap takes temporary memory when it is full, so the
two builds cannot be compared that way.

### Blocks over 2 KB

Measured on 2026-11-16, base `0477437`, before the pool served blocks
over 2 KB from slabs. To repeat it, count the blocks in `au_heap_alloc`
and `au_heap_free` with a temporary patch and load the page with
`AURA_HEAP=1` (above, "The pooled allocator").

Blocks over 2 KB that the host made while it loaded each page for 8
seconds and closed its tab (64-bit host; the script engine's arenas are
the same size on PowerPC):

| Size | GitHub repository page: made | GitHub: live at most | Wikipedia (PowerPC): made | Wikipedia: live at most | Hacker News: made | Hacker News: live at most |
|---|---|---|---|---|---|---|
| 2 to 3 KB | 7,167 | 273 | 3,520 | 72 | 235 | 11 |
| 3 KB to 4112 bytes (the script engine's 4 KB arenas are 3.6 KB to 4112 bytes) | 29,562 | 6,707 | 6,364 | 1,856 | 655 | 412 |
| 4113 bytes to 8 KB | 4,750 | 282 | 741 | 128 | 79 | 35 |
| 8 to 16 KB | 2,342 | 309 | 711 | 106 | 242 | 24 |
| 16 to 64 KB | 1,370 | 139 | 411 | 114 | 44 | 25 |
| over 64 KB | 421 | 70 | 172 | 47 | 80 | 23 |
| All | 45,612 | 7,601 at once | 11,919 | 2,263 at once | 1,335 | 518 at once |

- **Lifetimes.** Of the 38,051 blocks GitHub's page freed before its tab
  closed, 95.5% lived under 10 ms, 3.9% under 100 ms and 0.4% under a
  second. The script engine frees an arena as soon as its last block
  goes, so arenas are made and freed thousands of times; each was a
  `NewPtr` and a `DisposePtr` on Mac OS 9.
- **What outlives the page.** After the tab closed, 200 blocks over
  2 KB were still live: HTTP cache entries (`cache.c`, 66 blocks,
  0.4 MB), TLS connections kept open (`tls.c`, `tls_platform.c`, 81
  blocks), response headers and HTTP buffers.

What the pool held from the platform for GitHub's page (`--do heap`,
host), before the change, with 64 KB slabs for the classes over 2 KB,
and with 32 KB slabs (the change as merged):

| Reading | Before | 64 KB slabs | 32 KB slabs |
|---|---|---|---|
| Platform blocks, the page loaded | 7,917 (356 slabs, 7,561 large blocks) | 1,072 | 1,548 (1,348 slabs, 200 large blocks) |
| Bytes from the platform, the page loaded | 76,212 KB | 78,281 KB | 78,508 KB |
| Platform blocks, its tab closed | 443 | 323 | 339 |
| Bytes from the platform, its tab closed | 29,070 KB | 32,595 KB | 31,077 KB (393 KB of it empty slabs kept) |

The blocks that outlive the page were made while its arenas filled the
same slabs, so each pins a slab: 191 blocks held 89 slabs with 32 KB
slabs, 190 held 72 with 64 KB slabs. The Mac OS 9 application returns
the empty slabs it keeps every 10 seconds while no page loads
(`idle_all` in `main.c`); `aura-host` does not.

In the guest (Enforcer base, medians, [`performance.md`](performance.md),
"The pooled allocator's classes over 2 KB"):

| Reading, GitHub's repository page | Before | After |
|---|---|---|
| Free memory, the page loaded | 21.61 MB of 72.02 MB (largest block 19.06 MB) | 20.20 MB (largest block 17.75 MB) |
| Free heap, its tab closed | 57.40 MB (largest block 20.07 MB) | 56.60 MB (largest block 18.92 MB) |

## Cold memory

Memory held by things that are not looked at, or rarely read, is given
up or compressed. To change any of these, start at the place named.

| What | What happens | Where |
|---|---|---|
| A document kept for Back and Forward | frees its layout (boxes, lines, fragments, stacking data, measured words, font lists) when hidden and builds it again before the first paint on return | `au_docview_set_hidden`, `au_layout_release` ([`browser-chrome.md`](browser-chrome.md)) |
| A background tab | is unloaded when memory is short: its document is freed and loads again when the tab is selected | `browser.c`, "Unloaded tabs" ([`browser-chrome.md`](browser-chrome.md)) |
| The page source (Page Source, View Source, Save Page As) | is deflated once the load is over and inflated when asked for | `page_doc.c`, "The page source" |
| HTTP cache entries | hold the body as it arrived (gzip, deflate, brotli) and decode it when served | `cache.h`, `deliver_cached` in `http.c` ([`networking.md`](networking.md)) |
| A bundled font file over 2 MB (Noto Sans SC, 3.4 MB) | is paged: its tables (117 KB) are in memory, its glyph outlines are read from the file as they are drawn, with 4 blocks of 16 KB and 256 KB of glyphs kept | `fontpage.c` ([`fonts.md`](fonts.md), "Paged faces") |
| The text a parsed style sheet keeps | is deflated in 4 KB pieces once the page has loaded and five seconds have passed without a block parsed late; a piece is inflated when a block is parsed or the CSSOM reads the sheet | `au_docview_pack_sheets`, `css_sheet_text_at` ([`css.md`](css.md), "How a parsed sheet is stored") |

Computed styles stay with a kept document: an article's are 0.7 MB and
a full style pass is 12 ms on the host. Canvas bitmaps stay (they
cannot be made again). The DOM and the script heap are not compressed.

Measured on the host (64-bit, bundled fonts, live Wikipedia,
2026-11-15), before and after:

| What | Before | After |
|---|---|---|
| Layout held by a kept article (Mac (computer), 11,300 boxes) | 2548 KB | 0 (68 KB for the empty layout) |
| The same for a shorter article (Mac OS 9, 3000 boxes) | 874 KB | 0 |
| The kept article's estimate (`au_docview_memory_estimate`) | 22.5 MB | 20.8 MB |
| Back to a kept article, in the call | 4 ms (the shorter article; 7 to 10 ms recorded earlier) | 9 ms for the shorter article, 26 to 28 ms for the longer (22 to 24 ms of it building the layout) |
| A background tab's article after Unload Tab | 14.8 MB estimated; 16.0 MB live | 3 KB |
| Selecting the unloaded tab (reload from the HTTP cache) | | 550 ms |
| Page source of the article (1085 KB of HTML) | 1060 KB | 180 KB (deflated in 6 ms, level 1) |
| HTTP memory cache after loading the article (46 entries) | 4362 KB | 870 KB |
| Live bytes with the one article loaded | 35.7 MB | 31.3 MB |

In the guest (stock base, QEMU, 2026-11-15, live Wikipedia; the
unload threshold was raised for the test, see
[`browser-chrome.md`](browser-chrome.md)):

| Point | Free heap | Free temporary memory |
|---|---|---|
| one tab, Mac OS 9 article loaded | 45,560 KB | 401,938 KB |
| two tabs, PowerPC loaded | 29,552 KB | 401,805 KB |
| three tabs, Mac (computer) loaded; the two background tabs were unloaded as it loaded | 34,703 KB | 401,717 KB |
| the first tab selected (it loads again in 1.5 s; the third, now in the background, is unloaded) | 40,498 KB | 401,717 KB |
| the same tab after going to another article and Back (one kept document, 12.0 MB estimated) | 29,498 KB | 401,717 KB |

- Back to the kept article took 48 ms in the engine there (10 ms hiding
  the page left, 31 ms building the layout again) and the page was
  painted 220 ms after the key.
- The page source of the Mac (computer) article (1060 KB) was held in
  179 KB and compressed in 90 ms; the Mac OS 9 article's 303 KB in
  53 KB, 14 ms.

### The deflate encoder

`src/codec/deflate.c` compresses to raw deflate, zlib or gzip
(`codec.h` describes the interface): `au_deflate_all(fmt, level, data,
len, &out)` for a buffer, or `au_deflate_new`, `au_deflate_run(z, &in,
&in_len, &out, &out_len, finish)` and `au_deflate_free` for a stream.
Level 1 is greedy matching with short chains, level 2 lazy matching;
blocks are stored, fixed or dynamic Huffman, whichever is smallest. An
encoder takes about 300 KB while it lives.

To see sizes and speed, run `build/host/tests/codec/test_deflate
--bench FILE...`. Measured on the host (Apple silicon, `-O2`,
2026-11-15) on the 1085 KB article:

| | Size | Deflate | Inflate |
|---|---|---|---|
| level 1 | 183,659 bytes (16.9%) | 245 MB/s | 435 MB/s |
| level 2 | 159,419 bytes (14.7%) | 94 MB/s | 468 MB/s |
| zlib level 1, for comparison | 200,446 bytes (18.5%) | 230 MB/s | |
| zlib level 6 | 159,824 bytes (14.7%) | 89 MB/s | |

In the QEMU guest level 1 ran at 11.8 MB/s (1060 KB in 90 ms), a
twentieth of the host; no real G4 has been measured. By that ratio
level 2 would take about 230 ms for the article and inflating it 45 ms.

Tests: `tests/codec/test_deflate.c` (round trips through Aura's
inflate), `check-deflate.py` (the same streams decoded by Python's
zlib) and `fuzz_deflate.c` ([`fuzzing.md`](fuzzing.md)).

## What was cut on 2026-11-15

Style sheets ([`css.md`](css.md), "How a parsed sheet is stored"):

- A document parsed a sheet once for every element that linked it
  (11 MB of GitHub's 24.7 MB).
- A rule was five to seven heap blocks, and its records were two to
  three times the size they are now.
- Every block of declarations was parsed with the sheet; about one
  rule in eight of GitHub's sheets matches an element of its home page.
- `@media` blocks for other devices, the contents of false `@supports`
  rules and of `@container` rules were parsed and kept.
- A sheet linked several times was indexed and matched each time.
- A token list was three blocks, and a custom property that only names
  another one held a copy of its value.

Elsewhere:

- An inline formatting context kept room for 64 fragments and 16 lines
  whatever it used (3.4 MB of the article).
- The page source buffer kept its doubled capacity; the rule entry
  array too.
- An element's attribute array started at 4 entries.
- Every resolver parsed its own user agent sheets (now shared).
- The prelude's source and line tables in every runtime.
- Nested function source text.
- The memory cache duplicating the disk cache.

## Not done

See [`known-gaps.md`](known-gaps.md#memory).
