# Browser chrome

This document covers the browser model in `src/browser`: windows, tabs,
back and forward, the documents kept for Back, site icons, keyboard
focus, scrolling by moving pixels, selection and find, contextual
menus, Internet Config, downloads, the list windows and the files Aura
keeps. Read it before changing how a browser window looks or behaves.
The Mac OS 9 side of each feature is named here; Toolbox lessons are in
[`mac-os-9-shell.md`](mac-os-9-shell.md).

## Where things are

| File | Holds |
|---|---|
| `chrome.c` | lays out, paints and hit-tests the tab strip (or left tab column), toolbar and status bar |
| `browser.c` | windows, tabs and back/forward lists; routes input |
| `page.h` | the interface a page implements |
| `page_doc.c` | implements it on the document view (`src/page`) |
| `commands.c` | the command table: menus, titles, shortcuts, enable rules, running commands |
| `context.c` | contextual menus |
| `favicon.c` | site icons |
| `present.c` | the shells' damage list |
| `credits.c` | the About box and about:licences table |
| `prefs.c`, `prefs_ui.c` | the preferences and their file; the Preferences window's descriptions and editing session |
| `lists.c` | the models of the Bookmarks, History, Downloads and Cookies windows |
| `listview.c` | the painted list the Mac OS 9 list windows show |
| `app.c`, `app_net.c` | the application services: the files Aura keeps, loading and saving them, session restore, the network stack set up from the preferences ("Files Aura keeps") |

The model never calls the OS; the shells pass events in and show the
rectangles it reports through `AuBrowserHost.invalidate`.

## To render a browser window on the host

Run:

```bash
build/host/src/platform/host/aura-host chrome -w 800 -h 600 --tabs left \
  --tab about:home --tab file:tests/page/samples/wiki.html -o run/chrome.png
```

