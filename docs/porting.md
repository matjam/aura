# Porting Aura to another platform

This document says how Aura's code is divided between the shared
browser and each platform's port, what a new port has to write, and
what is still being moved out of the Mac OS 9 port to make that small.
Read it before you add a port or change the boundary between
`src/browser` and `src/platform`.

## Why Aura has more than one port

Aura is a browser for retro computers and legacy operating systems;
Mac OS 9 on PowerPC is the first. Ports to mainstream systems serve that
goal: on a current machine the engine can be run under debuggers and
sanitizers, compared page by page with other browsers, and tested in CI,
and what is fixed there is fixed on the old systems too. They are not
meant to compete with modern browsers.

So when the two pull in different directions, the legacy platforms win:

- The engine stays small enough, and fast enough, for the oldest
  supported machine. A feature that only a mainstream port could afford
  needs a way to leave it out.
- Shared code assumes only what the oldest supported system provides:
  C99, no threads in the engine, no OS calls outside `src/platform`.
- A mainstream port may be plainer than the legacy ones, as long as it
  is native and complete enough to test the engine.

Further legacy platforms can follow the same plan as a mainstream one.

## Platforms

| Platform | Kind | Toolkit | Status |
|---|---|---|---|
| Mac OS 9.1 to 9.2.2 on PowerPC | legacy | Toolbox, Appearance Manager | in development, released |
| Linux on Wayland | mainstream, for testing | GTK4 | done, with a CI build ([`gtk.md`](gtk.md)); packages not started |
| Amiga with an accelerated 68k processor | legacy | Intuition, with the system's own gadget classes | planned |
| Modern macOS | mainstream, for testing | AppKit | planned |

What each planned port will need beyond this document's interfaces:

- **Amiga.** A 68k cross toolchain, and a processor and memory the
  engine can live with (a 68040 or 68060 accelerator with an FPU, and
  tens of megabytes of fast memory; the figures in
  [`memory.md`](memory.md) say what pages need). The network services
  go over `bsdsocket.library` (Roadshow, AmiTCP or Miami), coroutines
  over Exec tasks or stack switching, and files through `dos.library`.
  The engine's big-endian code paths are already exercised by the
  PowerPC build. Measure before choosing a toolkit: Intuition with the
  system's own gadgets keeps the port native without a third-party
  library.
- **Modern macOS.** AppKit for the user interface, with native chrome
  as GTK4 uses it, and the services in `src/platform/posix` (sockets,
  files, clocks, entropy, coroutines) as they are. This is the
  mainstream port for developers on a Mac, beside `aura-host`.

The first port after Mac OS 9 is Linux with GTK4.

## The rule: each port looks native

Each port uses its platform's own toolkit and looks like an application
of that platform: Toolbox and Appearance Manager on Mac OS 9, GTK4 on
Linux, and in future Win32 on Windows or AppKit on macOS. Aura does not
use a cross-platform UI library.

To make that cheap, the shared code describes every part of the user
interface as data and actions, and each port draws that data with its
own widgets:

- **What the shared code owns**: the page's pixels and input, the tabs
  and their state, the address field's text and completion, the menu
  commands with their titles, shortcuts and enabled state, the
  preferences with their labels, types and ranges, the contents of the
  History, Bookmarks, Downloads and Cookies lists, the developer
  windows' text, and every decision about what an action does.
- **What a port owns**: windows, menus, buttons, text fields, lists,
  dialogs, the clipboard, files and folders, the network and clock
  primitives, and turning its input events into Aura's.

A port must not decide what a command does or whether it is enabled,
and must not hold browser state of its own. When a port needs logic
that is not about drawing or the OS, add it to `src/browser` with a
host test.

When a browser feature needs platform code, implement it on every port
in the same change. If you add to a platform interface (a host callback,
an `AuAppNet` hook, a service in `src/platform/posix`), give the Mac OS 9
port and the GTK4 port their implementations in that change, so no port
falls behind and the shared code never tests which port it runs on.

## The layers

| Layer | Where | Depends on |
|---|---|---|
| Engine: HTML, CSS, layout, paint, script, network, TLS | `src/core`, `src/html`, `src/css`, `src/layout`, `src/paint`, `src/js`, `src/net`, ... | nothing outside `src/` |
| Browser model: windows, tabs, history, bookmarks, downloads, preferences, commands, lists, developer tools | `src/browser` | the engine |
| Platinum chrome painter (optional view of the model) | `src/browser/chrome.c` | the browser model |
| Port services: sockets, DNS, clock, entropy, files, coroutines, fonts | `src/platform/posix` for any Unix port (`posix.h`); `src/platform/macos9` for Mac OS 9 | the OS |
| Port user interface | `src/platform/<port>` | the browser model, the toolkit |
| Host test tool `aura-host` (headless; not a port users run) | `src/platform/host` | the browser model, `src/platform/posix` |

The engine, the browser model and `src/platform/posix` build and pass
their tests on Linux and macOS hosts in CI today; a Unix port adds only
its user interface and the few services listed below that are not in
`src/platform/posix`.

