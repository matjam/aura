# Developer tools

This document covers the Develop menu, the six developer windows (Page
Source, Console, Network, Performance, System Status, Report a
Problem), the timing test and the paint benchmark, and the host
commands that print the same text. Read it before changing a developer
window or a report, or when you need to see what a page requested,
logged or cost.

The windows show the active tab of the frontmost browser window and
follow it. Everything about a window except drawing and the Toolbox is
portable code in `src/browser`: its text, when it refreshes, its parts
and their rectangles, its buttons and toggles and what they do
(`devwin_model.c`, "The window models" below). The Mac OS 9 windows are
`src/platform/macos9/devwin.c`. To change a window's text or behaviour,
change it in `src/browser` and check it on the host first.

## To see the same text on the host

Run `aura-host chrome` with these `--do` actions after the page loads
(`src/platform/host/devtools_host.c`); add `--net` for network pages.

| Action | Prints |
|---|---|
| `netlog`, `netlog:details` | the request table and summary; with details and headers |
| `netlog:problems`, `netlog:blocked`, `netlog:sort=COLUMN`, `netlog:filter=TEXT` | the rows as the Network window shows them, 100 cells wide |
| `perf` | the Performance report |
| `status` | the System Status report |
| `bugreport`, `bugreport:tabs` | the bug report; with the addresses of the other tabs |
| `devconsole`, `devconsole:errors`, `:warnings`, `:logs`, `:filter=TEXT` | the Console window's lines |
| `console` | the console lines |
| `eval:EXPR` | the console's input line with EXPR |
| `source`, `source:FIRST,COUNT` | the page source (lines with their colour runs) |
| `devshot:KIND:FILE.png` | the text view of `source`, `console`, `network`, `perf`, `status` or `report`, 624 by 380 pixels; `devshot:KIND:WxH:FILE.png` for another size |

For example:

```bash
build/host/src/platform/host/aura-host chrome --net --bundled-only \
    --tab tests/browser/devtest.html --do wait:1500 --do bugreport \
    --do devshot:perf:run/perf.png
```

- `tests/browser/devtest.html` writes to the console at every level,
  throws an error, leaves a promise rejection unhandled, asks for an
  image that does not exist and for scripts on three hosts that
  blocklists name. To use it in the guest, push it beside the
  application and open `AuraTest:devtest.html`.
- To see blocked requests on the host, give `--blocklist FILE` a text
  file with `static.ads-twitter.com` and `cdn.taboola.com`.
- The host tool fills its `AuSysInfo` with next to nothing, which shows
  how the reports read where a fact is unknown.
- The timing test and the paint benchmark exist only in the Mac shell.
- `aura-host chrome` paints the window into a scratch surface after
  each idle call that invalidated anything (`chrome_present` in
  `src/platform/host/main.c`), as a shell does. Painting is what asks
  for background images, masks and images in SVG, so a screenshot taken
  after `--do wait:MS` shows them.

## To compare a page with Chrome

To compare a page with Chrome, run `tools/render-compare.sh URL`. It
renders the page with `aura-host chrome --native-chrome --net` and with
headless Chrome (`google-chrome-stable --headless=new`, a fresh profile
in a temporary directory) at a narrow viewport (400x900) and a wide one
(1280x900), and writes these files to `run/compare` for each width W:

| File | Holds |
|---|---|
| `aura-W.png`, `chrome-W.png` | the two renders |
| `side-W.png` | Aura on the left, Chrome on the right, 8 grey pixels apart |
| `diff-W.png` | the differences, amplified four times |
| `aura-W.log`, `chrome-W.log` | what each browser printed |

It prints the `tools/png-compare.py` figures for each width: the mean
and maximum channel difference and the share of pixels that differ by
more than 8 levels. Check both widths: a fix that holds at one often
fails at the other.

- To render one size, give it: `tools/render-compare.sh URL 933 1300`.
  A fourth argument names another output directory.
- `AURA_HOST`, `CHROME`, `AURA_WAIT` (milliseconds Aura waits after the
  load, 4000 by default) and `TIMEOUT` (seconds for each render, 120)
  change the defaults.
- Headless Chrome screenshots when the page has loaded and Aura after
  its wait, so content a script adds late can differ between them.
- `tools/png-compare.py --side LEFT.png RIGHT.png OUT.png [DIFF.png]`
  makes the side-by-side image and figures for any two PNGs.

