# Developer tools: Report a Problem, System Status and the Develop menu

These notes replace the "Developer tools" section of `CLAUDE.md` and
add to "Guest testing", "Browser chrome" and "Networking" where those
name Network Status, Page Timings or Benchmark. They were written on
the `devmenu` branch on 2026-11-15.

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
- To open the submenu in the guest, click the Develop title, then
  `guest-qmp.py move 300 142` (the Tests item), `move 420 142`, and
  click the item. A click on the Tests item itself closes the menu.
- On a 640x480 screen the Develop title is at 298,10 and the items are
  at y 28 (Page Source), 44, 60, 82 (Performance), 98, 120 (System
  Status), 142 (Tests) and 164 (Report a Problem).
- Command-K and Command-E were free. MenuKey ignores Shift and Option,
  so the shortcuts use Command alone.
- The contextual menu's View Source opens the Page Source window on
  Mac OS 9 (`browserwin.c` takes `AU_CMD_VIEW_SOURCE` before the
  model). The model's own command still opens a `view-source:` tab,
  for a shell without the window, and a typed `view-source:` address
  still works.

What was removed: the Network Status, Page Timings and Benchmark
alerts (`ALRT` 129 and 131). Network Status is the System Status
window; Page Timings is Run Timing Test, whose table is a section of
the Performance report; Benchmark is Tests > Paint Benchmark, whose
table is a section of System Status.

## The six windows

To change a window, work in `src/platform/macos9/devwin.c`. The text of
every window comes from portable code in `src/browser`, so change the
text there and check it on the host first.

All six are laid out alike:

- A tool band at the top where the window has a filter or options:
  a text filter and check boxes (Console), a text filter and radio
  buttons (Network), two lines of instructions and a check box (Report
  a Problem).
- The text, drawn by the text view (`textview.c`) in Monaco 10 with a
  vertical scroll bar. Drag to select; Edit > Copy copies the
  selection; Edit > Select All selects the text; Edit > Find, Find
  Again and Find Previous search it.
- A bar of push buttons at the bottom: Copy All first, Find second,
  then the window's own button, then one line of status text. Report a
  Problem has Copy Report, Save Report and Open GitHub Issues instead.
- The window opens where it was left and at the size it had. The
  `develop.windows` preference holds `name=x,y,w,h` per window
  (`au_dev_window_get`, `au_dev_window_set` in `devtools.c`); a place
  whose title bar would be off the screen is ignored. The default size
  fits a 640x480 screen.
- The window's background colour is the grey of its bands
  (`band_back`), because check boxes and radio buttons erase to the
  port's background colour.

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

### Report layout

Every report follows the layout that the comment at the top of
`devtools.h` states:

- A section is a title at the left margin, then its lines indented by
  two spaces, then a blank line. `au_dev_report_lines` draws a title as
  a heading (a line that is not indented and is followed by an indented
  one).
- A line is a label padded to 22 cells (`AU_REPORT_LABEL`), then its
  value: write it with `au_report_kv`. Write a title with
  `au_report_heading`.
- Tables put numbers right-aligned under a line of column titles.
- Counts carry thousands separators (`au_put_count`). Sizes are B, KB
  or MB (`au_put_size`). Times are ms or s (`au_put_ms`). Clock times
  are `YYYY-MM-DD HH:MM:SS`.
- A bar is `au_put_bar`: `[#####...............]`, 20 cells.
- No line ends in a space. `test_perf` in `tests/browser/test_devtools.c`
  checks that and that the bars of a table start in one column.

These helpers are declared in `netlog.h` beside `au_put_size`.

### Console

- The filter field and the Errors, Warnings and Logs check boxes fill
  an `AuConsoleFilter`. Logs covers log, info and debug. The
  expressions typed and their results are always listed.
- The console keeps lines across loads, as before. Where it holds lines
  of earlier pages, a heading line "Page: URL" marks where the lines of
  the page now shown begin. The page counts them
  (`au_page_console_current` in `page_doc.c`: lines added since the
  last navigation began).
- Tab moves the keyboard between the filter field and the input line; a
  click in either takes it there.