CMake builds one platform per build folder, chosen by `AURA_PLATFORM`
(`macos9`, `host`, or `gtk`, which is reserved and stops configuring
until the port exists); see [`building.md`](building.md), "Choosing the
platform". A new port adds its value there and its folder under
`src/platform`.

## What a port writes

A port implements these interfaces. Each is a table of callbacks or a
set of functions declared in the header named.

| Interface | Header | What it provides |
|---|---|---|
| `AuNetOps` | `src/platform/platform.h` | DNS, TCP connect, send, receive, close |
| clock and time zone | `AuNetOps.now_ms` and `wall_time`, `au_js_set_clock` | milliseconds, wall time, UTC offset |
| entropy | `AuNetOps.entropy` | random bytes for TLS and `crypto` |
| application files and clocks | `src/browser/app.h` (`AuAppPort`, `AuAppFiles`) | the profile folder and its `Cache` and `Local Storage` folders: read a named file, write it safely (a temporary file, then replaced), rename a damaged file, remove, test, make a folder, list it; monotonic, wall and microsecond time and the UTC offset. `src/browser/app.c` decides the rest: which files, their formats, loading order, damaged files, when to save, the Running marker and session restore ([`browser-chrome.md`](browser-chrome.md), "Files Aura keeps") |
| network hooks | `src/browser/app.h` (`AuAppNet`) | the `AuNetOps`, and optionally entropy for the page fetcher, the offline reason, the root store's bytes, `AuDownloadFiles`, deferred cache writes, hooks around the loader and the port's System Status lines; `au_app_net_start` builds the stack from them |
| coroutines | `src/core/coro.h` | stacks for workers and scripts that wait |
| fonts | `src/platform/platform.h` (`au_platform_load_fonts`) | the system's and the bundled font files |
| `AuBrowserHost` | `src/browser/browser.h` | redraw requests, change notices, pop-up menus, confirm, file chooser, download confirmation, opening other programs |
| `AuDownloadFiles` | `src/net/transfer.h` | creating and finishing download files |
| system facts | `src/browser/sysinfo.h` | the machine and OS for System Status and bug reports |
| `AuDevWinHost` | `src/browser/devwin_model.h` | for the developer windows: the page shown, the machine's facts, the clipboard, saving a file, the Find dialog |

A Unix port takes these from `src/platform/posix` (link `aura_posix`,
include `platform/posix/posix.h`) instead of writing them:

| Interface | File | Function |
|---|---|---|
| `AuNetOps`: DNS, TCP, clocks, entropy | `net_posix.c` | `au_posix_net_new`, `au_posix_net_ops`, and the main loop calls below |
| clock and time zone | `clock_posix.c` | `au_posix_ms`, `au_posix_us`, `au_posix_wall_time`, `au_posix_utc_offset`; `au_platform_time_ms` |
| entropy | `entropy_posix.c` | `au_posix_entropy` (getentropy, else `/dev/urandom`) |
| application files and clocks | `files_posix.c` | `au_posix_files_init` on the profile folder, then `au_posix_app_port`; `au_posix_read_file`, `au_posix_write_file`, `au_posix_make_dirs`; `au_platform_read_file` |
| coroutines | `coro_posix.c` | `au_posix_coro_ops` |
| font files | `fontdir_posix.c` | `au_posix_add_font_file` for one file, `au_posix_add_font_dir` for the bundled Fonts folder and its `fonts.idx` |

The port still writes `au_platform_load_fonts` (which files to load:
fontconfig on Linux), `au_platform_window_surface` and
`au_platform_present`, `AuDownloadFiles`, the system facts, and
everything in the user interface.

## The POSIX services

`src/platform/posix` has no event loop and does not block the caller.
To run the network stack from a GUI main loop (the GLib main loop in the
GTK4 port), do this:

1. Make the sockets with `au_posix_net_new`, and the stack with
   `au_net_new(au_posix_net_ops(n))` (or `AuAppNet.ops` for
   `au_app_net_start`).
2. List the descriptors to watch with `au_posix_net_watch`: the wake
   pipe that DNS lookup threads write to, every socket for reading, and
   for writing each socket that is still connecting or that last
   refused part of a send. Add a watch for each (`g_unix_fd_add` with
   `G_IO_IN`, `G_IO_OUT`, and always `G_IO_ERR | G_IO_HUP`).
3. When one is ready, call `au_posix_net_ready` (it empties the wake
   pipe), then `au_net_pump` until it returns false.
4. After each pump, compare `au_posix_net_watch_serial` with the value
   from step 2; when it changed, list the descriptors again and replace
   the watches.
5. Also pump from a timer, for the stack's connect, idle and total
   timeouts and for the page's own idle work; the host tool pumps at
   least every 20 ms while a load runs.

A tool without a main loop calls `au_posix_net_wait(n, ms)`, which does
steps 2 and 3 with `poll`; the `aura-host` commands use it.

The code builds on macOS and on Linux (glibc and musl). It uses no call
that one of them lacks without a fallback: entropy uses `getentropy`
where the C library declares it and `/dev/urandom` otherwise, and
`SO_NOSIGPIPE` and `MSG_NOSIGNAL` are each used only where defined.

