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

## To watch a page's memory while you use it

Choose View > Show Memory Use. The status bar's right end (on GTK the
status line's, on macOS a bubble over the page's bottom right corner)
then shows the active page's script runtime, its workers and its
decoded images against their limits, measured about once a second
(`au_bw_memory_text`, from `au_page_memory_use`):

```
Script 12.3 / 96 MB   Workers 2.1 MB (1 of 64 MB)   Images 8.0 / 64 MB
```

A limit set to Unlimited shows no "/ limit" part. The preference is
`view.show_memory`; it is off by default. For every runtime, document
and worker in detail, use Develop > Performance or System Status
([`developer-tools.md`](developer-tools.md)).

## To lift a page's limits

In Preferences > Content, set "Script memory per page", "Script memory
per worker" or "Image memory per page" to Unlimited (stored as -1,
[`javascript.md`](javascript.md), "Script memory limits"). The page
can then take what Aura has free; the engine still refuses script
memory when the machine runs short (`au_memory_low`), and on Mac OS 9
an unlimited page can leave other tabs with little.

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

## To see where blocks sit in the Mac's heap

Run `aura-host` with `AURA_HEAP=mac` and add `--do heapmap:FILE`:

```bash
AURA_HEAP=mac:200 build/mem/src/platform/host/aura-host chrome --bundled-only --net \
  --tab about:blank --do newtab:URL --do wait:8000 --do heapmap:run/loaded.txt \
  --do closetab --do wait:3000 --do heapmap:run/closed.txt
```

- The pool's platform blocks then come from a region of 200 MB (the
  number after `mac:`, 160 by default) placed as the Mac's `NewPtr`
  places them: the lowest free space that fits. The long-lived zone
  goes outside the region, as the Mac application keeps it in temporary
  memory ("Zones" below). `mac:200:top` puts it at the top of the
  region instead, and `mac:200:flat` low with the page zone, as before
  the zones. `:noresize` (for example `mac:200:noresize`) stops large
  blocks from being resized in place ("The pooled allocator" below).
- `heapmap` gives the empty slabs back (as the Mac does when idle) and
  prints the free space, the largest free block, the blocks held, how
  far the page zone's blocks have reached, and the bytes held outside
  the region.
- FILE gets the map: one line per platform block in address order (its
  offset, zone, slab or large block, size, allocation number, "new"
  when it was made after the previous `heapmap`), and the free extents
  of 64 KB or more. In a tree built with `-DAURA_MEM_TRACK=ON` each
  block's line also names the three sites that hold most of its bytes,
  a lone small block shows its first bytes, and the file ends with the
  platform bytes per site.
- To find what splits the free space after a page closes, read the
  page-zone blocks between the free extents of the closed map.
- The host's structures are larger than the Mac's (64-bit pointers),
  so compare the placement of the two builds, not the megabytes with
  the guest's.

To see what stays allocated when everything is freed (leaks, and what
the program keeps for good), run the same tree with `AURA_MEM_EXIT=1
AURA_MEM_MIN=1`: `aura-host chrome` prints the table once more after it
has freed the browser, the network stack and the cache.

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

**Large blocks resize in place.** `au_realloc` of a block over 16 KB
that stays over 16 KB first asks the platform to resize it where it is
(`au_heap_set_resize`): `SetPtrSize` on the Mac, which never moves a
nonrelocatable block, and `au_region_resize` in the host's stand-in
heap. A growing buffer then needs no copy and leaves no hole below it.
Only when the space after the block is taken does the pool allocate,
copy and free. Blocks in temporary memory and the long-lived region are
not resized.

Measured on the host (2026-11-16, `AURA_HEAP=mac:200`, Wikipedia's
PowerPC article at 1024x768, 10 seconds; `:noresize` turns the resize
off): 80 large blocks were resized in place, the page's blocks reached
23.6 MB of the region instead of 25.1 MB, the largest free block while
the page was open was 184.3 MB instead of 182.1 MB, and the platform
was asked for 904 blocks instead of 1,143. `--do heap` counts the
blocks resized.

**Empty slabs.** A slab is returned only when its last block is freed.
The pool then keeps up to 8 empty small slabs and, for the classes
over 2 KB, one empty slab per class and 4 in all; the others go back to
the platform at once. `au_heap_trim` gives back the kept slabs too.
`au_alloc` and `au_try_alloc` call it when the platform has no memory,
before the release handler, and again after the handler has run.

