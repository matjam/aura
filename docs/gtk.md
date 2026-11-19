# The Linux GTK4 port

This document says how to build, run and test the Linux port in
`src/platform/gtk`, and how it maps the browser model onto GTK4. Read
it before you change the port. The boundary between the port and
`src/browser` is in [`porting.md`](porting.md); the browser model's
behaviour is in [`browser-chrome.md`](browser-chrome.md).

The port runs on Wayland only, with plain GTK4 (4.12 or later) and no
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

The secondary windows (Preferences, the lists, the developer windows,
About) have their own sources, listed in `windows.cmake`, which
`CMakeLists.txt` includes. To add a source file to them, add it to
`AURA_GTK_WINDOW_SOURCES` there. They are described in "Secondary
windows" below.

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
  a download or the network is busy, and at the next idle turn (below
  GTK's drawing) while a page has work it could do now
  (`au_browser_ready`, a parked script). It runs the model's timers, pumps
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
icon and load progress, and the main menu button at the right), a
`GtkNotebook` holding only the
tab labels, an optional find bar, the page (a `GtkDrawingArea` with
scroll bars) and a status line. The model's window is made with
`au_browser_window_new_native`, so its surface holds only the page.
The status line's right end holds the memory readout (View > Show
Memory Use, `au_bw_memory_text`) while that preference is on.

- Reordering tabs by dragging calls `au_bw_move_tab`.
- Ctrl+1 to Ctrl+8 choose a tab and Ctrl+9 the last.
- Typing in the address field calls `au_bw_set_field_text`; Escape puts
  back the page's address; Return calls `au_bw_navigate_input` and moves
  the focus to the page.

## Appearance

`appearance_gtk.c` finds the desktop's light or dark setting and themes
the chrome from the Appearance preference
([`browser-chrome.md`](browser-chrome.md), "Appearance").

- The system setting comes from the settings portal
  (`org.freedesktop.portal.Settings`, `org.freedesktop.appearance`
  `color-scheme`: 1 is dark) over GDBus, read with `ReadOne` (or `Read`
  on an older portal) and followed through `SettingChanged`. Without a
  portal it comes from the `gtk-interface-color-scheme` setting of
  GTK 4.20 and later (2 is dark), followed through its notify signal.
  Either one calls `au_browser_set_system_dark`.
- Light and Dark set `gtk-application-prefer-dark-theme` (and
  `gtk-interface-color-scheme` where it is writable) so the windows'
  widgets match the pages. Same as system puts back the values GTK had
  at start, so the desktop's own setting stays in charge.
- `au_gtk_appearance_start` runs after the profile loads and
  `au_gtk_appearance_stop` at quit. `au_gtk_appearance_apply` runs
  whenever the Preferences window reports a change (a live item, OK or
  Cancel), so the chrome follows the pop-up as it is chosen.

To test Same as system without a desktop, run the port in the headless
compositor ("To test without a desktop") inside `dbus-run-session`, and
own `org.freedesktop.portal.Desktop` on that bus with a small script
that answers `ReadOne` for `color-scheme` and emits `SettingChanged`
when you change the value.

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

`menus.c` builds the application's menu model from the command table
([`browser-chrome.md`](browser-chrome.md), "Commands and menus"). The
window does not show it as a menu bar: each browser window's header bar
has a main menu button (`open-menu-symbolic`) holding the same model, with
each menu as a submenu. F10 opens it, because the button is the window's
primary menu.

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

## Secondary windows

Every window other than the browser window is a view of a portable
model in `src/browser`. The GTK code builds widgets from the model's
descriptions, turns GTK events into the model's calls, and gives the
model what it asks of the system. It holds no browser state and makes
no decisions of its own.

| File | Window | Model |
|---|---|---|
| `windows_gtk.h` | the API the rest of the port calls | |
| `winutil_gtk.c`, `winutil_gtk.h` | the shared dialogs, CSS, clipboard, entry points | |
| `prefswin_gtk.c` | Preferences | `prefs_ui.h` |
| `listwin_gtk.c` | Bookmarks, History, Downloads, Cookies | `lists.h` |
| `devwin_gtk.c` | Page Source, Console, Network, Performance, System Status, Report a Problem | `devwin_model.h` |
| `about_gtk.c` | About Aura | `credits.h`, `sysinfo.h` |

### How the port connects them

- **Start and quit.** `start()` in `main.c` calls `au_gtk_windows_start`
  (`host.c`), which fills an `AuGtkWindowsHost` and calls
  `au_gtk_windows_init`. `au_gtk_quit` calls `au_gtk_windows_shutdown`
  before the last save, so an open Preferences window is cancelled
  first.
- **Opening.** `au_gtk_show_window` in `menus.c` passes every
  `AuWindowKind` to `au_gtk_window_kind`. Page Source opens the Source
  developer window.
- **The host's hooks.** `host.c` serves the front window and page,
  opening a URL through `au_command_open_url`, showing a download in the
  file manager (`GtkFileLauncher`), the system facts (`sysinfo_gtk.c`),
  the network's connections and header recording, and free memory.
  After OK in Preferences it calls `au_app_prefs_changed`. The hooks it
  leaves NULL are listed in "Known gaps".
- **Edit and Find.** When a developer window is active,
  `au_gtk_command_state` reports `AU_FRONT_TOOL`. A focused entry takes
  the Edit commands first; otherwise Copy, Paste and Select All go to
  `au_gtk_devwin_edit`. Find goes to `au_gtk_devwin_find`, and Find
  Again and Find Previous to `au_gtk_devwin_find_again`.
- **Refreshing.** A model change with `AU_CHANGED_MENUS` or
  `AU_CHANGED_BOOKMARK` touches the History and Bookmarks lists
  (`au_gtk_list_touch`). The Cookies list notices changes through
  `au_list_stale`. A new download brings the Downloads window forward.