Then the user interface:

- a browser window: the page drawn from Aura's surface, input turned
  into `AuEvent`, and either the Platinum chrome painter or the port's
  own tabs, toolbar, address field and status bar (see "Native chrome"
  below);
- the menu bar or application menu, built from the command table;
- the Preferences window, built from the preference descriptions;
- the History, Bookmarks, Downloads and Cookies windows, built from the
  list models;
- the developer windows, built from the window models
  (`src/browser/devwin_model.h`), with the text the engine draws;
- the About box.

## Native chrome

A browser window can show its chrome in one of two ways:

- **Painted chrome.** The model paints the tab strip, toolbar and status
  bar with the Platinum painter into the window's surface and routes
  clicks in them itself. Mac OS 9 uses this, with native scroll bars,
  grow box and address field placed over rectangles the layout reserves.
- **Native chrome.** The model's surface holds only the page. The port
  shows the tabs, buttons, address field, progress and status with its
  own widgets, reads their state from the model, and calls `au_bw_*`
  actions when the user uses them. The model tells the port what to
  update through `AuBrowserHost.changed` and the `AU_CHANGED_*` bits.

The GTK4 port uses native chrome: `GtkHeaderBar` with Back, Forward,
Reload and the address `GtkEntry`, a `GtkNotebook` for tabs, and a
`GtkDrawingArea` for the page. It uses plain GTK4, without libadwaita.

## The GTK4 port

| Part | GTK4 |
|---|---|
| application and windows | `GtkApplication`, `GtkApplicationWindow` |
| menus and shortcuts | `GMenu` and `GAction` from the command table, `gtk_application_set_accels_for_action` |
| page | `GtkDrawingArea` drawing the page surface with Cairo; `GtkEventControllerKey`, `GtkEventControllerLegacy` (pointer, with its own click counting), `GtkEventControllerScroll`, `GtkEventControllerMotion` for input; `GtkIMContext` for text input |
| chrome | `GtkHeaderBar`, `GtkEntry`, `GtkNotebook`, `GtkProgressBar` or the entry's progress, a status label |
| dialogs | `GtkAlertDialog`, `GtkFileDialog` |
| Preferences | a window of `GtkStack` pages built from the preference descriptions |
| lists | `GtkColumnView` over a `GListModel` fed from the list models |
| clipboard | `GdkClipboard` |
| network | the POSIX sockets in `src/platform/posix`, run from the GLib main loop (`g_unix_fd_add` on the descriptors `au_posix_net_watch` lists; "The POSIX services" above) |
| coroutines, clocks, entropy, files | `src/platform/posix` |
| fonts | fontconfig to find font files, read by Aura's own font code |
| files | `$XDG_CONFIG_HOME/aura`, `$XDG_CACHE_HOME/aura`, downloads in `$XDG_DOWNLOAD_DIR` |
| packaging | an AppImage and a `.deb`, built in CI ([`gtk.md`](gtk.md), "Packaging") |

GTK4 is LGPL and is linked from the system, not bundled; Aura stays
MIT. Its build is `-DAURA_PLATFORM=gtk` (`./build.sh gtk`). To build,
run or change it, read [`gtk.md`](gtk.md).

## Status of the refactoring

Work that moves Mac OS 9 code into shared code, in order. Each item
keeps the Mac OS 9 build working and looking the same.

| Step | Status |
|---|---|
| Address completion and menu item text in `src/browser` (`au_complete_fill`, `au_menu_title`) | done |
| Native chrome mode in the browser model, with complete `AU_CHANGED_*` notices ([`browser-chrome.md`](browser-chrome.md), "Native chrome") | done |
| Command table (`src/browser/commands.c`); Mac menus built from it ([`browser-chrome.md`](browser-chrome.md), "Commands and menus") | done |
| Preference descriptions (`src/browser/prefs_ui.c`); Mac Preferences window built from them ([`browser-chrome.md`](browser-chrome.md), "Preferences") | done |
| List models for History, Bookmarks, Downloads and Cookies (`src/browser/lists.c`); Mac list windows built from them | done |
| Split `src/platform/host` into `src/platform/posix` and the `aura-host` tool; `AURA_PLATFORM` in CMake | done |
| Developer windows' layout and actions in `src/browser` (`devwin_model.c`); Mac `devwin.c` only frames them ([`developer-tools.md`](developer-tools.md), "The window models") | done |
| Application services in `src/browser/app.c`: the data files Aura keeps, loading them at start, when to save them, damaged files, and setting up the network stack from preferences; the port gives named-file read, write and safe replace (Mac `storage.c`, `network.c`; Unix `src/platform/posix/files_posix.c`) | done |
| GTK4 port in `src/platform/gtk`: browser windows, tabs, page, input, menus, dialogs, fonts, network ([`gtk.md`](gtk.md)) | done |
| GTK4 Preferences, list, developer and About windows, and a CI build | done |
| Packaging for Linux: `.deb` and AppImage, built in CI and attached to releases ([`gtk.md`](gtk.md), "Packaging") | done; the AppImage is untested outside CI |
