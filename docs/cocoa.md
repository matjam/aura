# The macOS port

This document says how to build, run and test the modern macOS port in
`src/platform/cocoa`, and how it maps the browser model onto AppKit.
Read it before you change the port. The boundary between the port and
`src/browser` is in [`porting.md`](porting.md); the browser model's
behaviour is in [`browser-chrome.md`](browser-chrome.md). The GTK4 port
([`gtk.md`](gtk.md)) has the same structure, file for file.

The port is for testing and compatibility work on a current Mac, beside
`aura-host`. It needs macOS 11 or later, and is written in Objective-C
with ARC.

## To build and run

| To | Run |
|---|---|
| build | `./build.sh cocoa` (Xcode's command line tools, CMake and Ninja) |
| run | `open build/cocoa/src/platform/cocoa/Aura.app`, or run `build/cocoa/src/platform/cocoa/Aura.app/Contents/MacOS/Aura [URL or file...]` to see its messages |
| run with a throwaway profile | set `AURA_PROFILE` to an empty folder |
| run with the bundled fonts only | set `AURA_NO_SYSTEM_FONTS=1` |
| point it at another assets folder | set `AURA_ASSETS=/path/to/assets` |
| build the universal zip CI makes | `tools/package-macos.sh [OUTDIR]` |

The build must finish without warnings, and `./build.sh test` must
still pass.

## Files

`src/platform/cocoa/cocoa.h` lists each file and what it holds.
Everything that includes an AppKit, Foundation, Core Graphics or Core
Text header lives in this folder. The OS services (sockets, files,
clocks, entropy, coroutines) come from `src/platform/posix` unchanged.

The secondary windows (Preferences, the lists, the developer windows,
About) are listed in `windows.cmake` and declared in
`windows_cocoa.h`. `winutil_cocoa.m` holds their shared entry points and
a weak placeholder for each window's functions, which the window's own
file replaces; a window whose file is missing reports that it is not
built, and the model falls back where it can.

## Profile and assets

- The profile is `~/Library/Application Support/Aura`, read and written
  through the POSIX `AuAppFiles`. Its `Cache` folder goes to
  `~/Library/Caches/Aura/Cache` instead. `AURA_PROFILE` puts both in one
  folder.
- Downloads go to the download folder preference, else `~/Downloads`. A
  download is written as `<id> <name>.part` and renamed when it
  completes, then marked with `com.apple.quarantine` so the system asks
  before anything in it runs. Nothing is opened automatically.
- The assets folder is the first that holds `fonts/fonts.idx`:
  `$AURA_ASSETS`, the bundle's `Contents/Resources/assets` (the build
  copies the fonts and the root store there), then the source tree's
  `assets`. Without `net/aura-roots.bin` HTTPS fails and the port says
  so on standard error.

## Fonts

