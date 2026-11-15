# Aura: development notes

`README.md` describes Aura for its users. Keep it short. This file holds
what you need to build, test and change Aura.

## Conventions

- C99, portable. Engine code (everything in `src/` outside
  `src/platform/`) makes no OS calls and includes no OS headers. It must
  build for the host and for PowerPC Mac OS 9 (big-endian, 32-bit, newlib).
- Do not recurse over document depth (tree walks, destruction, layout of
  nested boxes where it can be avoided): Mac OS 9 stacks are small and
  pages nest deeply. Use explicit loops or an explicit stack.
- Allocate with `au_alloc`, `au_realloc`, `au_free` (`src/core/core.h`).
  They do not return NULL. When malloc fails they first call the
  release handler (`au_set_release_handler`), which frees caches, and
  try again; the out-of-memory handler runs when nothing is left to
  free.
- On Mac OS 9, `au_alloc` takes small blocks from the pooled allocator
  (`src/core/heap.c`, turned on by `au_heap_enable` at the start of
  `main`): 32 KB slabs per size class, and one `NewPtr` only for a slab
  or a block over 2 KB. Retro68's `malloc` is one `NewPtr` per block, and
  the Memory Manager took tens of seconds per layout with the 90,000
  blocks of a large page. Do not call `malloc` in engine code, and
  enable the pool before the first allocation in a new Mac OS 9
  program. `tests/core/test_heap.c` covers it on the host.
- Text is UTF-8. Names (tags, attributes, CSS properties and keywords) are
  atoms. Names known at compile time go in `src/core/atoms_html.def` or
  `src/core/atoms_css.def`.
- Public names start with `au_` (functions), `Au` (types), `AU_`
  (constants).
- Third-party code is limited to QuickJS-ng (JavaScript) and BearSSL
  (TLS), in `third_party/`. Everything else is written here. Ask before
  adding another.
- Commit each working step to `main`. Commit only our own sources. Apple
  files, downloaded test suites and fonts stay out of git, except the
  redistributable bundled fonts in `assets/fonts`, which are committed
  with their licences.
- Write docs instruction first ("To X, use Y"), in plain technical prose.
- Commit messages follow Conventional Commits, because release-please
  works out the next version and the changelog from them: a type, a
  colon, then the subject as a plain sentence, as before. `fix:` a bug
  fix (bug-fix digit); `feat:` something a user of Aura can see or use
  (minor digit); `perf:` faster or smaller; `docs:` documentation only;
  `test:` tests only; `chore:` tooling, CI, merges and clean-up;
  `refactor:` no change in behaviour. `feat!:` or `fix!:` marks a change
  that breaks something users rely on (stored preferences that no longer
  load, say). Only `feat`, `fix`, `perf` and breaking changes appear in
  the changelog, and only they cause a release. A merge commit keeps a
  conventional subject too: `chore: merge main into grid`, or `feat:
  CSS grid layout` when the merge is what brings the feature to `main`
  and its own commits were not conventional.

## Layout

| Folder | Holds |
|---|---|
| `src/core` | allocation (and the pooled allocator), buffers, UTF-8, atoms |
| `src/dom` | the node tree |
| `src/html` | HTML tokenizer and tree builder |
| `src/css` | CSS parser, selectors, cascade |
| `src/layout` | box tree, layout, painting, hit testing |
| `src/page` | the document view: parsing, style sheets, resources, input, nested documents (`frames.c`) |
| `src/paint` | surfaces, rasterizer, fonts |
| `src/image` | image decoders (PNG and APNG, JPEG, GIF, WebP, BMP, ICO, SVG through `src/svg`), the per-document image cache |
| `src/svg` | XML parser, SVG path and transform grammars, the static SVG renderer, SVG documents |
| `src/browser` | browser model and chrome: tabs, history, bookmarks, downloads, preferences, the page interface and its implementation on the document view |
| `src/js` | JavaScript: the QuickJS-ng runtime wrapper, the event loop, the DOM and window bindings (C natives plus the `prelude*.js` files) |
| `src/url` | WHATWG URL parser and serializer, hosts (IPv4, IPv6, Punycode) |
| `src/codec` | streaming inflate (gzip, zlib, raw deflate) and Brotli decoders |
| `src/net` | HTTP/1.1 client, TLS glue, cookies, HSTS, PSL, cache, sniffing, encodings, the resource loader, blocklists |
| `src/platform/host` | host tools: headless render to PNG, tree dumps, `fetch`, BSD sockets for the network stack, `js` and `page` for scripts |
| `src/platform/macos9` | the Mac OS 9 application; Open Transport sockets, entropy and clocks for the network stack; `nettest/` |
| `src/platform/macos9/jstest` | JSTest, the JavaScript engine self-test and benchmark for Mac OS 9 |
| `src/platform/macos9/sea` | the installer: the extractor of the self-extracting archive (see CI and releases) |
| `third_party` | QuickJS-ng (`quickjs`, with `quickjs-patches` and `quickjs.cmake`) and BearSSL 0.6; `third_party/README.md` records sources and licences |
| `tests/<area>` | one test area per folder, each with a `CMakeLists.txt` |
| `tests/frames` | nested documents: the document view with a table of pages, and a browser window over the fake transport |
| `tests/js` | binding, loop, limit, lifetime and security scripts for `aura-host js`, scripted pages for `aura-host page`, the benchmark |
| `assets/fonts` | the bundled fonts, their licences and `fonts.idx` |
| `assets/net` | `aura-roots.bin`, the root certificate store |

`CMakeLists.txt` globs `src/**/*.c` into the `aura_engine` library and adds
every `tests/*/CMakeLists.txt`, so a new source file or test area needs no
change to the top-level file.

## Build and test

