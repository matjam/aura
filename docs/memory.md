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

To see what parsed style sheets hold, and where, run `css_bench --mem`,
`--where` or `--page` ([`css.md`](css.md), "To measure").

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
`main.c`, which drops the kept documents; the out-of-memory handler
closes Open Transport before ExitToShell
([`mac-os-9-shell.md`](mac-os-9-shell.md)). Command-Option-M is the
out-of-memory test.

## The pooled allocator

On Mac OS 9, `au_alloc` takes small blocks from the pooled allocator
(`src/core/heap.c`, turned on by `au_heap_enable` at the start of
`main`): 32 KB slabs per size class, and one `NewPtr` only for a slab
or a block over 2 KB.

**What a block costs there.** Every block has a header of 8 bytes (the
slab and the size asked for), and its size is rounded up to its class:
steps of 8 bytes up to 128, then four classes per doubling up to 2 KB
(a 130-byte block takes a 160-byte slot). A block over 2 KB is a
`NewPtr` of its own, with the same 8-byte header and the Memory
Manager's. A slab is returned only when its last block is freed, so
many small long-lived blocks also pin slabs. Prefer one allocation for
records that live and die together: a parsed style sheet keeps its
rules and selectors in an arena for this reason
([`css.md`](css.md), "How a parsed sheet is stored").

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

The style sheet work that followed (below, "Parsed style sheets")
changed the GitHub column: CSS is 6036 KB, the sheets' text adds
2499 KB to the document view's row, and the total live is 19,126 KB.

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
sheets: what the sheets of a page hold once the page is styled, on the
host and, from the struct layouts, with 32-bit pointers. Before that
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
| The sheets' text | 2559 KB | 2559 KB | 220 KB | 220 KB |
| Value trees | 20 KB | 16 KB | 14 KB | 12 KB |
| Custom property and `var()` token lists | 473 KB | 396 KB | 48 KB | 41 KB |
| Sheet records, keyframes, font faces | 29 KB | 21 KB | 0.4 KB | 0.2 KB |
| Total | 5.3 MB in 4300 blocks | 4.3 MB | 0.59 MB in 480 blocks | 0.46 MB |

Of GitHub's 16,400 rules, 14,200 blocks of declarations and 280
`@media` blocks are still text after the page is styled. For the page
as the browser loads it (`AURA_MEM=1`, host), `src/css` went from
24.7 MB in 347,000 blocks to 6.0 MB in 19,600, plus the 2.5 MB of sheet
text that the document view used to free; the picture painted is the
same, byte for byte. [`css.md`](css.md) has the figures step by step
and the times.

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