`--tab` takes a URL, a path or `file:` with a relative path; `--scroll
Y` scrolls the active tab and `--mouse X Y` moves the pointer (hover, a
link's status text). Run `aura-host` without arguments for the other
options (tab overflow, hover, address field text). `--net` loads http
and https pages over the network. `--do ACTION` runs actions after the
page loads; the actions are listed where they are used:
[`forms.md`](forms.md) (clicks, keys, typing),
[`developer-tools.md`](developer-tools.md) (netlog, perf, source),
[`javascript.md`](javascript.md) (js, console, save),
[`memory.md`](memory.md) (mem) and below (back, cache, resize).
`--color-scheme light|dark` sets `prefers-color-scheme` for `chrome`
and the single-page renders.

## How a page loads

Pages load asynchronously: `au_page_navigate` only starts a load, and
`au_page_idle` reads files (the document, then its style sheets,
@imports and images in request order) and feeds the parser, at most
40 ms per call.

- While a document loads it is laid out at most every 300 ms (four
  times the last layout's duration for slow pages), then once at the
  end.
- Generated about: pages (home, blank, fonts) load inside
  `au_page_navigate`; about:scene paints the test scene directly.
- A URL that differs from the current one only by its fragment scrolls
  instead of loading, and back and forward restore the scroll position
  each entry was left at.
- Networking comes through a fetcher per scheme
  (`au_browser_set_fetcher`, `AuPageFetcher` in `page.h`); the Mac OS 9
  shell registers `src/net/page_fetcher.c` for http and https, and
  `aura-host chrome --net` does the same on the host. Without one, http
  and https show a placeholder after a simulated load. See
  [`networking.md`](networking.md).
- Images are decoded by `src/image` (see [`images.md`](images.md));
  `au_page_idle` advances animations.
- While a page loads, the status bar shows its progress in the middle
  (`AuChromeState.progress_text`).

## Scrolling while script runs

To let the window scroll and paint while a page's script runs a long
task, give the browser coroutines that run on the thread of the user
interface: `AuBrowserHost.script_coro`. The Mac OS 9 shell gives the
Thread Manager's cooperative threads, and `aura-host chrome` gives the
POSIX ones (it has no user interface). The GTK and Cocoa shells give
none: their coroutines are threads, and the page's script calls the
shell's callbacks from its stack.

With them, `au_page_idle` runs the page's script slice
(`au_docview_run_scripts`) on a coroutine (`page_doc.c`, "Script that
parks"):

- Every 100 ms of script the engine's interrupt check calls
  `AuJsHooks.yield` (`settings.yield_ms`), between two steps of the
  script and never inside a native. The page leaves the coroutine there:
  the page is *parked*, its view frozen (`au_docview_set_frozen`), and
  `au_page_idle` returns.
- The next `au_page_idle` goes on with the script from where it left.
  Nothing else of the page runs until the slice ends.
- While the page is parked:
  - It paints and scrolls from the last layout: style and layout
    updates wait. Boxes hold their nodes, styles and text, so a
    document the script has half changed is safe to paint.
  - A restyle that no layout followed is not: `getComputedStyle`
    restyles, and the restyle rebuilds subtrees of boxes
    (`au_layout_styles_changed`), so the stacking lists name dropped
    boxes until the next `au_layout_update`. The yield hook therefore
    lays the document out before it parks when `au_layout_settled` is
    false (`h_yield` in `script.c`), as `offsetHeight` would at that
    point. A frame left unsettled paints white, and its hit test finds
    nothing, until the slice ends. Without this, Reddit's feed crashed
    the paint on the host and froze Mac OS 9 (2026-11-18).
  - What the network delivers is held (`Held`) and handed to the view,
    in order, when the slice ends. The main document's response and its
    end run the slice to its end first.
  - Mouse moves are dropped. The scrolling keys (arrows, Page Up and
    Down, Home, End, space) outside a field go to the browser.
  - Presses, releases and other keys wait in a queue of 16
    (`queue_input`) and reach the page in order when the slice ends
    (`deliver_input`, from `au_page_idle`), as a browser queues input
    behind a long task; the window scrolls meanwhile. A key or press
    that comes after the slice ends goes behind what is queued. Stop,
    a navigation and closing the tab drop the queue. A full queue
    thaws.
  - Any other call first runs the slice to its end without parking
    (`thaw`), as if it had never parked: Find, the console, a resize.
  - A navigation, Stop and closing the tab stop the script instead
    (`stop_script`): it ends with the long-script limit's uncatchable
    error. Stop is enabled while a page is parked
    (`au_page_script_parked`).
  - A dialog the script opens (the insecure-form confirmation, a file
    chooser, a pop-up menu) runs on the main stack (`on_main`).
- Time parked is not script time: it counts neither toward the
  long-script limit nor in the Performance window.
- One coroutine serves every page, so Aura holds one more script stack
  (the runtime's stack limit and half again: 768 KB on Mac OS 9), not
  one per tab. A page whose script runs while another page's is parked
  runs it on the caller's stack, as without coroutines.

Checked on stock Mac OS 9.2.2 in the guest on 2026-11-18: during a
6-second loop, Page Down scrolled the window at once, and the script
read the new `scrollY` when it ended. A loop longer than 10 s of script
time is still stopped by the long-script limit.

To test it, run `test_script_park` (`tests/browser`), or load a page
with `aura-host chrome` and send keys while its script runs:

```bash
aura-host chrome -w 640 -h 480 -o out.png --tab file:///path/long.html --do pause:600 --do key:pagedown --do shot:during.png
```

## Font preferences and text zoom

The font preferences reach the engine as the generic families of the
font set and, for the default and monospace sizes, the minimum size and
the tab's text zoom, `au_docview_set_font_prefs` (which passes them to
`au_css_resolver_set_font_prefs`).

Text zoom is per tab (`au_bw_zoom`, View > Zoom In, Zoom Out, Actual
Size); the last zoom chosen for a site (host, or scheme for `file:`) is
kept in the `zoom.sites` preference and applied when a tab opens that
site again.

## Appearance

The `appearance.color_scheme` preference (`AuPrefs.color_scheme`) is
`system` (the default), `light` or `dark`. A preferences file without
the key gives `system`; an unknown value is ignored.

- `au_browser_dark(b)` is the scheme pages get: Dark, or Same as system
  while `b->system_dark` is set.
- To report the system setting, a port calls
  `au_browser_set_system_dark(b, dark)` at start and whenever it
  changes. The GTK port reads it from the desktop
  ([`gtk.md`](gtk.md), "Appearance"); Mac OS 9 has no such setting, so
  Same as system is Light there.
- When either changes, every tab of every window restyles in place
  (`au_page_set_dark`, then [`css.md`](css.md), "The colour scheme"):
  no reload, and frames follow. A new tab gets the scheme through the
  `dark` callback of `AuPageEnv`.
- In Preferences it is the Appearance pop-up in General, an
  `AU_PREF_LIVE` item: a choice applies at once and Cancel puts it
  back. `au_prefs_edit_apply_live` returns `AU_PREF_CHANGED_APPEARANCE`
  so the port can theme its own chrome.

`test_color_scheme_prefs` and `test_color_scheme_live` in
`tests/browser/test_browser.c` cover the file and the live switch.

## Documents kept for Back and Forward

Back and Forward show a kept document instead of loading the page
again. To change that, work in `page_doc.c` ("Kept documents") and
`browser.c` ("The document cache"); `page.h` describes the interface.

**To try it on the host**, run `aura-host chrome --net --tab URL --do
go:URL2 --do back --do cache`: `back` prints whether the kept document
was shown and how long it took, `cache` the kept documents and their
estimated size. `--no-doc-cache` turns the document cache off.
`tests/browser/test_back_cache.c` covers it on the fake transport.

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
  navigation (`entry_load` in `browser.c`). A navigation path that
  skips it gets documents that are never kept.
  `au_page_retain_entries` drops the documents of entries that are
  gone.
- **What is not kept:** a document still loading or with fetches under
  way, a failed load, an internal page, a POST result, `Cache-Control:
  no-store` on the main document (`AuFetchResponse.no_store`), and a
  window with an unload or beforeunload listener
  (`au_docview_can_persist`). A reload replaces the document. Reopen
  Closed Tab, duplicated tabs and session restore load through the
  history cache mode ([`networking.md`](networking.md)).
- **Frames.** A kept document keeps its nested documents, and they
  pause and resume with it (see [`frames.md`](frames.md)).
- **While hidden** nothing calls the view, so timers, animation frames,
  image animation and the caret wait, and run again after Back. The
  view's callbacks and loader in `page_doc.c` ignore a view that is not
  the page's current one. `au_docview_set_hidden` fires pagehide and
  pageshow with `persisted` true, drops decoded image pixels (they
  decode again when painted) and runs the script collector.
- **A hidden document frees its layout** (`au_layout_release`): the box
  tree, lines, fragments, stacking data, measured words and font lists.
  The first `au_docview_update` after it is shown builds them again,
  before the paint (`AuPageStats.rebuild_ms`). The scroll offsets of
  inner scroll containers live in the boxes, so they are saved and put
  back (`au_layout_scroll_save`); the page's scroll position, form
  contents and focus are in the tab, the tree and the view and are
  untouched. The estimate below is taken after the layout went.
  Computed styles, the rule index and canvas bitmaps stay. A text
  selection does not survive Back (a navigation clears it).
- **Limits.** `cache.documents` (3 per tab, 0 turns the cache off) and
  `cache.documents_kb` (24 MB over all tabs, by
  `au_docview_memory_estimate`; a document larger than that is not
  kept). `au_bw_idle` evicts the oldest over the budget
  (`au_browser_trim_documents`). On Mac OS 9, `idle_all` in `main.c`
  drops the oldest kept document while less than 6 MB of heap is free,
  and `au_alloc` calls the release handler (`au_set_release_handler`,
  `release_caches` in `main.c`) to drop them all before the
  out-of-memory alert. Clear Cache in Preferences drops them too.

Measured (Wikipedia, live):

| Where | What | Time |
|---|---|---|
| Host | Back to a kept article | 7 to 10 ms |
| Host | the same Back through the history cache mode | 450 to 800 ms |
| Guest (stock base) | Back to a kept article, in the engine | 56 ms, 46 ms of it hiding the page left |
| Guest (stock base) | page painted after the key | 971 ms |
| Host, with the layout freed (2026-11-15) | Back to a kept article of 3000 boxes; of 11,300 boxes | 9 ms; 26 to 28 ms |
| Guest (stock base), with the layout freed | Back to a kept article of 3000 boxes, in the engine | 48 ms: 10 ms hiding the page left, 31 ms building the layout |
| Guest (stock base), with the layout freed | page painted after the key | 220 ms |
| Guest (stock base) | loading the same articles | 30 to 70 seconds |

- On the host an article is estimated at 12 to 15 MB (its script heap
  is most of it), a very long one (World War II, 2 MB of HTML) at over
  30 MB, which is not kept.
- In the guest the change showed on screen about 3 seconds after the
  key was sent, because the page being left was still running its
  scripts.
- Develop > Performance shows these times for a page that Back or
  Forward showed from the kept documents (the "Page load" line), with
  the number and estimated size of kept documents in its Memory section
  ([`developer-tools.md`](developer-tools.md)).

## Unloaded tabs

A background tab can give up its document to save memory. To change
that, work in `browser.c` ("Unloaded tabs"; `browser.h` describes the
rules) and `page_doc.c` ("Discarding the document").

**To try it on the host**, run `aura-host chrome --net --tab URL1 --tab
URL2 --do unload:0 --do tab:0`: `unload:N` is the tab's Unload Tab
command and prints what the tab held, `tab:N` selects a tab and prints
how long an unloaded one took to load again. `tests/browser/test_unload.c`
covers it on the fake transport.

- **What stays:** the tab with its title (drawn dimmed,
  `AuChromeTab.unloaded`), icon, address, back/forward list, zoom and
  scroll position; the session file names it as before. The document
  view, its kept documents, the page source, the request log and the
  console go (`au_page_discard`). No pagehide or unload event fires.
- **Coming back:** selecting the tab loads its entry through the
  history cache mode ([`networking.md`](networking.md)), scrolled to
  where it was (`load_unloaded`). Reload on an unloaded tab loads it in
  the background.
- **Which tabs:** never a window's active tab or a tab that is loading;
  and none that holds what a reload would lose (`au_page_can_discard`):
  a POST result, a form control the user changed or gave files
  (`AuControl.user_interacted`; a value set by script does not count),
  a window with an unload or beforeunload listener, a worker, a canvas
  with a bitmap (drawn by script or by the user cannot be told apart),
  transfers under way, an internal or failed page. Nested documents
  count (`au_docview_has_state`).
- **When.** `au_bw_idle` applies the rules once a second while the
  `tabs.unload_low_memory` preference is on (Preferences > Network,
  "Unload background tabs when memory is low"; on by default) and the
  shell reports free memory (`AuBrowserHost.memory_free_kb`: on Mac OS 9
  the free heap plus free temporary memory, the figure the script
  memory limits use):
  - under `tabs.unload_free_kb` (6144) free, the oldest kept document
    goes first, then the least recently shown eligible tab, one a
    second;
  - under `tabs.unload_idle_kb` (16384) free, a tab not shown for
    `tabs.unload_idle_minutes` (30; 0 turns this rule off) goes.
- **Before the out-of-memory alert**, the allocator's release handler
  (`release_caches` in `main.c`, `au_browser_release_memory`) drops the
  kept documents, and when there are none, every eligible tab. The tab
  whose page `au_bw_idle` is inside is left alone (`AuBrowser.idle_page`),
  since its document is on the stack.
- **By hand:** a tab's contextual menu has Unload Tab (`AU_CMD_UNLOAD_TAB`),
  enabled when the tab is eligible; it does not ask the preference.
- **Session restore** loads each window's active tab and brings the
  others back unloaded (`au_page_set_discarded`), so a session of many
  tabs starts as fast as one of a single tab; `tabs.unload_restored`
  set to false loads them all.

**To make the rules act in the guest**, raise `tabs.unload_free_kb`
above the free memory: the guest has 400 MB of temporary memory, so
6 MB is never reached there. The measurements in
[`memory.md`](memory.md) ("Cold memory") were taken with a build whose
default was 425,000.

## Back, Forward and closed tabs

- A Command-click on Back or Forward opens that entry in a new tab
  behind (`au_bw_go_new_tab`).
- Holding the button for half a second (30 ticks) shows a pop-up of the
  entries that way (`au_bw_history_items`, `history_popup` in
  `browserwin.c`).
- File > Reopen Closed Tab (Shift-Command-T) brings back the last of up to
  `AU_MAX_CLOSED_TABS` closed tabs with their history and scroll
  position.
- Each window's position goes into the session (`au_bw_set_position`
  after a drag or zoom) and a restored window returns there when its
  title bar is still on screen.

### Entries a page's script makes (the history API)

`history.pushState` gives the tab a back/forward entry without a load,
and `history.back()`, `forward()`, `go()` and `length` act on the tab's
list. The path is `AuJsHooks` (`history_push`, `history_go`,
`history_length`) to `script.c`, `AuDocViewCallbacks`, `page_doc.c`
("The history API") and `AuPageCallbacks`, which `browser.c` answers
(`cb_history_push`, `cb_history_go`, `cb_history_length`).

- The page keeps the entries of the shown document (`AuPage.sd`): the
  one it loaded in and the ones its script pushed, each with the key
  the script named its state with. A move to one of them
  (`au_page_navigate_entry`) calls `au_docview_pop_state`: the URL
  changes and the script gets `popstate` with that entry's state, and
  `hashchange` when only the fragment differs. Nothing is fetched.
- A document kept for Back and Forward takes its entries with it
  (`Kept.sd`), so Back into a kept page's pushed entries pops state too.
- The document's URL and, without a `<base>`, its base URL follow
  `pushState` (`au_dv_set_url`, `au_js_set_url`), so relative links
  resolve against the new address.
- `history.go` waits until the script turn ends (`go_pending` in
  `script.c` and `page_doc.c`); a navigation asked for in the same turn
  wins. A tab in the background does not move.
- Only the top-level document reaches the list: a frame's pushState
  changes the frame's URL alone ([`known-gaps.md`](known-gaps.md)).
- On the host, `--do back` prints "popstate in the shown document" for
  such a move (`AuPageStats.popped`). `test_push_state` in
  `tests/browser/test_back_cache.c` covers it on the fake transport.

Before 2026-11-18 none of these hooks was set: the address field kept
the old URL, Back left the site, and `popstate` never fired.

## Site icons

Tabs show their site's icon. To change that, work in
`src/browser/favicon.c` (`favicon.h` describes it).

- When a document has loaded, `cb_finished` in `browser.c` asks the
  page for its icon URL (`au_page_icon_url` in `page_doc.c`: the `<link
  rel=icon>` whose `sizes` suit 16 pixels best, else `/favicon.ico`;
  http and https documents only).
- `au_favicons_request` fetches it through the scheme's fetcher as an
  image subresource of the page, so the blocklist, mixed content
  blocking and the HTTP cache apply. An icon on another host is fetched
  without cookies (`AuFetchRequest.omit_credentials`).
- The bytes (at most 512 KB) are decoded with `au_image_decode_favicon`
  to 16 x 16 and kept per host, at most 200 hosts; a host is asked
  again after a week, and a host that gave no icon is not asked again
  while Aura runs.
- `au_bw_idle` polls the fetches and repaints the tab strip when one
  arrives; `chrome.c` draws `AuChromeTab.icon`, and the bookmark and
  history lists draw the icon of each link's host (`AuListView.icons`).
- Fetching is off until the shell sets `AuBrowser.site_icons` (the Mac
  application and `aura-host chrome` do), so an embedder or a test with
  a scripted server gets no request it did not expect; the
  `load_images` preference turns it off too.
- The Mac shell saves the store as `Favicons` in the Aura preferences
  folder (`storage.c`), keeps it in memory only when history is off,
  and clears it wherever it clears the history, because it names
  visited hosts.

`tests/browser/test_favicon.c` covers it on the fake transport.

## Keyboard focus and the address field

Keyboard focus follows one rule (`browser.c`, "Keyboard focus"): after
a navigation or a tab switch the page has the focus, unless the user is
typing in the address field; a tab showing a fresh blank page puts it
in the field. The model reports the move with `AU_CHANGED_FOCUS_PAGE`
or `AU_CHANGED_FOCUS_FIELD`.

Open Location selects the whole address again when the field already
has the focus, and a paste into the field turns line breaks into
spaces.

To complete the address as the user types, call `au_bw_complete_field`
with the field's text after each keystroke that leaves the caret at the
end: it finds the best history or bookmark match and returns the field's
new text, and the platform selects the part after what was typed. The
matching (`au_complete_fill`) ignores the scheme, `//` and `www.` unless
the user typed them. History and bookmark menu items get their text
from `au_menu_title` (`history.c`), which shortens long titles at a
character boundary. Keep logic like this in `src/browser`, where the
host tests reach it; the platform code only sets the text field or adds
the menu item.

## The horizontal scroll bar

A horizontal scroll bar appears along the bottom of the page area while
the active page is wider than it (`au_chrome_layout_hscroll`); the two
bars meet at a blank corner square above the grow box, and Shift turns
a vertical wheel scroll horizontal (Mac OS 9 itself reports no wheel).
[`layout.md`](layout.md) says how its state is kept across a resize.

## Native chrome

To show a browser window's tabs, toolbar, address field, progress,
status and scroll bars with a port's own widgets, create the window with
`au_browser_window_new_native(b, width, height)` instead of
`au_browser_window_new`. The choice is fixed for the window's life
(`AuBrowserWindow.native_chrome`). The Mac OS 9 port uses painted
chrome; [`porting.md`](porting.md) says which a port should use.

In native chrome mode:

- `width` and `height` are the page's size, and `au_bw_resize` takes
  the page's new size. The layout (`au_chrome_layout_page_only`) makes
  the page the whole surface; every other region is empty, so no tab
  strip, toolbar, status bar or scroll bar column is laid out, painted
  or hit.
- `au_bw_paint` paints only the page, and `invalidate` reports only page
  rectangles.
- Mouse events are in page coordinates: every point inside the surface
  is `AU_HIT_PAGE`. When the pointer leaves the page widget, call
  `au_bw_mouse_leave` so that hover ends and a link's status text goes.
- Keys, the wheel (Shift turns it horizontal), Find, the Edit commands
  and the address field functions (`au_bw_field_text`,
  `au_bw_set_field_text`, `au_bw_set_field_focus`, `au_bw_navigate_input`,
  `au_bw_complete_field`) work as in painted mode.

To read what the painted chrome shows, call these. They work in both
modes, and the painter itself is built on them (`paint_chrome` in
`browser.c`), so the two cannot disagree.

| Shows | Call |
|---|---|
| tabs: count and active tab | `au_bw_tab_count`, `au_bw_active_index` |
| a tab's label, address, loading state, progress, site icon (16 x 16 `AuSurface`, NULL for the generic page icon), unloaded | `au_bw_tab_info` (fills an `AuChromeTab`) |
| Back and Forward enabled | `au_bw_can_go(w, -1)`, `au_bw_can_go(w, 1)` |
| Reload or Stop | `au_bw_loading` |
| progress bar and its text ("Loading, 40%") | `au_bw_progress`, `au_bw_progress_text` |
| security button | `au_bw_security` |
| bookmark star | `au_bw_is_bookmarked` |
| status text (link under the pointer, else what the load is doing) | `au_bw_status_text` |
| blocked requests | `au_bw_blocked_count` |
| window title | `au_bw_window_title` |
| scroll position and range | `au_bw_scroll_pos`, `au_bw_scroll_max` |

To act on what the user does with the port's widgets, call these:

| Widget | Call |
|---|---|
| select a tab | `au_bw_select_tab` |
| close a tab | `au_bw_close_tab` |
| reorder tabs (drag) | `au_bw_move_tab(w, from, to)` |
| New Tab button | `au_bw_open_new_tab` (a blank tab at the end, the field focused) |
| Duplicate, Reopen Closed Tab, Close Other Tabs, Unload Tab | `au_bw_duplicate_tab`, `au_bw_reopen_closed_tab`, `au_bw_close_other_tabs`, `au_bw_unload_tab` |
| Back, Forward | `au_bw_back`, `au_bw_forward`; with Command, `au_bw_go_new_tab(w, -1 or 1)` |
| history pop-up on Back or Forward | `au_bw_history_items`, then `au_bw_go(w, delta)` |
| Reload or Stop | `au_bw_reload_or_stop(w, bypass)`; bypass for Shift or Option |
| Home | `au_bw_home` |
| bookmark star | `au_bw_bookmark_page(w, true)` |
| security button | `au_bw_show_certificate` |
| scroll bars | `au_bw_scroll_to` |

`au_bw_press(w, hit, mods)` does what a click on a painted part does,
with its modifier keys; painted mode routes its clicks through it. Tab
scrolling and the tab column's splitter have no function: a port's own
tab bar scrolls itself.

To know what to refresh, read the bits of `AuBrowserHost.changed`. The
model sends them from every place the state changes, in both modes, and
several can arrive in one call.

| Bit | Refresh |
|---|---|
| `AU_CHANGED_TABS` | the tab list: a tab was added, closed or moved; read the list and the active index again |
| `AU_CHANGED_TAB_INFO` | tab labels: a title, address, site icon, loading or unloaded state changed |
| `AU_CHANGED_ACTIVE_TAB` | the selected tab |
| `AU_CHANGED_NAV` | Back, Forward, and Reload or Stop |
| `AU_CHANGED_STATUS` | the status text |
| `AU_CHANGED_SECURITY` | the security button |
| `AU_CHANGED_PROGRESS` | progress: a tab's, and the active tab's progress text and blocked count |
| `AU_CHANGED_BOOKMARK` | the bookmark star |
| `AU_CHANGED_TITLE` | the window title |
| `AU_CHANGED_FIELD` | the address field's text, unless the user is typing in it |
| `AU_CHANGED_SCROLL` | the scroll bars: position or range |
| `AU_CHANGED_LAYOUT` | the page's size changed (in native mode, after `au_bw_resize`) |
| `AU_CHANGED_MENUS` | menus built from tabs, history or bookmarks |
| `AU_CHANGED_FOCUS_FIELD`, `AU_CHANGED_FOCUS_PAGE` | the keyboard focus (see "Keyboard focus and the address field") |

A tab selection sends the bits for everything that belongs to the active
tab, so a port can refresh the toolbar from it alone.

To show scroll bars, set each bar's range from `au_bw_scroll_max` and
its value from `au_bw_scroll_pos` on `AU_CHANGED_SCROLL`, and call
`au_bw_scroll_to(w, x, y)` when the user moves either bar. The page
area does not shrink for a horizontal bar in native mode: the port puts
its bars outside the page's surface and passes the remaining size to
`au_bw_resize`.

To check native chrome by hand, run:

```bash
build/host/src/platform/host/aura-host chrome --native-chrome -w 640 -h 400 \
  --tab about:home --tab file:tests/page/samples/wiki.html -o run/native.png
```

It renders only the page, prints each notice as it arrives and prints
the chrome's state after the loads and at the end; `--do click:X,Y`
takes page coordinates. `tests/browser/test_native_chrome.c` covers the
page-only surface, clicks on links in page coordinates, every action,
the accessors and each notice in both modes, and scrolling both ways;
`test_favicon.c` checks that a site icon's arrival sends
`AU_CHANGED_TAB_INFO`.

## Save Page As

To change Save Page As, see `au_page_source` (the page keeps the main
document's bytes as they arrived, until the next load; it holds them
deflated once the load is over and inflates them for the call, so use
or copy them before returning to the event loop, and ask
`au_page_has_source` when only their presence matters),
`au_bw_save_name` (the suggested file name) and `mac_save_page` in
`storage.c`, which asks with `StandardPutFile` and writes the bytes
unchanged as a TEXT file with creator AURA.

## The start page (about:home)

To change the start page, edit `build_home` in `src/browser/page_doc.c`.
It is generated HTML with no script, built each time it loads:
- **Header:** the kookaburra, `assets/icon/kookaburra/home.png` (72 px),
  embedded with `#embed` and sent as a `data:` URL.
- **Search:** a form made from the search preference's template. The
  parameter whose value is `%s` becomes the text field and the others
  hidden fields. A template without such a parameter gets no form.
- **Most visited:** up to 8 web pages visited at least twice, from the
  history, most visits first.
- **Bookmarks:** the first 12 links in the bookmark tree.
- **Sites that work well:** the `home_sites` table.
- **Tip:** one entry of `home_tips`, chosen by the clock.
- **Footer:** the version, the author, and links to GitHub, the licences,
  the fonts and the test scene.

History and bookmarks reach the page through `AuPageEnv.history` and
`AuPageEnv.bookmarks`. `test_home` in `tests/browser/test_page_doc.c`
checks the form, the escaping and the sections.

## The About box and about:licences

To change the About box or about:licences, edit the credits table in
`src/browser/credits.c` (the version, the tagline, and one entry per
bundled font, third-party library and data set with its licence);
`aboutbox.c` and `build_licences` in `page_doc.c` both read it. Data
under the MPL (the root store, the Public Suffix List) carries the MPL
notice and where its source form is. QuickJS-ng and Mbed TLS have their
entries there.

The About box (DLOG 202, `aboutbox.c`) draws all its text itself in
user items:
- the name, the version (`au_build_identity`) and `AU_TAGLINE`;
- a link to `AU_HOMEPAGE`, which opens it in a new tab;
- `AU_AUTHOR`, the libraries from the credits table, and the Mac it runs
  on (`mac_sysinfo`);
- a quip from the `quips` table, picked at random. A click on the
  kookaburra shows the next one.
Fonts and data are not listed there; Credits & Licences opens
about:licences, which lists them all. To change the wording, edit
`aboutbox.c` and the macros in `credits.h`.

The About box draws icon family 128 at 64 pixels with `PlotIconID`. Its
empty `dctb` gives it a colour port; without one the icon draws in
black and white.

## Internet Config

To change how Aura works with Internet Config, edit
`src/platform/macos9/internetconfig.c`.

- Mac OS 9.2.2 has the Internet Config component ('PREF', 'ICAp') but
  no InternetConfigLib shared library (GetSharedLibrary fails with
  -2804 in the stock guest), so Retro68's InternetConfigLib stub cannot
  be used: Aura opens the component with `OpenDefaultComponent` and
  makes each call through `CallComponentUPP` with a parameter block,
  checking every selector with `kComponentCanDoSelect` first.
- `internetconfig.h` declares what the Multiversal Interfaces lack (the
  IC records, in 68K alignment, and the Component Manager calls).
- Addresses Aura does not open itself (`au_browser_opens_url`) reach
  the shell through `AuBrowserHost.open_external`, which calls
  `ICLaunchURL`.
- The GURL Apple event opens its URL in a new tab, or a new window with
  `links.external_new_window`. To send a GURL in the guest, run `open
  location "URL"` in Script Editor.
- Preferences > General has the Make Aura the Default Browser button,
  the only place Aura writes Internet Config (the http, https and file
  helpers).
- `mac_ic_download_folder` and `mac_ic_map_filename` are there for
  downloads.

## Commands and menus

To add a menu command, add an `AU_CMD_*` value in
`src/browser/commands.h`, its row in the `commands` table in
`commands.c` (menu, title, shortcut), its place in the menu's item
list, its enable rule in `au_command_enabled` (commands are enabled
unless a rule says otherwise) and its action in `au_command_run`. Then
add a case to `tests/browser/test_commands.c`. No port changes unless
the command opens a native window.

- **The table.** `au_command_info` gives each command's title (UTF-8;
  ports convert), menu, shortcut and submenu. `au_menu_info` gives each
  menu's title, fixed items in order (`AU_CMD_SEPARATOR` is a divider)
  and the dynamic list that follows them; `au_menu_bar` gives the
  menu bar's order. Help's items go after the system's own Help items
  on Mac OS 9.
- **Shortcuts.** An `AuShortcut` is a key (an upper-case letter, digit
  or punctuation) and modifiers: `AU_SHORTCUT_PRIMARY` is Command on Mac
  OS 9 and Ctrl on Linux, plus `AU_SHORTCUT_SHIFT` and
  `AU_SHORTCUT_ALT`. Each key and modifier set belongs to one command.
  To find the command for a key press, call `au_command_for_key`, which
  also makes Close Tab's shortcut close a window that has no tabs.
- **State.** The port fills an `AuCommandState`: the browser, the
  frontmost browser window (in front or not), what kind of window is in
  front (`AU_FRONT_BROWSER`, `AU_FRONT_TOOL` for a developer window,
  `AU_FRONT_OTHER`, `AU_FRONT_NONE`), whether a text field has the
  focus, whether the developer window can find again, and the modifier
  keys held (Reload with Shift or Option reloads past the cache).
- **Enabled, checked, title.** Before a menu is shown or a shortcut is
  looked up, call `au_command_enabled` and `au_command_checked` for each
  item. `au_command_title` gives the title to show for commands whose
  title follows the state (Turn Off or Turn On Blocking for This Site).
- **Running.** `au_command_run` does nothing for a disabled command and
  otherwise does what the command does with `au_bw_*` calls. What needs
  the port comes back through its `AuCommandHost`: `new_window`,
  `raise`, `focus_address`, `show_window` (About, Preferences, the list
  and developer windows, the timing test and paint benchmark),
  `choose_file_url`, `save_page`, `close_front`, `find`,
  `tool_find_again`, `edit` (Undo to Select All, Paste and Go),
  `copy_text`, `select_window`, `beep` and `quit`. A port without a
  Page Source window returns false from `show_window` for it, and the
  model opens a `view-source:` tab instead. To open a URL from a port
  (a GURL event, a list window), call `au_command_open_url`.
- **Dynamic menus.** `au_menu_list_history` lists a separator and up to
  `AU_GO_MENU_ENTRIES` recent pages for Go. `au_menu_list_bookmarks`
  lists the bookmarks breadth-first; an entry's `parent` is the index of
  its folder entry (-1 for the menu itself), every entry comes after its
  folder, and an empty folder holds a disabled "(empty)" item.
  `au_menu_list_windows` takes the port's visible windows front to back
  and lists a separator and one item each, the front one checked. History
  and bookmark titles come from `au_menu_title`; window titles are
  used as they are. To run an entry, call
  `au_menu_entry_run`; a bookmark is looked up by id when it runs, so a
  deleted one does nothing. Free a list with `au_menu_list_free`.

The Mac OS 9 menus are built from the table at start-up
([`mac-os-9-shell.md`](mac-os-9-shell.md), "Menus").

## Contextual menus

To change what a contextual menu offers, edit `src/browser/context.c`:
`au_bw_context_menu` builds the items for a window point (link, image,
page, address field or tab, using `au_page_hit_info` for the page),
with labels from the command table. To run the chosen item, call
`au_command_run_context`: it runs the model's commands with
`au_bw_context_command` and passes the rest to the port's
`AuCommandHost` (the clipboard, editing in the address field, a new
window, the Page Source window).

On Mac OS 9, `contextmenu.c` finds the Contextual Menu Manager in the
ContextualMenu shared library at run time (the Multiversal Interfaces
do not declare it) and falls back to `PopUpMenuSelect`; Control-click
opens the menu, and so does holding the button still on the page for
45 ticks.

## Selection and find

To change text selection or find, work in two places:

- `src/layout/selection.c`: positions, words, paragraphs, the text of a
  range, find, and the painted highlight; `draw.c` calls
  `l_select_paint_frag` for each text fragment while a selection is
  set.
- The selection section of `page_doc.c`, which turns mouse events into
  selections: a drag selects characters, a double click a word, a
  triple click a paragraph, Shift extends, and a drag that selects does
  not follow the link it started on.

`au_page_find` searches the page's text with simple case folding
(ASCII, Latin-1, Greek, Cyrillic, Latin Extended-A), from the
selection, wrapping. Selection and find leave inert content out (see
[`layout.md`](layout.md), "Inert content"), so with a modal dialog open
they work in the dialog only.

The Mac OS 9 shell counts clicks within the double-click time, copies
with `mac_put_scrap` (TEXT in Mac Roman with transliteration and
carriage returns, plus 'utxt'), asks for the text with the Find dialog
(DLOG 201), and paints selections in the Appearance highlight
colour (`LMGetHiliteRGB`, passed in `AuBrowser.highlight`).

Script has a selection of its own that is not joined to this one (see
[`javascript.md`](javascript.md), "Selection").

## Scrolling by moving pixels

Scrolling moves pixels instead of repainting the page. To change it,
work in these places.

**`browser.c` (`page_scrolled`).** A scroll by the user
(`au_bw_scroll_to` and everything built on it: wheel, arrows, page
keys, the scroll bars) calls `AuBrowserHost.scroll_area` with the page
area and the distance, then invalidates the strip that scrolled in and
the rectangles `au_page_scroll_damage` returns. Script scrolls, scrolls
of at least the page's size and hosts without `scroll_area` repaint the
page.

**`au_page_scroll_damage`** (`page_doc.c`, `au_docview_scroll_damage`,
`au_layout_scroll_damage` in `draw.c`) returns the rectangles that do
not move with the page: fixed boxes (old and new place), sticky boxes
whose offset changed, and their shadows and outlines. A box's extent
is the union of what it and its descendants paint: boxes of no area
add nothing, and a descendant that is a fixed or sticky box itself is
left to its own rectangles. (Reddit keeps an empty fixed toast
container at the top with a fixed slot at the bottom; their union was
a rectangle the height of the view, damaged at both places, and a
32-pixel step painted the view twice over.) It returns -1
when the whole page must be repainted: layout pending, a fixed or
gradient canvas background, more than `AU_PAGE_SCROLL_DAMAGE`
rectangles, or a viewport box under a transformed ancestor. Layout
records the fixed and sticky boxes while building stacking contexts
(`view_boxes`).

**`present.c` (`AuPresent`)** is the shells' damage list: up to 8
rectangles merged by least added area, and the pending shift.
`au_present_begin` moves the surface's pixels and hands over the
rectangles to paint; the shell then moves the screen (Mac OS 9:
`ScrollRect`, then `CopyBits` from the GWorld into the region it
reports) and copies the painted rectangles.

**Painting skips boxes outside the canvas clip** (`au_layout_paint`),
so a strip paints only what it shows. A transformed box is painted
into a layer that covers only the part of it the strip can show (the
dirty area mapped back through the transform, plus two pixels for the
filter; `paint_layer` in `draw.c`); before 2026-11-17 the layer was the
whole box, painted again for each strip. An `<img>` that its transform
only scales or moves, with no background, border, shadow, outline,
rounded corners, mask, clip path or object-fit, is drawn straight to
its transformed rectangle with no layer (`paint_scaled_image`): 20
images at `scale(1.2)` repainted in 1.8 ms instead of 4.3 ms (Release,
800x700), and closer to Chrome, since the image is resampled once.
Reddit's blurred post backgrounds are such images. Box and text ink extents
(shadows, outlines, inline SVG with visible overflow) must cover what
they draw. `rect_hits` in `draw.c` allows one pixel above and below,
because a text fragment is as tall as the face's rounded ascent and
descent and glyphs reach up to half a pixel past it.

**Drawing must not depend on the scroll offset**, or moved pixels
differ from a fresh paint by a coverage level on antialiased edges.

- `au_layout_paint` puts the whole pixels of the scroll offset in the
  canvas translation and paints boxes at document coordinates.
- `raster()`, `fill_rect_source`, `au_draw_border` and
  `au_draw_box_shadow` add the whole pixels of the translation to their
  integer bounds instead of to the coordinates.
- Keep new drawing code to that rule: take the fraction of the
  translation into the coordinates and add its whole pixels when
  writing pixels. `paint_svg` does the same for an inline SVG's place.
- To check a page, render it with `aura-host render PAGE -s Y` at
  several offsets and compare the rows that both images show.

**Hover follows a scroll only after the scroll pauses** for
`AU_REHOVER_MS` (150 ms). The delay dates from when a hover change
restyled the whole document on pages with `:has()` rules (1.7 s per
scroll step on Wikipedia in the guest); a hover change now restyles
about two elements there (see [`css.md`](css.md)), and the delay has
not been measured again in the guest.

**Tests and timing.** `tests/browser/test_scroll_shift.c` scrolls
sample pages and inline pages with fixed, sticky and transformed boxes
both ways and compares the moved result with a full repaint pixel for
pixel; `SHIFT_DUMP=1` writes both images when they differ. It also
checks that a 32-pixel step over an empty fixed container paints a
small part of the view, and that a page of rotated, skewed and
translucent boxes painted in strips matches one whole paint
(`strips_match`; the shifting comparison cannot check those, because
the layer's filter rounds about one pixel in a thousand differently at
other scroll offsets, by one level). To time
scrolling on the host, run `aura-host chrome -w 1440 -h 1050 --tab PAGE
--scroll-bench 20` (add `--bundled-only` for reproducible fonts): it
times 32-pixel steps and page downs by repainting, then by moving
pixels (`--no-shift` skips that), through the Mac shell's paint path,
with pixels painted and copied, glyph cache counts and image decodes,
and the rectangles the first step of each kind painted. For a live
page, add `--net --load-ms 8000`: a page that keeps timers running
stays busy until the load limit (60 s with `--net`), and the benchmark
starts only after it.

## Files Aura keeps

Aura keeps its files in a profile folder: on Mac OS 9 the Aura folder
of the system Preferences folder. Which files there are, how each is
parsed, the order they are loaded in, what happens to a damaged one and
when each is saved is decided in `src/browser/app.c` and `app_net.c`
(`browser/app.h`); the port gives only the folder primitives (see
"To keep the files in a port" below).

| File | Holds | Kept by |
|---|---|---|
| `Aura Preferences` | preferences | `app.c` |
| `Bookmarks.html` | bookmarks (Netscape format) | `app.c` |
| `History` | history | `app.c` |
| `Session` | windows, tabs, back and forward lists, scroll positions | `app.c` |
| `Downloads` | the download list | `app.c` |
| `Favicons` | site icons | `app.c` |
| `Running` | exists while Aura runs | `app.c` |
| `Cookies`, `HSTS` | cookies and HSTS hosts | `app_net.c` ([`networking.md`](networking.md)) |
| `Intermediates` | the intermediate certificate pool | `app_net.c` ([`tls.md`](tls.md)) |
| `Cache` (folder) | the disk cache | `app_net.c`, `src/net/disk_cache.c` |
| `Local Storage` (folder) | web storage | `app_net.c` ([`web-storage.md`](web-storage.md)) |
| `Random Seed` | entropy carried between runs | the port ([`mac-os-9-shell.md`](mac-os-9-shell.md)) |
| `Blocklists` (folder) | compiled blocklists | the port ([`content-blocking.md`](content-blocking.md)) |
| `NetTest Log` | NetTest's output | [`networking.md`](networking.md) |

On Mac OS 9 the files have creator `AURA`: type `TEXT` in the Aura
folder (`SEED` for `Random Seed`), `AuCa` in `Cache`, `AuLS` in
`Local Storage`.

- **Loading.** `au_app_load` reads the preferences first, then the
  bookmarks (a missing file gives the first-run bookmarks), the history
  (expired by the preference), the downloads and the site icons, then
  marks Aura as running. `au_app_net_start` reads `Cookies`, `HSTS` and
  `Intermediates` when it makes the network stack.
- **Damaged files.** A file that fails to parse is renamed
  `<name> (damaged)` (replacing an older one) before anything can write
  over it, and the defaults are used. A damaged `Favicons` or
  `Intermediates` file is only dropped: its contents are fetched again.
- **Safe writes.** Each file is written to `<name>.tmp` and then
  replaced (Mac OS 9: `FSpExchangeFiles`, then the volume is flushed).
- **When to save.** `au_app_save` compares each data set's change
  counter (`prefs_gen`, `bookmarks.generation`, `history.generation`,
  `session_gen` with `scroll_gen`, `downloads.generation`) with the one
  last saved, and writes a set 3 seconds after its first unsaved
  change. Only changed sets are written; a failed write is tried again
  after the delay. The session waits 30 seconds while only scroll
  positions changed; site icons are written at most every 30 seconds;
  cookies and HSTS 5 seconds after a change; web storage as
  [`web-storage.md`](web-storage.md) says. `au_app_save(app, true)`
  writes every changed set at once: the Mac port calls it when Aura goes
  to the background, and `au_app_quit` at quit.
- **Crash recovery.** `Running` is made at start and deleted after the
  last save at quit; when Aura finds it at start, the last run crashed
  or ran out of memory, and `au_app_session` returns
  `AU_APP_RESTORE_ASK`: the Mac port offers to restore the session
  (ALRT 133). With the startup preference set to restore it returns
  `AU_APP_RESTORE_YES` and nothing is asked. To test this in the guest,
  press Command-Option-M (the out-of-memory test) and launch Aura
  again.
- **To start with clean state in the guest**, delete that folder.
- **On the host**, `aura-host chrome --profile DIR` loads and saves the
  same files in `DIR` (`src/platform/posix/files_posix.c`); `aura-host fetch` and `--net`
  keep `Cookies`, `HSTS`, `Intermediates` and `Local Storage` in
  `$AURA_PROFILE` (default `~/.aura-host`).

### To keep the files in a port

Fill an `AuAppPort` (`browser/app.h`): the `AuAppFiles` table, whose
calls take a folder (NULL for the profile folder itself, else
`AU_APP_FOLDER_CACHE` or `AU_APP_FOLDER_LOCAL_STORAGE`) and a name of
at most 31 bytes (`read` appending to an `AuBuf` up to a size limit,
`write` safely, `rename` replacing the target, `remove`, `exists`,
`make_folder`, `list`), and the clocks (`ms`, `wall_time`,
`utc_offset`; optional `wall_ms` installs script's clock, optional `us`
times file work for System Status). Then:

1. `app = au_app_new(browser, &port)`, and `unclean = au_app_load(app)`
   when the profile folder exists;
2. `au_app_net_start(app, &net)` with an `AuAppNet`: the port's
   `AuNetOps` and its optional hooks (entropy, the offline reason, the
   root store's bytes, download files, deferred cache writes, `started`
   and `stopping` around the loader for content blocking, its own
   System Status lines);
3. `au_app_session(app, unclean, &session)`, asking the user on
   `AU_APP_RESTORE_ASK`; with no session, open one window at
   `au_app_start_url(app)`;
4. once per pass of the event loop, `au_app_net_idle(app)` and
   `au_app_save(app, false)`;
5. at quit, `au_app_quit(app)` while the windows are open, then
   `au_app_net_stop(app)` after the pages are gone, then
   `au_app_free(app)`.

`tests/browser/test_app.c` runs all of this over an in-memory port.

## Downloads

Downloads go through `src/net/transfer.c` (`au_net_transfer_source_*`)
over the platform's `AuDownloadFiles`; on Mac OS 9 that is
`downloads_mac.c`: a partial file ("<id> <name>.part") in the download
folder (the preference, else Internet Config's, else the desktop),
renamed and given Internet Config's type and creator when complete,
resumed with Range. A page's response that turns out to be a download
(attachment or a type Aura cannot show) continues on its own transfer.
`au_sim_source` remains for tests.

## The list windows

The Bookmarks, History, Downloads and Cookies windows are built from
the list models in `src/browser/lists.h`. To change what a list shows,
which buttons a window has, when a button is enabled or what it does,
change `lists.c` and its test, `tests/browser/test_lists.c`; a port only
shows the model.

- **To show a list in a port**, call `au_list_sources` with the browser,
  the cookie jar and the storage service, then `au_list_init` with the
  kind (`AU_LIST_BOOKMARKS`, `AU_LIST_HISTORY`, `AU_LIST_DOWNLOADS` or
  `AU_LIST_COOKIES`) and an `AuListHost`. Show `AuListModel.items` in the
  port's list widget: `au_list_columns` gives the headings, each item's
  `cols` the text of each column, `depth` the indent, `type` whether it
  is a day heading, a folder or an entry, and `icon` and `icon_host` its
  icon. A list that shows one line of text and a detail uses `cols[0]`
  and `summary`.
- **To keep it current**, call `au_list_stale` from the event loop and,
  when it returns true, `au_list_refresh` and redraw. History, Bookmarks
  and Downloads compare their data's generation counter; Cookies compare
  the number of cookies, rules and storage areas and the storage size in
  KB. When the jar changes in a way those numbers do not show (Clear
  Cookies in Preferences), call `au_list_touch`.
- **To make the buttons**, call `au_list_buttons`: each has an action, a
  label and the width the Mac OS 9 window gives it. Enable each with
  `au_list_can` after every change of selection or data, and run it with
  `au_list_run`. A double click or Return calls `au_list_activate`, a
  click on a folder's triangle `au_list_toggle`, and the Delete key runs
  `AU_LIST_DELETE`. A port that lets the user drag bookmarks calls
  `au_list_move_bookmark`.
- **Selection.** The model holds one selected row (`au_list_select`).
  A refresh keeps it on the same id; when that row went, History and
  Downloads keep the position, Bookmarks select nothing, and Cookies
  select the site of a removed cookie.
- **Ids.** Bookmarks and downloads use their own ids, history entries
  `AuHistoryEntry.id` (given at each new entry, not saved), day headings
  `AU_LIST_ID_GROUP` plus the day number, and cookie rows a hash of what
  they name.
- **Dialogs.** Actions that ask call the port: `ask_text` for New Folder
  and Rename, `show_text` for a cookie's details, `choose` for Site
  Rule (the Mac OS 9 window pops it up under the button), and
  `reveal_download` for Reveal. `AU_LIST_FAILED` from `au_list_run`
  means the port should alert the user (the Mac beeps). No action asks
  for confirmation.

| Window | Rows | Buttons |
|---|---|---|
| Bookmarks | the bookmark tree; a closed folder hides its contents | Open (a folder opens or closes), New Folder, Rename..., Delete |
| History | entries most recent first, under a heading per local day | Open, Delete, Clear History (also clears the site icons) |
| Downloads | downloads oldest first, with state and progress | Stop, Resume, Remove, Clear Completed, Reveal |
| Cookies | a folder per site; an open site lists its cookies and storage areas | Details..., Remove, Remove All, Site Rule... |

The Cookies window (Window > Cookies, or Preferences > Privacy) lists
`src/net/cookie_view.c`'s model through `lists.c`; per-site rules
(allow, session only, block) live in the `cookies.sites` preference.

## Preferences

The settings are fields of `AuPrefs` (`prefs.h`), stored in the
`Aura Preferences` file. The Preferences window's contents are
described in `prefs_ui.h`, and a port builds its own window from them.

To build a Preferences window, walk `au_pref_desc(id)` for every
`AuPrefId` in order. Each item has a section (the pane), a kind (check
box, radio group, number with a range and unit, text, choice, font
family, folder, button, note, or the blocking list rows), a caption, a
title, choices, its preferences file key, the item whose choice enables
it, and flags: `AU_PREF_LIVE` for items that apply at once and
`AU_PREF_STATUS` for items with a status line. The port keeps only its
layout.

To run the window, use an editing session (`AuPrefsEdit`):

1. `au_prefs_edit_new(&browser->prefs, &host)` when the window opens.
   `AuPrefsHost` gives the font set, the free memory, the blocking
   manager, whether Aura is the default browser, and the words for the
   system ("this Mac", "Internet Config", an example folder path, the
   system's download folder). Load each control from
   `au_prefs_edit_get_bool`, `_get_choice` or `_get_text`, and the
   choices from `au_prefs_edit_choice` (the font families come from the
   font set, unique and sorted).
2. When the user changes a check box, radio group or choice, call the
   matching `au_prefs_edit_set_*`. For an `AU_PREF_LIVE` item (the tab
   position, the appearance, the blocking switch and lists), call
   `au_prefs_edit_apply_live`; it returns `AU_PREF_CHANGED_TABS`,
   `AU_PREF_CHANGED_APPEARANCE` or `AU_PREF_CHANGED_BLOCKING` for the
   port to act on. Then read
   `au_prefs_edit_enabled` and `au_prefs_edit_text` again.
3. For a button, call `au_prefs_edit_action`. It does the model's part
   (Clear History, Use Current Page, Update Now) and returns
   `AU_PREF_DO_*` bits for the parts that need the OS (clearing cookies
   and the cache, the Cookies window, making Aura the default browser).
4. On OK, set every text and number field with `au_prefs_edit_set_text`
   and call `au_prefs_edit_commit`, which writes the shown items into
   `b->prefs` and calls `au_browser_prefs_changed`. Items the window does
   not show (zoom per site, window sizes) are left as they are. On
   Cancel, call `au_prefs_edit_cancel`, which puts the live items back.
   Then `au_prefs_edit_free`.

The session checks values when they are set, with the rules the Mac
window used before the descriptions (tested in
`tests/browser/test_prefs_ui.c`):

- A number is clamped to its range, which is the preferences file's
  range. Text that is not a number gives the value the preference had
  when the window opened.
- Script memory is Automatic (stored as 0) or Custom, 4 to 256 MB.
- A choice out of range is clamped, except the blocking update
  interval, which becomes daily.
- A font family must be installed; it is matched without regard to
  case and stored with the font set's spelling.
- The user agent equal to Aura's default is stored as empty.
- More lists keeps only https addresses of 12 to 1023 bytes; Always
  allow and Always block keep cleaned domains, without repeats.

To reset a pane to the defaults, call `au_prefs_edit_reset_section`.

## Not done

See [`known-gaps.md`](known-gaps.md).