- **The Window menu** lists the browser windows, then the other
  visible windows of the application (`window_refs` in `menus.c`).

### How each window is built

**Preferences.** A `GtkStackSidebar` beside a `GtkStack` with one page
per `AuPrefSection`. Each page is a grid of caption and control, made by
walking `au_pref_desc` for every `AuPrefId` and choosing the widget by
`AuPrefKind`: check button, a pair of grouped check buttons for a radio
item, an entry and its unit for a number, an entry for text, an entry
and a Choose button (`GtkFileDialog`) for a folder, a `GtkDropDown` for
a choice or a font, a button for an action, a wrapping label for a
note, and one check button and status line per blocking list. As on
Mac OS 9, check boxes, radio buttons and drop-downs are set in the edit
session as they change (live items are applied to the browser at once),
text and number fields are read on OK, and Cancel or closing the window
cancels the session. Restore Defaults resets the visible section. A
one-second timer redraws the blocking lists' status while they update.
Tabs on Left takes effect at once: the browser window's body turns
horizontal and the notebook puts its tabs on the left, at the tab
column width.

**Lists.** A `GtkColumnView` over a `GListStore` of row handles; the
model's rows, columns, buttons and actions drive it. A 250 ms timer
refreshes the model when `au_list_stale` says so. Delete and Backspace
run Delete; a double click or Return activates the row.

**Developer windows.** The model is painted, as on Mac OS 9, with the
engine's canvas into a Cairo image surface the size of a
`GtkDrawingArea`: the engine's premultiplied `0xAARRGGBB` pixels are
`CAIRO_FORMAT_ARGB32`, so `au_surface_wrap` paints straight into it.
The push buttons, the tool band's toggles and a `GtkScrollbar` per text
pane sit over the drawing area in a `GtkOverlay`, at the rectangles
`au_devwin_button_rect`, `au_devwin_toggle_rect` and `au_devwin_areas`
give. The drawing area keeps the keyboard; the overlaid widgets never
take focus, so Tab and Return reach the model. Clicks are counted by
hand (double-click time, 4 pixels) and a drag past a pane's edge
scrolls on a 40 ms timer. Only the window's size is kept in the
`develop.windows` preference, since Wayland gives no position.

**About.** A `GtkAboutDialog` with the build identity, the tagline, the
credits by kind and a short MIT licence statement
(`GTK_LICENSE_CUSTOM`). Its links open in Aura.

### Dialogs the window models wait for

The models' callbacks (ask for a name, choose a cookie rule, Find)
return their answer. `au_gtk_ask_text` and `au_gtk_choose` run a modal
window in a nested `GMainLoop`. While one runs, `au_gtk_dialog_running()`
is true: the windows' timers skip their refresh and their close
requests are refused, so the model the dialog belongs to does not
change or go away under it.

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
  `strace -f -e trace=none -e signal=none -o /dev/null sway ...`.

## Packaging

The port ships as a `.deb` and an AppImage, both built by
`tools/package-linux.sh` and in CI by `.github/workflows/build-linux.yml`.
GTK4 is LGPL and Aura is MIT, so GTK4 and fontconfig are linked from the
system and are never bundled in either package.

| To | Run |
|---|---|
| build both packages | `tools/package-linux.sh [OUTDIR]` (default `dist/linux`; needs the GTK4 build packages of [`building.md`](building.md)) |
| build the AppImage too | set `APPIMAGETOOL` to an appimagetool binary (CI uses release 1.9.1, checked by SHA-256; it is not in the repository); without it the script leaves the AppDir in `build/package-linux/AppDir` and says so |
| install into a folder | `cmake --install build/gtk --prefix DIR` |

- The install rules are in `packaging/linux/CMakeLists.txt`: `bin/aura`,
  the fonts and `net/aura-roots.bin` in `share/aura` (where the binary
  looks beside itself, see "Profile and assets"), the desktop file, the
  AppStream metainfo, the icons in `share/icons/hicolor` and the licence.
  The desktop file and metainfo are `org.aura.Browser.*`, the application
  ID, and the desktop file claims `text/html` and the `http` and `https`
  schemes.
- The `.deb` depends on `libgtk-4-1 (>= 4.12)`, `libfontconfig1` and
  `libc6`. Its version is `AU_VERSION`.
- The AppImage holds an AppDir with `AppRun`, which runs `usr/bin/aura`.
  It needs GTK4 4.12 or later and fontconfig on the host, as the `.deb`
  does; on a machine without them, install `libgtk-4-1`.
- CI uploads both as artifacts of every push and pull request, and
  attaches them to a release ([`ci-and-releases.md`](ci-and-releases.md)).

## Known gaps

- No content blocking: the Blocking pane of Preferences has no
  blocking manager behind it.
- Develop > Tests > Timing Test and Paint Benchmark only beep. The
  measurements are Mac shell instrumentation (`macos9/browserwin.c`);
  the port leaves `timing_test` and `benchmark` NULL until they move to
  shared code.
- No default-browser switch in Preferences.
- HiDPI enlarges logical pixels instead of drawing at device pixels.
- The application is not unique: a second launch is a second process
  sharing the same profile.
- Tabs have no contextual menu and no site icons.
- Windows open where the compositor puts them; Wayland gives a client
  no window position, so the session does not restore positions.
- Lists: dragging bookmarks to move them, and favicons in the rows.
- Developer windows: the canvas is painted at the logical size, so it
  is not sharp at a scale above 1.
- Preferences: a number field's range is checked by the session on OK,
  not as it is typed.
- Download names are cut to the Mac's 31 characters by shared code.