- `./build.sh test` builds for the host and runs the tests.
- `./build.sh ppc` builds for Mac OS 9 with Retro68 (GCC 16.1, Multiversal
  Interfaces). The toolchain is found through `RETRO68`, then in
  `~/.local/opt/retro68` (`toolchain/find.sh`). A `build/ppc` folder
  keeps the toolchain it was first configured with; delete it to switch.
  It also assembles the distributable folder `build/ppc/dist/Aura`: the
  application (`Aura.bin`, MacBinary) beside `Fonts` (the family folders
  of `assets/fonts`, `fonts.idx` and the fonts' README). Then it packs
  that folder as the self-extracting archive, `Aura-VERSION.sea.bin` and
  `Aura-VERSION.hqx` in `build/ppc/dist` (see CI and releases).
- To run the tests under AddressSanitizer and UBSan, configure a separate
  folder: `cmake -S . -B build/asan -G Ninja
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined"`, then `cmake --build
  build/asan` and `ctest --test-dir build/asan`.

## CI and releases

`.github/workflows/ci.yml` runs on every push to `main` and on pull
requests; `release.yml` makes releases. They need nothing outside the
repository: no disk images, no QEMU, no secrets.

- **Jobs.** `host` runs `./build.sh test` on Ubuntu (GCC) and macOS
  (Apple Clang). `sanitize` runs the tests on Ubuntu under Clang's
  AddressSanitizer and UBSan, with leak detection off (several tests do
  not free everything at exit; macOS, where the sanitizer build is run by
  hand, does not detect leaks either). `toolchain` builds Retro68 when
  the cache has none and `ppc` runs `./build.sh ppc` and uploads the two
  archives as artifacts named `Aura-VERSION-SHORTHASH-hqx` and
  `Aura-VERSION-SHORTHASH-sea-bin`; both are in `build-ppc.yml`, a
  reusable workflow.
- **The toolchain cache.** `~/.local/opt/retro68` is cached under the
  key `retro68-Linux-` plus the hash of `toolchain/build.sh`. To rebuild
  the toolchain in CI, change that script (the pinned commit, say). A
  cold build takes about 15 minutes; a newer push does not cancel it. A
  cache made on a branch serves that branch only; `main`'s serves every
  branch and pull request.
- **To make a release**, merge the pull request release-please keeps
  open ("chore: release VERSION"). `.github/workflows/release.yml` runs
  release-please on every push to `main`: it reads the Conventional
  Commits since the last release and opens or updates that pull request,
  which sets `AU_VERSION` in `src/browser/credits.h` (the line marked
  `x-release-please-version`), `version.txt`, `CHANGELOG.md` and
  `.release-please-manifest.json`. When the pull request is merged, the
  same workflow tags the merge commit `vVERSION`, creates the GitHub
  release with the changelog entry as its notes, builds the archive from
  that commit and attaches `Aura-VERSION.hqx` and
  `Aura-VERSION.sea.bin`. Do not push a tag or edit `AU_VERSION` by
  hand. To force a version, add `"release-as": "X.Y.Z"` to the package in
  `release-please-config.json` and remove it after that release.
- **The version** lives in one place, `AU_VERSION` (three numbers, with
  one-digit minor and bug-fix parts so that it fits the BCD byte of a
  `vers` resource). The About box, the user agent, both applications'
  `vers` resources (generated by CMake from `aura_vers.r.in` and
  `sea/sea_vers.r.in`), `tools/version.sh` and the archive's file names
  read it. A `feat:` raises the minor part, a `fix:` or `perf:` the last
  part; before 1.0 a breaking change raises the minor part.
- **release-please's settings** are `release-please-config.json`
  (release type `simple`; `bootstrap-sha` is the commit where changelog
  collection starts, so the history before Conventional Commits is not
  read; `initial-version` makes the first release 0.3.0; both stop
  mattering after the first release) and `.release-please-manifest.json`
  (the last released version, written by release-please). It needs the
  repository setting Settings > Actions > General > "Allow GitHub
  Actions to create and approve pull requests". A release made with the
  workflow's token starts no other workflow, which is why the build is a
  job of `release.yml` (it calls `build-ppc.yml`, as `ci.yml` does).
- **Actions are pinned** to a commit, with the release in a comment. To
  update one, look up the release's commit (`gh api
  repos/OWNER/REPO/commits/TAG -q .sha`) and change both.
- **To build the archive locally**, run `./build.sh ppc`. It writes
  `build/ppc/dist/Aura-VERSION.sea.bin` (MacBinary) and
  `Aura-VERSION.hqx` (BinHex 4.0) from `build/ppc/dist/Aura`. To pack
  another folder or stub, run `tools/make-sea.py DISTFOLDER OUT.bin
  [--stub AuraSEA.bin] [--date SECONDS]`, then `tools/binhex.py OUT.bin
  OUT.hqx`. `tools/make-sea.py --list OUT.bin` checks an archive and
  prints its entries and a SHA-256 of the unpacked contents;
  `tools/binhex.py --decode IN.hqx OUT.bin` and `--compare IN.hqx
  REF.bin` read BinHex back.
- **The archive** is one Mac OS 9 application, "Aura VERSION Installer"
  (`src/platform/macos9/sea`, creator `AuSX`). `make-sea.py` appends the
  packed folder to the stub's data fork, after the PowerPC code, and
  changes the stub's `cfrg` resource from "the code runs to the end of
  the fork" to the code's length, so that Mac OS does not load the
  archive with the code. Each file carries its path, type, creator,
  Finder flags, dates and both forks, each fork as a raw deflate stream
  with a CRC-32; `sea_archive.h` gives the layout. The packer maps host
  files as `tools/guest-push.sh` does (`.bin` is MacBinary, fonts become
  `tfil`/`AURA`, licences become SimpleText documents). Every date is
  the last commit's (or `SOURCE_DATE_EPOCH`), so one machine builds the
  same bytes from the same commit. A Linux and a macOS build of one
  commit differ in the application and the stub: the same code, with
  functions at other addresses.
- **The installer** (`sea.c`) asks for the folder with `StandardPutFile`
  (starting beside itself, with the first free name of "Aura", "Aura 2"
  and so on; it never replaces an item), writes the files through
  `sea_archive.c` with two fixed buffers (16 KB in, 32 KB out) and
  Aura's inflate decoder, sets Finder info and dates, checks every CRC,
  and sends the Finder an Open Documents event for the new folder. On
  an error or Command-period it deletes the folder it made and shows
  what failed. Its partition is 1 MB. `sea_archive.c` is portable:
  `tests/tools` builds `sea_extract` from it and checks it on the host
  against the packer, with damaged copies (`tools_sea_extract` in
  ctest), beside the self-tests of `binhex.py`, `make-sea.py` and
  `finder-flags.py`.
- **To test the archive in the guest**, push the `.hqx` with
  `tools/guest-push.sh` (it arrives as a StuffIt Expander text document)
  and open it: Expander is on the stock system, asks for a folder
  because the disc is locked, and writes the installer there. Or push
  the `.sea.bin`, which arrives as the application.

## Dependencies outside the repository

Aura depends on no other project's checkout. What it needs is installed
by its own scripts or kept in git-ignored folders here.

- **Retro68.** To install the toolchain, run `toolchain/build.sh`. It
  fetches Retro68 at the commit pinned in the script (a4a389c41e90,
  PowerPC only, Multiversal Interfaces) into `toolchain/Retro68`, builds
  in `toolchain/build`, and installs in `~/.local/opt/retro68` (or
  `$RETRO68`). It does nothing when that folder's `REVISION` names the
  pinned commit. To move to another commit, change it in the script,
  empty the install folder, and run the script. In CI, cache
  `~/.local/opt/retro68` keyed on the hash of `toolchain/build.sh`
  (the workflow does; see CI and releases).
- **QEMU.** To install the QEMU the test machine runs, run
  `qemu/build.sh`. It clones the release pinned in the script (v11.1.3)
  into `qemu/src`, applies `qemu/patches/*.patch`, and installs in
  `~/.local/opt/qemu-os9` (or `$QEMU_PREFIX`). It does nothing when the
  install's `REVISION` matches.
- **Disk images.** The test machine's base images are in `disks/`
  (git-ignored): `os9-novm.qcow2` is a stock Mac OS 9.2.2 with virtual
  memory off, and `os9-vmoff.qcow2` is the same system with Enforcer
  installed. Both are standalone qcow2 files with no backing file. To
  update Enforcer on the second, install a new Enforcer build in that
  guest with `PERSIST=1` and fold the overlay into the image.
- **hfsutils** (`hformat`, `hcopy`) for `tools/guest-push.sh`:
  `brew install hfsutils`.

The install scripts, the QEMU patch and the two disk images were copied
from the os9-enforcer project on 2026-11-14.

## Guest testing

Aura has its own test machine: a QEMU mac99 guest running Mac OS 9.2.2,
with outbound network access. Runtime files go in `run/` of the main
checkout (set `AURA_RUN` to use another folder, for example to run a
second guest beside the first); `tools/guest-env.sh` holds the settings.

To start the guest, run `tools/guest-run.sh start`. QEMU runs headless in
the background. `BASE` picks the system: `stock` (the default) is
`disks/os9-novm.qcow2`, Mac OS 9.2.2 with virtual memory off and no
Enforcer; `enforcer` (`BASE=enforcer tools/guest-run.sh start`) is
`disks/os9-vmoff.qcow2`, which has the Enforcer kernel and extension
installed. The guest boots an overlay on the base image
(`run/os9-overlay-BASE.qcow2`), so the base is never written. One guest
runs per run folder; `status` names the running base. Disk writes are
discarded when QEMU exits; set `PERSIST=1` to keep them in the overlay.
Mac OS takes about a minute to boot. `GEOMETRY` sets the largest screen
mode the guest offers (default `1024x768x32`; for example
`GEOMETRY=1440x1050x32`); Mac OS starts at 640x480 or the last mode
chosen, and the Control Strip's resolution module switches to it.

Test on the stock base first. Use the Enforcer base to check Aura under
Enforcer; its own bugs show there too (see the scroll bar thumb note
under Browser chrome). Enforcer's state and offense log are read with
that project's own tools, which Aura does not carry.

To copy the application in, build it with `./build.sh ppc` and run
`tools/guest-push.sh -n AuraTest build/ppc/dist/Aura/*`, which puts the
application and its `Fonts` folder side by side at the top of the disc
(push `build/ppc/src/platform/macos9/Aura.bin` alone to test without the
bundled fonts). A folder argument is copied with its subfolders; fonts
get type `tfil` and creator `AURA`, licences and READMEs become
SimpleText documents, and `.html`, `.css`, `.js` and `.idx` files are
copied byte for byte (hcopy's text mode would translate UTF-8 as
Latin-1). A `.hqx` file is copied byte for byte as a StuffIt Expander
text document, for the guest to decode. HFS names are at most 31 characters.
The files arrive as a CD named AuraTest on the guest's desktop. The
previous disc is ejected first through the Finder; the script types
nothing until the guest's current application (`CurApName`) is the
Finder, and fails if an application on the disc is still open, so quit
Aura before pushing a new build.

To launch it, run `tools/guest-qmp.py open AuraTest Aura`. To take a
screenshot, run `tools/guest-qmp.py screenshot run/NAME.png`. To type, use
`tools/guest-qmp.py key cmd-q` (any character works as the key, as in
`'cmd-['`) or `type TEXT`; to click or drag, use `click X Y` or `drag X1
Y1 X2 Y2` (guest screen pixels). `front-app` prints the current
application.

To compare the window with the host rendering, render the scene at the
window's size (the title shows it) and compare the content area, which
starts at 20,50 on the guest screen:

```bash
build/host/src/platform/host/aura-host scene -w 584 -h 390 -o run/host.png
tools/png-compare.py run/guest.png 20 50 run/host.png run/diff.png
```

To change the screen depth, click the Monitor BitDepth module in the
guest's Control Strip (the seventh icon, at 197,466 on a 640x480 screen)
and pick 256 Colors, Thousands or Millions. In Aura, Develop >
Benchmark times the scene, fills and the chrome and shows how long font
registration took (and the bundled part of it) and how much font data
is loaded; Develop > Font List (about:fonts) shows the fonts it
found, with the bundled ones marked; and Develop > Page Timings
shows the active page's first paint, load and layout times, image
decodes, then times scrolling: ten 32-pixel steps (twice: the first pass
paints content for the first time, the second shows the steady state),
five page downs and three repaints of the whole page, each split into
moving the offscreen pixels, painting, copying to the screen and the
number of areas painted, and shows free heap, temporary memory and the
glyph cache. Network Status shows the HTTP cache's lookups (fresh,
history, revalidated, 304, missed) and the time spent reading and
writing cache files.
The Develop menu is in the menu bar (its title is at 298,10 on a 640x480
screen): click the title, then click the item; the menu stays open
between the two clicks. While Aura is busy (a long layout), the click
is lost; wait and try again. The developer windows it opens are
described under Developer tools.

The Finder ignores `guest-qmp.py drag` (it moves in one step). To drag
a file in the Finder (onto the Aura icon, or a copy to the hard disk),
hold the button for most of a second and move in about ten steps of
0.15 s each, with the QMP class from `tools/guest-qmp.py`.

To open a local page in the guest, push it with the application (for
example `tools/guest-push.sh -n AuraTest Aura.bin
tests/page/samples/*`) and choose File > Open File, drop it on the Aura
icon, or type its path (`AuraTest:wiki.html`) in the address field.

To stop the guest, run `tools/guest-run.sh stop`, or choose Special > Shut
Down in the guest.

## Browser chrome

To change how a browser window looks or behaves, work in `src/browser`:
`chrome.c` lays out, paints and hit-tests the tab strip (or left tab
column), toolbar and status bar; `browser.c` holds windows, tabs and
back/forward lists and routes input; `page.h` is the interface a page
implements, and `page_doc.c` implements it on the document view
(`src/page`). The model never calls the OS; the shells pass events in and
show the rectangles it reports through `AuBrowserHost.invalidate`.

Pages load asynchronously: `au_page_navigate` only starts a load, and
`au_page_idle` reads files (the document, then its style sheets,
@imports and images in request order) and feeds the parser, at most
40 ms per call. While a document loads it is laid out at most every
300 ms (four times the last layout's duration for slow pages), then once
at the end. Generated about: pages (home, blank, fonts) load inside
`au_page_navigate`; about:scene paints the test scene directly. A URL
that differs from the current one only by its fragment scrolls instead
of loading, and back and forward restore the scroll position each entry
was left at. Networking comes through a fetcher per scheme
(`au_browser_set_fetcher`, `AuPageFetcher` in `page.h`); the Mac OS 9
shell registers `src/net/page_fetcher.c` for http and https, and
`aura-host chrome --net` does the same on the host. Without one, http and
https show a placeholder after a simulated load. Images are
decoded by `src/image` (see Images); `au_page_idle` advances animations.
The font preferences reach the engine as the generic families of
the font set and, for the default and monospace sizes, the minimum size
and the tab's text zoom, `au_docview_set_font_prefs` (which passes them
to `au_css_resolver_set_font_prefs`). Text zoom is per tab
(`au_bw_zoom`, View > Zoom In, Zoom Out, Actual Size); the last zoom
chosen for a site (host, or scheme for `file:`) is kept in the
`zoom.sites` preference and applied when a tab opens that site again.

Back and Forward show a kept document instead of loading the page again.
To change that, work in `page_doc.c` ("Kept documents") and `browser.c`
("The document cache"); `page.h` describes the interface.

- **What is kept.** When a tab leaves a document for another entry, the
  page moves the document view (tree, layout, images, script runtime,
  form contents) into its kept list and makes a new view for the next
  document. The move happens when the new document begins, so the old
  one stays on screen until then; a load that never begins (a download,
  a Stop) leaves it in place. `au_page_navigate_entry` for an entry
  whose document is kept shows it without a load and sets
  `AuPageStats.restored`.
- **Entries.** The browser names each back/forward entry with an id
  (`AuNavEntry.id`) and calls `au_page_set_entry` before every
  navigation (`entry_load` in `browser.c`). A navigation path that skips
  it gets documents that are never kept. `au_page_retain_entries` drops
  the documents of entries that are gone.
- **What is not kept:** a document still loading or with fetches under
  way, a failed load, an internal page, a POST result,
  `Cache-Control: no-store` on the main document
  (`AuFetchResponse.no_store`), and a window with an unload or
  beforeunload listener (`au_docview_can_persist`). A reload replaces
  the document. Reopen Closed Tab, duplicated tabs and session restore
  load through the history cache mode, as before.
- **Frames.** A kept document keeps its nested documents, and they
  pause and resume with it (see Frames).
- **While hidden** nothing calls the view, so timers, animation frames,
  image animation and the caret wait, and run again after Back. The
  view's callbacks and loader in `page_doc.c` ignore a view that is not
  the page's current one. `au_docview_set_hidden` fires pagehide and
  pageshow with `persisted` true, drops decoded image pixels (they
  decode again when painted) and runs the script collector.
- **Limits.** `cache.documents` (3 per tab, 0 turns the cache off) and
  `cache.documents_kb` (24 MB over all tabs, by
  `au_docview_memory_estimate`; a document larger than that is not
  kept). `au_bw_idle` evicts the oldest over the budget
  (`au_browser_trim_documents`). On Mac OS 9, `idle_all` in `main.c`
  drops the oldest kept document while less than 6 MB of heap is free,
  and `au_alloc` calls the release handler (`au_set_release_handler`,
  `release_caches` in `main.c`) to drop them all before the
  out-of-memory alert. Clear Cache in Preferences drops them too.
- **Measured on the host** (Wikipedia, live): Back to a kept article
  takes 7 to 10 ms; the same Back through the history cache mode takes
  450 to 800 ms. An article is estimated at 12 to 15 MB (its script heap
  is most of it), a very long one (World War II, 2 MB of HTML) at over
  30 MB, which is not kept.
- **Measured in the guest** (stock base, Wikipedia, live): Back to a
  kept article took 56 ms in the engine, 46 ms of it hiding the page
  left, and the page was painted 971 ms after the key; on screen the
  change showed about 3 seconds after the key was sent, because the
  page being left was still running its scripts. Loading the same
  articles took 30 to 70 seconds. Develop > Page Timings shows
  these times for a page that Back or Forward showed from the kept
  documents, with the number and estimated size of kept documents.
- **To try it on the host**, run `aura-host chrome --net --tab URL --do
  go:URL2 --do back --do cache`: `back` prints whether the kept document
  was shown and how long it took, `cache` the kept documents and their
  estimated size. `--no-doc-cache` turns the document cache off.
  `tests/browser/test_back_cache.c` covers it on the fake transport.

Keyboard focus follows one rule (`browser.c`, "Keyboard focus"): after
a navigation or a tab switch the page has the focus, unless the user is
typing in the address field; a tab showing a fresh blank page puts it in
the field. The model reports the move with `AU_CHANGED_FOCUS_PAGE` or
`AU_CHANGED_FOCUS_FIELD`.

A horizontal scroll bar appears along the bottom of the page area while
the active page is wider than it (`au_chrome_layout_hscroll`); the two
bars meet at a blank corner square above the grow box, and Shift turns
a vertical wheel scroll horizontal (Mac OS 9 itself reports no wheel).

To change Save Page As, see `au_page_source` (the page keeps the main
document's bytes as they arrived, until the next load), `au_bw_save_name`
(the suggested file name) and `mac_save_page` in `storage.c`, which asks
with `StandardPutFile` and writes the bytes unchanged as a TEXT file with
creator AURA. Open Location selects the whole address again when the
field already has the focus, and a paste into the field turns line
breaks into spaces.

Back and Forward: a Command-click opens that entry in a new tab behind
(`au_bw_go_new_tab`); holding the button for half a second (30 ticks)
shows a pop-up of the entries that way (`au_bw_history_items`,
`history_popup` in `browserwin.c`). File > Reopen Closed Tab
(Shift-Command-T, mapped in `key_down` because MenuKey ignores Shift)
brings back the last of up to `AU_MAX_CLOSED_TABS` closed tabs with
their history and scroll position. While a page loads, the status bar
shows its progress in the middle (`AuChromeState.progress_text`). Each
window's position goes into the session (`au_bw_set_position` after a
drag or zoom) and a restored window returns there when its title bar
is still on screen.

To change the About box or about:licences, edit the credits table in
`src/browser/credits.c` (the version, the tagline, and one entry per
bundled font, third-party library and data set with its licence);
`aboutbox.c` and `build_licences` in `page_doc.c` both read it. Data
under the MPL (the root store, the Public Suffix List) carries the MPL
notice and where its source form is. The table has a commented slot for
QuickJS-ng: add it there when the library joins the build. The About box (DLOG 202) draws
icon family 128 at 64 pixels with `PlotIconID`; its empty `dctb` gives
it a colour port, without which the icon draws in black and white.

To change how Aura works with Internet Config, edit
`src/platform/macos9/internetconfig.c`. Mac OS 9.2.2 has the Internet
Config component ('PREF', 'ICAp') but no InternetConfigLib shared
library (GetSharedLibrary fails with -2804 in the stock guest), so
Retro68's InternetConfigLib stub cannot be used: Aura opens the
component with `OpenDefaultComponent` and makes each call through
`CallComponentUPP` with a parameter block, checking every selector with
`kComponentCanDoSelect` first. `internetconfig.h` declares what the
Multiversal Interfaces lack (the IC records, in 68K alignment, and the
Component Manager calls). Addresses Aura does not open itself
(`au_browser_opens_url`) reach the shell through
`AuBrowserHost.open_external`, which calls `ICLaunchURL`. The GURL Apple
event opens its URL in a new tab, or a new window with
`links.external_new_window`. Preferences > General has the Make Aura
the Default Browser button, the only place Aura writes Internet Config
(the http, https and file helpers). `mac_ic_download_folder` and
`mac_ic_map_filename` are there for downloads. To send a GURL in the
guest, run `open location "URL"` in Script Editor.

To change what a contextual menu offers, edit `src/browser/context.c`:
`au_bw_context_menu` builds the items for a window point (link, image,
page, address field or tab, using `au_page_hit_info` for the page) and
`au_bw_context_command` runs the commands the model can; the shell does
the rest (clipboard, field editing, a new window). On Mac OS 9,
`contextmenu.c` finds the Contextual Menu Manager in the ContextualMenu
shared library at run time (the Multiversal Interfaces do not declare
it) and falls back to `PopUpMenuSelect`; Control-click opens the menu,
and so does holding the button still on the page for 45 ticks.

To change text selection or find, work in `src/layout/selection.c`
(positions, words, paragraphs, the text of a range, find, and the
painted highlight; `draw.c` calls `l_select_paint_frag` for each text
fragment while a selection is set) and in the selection section of
`page_doc.c`, which turns mouse events into selections: a drag selects
characters, a double click a word, a triple click a paragraph, Shift
extends, and a drag that selects does not follow the link it started
on. `au_page_find` searches the page's text with simple case folding
(ASCII, Latin-1, Greek, Cyrillic, Latin Extended-A), from the
selection, wrapping. The Mac OS 9 shell counts clicks within the
double-click time, copies with `mac_put_scrap` (TEXT in Mac Roman with
transliteration and carriage returns, plus 'utxt'), asks for the text
with the Find dialog (DLOG 201), maps Shift-Command-G to Find Previous
itself (MenuKey ignores Shift), and paints selections in the
Appearance highlight colour (`LMGetHiliteRGB`, passed in
`AuBrowser.highlight`).

To render a browser window on the host, run
`build/host/src/platform/host/aura-host chrome -w 800 -h 600 --tabs left
--tab about:home --tab file:tests/page/samples/wiki.html -o
run/chrome.png`. `--tab` takes a URL, a path or `file:` with a relative
path; `--scroll Y` scrolls the active tab and `--mouse X Y` moves the
pointer (hover, a link's status text). Run `aura-host` without arguments
for the other options (tab overflow, hover, address field text).

Scrolling moves pixels instead of repainting the page. To change it,
work in these places:

- `browser.c` (`page_scrolled`): a scroll by the user (`au_bw_scroll_to`
  and everything built on it: wheel, arrows, page keys, the scroll bars)
  calls `AuBrowserHost.scroll_area` with the page area and the distance,
  then invalidates the strip that scrolled in and the rectangles
  `au_page_scroll_damage` returns. Script scrolls, scrolls of at least
  the page's size and hosts without `scroll_area` repaint the page.
- `au_page_scroll_damage` (`page_doc.c`, `au_docview_scroll_damage`,
  `au_layout_scroll_damage` in `draw.c`) returns the rectangles that do
  not move with the page: fixed boxes (old and new place), sticky boxes
  whose offset changed, and their shadows and outlines; or -1 when the
  whole page must be repainted (layout pending, a fixed or gradient
  canvas background, more than `AU_PAGE_SCROLL_DAMAGE` rectangles, or a
  viewport box under a transformed ancestor). Layout records the fixed
  and sticky boxes while building stacking contexts (`view_boxes`).
- `present.c` (`AuPresent`) is the shells' damage list: up to 8
  rectangles merged by least added area, and the pending shift.
  `au_present_begin` moves the surface's pixels and hands over the
  rectangles to paint; the shell then moves the screen (Mac OS 9:
  `ScrollRect`, then `CopyBits` from the GWorld into the region it
  reports) and copies the painted rectangles.
- Painting skips boxes outside the canvas clip (`au_layout_paint`), so a
  strip paints only what it shows; box and text ink extents (shadows,
  outlines, inline SVG with visible overflow) must cover what they draw.
- Hover follows a scroll only after the scroll pauses for
  `AU_REHOVER_MS` (150 ms). The delay dates from when a hover change
  restyled the whole document on pages with `:has()` rules (1.7 s per
  scroll step on Wikipedia in the guest); a hover change now restyles
  about two elements there (see CSS), and the delay has not been
  measured again in the guest.

`tests/browser/test_scroll_shift.c` scrolls sample pages and inline
pages with fixed, sticky and transformed boxes both ways and compares
the moved result with a full repaint pixel for pixel; `SHIFT_DUMP=1`
writes both images when they differ. To time scrolling on the host, run
`aura-host chrome -w 1440 -h 1050 --tab PAGE --scroll-bench 20` (add
`--bundled-only` for reproducible fonts): it times 32-pixel steps and
page downs by repainting, then by moving pixels (`--no-shift` skips
that), through the Mac shell's paint path, with pixels painted and
copied, glyph cache counts and image decodes.

The Mac OS 9 shell (`src/platform/macos9`) keeps one module per kind of
window (`browserwin.c`, `listwin.c`, `prefswin.c`), with menus and the
event loop in `main.c` and files in `storage.c`. The address field is a
TextEdit record placed over the field the engine draws; the scroll bars
and menus are native; the Preferences window uses Appearance Manager
controls, declared in `appearance.h` because the Multiversal Interfaces
lack them, from AppearanceLib.

Aura keeps its files in the Aura folder of the system Preferences folder:
`Aura Preferences`, `Bookmarks.html` (Netscape format), `History`,
`Session` and `Downloads`, the `Blocklists` folder (see Content
blocking), and `Running` while it runs. The session
(windows, tabs, back and forward lists, scroll positions) is saved
3 seconds after tabs change and every 30 seconds while only scroll
positions do (`session_gen`, `scroll_gen`). `Running` is made at start
and deleted after the last save at quit; when Aura finds it at start,
the last run crashed or ran out of memory, and Aura offers to restore
the session (ALRT 133) unless the startup preference restores it
anyway. To test this in the guest, press Command-Option-M (the
out-of-memory test) and launch Aura again. Each is written to a `.tmp` file and swapped
in; a file that fails to parse is renamed with ` (damaged)` before a new
one is written. To start with clean state in the guest, delete that
folder.

Downloads go through `src/net/transfer.c` (`au_net_transfer_source_*`)
over the platform's `AuDownloadFiles`; on Mac OS 9 that is
`downloads_mac.c`: a partial file ("<id> <name>.part") in the download
folder (the preference, else Internet Config's, else the desktop),
renamed and given Internet Config's type and creator when complete,
resumed with Range. A page's response that turns out to be a download
(attachment or a type Aura cannot show) continues on its own transfer.
`au_sim_source` remains for tests.

The network stack in the Mac OS 9 application is `network.c`: the
entropy pool starts at launch, Open Transport on the first DNS lookup
(a machine without it gets the No network page with the reason, and a
failed start is retried after 10 seconds). Cookies and HSTS are saved
as `Cookies` and `HSTS` in the Aura preferences folder 5 seconds after a
change and at quit (session cookies are dropped); the disk cache is the
`Cache` folder (`src/net/disk_cache.c`, limited by the disk cache
preference). The Cookies window (Window > Cookies, or Preferences >
Privacy) lists `src/net/cookie_view.c`'s model; per-site rules (allow,
session only, block) live in the `cookies.sites` preference. Develop >
Network Status shows entropy, Open Transport counters, roots, cookies
and cache use. The out-of-memory handler closes Open Transport
before ExitToShell: with providers open at exit the guest froze.

Engine code must not use `%lld` or `%llu`: newlib on Mac OS 9 lacks
them and writes garbage (the Cookies file was unreadable for that
reason). Use `au_put_i64` (`history.h`).

Toolbox points learned the hard way:

- In a Rez `MENU`, the enable flags put item k at bit k-1 (the menu's
  own flag is the separate `enabled` field). A separator at item 5
  clears bit 4.
- Do not call `TEGetPoint` from PowerPC code; it crashed the guest. The
  caret position comes from `TextWidth` with the TextEdit handles
  locked.
- Scroll bar thumbs drawn as coloured noise on the second launch of
  Aura in a guest boot come from Enforcer, not Aura: they appear only on
  the Enforcer base, at any depth, with or without dithering. The
  Appearance theme draws thumbs through a 16x17 GWorld it keeps in the
  system heap, with its pixels in temporary memory, made the first time
  a thumb is drawn in the boot. Enforcer's temporary memory pool puts
  those pixels in a chunk of the current application (Aura) and frees
  the chunk when Aura quits; the stock Process Manager keeps them. The
  next Aura launch gets a chunk at the same address, and the theme's
  handle then names Aura's font data.

## Developer tools

Aura has four developer windows, opened from the Develop menu: Page
Source (also View > Page Source, Command-U), Console, Network and
Performance. Each shows the active tab of the frontmost browser window
and follows it. The models are portable and in the engine; the Mac OS 9
windows are `src/platform/macos9/devwin.c`. The Develop menu also holds
the debug items (Test Scene, Font List, Benchmark, Network Status, Page
Timings).

- **To change how the windows draw text**, edit
  `src/browser/textview.c`: a fixed-width text view (Monaco, else the
  monospace family, 10 pixels) with optional line numbers, wrapping,
  colour runs, a character selection or a selected row, find, and
  scrolling by rows. It owns no text: it asks an `AuTextSource` for one
  line at a time, reads each line's cell count once per layout and paints
  only the rows on screen. `AuTextList` is a ready-made source for text
  made on the spot.
- **Page Source** is `src/browser/sourceview.c`: one pass over the
  page's bytes (`au_page_source`) records each line's start and the
  colouring tokenizer's state there. Lines longer than 2048 bytes are
  cut into segments, so a minified page stays cheap. The tokenizer
  colours tags, attribute names, values, comments and `<!DOCTYPE`, and
  leaves `<script>` and `<style>` text plain; it is not the HTML parser.
  The window is a native text view, not a page: on the host, laying out
  a 1.3 MB document as plain text in the engine took 29 ms and would
  cost far more with colour spans, where the text view lays the same
  document out in under 1 ms and paints a screen of it in 1 ms. The
  contextual menu's View Source still opens a `view-source:` tab.
- **Network** is the page's request log, `src/browser/netlog.c`: a ring
  of 300 records, cleared by a navigation and freed with the page. A
  document kept for Back and Forward keeps its log, which returns with
  it.
  `page_doc.c` fills it from its fetches (start, response head, data,
  end). The times, the connection, the remote address, TLS version and
  cipher and the source (network, memory cache, disk cache, history,
  304) come from the HTTP layer: every transfer stamps the start of each
  phase (`au_http_trace` in `net.h`), the loader passes the trace in
  `AuLoadInfo`, and `page_fetcher.c` turns it into `AuFetchInfo.net`
  (`AuFetchNet` in `page.h`). The cheap fields are always recorded.
  Request and response headers are copied (4 KB each at most) only after
  `au_net_set_trace_headers(net, true)`, which the Network window calls
  when it first opens, so headers appear for loads made after that.
  A request refused by policy is logged as blocked with
  `blocked_reason`: a blocklist's refusal arrives in
  `AuLoadInfo.blocked_reason` ("blocked: LIST NAME") and mixed content
  as `AuLoadInfo.mixed`; `page_fetcher.c` passes either on as
  `AuFetchNet.blocked_reason`. While no request is selected, the
  window's lower pane lists the page's blocked requests with the list
  that stopped each (`au_netlog_blocked`).
- **Console** shows the page's console lines (`au_page_console_line`)
  through `au_dev_console_lines` in `src/browser/devtools.c`. Uncaught
  errors and unhandled rejections arrive as error lines with their
  stack; the first stack frame gives the source URL and line. The input
  line calls `au_dev_console_eval`, which runs the expression with
  `au_page_eval` (starting the script runtime on a page that has no
  script) and adds the expression and its result to the console.
- **Performance** is `au_dev_perf_report` in `devtools.c`, rebuilt once
  a second. The time counters are `AuPerf` (`src/page/perf.h`), one per
  document view, cleared when a document begins: the view times its
  style and layout passes, its paints and its outermost script entries
  with the embedder's millisecond clock, and the Mac shell adds each
  screen copy (`flush` in `browserwin.c`). Tasks of 50 ms or more are
  kept for the slowest-tasks list, scripts with their URL. The shell
  supplies free heap and temporary memory, and `au_net_dev_info`
  (`src/net/net_report.c`) the HTTP cache and the open connections.
  Copy Report puts the text on the clipboard, for measurements from a
  real machine.
- **To see the same text on the host**, run `aura-host chrome` with
  `--do netlog` (or `netlog:details`), `--do perf`, `--do eval:EXPR`,
  `--do source` (or `source:FIRST,COUNT` for lines with their colour
  runs) after the page loads; add `--net` for network pages. `--do
  devshot:source|console|network|perf:FILE.png` paints that window's
  text view to a PNG. For example, `aura-host chrome --net --tab
  https://example.com/ --do netlog:details --do perf`.
- **Tests.** `tests/browser/test_devtools.c` runs the request log on the
  fake transport (sources, connections, timings, headers, blocked
  requests, the ring), the counters and the report, the console and its
  input line, and the source view and text view (lines, colouring,
  wrapping, selection, find).
- **Not done:** a source position for `console.log` lines (only errors
  carry a stack); a case-sensitive find, a horizontal scroll bar and
  column sorting in the windows; request bodies and cookies in the
  Network details; saving the log to a file.

## Layout and paint speed

To time layout and painting of a page on the host, build a Release tree
(`cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release`) and run
`build/rel/src/platform/host/aura-host layout-bench PAGE --bundled-only`.
It prints, as CPU time, a layout from scratch at two widths (`-w`,
`-w2`), a width change in the layout alone and through the document
view (style included), a height change, what each change cost (box tree
builds, layout passes, intrinsic width runs, subtrees reused), and
painting: page downs over new content, the same pages again, a
32-pixel strip and a one-pixel paint (the cost that does not depend on
the area). `--phase relayout|resize|paint|paint-cold|strip --repeat N`
runs one step N times for a profiler (`sample aura-host 3`). To count
the passes a window resize costs, run `aura-host chrome --tab PAGE --do
resize:W,H`.

- **Mac OS 9 is not the host.** Host timings did not show the allocator
  problem above: a layout pass that took 6 ms on the host took 11 s in
  the guest. When a guest figure is far from the host's, look for what
  differs there (the Memory Manager, newlib's byte-wise `memcpy`)
  before tuning the engine.
- **A viewport change** is an incremental layout. The document view
  restyles and calls `au_layout_styles_changed`, not
  `au_layout_tree_changed`; the box tree is rebuilt only when a style
  difference needs it (display, position, float, overflow, generated
  content) or an element that had `display: none` is shown (layout keeps
  the elements it left out in `AuLayout.hidden`). Layout then reuses
  every independent subtree whose constraints did not change, and keeps
  intrinsic widths, which do not depend on the viewport. Lengths in
  viewport units reach layout as style changes.
- **Generated text** (a pseudo-element's content, a marker's number) is
  a text box whose node is the element; `current_style` in `layout.c`
  finds its style through the pseudo-element box above it.
- **Overflow** (`l_finish_overflow`) leaves out an absolutely positioned
  box whose containing block is outside the box: it is laid out later,
  so its geometry there is the previous layout's.
- **The horizontal scroll bar** keeps its state across a change of the
  page area's width (`relayout` in `browser.c`), and `check_hscroll`
  corrects it after the layout. Deciding from the old content width cost
  a second pass on every resize of a page that follows the window.
- **Text painting.** A face caches the glyph of each code point below
  U+0250 and recent kerning pairs (`AuFontCache` in `font_internal.h`).
  The glyph cache stores masks with the text coverage table already
  applied; `au_draw_coverage` draws them, `au_draw_mask` still applies
  the table for other callers. The rasterizer uses stack work areas for
  paths up to 62 pixels wide and writes masks without the span
  callback. Layout shapes into one kept glyph run (`AuLayout.run`).
- **Tests.** `tests/layout/test_relayout.c` resizes each sample page and
  several inline pages through ten viewports and compares boxes and
  pixels with a fresh document view at each size, then checks the
  counters (no tree build or intrinsic run for a height or width change,
  a new measure after a text or font size change).
  `tests/paint/test_shape_cache.c` covers the face caches and the glyph
  cache.
- **Measured** on a Wikipedia article of 18,053 boxes. Host (CPU time):
  width change in layout 2.6 ms (3.7 before), paint of a 1440x900 view
  0.92 ms (1.22), first paint of a page of new text 1.36 ms (1.68), a
  32-pixel strip 0.046 ms (0.061), a one-pixel paint 0.005 ms: painting
  already costs in proportion to what it shows. A width change through
  the document view takes 18 ms, of which the restyle of every element
  is 15 ms; the cascade does not yet tell which viewport changes leave
  styles alone. Guest (emulated, stock base, 600 pixels wide): the last
  style and layout pass of the load 465 ms (11,251 ms before the pooled
  allocator), a whole-page paint 4 to 5 ms (54), a page down 4 to 11 ms
  (349), a window zoom two passes of about 280 ms each.
- **Not done:** an image whose size becomes known still rebuilds the
  box tree (`au_layout_images_changed`); text is segmented and measured
  again for each layout of its block (the word cache answers the
  widths); `l_queue_abspos` searches its queue linearly.

## Grid

To change grid layout (`display: grid`, `inline-grid`), work in
`src/layout/grid.c`. It is a formatting context like flex: `l_fc_ops`
in `layout.c` dispatches `LI_GRID` boxes to `l_grid_layout` and
`l_grid_intrinsic`, and the box tree treats a grid container's children
as it treats flex items (blockified, text runs wrapped in anonymous
items, `LF_FLEX_ITEM | LF_BFC`, so `z-index` makes a stacking context
and incremental layout reuses an item whose constraints did not
change). The grid properties share the flex style group
(`AuStyleFlex`), so a change relays the box out without a tree rebuild.

- **Order of a layout.** `grid_build` expands the track lists
  (`repeat()`, `auto-fill`, `auto-fit`, line names), reads the named
  areas, resolves each item's lines (`resolve_lines`) and places the
  items (`place_items`: definite items, items locked to a row, then the
  auto-placement cursor, sparse or dense, over an occupancy bitmap).
  Implicit tracks before the explicit grid shift every index, so tracks
  count from 0. `size_axis` then sizes columns, each item's width is
  found (`item_width`), `size_axis` sizes rows, `position_axis` applies
  `justify-content` and `align-content`, and each item is laid out and
  aligned in its area.
- **Track sizing** follows section 11 of the specification: items are
  taken by span, each phase distributes an item's extra space to the
  tracks it spans (`dist_item`, up to their limits by `water_fill`, then
  beyond), then tracks are maximized, flexible tracks expanded
  (`find_fr`) and auto tracks stretched. `mode` 1 and 2 size under a
  min-content and a max-content constraint for the container's
  intrinsic widths.
- **Measurements.** An item's column contributions come from one
  `l_outer_intrinsic` call (kept by `LF_INTRINSIC_OK`), its row
  contribution from one `l_measure_content` call at its used width (kept
  by the measure cache). A stretched item is laid out a second time at
  its final height, as in flex.
- **Percentages.** A percentage track or gap behaves as auto while the
  container's size in that axis is unknown (intrinsic widths, an auto
  height); the rows are then sized again against the height found.
- **Limits.** An axis has at most 1000 tracks (`G_MAX_TRACKS`): `repeat`
  counts stop there, lines beyond are clamped into the last track and
  spans are cut. Line names are capped at 4096 per axis and areas at
  1024. Keep new loops bounded by these.
- **Absolutely positioned children** of a grid container that is their
  containing block are positioned against the area their grid lines
  name; an `auto` line is the padding edge. `abs_area` records the area
  in `AuLayout.grid_abs`, and `layout_abspos` in `layout.c` takes it
  with `l_grid_abspos_area`.
- **Tests.** `tests/layout/cases/grid*.txt` (sizing, placement,
  alignment, integration; expected values are worked out from the
  specification, and a comment says where a browser was not consulted),
  `tests/css/cases/grid.txt`, and `tests/page/samples/grid.html`, which
  the relayout and scroll-shift tests load.
- **Measured on the host** (Release, CPU time): a 12-column grid of 200
  items with spans of 1 to 6 (803 boxes) lays out from scratch in
  0.93 ms at 1440 pixels and 1.04 ms at 656; a width change takes 0.6
  to 0.7 ms.
- **Not done:** `subgrid` (parsed; laid out as `none`), baseline
  alignment of items (they align at the start), `direction: rtl` and
  vertical writing modes, `safe` overflow alignment, painting items in
  `order` order, `masonry`. An item's min-content and max-content
  heights are both its height at its used width.

## Fonts

Aura draws with the system's fonts first and with the fonts bundled in
the `Fonts` folder beside the application for families and characters
the system lacks.

- **Where faces come from.** On Mac OS 9, `platform_mac.c` registers
  every outline face of the FONDs in the resource chain (their `sfnt`
  resources load on first use), then the bundled faces listed in
  `Fonts/fonts.idx`. On the host, `platform_host.c` loads a fixed list of
  macOS fonts that match Mac OS 9's, then the bundled faces of
  `assets/fonts` (`fontdir.c`), lazily. `aura-host` takes
  `--bundled-only` (no system fonts: reproducible renders, close to a
  Mac OS 9 machine without the system's fonts), `--no-bundled-fonts` and
  `--fonts-dir D`; `$AURA_FONTS` also names the folder. `aura-host
  fonts` lists the faces and marks the bundled ones.
- **The bundled set** is in `assets/fonts`, one folder per family with
  its licence: Aura Sans, Aura Serif and Aura Mono (Liberation 2.1.5,
  metric-compatible with Arial, Times New Roman and Courier New, renamed
  because Liberation's Reserved Font Name forbids a subset under its own
  name), Inter (modern interface fonts), DejaVu Sans (the last-resort
  text face), Noto Emoji (monochrome emoji) and Twemoji Mozilla (colour
  emoji, COLR version 0), about 4.6 MB. `assets/fonts/README.md`
  records sources, versions, licences, sizes, the choice of Twemoji over
  Noto Color Emoji, and what was left out.
- **To add a font**, put its folder (licence included) in
  `assets/fonts`; reduce it with `tools/fonts-prepare.py` (one family:
  `--only=FAMILY`) or pyftsubset to TrueType outlines (no CFF), keeping
  file and folder names to 31 characters; regenerate the index
  with `build/host/src/platform/host/aura-host font-index assets/fonts`;
  add its sources to the README; and name it in the alias table or the
  fallback lists if it should stand in for other families. The paint
  tests check that `fonts.idx` matches the folder and that every face
  parses and draws.
- **The alias table** is `src/paint/fontalias.c`: rows map a family name
  as pages write it (ASCII case-insensitive) to the families tried in
  its place, installed system families first and bundled faces last.
  Rows for the CSS generic families also name the preference slot whose
  family (the user's font preferences, or the platform's choice) is
  tried before the row; a target naming a generic expands to it. To
  extend it, add a row. Matching (`au_fontset_match`) tries the families
  the page names up to its first generic family, then their alias
  targets, then the generic, and so on, then sans-serif. So "Roboto,
  Arial, sans-serif" uses an installed Arial before Inter, and "Roboto,
  sans-serif" uses Inter.
- **Per-character fallback** is `AuFontList` (`au_fontlist_new`) and
  `au_shape_text_list` in `src/paint/fallback.c`, which layout uses for
  all text through its `LFont`. Text is split into clusters (a base with
  its combining marks, variation selectors and emoji modifiers) and each
  cluster gets the first face that has all of it: the primary face, the
  list's other candidates in matching order, the text fallback list
  (DejaVu Sans first, then system faces with wide coverage, including
  CJK faces when a machine has them), the emoji list, then any face
  already loaded. Emoji presentation (by default or with U+FE0F) tries
  a colour font the page names, then the emoji list (Twemoji Mozilla
  first); text presentation (U+FE0E) takes the monochrome Noto Emoji.
  An emoji sequence (ZWJ sequences, flags, keycaps, skin tones, tag
  sequences) is one cluster: the face's GSUB ligature draws it, and
  without one its components are drawn. Default-ignorable code points
  draw nothing. Marks are placed over their base by their ink boxes (the
  fonts carry no GPOS for Aura to read). Fallback faces keep the
  requested weight and style, synthesized when missing. A per-list cache
  and a per-set cache keyed by code point and style keep this fast; a
  face is loaded only when it is tried.
- **Variable fonts** (`fontvar.c`): fvar, avar (versions 1 and 2), gvar
  (with IUP, composite offsets and phantom points), HVAR, MVAR and STAT
  names. `au_font_face_instance` makes an instance at axis values;
  `AuFontSet` matching makes instances for the requested weight (wght),
  italic (ital, else slnt), and, through `au_fontlist_new_style`,
  stretch (wdth), optical size (opsz) and `font-variation-settings`.
  Instances are cached per set entry (at most 64). A weight the font
  cannot reach is synthesized as before. Glyph boxes (`au_font_glyph_box`)
  are the default instance's.
- **Colour glyphs** (`colr.c`): CPAL palette 0, COLR version 0 layers
  and version 1 paint graphs (solid, linear, radial and sweep gradients,
  transforms, composite modes), with limits on depth (32), paints
  visited (4096) and cycles. Colour glyphs go in the glyph cache as
  premultiplied pixels under the same byte limit, keyed with the text
  colour only when they use it.
- **GSUB** (`gsub.c`): lookup types 1 (single), 4 (ligature) and 7
  (extension) of the features ccmp, locl, rlig, liga and clig, for the
  DFLT, else latn, script. Fallback uses it for emoji sequences only;
  Latin text is not shaped.
- **Web fonts.** `au_font_decode` (`woff.c`) turns WOFF 1.0 and WOFF2
  (transformed glyf, loca and hmtx; collections) into sfnt bytes and
  refuses CFF-flavoured fonts. `src/page/webfont.c` loads `@font-face`
  rules for a document view into a font set layered over the platform
  set (`au_fontset_new_layered`, `au_fontset_add_web`), fetching only
  faces whose family the styled document uses for text in their
  `unicode-range`, and lays the document out again when one arrives
  (font-display: swap). The faces go with the document.
  `tests/page/samples/webfonts.html` shows a variable WOFF2 font at
  several weights and a WOFF face limited by `unicode-range`.
- **Test fonts.** `tests/paint/fonts` holds small subsets of Inter
  Variable, Open Sans variable, Twemoji Mozilla and Noto Color Emoji
  (COLR version 1), WOFF and WOFF2 versions, a font with every COLR
  paint format, and static instances made by fontTools' instancer as
  references. To rebuild them, run `tools/test-fonts.py fetch`, then
  `tools/test-fonts.py make` (fontTools and the brotli command needed;
  building Aura needs neither); `tools/test-fonts.py refs` writes full
  fonts for `paint_fontvar_full`, which skips without them. To fuzz the
  font parsers for longer, run the sanitizer build's `fuzz_font
  ITERATIONS SEED SECONDS tests/paint/fonts/*.ttf tests/paint/fonts/*.woff*`
  and `fuzz_webfont ITERATIONS SEED SECONDS`. `test_fontvar DIR --bench`,
  `test_colr DIR FONTS --bench` and `test_woff DIR --bench FILE` time
  varied glyphs, colour glyphs and decoding.
- **Line height.** With `line-height: normal`, a line holding fallback
  glyphs grows to the fallback face's ascent and descent (each with half
  its line gap) where they exceed the primary's, as Blink does; a fixed
  line height ignores fallback faces.
- **Not done:** OpenType shaping of text (GPOS, and GSUB beyond emoji
  sequences), so no ligatures, kerning beyond `kern`, Arabic joining,
  Indic reordering or bidirectional text; CFF and CFF2 outlines; SVG,
  sbix and CBDT colour glyphs; COLR variations (version 1 draws at the
  default instance); a CJK font pack (see the fonts README for what one
  needs). CSS does not parse `font-tech()`, and layout passes only
  weight and style to the font list (`font-variation-settings` and
  `font-optical-sizing` are computed, in `AuStyleInhRare`). webfont.c
  still reads `unicode-range` from the sheet text, which it gets once,
  when the sheet is parsed; the parsed descriptor is there to replace
  that (see CSS).

## CSS

`src/css/css.h` is the interface: sheets, the resolver, computed styles,
invalidation, selectors. `css_int.h` is shared by the files of `src/css`
and its tests only.

- **To restyle after a change**, call `au_css_invalidate` (before or
  after the change) and then `au_css_restyle`. For a state bit use
  `au_css_invalidate_state` with the `AU_STATE_*` bits that changed, on
  every element whose bits changed; for hover and active use
  `au_css_move_state`, which sets the bits and reports only the elements
  below the common ancestor of the old and the new element.
- **What a change restyles.** When the rule index is built
  (`deps_build` in `cascade.c`), every selector is analysed for the
  states it reads and the classes, ids and attributes it names, and for
  where the elements it can style lie from the element that changes:
  the element itself, its descendants, its following siblings, or the
  ancestors and preceding siblings a `:has()` argument can anchor on.
  A change marks only those (`AU_STATE_STYLE_SELF_DIRTY` for one
  element, `AU_STATE_STYLE_DIRTY` for a subtree). A class, id or
  attribute that no selector names, no presentational hint reads and no
  `attr()` or pseudo-class depends on restyles nothing. Selectors the
  analysis cannot follow (`:nth-child(... of S)` with a state or name
  in S, a complex selector nested inside a `:has()` argument) restyle
  the document. Child insertions and removals are not targeted: they
  restyle the element's subtree and its following siblings' subtrees,
  or the document when a `:has()` rule exists.
- **Class and id changes** need the names that changed. `src/dom` notes
  them in `AuDocument.style_names` when the attribute changes, and the
  resolver resolves the elements reported for class or id when the
  restyle starts (the DOM mutation hook reports before the change). To
  add an attribute that a pseudo-class or a presentational hint reads,
  add it to `attr_state` or leave it out of `attr_never_hint` in
  `cascade.c`.
- **Equal styles keep their object.** A style pass compares each newly
  computed style with the element's old one by value (`style_same`) and
  keeps the old `AuStyle` when they are equal; a changed style takes
  over the old groups whose values are equal (`adopt_groups`). Code that
  compares style or group pointers (layout's `style_diff`) then sees
  only what changed. A marked element whose style is unchanged does not
  restyle its subtree.
- **Sharing.** Elements with the same parent style, tag and matched
  rules share one `AuStyle`; the key includes the matched `::before`,
  `::after` and `::marker` rules, and an element whose pseudo-element
  uses `attr()` is not shared. Custom property maps are layered: a map
  holds the element's own entries and refers to the parent's map
  (`AuCustomProps.base`, chains of at most 12).
- **Sheets in a document** (`src/page/docview.c`, "Requests" and
  "Styles"): each `<style>`, `<link>` and `@import` is a `SheetSrc`. It
  is parsed once, when its text arrives (`parse_sheet`); the resolver
  borrows the parsed sheets, and a change of the sheet list detaches and
  adds them again (`au_css_resolver_detach_sheets`) without parsing. A
  fetched sheet's text is freed after parsing; a `<style>`'s is kept to
  notice edits. A source whose media query does not match the device is
  neither fetched nor parsed (`start_sheet`); `undefer_sheets` starts it
  when the viewport changes so that it matches. A sheet that has been
  parsed stays, and the cascade evaluates its media query.
- **Memory of parsed sheets.** During a sheet parse equal `var()` and
  custom property token lists share one list
  (`css_toklist_intern_begin`) and equal declared values one blob
  (`css_blob_intern_begin`); arrays are trimmed to their length. To see
  what sheets cost, run `build/host/tests/css/css_bench --mem
  FILE.css...` (macOS only): it prints the heap each parsed sheet keeps
  and the index size.
- **Selector queries.** `au_css_query_selector` and
  `au_css_selector_matches_scoped` take the element `:scope` stands for
  (the element querySelector, querySelectorAll or closest was called
  on); with a document or NULL, `:scope` is the root element.
- **`@font-face` descriptors** are generic value trees
  (`au_css_font_face_descriptor`), except `unicode-range`: a COMMA_LIST
  of LISTs of two integer NUMBERs, the first and last code point of
  each range; an invalid `unicode-range` is dropped. The tokenizer keeps
  each number as written (`css_tok_num_text`) for this, since
  `U+0025-00FF` and `U+1e3` are number tokens.
- **Tests.** `tests/css/cases/*.txt` (one ctest per file, format in
  `test_css.c`), `test_css_api.c` (API tests), and
  `test_css_invalidate.c`, which changes states, classes, ids,
  attributes and inline styles on a page with and without `:has()`
  rules and checks every computed value of every element against a full
  restyle. To time hover moves over a page, run
  `build/host/tests/css/test_css_invalidate --bench [--verify]
  PAGE.html [SHEET.css ...]`: it prints elements restyled and time per
  move, first with the old coarse invalidation, then targeted.
- **Measured on the host.** A saved Wikipedia article (9301 elements,
  `:has()` rules): a hover move restyled 9301 elements in 12 ms, now 2
  on average (8 at most) in 0.012 ms. The GitHub home page (20 sheets,
  2.5 MB of CSS, 2157 custom properties): `aura-host chrome` peaked at
  147 MB, now 79 MB; the parsed sheets take 10.3 MB instead of 17.1 MB,
  and the computed styles 5.8 MB instead of 44 MB.
- **Not done:** targeted invalidation of child insertions and removals;
  `@container`, `@scope` and `@starting-style` are parsed and never
  apply; preference changes (dark scheme, reduced motion) do not reach
  the device, so sheets deferred for them stay deferred.

## Images

The decoders in `src/image` parse hostile data on a machine without
memory protection. Every length comes from the file and is checked
before use; every size computation is checked for overflow; dimensions
are capped at 32768 and source pixels at 64 million (`AuImageOptions`);
the decoded canvas never exceeds `max_bytes` (16 MB by default), and a
larger image decodes at 1/2, 1/4 and so on down to 1/256, or fails.
Keep it that way: a change to a decoder needs a vector, a truncation
test and a fuzz run under the sanitizers.

- **To decode bytes**, create an `AuImageDecoder` with
  `au_image_decoder_new` and call `au_image_decode` with each chunk as
  it arrives (`image.h`). The size is known as soon as the header
  arrives (`au_image_info`), rows fill in as they decode
  (`au_image_take_dirty`), and each animation frame returns
  `AU_IMAGE_FRAME`. Output is a premultiplied `AuSurface`; its `opaque`
  flag lets painting copy instead of blend. `au_image_decode_buffer`
  decodes a whole file, `au_image_probe` reads only the header, and
  `au_image_decode_favicon` picks and scales an icon.
- **Formats.** One file per format: `png.c` (all colour types and
  depths, Adam7, APNG, CRCs checked), `jpeg.c` (baseline and
  progressive Huffman, CMYK and YCCK, restarts, EXIF orientation,
  reduced decoding by IDCT scaling), `gif.c`, `webp.c` with
  `webp_vp8.c` and `webp_vp8l.c` (lossy, lossless, alpha, animation),
  `bmp.c` (BMP and ICO). `image.c` holds what they share: sniffing, the
  limits, row reduction, orientation and frame composition. Each codec
  is a state machine that consumes every byte it is given, so chunk
  boundaries never matter.
- **The cache** (`cache.h`) holds one document's images by URL. Layout
  gets the size from the header; pixels are decoded only when an image
  is used in a paint (`au_image_entry_use`), at the smallest power-of-two
  reduction that still covers the displayed size. The budget comes from
  the memory cache preference (`au_docview_set_image_budget`, 8 MB by
  default, as without a browser). Over the budget, images not
  used in the last paint lose their pixels, oldest first, and keep their
  compressed bytes to decode again. One image may use at most half the
  budget (at least 1 MB, at most 16 MB). A still image whose bytes are
  larger than its pixels keeps only the pixels. Animations decode frame
  by frame from the compressed bytes, keep one canvas, and pause when
  not painted; delays of 10 ms or less play at 100 ms, as browsers do.
- **The document view** (`src/page/docimages.c`) decodes images within
  a viewport above or below the visible area before they scroll in,
  invalidates only the rectangles of images that changed, and passes
  pixels to layout through the image callback. A pending image is an
  empty box; a failed one shows its alt text.
- **To try a file**, run `build/host/src/platform/host/aura-host image
  FILE -o out.png`. It prints the format, size, decode time and peak
  memory; `--scale N`, `--target WxH`, `--chunk N` (feed N bytes at a
  time), `--frames`, `--repeat N` and `--favicon SIZE` change what it
  does. `tests/page/samples/images.html` shows every format; its images
  are drawn by `tests/page/make-sample-images.py`.
- **Tests.** `tests/image` has one test per format with committed
  vectors and references made by the host's own decoders (the
  generator scripts and their headers say how). PngSuite and the libwebp
  test images are fetched into `build/` by
  `tests/image/fetch-pngsuite.sh` and a `git clone` of
  `https://chromium.googlesource.com/webm/libwebp-test-data` into
  `build/webp-testdata`; their tests skip without them. To fuzz longer,
  build with the sanitizers and run `build/asan/tests/image/fuzz_image
  ITERATIONS SEED tests/image/vectors/*/*`.
- **Not done:** arithmetic-coded, lossless and 12-bit JPEG; colour
  management (gamma, ICC profiles and colour spaces are ignored);
  object-fit, border-image and list-style-image in layout. At 256
  colours images band; nothing dithers them.
- Retro68's `<string.h>` pulls in the Toolbox `Strings.h`, so names like
  `FInfo` collide with Toolbox types. Prefix types with `Au`.

## SVG

`src/svg/svg.h` describes what the renderer supports and what it does
not. One renderer serves inline SVG and SVG documents; it reads the
elements' computed styles, so page CSS applies to inline SVG, and
presentation attributes reach the cascade as presentational hints
(`svg_hints` in `src/css/ua.c`).

- **Inline SVG.** Layout makes an outer `svg` element a replaced box
  (`LR_SVG`): its size comes from the width and height attributes, its
  ratio from them or the viewBox, else 300 x 150. `paint_svg` in
  `src/layout/draw.c` draws it into the content box with
  `au_svg_paint`, at the size it is shown. References (`use`, gradients,
  clip paths, masks) resolve through a document id index the layout keeps
  until the tree changes.
- **SVG as an image.** `src/image/svgimg.c` is the codec: markup is
  sniffed as SVG, the bytes are held to the end, and the document is
  drawn at the target size rather than reduced. The cache keeps SVG bytes
  and draws the image again when it is shown larger or in another shape;
  layout asks for each paint size through `AuLayoutEnv.image_use`. An SVG
  image loads nothing: only `data:` URLs in `<image>` draw, nested SVG to
  depth 2.
- **Masks.** `mask-image` and `-webkit-mask-image` layers draw into a
  canvas mask group (`paint_mask` in `draw.c`), alpha by default,
  luminance with `mask-mode: luminance`.
- **Limits.** The XML parser caps depth (256), nodes (100000) and
  attributes (256), skips the doctype and never expands entities beyond
  the predefined five and character references. The renderer caps
  nesting (48), `use` depth (12) and cycles, elements per paint, href
  chains and text length. Clip pieces and text layout go on the heap
  because the renderer recurses.
- **To try a file**, run `build/host/src/platform/host/aura-host image
  FILE.svg --target WxH -o out.png`. `tests/page/samples/svg.html` shows
  each feature.
- **Tests.** `tests/svg` holds `test_svg` (grammars, XML, pixels,
  the image pipeline; `--bench` times 200 icons) and `fuzz_svg`. To fuzz
  longer, run the sanitizer build's `fuzz_svg ITERATIONS SEED [SECONDS]
  tests/svg/corpus/*`. Layout cases are in `tests/layout/cases/svg.txt`.
- **Not done:** filters, patterns, markers, textPath, foreignObject,
  multi-line text, animation, CSS basic shapes in `clip-path`,
  `mask: url(#id)` on HTML elements, and one raster per SVG image URL
  (an image shown at two shapes on one page shares the larger raster).

## Networking

The network stack is portable engine code that never touches a socket
itself. `src/net` talks to the platform through `AuNetOps`
(`src/platform/platform.h`): DNS, non-blocking TCP, a monotonic and a
wall clock, and an entropy source. The host implements it with BSD
sockets (`src/platform/host/net_host.c`); Mac OS 9 with Open Transport
(`src/platform/macos9/net_ot.c`). There is no global state: one `AuNet`
per application holds the connection pool, TLS configuration and the
policy stores the embedder attaches.

- **To fetch with the engine**, create the platform's ops, then
  `au_net_new(ops)`, attach a root store (`au_roots_load` on
  `assets/net/aura-roots.bin`), and optionally a cookie jar, HSTS store
  and cache (`au_net_set_cookie_jar`, `au_net_set_hsts`,
  `au_net_set_cache`). Start requests with `au_http_start` and call
  `au_net_pump` on every pass of the event loop; callbacks run only from
  the pump. `au_net_apply_prefs` copies the browser preferences in.
- **To load pages and resources**, use the loader (`src/net/loader.h`):
  it adds mixed content blocking, MIME sniffing with `nosniff`, encoding
  detection and conversion to UTF-8, `data:` URLs, HTTPS-first, and the
  download hand-off (`au_net_download_*`, shaped for `AuTransferSource`).
  `src/net/page_fetcher.c` adapts it to the browser's `AuPageFetcher`:
  `au_net_page_fetcher_interface` fills one, and the shell registers it
  for `http` and `https` with `au_browser_set_fetcher`.
- **The HTTP cache** (`src/net/cache.h` describes what is stored and
  when it is fresh) is used through the page's cache mode
  (`AuFetchCacheMode`): a navigation uses it as HTTP says; Reload
  revalidates the document; Shift- or Option-Reload bypasses it; and a
  history navigation (Back, Forward, the history pop-up, reopened or
  duplicated tabs, session restore) uses `AU_FETCH_CACHE_HISTORY` for
  the document and its subresources, which serves any stored response
  as it is, stale, no-cache or must-revalidate, without a request
  (`au_cache_lookup_history`, RFC 9111 section 6). Responses with
  neither freshness nor a validator are stored for this alone. no-store
  responses are never stored, so history fetches them again, and Vary
  (Cookie included) still selects the variant. An entry may take a
  quarter of the memory cache (8 MB by default), so a large article's
  HTML fits. To count hits on the host, run `aura-host chrome --net
  --no-doc-cache --tab URL --do cache --do go:URL2 --do back --do cache`
  (without `--no-doc-cache`, Back shows the kept document and the HTTP
  cache is not asked; see Browser chrome). `au_cache_stats` counts
  lookups, 304s, stores and disk reads.
- **stale-while-revalidate** (RFC 5861): inside the window
  `au_cache_lookup` returns `AU_CACHE_STALE_USABLE`. For a subresource,
  `cache_try` in `http.c` delivers the stale entry at once and starts a
  transfer of its own to revalidate it (`revalidate_in_background`, one
  per entry at a time, no callbacks); a document waits for the
  revalidation. must-revalidate, no-cache and a request's no-cache or
  max-age turn it off. `tests/net/test_swr.c` covers it.
- **Disk cache writes are queued.** With
  `au_disk_cache_set_deferred`, a store only keeps the file's bytes;
  `au_disk_cache_idle` writes them oldest first and
  `au_disk_cache_flush` (also `au_disk_cache_free`) writes the rest. A
  lookup for a queued key is answered from the queue, and more than
  2 MB of queued bytes are written at once. The Mac OS 9 shell turns it
  on and writes one file per event loop pass while no transfer is under
  way (`mac_net_idle`); a write takes about 9 ms there.
- **The DNS cache** (`src/net/dns.c`) sits between the HTTP client and
  `AuNetOps.resolve_*`: an answer is used for 5 minutes and a failure
  for 10 seconds (the platforms give no TTL), at most 64 hosts, and
  lookups of one host that overlap share one platform request. A
  refused or failed connection forgets the host's answer.
  `au_net_clear_dns` clears it (Clear Cache in Preferences does);
  `au_net_dns_stats` counts lookups, and `aura-host chrome --do cache`
  prints them. `tests/net/test_dns.c` covers it.
- **To persist state**, serialize the stores with
  `au_cookie_jar_serialize`, `au_hsts_serialize` and the cache's
  `AuCacheStorage`, and parse them back at start. `aura-host fetch` keeps
  `cookies` and `hsts` in `$AURA_PROFILE` (default `~/.aura-host`).
- **To try a URL on the host**, run `build/host/src/platform/host/aura-host
  fetch -v URL`. It prints the response headers, the TLS version, cipher
  and certificate chain, and timings. `aura-host render URL -w 800 -o
  out.png` loads a page and its resources over the network and renders
  it. Set `AURA_FAKE_TIME` (seconds since 1970) to test the clock check.
- **To try the stack on Mac OS 9**, build with `./build.sh ppc` and push
  `build/ppc/src/platform/macos9/nettest/NetTest.bin` with a copy of
  `assets/net/aura-roots.bin` named `aura-roots` (`guest-push.sh` would
  decode a `.bin` file as MacBinary). NetTest times the crypto
  primitives, a DNS lookup and a TCP connection, then fetches a URL list
  twice and prints status, cipher and timings; a `NetTest URLs` file
  beside it replaces the list. It also writes `NetTest Log` in the Aura
  preferences folder.

TLS is BearSSL 0.6 with the 32-bit implementations: TLS 1.2 only, ECDHE
with AES-GCM or ChaCha20-Poly1305, then RSA key exchange with AES-GCM
as a last resort (www.fsf.org offers no ECDHE, only DHE, which BearSSL
lacks, and RSA), SNI, ALPN `http/1.1`, session resumption, and
certificate errors reported with the chain (`au_http_cert_info`). TLS
1.3 is not available in BearSSL; every acceptance site still offers
TLS 1.2.

- **Incomplete chains.** Some servers send their certificate without
  the intermediate (macintoshgarden.org, incomplete-chain.badssl.com).
  When the chain as sent does not reach a root, the X.509 wrapper in
  `tls.c` (`complete_chain`) builds a path from the leaf over the
  server's certificates and the intermediates pool (`certpool.c`) and
  replays it into BearSSL's minimal engine from `start_chain`, so every
  check runs on the completed chain as if the server had sent it. Pool
  certificates are never trust anchors. When the path stops for want of
  an issuer, `AuCertInfo` names the last certificate's caIssuers URL and
  the transfer (`aia_begin`, `aia_step` in `http.c`) fetches it over
  plain http as a child transfer (no cookies, referrer or cache, 16 KB,
  10 s, at most 4 per transfer, each URL once, failures remembered for
  the session), adds the DER, PKCS#7 or PEM certificates
  (`au_cert_bundle_parse`, `certinfo.c`) to the pool and connects again.
  The pool (32 certificates) is saved as `Intermediates` in the Aura
  preferences folder and `intermediates` in the `aura-host` profile
  (`au_net_intermediates_serialize`); a hostile file cannot grant trust.
  `aura-host fetch -v` says when certificates came from caIssuers URLs.
- **HTTPS-first failures.** `au_http_fallback_cause` says why the https
  attempt of an upgraded navigation failed, and reaches the page as
  `AuFetchInfo.cause`. Refused, timed out, closed or plain-text
  connections give "This site does not support HTTPS"; certificate and
  TLS failures give "This site's secure connection failed" with the
  reason and certificate, Continue to the HTTP Site, and for certificate
  problems the session exception (`show_https_fallback` in
  `page_doc.c`).

All network work runs on the main task. Open Transport is called only
from the main task in asynchronous mode; its notifiers run at deferred
task time and only record which completion arrived, and the next pump
acts on it. Do not call Open Transport or the engine from an MP task or
a Time Manager task: Apple's MP documentation (TN2006) lists Open
Transport among the services that are not MP-safe, and the Toolbox is
not reentrant.

The Mac OS 9 entropy pool (`src/platform/macos9/entropy.c`) gathers
loop timing jitter, user input, the mouse, clocks and free memory into a
SHA-256 pool and refuses output until it has credited 128 bits, so the
first TLS handshake waits for it (about 15 s from a cold start with no
user input, less with a seed file or input). The credit rules are in the
file's header. A seed file, `Random Seed` in the Aura preferences folder,
carries state between runs and is rewritten at start, when the pool
first fills, and at exit. The shell calls `au_mac_entropy_event` for
every event, `au_mac_entropy_idle` once per loop pass, and
`au_ot_net_idle` with them.

Open Transport has no headers in the Multiversal Interfaces, and Aura has
no Apple glue library. `src/platform/macos9/ot.h` declares the calls
used, and `net_ot.c` calls the exported `Priv` routines with its own
client record, as Apple's glue did.

On the test machine, QEMU sets the guest clock to UTC while the guest's
Map control panel says UTC-7, so the wall time Aura computes there is
7 hours ahead. Certificates are still within their dates.

The root store (`assets/net/aura-roots.bin`) was made from curl's
Mozilla bundle dated 2026-11-05 (118 roots, checked current on
2026-11-15); it holds the USERTrust and Sectigo Public Server
Authentication R46/E46 roots and ISRG Root X1/X2. To regenerate it,
download `https://curl.se/ca/cacert.pem` and run `python3 -I
src/net/gen_roots.py cacert.pem assets/net/aura-roots.bin`. Never add
intermediates to it.

Limits: IPv4 only on Mac OS 9; no HTTP/2; no proxy auto-configuration;
sites that offer only TLS 1.3, or only DHE or other ciphers outside
BearSSL's set, fail with "Secure connection failed".

## Content blocking

Aura blocks by domain: a request to a host on a blocklist is never made
(no DNS lookup, no connection). Aura carries no list. It downloads the
lists the user turns on from their publishers and keeps a compiled copy
(HaGeZi's lists are GPL-3.0; do not add a list to the repository or the
application).

- **To change matching or the file format**, work in
  `src/net/blocklist.c` (`blocklist.h` describes both). A compiled list
  is a sorted array of 48-bit hashes of lower-case domains, searched by
  bisection; a host is listed when it or a parent domain down to its
  registrable domain is in a list (never a bare public suffix), unless
  an exception covers it. Order: the personal allow entries, the
  personal block entries, any list's exceptions, the lists. Verdicts are
  cached per host (512 slots). With n entries a probe of an unlisted
  name hits by chance with probability n / 2^48 (3.2e-10 for Light), and
  a host takes two or three probes; the personal allow entries are kept
  as text and clear a false hit.
- **The parser** (`AuBlockBuilder`) takes plain domains, hosts files,
  adblock domain rules (`||domain^`, `@@||domain^`), `*.domain` and
  dnsmasq rules in any mix, streaming, with lines capped at 512 bytes
  and names validated; non-ASCII names go through
  `au_url_host_parse`. It sorts in chunks of 16384 entries and merges in
  slices (`au_block_builder_finish` with a budget), so a shell compiles
  at idle time. Every allocation may fail; the builder then fails and
  the old list stays.
- **Where it applies.** `au_net_loader_start` (`loader.c`) asks
  `au_blocklist_check` before anything else, and again for each redirect
  (`t_redirect`). A request with a top-level URL (subresources, fetch,
  XMLHttpRequest, frames) is blocked when its host is listed, unless it
  is first-party (the page's registrable domain) or the page's site is
  switched off. A document request without one is a navigation: it ends
  with `AU_NET_ERR_BLOCKED` and `page_doc.c` shows the blocked-site page
  (`show_net_error`), whose Visit Anyway calls
  `AuPageFetcher.allow_blocked` (the site is then allowed as a
  navigation until Aura quits) and whose Go Back calls
  `AuPageCallbacks.go_back`. A blocked request reports
  `AuLoadInfo.blocked_reason` and `AuFetchInfo.blocked_reason`
  ("blocked: LIST NAME"); the page counts them in `AuPageStats.blocked`
  and the status bar shows a shield and the count
  (`AuChromeState.blocked`).
- **Lists and updates** are `src/net/blocklist_store.c`
  (`AuBlockManager`): the catalogue (HaGeZi Light, Normal, Pro, Pro++
  and the Threat Intelligence Feeds in three sizes, in the wildcard
  domains format, at the jsDelivr URLs the project's README gives), the
  lists added by https URL, conditional requests (ETag, Last-Modified)
  at most once per list per interval, started only from
  `au_block_manager_idle` while nothing loads, outside the HTTP cache
  (`AuHttpRequest.no_cache`) and without cookies. A download that fails,
  leaves https, holds no domains or under a quarter of the old (or
  expected) count keeps the old list and is tried again after 6 hours
  (10 minutes when there is no old list). The platform supplies an
  `AuBlockStore` (files) and an `AuBlockMem` (large blocks).
- **Preferences:** `privacy.block_trackers` (the switch, on),
  `blocking.lists` (catalogue ids, "light"), `blocking.custom` (https
  URLs), `blocking.allow`, `blocking.block`, `blocking.sites_off`
  (registrable domains, set by Turn Off Blocking for This Site in the
  page's contextual menu, `au_bw_toggle_site_blocking` in `context.c`)
  and `blocking.update` (daily, weekly, never).
- **Mac OS 9.** `blocking_mac.c` keeps the lists in temporary memory
  (`TempNewHandle`, else the heap through `au_try_alloc`; a list that
  fits in neither is not loaded and the Preferences pane says so) and
  the compiled files in the `Blocklists` folder of the Aura preferences
  folder (one file per list, named by its id, and `Index`). Downloads
  start 4 seconds after launch at the earliest. Preferences > Blocking
  (`prefs_blocking.c`; the switch and the three fields are in
  `prefswin.c`) lists the catalogue with each list's size and date or
  what the manager is doing; its checkboxes and Update Now act at once.
  Develop > Network Status shows the totals, where the list memory
  came from and what the last compile cost.
- **To try it on the host**, run `aura-host blocklist compile LIST.txt
  OUT` (prints entries, time and peak memory), `aura-host blocklist
  check FILE HOST...`, `aura-host blocklist bench LIST.txt` (lookups per
  second) and `aura-host blocklist update DIR` (the manager against a
  folder, over the network). `aura-host chrome --net --blocklist FILE`
  and `aura-host render URL --blocklist FILE` check requests against a
  compiled or text list and print the counts.
- **Measured on the host** (lists of 2026-11-15): Light compiles to
  549 KB from 1.96 MB of text in 24 ms (91,414 entries); Normal 1.34 MB
  (223,018); Pro 1.42 MB (237,247); Pro++ 1.56 MB (259,753) in 63 ms;
  the full threat feeds 8.7 MB (1,455,489) in 387 ms. Compiling peaks at
  twice the compiled size. About 600,000 uncached lookups per second.
  arstechnica.com with scripts made 77 requests for 9.8 MB without
  blocking and 61 for 4.4 MB with Light; theguardian.com made the same
  118 either way.
- **Measured in the guest** (stock base): the first launch fetched Light
  10 seconds after the download began (653 KB on the wire; the time
  includes the wait for the entropy pool), merged it in 30 ms over 5
  idle calls and held it in 535 KB of temporary memory; Normal took
  0.8 s to fetch and parse and 73 ms to merge. Both lists use 1.8 MB.
- **Tests.** `tests/blocklist`: `test_blocklist` (formats and junk,
  matching, the public suffix guard, exceptions, collisions, damaged
  files, allocation failures, the request decision),
  `test_blocklist_net` (the loader, a browser window and the update
  manager on the fake transport) and `fuzz_blocklist ITERATIONS SEED
  [SECONDS]` (the parser, compiled files and the store's index; it
  prints the iterations done).
- **Not done:** lists per element or URL path (cosmetic and path rules
  are ignored), a list of the requests blocked on a page, IDN mapping
  beyond the URL parser's, and removing the compiled file of a catalogue
  list that was turned off.

## JavaScript

The engine is QuickJS-ng (`third_party/quickjs`), reached only through
`src/js`; no other folder sees a QuickJS type. `src/js/js.h` is the
interface: one `AuJs` per document, holding a context whose global
object is the window, the DOM bindings and the event loop. The documents
of one top-level page share one runtime (see Frames).

- **To run script in a document**, create an `AuJs` with `au_js_new`,
  run scripts with `au_js_run_classic` or the module calls, and call
  `au_js_run_tasks` and `au_js_run_frame` from the idle loop. Free it with
  `au_js_free` before the document goes, never from inside a hook.
- **Script in pages** is driven by `src/page/script.c`: parser-blocking
  scripts pause the parser, `document.write` feeds it, defer, async,
  module and nomodule scripts run in their order, and the document view
  fires DOMContentLoaded and load. The runtime starts only when a page has
  a script or an inline event handler. Scripting is off in a bare
  document view; `au_docview_set_scripting` turns it on from the next
  load. In the browser, `page_doc.c` sets it from the `content.javascript`
  preference (on by default) at each load, runs `au_docview_run_scripts`
  for up to 20 ms in every `au_page_idle` and follows the navigations it
  reports through `open_url`, and keeps the last 300 console lines
  (`au_page_console_line`) for the Console window (see Developer tools).
- **Response heads.** A loader that knows a response's status and headers
  passes them with `au_docview_resource_head` before the data; script
  then sees them in `fetch` and `XMLHttpRequest`, and an error status
  fails a script element's load. Without it script sees status 200, no
  headers and a type guessed from the URL; `page_doc.c` passes the final
  URL and content type, and status 200 until `AuFetchInfo` has a status.
- **Ownership.** A node's wrapper holds one reference on the node; the
  node holds its wrapper while the node is in a tree or otherwise
  referenced; a node no script has touched has no wrapper. `js_int.h`
  has the details.
- **Limits.** Each `AuJs` has a memory limit enforced by its allocator (a
  refused allocation is an uncatchable out-of-memory error, with a 64 KB
  reserve so the error can be reported), a C stack limit (deep recursion
  is a RangeError) and a time budget (`AuJsHooks.long_script` decides
  whether a long script goes on). Allocation failures never reach
  `au_alloc`'s fatal handler.
- **To run a script on the host**, use `build/host/src/platform/host/aura-host
  js FILE.js...` (`--html PAGE.html` loads a document first, `--stats`
  prints start-up time and memory). `aura-host page PAGE.html` loads a
  scripted page through the document view (`--click ID` clicks an
  element, `--chunk N` feeds the parser N bytes at a time, `--no-script`
  turns script off). The `tests/js` scripts and pages run under ctest.
- **To change QuickJS-ng**, add or edit a patch in
  `third_party/quickjs-patches` and rebuild the tree with `apply.sh` (see
  `third_party/README.md`); never edit `third_party/quickjs` alone.
- **To test on Mac OS 9**, build with `./build.sh ppc` and push
  `build/ppc/src/platform/macos9/jstest/JSTest.bin` with
  `tools/guest-push.sh -n AuraTest`, then `tools/guest-qmp.py open
  AuraTest JSTest`. JSTest prints its start-up time and memory, an "ok"
  or "FAIL" line per self-test, the benchmark times of
  `tests/js/bench/bench.js` at `BENCH_SCALE` 0.1, and the heap left. To
  compare with the host, run `aura-host js --stats` on a file that sets
  `globalThis.BENCH_SCALE = 0.1` followed by `tests/js/bench/bench.js`,
  with a Release build.
- **To run JSTest on a real G4**, copy `JSTest.bin` to the machine (a CD
  made from the image `guest-push.sh` writes, or a file share), decode it
  with StuffIt Expander if the Finder shows a MacBinary file, and open
  it. Its SIZE resource asks for 24 MB (20 MB at least); raise it in Get
  Info if the self-test reports out-of-memory errors. Quit other
  applications first so the timings are not disturbed. QEMU timings do
  not predict a G4's: the guest's speed depends on the host's TCG
  translation, so report G4 and guest numbers separately.
- **Retro68 compiler points.** QuickJS is built with
  `-fno-strict-aliasing` (with `uint32_t` as `unsigned long`, string
  iterators never advanced), and patch 0004 pads every `alloca`, because
  Retro68's GCC places spilled locals in the top 8 bytes of alloca space.
  Do not use `alloca` or variable-length arrays in Aura's own code.
- **The prelude** is one function expression split over
  `src/js/prelude*.js`; `prelude.c` embeds the files in order (base, dom,
  html, range, cssom, window, net, obs). To add a file, add its `#embed`
  line there. A name declared twice across the files stops the runtime
  from starting ("invalid redefinition").
- **To debug a script on a live page**, use `aura-host chrome` actions:
  `--do js:CODE` evaluates CODE in the active tab and prints the result,
  `--do save:FILE` writes the page source, `--do console` prints the
  console. Uncaught errors, errors in event listeners and unhandled
  promise rejections go to the console at error level; one that the
  page's `error` or `unhandledrejection` listener cancels (Google's
  `window.onerror` returns true) goes there at debug level, marked
  "handled by the page". To work on a site's scripts offline, save the
  page and the scripts it loads and serve them from a local HTTP server
  (the origin must be http for origin checks to behave as on the site).
- **window.postMessage** takes any absolute URL as the target origin
  and compares only its origin (between frames: see Frames). Google's search box defers its submit
  with `postMessage("sbox.df", location.href)`; comparing the string
  with the origin dropped the message and Return did nothing
  (`tests/js/pages/search_defer.html`, `test_search_box_defer` in
  `tests/forms`).
- **Range** (`prelude_range.js`, `ranges.c`). Boundary points live in C
  so that the DOM mutation hooks can move them without running script:
  `js_ranges_inserted` and `js_ranges_removed` follow the DOM standard's
  insert and remove steps. Offsets in text are UTF-16 code units, which
  only script knows, so `replaceData` and `splitText` in
  `prelude_dom.js` report their edits with `N.rangeText` and
  `N.rangeSplit`; the `data`, `nodeValue` and `textContent` setters and
  the parser are handled in `js_ranges_text` (data only appended moves
  nothing, anything else puts the node's points at offset 0). To change
  a text mutation path in the prelude, keep it going through
  `replaceData`. Not exact: `getClientRects` and
  `getBoundingClientRect` return the boxes of the elements a range
  covers (layout has no query from a DOM text offset to a position),
  and `normalize()` moves points in merged text nodes to their parent.
- **Selection** is script state with one range and a direction
  (`extend`, `setBaseAndExtent`, `selectionchange`). It is not the
  selection the user makes with the mouse (`page_doc.c`), and it is not
  painted: joining them needs a mapping between DOM offsets and
  `AuTextPos` (processed text offsets) in layout.
- **CSSOM** (`prelude_cssom.js`). `element.sheet` and
  `document.styleSheets` give a `CSSStyleSheet` per style or link
  element. Rules are read from the sheet's text
  (`AuJsHooks.sheet_text`, `au_dv_sheet_text` in `docview.c`) by a
  splitter in script; `insertRule`, `deleteRule` and changes to a rule
  write the whole text back (`au_dv_sheet_set_text`, kept in
  `SheetSrc.om_text`), and the document view parses it again, so the
  element's own text does not change. `sheet.disabled` sets
  `SheetSrc.disabled`. A rule's declarations are the `style` attribute
  of an element that is never inserted, so they share the inline style
  code. The text script writes back is parsed into `SheetSrc.om_sheet`,
  which replaces the sheet's own in the cascade; the resolver borrows
  sheets, so `au_dv_sheet_set_text` detaches them before it frees the
  old one. A link element's sheet is listed and can be disabled, but its
  rules throw NotSupportedError: the document view frees a fetched
  sheet's text once it is parsed (`parse_sheet`), so there is nothing to
  read them from. Not done: constructed sheets and
  `adoptedStyleSheets`, the sheets of `@import` rules, rules nested in a
  style rule, `@font-face` rules added through the CSSOM (they are not
  given to the web font loader); a cross-origin sheet's rules throw
  SecurityError.
- **getComputedStyle** returns the used width and height in pixels
  (`computedValue` in `prelude_dom.js`, through `N.rect`, which forces a
  layout), takes `::before`, `::after` and `::marker`, and reads
  `white-space` as one keyword. Margins, insets and percentages in other
  properties are still computed values, `transform` is not a matrix, and
  the `border` and `font` shorthands serialize every part.
- **crypto** (`getRandomValues`, `randomUUID`; no `subtle`) exists only
  when the embedder passes `AuJsHooks.random`
  (`AuDocViewCallbacks.random` for a document view). `aura-host js`
  reads `/dev/urandom`. The browser does not pass one yet: to give pages
  `crypto`, set the callback in `page_doc.c` from the platform's entropy
  source (`AuNetOps.entropy`).
- **Start-up cost.** `aura-host js --stats` reports 21 to 24 ms and
  1.66 MB to start a runtime on the host; 18 to 21 ms of it compiles the
  293 KB prelude and 2 to 4 ms runs it. The prelude is not precompiled
  to bytecode yet: to do it, compile with `JS_EVAL_FLAG_COMPILE_ONLY` in
  a host tool at build time, write it with `JS_WriteObject`
  (`JS_WRITE_OBJ_BYTECODE`; QuickJS-ng swaps bytes itself when reading
  on a big-endian machine), load it with `JS_ReadObject` and
  `JS_EvalFunction`, and check JSTest in the guest.

## Forms

Form controls keep their state in C, so typing, clicks and submission
work with script off, and script reads and writes the same state.

- **Control state.** `src/dom/control.c` gives each input, textarea,
  select and option an `AuControl` on first use (`au_ctl`): the UTF-8
  value and its dirty flag, the selection as byte offsets (anchor and
  caret), scroll offsets, one undo step, the value at focus (for
  change), the custom validity message, chosen files, the list box
  cursor and the pressed flag. Checkedness is the element state
  `AU_STATE_CHECKED` with `AU_STATE_CHECKED_SET`. Layout reads the value
  at paint time, so typing repaints without a box tree rebuild.
- **Painting.** `src/layout/controls.c` paints text fields and textareas
  (placeholder, password bullets, selection, a one-pixel caret, soft
  wrapping and scrolling) and maps points to offsets
  (`au_layout_field_offset_at`, `au_layout_field_caret_rect`). A
  textarea is a replaced box; its children are not laid out.
- **Editing.** `src/page/editing.c` is the portable model: insertion
  with maxlength and single-line rules, deletion by character and word,
  movement, word and line boundaries, and undo (Undo twice redoes).
- **Input.** `src/page/forms.c` turns keys and mouse events into focus
  changes, edits and activation: click and Tab focus (positive tabindex
  first, then tree order), autofocus at the end of parsing, the caret
  blink (`au_docview_blink`, which repaints only the caret), checkbox,
  radio group, select, list box, range, file, details and label
  behaviour, implicit submission, and the Edit commands
  (`au_docview_edit`). With the JavaScript runtime running, a click's
  activation runs in the bindings and reaches C through `N.submitForm`;
  without it `au_forms_activate` does the same in C.
- **Submission.** `src/page/formdata.c` builds the entry list
  (`au_form_entries_build`) and encodes it (urlencoded, multipart with
  file parts, text/plain), and checks validity (`au_form_validity`;
  pattern goes through the script RegExp when the runtime runs).
  `form_submit` in `forms.c` applies the formaction, formmethod,
  formenctype, formtarget and formnovalidate attributes, asks before an
  https page sends a form over http, and passes an `AuFormSubmission` to
  `AuDocViewCallbacks.submit`. `page_doc.c` sends GET through
  `open_url` and POST to the browser (`submit_post`), which adds a
  history entry and calls `au_page_navigate_post`; the fetcher sends the
  body. Reloading a POST result asks first (`AuBrowserHost.confirm`); a
  history entry of a POST result loads with GET.
- **Script.** `src/js/natives_forms.c` exposes the control state
  (`N.ctlValue`, `N.ctlSel`, `N.ctlFiles` and so on), converting
  selection offsets between UTF-16 and UTF-8. `prelude_html.js` builds
  the DOM API on them (value, selectionStart, setRangeText, files,
  validity, requestSubmit) and the legacy named access
  (`document.formName`, `form.controlName`, `form[0]`, RadioNodeList)
  through a proxy between `Node.prototype` and `EventTarget.prototype`.
  Events go in the standard order: keydown, beforeinput, input, keyup,
  change at blur or implicit submission, submit.
- **Mac OS 9 shell.** `browserwin.c` maps typed Mac Roman characters
  to Unicode and passes every key to the page when the address field
  does not have the focus; `main.c` routes Command-arrows and
  Command-Delete to the window instead of MenuKey. The Edit menu acts on
  a focused page field (`page_field_edit`, TEXT scrap in Mac Roman).
  Selects use `PopUpMenuSelect` (`host_popup`), file inputs
  `StandardGetFile`, confirms ALRT 136, and the I-beam shows over text
  fields (`au_bw_text_field_at`).
- **To drive forms on the host**, use `aura-host chrome` with `--do`
  actions, run in order after the page loads: `clickname:NAME` (the
  element with that name attribute), `click:X,Y`, `type:TEXT`,
  `key:[cmd-][shift-][opt-]NAME`, `edit:copy|cut|paste:TEXT`,
  `wait:MS`, `shot:FILE.png` and `console`. For example, `aura-host
  chrome --net --tab https://html.duckduckgo.com/html/ --do clickname:q
  --do "type:mac os 9" --do key:return --do wait:3000 -o run/ddg.png`.
- **What a keystroke costs.** A character typed into, or deleted from,
  a text field that stays non-empty restyles nothing, builds no box
  tree, runs no layout and invalidates the field's rectangle only.
  `edited` in `forms.c` restyles only when `field_style_state` changes:
  the field becomes empty or stops being empty (`:placeholder-shown`),
  or its validity bits change. Enter also fires keypress (code 13)
  before implicit submission. To measure, run `aura-host chrome --tab
  PAGE --do clickname:NAME --do keys:TEXT`: for each key it prints the
  time in the event, the idle pass and painting, and the counts from
  `au_docview_stats` (style passes, elements styled, tree builds,
  layouts) and the pixels invalidated. In the guest, focus a page field
  and choose Develop > Page Timings: it types 16 keys and shows
  milliseconds per key with the restyle and layout counts.
  `test_keystroke_cost` in `tests/forms` asserts zero of each on a page
  with `:has()` rules, with and without script listeners. Measured in
  the guest on a 400-paragraph page with `:has()` rules: 61 to 124 ms
  per key before (a restyle of every element and a layout per key),
  4.5 ms after.
- **What script does per key still costs.** A change in a subtree that
  is not in the document marks nothing dirty (`h_dom_changed` in
  `script.c`), and attributes the box tree never reads (`aria-*`,
  `data-*`, `role`, `title`, `tabindex`, a hidden input's `value`)
  restyle without a box tree rebuild (`au_dv_dom_changed`). While a key
  is handled, `au_dv_turn_end` does not invalidate the page for what
  listeners changed: `au_docview_key` updates at its end and invalidates
  the page only if the update changed geometry or raised a paint, so a
  restyle that computes the same values repaints the field only
  (`test_keystroke_cost`, "attribute writers"). Open: on live Wikipedia
  (`aura-host chrome -w 1440 -h 900 --net --tab
  https://en.wikipedia.org/wiki/Mac_OS_9 --do clickname:search --do
  keys:abcdef`) a key restyles 2 elements (the hidden inputs whose value
  the page sets), but a box tree build and a layout still run on every
  key and the viewport is repainted; no DOM change other than those two
  attributes is reported, so the build is asked for inside layout
  (`au_layout_styles_changed` deciding on a rebuild, or
  `au_layout_images_changed`). Find which before changing anything.
- **Mac OS 9 key handling.** `bw_key` does not paint: the event loop in
  `main.c` handles up to 8 queued keys and then paints once
  (`idle_all`).
- **Tests.** `tests/forms` (editing, encodings, focus, submission,
  validation, event order), `tests/js/forms.js` (bindings and named
  access), the forms part of `tests/browser/test_net_page.c` (GET and
  POST over the fake transport) and `tests/page/samples/qa-forms.html`
  (every control, and a Try it form that shows what it sent).
- **Not done:** date, time and color pickers (they are text fields), the
  dirname attribute's direction, a resend prompt for POST history
  entries, form-associated custom elements, and IME input.

## Frames

`iframe`, `frame`, `object` and `embed` elements show nested documents.
To change them, work in `src/page/frames.c` (the frame list, loading,
painting, input, limits), `src/js/frames.c` and the "windows of other
documents" part of `src/js/prelude_window.js` (script), and the frame
parts of `src/browser/page_doc.c` (fetch context, selection, hit
information).

- **Structure.** Each frame element has an `AuFrame` in the view of the
  document that holds it (`AuDocView.frames`, tree order). A frame that
  shows a document owns an `AuDocView` for it, with its own document,
  style, layout, images and script context. The view is created only
  when there is content to show or script asks for the frame's window,
  so an empty iframe costs one record. `au_frames_sync` brings the list
  in step with the tree: after each parser chunk, before a style pass,
  and at the end of any script native that inserted, removed or changed
  a frame element.
- **The embedder sees one view.** Nested views load through the
  top-level view's loader and report through its callbacks, always with
  the top-level view as `dv`. `au_docview_set_request_ex` gives the
  loader the requesting document's URL and a body (`AuResourceRequest`);
  data for any request is delivered to the top-level view, which passes
  it on (`FrameRoute`). Input goes to `au_docview_mouse`, `_key` and
  `_scroll_at` of the top-level view; `au_docview_view_at` and
  `au_docview_focus_view` name the nested view under a point or with the
  keyboard, for code that works on one document (the page's selection,
  hit information, the Edit commands, the caret).
- **Loading.** A frame's document is an `AU_RES_FRAME` request of the
  owner view (`AU_FETCH_FRAME` in the page, `AU_DEST_IFRAME` in the net
  loader). The response head decides what the element shows: HTML or
  text is a nested document, an image is a generated document with an
  `<img>` (iframe) or an image box in layout (object, embed), anything
  else nothing. `srcdoc`, `about:blank` and `data:` URLs need no
  network; `page_doc.c` decodes `data:` URLs itself, for scripts too.
  In `page_doc.c` the requesting document is the referrer and decides
  mixed content (an http frame in an https page is blocked by the net
  loader), and the tab's URL is the top-level site for cookies and the
  blocklist (a listed third-party frame host is not contacted and counts
  in `AuPageStats.blocked`).
- **Framing rules** (`framing_allowed`): `X-Frame-Options` DENY and
  SAMEORIGIN, and the CSP `frame-ancestors` directive with `'none'`,
  `'self'`, scheme and host sources, each checked against every
  ancestor; with `frame-ancestors` present `X-Frame-Options` is ignored.
  A refused frame is empty and the console names it. No other CSP
  directive is enforced anywhere (`script-src`, `frame-src`, `sandbox`,
  `default-src` and the rest are ignored), and neither is the
  `Permissions-Policy` header or the `allow` attribute.
- **Sandbox.** The `sandbox` attribute turns off scripts (unless
  `allow-scripts`), form submission (`allow-forms`), the parent's origin
  (`allow-same-origin`; without it the document's origin is opaque, so
  it and its parent cannot reach each other), navigation of anything but
  itself and its descendants (`allow-top-navigation`) and new tabs
  (`allow-popups`). A nested document inherits its ancestors'
  restrictions. Other tokens are ignored.
- **Limits.** 64 nested documents per page (`AU_FRAMES_MAX`), 8 levels
  (`AU_FRAMES_DEPTH`); a frame whose URL is that of one of its ancestors
  is refused; `loading=lazy` frames load when they come within a
  viewport of the visible area or are painted, and do not hold the
  page's load event. A refused frame stays an empty box and still gets
  its load event.
- **Geometry.** Layout makes the element a replaced box (300 x 150 by
  default) and calls `AuLayoutEnv.paint_frame` with its content box;
  `au_frames_paint` sets the nested view's viewport to the box, paints
  it there with the frame's own scroll offset (`AuFrame.sx`, `sy`) and
  draws the scroll bars (as the page's overflow scrollers, unless
  `scrolling=no`). A nested document's canvas is transparent unless its
  root or body has a background (`au_layout_set_transparent`). Rectangles
  a nested view invalidates are moved into the owner's coordinates and
  clipped to the box. When the page scrolls by moving pixels a frame
  moves as a box does; content fixed inside a frame is fixed to the
  frame (`frames.html` in `tests/browser/test_scroll_shift.c`).
  `AuLayoutEnv.embed_kind` tells layout whether an object or embed is a
  nested document, an image or its fallback content. `frameset` is a
  flex container in the UA sheet: rows or columns of equal size (the
  `rows` and `cols` lengths are not read).
- **Input.** A press inside a frame goes to the nested view, which keeps
  the drag and the release, and gives the frame the keyboard
  (`AuDocView.focus_frame`) until a press elsewhere. Keys the nested
  document does not use scroll the frame, and at its end the page. The
  wheel scrolls the innermost scroller under the pointer, then the
  frame, then the page. Links and forms follow `target`: a frame's name,
  `_self`, `_parent`, `_top`, `_blank` (`au_frames_route`); a link in a
  frame without a target loads in that frame.
- **Script.** The documents of a top-level page share one QuickJS
  runtime; each has its own context (`au_js_new_child`), so a
  same-origin document's objects are reached directly:
  `contentWindow`, `contentDocument`, `window.parent`, `top`, `frames`,
  `length`, `frameElement`, indexed and named frames (own accessor
  properties of the window, set by `P.framesChanged`). The memory limit,
  the stack limit and the time budget are the runtime's. A node's
  wrapper takes its prototypes from the realm of the node's document.
  For a document of another origin `wrapWindow` returns an object with
  only `postMessage`, `location` (set only), `closed`, `parent`, `top`,
  `frames`, `length`, indexed frames, child frames by name, `focus`,
  `blur` and `close`;
  anything else throws SecurityError, and `contentDocument` is null.
  `postMessage` checks the target origin, the receiving document clones
  the data in its own realm, and `event.source` is the sender's window
  as the receiver may see it. `MessagePort`s in the transfer list move
  to the receiving document (reCAPTCHA-style widgets hand their parent
  a port this way). `about:blank` and `srcdoc` frames have their
  parent's origin. A frame with script off (a sandbox without
  `allow-scripts`, or the JavaScript preference) has no window object:
  `contentWindow` is null.
- **Lifetime.** Script may remove or navigate a frame while that frame's
  script, or any script of the page, is on the stack. Nothing is freed
  then. A removed frame leaves its owner's list at once (script sees no
  window) and waits in the top view's `dead` list; navigations, restarts
  and load events wait in the frames; `au_frames_idle` does the work
  when no script of the page runs (`safe`). `au_js_free` of a frame's
  context leaves the objects other documents still hold valid: wrappers
  belong to the runtime, the freed context's natives then act for the
  top-level document (its opaque pointer is set to the root), and the
  frame's document lives while a wrapper names one of its nodes. So a
  held reference to a removed frame's function still runs, but
  `location`, timers and `document.URL` reached through a dead window
  answer for the top-level document. Free a frame's `AuJs` before the
  top-level one, and never while `au_js_running`.
- **The document cache.** A kept document keeps its frames.
  `au_docview_can_persist`, `au_docview_set_hidden` (pagehide, pageshow,
  dropped image pixels) and `au_docview_memory_estimate` include them; a
  page with a frame still loading is not kept.
- **History.** A frame navigation adds no session history entry: Back
  leaves the page, and a kept page comes back with each frame's current
  document. `history.back()` inside a frame acts on the tab.
- **Also here.** `document.open()` on a loaded document followed by
  `write` replaces the document's contents with the parse of what was
  written (the same Document and window; scripts in the written text do
  not run; a `write` without `open` after load is still ignored).
  `DOMImplementation.createDocument` and `createDocumentType` make plain
  documents and doctypes. `fetch` and `XMLHttpRequest` send POST bodies
  through `request_ex` (the Content-Type header goes with the body;
  other request headers and other methods are still not sent).
- **Cost, measured on the host** (Release): a frame without script adds
  about 6 KB to the estimate and 0.15 ms; a frame with script adds a
  context with its own copy of the prelude, 1.4 MB and 16 ms (57 ms each
  with 20 of them, as collections of the growing heap add up). The
  runtime's 32 MB limit bounds the total: about 20 scripted frames.
- **To try it**, serve a page with frames from a local HTTP server and
  run `aura-host chrome --net --tab URL --do wait:500 --do click:X,Y
  --do type:TEXT -o out.png`; `--do js:CODE` reaches frames through
  `frames[0]`. `aura-host page PAGE.html` loads file: frames too.
- **Tests.** `tests/frames/test_frames.c` (the document view with a
  table of pages: paint against a reference render, scrolling, clicks
  and keys, script access both ways, postMessage and ports, sandbox,
  framing headers, limits, lazy loading, objects, removal and navigation
  while script runs, a kept document, a cross-origin widget) and
  `test_frames_net.c` (a browser window over the fake transport:
  referrer, blocklist, X-Frame-Options from a real response, typing and
  Edit commands in a frame, Back to a kept page with frames). Run both
  under the sanitizers after a change to lifetimes.
- **Acid3.** `run/acid3` is not in git: fetch
  `http://acid3.acidtests.org/` and the files it names once, keep each
  response's status and Content-Type, and serve them from a local HTTP
  server (the 404 answers matter). The score went from 59 to 87 with
  frames. Still failing: 2 (a NodeIterator whose filter removes nodes),
  21, 23 and 25 (namespace prefixes: `tagName` with a prefix,
  `createElementNS` name checks), 35 (`:first-child` matches a root
  without a parent), 46 (a media query case), 69, 74 and 79 (XML and SVG
  documents in frames: they are shown as text, not parsed into a DOM),
  72 (`cssRules` is not live), 75 and 77 (SVG DOM interfaces), 80
  (script in an XHTML document).
- **Not done:** XML documents in frames (`text/xml`,
  `application/xhtml+xml`, `image/svg+xml` are shown as text; the XML
  parser in `src/svg` builds SVG trees only); session history for frame
  navigations; a nested document's own Referrer-Policy and the
  `referrerpolicy` attribute (the top-level policy is used); frame
  scroll bars cannot be dragged (wheel and keys only); `rows` and `cols`
  sizes of a frameset; Find searches one document (the page, or the
  frame the selection is in) and Tab does not move between a page and
  its frames; invalidation rectangles of a frame inside a transformed
  or scrolled container use the frame's untransformed box; the
  WindowProxy of a frame is a new object after each navigation; POST
  into a frame works only with a loader that implements `request_ex`.

## Design

The decisions and milestones are in the Gandalf vault,
`project:aura:decisions`.

The scope of version 1 (acceptance sites, required features, what is left
out) is in `docs/v1-scope.md`. Check a feature against it before building
it.