**Long-lived blocks pin slabs.** One block that outlives a page keeps
its whole slab, so many small long-lived blocks hold more memory than
they use ("Blocks over 2 KB" below has the figures). Allocate such
blocks in the long-lived zone ("Zones" below), and prefer one
allocation for records that live and die together: a parsed style
sheet keeps its rules and selectors in an arena for this reason
([`css.md`](css.md), "How a parsed sheet is stored").

Retro68's `malloc` is one `NewPtr` per block, and the Memory Manager
took tens of seconds per layout with the 90,000 blocks of a large page.
Do not call `malloc` in engine code, and enable the pool before the
first allocation in a new Mac OS 9 program. `tests/core/test_heap.c`
covers it on the host.

## Zones

Every block belongs to the page zone or the long-lived zone. The page
zone holds what a page makes and frees with it: documents, styles,
layout, images, script heaps. The long-lived zone holds what outlives
pages: the network stack (the HTTP cache, connections and TLS, response
headers, cookies, DNS), fonts and their caches, atoms, the favicon
store, the browser's tabs, history and bookmarks, and the platform
shell. The pool never puts blocks of the two zones in one slab, and it
tells the platform the zone of each slab and large block it asks for
(`get`'s `how`, `core.h`).

**To choose the zone of new code:**

- A whole source file: add it to `AURA_LONG_LIVED_SOURCES` in
  `CMakeLists.txt` (every file of `src/net` is there). The file is then
  compiled with `AU_HEAP_ZONE=1`, and each `au_alloc`, `au_realloc`,
  `au_strndup` and `au_buf_*` call in it sets the zone first. Other
  files are in the page zone. The platform shells' targets set
  `AU_HEAP_ZONE=1` for all their files.
- Helpers that allocate for their callers (`src/core`, `src/codec`,
  `src/url`) are compiled with `AU_HEAP_ZONE_INHERIT`: they leave the
  zone alone, so their blocks take the zone of the caller's last
  allocating call.
- One long-lived block in a page-zone file (a cache in a static
  variable): `au_alloc_long`, `au_calloc_long`, `au_realloc_long`
  (`css/match.c`'s case-folding caches).
- Everything a call allocates, whatever files it reaches:
  `int h = au_heap_zone_hold(AU_HEAP_ZONE_LONG); ...;
  au_heap_zone_unhold(h);` (the favicon store's surfaces in
  `browser/favicon.c`, and a block of the user agent sheets parsed late
  in `css/parser.c`).
- `au_heap_zone_of(p)` says which zone a block is in.

A block in the wrong zone costs placement, never correctness: a
long-lived block in the page zone splits the free space a page leaves,
and a page block in the long-lived zone uses that zone's memory until
the page goes. To find one, use the heap map ("To see where blocks sit
in the Mac's heap" above) and read the page-zone blocks left between
the free extents after the page's tab closes.

**On Mac OS 9** (`heap_get` in `main.c`) the page zone's slabs and large
blocks are `NewPtr` blocks in the application heap, as before. The
long-lived zone's come from temporary memory: slabs are cut from
512 KB chunks (`LONG_CHUNK`, locked handles) by an `AuRegion`
(`src/core/region.h`, first fit, so freed space is used again), large
blocks get a handle each (receive buffers, TLS records and bodies come
and go with transfers, and in the chunks they left each chunk held by a
few slabs), and a chunk whose slabs are all freed goes back, except the
last one. When temporary memory has no room, a long-lived block comes
from the heap as a page's does. Develop >
Performance shows the zone's bytes and the temporary memory its chunks
take (the "Long-lived blocks" line).

So a closed page leaves the application heap as it found it: the only
nonrelocatable blocks there are the page zone's, and they go with the
page.

**What split the heap before.** Mapped on the host after GitHub's home
page closed (`AURA_HEAP=mac:512:flat`, 2026-11-16, base `81ca065`), the
blocks left among the free space were HTTP cache entries and their
bodies (`cache.c`, 70 platform blocks), response headers kept with them
(`headers.c`, 63), TLS records and contexts of idle connections
(`tls.c`, `tls_platform.c`, 50), HTTP connection buffers (`http.c`),
font caches (`sfnt.c`, `fontpage.c`), atom texts, `match.c`'s
case-folding caches (64 KB each), blocks of the user agent sheets parsed
late, favicon surfaces, and a 32-byte block each PNG decoder leaked.
Each pinned a 32 KB slab or sat alone between free extents.

**Measured on the host** (2026-11-16, base `81ca065`, 64-bit,
`--bundled-only`, live sites): the free space of a 200 MB stand-in heap
(`AURA_HEAP=mac:200`) and its largest free block, in KB. "Before" places
both zones low (`:flat`), as the heap did before the change (the slabs
are already per zone); "after" keeps the long-lived zone outside, as
the Mac application does. Each page was opened in a second tab beside
about:blank and closed after 8 seconds (YouTube 15).

| Reading | Before: free | Largest | After: free | Largest |
|---|---|---|---|---|
| about:blank | 202,452 | 202,075 | 203,719 | 203,577 |
| GitHub's home page loaded | 46,726 | 18,401 | 62,461 | 31,694 |
| its tab closed | 192,829 | 58,159 | 203,719 | 203,577 |
| YouTube's home page loaded | 107,847 | 92,824 | 116,279 | 113,405 |
| its tab closed | 197,040 | 106,901 | 203,719 | 203,577 |
| ten pages opened and closed in turn (the list under "Atoms") | 186,907 | 47,734 | 203,110 | 188,569 |

- After GitHub's and YouTube's tabs closed, the stand-in heap is as it
  was before they opened.
- The long-lived zone outside held 1.3 MB at the start, 16.7 MB with
  GitHub's page loaded, 10.7 MB after it closed and 16.2 MB after the
  ten pages (the host's HTTP memory cache keeps 8 MB, and the host
  reads font files whole).
- What still splits the space after the ten pages is what MDN's CSS
  page leaks: computed style groups, custom property maps, token lists
  and text nodes that stay allocated after everything is freed
  (`AURA_MEM_EXIT=1`; [`known-gaps.md`](known-gaps.md#memory)).

**Measured in the guest** (2026-11-16, Enforcer base, 640x480, base
`81ca065` against this change with the temporary memory pins, one run
each): Develop > Performance's "Free memory" line, in MB. The change's
run went launch, GitHub, YouTube, Wikipedia, ten pages in one session.
The base build hangs when YouTube follows GitHub ("Temporary memory
areas" below), so its YouTube rows are from a fresh boot and its ten
pages from another.

| Reading | Base: free | Largest block | This change: free | Largest block |
|---|---|---|---|---|
| about:home at launch | 60.92 of 71.83 | 60.83 | 61.50 of 75.15 | 61.38 |
| GitHub's home page loaded | 7.21 of 100.97 | 1.02 | 5.78 of 111.26 | 1.09 |
| its tab closed | 57.47 of 79.45 | 11.50 | 61.47 of 111.26 | 60.97 |
| YouTube's home page loaded | 8.09 of 103.25 | 0.22 | 11.61 of 111.26 | 0.66 |
| its tab closed | 58.38 of 103.25 | 11.10 | 61.47 of 111.26 | 60.97 |
| ten pages opened and closed in turn (the list under "Atoms") | 54.84 of 76.81 | 14.53 | 60.66 of 111.26 | 53.57 |

The long-lived zone held
1.54 MB in 1,681 KB of temporary memory at launch, 3.05 MB in 6,596 KB
after GitHub's tab closed and 4.72 MB in 7,673 KB after the ten pages.

## What grows from page to page

The live bytes left after each page closes grow over the first ten
pages and then stop. What grows is in the long-lived zone, and each part
has a bound:

| What | Where | Bound |
|---|---|---|
| The HTTP memory cache: entries, response headers, bodies | `src/net/cache.c`, `headers.c` | 8 MB of cost on the host (each entry costs 256 bytes, its headers and its body; the newest entry is always kept); 1 MB resident on the Mac when the disk cache is on ([`networking.md`](networking.md)) |
| Idle connections: TLS contexts, records, receive buffers | `tls.c`, `tls_platform.c`, `http.c` | 16 connections, 6 per host, closed after 30 seconds idle; about 1 MB |
| Font files read on first use | `src/platform/posix/fontdir_posix.c` | the Fonts folder (8.3 MB with the bundled fonts); each file is held at its size |
| Web storage | `src/net` | about 1 MB |

**To find a leak among them**, run with `AURA_MEM_EXIT=1`: after
`aura-host` frees everything at exit it prints the blocks still
allocated by site. In `src/net` and `src/platform/host` only 7 blocks
(8 KB) remain, the TLS tables kept for the life of the program, so
neither leaks.

The leaks it found elsewhere and the fixes:

- The PNG decoder did not free its pixel block (`destroy` in
  `src/image/png.c`), 32 bytes or more per image.
- A fetch that a page's script started while the page was being freed
  stayed in the fetch list with its URL (`au_page_free` in
  `src/browser/page_doc.c` now cancels and compacts the list after the
  document view goes). YouTube's page did this.
- Not fixed: MDN's CSS page leaves about 40 KB
  ([`known-gaps.md`](known-gaps.md#memory)).

The host also held memory that a bound did not explain: cached bodies
and font files were grown by doubling and kept the slack (up to twice
their size). `au_cache_commit` now copies a body into a block of its
size, and `au_platform_read_file` reads a file of known size into a
block of that size.

Measured on the host (2026-11-16, base `81ca065`, the ten pages under
"Atoms" opened and closed in turn), in KB after the tenth close:

| | Before | After |
|---|---|---|
| Live bytes | 15,350 | 13,570 |
| `src/net` | 7,910 | 7,860 |
| `src/platform/host` | 6,800 | 4,760 |

The same ten pages twice (twenty closes, after the change): the live
bytes were 12,144 KB after the fifth close, 13,370 KB after the eighth,
and between 12,680 and 13,710 KB from the tenth to the twentieth;
`src/net` stayed between 6,980 and 8,000 KB and `src/platform/host` at
4,762 KB from the eighth close on.

## Atoms

Atoms (`au_atom`, `src/core/core.c`) are freed with the pages that made
them. Each top-level document view takes an atom scope
(`au_atom_scope_new` in `au_docview_new`; a frame's view uses its top
view's, `au_dv_use_scope_of`) and enters it in every public
`au_docview_*` function that can intern (`DV_ENTRY` in
`docview_int.h`). Every atom interned or looked up meanwhile gets the
scope as a holder; `au_docview_free` frees the scope last, and the
atoms only it held are freed, their numbers used again and the table
made smaller.

- **To keep an atom past the page that made it** (a table every
  document reads, a cache), intern it with `au_atom_permanent`, or
  enter no scope around the code (`au_atom_scope_enter(0)`):
  `au_css_init`, the user agent sheets, the initial values and the
  attribute lists of `match.c` do this. Code that runs in no scope
  (the browser layer, the host tools, the tests) makes every atom it
  touches permanent, so a missed entry point costs memory, never an
  atom freed while it is in use.
- **A cache that returns atoms it keeps** calls `au_atom_use` on each
  (the caller's scope then holds it) and empties itself when
  `au_atom_generation` changes (`css_atom_lower`, `css_hash_atom`).
- **A structure that several pages share** keeps the set of atoms it
  refers to (`AuAtomSet`, collected with `au_atom_collect_begin`) and
  each page that takes it holds them (`au_atom_set_hold`): a style
  sheet shared between documents does this ([`css.md`](css.md),
  "Sheets shared between documents").
- **Limits.** 31 scopes can be live; a document made while all are
  taken gets none and its atoms are permanent. A freed scope that is
  still entered holds nothing new: atoms made then are permanent.
- To see the counts, `aura-host chrome ... --do mem` prints a line
  `atoms: N live, M made at run time (P permanent), S scopes`.
- Tests: `tests/core/test_atoms.c` (ctest `core_atoms`) and
  `test_atoms_freed` in `tests/page/test_page.c` (three pages loaded and
  freed in turn leave the count where it was; a shared sheet's names,
  those parsed late included, stay while the document sharing it
  lives).

QuickJS's own atoms belong to the page's script runtime and are freed
with it: the script heap was 0 KB after each page closed in the
measurement below, and no QuickJS patch was needed.

**An element's computed style is freed with the element.** An element
that script removed from its document kept its `AuStyle` (with its
groups, custom property maps and value blobs) when it was freed: the
DOM did not know about styles, and `au_css_clear_document` reaches only
the elements still in the tree. GitHub's pages, whose script replaces
much of the page, left 29.8 MB of styles behind when their tab closed,
and held them while open. `node_free` in `dom.c` now calls the release
function src/css registers (`au_dom_set_style_release` in
`au_css_init`).

Measured on 2026-11-16, base `49d5f6e`, host (64-bit, bundled fonts,
`AURA_MEM=1`), loading GitHub's home page in a second tab for 8
seconds and closing it (`--tab about:blank --tab URL --active 1 --do
wait:8000 --do closetab --do wait:2000 --do mem`):

| After the tab closed | `49d5f6e` | This change |
|---|---|---|
| Live bytes | 40,372 KB | 9,921 KB |
| `src/css` | 29,828 KB in 168,891 blocks | 232 KB in 318 blocks |
| `src/core` (atoms and other core) | 718 KB in 10,878 blocks | 412 KB in 1,016 blocks |

Ten pages opened in a new tab and closed in turn (Wikipedia's PowerPC
article, Hacker News, GitHub's home page, lobste.rs, Wikipedia's Mac OS 9
article, GitHub's torvalds/linux, MDN's CSS page, Python's built-in
types page, w3.org, rust-lang.org; `newtab:URL`, `wait:8000`,
`closetab`, `wait:1000`, `mem` for each), with the style fix in both
columns, atoms permanent before and freed after:

| After closing page | Atoms made at run time, before | After | `src/core`, before | After | Live bytes, before | After |
|---|---|---|---|---|---|---|
| (none open) | 1,001 | 1,001 | 76 KB | 76 KB | 768 KB | 768 KB |
| 1 Wikipedia PowerPC | 4,012 | 1,022 | 301 KB | 220 KB | 9,195 KB | 9,135 KB |
| 2 Hacker News | 4,175 | 1,063 | 302 KB | 220 KB | 9,669 KB | 9,552 KB |
| 3 GitHub home | 16,521 | 1,063 | 1,390 KB | 412 KB | 14,268 KB | 12,728 KB |
| 4 lobste.rs | 16,807 | 1,063 | 1,393 KB | 412 KB | 14,212 KB | 12,165 KB |
| 5 Wikipedia Mac OS 9 | 16,904 | 1,063 | 1,395 KB | 412 KB | 14,664 KB | 12,948 KB |
| 6 GitHub torvalds/linux | 18,427 | 1,063 | 1,424 KB | 412 KB | 15,548 KB | 14,054 KB |
| 7 MDN CSS | 18,899 | 1,063 | 1,429 KB | 412 KB | 15,937 KB | 14,409 KB |
| 8 Python built-in types | 19,663 | 1,067 | 1,437 KB | 412 KB | 16,517 KB | 14,937 KB |
| 9 w3.org | 20,009 | 1,067 | 1,441 KB | 412 KB | 16,948 KB | 15,380 KB |
| 10 rust-lang.org | 21,677 | 1,067 | 1,457 KB | 412 KB | 17,110 KB | 15,537 KB |

The "before" column is the same build with every atom permanent. After
the change, the atoms made at run time stay at the ones the program
keeps (user agent sheets, initial values, caches), and `src/core` stops
growing. Of the 412 KB left, 400 KB is the atom array and hash table:
the array keeps its high-water size when a permanent atom made during a
large page sits near its end, because atom numbers cannot move. The
live bytes that still grow from close to close are in `src/net` (5.3 MB
after the third close, 6.8 MB after the tenth) and `src/platform/host`
(6.4 MB to 7.8 MB). Both are bounded caches, not leaks ("What grows
from page to page" below).

In the guest on 2026-11-16 (Enforcer base, 640x480, one run each),
Develop > Performance read as follows with `49d5f6e` and with the shared
sheets, freed atoms, the style fix and the lazy prelude modules
([`javascript.md`](javascript.md), "Modules read on first use"):

| Reading | `49d5f6e` used | This change used | Largest free block, `49d5f6e` | This change |
|---|---|---|---|---|
| about:home | 10.89 MB of 71.82 | 10.91 MB of 71.83 | 60.84 MB | 60.83 MB |
| two tabs of Wikipedia's PowerPC article | 41.26 MB of 75.65 | 40.94 MB of 75.67 | 33.22 MB | 32.60 MB |
| one Wikipedia tab, after a GitHub torvalds/linux tab closed | 34.68 MB of 76.28 | 34.16 MB of 76.30 | 4.83 MB | 4.70 MB |

The heap held about 0.3 to 0.5 MB less with the pages open. This is a
smaller gain than the host tables show; the guest reading was taken on
torvalds/linux rather than GitHub's home page, and a 32-bit build's
structures are smaller. The cause of the remaining difference was not
measured. After the GitHub tab closed, the largest free
block was under 5 MB in both builds: the heap was fragmented. The
zones fixed that ("Zones" above).

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
- In the guest (Enforcer base, 2026-11-16) GitHub's home page took
  29.9 MB of temporary memory for the heap and YouTube's 15.9 MB, and
  both pages ran.

### Temporary memory areas

Mac OS 9.2.2 can hang for good inside the Memory Manager when temporary
memory grows, shrinks and grows again. Aura's temporary memory starts in
one zone (71 MB on the 512 MB test machine, at 0x20001000); beyond it
the system makes more zones, and adds 16 MB areas when the first zone
is full. In the hang, one area's link to the next points to itself, and
the system, adding another area, follows that link without end. The
menu bar clock stops and nothing but a restart helps.

Seen in the guest (Enforcer base, 2026-11-16):

| Build | Sequence | Result |
|---|---|---|
| `81ca065` (base) | fresh boot, YouTube | runs |
| `81ca065` | YouTube, Wikipedia, GitHub, then YouTube again | hangs while YouTube loads |
| zones, no pins | fresh boot, GitHub, then YouTube | hangs while YouTube loads |
| zones, no pins | fresh boot, YouTube | hangs while YouTube loads |
| zones, no pins | a script page of 88 MB opened and closed 4 times, then 2 at once 3 times, then YouTube | the script pages run; YouTube hangs |
| zones, with pins | fresh boot, YouTube | runs |
| zones, with pins | fresh boot, GitHub, YouTube, Wikipedia, ten pages | runs |

The zones build puts the long-lived blocks in temporary memory too, so
temporary memory comes and goes more often, and YouTube's page alone was
enough.

**What Aura does.** `mac_temp_dispose` in `main.c` frees every temporary
memory handle the application takes. The first handle freed in each
zone other than the one temporary memory starts in is shrunk to 16 bytes
and kept until Aura quits, so no such zone empties because of Aura. Up
to 256 are kept; Develop > Performance shows how many ("Areas held"):
14 after launch (reading the Fonts folder), 109 after GitHub's page.
They cost little: after YouTube's page closed, 343 MB of temporary
memory was free with them and 349 MB with the base build.

**To find the hang in the guest**, sample the processor with
`tools/guest-qmp.py hmp "info registers"`: in the hang the instruction
address stays between 0x34f010 and 0x34f03c (system code), and the area
list, read with `x/11wx` from 0x2800 following the word at offset 0x20,
reaches an area whose word at 0x20 is its own address.

The cause inside the system was not found, and the pins were tested
with the sequences above only ([`known-gaps.md`](known-gaps.md#memory)).

## Budgets

| What | Budget | Document |
|---|---|---|
| The application partition | 64 MB (12 MB at least) | |
| Kept documents | 3 per tab, 24 MB over all tabs; the oldest is dropped while less than 6 MB of heap is free | [`browser-chrome.md`](browser-chrome.md) |
| Background tabs | unloaded, least recently shown first, while less than 6 MB is free (heap and temporary memory together), or after 30 minutes in the background while less than 16 MB is free | [`browser-chrome.md`](browser-chrome.md) |
| The image cache | per document: Automatic, an eighth of the memory free, 8 to 64 MB; or the `content.image_memory_kb` preference, 4 to 256 MB or Unlimited; one image at most half of it. Opaque still images are kept in 32, 16 or 8 bits (`content.image_colors`) | [`images.md`](images.md) |
| A decoded image | 16 MB | [`images.md`](images.md) |
| The HTTP memory cache | 8 MB by default; 1 MB resident with the disk cache on | [`networking.md`](networking.md) |
| A page's script runtime, shared by its frames | Automatic: half of the memory free, at least 8 MB, at most 96 MB (64 MB where the machine is unknown); or the `content.script_memory_kb` preference, 4 to 256 MB or Unlimited | [`javascript.md`](javascript.md), "Script memory limits" |
| A worker | Automatic: a quarter of the memory free, at least 4 MB, at most 32 MB (32 MB where the machine is unknown); or the `content.worker_memory_kb` preference, 4 to 256 MB or Unlimited. 256 KB of stack | [`javascript.md`](javascript.md), "Script memory limits"; [`workers.md`](workers.md) |
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

**A 9.8 MB script.** YouTube's player bundle, compiled alone on the
64-bit host (a tool that links QuickJS with the allocator counters),
2026-11-16, base e04ed48. Each row adds one change of patch 0013:

| Change | Compile peak | Held after the compile |
|---|---|---|
| none (base) | 75,831 KB | 40,567 KB |
| 5-byte source positions, phase 2 and 3 state out of `JSFunctionDef`, one trim per function | 62,779 KB | 41,411 KB |
| the compiler's blocks outside the arenas | 63,019 KB | 34,887 KB |
| no source text (scripts of 2 MB or more) | 54,979 KB | 25,351 KB |

At the base's peak, the parts were about: function definitions (552
bytes each on the host, 408 on PowerPC; 472 and 356 after) and their
jump and position tables 3.5 MB, source positions in phase 1 bytecode
3.2 MB, unused buffer space 6 MB, the source text 8 MB, and holes in
the arenas left by the compiler's freed blocks 6.5 MB of what stays
after. The page's results are in [`javascript.md`](javascript.md),
"Large scripts".

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
| Function source text (kept for `Function.prototype.toString`) | each text of 4 KB or more is deflated (raw, level 1) once a 5-second pass of its runtime finds it unread since the pass before, and inflated when `toString` (or a document compiling the same script) reads it; the deflated copy stays, so packing it again frees the inflated one | `js_code_expire` and `source_pack` in `src/js/runtime.c`, QuickJS patch 0017 ([`javascript.md`](javascript.md)) |
| The text a parsed style sheet keeps | is deflated in 4 KB pieces once the page has loaded and five seconds have passed without a block parsed late; a piece is inflated when a block is parsed or the CSSOM reads the sheet | `au_docview_pack_sheets`, `css_sheet_text_at` ([`css.md`](css.md), "How a parsed sheet is stored") |

Computed styles stay with a kept document: an article's are 0.7 MB and
a full style pass is 12 ms on the host. Canvas bitmaps stay (they
cannot be made again). The DOM and the script heap, apart from source
text, are not compressed.

Function source text on Reddit's home page (host, 2026-11-18, base
`0456b07`, 15 s after the load began): 4,411 KB in 22 texts before,
1,623 KB in 26 after. A text is bytes, so the PowerPC saves the same.

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

**Per-document zones were tried and dropped** (2026-11-17). The idea
was to split the page zone per document, so that a document kept for
Back holds its memory apart and freeing it leaves whole chunks:

- Each document view made a zone (14 ids) and entered it in its entry
  points (`DV_IN`).
- The pool cut that zone's slabs and blocks of up to 64 KB from chunks
  of its own (64 KB first, doubling to 512 KB, placed by an
  `AuRegion`).
- Blocks larger than that went to the platform one by one, as before.

It did not pay, for two reasons:

- **Peak memory.** A kept document's slabs and chunks keep the space
  its freed blocks left, and no other document can use it. At the
  fourth of four Wikipedia articles, the same 51.4 MB in use took
  67.7 MB from the platform with the zones and 59.3 MB without
  (+14%), and the page blocks reached 72 MB instead of 60 MB of the
  stand-in heap.
- **Blocks that outlive their document.** Anything a document's code
  allocates for the tab or for every tab pins that document's chunks
  after it goes. Before they were moved to the shared page zone, four
  kinds of block each kept 64 to 512 KB chunks of an ended zone:
  - shared style sheets (`au_css_sheet_parse_shared`);
  - blocks of those sheets parsed late (`css_rule_decls`);
  - the shared inflater of packed sheet text;
  - the tab's console lines and fetch list (`page_doc.c`).

  Every new cache of this kind would have to be found the same way.

Measured on the host (2026-11-17, base `5dd32fb`, `AURA_HEAP=mac:200`,
one build with the zones turned on and off, 1024x768). Three articles
were loaded (PowerPC, Mac OS 9, Power Macintosh G4), then the kept
documents were released as the release handler does, then QuickTime
was loaded and the kept one released again. Each cell is the free
space and the largest free block, in MB:

| Reading | Zones | No zones |
|---|---|---|
| third article loaded | 150.2, 147.9 | 161.6, 157.6 |
| kept documents released | 184.0, 147.9 | 184.0, 157.6 |
| fourth article loaded | 164.1, 148.5 | 171.3, 159.6 |
| kept document released | 181.9, 167.3 | 181.3, 159.6 |

The zones won only at the last reading; at the others the higher peak
left the free space smaller and more split. To split the heap less
after a kept document goes, look first at the blocks that outlive
their document in the page zone (the list above, found with the heap
map, "To see where blocks sit in the Mac's heap").
