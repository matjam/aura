# Fuzzing notes: Domato

The Fuzzing section of the development notes lists Aura's own fuzzers.
This file covers Domato, a generator from outside the repository.

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
- **State on 2026-11-15:** the fetch script, the generator wrapper, the
  runner, the ctest registration and the CI step are in. The runner was
  tried on the sample pages and the scripted test pages (30 pages, no
  report). Domato itself has not been fetched or run yet: fetching and
  running a third-party program needs the owner's go-ahead, which the
  agent that wrote this did not have. The first runs, their triage and
  any fixes are still to do.