- To find system fonts, the port lists Core Text's font files
  (`CTFontManagerCopyAvailableFontURLs`) and each file's faces
  (`CTFontManagerCreateFontDescriptorsFromURL`, in file order, so a
  collection's face index is its place in the list). The listed URLs
  name one face each (`Helvetica.ttc#postscript-name=Helvetica-Bold`),
  and such a URL's descriptors are that face alone at place 0, so the
  port reads each file once through a URL without the fragment. Reading
  the per-face URLs registered every face of a collection as face 0: no
  system font drew bold or italic (fixed 2026-11-16). It registers the
  TrueType faces lazily; Aura's own font code reads the files. CFF faces,
  the system's private families (names beginning with a dot) and Apple
  Color Emoji (bitmap glyphs) are left out.
- The bundled faces come after the system faces, so a system face of
  the same family wins.
- The generic families map to the first of the usual Mac families the
  set has: Times New Roman for serif, Helvetica for sans-serif, Menlo for
  monospace, Helvetica Neue for system-ui.

## The main loop

AppKit's run loop drives everything; the engine has no threads.

- **The tick** (`main.m`) is an `NSTimer` in the common run loop modes,
  so it runs while a menu is open or a window is resized. It fires every
  20 ms, or every 10 ms while a page, a download or the network is busy,
  and runs the model's timers, pumps the network and flushes each
  window.
- **`au_cocoa_kick`** runs a flush on the main queue, sooner than the
  next tick. Call it after input or a control's action.
- **The network** (`net_cocoa.m`) registers the descriptors
  `au_posix_net_watch` lists with one kqueue, and one dispatch source on
  the main queue watches the kqueue. A dispatch source per socket is not
  allowed: the POSIX layer closes sockets itself, and libdispatch must
  never watch a closed descriptor, whereas a kqueue forgets one by
  itself. The registrations are made again when
  `au_posix_net_watch_serial` changes ([`porting.md`](porting.md), "The
  POSIX services").
- **The flush** (`au_cocoa_win_flush`) applies the `AU_CHANGED_*` bits
  recorded in `AuCocoaWin.pending`, then paints. Never change a control
  from inside a model call or a control's action; record the change and
  kick. Of the two focus bits, the later request in a batch wins.
- As on every Mac, closing the last window leaves Aura running, and
  clicking its Dock icon then opens a window. Quit, the Dock's Quit,
  SIGTERM and SIGINT save the session and the files first.

## Windows and tabs

A browser window is an `NSWindow` with a unified toolbar and a body
that `AuBodyView` lays out by hand: the tab strip, the find bar, the
page with its scroll bars, and the status bubble. The title is hidden,
as in Safari. The model's window is made with
`au_browser_window_new_native`, so its surface holds only the page.

- The toolbar holds Back and Forward (plain `NSToolbarItem`s marked
  navigational, so AppKit draws them as one group at the leading edge)
  and the address field between two flexible spaces. Their enabled
  state comes from `sync_nav`; the items do not validate themselves.
  New Tab is a button in the tab strip, right after the last tab (under
  it with tabs on the left).
- The address field (`AuAddressField`) holds the security icon at its
  left, Reload or Stop at its right, and a 2-point accent line along its
  bottom while the page loads (`au_bw_progress`, shown at 5% at least
  from the moment a load starts). `AuAddressCell` keeps the text and
  the field editor clear of the two.
- The status text (`au_bw_status_text`: the link under the pointer, or
  what a load is doing) shows in a bubble over the page's bottom left
  corner, at most two thirds of the page wide, and only while there is
  some. The window has no status bar.
- The tab strip lies on an `NSVisualEffectView` of the toolbar's
  material (the sidebar material for tabs on the left), and the window
  draws no separator under the toolbar, so the toolbar and the strip
  read as one bar; the strip draws the line below itself. The active tab
  has a neutral highlight, and inactive tabs are divided by short lines.

- The tab strip (`AuTabStrip`, `AuTabItem` in `window.m`) shows each
  tab's site icon or a spinner, its title and a close button. A drag
  moves a tab (`au_bw_move_tab`), a middle click closes it, and its
  contextual menu has New, Duplicate, Reload, Unload, Close and Close
  Other Tabs. With tabs on the left the strip is a column of the tab
  column width.
- Command-1 to Command-8 choose a tab and Command-9 the last.
- Typing in the address field calls `au_bw_set_field_text` and completes
  from history and bookmarks; Escape puts back the page's address;
  Return calls `au_bw_navigate_input` and moves the focus to the page.
- The address field's toolbar item is 240 to 1000 points wide, and the
  toolbar stretches it by itself. A width constraint asking for more,
  even at a low priority, makes the toolbar move every item into its
  overflow menu.
- The scroll bars are legacy `NSScroller`s driven by the model's scroll
  position; the vertical one stays, the horizontal one shows only when
  the page is wider than the window.

## Appearance

`appearance_cocoa.m` follows the system's light or dark setting
(`AppleInterfaceStyle`, re-read on `AppleInterfaceThemeChangedNotification`
and on changes of `NSApp.effectiveAppearance`) and calls
`au_browser_set_system_dark`. Light and Dark set `NSApp.appearance` to
Aqua or Dark Aqua, so the chrome matches the pages; System clears it.

## HiDPI

The page surface is in points. On a Retina screen the page layer is
enlarged with nearest-neighbour filtering, so pages look sharp-edged but
not smooth. Drawing at device pixels needs the engine to lay out at a
scale; it is a known gap, as on GTK.

## Input

`input.m` is the page view (`AuPageView`). It turns AppKit events into
`AuEvent`s for `au_bw_key` and `au_bw_mouse`, in page coordinates.

- **Modifiers** follow the Mac, as on Mac OS 9: Command is
  `AU_MOD_COMMAND` (Command-click opens a link in a new tab), Option is
  `AU_MOD_OPTION` (Option with an arrow or Delete works by word in a
  text field), Shift is `AU_MOD_SHIFT`. Control-click opens the
  contextual menu.
- **Text** comes through `NSTextInputClient` (`interpretKeyEvents:`), so
  dead keys, the accent menu and input methods work. Each inserted code
  point is one `AU_EVENT_KEY_DOWN`. The input method's window is placed
  at the pointer's last place on the page, since the model does not
  report its caret.
- **Special keys** (arrows, paging, Home, End, Delete, Forward Delete,
  Return, Tab, Escape) go straight to `au_bw_key`. Escape the page does
  not take stops the load; Tab the page does not take stays on the page.
- **The pointer**: `NSEvent.clickCount` gives double and triple clicks.
  The middle button clicks with Command; buttons 4 and 5 go Back and
  Forward.
- **Scrolling**: a mouse wheel step is 48 points; a trackpad's precise
  deltas pass through, with the system's natural scrolling already
  applied. Their fractions of a pixel carry to the next event, so slow
  movements are not lost to rounding.
- A link, file or text dropped on the page opens it.

## Menus

`menus.m` builds the menu bar from the command table
([`browser-chrome.md`](browser-chrome.md), "Commands and menus"), laid
out as Mac applications lay theirs out:

- The application menu holds About Aura, Preferences (Command-comma),
  Services, the Hide items and Quit Aura; the table's Preferences (Edit)
  and Quit (File) are moved there. The Window menu begins with Minimize,
  Zoom and Bring All to Front. Help is the last menu.
- Each command item runs `runCommand:` with its `AuCommand` as the tag;
  `validateMenuItem:` asks `au_command_enabled` and `au_command_checked`.
  A disabled command's shortcut runs what `au_command_for_key` names
  (Close Tab's key closes a window without tabs).
- The Go, Bookmarks and Window menus end with the model's dynamic list,
  rebuilt in `menuNeedsUpdate:` as the menu opens.
- The contextual menu is an `NSMenu` built from `au_bw_context_menu`;
  its items run `au_command_run_context`.
- The Edit commands go to a focused text field (the field editor, by
  `cut:`, `copy:`, `paste:`, `delete:`, `selectAll:`, `undo:`), else to
  a developer window, else to the page's text field
  (`au_bw_page_edit`), else to the page's selection.

## Dialogs

The model asks some questions synchronously: a select's pop-up menu,
`confirm()`, files for `input type=file`, the download warning, and the
file for File > Open and Save Page As. `NSAlert`, `NSOpenPanel`,
`NSSavePanel` and `NSMenu`'s pop-up run modally in place, between
`au_cocoa_modal_begin` and `au_cocoa_modal_end` (`host.m`). While one
runs, the network's dispatch source is suspended, the tick does nothing
and input to browser windows is ignored, so the model is never entered
again from inside its own call. Quitting is refused until the dialog
answers.

## Secondary windows

| File | Window | Model |
|---|---|---|
| `windows_cocoa.h` | the API the rest of the port calls | |
| `winutil_cocoa.m` | the shared host, routing by `AuWindowKind`, weak placeholders | |
| `prefswin_cocoa.m` | Preferences | `prefs_ui.h` |
| `listwin_cocoa.m` | Bookmarks, History, Downloads, Cookies | `lists.h` |
| `devwin_cocoa.m` | Page Source, Console, Network, Performance, System Status, Report a Problem | `devwin_model.h` |
| `about_cocoa.m` | About Aura | `credits.h`, `sysinfo.h` |
| `dialogs_cocoa.m` | the shared dialogs (`au_cocoa_dlg_*`): ask for text, choose from a list, details, a one-button alert | |

- Ask for text, choose from a list and the one-button alert are sheets
  on the window they came from (an app-modal alert when it has none, or
  it already shows a sheet). The models wait for the answer, so the
  sheet runs in a modal session (`runModalForWindow:`) that its
  completion handler stops. While one runs, the windows skip their
  timer refresh, ignore input and refuse to close. Save Report's panel
  is a sheet that writes the file when it is answered.
- The developer windows take characters through `NSTextInputClient`, so
  dead keys and input methods compose. The text being composed is not
  drawn; it is typed when it is committed, and dropped when the window
  stops being key.
- Edit > Copy in a list window copies the selected row's columns,
  separated by tabs. `AuCommandState.other_can_copy` turns Copy on for
  it; the other Edit commands stay off. A day heading is not copied.
- The developer windows use the light appearance, because the painted
  area is always light. A native control wider than its Mac OS 9
  rectangle keeps its width and moves the controls after it right.
- `develop.windows` keeps each developer window's size and position.
  The position is the content's top-left in points from the main
  screen's top-left. A place no screen shows the title bar of is
  centred instead.
- History's day headings are group rows and cannot be selected.

`host.m` fills the `AuCocoaWindowsHost` in `au_cocoa_windows_start`: the
front window and page, opening a URL through `au_command_open_url`,
revealing a download in the Finder, the system facts
(`sysinfo_cocoa.m`), the network's connections and header recording,
free memory, and the modal hooks the windows run their own dialogs
between.

## To test

Screen recording permission is not needed to look at the page:

```bash
env AURA_PROFILE=/tmp/aura-prof AURA_SNAPSHOT=/tmp/snap.png \
    build/cocoa/src/platform/cocoa/Aura.app/Contents/MacOS/Aura https://en.wikipedia.org/wiki/Kookaburra &
kill -USR1 %1    # writes the front browser window to /tmp/snap.png
```

- With `AURA_SNAPSHOT` set, SIGUSR1 writes the front browser window to
  that PNG: the views draw themselves into a bitmap and the page's
  pixels are drawn over the page view's place. Toolbar controls draw
  only as their outlines, so the snapshot also prints each toolbar
  item's frame and whether it is in the window or in the overflow menu,
  and for a plain button item whether it is enabled.
- Send SIGUSR1 to the Aura process itself, not to `timeout` wrapping
  it: `timeout` passes the signal on, and the snapshot is written twice.
- For the engine's rendering, use `aura-host` and
  `tools/render-compare.sh` ([`developer-tools.md`](developer-tools.md));
  the port draws the same pixels.

## Packaging

`tools/package-macos.sh` builds a universal `Aura.app` (arm64 and
x86_64, macOS 11 or later) in Release and packs it with `ditto` as
`dist/macos/Aura-VERSION-macos.zip`, about 10 MB. CI builds it on every
push and uploads it as an artifact
([`ci-and-releases.md`](ci-and-releases.md)).

- The bundle carries the fonts, the root store and the licence in
  `Contents/Resources`; `Info.plist` comes from
  `packaging/macos/Info.plist.in`, with the version from `AU_VERSION`. It
  claims the `http` and `https` schemes and HTML files, so the system can
  offer Aura as the default browser.
- The icon is made from `assets/icon/kookaburra/aura.png` with `sips`
  and `iconutil` at build time.
- The application is signed ad hoc and not notarized. To open a
  downloaded copy the first time, Control-click it in the Finder and
  choose Open.

## Known gaps

- No content blocking: the Blocking pane has no blocking manager behind
  it, as on GTK.
- Develop > Tests > Timing Test and Paint Benchmark only beep.
- No default-browser switch in Preferences.
- HiDPI enlarges points instead of drawing at device pixels.
- A tab strip with many tabs gives each tab at least 70 points and runs
  off the end instead of scrolling.
- The input method's window sits at the pointer, not at the caret.
- Not signed with a Developer ID or notarized.
- The developer windows do not draw the text an input method is
  composing.
- The list and developer windows have been driven from code (a
  temporary hook that opens them, selects, copies, answers a sheet and
  sends Core Graphics key events), not yet clicked through by hand.
  `osascript` cannot drive the app unless the terminal has
  Accessibility access (System Settings > Privacy & Security >
  Accessibility).
