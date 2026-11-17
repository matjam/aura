# The Linux GTK4 port

This document says how to build, run and test the Linux port in
`src/platform/gtk`, and how it maps the browser model onto GTK4. Read
it before you change the port. The boundary between the port and
`src/browser` is in [`porting.md`](porting.md); the browser model's
behaviour is in [`browser-chrome.md`](browser-chrome.md).

The port runs on Wayland only, with plain GTK4 (4.10 or later) and no
libadwaita. Never set `GDK_BACKEND=x11` or run it under XWayland.

## To build and run

| To | Run |
|---|---|
| build | `./build.sh gtk` (packages: [`building.md`](building.md), "The GTK4 build") |
| run | `build/gtk/src/platform/gtk/aura [URL or file...]` in a Wayland session |
| run with a throwaway profile | set `XDG_CONFIG_HOME`, `XDG_CACHE_HOME` and `XDG_DATA_HOME` to an empty folder first |
| run with the bundled fonts only | set `AURA_NO_SYSTEM_FONTS=1` |
| point it at another assets folder | set `AURA_ASSETS=/path/to/assets` |

The build must finish without warnings, and `./build.sh test` must
still pass.

## Files

`src/platform/gtk/gtk.h` lists each file and what it holds. Everything
that includes a GTK, GLib, GDK, Cairo or fontconfig header lives in this
folder. The OS services (sockets, files, clocks, entropy, coroutines)
come from `src/platform/posix` unchanged.

To add a native window (Preferences, the list windows, the developer
windows, About), add its sources through `src/platform/gtk/windows.cmake`,
which sets `AURA_GTK_WINDOW_SOURCES`; `CMakeLists.txt` includes it when
it exists. Point the matching `show_*` function in the "Native windows"
section of `menus.c` at the window's entry point; every menu command
and `au_gtk_show_window` reach those windows through that section only.
Those windows are documented in `docs/gtk-windows.md`, owned by the
second phase of the port.

## Profile and assets

- The profile is `$XDG_CONFIG_HOME/aura` (`~/.config/aura`), read and
  written through the POSIX `AuAppFiles`. Its `Cache` folder goes to
  `$XDG_CACHE_HOME/aura/Cache` instead.
- Downloads go to the download folder preference, else the XDG download
  folder, else `~/Downloads`. A download is written as `<id> <name>.part`
  and renamed when it completes; nothing is opened automatically.
- The assets folder is the first that holds `fonts/fonts.idx`, in this
  order: `$AURA_ASSETS`, `assets` beside the binary, `../share/aura`
  beside the binary, the source tree's `assets` the binary was built
  from, and the install prefix's `share/aura`. The root store is
  `net/aura-roots.bin` in the same folder; without it HTTPS fails and
  the port says so on standard error.

## Fonts

- To find system fonts, the port asks fontconfig for every TrueType and
  OpenType face and registers each lazily: a file is read the first time
  a page uses one of its faces.
- The bundled faces in the assets' `fonts` folder come after the system
  faces, so a system face of the same family wins.
- The generic families (serif, sans-serif, monospace, system-ui) map to
  what fontconfig picks for them.
- Aura's own font code reads the files; GTK's text rendering draws only
  the widgets. See [`fonts.md`](fonts.md).

## The main loop

GLib's main loop drives everything; the engine has no threads.

- **The tick** (`main.c`) runs every 20 ms, or every 10 ms while a page,
  a download or the network is busy. It runs the model's timers, pumps
  the network for its timeouts and flushes each window.
- **`au_gtk_kick`** runs a flush at high idle priority, sooner than the
  next tick. Call it after input or a widget action.
