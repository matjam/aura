# Architecture

This document says which folder holds what and how the parts of Aura
call each other, from the bytes of a response to the pixels of a
window. Read it first when you are new to the code or when a change
crosses more than one area; each part has its own document, linked
below.

## Which folder holds what

| Folder | Holds | Document |
|---|---|---|
| `src/core` | allocation (and the pooled allocator), buffers, UTF-8, atoms | [`memory.md`](memory.md) |
| `src/dom` | the node tree | |
| `src/html` | HTML tokenizer and tree builder | |
| `src/css` | CSS parser, selectors, cascade | [`css.md`](css.md) |
| `src/layout` | box tree, layout, painting, hit testing | [`layout.md`](layout.md) |
| `src/page` | the document view: parsing, style sheets, resources, input, nested documents (`frames.c`) | [`frames.md`](frames.md), [`forms.md`](forms.md) |
| `src/paint` | surfaces, rasterizer, fonts, the 2D canvas context (`canvas2d*.c`) | [`fonts.md`](fonts.md), [`canvas.md`](canvas.md) |
| `src/image` | image decoders (PNG and APNG, JPEG, GIF, WebP, BMP, ICO, SVG through `src/svg`), the per-document image cache | [`images.md`](images.md) |
| `src/svg` | XML parser, SVG path and transform grammars, the static SVG renderer, SVG documents | [`svg.md`](svg.md) |
| `src/browser` | browser model and chrome: tabs, history, bookmarks, downloads, preferences, the page interface and its implementation on the document view | [`browser-chrome.md`](browser-chrome.md), [`developer-tools.md`](developer-tools.md) |
| `src/js` | JavaScript: the QuickJS-ng runtime wrapper, the event loop, the DOM and window bindings (C natives plus the `prelude*.js` files) | [`javascript.md`](javascript.md), [`script-requests-and-cors.md`](script-requests-and-cors.md), [`workers.md`](workers.md) |
| `src/url` | WHATWG URL parser and serializer, hosts (IPv4, IPv6, Punycode) | |
| `src/codec` | streaming inflate (gzip, zlib, raw deflate) and Brotli decoders | |
| `src/net` | HTTP/1.1 client, TLS glue, cookies, HSTS, PSL, cache, sniffing, encodings, the resource loader, blocklists | [`networking.md`](networking.md), [`tls.md`](tls.md), [`content-blocking.md`](content-blocking.md) |
| `src/platform/posix` | the POSIX services a Unix port reuses (`posix.h`): BSD sockets and DNS for the network stack, files and the profile folder, clocks, entropy, coroutines, font files; no event loop | [`porting.md`](porting.md) |
| `src/platform/host` | the `aura-host` tool (`AURA_PLATFORM=host`): headless render to PNG, tree dumps, `fetch`, `js` and `page` for scripts, `chrome`, `blocklist`; built on `src/platform/posix` | [`building.md`](building.md) |
| `src/platform/macos9` | the Mac OS 9 application; Open Transport sockets, entropy and clocks for the network stack; `nettest/` | [`mac-os-9-shell.md`](mac-os-9-shell.md) |
| `src/platform/macos9/jstest` | JSTest, the JavaScript engine self-test and benchmark for Mac OS 9 | [`javascript.md`](javascript.md) |
| `src/platform/macos9/sea` | the installer: the extractor of the self-extracting archive | [`ci-and-releases.md`](ci-and-releases.md) |
| `third_party` | QuickJS-ng (`quickjs`, with `quickjs-patches` and `quickjs.cmake`) and Mbed TLS 4.1 (`mbedtls`, with `mbedtls-patches`, `mbedtls-config` and `mbedtls.cmake`); `third_party/README.md` records sources and licences | [`conventions.md`](conventions.md) |
| `tests/<area>` | one test area per folder, each with a `CMakeLists.txt` | |
| `tests/frames` | nested documents: the document view with a table of pages, and a browser window over the fake transport | [`frames.md`](frames.md) |
| `tests/canvas` | the canvas element and its 2D context: the context in C, a fuzzer, the document view over made-up origins, script pages, reference pictures | [`canvas.md`](canvas.md) |
| `tests/js` | binding, loop, limit, lifetime, security and worker scripts for `aura-host js`, scripted pages for `aura-host page`, the benchmark | [`javascript.md`](javascript.md) |
| `tests/webapi` | script requests over the fake transport (methods, headers, CORS, redirects), the CORS rules and the digests | [`script-requests-and-cors.md`](script-requests-and-cors.md) |
| `assets/fonts` | the bundled fonts, their licences and `fonts.idx` | [`fonts.md`](fonts.md) |
| `assets/net` | `aura-roots.bin`, the root certificate store | [`tls.md`](tls.md) |

`CMakeLists.txt` globs `src/**/*.c` outside `src/platform` into the
`aura_engine` library and adds every `tests/*/CMakeLists.txt`, so a new
engine source file or test area needs no change to the top-level file.
`AURA_PLATFORM` chooses the platform folder it adds: `macos9`, or
`host` (`src/platform/posix` and `src/platform/host`, with the tests)
([`building.md`](building.md), "Choosing the platform").

## To add a test

Put it in the folder of its area under `tests/` (or make a new folder
with a `CMakeLists.txt`; the top-level file picks it up). The area's
document names the existing tests and their case formats: for example
`tests/layout/cases/*.txt` in [`layout.md`](layout.md) and
`tests/css/cases/*.txt` in [`css.md`](css.md). Tests that need a
network use the fake transport (`tests/net/fakenet.c`, see
[`tls.md`](tls.md)). Run everything with `./build.sh test`
([`building.md`](building.md)).

