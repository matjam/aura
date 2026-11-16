# Developer tools

This document covers the Develop menu: the four developer windows (Page
Source, Console, Network, Performance), the debug items, and the host
commands that print the same text. Read it before changing a developer
window or when you need to see what a page requested, logged or cost.

Aura has four developer windows, opened from the Develop menu: Page
Source (also View > Page Source, Command-U), Console, Network and
Performance. Each shows the active tab of the frontmost browser window
and follows it. The models are portable and in the engine; the Mac OS 9
windows are `src/platform/macos9/devwin.c`. The Develop menu also holds
the debug items (Test Scene, Font List, Benchmark, Network Status, Page
Timings).

## To see the same text on the host

Run `aura-host chrome` with these actions after the page loads; add
`--net` for network pages.

| Action | Prints |
|---|---|
| `--do netlog` (or `netlog:details`) | the request log |
| `--do perf` | the performance report |
| `--do eval:EXPR` | the console's input line with EXPR |
| `--do source` (or `source:FIRST,COUNT`) | the page source (lines with their colour runs) |
| `--do devshot:source\|console\|network\|perf:FILE.png` | paints that window's text view to a PNG |
| `--do console` | the console lines |

For example, `aura-host chrome --net --tab https://example.com/ --do
netlog:details --do perf`.

## The debug items

[`guest-testing.md`](guest-testing.md) says how to click the Develop
menu in the guest.

- **Develop > Benchmark** times the scene, fills and the chrome and
  shows how long font registration took (and the bundled part of it)
  and how much font data is loaded.
- **Develop > Font List** (about:fonts) shows the fonts Aura found,
  with the bundled ones marked.
- **Develop > Page Timings** shows the active page's first paint, load
  and layout times and image decodes, then times scrolling: ten
  32-pixel steps (twice: the first pass paints content for the first
  time, the second shows the steady state), five page downs and three
  repaints of the whole page, each split into moving the offscreen
  pixels, painting, copying to the screen and the number of areas
  painted. It shows free heap, the largest free block, temporary memory
  and the glyph cache, and the page source's size, what is held for it
  and how long compressing it took ([`memory.md`](memory.md), "Cold
  memory"). With a page field focused it types 16 keys and
  shows milliseconds per key ([`forms.md`](forms.md)); for a page shown
  from the kept documents it shows the Back times
  ([`browser-chrome.md`](browser-chrome.md)).
- **Develop > Network Status** shows the clock
  ([`mac-os-9-shell.md`](mac-os-9-shell.md)), entropy, Open Transport
  counters, roots, cookies and cache use; the HTTP cache's lookups
  (fresh, history, revalidated, 304, missed) and the time spent reading
  and writing cache files; the bytes the HTTP cache holds in memory and
  on disk; and the blocklist totals, where the list memory came from
  and what the last compile cost.
- **Develop > Test Scene** shows about:scene, the test scene the host
  renders with `aura-host scene`.

## The text view

To change how the windows draw text, edit `src/browser/textview.c`: a
fixed-width text view (Monaco, else the monospace family, 10 pixels)
with optional line numbers, wrapping, colour runs, a character
selection or a selected row, find, and scrolling by rows. It owns no
text: it asks an `AuTextSource` for one line at a time, reads each
line's cell count once per layout and paints only the rows on screen.
`AuTextList` is a ready-made source for text made on the spot.

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
out in under 1 ms and paints a screen of it in 1 ms. The contextual
menu's View Source still opens a `view-source:` tab.

## Network

Network is the page's request log, `src/browser/netlog.c`: a ring of
300 records, cleared by a navigation and freed with the page. A
document kept for Back and Forward keeps its log, which returns with
it.

- `page_doc.c` fills it from its fetches (start, response head, data,
  end).
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
- While no request is selected, the window's lower pane lists the
  page's blocked requests with the list that stopped each
  (`au_netlog_blocked`).

## Console

Console shows the page's console lines (`au_page_console_line`) through
`au_dev_console_lines` in `src/browser/devtools.c`. Uncaught errors and
unhandled rejections arrive as error lines with their stack; the first
stack frame gives the source URL and line. The input line calls
`au_dev_console_eval`, which runs the expression with `au_page_eval`
(starting the script runtime on a page that has no script) and adds the
expression and its result to the console.

## Performance

Performance is `au_dev_perf_report` in `devtools.c`, rebuilt once a
second.

- The time counters are `AuPerf` (`src/page/perf.h`), one per document
  view, cleared when a document begins: the view times its style and
  layout passes, its paints and its outermost script entries with the
  embedder's millisecond clock, and the Mac shell adds each screen copy
  (`flush` in `browserwin.c`).
- Tasks of 50 ms or more are kept for the slowest-tasks list, scripts
  with their URL.
- The shell supplies free heap and temporary memory, and
  `au_net_dev_info` (`src/net/net_report.c`) the HTTP cache and the
  open connections.
- Copy Report puts the text on the clipboard, for measurements from a
  real machine.

## Tests

`tests/browser/test_devtools.c` runs the request log on the fake
transport (sources, connections, timings, headers, blocked requests,
the ring), the counters and the report, the console and its input line,
and the source view and text view (lines, colouring, wrapping,
selection, find).

## Not done

See [`known-gaps.md`](known-gaps.md#developer-tools).
