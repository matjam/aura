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
| `src/layout` | box tree and layout |
| `src/paint` | surfaces, rasterizer, fonts |
| `src/browser` | browser model and chrome: tabs, history, bookmarks, downloads, preferences, the page interface and its stub |
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
  `../os9-protect/toolchain/build/toolchain`.

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
the background. The first start copies os9-protect's
`disks/os9-vmoff.qcow2` (read only) into `run/os9-base.qcow2` and boots an
overlay on the copy. Disk writes are discarded when QEMU exits; set
`PERSIST=1` to keep them. Mac OS takes about a minute to boot.

To copy the application in, build it with `./build.sh ppc` and run
`tools/guest-push.sh -n AuraTest build/ppc/src/platform/macos9/Aura.bin`.
The files arrive as a CD named AuraTest on the guest's desktop.

To launch it, run `tools/guest-qmp.py open AuraTest Aura`. To take a
screenshot, run `tools/guest-qmp.py screenshot run/NAME.png`. To type, use
`tools/guest-qmp.py key cmd-q` or `type TEXT`; to click or drag, use
`click X Y` or `drag X1 Y1 X2 Y2` (guest screen pixels).

To compare the window with the host rendering, render the scene at the
window's size (the title shows it) and compare the content area, which
starts at 20,50 on the guest screen:

```bash
build/host/src/platform/host/aura-host scene -w 584 -h 390 -o run/host.png
tools/png-compare.py run/guest.png 20 50 run/host.png run/diff.png
```

To change the screen depth, click the Monitor BitDepth module in the
guest's Control Strip (the eighth icon) and pick 256 Colors, Thousands or
Millions. In Aura, View > Debug > Benchmark times the scene, fills and
the chrome, and View > Debug > Font List shows the fonts it found.

To stop the guest, run `tools/guest-run.sh stop`, or choose Special > Shut
Down in the guest.

## Browser chrome

To change how a browser window looks or behaves, work in `src/browser`:
`chrome.c` lays out, paints and hit-tests the tab strip (or left tab
column), toolbar and status bar; `browser.c` holds windows, tabs and
back/forward lists and routes input; `page.h` is the interface a page
implements, and `page_stub.c` implements it until the real engine
replaces it. The model never calls the OS; the shells pass events in and
show the rectangles it reports through `AuBrowserHost.invalidate`.

To render a browser window on the host, run
`build/host/src/platform/host/aura-host chrome -w 800 -h 600 --tabs left
--tab about:home --tab http://example.com/ -o run/chrome.png`. Run
`aura-host` without arguments for the other options (tab overflow,
hover, address field text).

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
- After Aura has drawn a scroll bar thumb and quit, the next launch in
  the same guest boot can draw thumbs as coloured noise until another
  application draws one. Launching SimpleText and scrolling once clears
  it. The cause is not known yet.

## Design

The decisions and milestones are in the Gandalf vault,
`project:aura:decisions`.