- **The network** (`net_gtk.c`) puts a `g_unix_fd_add` source on each
  descriptor `au_posix_net_watch` lists. When one is ready it calls
  `au_posix_net_ready`, then `au_net_pump` until it reports nothing
  more, then the loader's poll. The list is rebuilt only when
  `au_posix_net_watch_serial` changes ([`porting.md`](porting.md), "The
  POSIX services").
- **The flush** (`au_gtk_win_flush`) applies the `AU_CHANGED_*` bits that
  `AuBrowserHost.changed` recorded in `AuGtkWin.pending`, then paints.
  Never change a widget from inside a model call or a widget's own
  signal; record the change and kick. Of the two focus bits, the later
  request in a batch wins.
- SIGTERM and SIGINT quit as the menu does, so the session is saved.

## Windows and tabs

A browser window is a `GtkApplicationWindow` with a `GtkHeaderBar`
(Back, Forward, Reload or Stop, the address `GtkEntry` with the security
icon and load progress), the menu bar, a `GtkNotebook` holding only the
tab labels, an optional find bar, the page (a `GtkDrawingArea` with
scroll bars) and a status line. The model's window is made with
`au_browser_window_new_native`, so its surface holds only the page.

- Reordering tabs by dragging calls `au_bw_move_tab`.
- Ctrl+1 to Ctrl+8 choose a tab and Ctrl+9 the last.
- Typing in the address field calls `au_bw_set_field_text`; Escape puts
  back the page's address; Return calls `au_bw_navigate_input` and moves
  the focus to the page.

## HiDPI

The page surface is in logical pixels. At a scale factor above 1 the
compositor's scale enlarges it with nearest-neighbour filtering, so
pages look sharp-edged but not smooth. Drawing at device pixels needs
the engine to lay out at a scale; it is a known gap.

## Input

`input.c` turns GTK events into `AuEvent`s for `au_bw_key` and
`au_bw_mouse`, in page coordinates.

- **Modifiers.** Ctrl is `AU_MOD_COMMAND`, as Command is on the Mac
  (Ctrl-click opens a link in a new tab). Ctrl with an arrow, Backspace
  or Delete becomes `AU_MOD_OPTION` instead, which text fields read as
  "by word". Alt is `AU_MOD_OPTION`; Shift is `AU_MOD_SHIFT`.
- **Text** comes from a `GtkIMMulticontext` attached to the page's key
  controller, so dead keys, compose sequences and input methods work.
  Each committed code point is one `AU_EVENT_KEY_DOWN`.
- **Special keys** (arrows, paging, Home, End, Backspace, Delete,
  Return, Tab, Escape) go straight to `au_bw_key`. Escape the page does
  not take stops the load. Tab the page does not take stays on the page.
  A key with Ctrl that is not special is left to the menu accelerators.
- **The pointer** uses a `GtkEventControllerLegacy`, which counts clicks
  itself from `gtk-double-click-time` and `gtk-double-click-distance`.
  Button 3 opens the contextual menu, buttons 8 and 9 go Back and
  Forward, and the middle button clicks with Ctrl.
- **Scrolling.** A wheel step is 48 pixels; smooth scrolling passes its
  pixels through. Shift turns vertical scrolling horizontal.

## Menus

`menus.c` builds the menu bar from the command table
([`browser-chrome.md`](browser-chrome.md), "Commands and menus").

- Each command is the application action `app.cmd-N`, where N is its
  `AuCommand` value, with the table's shortcut as its accelerator (Ctrl
  for the primary modifier, `<Control>equal` as well for Ctrl++).
- The tick sets each action's enabled and checked state from
  `au_command_enabled` and `au_command_checked`. When a command is
  disabled, its key goes on to the focused widget, which then gets the
  command `au_command_for_key` names.
- The Go, Bookmarks and Window menus end with a section rebuilt from the
  model's dynamic lists when they change.
- The contextual menu is a `GtkPopoverMenu` built from
  `au_bw_context_menu`; its items run `au_command_run_context`.
- The Edit commands work on a focused text widget through its own
  actions, else on the page's text field (`au_bw_page_edit`), else on
  the page's selection.

To add a command, add it to the command table; the menu, accelerator
and enabled state follow without port changes.

## Dialogs

The model asks some questions synchronously: a select's pop-up menu,
`confirm()`, files for `input type=file`, the download warning, and the
file for File > Open and Save Page As.
GTK4's dialogs (`GtkAlertDialog`, `GtkFileDialog`, a `GtkPopoverMenu`
for a select) answer through callbacks, so `host.c` shows the dialog and
runs a nested main loop until it answers (`au_gtk_modal_wait`). While
one waits, the network watches are off, the tick does nothing and input
to browser windows is ignored, so the model is never entered again from
inside its own call. To add a synchronous dialog, use
`au_gtk_modal_wait`; never call back into the model from its callback.

## To test without a desktop

Run the port only inside a nested headless compositor, never on your
own desktop. Use sway's headless backend, `grim` for screenshots and
`wtype` for keys:

```bash
RT=$(mktemp -d)              # short path: a Wayland socket path must stay under 108 bytes
export XDG_RUNTIME_DIR=$RT
cat > /tmp/sway-headless.conf <<'EOF'
xwayland disable
output HEADLESS-1 resolution 1280x900
default_border none
EOF
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    timeout 600 sway -c /tmp/sway-headless.conf &
export WAYLAND_DISPLAY=wayland-1   # the socket that appears in $RT
timeout 500 build/gtk/src/platform/gtk/aura file://$PWD/tests/some.html &
wtype -k Shift_L "typed text" -k Return
wtype -k Shift_L -M ctrl t -m ctrl  # Ctrl+T
grim shot.png
```

- Start every background process under `timeout`, and kill sway when you
  are done.
- Begin each `wtype` call with a harmless key (`-k Shift_L`): the first
  key after `wtype` uploads its keymap can be lost.
- The headless seat has no pointer, so `swaymsg seat - cursor press`
  does not reach the window. Test clicks on a real session or with a
  virtual pointer client.
- On some machines sway is killed a fraction of a second after it starts
  unless it runs under a tracer. If so, start it as
  `strace -e trace=none -e signal=none -o /dev/null sway ...`.

## Known gaps

- Preferences, the History, Bookmarks, Downloads and Cookies windows,
  the developer windows and About are not in this port yet; their
  commands show a notice. Page Source opens a `view-source:` tab. System Status data (`sysinfo_gtk.c`) is
  ready but has no window.
- No content blocking.
- HiDPI enlarges logical pixels instead of drawing at device pixels.
- The application is not unique: a second launch is a second process
  sharing the same profile.
- Tabs have no contextual menu and no site icons.
- Windows open where the compositor puts them; Wayland gives a client
  no window position, so the session does not restore positions.
- Find Again from a developer window does nothing.
- Download names are cut to the Mac's 31 characters by shared code.
- No CI job and no packages yet.
