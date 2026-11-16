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
newlib's byte-wise `memcpy`) before tuning the engine.

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

- The Wikipedia load builds the whole tree 4 times on both: no subtree
  rebuild applied during that load, and why was not traced. The page
  load times differ by the network.
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
GitHub's own code most of the rest; Aura's natives are under 5%. Not
measured in the guest.

## Things that were slow, and why

These are the causes found so far; look for the same shapes first.

- **One `NewPtr` per block.** Retro68's `malloc` cost tens of seconds
  per layout on a large page ([`memory.md`](memory.md)).
- **Restyling the whole document** for a hover change, a keystroke or a
  resize, on pages with `:has()` rules ([`css.md`](css.md),
  [`forms.md`](forms.md)).
- **Building the box tree again** when an image's size arrived, an
  attribute the box tree never reads changed, or script inserted or
  removed a node ([`layout.md`](layout.md), [`forms.md`](forms.md)).
- **Repainting the page to scroll** ([`browser-chrome.md`](browser-chrome.md)).
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