## The engine and the platforms

The engine (everything in `src/` outside `src/platform/`) is portable
C99 that never calls the OS ([`conventions.md`](conventions.md)). A
platform supplies what the engine cannot do itself, through tables of
functions:

| What | Interface | Host (`posix/` and `host/`) | Mac OS 9 |
|---|---|---|---|
| DNS, TCP, clocks, entropy | `AuNetOps` (`src/platform/platform.h`) | `posix/net_posix.c` (BSD sockets), `clock_posix.c`, `entropy_posix.c` | `net_ot.c` (Open Transport), `entropy.c`, `clock_mac.c` |
| Windows, invalidation, scrolling the screen, dialogs | `AuBrowserHost` | `aura-host chrome` | `browserwin.c`, `main.c` |
| Fonts | the platform font set | `host/platform_host.c` (which files), `posix/fontdir_posix.c` (reading them) | `platform_mac.c` |
| Coroutines for workers | `AuCoroOps` (`src/core/coro.h`) | `posix/coro_posix.c` | `coro_mac.c` (Thread Manager) |
| Download files | `AuDownloadFiles` | | `downloads_mac.c` |
| The profile folder's files, clocks, network hooks | `AuAppPort`, `AuAppNet` (`src/browser/app.h`) | `posix/files_posix.c` over the folder `host/net_host.c` picks | `storage.c`, `network.c` |
| Blocklist files and memory | `AuBlockStore`, `AuBlockMem` | | `blocking_mac.c` |

The host tools (`aura-host`) run the same engine headless, so most
behaviour is tested on the development machine and only the shell is
tested in the guest ([`guest-testing.md`](guest-testing.md)).

## The layers of a browser window

1. **The shell** (`src/platform/macos9`, or `aura-host chrome` on the
   host) owns the OS window and the event loop. It passes events to the
   browser model and shows the rectangles the model reports through
   `AuBrowserHost.invalidate`. See [`mac-os-9-shell.md`](mac-os-9-shell.md).
2. **The browser model** (`src/browser`: `browser.c`, `chrome.c`) holds
   windows, tabs and back/forward lists, lays out and paints the tab
   strip, toolbar and status bar, and routes input. It never calls the
   OS. See [`browser-chrome.md`](browser-chrome.md).
3. **The page** (`page.h`, implemented by `page_doc.c`) is what a tab
   shows. It starts loads through a fetcher per scheme
   (`AuPageFetcher`), keeps documents for Back and Forward, and owns
   one document view at a time.
4. **The document view** (`src/page`, `AuDocView`) is one document: it
   feeds the HTML parser, loads style sheets, images and scripts,
   styles, lays out, paints and takes input. Nested documents are
   document views owned by frames ([`frames.md`](frames.md)).
5. **Script** (`src/js`) is one `AuJs` per document, driven by
   `src/page/script.c` ([`javascript.md`](javascript.md)).

## From bytes to pixels

1. **Fetch.** `au_page_navigate` starts a load through the scheme's
   fetcher. For http and https that is `src/net/page_fetcher.c` over the
   loader (`src/net/loader.h`), which applies the blocklist, mixed
   content blocking, HTTPS-first, the HTTP cache, MIME sniffing and
   conversion to UTF-8 ([`networking.md`](networking.md)). Compressed
   bodies pass through `src/codec`.
2. **Parse.** `au_page_idle` feeds the bytes to the HTML tokenizer and
   tree builder (`src/html`), which build the node tree (`src/dom`).
   Parser-blocking scripts pause the parser
   ([`javascript.md`](javascript.md)).
3. **Style.** Each `<style>`, `<link>` and `@import` is parsed once
   into a sheet; the resolver computes a style per element and
   restyles only what a change can affect ([`css.md`](css.md)).
4. **Layout.** `src/layout` builds the box tree from the styled tree
   and lays it out: block, inline, flex, grid and table formatting
   contexts, with replaced boxes for images, inline SVG, canvases, form
   controls and frames ([`layout.md`](layout.md)). Text is shaped
   through the font list ([`fonts.md`](fonts.md)).
5. **Paint.** `au_layout_paint` paints the boxes inside the canvas clip
   onto an `AuSurface` with the rasterizer of `src/paint`; images are
   decoded when a paint uses them ([`images.md`](images.md)).
6. **Present.** The shell copies the painted rectangles to the screen.
   A scroll moves pixels and paints only the strip that scrolled in
   ([`browser-chrome.md`](browser-chrome.md), "Scrolling by moving
   pixels").

## The event loop and idle time

Everything runs on the main task: the Toolbox is not reentrant and
Open Transport is not safe to call from elsewhere
([`mac-os-9-shell.md`](mac-os-9-shell.md)). Work is done in short
slices from the shell's event loop:

- `au_net_pump` runs the network stack; its callbacks run only from the
  pump.
- `au_page_idle` reads data and feeds the parser, at most 40 ms per
  call, and runs the page's scripts for up to 20 ms.
- `au_bw_idle` polls icon fetches and trims kept documents.
- Workers run in slices on their own stacks through coroutines
  ([`workers.md`](workers.md)).
- Disk cache writes, blocklist compiles and blocklist updates happen at
  idle time while nothing loads.

## Memory

All engine memory comes from `au_alloc`, which on Mac OS 9 takes small
blocks from a pooled allocator. Caches (kept documents, decoded images,
the HTTP cache) give memory back through the release handler before
the out-of-memory handler runs. [`memory.md`](memory.md) describes the
model, the budgets and how to measure.