## The Develop menu

The menu is `MENU` 135 in `src/platform/macos9/aura.r`; `main.c`
handles it (`kDevelopMenu`, the `DEV_*` items) and enables its items in
`mac_adjust_menus`.

| Item | Key | Opens | Needs |
|---|---|---|---|
| Page Source | Command-U (on View > Page Source) | the Page Source window | a page with source |
| Console | Command-K | the Console window | a tab |
| Network | Command-E | the Network window | a tab |
| Performance | | the Performance window | a tab |
| Run Timing Test | | the timing test, then Performance | a tab |
| System Status | | the System Status window | nothing |
| Tests > Test Scene | | `about:scene` in the front tab | nothing |
| Tests > Font List | | `about:fonts` in the front tab | nothing |
| Tests > Paint Benchmark | | the benchmark, then System Status | a browser window |
| Report a Problem... | | the Report a Problem window | nothing |

- The groups, separated by lines: looking at the page, measuring it,
  the engine and the machine, test content, reporting.
- Report a Problem is also the last item of the system's Help menu.
  `main.c` appends it at start with `HMGetHelpMenuHandle`; a choice
  there arrives with menu ID -16490 (`kHelpMenu`).
- Tests is a submenu, `MENU` 190. Rez does not attach it: `main.c`
  inserts it with `InsertMenu(menu, -1)` and sets the parent item's
  command character to 0x1B and its mark to 190. Submenu IDs must stay
  below 256, and 200 to 255 belong to the bookmark folders.
- The shortcuts use Command alone, because MenuKey ignores Shift and
  Option. Command-K and Command-E were free.
- The contextual menu's View Source opens the Page Source window on
  Mac OS 9 (`browserwin.c` takes `AU_CMD_VIEW_SOURCE` before the
  model). The model's own command opens a `view-source:` tab, for a
  shell without the window, and a typed `view-source:` address works.
- Test Scene shows `about:scene`, the scene the host renders with
  `aura-host scene`. Font List shows `about:fonts`, the fonts Aura
  found, with the bundled ones marked.
- [`guest-testing.md`](guest-testing.md) gives the menu's coordinates
  in the guest and says how to open the Tests submenu there.

Earlier builds had three items that showed their text in alerts
(`ALRT` 129 and 131), which are removed:

| Removed item | Now |
|---|---|
| Network Status | the System Status window |
| Page Timings | Run Timing Test, whose table is a section of the Performance report |
| Benchmark | Tests > Paint Benchmark, whose table is a section of System Status |

## The six windows

To change what a window shows or does, change its model in
`src/browser/devwin_model.c` and `tests/browser/test_devwin.c`; to
change how the Mac OS 9 window draws it, `src/platform/macos9/devwin.c`.
All six are laid out alike:

- **A tool band at the top** where the window has a filter or options:
  a text filter and check boxes (Console), a text filter and radio
  buttons (Network), two lines of instructions and a check box (Report
  a Problem).
- **The text**, drawn by the text view in Monaco 10 with a vertical
  scroll bar. Drag to select; Edit > Copy copies the selection; Edit >
  Select All selects the text; Edit > Find, Find Again and Find
  Previous search it.
- **A bar of push buttons at the bottom:** Copy All first, Find second,
  then the window's own button, then one line of status text. Report a
  Problem has Copy Report, Save Report and Open GitHub Issues instead.
- **Place and size.** The window opens where it was left and at the
  size it had. The `develop.windows` preference holds `name=x,y,w,h`
  per window (`au_dev_window_get`, `au_dev_window_set` in
  `devtools.c`); a place whose title bar would be off the screen is
  ignored. The default size fits a 640x480 screen. The windows are not
  part of the session: they are not reopened at the next launch.
- **Background.** On Mac OS 9 the window's background colour is the
  grey of its bands (`band_back` in `devwin.c`), because check boxes and
  radio buttons erase to the port's background colour.

| Window | Text from | Updates | Own button |
|---|---|---|---|
| Page Source | `sourceview.c` | when the page's source changes | |
| Console | `au_dev_console_lines` | when the page writes a line | Clear |
| Network | `au_netview_build`, `au_dev_network_lines` | at most four times a second while the log changes | |
| Performance | `au_dev_perf_report` | every second | Run Timing Test |
| System Status | `au_sysinfo_report` | every two seconds | Refresh |
| Report a Problem | `au_bugreport_build` | when it is shown and when its check box changes | |

