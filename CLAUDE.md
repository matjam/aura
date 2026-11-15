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

## Layout

| Folder | Holds |
|---|---|
| `src/core` | allocation (and the pooled allocator), buffers, UTF-8, atoms |
| `src/dom` | the node tree |
| `src/html` | HTML tokenizer and tree builder |
| `src/css` | CSS parser, selectors, cascade |
| `src/layout` | box tree, layout, painting, hit testing |
| `src/page` | the document view: parsing, style sheets, resources, input |
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
| `third_party` | QuickJS-ng (`quickjs`, with `quickjs-patches` and `quickjs.cmake`) and BearSSL 0.6; `third_party/README.md` records sources and licences |
| `tests/<area>` | one test area per folder, each with a `CMakeLists.txt` |
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
  of `assets/fonts`, `fonts.idx` and the fonts' README).
- To run the tests under AddressSanitizer and UBSan, configure a separate
  folder: `cmake -S . -B build/asan -G Ninja
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined"`, then `cmake --build
  build/asan` and `ctest --test-dir build/asan`.

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
  `~/.local/opt/retro68` keyed on the hash of `toolchain/build.sh`.
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
Latin-1). HFS names are at most 31 characters.
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
and pick 256 Colors, Thousands or Millions. In Aura, View > Debug >
Benchmark times the scene, fills and the chrome and shows how long font
registration took (and the bundled part of it) and how much font data
is loaded; View > Debug > Font List (about:fonts) shows the fonts it
found, with the bundled ones marked; and View > Debug > Page Timings
shows the active page's first paint, load and layout times, image
decodes, then times scrolling: ten 32-pixel steps (twice: the first pass
paints content for the first time, the second shows the steady state),
five page downs and three repaints of the whole page, each split into
moving the offscreen pixels, painting, copying to the screen and the
number of areas painted, and shows free heap, temporary memory and the
glyph cache. Network Status shows the HTTP cache's lookups (fresh,
history, revalidated, 304, missed) and the time spent reading and
writing cache files.
The Debug submenu opens by dragging (a click closes it); the
`QMP.button` and `QMP.move` methods of `tools/guest-qmp.py` can hold
the button through View, Debug and the item. Release on the item's
own row: the submenu's first item is Test Scene. While Aura is busy
(a long layout), the drag is lost; wait and try again.

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
  articles took 30 to 70 seconds. View > Debug > Page Timings shows
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
  `rect_hits` in `draw.c` allows one pixel above and below, because a
  text fragment is as tall as the face's rounded ascent and descent and
  glyphs reach up to half a pixel past it.
