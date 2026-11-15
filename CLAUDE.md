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
- Commit each working step to `main`. Commit only our own sources. Fonts,
  Apple files and downloaded test suites stay out of git.
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

`CMakeLists.txt` globs `src/**/*.c` into the `aura_engine` library and adds
every `tests/*/CMakeLists.txt`, so a new source file or test area needs no
change to the top-level file.

## Build and test

- `./build.sh test` builds for the host and runs the tests.
- `./build.sh ppc` builds for Mac OS 9 with Retro68 (GCC 16.1, Multiversal
  Interfaces). The toolchain is found through `RETRO68`, then in
  `~/.local/opt/retro68/COMMIT` (the commit is `RETRO68_REV` in
  `build.sh`), then in `../os9-protect/toolchain/build/toolchain`. A
  `build/ppc` folder keeps the toolchain it was first configured with;
  delete it to switch.
  The guest scripts find QEMU through `QEMU`, then in
  `~/.local/opt/qemu/VERSION/bin` (`AURA_QEMU_REV` in
  `tools/guest-env.sh`). The versioned folders follow os9-enforcer's
  convention; its `toolchain/build.sh` and `qemu/build.sh` install them.
  The guest's disk images come from `../os9-enforcer/disks`, or from
  `../os9-protect/disks` when that is absent (`OS9_PROTECT` overrides).

The test machine is the QEMU Mac OS 9.2.2 guest in `os9-protect`
(`run.sh`, `tools/push.sh` to copy files in, `tools/qmp.py` for
screenshots and input). `os9-enforcer/CLAUDE.md` describes how to use it.
The guest has outbound network access.

## Guest testing

Aura has its own test machine, separate from the one os9-protect runs, so
the two never share a disk, a socket or a QEMU process. Runtime files go
in `run/` of the main checkout (set `AURA_RUN` to use another folder);
`tools/guest-env.sh` holds the settings.

To start the guest, run `tools/guest-run.sh start`. QEMU runs headless in
the background. `BASE` picks the system: `stock` (the default) is Mac OS
9.2.2 with virtual memory off and no Enforcer, from os9-protect's
`disks/os9-novm.qcow2`; `enforcer` (`BASE=enforcer tools/guest-run.sh
start`) is `disks/os9-vmoff.qcow2`, which has the os9-enforcer kernel and
extension installed. The first start of a base copies its image (read
only) into `run/os9-base-BASE.qcow2` and boots an overlay on the copy
(`run/os9-aura-BASE.qcow2`). One guest runs at a time; `status` names the
running base. Disk writes are discarded when QEMU exits; set `PERSIST=1`
to keep them. Mac OS takes about a minute to boot.

Test on the stock base first. Use the Enforcer base to check Aura under
Enforcer; its own bugs show there too (see the scroll bar thumb note
under Browser chrome). To read Enforcer's state and offense log, run
os9-enforcer's `tools/xlog-read.py` with
`QMP_SOCK=run/qmp.sock`; the extension on that disk is older than the
script, so the script may report a different layout.

To copy the application in, build it with `./build.sh ppc` and run
`tools/guest-push.sh -n AuraTest build/ppc/src/platform/macos9/Aura.bin`.
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
and pick 256 Colors, Thousands or Millions. In Aura, View > Debug > Benchmark times the scene, fills and
the chrome, View > Debug > Font List shows the fonts it found, and View
> Debug > Page Timings shows the active page's first paint, load and
layout times, times ten 32-pixel scroll repaints, and shows free heap
and temporary memory.

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
the font set and a user style sheet (`html { font-size }`, monospace
size). `src/css` now has `au_css_resolver_set_font_prefs` (default
sizes, text zoom, minimum size); the shell does not call it yet.

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

## Design

The decisions and milestones are in the Gandalf vault,
`project:aura:decisions`.

The scope of version 1 (acceptance sites, required features, what is left
out) is in `docs/v1-scope.md`. Check a feature against it before building
it.
