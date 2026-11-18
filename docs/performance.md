# Performance

This document says how to measure Aura's speed on the host and in the
guest, why the two differ, and where each set of measurements is
recorded. Read it when something is slow, before you tune anything.
Memory has its own document, [`memory.md`](memory.md).

## Mac OS 9 is not the host

Host timings did not show the allocator problem that the pooled
allocator fixed ([`memory.md`](memory.md)): a layout pass that took
6 ms on the host took 11 s in the guest. When a guest figure is far
from the host's, look for what differs there (the Memory Manager,
the C library's copies, [`mac-os-9-shell.md`](mac-os-9-shell.md)) before
tuning the engine.

QEMU timings do not predict a G4's either: the guest's speed depends on
the host's TCG translation and varies with the host's load, so report
G4 and guest numbers separately. No real G4 has been measured.

## To measure on the host

Build a Release tree first (`cmake -S . -B build/rel -G Ninja
-DCMAKE_BUILD_TYPE=Release`, see [`building.md`](building.md)), and add
`--bundled-only` for reproducible fonts.

| To time | Run | Document |
|---|---|---|
| layout and painting of a page | `aura-host layout-bench PAGE --bundled-only` | [`layout.md`](layout.md) |
| the passes a resize costs | `aura-host chrome --tab PAGE --do resize:W,H` | [`layout.md`](layout.md) |
| scrolling | `aura-host chrome -w 1440 -h 1050 --tab PAGE --scroll-bench 20` | [`browser-chrome.md`](browser-chrome.md) |
| a keystroke in a field | `aura-host chrome --tab PAGE --do clickname:NAME --do keys:TEXT` | [`forms.md`](forms.md) |
| hover restyles | `build/host/tests/css/test_css_invalidate --bench [--verify] PAGE.html [SHEET.css ...]` | [`css.md`](css.md) |
| Back to a kept document, HTTP cache hits | `aura-host chrome --net --tab URL --do go:URL2 --do back --do cache` | [`browser-chrome.md`](browser-chrome.md), [`networking.md`](networking.md) |
| a page's style, layout, paint and script time | `aura-host chrome --net --tab URL --do perf` | [`developer-tools.md`](developer-tools.md) |
| whether repaints cover what changed | `aura-host chrome --net --tab URL --do wait:3000 --do damage:OUT.png` | [`layout.md`](layout.md) ("Repainting after script") |
| an image decode | `aura-host image FILE --repeat N` | [`images.md`](images.md) |
| SVG icons | `test_svg --bench` | [`svg.md`](svg.md) |
| canvas calls | `test_canvas2d --bench` | [`canvas.md`](canvas.md) |
| variable fonts, colour glyphs, WOFF decoding | `test_fontvar DIR --bench`, `test_colr DIR FONTS --bench`, `test_woff DIR --bench FILE` | [`fonts.md`](fonts.md) |
| script start-up and the benchmark | `aura-host js --stats FILE` | [`javascript.md`](javascript.md) |
| blocklist compile and lookups | `aura-host blocklist compile LIST.txt OUT`, `aura-host blocklist bench LIST.txt` | [`content-blocking.md`](content-blocking.md) |
| a fetch's phases and the TLS handshake | `aura-host fetch -v URL` | [`networking.md`](networking.md) |

To profile one step, run `layout-bench` with `--phase
relayout|resize|paint|paint-cold|strip --repeat N` under `sample
aura-host 3`.

## To profile a page's script on the host

1. Start the page in the background and sample it for the whole load:
   `aura-host chrome -w 1100 -h 1100 --net --profile run/NAME --tab URL
   --do wait:8000 --do perf > run/NAME.txt &`, then `sample $! 30 1
   -mayDie -file run/NAME.sample`. The Performance report at the end
   gives the script, style and layout totals.
2. Read the call tree under `au_docview_run_scripts`: compiling is
   under `compile_module` (modules) and `eval_classic` (classic
   scripts), Aura's natives are the `n_*`, `m_*` and `g_*` functions
   under `js_call_c_function`, forced layout is `au_docview_update`
   reached from a native, and the collector is `JS_RunGC`. What is left
   under `JS_CallInternal` is the page's own code and the prelude.
