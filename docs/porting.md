# Porting Aura to another platform

This document says how Aura's code is divided between the shared
browser and each platform's port, what a new port has to write, and
what is still being moved out of the Mac OS 9 port to make that small.
Read it before you add a port or change the boundary between
`src/browser` and `src/platform`.

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

## The layers

| Layer | Where | Depends on |
|---|---|---|
| Engine: HTML, CSS, layout, paint, script, network, TLS | `src/core`, `src/html`, `src/css`, `src/layout`, `src/paint`, `src/js`, `src/net`, ... | nothing outside `src/` |
| Browser model: windows, tabs, history, bookmarks, downloads, preferences, commands, lists, developer tools | `src/browser` | the engine |
| Platinum chrome painter (optional view of the model) | `src/browser/chrome.c` | the browser model |
| Port services: sockets, DNS, clock, entropy, files, coroutines, fonts | `src/platform/<os>` (shared POSIX pieces in `src/platform/posix`) | the OS |
| Port user interface | `src/platform/<port>` | the browser model, the toolkit |

The engine and the browser model build and pass their tests on Linux
and macOS hosts in CI today; a port adds only the last two rows.

## What a port writes

A port implements these interfaces. Each is a table of callbacks or a
set of functions declared in the header named.

| Interface | Header | What it provides |
|---|---|---|
| `AuNetOps` | `src/platform/platform.h` | DNS, TCP connect, send, receive, close |
| clock and time zone | `AuNetOps.now_ms` and `wall_time`, `au_js_set_clock` | milliseconds, wall time, UTC offset |
| entropy | `AuNetOps.entropy` | random bytes for TLS and `crypto` |
| storage callbacks | `src/net/web_storage.h`, `src/net/cache.h` | Local Storage and the disk cache as named files |
| coroutines | `src/core/coro.h` | stacks for workers and scripts that wait |
| fonts | `src/platform/platform.h` (`au_platform_load_fonts`) | the system's and the bundled font files |
| `AuBrowserHost` | `src/browser/browser.h` | redraw requests, change notices, pop-up menus, confirm, file chooser, download confirmation, opening other programs |
| `AuDownloadFiles` | `src/net/transfer.h` | creating and finishing download files |
| system facts | `src/browser/sysinfo.h` | the machine and OS for System Status and bug reports |

Then the user interface:

- a browser window: the page drawn from Aura's surface, input turned
  into `AuEvent`, and either the Platinum chrome painter or the port's
  own tabs, toolbar, address field and status bar (see "Native chrome"
  below);
- the menu bar or application menu, built from the command table;
- the Preferences window, built from the preference descriptions;
- the History, Bookmarks, Downloads and Cookies windows, built from the
  list models;
- the developer windows, which show text and tables the engine draws;
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
Reload and the address `GtkEntry`, a `GtkNotebook` (or `AdwTabBar`
style tabs drawn with plain GTK4) for tabs, and a `GtkDrawingArea` for
the page.

## The GTK4 port

| Part | GTK4 |
|---|---|
| application and windows | `GtkApplication`, `GtkApplicationWindow` |
| menus and shortcuts | `GMenu` and `GAction` from the command table, `gtk_application_set_accels_for_action` |
| page | `GtkDrawingArea` drawing the page surface with Cairo; `GtkEventControllerKey`, `GtkGestureClick`, `GtkEventControllerScroll`, `GtkEventControllerMotion` for input; `GtkIMContext` for text input |
| chrome | `GtkHeaderBar`, `GtkEntry`, `GtkNotebook`, `GtkProgressBar` or the entry's progress, a status label |
| dialogs | `GtkAlertDialog`, `GtkFileDialog` |
| Preferences | a window of `GtkStack` pages built from the preference descriptions |
| lists | `GtkColumnView` over a `GListModel` fed from the list models |
| clipboard | `GdkClipboard` |
| network | the POSIX sockets in `src/platform/posix`, run from the GLib main loop (`g_unix_fd_add`) |
| fonts | fontconfig to find font files, read by Aura's own font code |
| files | `~/.config/aura` and `~/.cache/aura` (XDG base directories) |
| packaging | an AppImage and a `.deb`, built in CI |

GTK4 is LGPL and is linked from the system, not bundled; Aura stays
MIT. Its build is `-DAURA_PLATFORM=gtk`.

## Status of the refactoring

Work that moves Mac OS 9 code into shared code, in order. Each item
keeps the Mac OS 9 build working and looking the same.

| Step | Status |
|---|---|
| Address completion and menu item text in `src/browser` (`au_complete_fill`, `au_menu_title`) | done |
| Native chrome mode in the browser model, with complete `AU_CHANGED_*` notices ([`browser-chrome.md`](browser-chrome.md), "Native chrome") | done |
| Command table (`src/browser/commands.c`); Mac menus built from it ([`browser-chrome.md`](browser-chrome.md), "Commands and menus") | done |
| Preference descriptions (`src/browser/prefs_ui.c`); Mac Preferences window built from them | in progress |
| List models for History, Bookmarks, Downloads and Cookies (`src/browser/lists.c`); Mac list windows built from them | done |
| Split `src/platform/host` into `src/platform/posix` and the `aura-host` tool; `AURA_PLATFORM` in CMake | not started |
| Developer windows' layout and actions in `src/browser`; Mac `devwin.c` only frames them | not started |
| GTK4 port in `src/platform/gtk`, with a CI build | not started |
| Packaging for Linux | not started |
