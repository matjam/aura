# Fuzzing

This document lists Aura's own fuzzers and Domato, a page generator
from outside the repository. It says how to run them, reproduce and
reduce a failure and keep a fixed bug fixed, and states the rules the
fuzzers enforce on new code. Read it before changing a parser or any
code a page's data reaches, and after a fuzzer reports.

Aura parses hostile data on a machine with no memory protection, so
every parser has a fuzzer, and so do the layers behind the parsers
(style, layout, paint, the DOM bindings). The fuzzers are deterministic
programs in `tests/*/fuzz_*.c`: each mutates a corpus (or generates
input) with a PRNG, takes an iteration count and a seed, stops at a
time cap and prints the iterations it did. ctest runs each for a few
seconds. For a real run, build the sanitizer tree (see
[`building.md`](building.md)) and run them from there.

## To run them all

Use `tools/fuzz-all.sh [SECONDS [JOBS [SEED [BUILD]]]]` (600 seconds
each, 4 at a time, seed 1, `build/asan`). It copies the programs to
`run/fuzz/bin` first, runs each with the corpus below, and prints a
table: exit status, iterations, distinct UBSan report sites,
AddressSanitizer errors. Each fuzzer's output is in `run/fuzz/NAME.log`
and its last input in `run/fuzz/NAME.input`. Give each run a new seed.

**Do not rebuild a tree while its fuzzers run:** macOS kills a process
whose program file is replaced (exit status 137 or 138 with no report).
Run long sessions from a copy, as `fuzz-all.sh` does.

## To run one

Use the command in the table. A fuzzer whose command has no SECONDS
takes its cap from `AURA_FUZZ_SECONDS`.