- Drawing must not depend on the scroll offset, or moved pixels differ
  from a fresh paint by a coverage level on antialiased edges.
  `au_layout_paint` puts the whole pixels of the scroll offset in the
  canvas translation and paints boxes at document coordinates, and
  `raster()`, `fill_rect_source`, `au_draw_border` and
  `au_draw_box_shadow` add the whole pixels of the translation to their
  integer bounds instead of to the coordinates. Keep new drawing code to
  that rule: take the fraction of the translation into the coordinates
  and add its whole pixels when writing pixels. `paint_svg` does the same
  for an inline SVG's place. To check a page, render it with `aura-host
  render PAGE -s Y` at several offsets and compare the rows that both
  images show.
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
session only, block) live in the `cookies.sites` preference. View >
Debug > Network Status shows entropy, Open Transport counters, roots,
cookies and cache use. The out-of-memory handler closes Open Transport
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
  viewport units reach layout as style changes. The restyle itself is
  skipped or partial when the cascade says so (see CSS, "A viewport
  change").
- **An image change** (`au_layout_images_changed`: a size became known,
  an image failed) keeps the box tree. Each image box asks the loader
  again (`l_image_refresh`); a box whose size depends on the image is
  marked for layout, and one with a width and height in pixels is only
  repainted. The tree is built again only while its shape depends on
  images (`AuLayout.images_shape`: an object or embed element with a
  resource, a list-style-image that is not sized yet). The document view
  repaints the boxes that show a changed image
  (`au_layout_image_users` names the boxes with it as a background or
  mask) and the whole page only when layout changed geometry.
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
  cache. `test_image_arrival_cost` in `tests/page/test_page.c` checks
  that an arriving image builds no tree, lays out only when its size
  matters and invalidates the boxes that show it.
- **Measured** on a Wikipedia article of 18,053 boxes. Host (CPU time):
  width change in layout 2.6 ms (3.7 before), paint of a 1440x900 view
  0.92 ms (1.22), first paint of a page of new text 1.36 ms (1.68), a
  32-pixel strip 0.046 ms (0.061), a one-pixel paint 0.005 ms: painting
  already costs in proportion to what it shows. A width change through
  the document view took 18 ms, of which the restyle of every element
  was 15 ms; on live Wikipedia (`aura-host chrome --do resize:W,H`) a
  resize that crosses no media query breakpoint now spends 1 to 2 ms in
  style and layout, and one that crosses a breakpoint 15 to 17 ms. Guest (emulated, stock base, 600 pixels wide): the last
  style and layout pass of the load 465 ms (11,251 ms before the pooled
  allocator), a whole-page paint 4 to 5 ms (54), a page down 4 to 11 ms
  (349), a window zoom two passes of about 280 ms each.
- **Not done:** text is segmented and measured again for each layout of
  its block (the word cache answers the widths); `l_layout_abspos_for`
  removes queue entries with `memmove`; a DOM insertion or removal
  builds the whole box tree again (the search suggestions on Wikipedia
  cost a tree build each time the list changes).

## Layout behaviour

Cases are in `tests/layout/cases/*.txt` (format in `test_layout.c`; the
test loader serves `data:` URLs, so cases can use images). Where a rule
below names Blink, it comes from reading Blink's source and from CSS
specifications, not from running Chrome beside Aura.

- **Font metrics are whole pixels.** `l_font` rounds a face's ascent,
  descent and line gap each to a whole pixel, as Blink's
  `SimpleFontData::PlatformInit` does, so `line-height: normal` is their
  sum (16px Aura Sans, with Arial's metrics: 14 + 3 + 1 = 18px, the
  height Chrome gives 16px Arial). The leading above an inline box is
  half the difference to the line height rounded down, the rest goes
  below (`half_leading` in `inline.c`, Blink's `CalculateLeadingSpace`).
  Fallback faces are rounded the same way in
  `au_glyph_run_fallback_extents`.
- **Line breaking.** A `<wbr>` is a break opportunity whatever
  `white-space` says (Blink and Gecko break there inside nowrap text).
  Emoji break like ideographs (UAX #14 class ID): before and after each,
  and between a letter and an emoji. `l_break_class` gives `BK_EXTEND`
  to combining marks, variation selectors, ZWJ, emoji modifiers and tag
  characters, which stay with what precedes them (and a ZWJ joins what
  follows), and `BK_RI` to regional indicators, which break only between
  pairs. The emoji set is the pictograph blocks U+1F000 to U+1FAFF and
  the BMP characters with emoji presentation; it is not generated from
  the Unicode data files.
- **Containing blocks.** A box with a transform, `translate`, `rotate`,
  `scale`, `perspective`, a filter, `backdrop-filter`, layout or paint
  containment, or `will-change` naming one of those is the containing
  block of its absolutely positioned and fixed descendants
  (`l_style_contains_fixed`, `LF_TRANSFORM`). A fixed box under one
  scrolls with the page.
- **Rounded clips.** A box with `border-radius` and overflow clipped on
  both axes clips positioned descendants to its rounded padding box:
  `layer_origin` in `draw.c` paints the layer into a group that
  `layer_clips_pop` masks, unless the layer's ink stays clear of the
  corners.
- **`background-attachment: fixed`** positions a layer against the
  viewport on any box. Once a box has painted one,
  `au_layout_scroll_damage` returns -1 and a scroll repaints the page.
- **Clearance.** A block placed by clearance takes the top margins of
  its first children into its own top margin, which lies above the edge
  clearance placed (`LMargin.absorb` in `block.c`); an empty cleared
  block still lets the next block's margin apply.
- **Images.** `object-fit` and `object-position` are applied when an
  image box is painted. `list-style-image` makes the marker an image box
  with a 7px gap after it (Blink's marker padding), or the
  `list-style-type` marker when the image fails. `img_source` in
  `boxtree.c` chooses among `srcset` candidates for one device pixel per
  CSS pixel (the lowest density of at least 1; width descriptors against
  `sizes` and the viewport) and among the `<source>` elements of a
  `<picture>` (media and type), and the natural size is divided by the
  candidate's density. `au_layout_image_source` gives the document view
  the same choice. A choice that depends on the viewport makes a
  viewport change build the box tree again. Not done: `currentSrc` in
  script, `image-set()`, and `sizes` lengths other than px, vw, vh, em
  and rem.
- **Text offsets for script.** `au_layout_text_pos_from_dom` and
  `au_layout_text_pos_to_dom` (`selection.h`) convert between a text
  node's UTF-16 offsets and `AuTextPos` by processing the node's data as
  the box tree did (`l_text_process` with its offset map), and
  `au_layout_range_rects` gives a range's rectangles per line.
- **Acid2** (http://acid2.acidtests.org/#top, compared with its
  reference image at the scroll position of `#top`): the rows that still
  differ are four rows of the eyes (rows 51, 53, 55 and 57 of the
  168-pixel face: the Adam7 PNG shows the row above repeated in image
  rows 1 to 9 in `aura-host render`, though `aura-host image` decodes
  the file correctly; the cause is not found) and two pixels per row
  along the nose's diagonal border joins (rows 72 to 95, where the red
  background shows through the antialiased seam between two border
  colours).

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
  line height ignores fallback faces. Layout rounds these metrics to
  whole pixels (see Layout behaviour).
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
- **A viewport change.** The rule index records every media query it
  evaluates with its result (`media_dep` in `cascade.c`), and a computed
  style that used a viewport unit has `AU_STYLE_F_VIEWPORT_UNITS`.
  `au_css_resolver_set_device` with a device that differs only in the
  viewport size evaluates the recorded queries again: when none changed,
  the index is kept and `au_css_restyle` computes only the elements
  whose style (or pseudo-element style) has the flag, and what inherits
  from them; with no such style it computes nothing.
  `au_css_resolver_viewport_restyle` says which of the three applies,
  and the document view skips the style pass on `AU_CSS_VIEWPORT_NONE`.
  Any other device difference, or a media query that changed, restyles
  the document as before. Container queries are not evaluated, so
  nothing is recorded for them. `test_css_invalidate --viewport DIR`
  (ctest `css_viewport`) resizes pages through fourteen sizes and
  compares every computed value with a document styled from scratch.
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
  border-image in layout. At 256
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
  View > Debug > Network Status shows the totals, where the list memory
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
interface: one `AuJs` per document, holding a runtime, a context whose
global object is the window, the DOM bindings and the event loop.

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
  (`au_page_console_line`) for a debug view.
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
  and compares only its origin. Google's search box defers its submit
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
  `replaceData`. `getClientRects` and `getBoundingClientRect` give the
  rectangles of the selected part of each text node, one per line,
  through `N.textRects` and the `text_rects` hook (`h_text_rects` in
  `script.c`, on layout's `au_layout_text_pos_from_dom` and
  `au_layout_range_rects`); a collapsed range in text gives one
  rectangle of no width. Not exact: `normalize()` moves points in merged
  text nodes to their parent.
- **Selection** is script state with one range and a direction
  (`extend`, `setBaseAndExtent`, `selectionchange`). It is not the
  selection the user makes with the mouse (`page_doc.c`), and it is not
  painted. To join them, convert each boundary point with
  `au_layout_text_pos_from_dom` and `au_layout_text_pos_to_dom`
  (`src/layout/selection.h`); that is not wired yet.
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
  and choose View > Debug > Page Timings: it types 16 keys and shows
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
  (`test_keystroke_cost`, "attribute writers"). On live Wikipedia
  (`aura-host chrome -w 1440 -h 900 --net --tab
  https://en.wikipedia.org/wiki/Mac_OS_9 --do wait:4000 --do
  clickname:search --do wait:3000 --do keys:macint`) the box tree build
  on a key came from the suggestion list's thumbnails: an image whose
  size became known made layout build the tree again and the view
  invalidate the page, at the next update, which a key runs. An image
  change now keeps the tree (see Layout and paint speed), and a key
  there restyles 2 elements, builds no tree, runs no layout and
  invalidates the field (439 x 41 pixels). What remains is the page's
  own work: when the suggestion list changes, its DOM insertions build
  the tree again, and keys typed while the page still loads its search
  module pay for that loading.
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

## Design

The decisions and milestones are in the Gandalf vault,
`project:aura:decisions`.

The scope of version 1 (acceptance sites, required features, what is left
out) is in `docs/v1-scope.md`. Check a feature against it before building
it.