3. To time compiling alone, compile the page's scripts with a small
   program linked to `build/host/libaura_quickjs.a` that calls `JS_Eval`
   with `JS_EVAL_FLAG_COMPILE_ONLY` and times it with the thread's CPU
   clock (the machine's load moves wall-clock times by half).
4. For a change to the parser, also compare the bytecode before and
   after (`JS_WriteObject` of each compiled script): a change meant only
   to be faster must write the same bytes.

Machines shared with other builds give wall-clock figures that vary by
a factor of two; compare builds in alternating runs and report the
spread.

## To benchmark pages

To time a build's page loads in the guest, run `./build.sh ppc` and
then `tools/bench.sh`. To compare two builds, run it on each and then
`tools/bench-compare.py OLD.tsv NEW.tsv`.

- `tools/bench.sh` serves `tests/page/samples` with
  `tools/bench-serve.py`, boots a stock guest in `run-bench`, pushes
  `build/ppc/dist/Aura`, and types
  `about:bench?http://10.0.2.2:8642/manifest.txt` in the address field.
  Aura loads every page three times and posts a row per load to the
  server, which writes `run/bench/DATE-COMMIT-BASE-icount.tsv`. The
  script then quits Aura, stops the guest and prints the medians.
- The guest runs with `-icount shift=0,sleep=off`: its clock counts
  instructions, one per nanosecond, so script, style, layout and paint
  times depend on the code Aura runs and not on the host's speed or
  load. The times are not a G4's. `BENCH_ICOUNT=off` runs in real time.
- Network data still arrives on the host's clock, so load and first
  paint times vary between runs. Two runs of one build (b004ba8, stock
  base, two rounds, 2026-11-16) agreed within 5% on the totals of
  script, style, layout and paint time and on most pages' load times,
  but `qa-flex.html` loaded in 23.5 ms in one and 40.5 ms in the other.
  Compare builds on the script, style, layout and paint totals, and use
  more rounds (`BENCH_REPEAT=5`) to compare load times.
- The server sends every file with `Cache-Control: no-store`, so each
  load comes from the network in every round. The first round includes
  a cold start, and `bench-compare.py` leaves it out when there are
  three or more rounds.
- To benchmark other pages, set `BENCH_ROOT` to a folder of saved
  pages, or `BENCH_MANIFEST` to a manifest file. Keep saved copies of
  other people's sites out of the repository (`run/` is not committed).
  `BENCH_REPEAT` and `BENCH_SETTLE` set the rounds and the time each
  page gets after it loads.
- To run it on the host, start `tools/bench-serve.py --once` and run
  `aura-host chrome --net --tab about:blank --do
  'go:about:bench?http://127.0.0.1:8642/manifest.txt' --do
  wait:120000`.

`src/browser/bench.h` defines the manifest and the columns. Each row
holds the first paint and load times, the milliseconds in script,
style, layout, paint and screen copy (the Performance window's
counters, including the settle time), the passes counted, the bytes
and requests, the boxes, the image decodes, the script heap peaks and
the free memory.

## To measure in the guest

| To see | Use | Document |
|---|---|---|
| scene, fill and chrome timings | Develop > Tests > Paint Benchmark, whose table is in System Status | [`developer-tools.md`](developer-tools.md) |
| font registration time | Develop > System Status, the Fonts section | [`developer-tools.md`](developer-tools.md) |
| scrolling, repaints and keys, in milliseconds per step | Develop > Run Timing Test, whose table is in the Performance window | [`developer-tools.md`](developer-tools.md) |
| first paint, load, Back, style, layout, paint, script and screen copy time per page, the slowest tasks, free memory | Develop > Performance (Copy All) | [`developer-tools.md`](developer-tools.md) |
| cache lookups and cache file times | Develop > System Status, the HTTP cache section | [`developer-tools.md`](developer-tools.md) |
| the TLS primitives, DNS, TCP and handshakes | NetTest | [`networking.md`](networking.md) |
| the script engine's start-up and benchmark | JSTest | [`javascript.md`](javascript.md) |

## Where the measurements are

Each area's document holds its own figures, with the date and the base
where they were taken.

| Measured | Document |
|---|---|
| layout, relayout, painting, grid | [`layout.md`](layout.md) |
| hover restyles, sheet and style memory | [`css.md`](css.md) |
| Back to a kept document | [`browser-chrome.md`](browser-chrome.md) |
| a keystroke in a field | [`forms.md`](forms.md) |
| canvas calls and the sample animation | [`canvas.md`](canvas.md) |
| TLS handshakes, the size of the application | [`tls.md`](tls.md) |
| blocklist compile, lookups and what blocking saves | [`content-blocking.md`](content-blocking.md) |
| script start-up, prelude bytecode | [`javascript.md`](javascript.md) |
| workers | [`workers.md`](workers.md) |
| frames | [`frames.md`](frames.md) |
| the Page Source text view | [`developer-tools.md`](developer-tools.md) |
| memory per subsystem, script heaps, the guest's free heap | [`memory.md`](memory.md) |
| disk cache writes (about 9 ms each in the guest) | [`networking.md`](networking.md) |

## Scrolling on Reddit, 2026-11-17

Measured on the host (Release, base `2943372`) with `aura-host chrome
-w 640 -h 480 --net --load-ms 8000 --tab https://www.reddit.com/
--scroll-bench 60`, signed out, two runs each (they agreed within
10%). Paint time per step in ms, and pixels painted per step.

| Step | Before: ms | Pixels | After: ms | Pixels |
|---|---|---|---|---|
| 32 pixels, by repainting the page | 6.0 | 255,625 | 4.1 | 255,625 |
| 32 pixels, by moving pixels | 11.2 to 26.6 | 496,679 | 1.3 | 73,729 |
| page down, by moving pixels | 10.2 to 11.9 | about 297,000 | 4.1 to 4.9 | about 255,000 |

- Two faults made moving pixels cost more than repainting. The fixed
  toast container's damage covered the view twice
  ([`browser-chrome.md`](browser-chrome.md), "Scrolling by moving
  pixels"). Each painted rectangle also painted every transformed box
  it crossed whole, into a layer the box's size: `au_canvas_layer_end`
  and the images in those layers were 96% of the paint time in a
  profile.
- What a 32-pixel step paints now: the exposed strip, the sticky
  header, and a fixed button at the bottom right.
- The page itself still costs much at load: about 1.7 s of script on
  the host and a script heap at its 64 MB limit (Develop > Performance
  showed 8 s of script, 1.8 s of style and 4 s of paint signed in, in
  the Cocoa application).

## Script and style on Reddit, 2026-11-18

Measured on the host (Debug, base `e0587f0`) with `aura-host chrome -w
1024 -h 768 --net --load-ms 15000 --tab https://www.reddit.com/ --do
wait:3000 --do perf`, signed out. Runs differ by up to 20%, because the
page's script depends on network timing.

| | Before | After |
|---|---|---|
| Script during the load | 5.9 s | 2.7 to 2.9 s |
| Style during the load | about 1.2 s | 0.9 to 1.1 s |
| Rule index builds, in a `sample` profile | 275 samples | 84 samples |

- Script: one regular expression, `/[\n ]$/`, ran about 48,000 times
  on 10 KB strings. QuickJS tried it at every position; patch 0016
  tries an end-anchored pattern only near the end
  ([`javascript.md`](javascript.md), QuickJS patches).
- Style: each new component's shadow root rebuilt the rule index and
  restyled the whole document. The index is now kept when the sheet
  lists are ones it has, and a change made before the restyle is
  targeted ([`css.md`](css.md), "Shadow trees and scoped styles").
- `naturalWidth` and `naturalHeight` read the image cache and no longer
  force a layout.
- Still open: two script tasks of about 450 and 850 ms, and about ten
  full restyles a load, each for a `<style>` or `<link>` the page adds
  to the document (since targeted: "Restyles on Reddit" below).

## Restyles on Reddit, 2026-11-18

Measured on the host (Debug, base `a4c4424`) with `aura-host chrome -w
640 -h 480 --net --load-ms 1000 --tab https://www.reddit.com/`, ten
Page Downs 800 ms apart, then `--do perf`; two runs each.

| | Before | After |
|---|---|---|
| Style time | 990 to 1,060 ms | 776 to 778 ms |
| Elements styled | 205,000 to 217,000 | 158,000 to 166,000 |
| Layout boxes at the end | 4,300 to 5,200 | 9,900 to 12,900 |

The new build got two to three times further down the feed in the same
time. Each cause restyled the whole document (about 8,000 elements),
and each now restyles only what it reaches ([`css.md`](css.md)):

- A `<style>` inserted into the document: about 40 a load.
- A replaced adopted sheet (`replaceSync`) let the resolver go of every
  sheet, so each change script made before the next pass was a full
  restyle.
- More than 24 class names changed between two passes. Elements that
  script built off the tree filled the list; only connected elements
  count now, and the list holds 96.
- A change to the sheets of `shreddit-app`'s shadow root restyled its
  flat subtree, which is the page.

To find what restyles a page, count `au_css_resolver_styled_count`
around each `au_css_restyle` and log where `full_restyle` is set.

## Repainting after script on Reddit, 2026-11-17

Measured on the host (Debug, base `22a4ced`) with `aura-host chrome -w
1024 -h 768 --net --load-ms 15000 --tab https://www.reddit.com/ --do
wait:3000 --do perf --do wait:5000 --do perf`, signed out. The host
paints after each idle call when anything was invalidated, as a shell
does.

| | Before | After |
|---|---|---|
| Paint during the load | 14,023 ms, 1,198 paints | 4,827 ms, 454 paints |
| Paint in the 15 s after it | 7,418 ms, 641 paints | 0 ms, 0 paints |

- Each script turn that changed the document invalidated the whole
  page, and Reddit's script ran about 60 turns in a few seconds after
  load, each one a paint of the whole window. Most of those turns
  restyle elements to the values they already had.
- A top-level document now updates once after the script slice and
  repaints only the boxes whose paint changed
  ([`layout.md`](layout.md), "Repainting after script").
- Still open: one task takes about 800 ms. (The heap was at the host's
  64 MB limit because that limit was not doubled for 64-bit pointers; at
  128 MB it holds 47 MB.)
- Later the same day, at 1280 x 900, the idle page painted the whole
  window about 70 times a second (366 million pixels in 5 s): the
  reCAPTCHA badge frame repainted its box after every script turn, and
  the window merged that with the rest. Nested documents now settle
  with the top one; the same 5 s paint nothing.

## DOM changes: subtree rebuilds, 2026-11-16

To repeat these, build a Release tree and run, for the typing page,
`aura-host chrome -w 1440 -h 900 --tab tests/page/bench/typeahead.html
--do clickname:q --do keys:macintosh`; for Wikipedia's suggestions,
`aura-host chrome -w 1440 -h 900 --net --tab
https://en.wikipedia.org/wiki/Mac_OS_9 --do wait:4000 --do
clickname:search --do wait:3000 --do perf --do keys:m --do wait:1500`
and so on for each letter of "macint", then `--do perf` again (the wait
lets the suggestions arrive; the difference between the two reports is
the typing); for a load, `--tab URL --do wait:3000 --do perf`.

Host, Release, base `889cb4a` against the branch with subtree rebuilds
(`Subtree rebuilds` is the new line in the Performance report and in
the `keys:` output). Network pages vary between runs.

| Measurement | Base 889cb4a | Subtree rebuilds |
|---|---|---|
| typeahead.html, per key (9 keys) | 3.87 ms in the event; 958 elements styled, 1 tree build, 1 layout | 3.08 ms; 25 elements styled, 0 tree builds, 1 subtree rebuild of 33 boxes, 1 layout |
| Wikipedia suggestions, 6 keys with waits | 69 style passes, 71,859 elements styled, 14 tree builds, 15 layouts; style 61 ms, layout 39 ms | 68 style passes, 18,371 elements styled, 0 tree builds, 54 subtree rebuilds (1,087 boxes), 14 layouts; style 3 ms, layout 7 ms |
| Wikipedia Mac_OS_9 load | page load 271 ms; 4 tree builds; style 22 ms, layout 6 ms | page load 171 ms; 4 tree builds, 0 subtree rebuilds; style 21 ms, layout 6 ms |
| GitHub home, first 60 s (still busy) | 63 style passes, 126,327 elements styled, 61 tree builds; style 1,125 ms, layout 127 ms | 61 style passes, 48,149 elements styled, 10 tree builds, 49 subtree rebuilds (31,280 boxes); style 503 ms, layout 66 ms |

- The Wikipedia load built the whole tree 4 times on both. One build
  was the first layout, one the end of parsing (the parser's insertions
  are not recorded as DOM changes), and the rest came from style sheets
  that arrived later: the document view built the whole tree after any
  restyle of the whole document. It now builds it only when the DOM
  changed and otherwise compares styles (`au_layout_styles_changed`).
  Measured again (2026-11-16, three runs): page load 160 to 174 ms, 3
  tree builds, 0 subtree rebuilds, layout 5 to 6 ms. The late sheets
  then need only a relayout, or nothing. The third build is an image or
  script change recorded at `<html>`, which has no rebuild root but the
  whole tree; on some runs it is a subtree rebuild of `<body>` instead
  (3,439 boxes), by the order the resources arrive. The page load times
  differ by the network.
- A key on typeahead.html still invalidates the whole page (1,191,469
  pixels): the update changed geometry, and the view invalidates the
  page for that.
- Not measured in the guest.

## Script on GitHub's repository page

Measured on 2026-11-16 on the host (RelWithDebInfo, Apple silicon,
other builds running), base `0464028`, with `aura-host chrome -w 1100
-h 1100 --net --tab URL --do wait:8000 --do perf`; three alternating
runs of each build.

Where the base's 2.5 to 3.5 s of script went on
https://github.com/matjam/aura, from `sample`:

| Part | Share of script time |
|---|---|
| Compiling modules | 72% (25.5 MB of source compiled for 5.1 MB of modules: the 980 KB code view module 22 times, `core` 9 times) |
| GitHub's own code, the prelude, promise jobs | about 15% |
| Forced style and layout from `N.viewport` (`innerWidth`, `scrollY`, `getBoundingClientRect`) | 9% (of the time after the first fix) |
| Parsing selectors for `matches` and `querySelector` | 6% (of the time after the first fix) |
| Collector | 2 to 5% |

What each change gained, in script milliseconds on GitHub's page:

| Change | Before | After |
|---|---|---|
| Each module compiled once while its graph loads (patch 0010) | 3,464 | 878 |
| The parser's bracket look-ahead recorded (patch 0011) | compiling the 77 modules: 299 | 266 |
| Scoped variables found through the hash table (patch 0012) | compiling the 77 modules: 256 | 245 |
| `N.viewport` without the document's size | 9% of script in forced layout | none there |
| Parsed selectors kept | 24,000 selector parses, 6% of script | 6 samples of 345 |

The three pages, base against all the changes:

| Page | Base: script ms | After: script ms |
|---|---|---|
| https://github.com/matjam/aura | 2,473 to 2,683 | 511 to 559 |
| https://en.wikipedia.org/wiki/PowerPC | 125 to 138 | 110 to 112 |
| https://news.ycombinator.com/ | 0 to 5 | 1 to 4 |

After the changes, compiling is about 45% of GitHub's script time and
GitHub's own code most of the rest; Aura's natives are under 5%. In the
guest the page's script took 9.3 to 10 s, and about 3 s of it was the
allocator ("The pooled allocator's classes over 2 KB" below).

## The pooled allocator's classes over 2 KB

Measured on 2026-11-16 in the guest (Enforcer base, 640x480), base
`0477437` against the branch with the classes over 2 KB (`ca03101`),
in alternating runs. Each run launched Aura, opened a new tab, loaded
https://github.com/matjam/aura, read Develop > Performance 80 seconds
after the address was typed, closed the tab, read Develop > System
Status, and did the same for https://en.wikipedia.org/wiki/PowerPC.

To repeat it, build both, push one at a time with `tools/guest-push.sh`
into a guest of your own (`AURA_RUN`), and alternate. The host's load
moves every time in the guest: compare style time too, which the
change hardly touches, and leave out runs where it doubled.

GitHub's repository page, medians (base: 6 runs; the change: 3 runs,
2 more left out under load):

| Reading | Base | Classes over 2 KB |
|---|---|---|
| Page load | 10.57 s (9.70 to 13.29) | 9.15 s (8.60 to 10.23) |
| Script | 9,673 ms (9,279 to 13,337) | 6,850 ms (6,662 to 7,088) |
| Style | 1,087 ms | 1,315 ms |
| Free memory, the page loaded | 21.61 MB of 72.02 MB (largest block 19.06 MB) | 20.20 MB (largest block 17.75 MB) |
| Free heap, its tab closed | 57.40 MB (largest block 20.07 MB) | 56.60 MB (largest block 18.92 MB) |

The same page with 64 KB slabs for the classes (`f846bee`, 3 runs):
page load 8.63 s, script 6,833 ms, free memory 19.66 MB, free heap
after closing 55.64 MB (largest block 18.96 MB). The 32 KB slabs kept
the speed and pinned less.

The Wikipedia article (loaded second, so its "closed" reading follows
both pages), medians of 5 runs each:

| Reading | Base | Classes over 2 KB |
|---|---|---|
| Page load | 1.52 s | 1.40 s |
| Script | 1,754 ms | 1,442 ms |
| Style | 850 ms | 775 ms |
| Free heap, both tabs closed | 55.88 MB (largest block 20.30 MB) | 55.36 MB (largest block 18.90 MB) |

- Script on GitHub's page took 29% less time and the page loaded 13%
  faster: the script engine's arenas no longer go through `NewPtr` and
  `DisposePtr`, and the Memory Manager holds about 1,500 blocks for the
  pool instead of about 7,900.
- The heap holds 1.4 MB more while the page is loaded (blocks rounded
  up to their class) and 0.8 MB more after its tab closed (slabs
  pinned by blocks that outlive the page, [`memory.md`](memory.md),
  "Blocks over 2 KB"); the largest free block after closing was 1.2 MB
  smaller.
- Free temporary memory read about 2 MB lower with the change in every
  pair (3 to 5 MB with 64 KB slabs), although the heap took none from
  it (Develop > Performance showed no "taken by Aura" line). The cause
  was not found.
- The baseline given before this work (11.2 s, script 10.9 s, style
  3.2 s, 22.8 of 72.2 MB free) was taken under more load.
- Not measured on a real G4 or on the stock base.

## Things that were slow, and why

These are the causes found so far; look for the same shapes first.

- **One `NewPtr` per block.** Retro68's `malloc` cost tens of seconds
  per layout on a large page ([`memory.md`](memory.md)). The script
  engine's 4 KB arenas, one `NewPtr` each until 2026-11-16, cost GitHub's
  page about 3 s of script time in the guest.
- **Restyling the whole document** for a hover change, a keystroke or a
  resize, on pages with `:has()` rules ([`css.md`](css.md),
  [`forms.md`](forms.md)).
- **Building the box tree again** when an image's size arrived, an
  attribute the box tree never reads changed, or script inserted or
  removed a node ([`layout.md`](layout.md), [`forms.md`](forms.md)).
- **Repainting the page to scroll** ([`browser-chrome.md`](browser-chrome.md)).
- **Painting more than the dirty area**: an empty fixed container's
  extent reaching across the view, and transformed boxes painted whole
  into a layer for each strip ("Scrolling on Reddit" below).
- **Loading a page again for Back** ([`browser-chrome.md`](browser-chrome.md)).
- **Third-party scripts**: blocking them removes requests and bytes
  ([`content-blocking.md`](content-blocking.md)).
- **Compiling the prelude from source** at every runtime start
  ([`javascript.md`](javascript.md)).
- **Compiling a module again in every round of its graph's loading**:
  GitHub's code view module 22 times ([`javascript.md`](javascript.md),
  "Module scripts").
- **Work repeated per level of nesting** in the parser's look-ahead,
  and walks over every variable in scope (QuickJS patches 0011, 0012).
- **A native that forces layout for a value that needs none**
  (`N.viewport` for `innerWidth`).
- **Parsing the same selector at every call** of `matches`.

What is still slow is listed in [`known-gaps.md`](known-gaps.md).