- Not done: a time for each line. The page stores none.

### Network

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
  turns the order round. The title shows "^" or "v". The selected
  request stays selected through a new order.
- The lower pane shows the selected request's details. With nothing
  selected it shows the summary (`au_netlog_summary`): the totals, then
  one line per request that failed, was blocked or was redirected. The
  bug report prints the same summary.
- Copy All copies every request with its details and headers
  (`au_netlog_dump`), not only the rows shown.

### Performance

The report leads with what a person wants first: load time, requests,
how busy the last second was, layout boxes, free memory. Then:

- Time: one row per kind of work (Script, Style, Layout, Paint, Screen
  copy) with milliseconds and runs in the last interval and since the
  page began, and a bar for its share of the time.
- Work done: pixels painted, style passes, box tree builds, layouts,
  image decodes.
- Memory: one row per store with what it holds, its limit, and a bar.
- Network, the slowest recent tasks, the timing test when one ran for
  this page, and a legend that says what the numbers mean.

Run Timing Test (`browserwin_timing_test`) fills an `AuTimingTest`;
`au_dev_timing_report` prints it. The numbers are milliseconds per
step: total, moving pixels, painting, copying to the screen, and the
areas painted. To time typing, click a text field of the page first.

### System Status

`au_sysinfo_report` (`src/browser/sysinfo.c`) prints a summary, the
machine, the clock, Aura (version, user agent, memory, windows and
tabs, each page's script heap, the caches) and fonts. The Mac shell
adds the network sections with `mac_net_status_text` (`network.c`:
entropy, Open Transport, the stores, the HTTP cache) and
`mac_blocking_status_text` (`blocking_mac.c`), and the paint benchmark
when one ran.

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

To see the clock Aura uses, choose Develop > System Status and read the
Clock section: UTC as Aura computes it, UTC in script, the Mac's clock,
the zone and daylight saving.

## Report a Problem

To change the bug report, edit `src/browser/bugreport.c`
(`bugreport.h` states the rules). The shell fills an `AuBugReport` and
shows the text; nothing is sent.

Sections, in order, each under a Markdown heading:

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

Rules the builder keeps:

- The text is ASCII. Common punctuation becomes its ASCII form and
  other characters `\uXXXX` (`au_bugreport_ascii`), so the text
  survives the Mac clipboard and a file moved to another machine.
- Everything that comes from a page is inside a fenced code block,
  where GitHub renders no markup and notifies nobody. "```" in page
  text becomes "'''".
- A blank line follows each `<summary>` line. Without it GitHub shows
  the code block as plain text.
- Never included: cookies, request and response headers, request
  bodies, form data, history, bookmarks, the home page, the search
  template, the download folder, per-site lists, proxy addresses, and
  the host names of open connections.
- In addresses (`au_bugreport_redact_url`): a user name and password
  are removed; a query string or fragment longer than 100 characters is
  cut there with "..."; a `file:` address keeps only its file name.
  `au_bugreport_redact_text` does the same to addresses inside console
  lines and the Performance report.
- Settings are an allow-list (`settings_section`). Proxies print as
  set or not set; the personal block and allow lists print as counts.
  To show another preference, add its key to the table.
- The addresses of other tabs are included only with
  `AuBugReport.other_tabs` (the window's check box, off by default).
- The report is sized to fit a GitHub issue (65,536 characters): at
  most 200 table rows with URLs cut to 120 cells, 50 console lines of
  200 characters, a title of 200 and an address of 500. A page that
  fills every one of those limits gives about 55,000 characters.

On Mac OS 9:

- Copy Report uses `mac_put_scrap` (TEXT in Mac Roman with carriage
  returns, and `utxt`).
- Save Report asks with `StandardPutFile` (`mac_save_text` in
  `storage.c`) and writes a TEXT file, creator AURA, with CR LF line
  ends, which editors on every system read. SimpleText shows the line
  feeds as boxes and cannot open a file over 32 KB.
- Open GitHub Issues opens `github.com/matjam/aura/issues/new` in a new
  tab of the front browser window.
- Choosing the menu item again builds the report again.

## The build's identity

`tools/buildinfo.cmake` writes `build/*/gen/au_build.h` on every build
(the `aura_buildinfo` target in `CMakeLists.txt`): the short commit and
that commit's date. Only `src/browser/sysinfo.c` includes it, and the
file is rewritten only when its text changes, so a new commit compiles
one file again. `au_build_identity()` gives "0.3.0 (1a2b3c4)". A "+"
after the commit marks uncommitted changes to tracked files. The date
is the commit's, so one commit builds the same bytes at any time.
Without git both are empty and Aura shows the version alone.

## On the host

`aura-host chrome` prints the same text with `--do` actions
(`src/platform/host/devtools_host.c`):

| Action | Prints |
|---|---|
| `netlog`, `netlog:details` | the request table and summary; with details and headers |
| `netlog:problems`, `netlog:blocked`, `netlog:sort=COLUMN`, `netlog:filter=TEXT` | the rows as the window shows them, 100 cells wide |
| `perf` | the Performance report |
| `status` | the System Status report |
| `bugreport`, `bugreport:tabs` | the bug report |
| `devconsole`, `devconsole:errors`, `:warnings`, `:logs`, `:filter=TEXT` | the Console window's lines |
| `eval:EXPR`, `source`, `source:FIRST,COUNT` | as before |
| `devshot:KIND:FILE.png` | the text view of source, console, network, perf, status or report, 624 by 380 pixels; `devshot:KIND:WxH:FILE.png` for another size |

For example:

```bash
build/host/src/platform/host/aura-host chrome --net --bundled-only \
    --tab tests/browser/devtest.html --do wait:1500 --do bugreport \
    --do devshot:perf:run/perf.png
```

`tests/browser/devtest.html` writes to the console at every level,
throws an error, leaves a promise rejection unhandled, asks for an
image that does not exist and for scripts on three hosts that
blocklists name. Push it to the guest beside the application and open
`AuraTest:devtest.html`. On the host, give `--blocklist FILE` a text
file with `static.ads-twitter.com` and `cdn.taboola.com` to see blocked
requests.

The host tool fills its `AuSysInfo` with next to nothing, which shows
how the reports read where a fact is unknown.

## Tests

- `tests/browser/test_devtools.c`: the request log, the view (filter,
  problems, blocked, sorting), URL eliding, the header's columns, the
  Performance report's layout, the console's filters, the window
  places, the timing and benchmark tables, the Markdown lines.
- `tests/browser/test_bugreport.c` (ctest `browser_bugreport`): the
  report's sections in order, balanced code blocks and `<details>`,
  ASCII only, the machine and clock lines, and what must be absent: an
  Authorization header, a cookie, a password in an address, a query
  past 100 characters, another tab's address, the home page, the
  download folder, a proxy address, the search template, a site list
  and the history. It also checks the System Status report.

## What the audit found (2026-11-15, 640x480 guest)

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
- No item in the Help menu, and no way to collect a report in one step.
- The Cookies and Downloads windows are user features and were left as
  they are. `about:fonts`, `about:scene` and `about:licences` are pages
  and stay pages; the first two moved under Develop > Tests.

## Not done

- A horizontal scroll bar: the Network window cuts URLs to fit
  instead, and the other windows wrap.
- A time for each console line, and a source position for
  `console.log` lines (only errors carry a stack).
- A case-sensitive find in the developer windows.
- Request bodies and cookies in the Network details; saving the log to
  a file (Copy All copies it).
- The developer windows do not go into the session: they are not
  reopened at the next launch, only placed where they were.
- The timing test and the paint benchmark exist only in the Mac shell.
- `README.md` does not mention Report a Problem yet.

## Not verified in the guest

- The contextual menu's View Source opening the Page Source window (the
  guest tool has no Control-click).
- The contents of the file Save Report wrote: the file appeared on the
  desktop with its name, and was not opened.
- Pasting the copied report into another application: Copy All of the
  Console window was pasted into the Console's input line, which uses
  the same `mac_put_scrap`; the report itself is longer than that line
  accepts.
- The stock base (`BASE=stock`), where the Enforcer line should read
  "not installed", and any real Mac.
