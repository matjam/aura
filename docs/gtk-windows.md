# GTK4 port: the secondary windows

The Linux GTK4 port's windows other than the browser window live in
`src/platform/gtk/`: Preferences, the four lists (Bookmarks, History,
Downloads, Cookies), the six developer windows (Page Source, Console,
Network, Performance, System Status, Report a Problem) and the About
box. Each is a view of a portable model in `src/browser`; the GTK code
builds widgets from the model's descriptions, turns GTK events into the
model's calls, and gives the model what it asks of the system. It holds
no browser state and makes no decisions of its own.

| File | Window | Model |
|---|---|---|
| `windows_gtk.h` | the API the application calls | |
| `winutil_gtk.c`, `winutil_gtk.h` | the shared dialogs, CSS, clipboard, entry points | |
| `prefswin_gtk.c` | Preferences | `prefs_ui.h` |
| `listwin_gtk.c` | Bookmarks, History, Downloads, Cookies | `lists.h` |
| `devwin_gtk.c` | the developer windows | `devwin_model.h` |
| `about_gtk.c` | About Aura | `credits.h`, `sysinfo.h` |

## To build them

Include `windows.cmake` from `src/platform/gtk/CMakeLists.txt` and add
its list to the application:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/windows.cmake)
target_sources(aura_gtk PRIVATE ${AURA_GTK_WINDOW_SOURCES})
```

They need GTK 4.12 or later (`gtk_css_provider_load_from_string`,
`GtkFileDialog`, `GtkAlertDialog`) and no libadwaita.

## To use them from the application

The steps are in the comment at the top of `windows_gtk.h`. In short:

1. Fill an `AuGtkWindowsHost` once the `AuBrowser`, the fonts and the
   `AuApp` exist, and call `au_gtk_windows_init`. `app`, `browser`,
   `fonts`, `front_window`, `front_page` and `open_url` are required;
   every other member may be NULL, which turns off what it serves.
2. In `AuCommandHost.show_window`, call `au_gtk_window_kind(kind)`; it
   returns false for kinds these files do not make.
3. When a developer window is active (`au_gtk_devwin_owns`), send the
   Edit menu's Copy, Paste and Select All to `au_gtk_devwin_edit`, and
   Find, Find Again and Find Previous to `au_gtk_devwin_find` and
   `au_gtk_devwin_find_again`.
4. After the cookie store changes elsewhere, call
   `au_gtk_list_touch(AU_LIST_COOKIES)`.
5. At quit, before freeing the browser, call `au_gtk_windows_shutdown`.

## How each window is built

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
credits by kind and the MIT licence. Its links open in Aura.

## Dialogs the models wait for

The models' callbacks (ask for a name, choose a cookie rule, Find)
return their answer, and GTK4 has no blocking dialog. `au_gtk_ask_text`
and `au_gtk_choose` run a modal window in a nested `GMainLoop`. While
one runs, `au_gtk_dialog_running()` is true: the windows' timers skip
their refresh and their close requests are refused, so the model the
dialog belongs to does not change or go away under it.

## To look at a window without the application

Run each window under a headless nested compositor, never on the
desktop:

```bash
export XDG_RUNTIME_DIR=$(mktemp -d)
printf 'output HEADLESS-1 resolution 1280x900\n' > sway.conf
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 timeout 120 sway -c sway.conf &
WAYLAND_DISPLAY=wayland-1 AURA_SHOW=prefs timeout 20 ./driver &
WAYLAND_DISPLAY=wayland-1 grim prefs.png
```

The driver is a small program that creates an `AuBrowser` with a font
set, calls `au_gtk_windows_init` and shows one window; link it against
`aura_engine` and the five sources above.

## Known gaps

- Lists: dragging bookmarks to move them, and favicons in the rows.
- Developer windows: the canvas is painted at the logical size, so it
  is not sharp at a scale above 1.
- Preferences: a number field's range is checked by the session on OK,
  not as it is typed.