| Fuzzer | Covers | Command (after `build/asan/tests/`) | Corpus |
|---|---|---|---|
| `fuzz_html` | HTML tokenizer and tree builder: whole, in pieces, `document.write` with pauses, fragments | `html/fuzz_html N SEED SECONDS tests/page/samples/*.html tests/html/cases/*.dat` | sample pages, the `#data` sections of the tree-construction cases, built-in fragments |
| `test_html_prescan` | the `<meta charset>` prescan | `html/test_html_prescan N` | generated |
| `fuzz_css` | CSS tokenizer, parser, selectors, media and supports conditions, cascade; layout and paint of a small document under the sheet | `css/fuzz_css N SEED SECONDS tests/page/samples/*.css tests/css/cases/*.txt` | `wiki.css`, the CSS case files, built-in sheets with extreme values |
| `fuzz_layout` | style, layout at two sizes, paint, hit tests, mouse and keys, scroll damage, selection and find on mutated pages, script off | `page/fuzz_layout N SEED SECONDS tests/page/samples/*.html tests/page/samples/*.css tests/page/samples/*.gif` | sample pages, sheets and images, built-in layout snippets |
| `fuzz_page` | the same pipeline on generated documents (floats, tables, flex, deep nesting) | `page/fuzz_page N SEED` | generated |
| `fuzz_dom` | the DOM bindings with script on: generated scripts (tree edits, innerHTML, ranges, shadow roots, custom elements, observers, timers, listeners that destroy things during input events, workers, canvas drawing, iframes) with layout, paint, aimed input and hide-and-show cycles between steps | `js/fuzz_dom N SEED SECONDS` | generated |
| `fuzz_canvas2d` | the canvas 2D context: random drawing calls with extreme arguments | `canvas/fuzz_canvas2d N SEED SECONDS` | generated |
| `fuzz_svg` | XML parser, path grammar, the SVG image pipeline | `svg/fuzz_svg N SEED SECONDS tests/svg/corpus/*` | `tests/svg/corpus` |
| `fuzz_image` | PNG, JPEG, GIF, WebP, BMP and ICO decoders, whole and in chunks | `image/fuzz_image N SEED tests/image/vectors/*/*` (one format: `tests/image/vectors/png/*`) | `tests/image/vectors` |
| `fuzz_font` | sfnt tables, variations, COLR, GSUB, WOFF and WOFF2, paged faces (with short files and failing reads) | `paint/fuzz_font N SEED SECONDS tests/paint/fonts/*.ttf tests/paint/fonts/*.woff*` | `tests/paint/fonts` |
| `fuzz_webfont` | `@font-face` loading with mutated WOFF2 answers | `page/fuzz_webfont N SEED SECONDS` | `sample-inter.woff2` |
| `fuzz_url` | URL parser and serializer | `url/fuzz_url N SEED` | built-in URLs |
| `fuzz_net` | HTTP response parser, data URLs, PSL, root store, certificates, HSTS header and file, authentication, dates, MIME types, cache entries, WebSocket frames (whole and split, which must agree); `aia`: certificate bundles and the intermediates pool file; `tls`: the TLS handshake through `tls.c` | `net/fuzz_net all N SEED assets/net/aura-roots.bin`, `net/fuzz_net aia N SEED`, `net/fuzz_net tls N SEED SECONDS` | built-in seeds, the root store, test certificates |
| `fuzz_transfer` | the HTTP client and the loader on mutated responses from the fake transport: framing, chunked bodies, gzip, deflate and Brotli decoding, redirects, cookies, HSTS, the cache, sniffing | `net/fuzz_transfer N SEED SECONDS tests/codec/inflate-vectors/* tests/codec/brotli-vectors/*.br` | built-in response heads, the codec vectors as bodies |
| `fuzz_deflate` | the deflate encoder: made inputs compressed with random chunking and decoded again by Aura's inflate | `codec/fuzz_deflate N SEED tests/codec/brotli-vectors/page.html` (`AURA_FUZZ_SECONDS` caps it) | the codec originals |
| `fuzz_inflate`, `fuzz_brotli` | the decoders alone | `codec/fuzz_inflate N SEED tests/codec/inflate-vectors/*`, `codec/fuzz_brotli N SEED tests/codec/brotli-vectors/*.br` | the codec vectors |
| `fuzz_cookies` | Set-Cookie parsing, the cookie file | `net/fuzz_cookies N SEED` | built-in cookies |
| `fuzz_sse` | the EventSource event stream parser (`src/net/sse.c`): each stream parsed whole and in random pieces, which must agree; valid UTF-8 and the size limits in every callback | `net/fuzz_sse N SEED SECONDS` | built-in streams |
| `fuzz_webcrypto` | crypto.subtle's C side (`src/net/webcrypto.c`): mutated key data imported as every kind of key (RSA DER, EC scalars and points, X25519) and used; AES in four modes and PBKDF2 and HKDF with random lengths | `net/fuzz_webcrypto N SEED SECONDS` | keys generated at start |
| `fuzz_files` | Session, Bookmarks.html, History, Aura Preferences, Downloads, cookie rules, the disk cache folder, Script Hints, the script cache's Index | `browser/fuzz_files N SEED SECONDS` | files the stores write, made at start |
| `fuzz_blocklist` | list parser, compiled files, the store's index | `blocklist/fuzz_blocklist N SEED SECONDS` | generated |

## What counts as a failure

- a sanitizer report;
- a crash;
- an abort from the fuzzer's own checks (two parses that disagree, a
  transfer that does not finish, a count larger than its file allows);
- an iteration slower than `AURA_FUZZ_SLOW_MS` (`fuzz_css`,
  `fuzz_layout`, `fuzz_dom`: a hang is a bug);
- a stack overflow.

UBSan prints a report once per source line and goes on, so a run that
exits 0 can still hold reports: search the log for `runtime error`.

## To check incremental rebuilds against whole builds

To find a subtree rebuild or a targeted restyle that differs from a
whole one, run the fuzzers that change a document (`fuzz_dom`,
`fuzz_page`, `fuzz_layout`, Domato) from a tree configured with
`-DAURA_INCR_CHECK` ([`layout.md`](layout.md), "A DOM change"). Every
update that rebuilt a subtree is compared with a whole build, and a
difference aborts with the path of the first box that differs. The
check costs a whole build per update, so give the run a time cap and
expect fewer iterations. `fuzz_page` has no time cap of its own: run it
under `timeout` or kill it from a script.

To reduce a `fuzz_page` failure to a small case:

1. Run `fuzz_page N SEED FILE`. It writes each generated document to
   FILE before processing it and the generator's state to FILE.seed, so
   after a failure FILE holds the document that failed.