Performance and System Status do not rebuild while text in them is
selected, so that a selection can be copied.

### The window models

`src/browser/devwin_model.h` holds one model (`AuDevWin`) per open
window. A port shows it with its own widgets and holds no
developer-window logic of its own.

- **To make a window**, call `au_devwin_init` with the kind
  (`AU_DEVWIN_SOURCE`, `_CONSOLE`, `_NETWORK`, `_PERF`, `_STATUS` or
  `_REPORT`), the browser, the fonts, the width a scroll bar takes at
  the right of each text pane, and an `AuDevWinHost`. Then call
  `au_devwin_resize` with the window's content size and
  `au_devwin_refresh(dw, true)`.
- **What a kind has.** `au_devwin_kind` gives its preference name,
  title, default height, which parts it has (tool band, filter field,
  column titles, detail pane, input line), the text view's settings and
  Report a Problem's two lines of instructions. `au_devwin_buttons` and
  `au_devwin_toggles` give the push buttons and the check boxes or radio
  buttons, each with its label, its Mac OS 9 width and what it does;
  `AuDevWin.toggle_on` holds their state.
- **Layout.** `au_devwin_areas` gives the rectangle of each part for the
  model's size; `au_devwin_button_rect` and `au_devwin_toggle_rect`
  place the controls. A port that lays the window out natively uses the
  kind's parts and ignores the rectangles.
- **To keep it current**, call `au_devwin_refresh(dw, false)` from the
  event loop and redraw when it returns true. `au_devwin_stale` states
  the rules: Page Source when the page or its source changed (a page
  still loading keeps the old text), Console when the page wrote a
  line, Network when the log changed and 250 ms passed, Performance
  every second and System Status every two seconds while no text is
  selected, Report a Problem only when shown or when its check box
  changes. Another page always counts, except for System Status and
  Report a Problem.
- **What to draw.** `AuDevWin.tv` is the main text and `dv` the Network
  detail pane (paint them with `au_textview_paint`); `title`, `footer`
  (the status line), `header` (the Network column titles), `filter`,
  `input` and `filter_focus` (which field has the keyboard).
- **Input.** Pass keys to `au_devwin_key` (`AU_KEY_*`, or the UTF-8 of a
  typed character), clicks to `au_devwin_mouse_down` with the click
  count, then `au_devwin_mouse_drag` and `au_devwin_mouse_up` for a text
  pane; a scroll bar calls `au_devwin_scroll` or `au_devwin_scroll_to`.
  Buttons call `au_devwin_run`, toggles `au_devwin_toggle`, Edit > Copy
  `au_devwin_copy_text`, Paste `au_devwin_paste` and Select All
  `au_devwin_select_all`; Find and Find Again call `au_devwin_ask_find`
  and `au_devwin_find_next`. `AU_DEVWIN_FAILED` means alert the user
  (the Mac beeps).
- **The Console input line.** Return runs it (`au_devwin_console_run`:
  it goes into the history of 16 lines, is cleared, runs with the busy
  cursor, and the view scrolls to the end); Up and Down recall
  (`au_devwin_console_recall`). The filter field holds 60 bytes and the
  input line 4000.
- **The Network selection.** `au_devwin_network_selected` gives the
  selected request; a new selection fills the detail pane and puts the
  whole address in the status line, and a new order keeps it on the same
  request.
- **Copy All and Save.** `au_devwin_all_text` is what Copy All copies;
  `au_devwin_save_text` is the report with CR LF line ends, as Save
  Report writes it.
- **The port's callbacks** (`AuDevWinHost`): the page shown, the front
  browser window, the clock, the machine's facts (`host_info`), the
  open connections, the shell's own System Status sections, the last
  timing test and paint benchmark, the clipboard, saving a file,
  opening a page in a new tab, asking for the text to find, running the
  timing test, the busy cursor, the title, and turning on header
  recording when a Network window opens.

### The text view

To change how the windows draw text, edit `src/browser/textview.c`: a
fixed-width text view (Monaco, else the monospace family, 10 pixels)
with optional line numbers, wrapping, colour runs, a character
selection or a selected row, find, and scrolling by rows. It owns no
text: it asks an `AuTextSource` for one line at a time, reads each
line's cell count once per layout and paints only the rows on screen.
`AuTextList` is a ready-made source for text made on the spot.

