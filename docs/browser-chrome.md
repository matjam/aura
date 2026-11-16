# Browser chrome

This document covers the browser model in `src/browser`: windows, tabs,
back and forward, the documents kept for Back, site icons, keyboard
focus, scrolling by moving pixels, selection and find, contextual
menus, Internet Config, downloads and the files Aura keeps. Read it
before changing how a browser window looks or behaves. The Mac OS 9
side of each feature is named here; Toolbox lessons are in
[`mac-os-9-shell.md`](mac-os-9-shell.md).

## Where things are

| File | Holds |
|---|---|
| `chrome.c` | lays out, paints and hit-tests the tab strip (or left tab column), toolbar and status bar |
| `browser.c` | windows, tabs and back/forward lists; routes input |
| `page.h` | the interface a page implements |
| `page_doc.c` | implements it on the document view (`src/page`) |
| `context.c` | contextual menus |
| `favicon.c` | site icons |
| `present.c` | the shells' damage list |
| `credits.c` | the About box and about:licences table |

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

## Font preferences and text zoom

The font preferences reach the engine as the generic families of the
font set and, for the default and monospace sizes, the minimum size and
the tab's text zoom, `au_docview_set_font_prefs` (which passes them to
`au_css_resolver_set_font_prefs`).

Text zoom is per tab (`au_bw_zoom`, View > Zoom In, Zoom Out, Actual
Size); the last zoom chosen for a site (host, or scheme for `file:`) is
kept in the `zoom.sites` preference and applied when a tab opens that
site again.

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

## Back, Forward and closed tabs

- A Command-click on Back or Forward opens that entry in a new tab
  behind (`au_bw_go_new_tab`).
- Holding the button for half a second (30 ticks) shows a pop-up of the
  entries that way (`au_bw_history_items`, `history_popup` in
  `browserwin.c`).
- File > Reopen Closed Tab (Shift-Command-T, mapped in `key_down`
  because MenuKey ignores Shift) brings back the last of up to
  `AU_MAX_CLOSED_TABS` closed tabs with their history and scroll
  position.
- Each window's position goes into the session (`au_bw_set_position`
  after a drag or zoom) and a restored window returns there when its
  title bar is still on screen.

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

## The horizontal scroll bar

A horizontal scroll bar appears along the bottom of the page area while
the active page is wider than it (`au_chrome_layout_hscroll`); the two
bars meet at a blank corner square above the grow box, and Shift turns
a vertical wheel scroll horizontal (Mac OS 9 itself reports no wheel).
[`layout.md`](layout.md) says how its state is kept across a resize.

## Save Page As

To change Save Page As, see `au_page_source` (the page keeps the main
document's bytes as they arrived, until the next load),
`au_bw_save_name` (the suggested file name) and `mac_save_page` in
`storage.c`, which asks with `StandardPutFile` and writes the bytes
unchanged as a TEXT file with creator AURA.

## The About box and about:licences

To change the About box or about:licences, edit the credits table in
`src/browser/credits.c` (the version, the tagline, and one entry per
bundled font, third-party library and data set with its licence);
`aboutbox.c` and `build_licences` in `page_doc.c` both read it. Data
under the MPL (the root store, the Public Suffix List) carries the MPL
notice and where its source form is. QuickJS-ng and Mbed TLS have their
entries there.

The About box (DLOG 202) draws icon family 128 at 64 pixels with
`PlotIconID`; its empty `dctb` gives it a colour port, without which
the icon draws in black and white.

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

## Contextual menus

To change what a contextual menu offers, edit `src/browser/context.c`:
`au_bw_context_menu` builds the items for a window point (link, image,
page, address field or tab, using `au_page_hit_info` for the page) and
`au_bw_context_command` runs the commands the model can; the shell does
the rest (clipboard, field editing, a new window).

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
selection, wrapping.

The Mac OS 9 shell counts clicks within the double-click time, copies
with `mac_put_scrap` (TEXT in Mac Roman with transliteration and
carriage returns, plus 'utxt'), asks for the text with the Find dialog
(DLOG 201), maps Shift-Command-G to Find Previous itself (MenuKey
ignores Shift), and paints selections in the Appearance highlight
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
whose offset changed, and their shadows and outlines. It returns -1
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
so a strip paints only what it shows; box and text ink extents
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
pixel; `SHIFT_DUMP=1` writes both images when they differ. To time
scrolling on the host, run `aura-host chrome -w 1440 -h 1050 --tab PAGE
--scroll-bench 20` (add `--bundled-only` for reproducible fonts): it
times 32-pixel steps and page downs by repainting, then by moving
pixels (`--no-shift` skips that), through the Mac shell's paint path,
with pixels painted and copied, glyph cache counts and image decodes.

## Files Aura keeps

Aura keeps its files in the Aura folder of the system Preferences
folder:

| File | Holds | Written by |
|---|---|---|
| `Aura Preferences` | preferences | `storage.c` |
| `Bookmarks.html` | bookmarks (Netscape format) | `storage.c` |
| `History` | history | `storage.c` |
| `Session` | windows, tabs, back and forward lists, scroll positions | `storage.c` |
| `Downloads` | the download list | `storage.c` |
| `Favicons` | site icons | `storage.c` |
| `Running` | exists while Aura runs | `storage.c` |
| `Cookies`, `HSTS` | cookies and HSTS hosts | `network.c` ([`networking.md`](networking.md)) |
| `Cache` (folder) | the disk cache | `src/net/disk_cache.c` |
| `Intermediates` | the intermediate certificate pool | [`tls.md`](tls.md) |
| `Random Seed` | entropy carried between runs | [`mac-os-9-shell.md`](mac-os-9-shell.md) |
| `Blocklists` (folder) | compiled blocklists | [`content-blocking.md`](content-blocking.md) |
| `NetTest Log` | NetTest's output | [`networking.md`](networking.md) |

- **Safe writes.** Each is written to a `.tmp` file and swapped in; a
  file that fails to parse is renamed with ` (damaged)` before a new
  one is written.
- **The session** is saved 3 seconds after tabs change and every
  30 seconds while only scroll positions do (`session_gen`,
  `scroll_gen`).
- **Crash recovery.** `Running` is made at start and deleted after the
  last save at quit; when Aura finds it at start, the last run crashed
  or ran out of memory, and Aura offers to restore the session
  (ALRT 133) unless the startup preference restores it anyway. To test
  this in the guest, press Command-Option-M (the out-of-memory test)
  and launch Aura again.
- **To start with clean state in the guest**, delete that folder.

## Downloads

Downloads go through `src/net/transfer.c` (`au_net_transfer_source_*`)
over the platform's `AuDownloadFiles`; on Mac OS 9 that is
`downloads_mac.c`: a partial file ("<id> <name>.part") in the download
folder (the preference, else Internet Config's, else the desktop),
renamed and given Internet Config's type and creator when complete,
resumed with Range. A page's response that turns out to be a download
(attachment or a type Aura cannot show) continues on its own transfer.
`au_sim_source` remains for tests.

## Cookies window

The Cookies window (Window > Cookies, or Preferences > Privacy) lists
`src/net/cookie_view.c`'s model; per-site rules (allow, session only,
block) live in the `cookies.sites` preference.

## Not done

See [`known-gaps.md`](known-gaps.md).