2. Run `fuzz_page process STATE FILE` to process FILE again with the
   mutations that STATE (the contents of FILE.seed) chose.
3. Run `tools/fuzz-reduce.py -t SECS -m TEXT FILE OUT fuzz_page process
   STATE`, where TEXT is part of the failure message. It writes the
   smallest document that still fails with TEXT to OUT.

## To find recursion over input nesting

Run the plain build (`build/host`) with `AURA_FUZZ_STACK_KB=512`: the
fuzzer then runs on a thread with that stack and prints the innermost
frames when it overflows.

- Mac OS 9 frames are smaller than the host's: 512 KB on the host is
  about what layout may use there, where the application's stack is
  1 MB and script may use 512 KB of it
  ([`mac-os-9-shell.md`](mac-os-9-shell.md)).
- ctest runs the HTML, CSS and layout fuzzers and their regression
  inputs this way in a plain build.
- Sanitizer frames are several times larger, so do not set it there.

## To reproduce a failure

Read the line the fuzzer prints (`fuzz: ... at seed S iteration I`) and
run the same command with `AURA_FUZZ_FIRST=I`, an iteration count of
I+1 and seed S: every iteration's choices come from the seed and its
number alone, in any build.

- `AURA_FUZZ_SAVE=PATH` writes each input to PATH before it is used
  (and the PRNG state to `PATH.seed`); `fuzz_X replay FILE` runs a
  saved input.
- To shrink one, run `tools/fuzz-reduce.py IN OUT COMMAND` (for example
  `tools/fuzz-reduce.py crash.css small.css
  build/asan/tests/css/fuzz_css replay`); `-m TEXT` keeps to failures
  that print TEXT, `-t SECONDS` sets what counts as a hang.

## To keep a fixed bug fixed

Add the reduced input to the fuzzer's `regress` folder; ctest replays
every file there.

| Fuzzer | Folder |
|---|---|
| `fuzz_html` | `tests/html/regress` |
| `fuzz_css` | `tests/css/regress` |
| `fuzz_layout` | `tests/page/regress` |
| `fuzz_dom` | `tests/js/regress/*.js` |
| `fuzz_transfer` | `tests/net/regress/transfer-*` |

Where a file cannot show the bug (a cost that grows with the square of
the input, a call sequence), write a test instead, as
`test_textarea_wrap_scaling` in `tests/forms` and `test_cut_sequences`
in `tests/html` do. Fix the engine, never the fuzzer's input.

- **A cost that grows with the square of the page:** add a pattern to
  `tests/layout/test_scaling.c`, which lays a generated page out at two
  sizes and fails when four times the content takes more than ten times
  as long ([`layout.md`](layout.md), "Costs that must stay linear").
- **A node used after a listener freed it:** add the event, what the
  listener destroys or the control to `tests/forms/test_input_lifetime.c`
  (below).

## To test input while listeners destroy things

`tests/forms/test_input_lifetime.c` loads a page of form controls with
one listener for one event that destroys something (the target, its
form, an ancestor, the body, the document, the frame), then drives one
control with the mouse, the keyboard and the Edit commands. It checks
the rule in [`conventions.md`](conventions.md), "Pointers across
script"; a failure is an AddressSanitizer report.

- Under ctest (`forms_input_lifetime`) it runs 1,323 of its 10,710
  cases (about a minute and a half in the sanitizer tree).
- After a change to `src/page/forms.c`, to the input paths of
  `docview.c` or `frames.c`, or to event dispatch, run all of it in the
  sanitizer tree with script garbage collected at every exit from
  script, one process per event:

  ```bash
  cp build/asan/tests/forms/test_input_lifetime run/
  seq 0 20 | AURA_TEST_GC_STRESS=1 AURA_TEST_VERBOSE=1 xargs -P 8 -I{} \
      sh -c 'run/test_input_lifetime all {} > run/lifetime-{}.log 2>&1 || echo "event {} failed"'
  ```

  Each event is 510 cases and took 187–308 s (5,458 s for all 21, about
  16 minutes of wall time six at a time on an 8-core Mac, 2026-11-15,
  clean). The last case a log names is the one that failed; run it again with its three numbers
  (`test_input_lifetime EVENT DESTROYER CONTROL`).