### Report layout

To add a section or a line to a report, follow the layout that the
comment at the top of `devtools.h` states:

- A section is a title at the left margin, then its lines indented by
  two spaces, then a blank line. `au_dev_report_lines` draws a title as
  a heading (a line that is not indented and is followed by an indented
  one). Write a title with `au_report_heading`.
- A line is a label padded to 22 cells (`AU_REPORT_LABEL`), then its
  value: write it with `au_report_kv`.
- Tables put numbers right-aligned under a line of column titles.
- Counts carry thousands separators (`au_put_count`). Sizes are B, KB
  or MB (`au_put_size`). Times are ms or s (`au_put_ms`). Clock times
  are `YYYY-MM-DD HH:MM:SS`.
- A bar is `au_put_bar`: `[#####...............]`, 20 cells.
- No line ends in a space. `test_perf` in
  `tests/browser/test_devtools.c` checks that and that the bars of a
  table start in one column.

These helpers are declared in `netlog.h` beside `au_put_size`.

## Page Source

Page Source is `src/browser/sourceview.c`: one pass over the page's
bytes (`au_page_source`) records each line's start and the colouring
tokenizer's state there. Lines longer than 2048 bytes are cut into
segments, so a minified page stays cheap. The tokenizer colours tags,
attribute names, values, comments and `<!DOCTYPE`, and leaves
`<script>` and `<style>` text plain; it is not the HTML parser.

The window is a native text view, not a page: on the host, laying out a
1.3 MB document as plain text in the engine took 29 ms and would cost
far more with colour spans, where the text view lays the same document
out in under 1 ms and paints a screen of it in 1 ms.

## Console

Console shows the page's console lines (`au_page_console_line`) through
`au_dev_console_lines` in `src/browser/devtools.c`. Uncaught errors and
unhandled rejections arrive as error lines with their stack; the first
stack frame gives the source URL and line. The input line calls
`au_dev_console_eval`, which runs the expression with `au_page_eval`
(starting the script runtime on a page that has no script) and adds the
expression and its result to the console.

- The filter field and the Errors, Warnings and Logs check boxes fill
  an `AuConsoleFilter`. Logs covers log, info and debug. The
  expressions typed and their results are always listed.
- The console keeps lines across loads. Where it holds lines of earlier
  pages, a heading line "Page: URL" marks where the lines of the page
  now shown begin. The page counts them (`au_page_console_current` in
  `page_doc.c`: lines added since the last navigation began).
- Tab moves the keyboard between the filter field and the input line; a
  click in either takes it there.
