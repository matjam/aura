# Memory

This document covers Aura's memory model: `au_alloc` and what happens
when memory runs out, the pooled allocator on Mac OS 9, the budgets of
each cache, how to measure on the host and in the guest, and what
things cost. Read it when Aura runs out of memory, before adding a
cache, or before changing `src/core/heap.c`.

The Mac OS 9 application runs in a 64 MB partition (12 MB at least,
`aura.r`). Font data, the window's GWorld and the blocklists go to
temporary memory when there is room; everything else comes from the
application heap through `au_alloc`.

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

To see what parsed style sheets hold, run `css_bench --mem`
([`css.md`](css.md)).

## To measure the Mac application

Choose Develop > Page Timings: it shows the free heap, the largest free
block and free temporary memory. Develop > Network Status shows the
bytes the HTTP cache holds in memory and on disk. Take the first
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

On Mac OS 9, `au_alloc` takes small blocks from the pooled allocator
(`src/core/heap.c`, turned on by `au_heap_enable` at the start of
`main`): 32 KB slabs per size class, and one `NewPtr` only for a slab
or a block over 2 KB.

Retro68's `malloc` is one `NewPtr` per block, and the Memory Manager
took tens of seconds per layout with the 90,000 blocks of a large page.
Do not call `malloc` in engine code, and enable the pool before the
first allocation in a new Mac OS 9 program. `tests/core/test_heap.c`
covers it on the host.

## Budgets

| What | Budget | Document |
|---|---|---|
| The application partition | 64 MB (12 MB at least) | |
| Kept documents | 3 per tab, 24 MB over all tabs; the oldest is dropped while less than 6 MB of heap is free | [`browser-chrome.md`](browser-chrome.md) |
| Background tabs | unloaded, least recently shown first, while less than 6 MB is free (heap and temporary memory together), or after 30 minutes in the background while less than 16 MB is free | [`browser-chrome.md`](browser-chrome.md) |
| The image cache | 8 MB per document by default; one image at most half of it | [`images.md`](images.md) |
| A decoded image | 16 MB | [`images.md`](images.md) |
| The HTTP memory cache | 8 MB by default; 1 MB resident with the disk cache on | [`networking.md`](networking.md) |
| A page's script runtime | 32 MB | [`javascript.md`](javascript.md) |
| A worker | 24 MB, 256 KB of stack | [`workers.md`](workers.md) |
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

With the system fonts, `/usr/bin/time -l` reports a peak footprint of:

| Page | Now | Before |
|---|---|---|
| a blank tab (fonts, PNG and surface are all of it) | 31.7 MB | 31.3 MB |
| the Wikipedia article | 49.2 MB | 52.2 MB |
| the GitHub page without its sheets | 38.7 MB | 42.1 MB |
| the GitHub page with them | 78.2 MB | 80.2 MB |

### A live Wikipedia article's script heap

It was 10.6 MB when the load ended: function records 3.9 MB (bytecode
0.4 MB, the rest mostly source text held once per nesting level),
objects 1.6 MB, properties 1.2 MB, atoms 0.8 MB, the prelude's own
objects about 1 MB. Sharing the source text brought it to 8.7 MB; four
seconds later the page's lazy modules have raised it to 11.3 MB, and
the idle collection takes 0.6 MB off. The kept-document estimate for
the article fell from 15.3 MB to 12.9 MB.

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

## Cold memory

Memory held by things that are not looked at, or rarely read, is given
up or compressed. To change any of these, start at the place named.

| What | What happens | Where |
|---|---|---|
| A document kept for Back and Forward | frees its layout (boxes, lines, fragments, stacking data, measured words, font lists) when hidden and builds it again before the first paint on return | `au_docview_set_hidden`, `au_layout_release` ([`browser-chrome.md`](browser-chrome.md)) |
| A background tab | is unloaded when memory is short: its document is freed and loads again when the tab is selected | `browser.c`, "Unloaded tabs" ([`browser-chrome.md`](browser-chrome.md)) |
| The page source (Page Source, View Source, Save Page As) | is deflated once the load is over and inflated when asked for | `page_doc.c`, "The page source" |
| HTTP cache entries | hold the body as it arrived (gzip, deflate, brotli) and decode it when served | `cache.h`, `deliver_cached` in `http.c` ([`networking.md`](networking.md)) |

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