**Script garbage and freed nodes.** A script wrapper holds its node, and
a wrapper that is garbage in a reference cycle lives until the collector
runs, which on a small page is seldom: a node a listener removed can
stay allocated for a long time, and a pointer the engine kept to it goes
unnoticed. `au_js_set_gc_stress(true)` (`src/js/js.h`) makes every
runtime collect each time script is left. `fuzz_dom` turns it on for one
run in four, and `test_input_lifetime` with `AURA_TEST_GC_STRESS=1`.

## Rules the fuzzers enforce, for new code

- Convert a float that a page controls to an integer with `au_f2i` or
  `au_f2px` (`core.h`), not a cast: a cast of 1e38 or NaN is undefined.
- Do not let a function call itself once per level of markup, of
  templates in templates, or of combinators in a selector, without a
  depth limit; where a function that recurses has large local arrays in
  a helper, mark the helper `AU_NOINLINE` so they stay out of its frame
  (`finish_line` in `inline.c`).
- Do not size an allocation from a page's number without a cap
  (`AU_SHADOW_MAX_BLUR`).
- Do not measure a text prefix from its start in a loop over its
  characters; start from `l_prefix_seek`.
- Do not do work per line, per float or per box that grows with the
  paragraph or the page (an insertion into the middle of an array, a
  measurement of the rest of a text, a walk of every float): see
  [`layout.md`](layout.md), "Costs that must stay linear".
- Hold a reference to a node across any call that can run script, and
  check that it is still in the document afterwards
  ([`conventions.md`](conventions.md), "Pointers across script").

**To check the float indexes**, build a tree with
`-DCMAKE_C_FLAGS="-DAURA_FLOATS_CHECK -fsanitize=address,undefined"`
and run `fuzz_layout`, `fuzz_page`, `fuzz_css` and the layout cases in
it: every question to a formatting context's float list is then answered
by the index and by the plain walk, from the third float on, and a
difference aborts ([`layout.md`](layout.md)).

**Script that does not end** is stopped by its time limit, which the
engine checks once in ten thousand steps. A native that can take long
(it forces a layout, say) must ask `js_out_of_time` first and return
`js_throw_stopped` (`runtime.c`), and code that calls script on the way
out of a native must not do so while `js->root->interrupted` is set
(`window_frames` in `js/frames.c` dropped the interrupt and the loop
went on). See [`javascript.md`](javascript.md).

## Domato

Domato (Google Project Zero, Apache 2.0) generates HTML pages with CSS
and script from grammars of the DOM, CSS and SVG interfaces. Aura's own
`fuzz_dom` calls a fixed set of DOM functions; Domato reaches many more
interfaces and attribute values. Nothing of Domato is committed here.

- **To fetch it**, run `tests/domato/fetch-domato.sh`. It checks out the
  commit pinned in the script (`fadff396cc45`) into `build/domato`
  (git-ignored) and does nothing when that commit is already there. To
  move to another commit, change `PIN` in the script.
- **To run it**, build the sanitizer tree and run
  `tests/domato/run-domato.sh build/asan/tests/js/fuzz_domato build COUNT
  SEED SECONDS`. It generates COUNT pages into `build/domato-out/SEED`
  with `tests/domato/generate.py`, which seeds Python's `random` module
  before it runs Domato's `generator.py` (Domato has no seed option), so
  a seed gives the same pages again. `fuzz_domato` then loads each page
  through the document view with script on: the page is fed in pieces,
  its requests are refused, and its scripts, timers and animation
  frames run in steps with a forced layout, a paint, hit tests and mouse
  and key events between them, at two viewport sizes. It stops after
  SECONDS and prints the pages it ran. For a longer run by hand use, for
  example, 2000 pages and 600 seconds, with a new seed each time.
- **Under ctest** the `js_domato` test runs 30 pages of seed 1 for at
  most 60 seconds, and skips when `build/domato` is absent. The CI
  sanitizer job fetches Domato before its tests, so the test runs there.
- **What counts as a failure:** a sanitizer report, a crash, and a page
  whose steps take more than `AURA_FUZZ_SLOW_MS` (120 s by default) of
  CPU time. The runner gives script a clock that follows CPU time twenty
  times as fast, so the engine's ten-second limit on a script that does
  not end costs half a second.