- A script that runs out of memory is reported here
  ([`javascript.md`](javascript.md), "What an out-of-memory line
  says").

## Network

Network is the page's request log, `src/browser/netlog.c`: a ring of
300 records, cleared by a navigation and freed with the page. A
document kept for Back and Forward keeps its log, which returns with
it.

**What is recorded**

- `page_doc.c` fills the log from its fetches (start, response head,
  data, end).
- The times, the connection, the remote address, TLS version and cipher
  and the source (network, memory cache, disk cache, history, 304) come
  from the HTTP layer: every transfer stamps the start of each phase
  (`au_http_trace` in `net.h`), the loader passes the trace in
  `AuLoadInfo`, and `page_fetcher.c` turns it into `AuFetchInfo.net`
  (`AuFetchNet` in `page.h`). The cheap fields are always recorded.
- Request and response headers are copied (4 KB each at most) only
  after `au_net_set_trace_headers(net, true)`, which the Network window
  calls when it first opens, so headers appear for loads made after
  that.
- A request refused by policy is logged as blocked with
  `blocked_reason`: a blocklist's refusal arrives in
  `AuLoadInfo.blocked_reason` ("blocked: LIST NAME") and mixed content
  as `AuLoadInfo.mixed`; `page_fetcher.c` passes either on as
  `AuFetchNet.blocked_reason`.

**What the window shows**

- The columns are #, Status, Method, Type, Size, Time, From and URL
  (`g_cols` in `netlog.c`). Status is the HTTP status or a word:
  blocked, failed, stopped, pending, ok. The transferred size, the
  connection and the remote address are in the details only, which
  leaves the URL about 40 cells in a 616-pixel window.
- A URL longer than its cells loses its middle: the first two thirds
  and the last third stay, with "..." between (`au_netlog_url_elide`).
  The whole URL is in the details pane and, for the selected row, in
  the status line.
- A blocked request shows no size, time or source: it was never made.
- The view (`AuNetView`) holds the filter text, which rows are shown
  (All, Problems, Blocked) and the sort. A problem is a request that
  failed, was blocked, or got status 400 or more. The filter matches
  the address, the type, the status and the reason a request was
  blocked, ignoring ASCII case. Typing anywhere in the window goes to
  the filter field.
- A click on a column title sorts by it (`au_netview_click_column`):
  sizes and times largest first, the others rising; a second click
  reverses the order. The title shows "^" or "v". The selected request
  stays selected through a new order.
- The lower pane shows the selected request's details. With nothing
  selected it shows the summary (`au_netlog_summary`): the totals, then
  one line per request that failed, was blocked or was redirected. The
  bug report prints the same summary.
- Copy All copies every request with its details and headers
  (`au_netlog_dump`), not only the rows shown.

## Performance

Performance is `au_dev_perf_report` in `devtools.c`, rebuilt once a
second. The report starts with a summary: load time, requests, how busy
the last second was, layout boxes, free memory. Then:

| Section | Holds |
|---|---|
| Time | one row per kind of work (Script, Style, Layout, Paint, Screen copy) with milliseconds and runs in the last interval and since the page began, and a bar for its share of the time |
| Work done | pixels painted, style passes, box tree builds, layouts, image decodes |
| Memory | one row per store (application heap, script heap, decoded images, HTTP memory cache, glyph cache, kept documents) with what it holds, its limit, and a bar; free temporary memory, a "taken by Aura" line when the heap has taken temporary memory, and a "Long-lived blocks" line with the allocator's long-lived zone and the temporary memory that holds it ([`memory.md`](memory.md), "Zones"), and an "Areas held" line with the temporary memory zones Aura keeps from emptying ([`memory.md`](memory.md), "Temporary memory areas") |
| Script runtimes | one line per script runtime, document and worker ([`memory.md`](memory.md), "To see what a script heap holds") |
| Network | HTTP cache lookups and writes, DNS, the open connections |
| Slowest tasks | tasks of 50 ms or more, scripts with their URL |
| Timing test | the table of Run Timing Test, when one ran for this page |
| What the numbers mean | a legend |

- The time counters are `AuPerf` (`src/page/perf.h`), one per document
  view, cleared when a document begins: the view times its style and
  layout passes, its paints and its outermost script entries with the
  embedder's millisecond clock, and the Mac shell adds each screen copy
  (`flush` in `browserwin.c`).
- The shell supplies free heap and temporary memory (`AuDevHostInfo`),
  and `au_net_dev_info` (`src/net/net_report.c`) the HTTP cache and the
  open connections.
- To take measurements from a real machine, click Copy All and paste
  the text.

### Run Timing Test

To time scrolling, repainting and typing on the active page, choose
Develop > Run Timing Test or click the Performance window's button.
`browserwin_timing_test` fills an `AuTimingTest`, and
`au_dev_timing_report` prints it as the "Timing test" section of the
Performance report.

| Row | Steps |
|---|---|
| Typing a key | 16 keys in the focused text field, with the restyle and layout counts ([`forms.md`](forms.md)) |
| Scroll 32 px, new | ten 32-pixel steps over content not painted before |
| Scroll 32 px, again | the same steps again: the steady state |
| Page down | five page downs |
| Whole page repaint | three repaints of the whole page |

- The columns are milliseconds per step: total, moving the offscreen
  pixels, painting, copying to the screen, and the number of areas
  painted. A line under the table gives the glyph cache's hits, misses
  and evictions in these steps.
- To time typing, click a text field of the page first; without one the
  report says typing was not timed.
- A page that does not scroll gets only the whole page repaint.
- For a page shown from the kept documents, the summary's "Page load"
  line says so and gives the time hiding the last page took, with or
  without the test ([`browser-chrome.md`](browser-chrome.md)).

## System Status

To see the state of the machine and of Aura as a whole, choose Develop
> System Status. `au_sysinfo_report` (`src/browser/sysinfo.c`) prints:

| Section | Holds |
|---|---|
| Summary | Aura's version and build date, one line about the machine, free memory, the clock |
| Machine | the model, the processor, memory installed, virtual memory, the system, CarbonLib, the Appearance Manager, Open Transport, the screen, Enforcer |
| Clock | UTC as Aura computes it, UTC in script, the Mac's clock, the zone and daylight saving ([`mac-os-9-shell.md`](mac-os-9-shell.md), "The clock") |
| Aura | version, user agent, the memory partition, free heap, the largest free block and free temporary memory, windows and tabs, kept documents, each page's script heap, the caches |
| Fonts | the faces found and loaded and the font data held; the Mac shell adds how long font registration took |
| Network (Mac shell, `mac_net_status_text` in `network.c`) | entropy, Open Transport counters, the stores (roots, cookies), and the HTTP cache: lookups (fresh, history, revalidated, 304, missed), the time spent reading and writing cache files, the bytes held in memory and on disk |
| Content blocking (Mac shell, `mac_blocking_status_text` in `blocking_mac.c`) | the blocklist totals, where the list memory came from and what the last compile cost |
| Paint benchmark | the table of Tests > Paint Benchmark, when one ran |

The machine's facts are an `AuSysInfo` (`sysinfo.h`). To change what is
read on Mac OS 9, edit `src/platform/macos9/sysinfo_mac.c`: every
Gestalt selector is in that file, and a selector that fails leaves its
field empty, which the reports print as "unknown".

- The model name comes from Gestalt `'mnam'`, a Pascal string. New
  World machines all have machine type 406, so the name is what tells
  them apart. The QEMU guest answers "Power Mac G4".
- Enforcer is detected with Gestalt `'Enfr'`, which its extension
  installs: a value of 0x1000 or more is its state record (running), a
  smaller one a status code.
- The memory partition is the Process Manager's `processSize`. Under
  Enforcer it is larger than the SIZE resource asks for and changes
  while Aura runs.

### Paint Benchmark

To time painting without a page, choose Develop > Tests > Paint
Benchmark. It needs a browser window. It fills an `AuPaintBench`, and
`au_dev_bench_report` prints the "Paint benchmark" section of System
Status, in time per run: the test scene with cached glyphs, the test
scene with the glyph cache emptied, one half-transparent fill of the
area, and the toolbar and tabs.

### The build's identity

`tools/buildinfo.cmake` writes `build/*/gen/au_build.h` on every build
(the `aura_buildinfo` target in `CMakeLists.txt`): the short commit and
that commit's date. Only `src/browser/sysinfo.c` includes it, and the
file is rewritten only when its text changes, so a new commit compiles
one file again.

- `au_build_identity()` gives "0.3.0 (1a2b3c4)".
- A "+" after the commit marks uncommitted changes to tracked files.
- The date is the commit's, so one commit builds the same bytes at any
  time.
- Without git both are empty and Aura shows the version alone.

## Report a Problem

To collect a bug report, choose Help > Report a Problem or Develop >
Report a Problem with the page in question in the front window
(`CONTRIBUTING.md` describes it for users). To change the report, edit
`src/browser/bugreport.c` (`bugreport.h` states the rules). The shell
fills an `AuBugReport` and shows the text; nothing is sent.

**Sections**, in order, each under a Markdown heading:

1. What I did, What I expected, What happened: for the person to fill
   in.
2. Aura: version and commit, the commit's date.
3. Machine (`au_sysinfo_machine`).
4. Aura's state (`au_sysinfo_state`).
5. Page: address, title, whether it finished loading, user agent,
   security, TLS version and cipher, legacy key exchange, certificate
   exception, mixed content, blocked requests.
6. Console: the page's errors and warnings, the last 50 lines.
7. Network: the summary, then the request table inside `<details>` (the
   last 200 requests).
8. Performance: the Performance report inside `<details>`.
9. Settings that differ from the defaults.
10. Clock.

Then a footer that says what was left out.

**Privacy rules**

- Never included: cookies, request and response headers, request
  bodies, form data, history, bookmarks, the home page, the search
  template, the download folder, per-site lists, proxy addresses, and
  the host names of open connections.
- In addresses (`au_bugreport_redact_url`): a user name and password
  are removed; a query string or fragment longer than 100 characters is
  cut there with "..."; a `file:` address keeps only its file name.
  `au_bugreport_redact_text` does the same to addresses inside console
  lines and the Performance report.
- Settings are an allow-list (`settings_section`). Proxies print as set
  or not set; the personal block and allow lists print as counts. To
  show another preference, add its key to the table.
- The addresses of other tabs are included only with
  `AuBugReport.other_tabs` (the window's check box, off by default).

**Format rules**

- The text is ASCII. Common punctuation becomes its ASCII form and
  other characters `\uXXXX` (`au_bugreport_ascii`), so the text
  survives the Mac clipboard and a file moved to another machine.
- Everything that comes from a page is inside a fenced code block,
  where GitHub renders no markup and notifies nobody. "```" in page
  text becomes "'''".
- A blank line follows each `<summary>` line. Without it GitHub shows
  the code block as plain text.
- The report is sized to fit a GitHub issue (65,536 characters): at
  most 200 table rows with URLs cut to 120 cells, 50 console lines of
  200 characters, a title of 200 and an address of 500. A page that
  fills every one of those limits gives about 55,000 characters.

**On Mac OS 9**

- Copy Report uses `mac_put_scrap` (TEXT in Mac Roman with carriage
  returns, and `utxt`).
- Save Report asks with `StandardPutFile` (`mac_save_text` in
  `storage.c`) and writes a TEXT file, creator AURA, with CR LF line
  ends, which editors on every system read. SimpleText shows the line
  feeds as boxes and cannot open a file over 32 KB.
- Open GitHub Issues opens `github.com/matjam/aura/issues/new` in a new
  tab of the front browser window.
- Choosing the menu item again builds the report again.

## Tests

- `tests/browser/test_devtools.c` runs the request log on the fake
  transport (sources, connections, timings, headers, blocked requests,
  the ring), the view (filter, problems, blocked, sorting), URL
  eliding, the header's columns, the counters and the Performance
  report's layout, the console, its filters and its input line, the
  window places, the timing and benchmark tables, the Markdown lines,
  and the source view and text view (lines, colouring, wrapping,
  selection, find).
- `tests/browser/test_bugreport.c` (ctest `browser_bugreport`): the
  report's sections in order, balanced code blocks and `<details>`,
  ASCII only, the machine and clock lines, and what must be absent: an
  Authorization header, a cookie, a password in an address, a query
  past 100 characters, another tab's address, the home page, the
  download folder, a proxy address, the search template, a site list
  and the history. It also checks the System Status report.

## What the redesign of 2026-11-15 changed

An audit in the 640x480 guest found these faults; the menu and windows
described above replace them. Keep them in mind when adding an item.

- The Develop menu mixed windows and one-off debug commands without
  groups, and named two things alike (Network, Network Status).
- Network Status, Page Timings and Benchmark put their text in alerts:
  bold proportional type, run-on sentences, 255 characters per
  paragraph, no copy, no scroll bar, and the application blocked while
  one was up. Page Timings printed "4.2 = 0.2+2.7+0.5 (2)" without
  saying which number was which.
- Units were mixed: "209K of 8192K", "1832990 bytes", "3602 thousand".
- The Network window showed about 17 characters of each URL; the
  Transfer and Conn columns took the room. Status words were cut
  ("block", "stop", "fail"). The blocked requests showed only while no
  row was selected. The status line was cut before DOMContentLoaded.
- The Performance report began with one long line that wrapped in the
  middle of a word, and its memory lines wrapped too. It rebuilt every
  second, which dropped a selection before it could be copied.
- The Console kept the earlier page's lines with nothing to tell them
  from the current page's.
- Benchmark and Page Timings did nothing, without a sign, when no
  browser window was open; the items were never disabled.
- The windows opened at fixed places that covered most of a 640x480
  screen, and forgot where they were moved to.
- The machine line was built in `devwin.c` and the clock line in
  `network.c`; nothing else could use them.
- The contextual menu's View Source opened a `view-source:` tab while
  View > Page Source opened the window.
- The Help menu had no item, and there was no way to collect a report
  in one step.
- The Cookies and Downloads windows are user features and were left as
  they are. `about:fonts`, `about:scene` and `about:licences` are pages
  and stay pages; the first two moved under Develop > Tests.

## Not done

See [`known-gaps.md`](known-gaps.md#developer-tools).
