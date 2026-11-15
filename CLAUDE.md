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
  They do not return NULL.
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
| `src/core` | allocation, buffers, UTF-8, atoms |
| `src/dom` | the node tree |
| `src/html` | HTML tokenizer and tree builder |
| `src/css` | CSS parser, selectors, cascade |
| `src/layout` | box tree, layout, painting, hit testing |
| `src/page` | the document view: parsing, style sheets, resources, input |
| `src/paint` | surfaces, rasterizer, fonts |
| `src/browser` | browser model and chrome: tabs, history, bookmarks, downloads, preferences, the page interface and its implementation on the document view |
| `src/platform/host` | host tools: headless render to PNG, tree dumps |
| `src/platform/macos9` | the Mac OS 9 application |
| `tests/<area>` | one test area per folder, each with a `CMakeLists.txt` |
| `assets/fonts` | the bundled fonts, their licences and `fonts.idx` |

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
Mac OS takes about a minute to boot.

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
shows the active page's first paint, load and layout times, times ten
32-pixel scroll repaints, and shows free heap and temporary memory.
The Debug submenu opens by dragging (a click closes it); the
`QMP.button` and `QMP.move` methods of `tools/guest-qmp.py` can hold
the button through View, Debug and the item.

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
was left at. To plug in networking, register a fetcher per scheme with
`au_browser_set_fetcher` (see `AuPageFetcher` in `page.h`); without one,
http and https show a placeholder after a simulated load. Image sizes
come from the file headers (PNG, GIF, JPEG, BMP); there are no decoders
yet. The font preferences reach the engine as the generic families of
the font set and, for the default and monospace sizes, the minimum size
and the tab's text zoom, `au_docview_set_font_prefs` (which passes them
to `au_css_resolver_set_font_prefs`). Text zoom is per tab
(`au_bw_zoom`, View > Zoom In, Zoom Out, Actual Size); the last zoom
chosen for a site (host, or scheme for `file:`) is kept in the
`zoom.sites` preference and applied when a tab opens that site again.

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

The Mac OS 9 shell (`src/platform/macos9`) keeps one module per kind of
window (`browserwin.c`, `listwin.c`, `prefswin.c`), with menus and the
event loop in `main.c` and files in `storage.c`. The address field is a
TextEdit record placed over the field the engine draws; the scroll bars
and menus are native; the Preferences window uses Appearance Manager
controls, declared in `appearance.h` because the Multiversal Interfaces
lack them, from AppearanceLib.

Aura keeps its files in the Aura folder of the system Preferences folder:
`Aura Preferences`, `Bookmarks.html` (Netscape format), `History`,
`Session` and `Downloads`. Each is written to a `.tmp` file and swapped
in; a file that fails to parse is renamed with ` (damaged)` before a new
one is written. To start with clean state in the guest, delete that
folder.

Downloads use a simulated transfer source (`au_sim_source` in
`downloads.c`) until networking exists; View > Debug > Start Simulated
Downloads adds a few.

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
  text face) and Noto Emoji (monochrome emoji), about 3.4 MB.
  `assets/fonts/README.md` records sources, versions, licences, sizes
  and what was left out.
- **To add a font**, put its folder (licence included) in
  `assets/fonts`; reduce it with `tools/fonts-prepare.py` or pyftsubset
  to TrueType outlines (no CFF, no variable fonts, no colour tables),
  keeping file and folder names to 31 characters; regenerate the index
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
  the emoji list first. Default-ignorable code points draw nothing, so
  U+FE0F is dropped and ZWJ sequences draw their components; skin tone
  modifiers are dropped. Marks are placed over their base by their ink
  boxes (the fonts carry no GPOS for Aura to read). Fallback faces keep
  the requested weight and style, synthesized when missing. A per-list
  cache and a per-set cache keyed by code point and style keep this
  fast; a face is loaded only when it is tried.
- **Line height.** With `line-height: normal`, a line holding fallback
  glyphs grows to the fallback face's ascent and descent (each with half
  its line gap) where they exceed the primary's, as Blink does; a fixed
  line height ignores fallback faces.
- **Not done:** OpenType shaping (GSUB, GPOS), so no ligatures, Arabic
  joining, Indic reordering or bidirectional text; colour emoji; a CJK
  font pack (see the fonts README for what one needs).

## Design

The decisions and milestones are in the Gandalf vault,
`project:aura:decisions`.

The scope of version 1 (acceptance sites, required features, what is left
out) is in `docs/v1-scope.md`. Check a feature against it before building
it.