- **To reproduce a failure**, run `build/asan/tests/js/fuzz_domato replay
  build/domato-out/SEED/fuzz-N.html`. With `AURA_FUZZ_VERBOSE=1` the
  runner prints each page's name before it loads it, so the last name
  printed is the page that failed. The runner writes its random state
  beside each page (`fuzz-N.html.seed`) and `replay` uses it. To shrink
  a page, use `tools/fuzz-reduce.py PAGE OUT build/asan/tests/js/fuzz_domato
  replay`.
- **To keep a fixed bug fixed**, reduce the page to a script and add it
  to `tests/js/regress` (replayed by `fuzz_dom`), or to
  `tests/page/regress` when it needs no script; do not commit Domato's
  pages whole.

### The pass before a release

Before a release, run at least 3,000 Domato pages over ten or more
seeds that have not been used, in the sanitizer tree, from a copy of the
runner (a rebuild must not replace it while it runs):

```bash
tests/domato/fetch-domato.sh
mkdir -p run/domato && cp build/asan/tests/js/fuzz_domato run/domato/
for seed in 201 202 203 204 205 206 207 208 209 210; do
    AURA_FUZZ_VERBOSE=1 tests/domato/run-domato.sh run/domato/fuzz_domato build 300 $seed 900 \
        > run/domato/seed-$seed.log 2>&1 || echo "seed $seed failed"
done
grep -l 'runtime error' run/domato/seed-*.log
```

- **Seeds.** Take seeds above the highest in the table below, and add a
  row for the pass. A seed gives the same pages again, so a used seed
  finds nothing new; a seed that found a bug is recorded with the bug,
  and its page can be made again (`tests/domato/generate.py build/domato
  OUT COUNT SEED`).
- **Time.** 300 pages take about 100 seconds in the sanitizer build on
  an 8-core Mac with four seeds running at once, so the ten seeds take
  about 6 minutes run four at a time (`xargs -P 4`) and about 15 one
  after another. The pages take about 150 MB per seed under
  `build/domato-out`; delete them afterwards.
- **A failure** is a seed whose log ends without `fuzz_domato: N pages`,
  or holds `runtime error` (UBSan goes on after a report). The last
  page the log names is the one that failed. Reproduce, reduce and add
  a regression as above, fix the engine, and run the seed again.
- Run `tools/fuzz-all.sh 120` with a new seed in the same session
  ("To run them all").

### Runs so far

Sanitizer build, macOS host (8 cores), pages of about 0.5 MB. Pages per
seed; "not recorded" where no log of the count was kept.

| Date | Seeds | Pages per seed | Found |
|---|---|---|---|
| 2026-11-15 | 1 | 200 | nothing |
| 2026-11-15 | 2 | 1,000 | nothing |
| 2026-11-15 | 23 | not recorded | page 197: an element freed by a focus listener and read by `au_forms_mouse` (`tests/forms/test_input_lifetime.c`) |
| 2026-11-15 | 29 | not recorded | page 79: inline layout that took minutes, `split_seg` ([`layout.md`](layout.md), "Costs that must stay linear") |
| 2026-11-15 | 31 | not recorded | nothing |
| 2026-11-15 | 101–107, 109–112 | 300 (79–141 s each, four at a time) | nothing |
| 2026-11-15 | 108 | 1, then 400 after the fix | page 0: `frame_show_html` parsed `srcdoc` text that the old document's unload listener had freed (`tests/js/regress/srcdoc-set-by-unload.js`) |
| 2026-11-15 | 113–122 | 400 (208–296 s each, four at a time) | nothing |
| 2026-11-15 | 123–124 | 400 (182 s each, two at a time) | nothing |

## Not fuzzed

TLS record parsing inside the TLS library beyond what `fuzz_net tls`
reaches through the handshake, the JavaScript engine itself
(QuickJS-ng), `fetch` and `XMLHttpRequest` bodies through script, the
Mac OS 9 shell (resource and Internet Config data), font files through
the Mac OS 9 loader, and the browser model under random user actions
(tabs, history, downloads).
