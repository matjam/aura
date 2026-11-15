# Aura: development notes

`README.md` describes Aura for its users. Keep it short. This file holds
what you need to build, test and change Aura.

## Conventions

- C99, portable. Engine code (everything in `src/` outside
  `src/platform/`) makes no OS calls and includes no OS headers. It must
  build for the host and for PowerPC Mac OS 9 (big-endian, 32-bit, newlib).
- Do not recurse over document depth (tree walks, destruction, layout of
  nested boxes where it can be avoided): Mac OS 9 stacks are small and
  pages nest deeply. Use explicit loops or an explicit stack.
- Allocate with `au_alloc`, `au_realloc`, `au_free` (`src/core/core.h`).
  They do not return NULL.
- Text is UTF-8. Names (tags, attributes, CSS properties and keywords) are
  atoms. Names known at compile time go in `src/core/atoms_html.def` or
  `src/core/atoms_css.def`.
- Public names start with `au_` (functions), `Au` (types), `AU_`
  (constants).
- Third-party code is limited to QuickJS-ng (JavaScript) and BearSSL
  (TLS), in `third_party/`. Everything else is written here. Ask before
  adding another.
- Commit each working step to `main`. Commit only our own sources. Apple
  files, downloaded test suites and fonts stay out of git, except the
  redistributable bundled fonts in `assets/fonts`, which are committed
  with their licences.
- Write docs instruction first ("To X, use Y"), in plain technical prose.

## Layout

| Folder | Holds |
|---|---|
| `src/core` | allocation, buffers, UTF-8, atoms |
| `src/dom` | the node tree |
| `src/html` | HTML tokenizer and tree builder |
| `src/css` | CSS parser, selectors, cascade |
| `src/layout` | box tree, layout, painting, hit testing |
| `src/page` | the document view: parsing, style sheets, resources, input |
| `src/paint` | surfaces, rasterizer, fonts |
| `src/image` | image decoders (PNG and APNG, JPEG, GIF, WebP, BMP, ICO), the per-document image cache |
| `src/browser` | browser model and chrome: tabs, history, bookmarks, downloads, preferences, the page interface and its implementation on the document view |
| `src/url` | WHATWG URL parser and serializer, hosts (IPv4, IPv6, Punycode) |
| `src/codec` | streaming inflate (gzip, zlib, raw deflate) and Brotli decoders |
| `src/net` | HTTP/1.1 client, TLS glue, cookies, HSTS, PSL, cache, sniffing, encodings, the resource loader |
| `src/platform/host` | host tools: headless render to PNG, tree dumps, `fetch`, BSD sockets for the network stack |
| `src/platform/macos9` | the Mac OS 9 application; Open Transport sockets, entropy and clocks for the network stack; `nettest/` |
| `third_party` | BearSSL 0.6 (`third_party/README.md` records sources and licences) |
| `tests/<area>` | one test area per folder, each with a `CMakeLists.txt` |
| `assets/fonts` | the bundled fonts, their licences and `fonts.idx` |
| `assets/net` | `aura-roots.bin`, the root certificate store |

`CMakeLists.txt` globs `src/**/*.c` into the `aura_engine` library and adds
every `tests/*/CMakeLists.txt`, so a new source file or test area needs no
change to the top-level file.

## Build and test

- `./build.sh test` builds for the host and runs the tests.
- `./build.sh ppc` builds for Mac OS 9 with Retro68 (GCC 16.1, Multiversal
  Interfaces). The toolchain is found through `RETRO68`, then in
  `~/.local/opt/retro68` (`toolchain/find.sh`). A `build/ppc` folder
  keeps the toolchain it was first configured with; delete it to switch.
  It also assembles the distributable folder `build/ppc/dist/Aura`: the
  application (`Aura.bin`, MacBinary) beside `Fonts` (the family folders
  of `assets/fonts`, `fonts.idx` and the fonts' README).
- To run the tests under AddressSanitizer and UBSan, configure a separate
  folder: `cmake -S . -B build/asan -G Ninja
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined"`, then `cmake --build
  build/asan` and `ctest --test-dir build/asan`.

## Dependencies outside the repository

Aura depends on no other project's checkout. What it needs is installed
by its own scripts or kept in git-ignored folders here.

- **Retro68.** To install the toolchain, run `toolchain/build.sh`. It
  fetches Retro68 at the commit pinned in the script (a4a389c41e90,
  PowerPC only, Multiversal Interfaces) into `toolchain/Retro68`, builds
  in `toolchain/build`, and installs in `~/.local/opt/retro68` (or
  `$RETRO68`). It does nothing when that folder's `REVISION` names the
  pinned commit. To move to another commit, change it in the script,
  empty the install folder, and run the script. In CI, cache
  `~/.local/opt/retro68` keyed on the hash of `toolchain/build.sh`.
- **QEMU.** To install the QEMU the test machine runs, run
  `qemu/build.sh`. It clones the release pinned in the script (v11.1.3)
  into `qemu/src`, applies `qemu/patches/*.patch`, and installs in
  `~/.local/opt/qemu-os9` (or `$QEMU_PREFIX`). It does nothing when the
  install's `REVISION` matches.
- **Disk images.** The test machine's base images are in `disks/`
  (git-ignored): `os9-novm.qcow2` is a stock Mac OS 9.2.2 with virtual
  memory off, and `os9-vmoff.qcow2` is the same system with Enforcer
  installed. Both are standalone qcow2 files with no backing file. To
  update Enforcer on the second, install a new Enforcer build in that
  guest with `PERSIST=1` and fold the overlay into the image.
- **hfsutils** (`hformat`, `hcopy`) for `tools/guest-push.sh`:
  `brew install hfsutils`.

The install scripts, the QEMU patch and the two disk images were copied
from the os9-enforcer project on 2026-11-14.

## Guest testing

Aura has its own test machine: a QEMU mac99 guest running Mac OS 9.2.2,
with outbound network access. Runtime files go in `run/` of the main
checkout (set `AURA_RUN` to use another folder, for example to run a
second guest beside the first); `tools/guest-env.sh` holds the settings.

To start the guest, run `tools/guest-run.sh start`. QEMU runs headless in
the background. `BASE` picks the system: `stock` (the default) is
`disks/os9-novm.qcow2`, Mac OS 9.2.2 with virtual memory off and no
Enforcer; `enforcer` (`BASE=enforcer tools/guest-run.sh start`) is
`disks/os9-vmoff.qcow2`, which has the Enforcer kernel and extension
installed. The guest boots an overlay on the base image
(`run/os9-overlay-BASE.qcow2`), so the base is never written. One guest
runs per run folder; `status` names the running base. Disk writes are
discarded when QEMU exits; set `PERSIST=1` to keep them in the overlay.
Mac OS takes about a minute to boot.

Test on the stock base first. Use the Enforcer base to check Aura under
Enforcer; its own bugs show there too (see the scroll bar thumb note
under Browser chrome). Enforcer's state and offense log are read with
that project's own tools, which Aura does not carry.

To copy the application in, build it with `./build.sh ppc` and run
`tools/guest-push.sh -n AuraTest build/ppc/dist/Aura/*`, which puts the
application and its `Fonts` folder side by side at the top of the disc
(push `build/ppc/src/platform/macos9/Aura.bin` alone to test without the
bundled fonts). A folder argument is copied with its subfolders; fonts
get type `tfil` and creator `AURA`, licences and READMEs become
SimpleText documents, and `.html`, `.css`, `.js` and `.idx` files are
copied byte for byte (hcopy's text mode would translate UTF-8 as
Latin-1). HFS names are at most 31 characters.
The files arrive as a CD named AuraTest on the guest's desktop. The
previous disc is ejected first through the Finder; the script types
nothing until the guest's current application (`CurApName`) is the
Finder, and fails if an application on the disc is still open, so quit
Aura before pushing a new build.

To launch it, run `tools/guest-qmp.py open AuraTest Aura`. To take a
screenshot, run `tools/guest-qmp.py screenshot run/NAME.png`. To type, use
`tools/guest-qmp.py key cmd-q` (any character works as the key, as in
`'cmd-['`) or `type TEXT`; to click or drag, use `click X Y` or `drag X1
Y1 X2 Y2` (guest screen pixels). `front-app` prints the current
application.

To compare the window with the host rendering, render the scene at the
window's size (the title shows it) and compare the content area, which
starts at 20,50 on the guest screen:

```bash
build/host/src/platform/host/aura-host scene -w 584 -h 390 -o run/host.png
tools/png-compare.py run/guest.png 20 50 run/host.png run/diff.png
```

To change the screen depth, click the Monitor BitDepth module in the
guest's Control Strip (the seventh icon, at 197,466 on a 640x480 screen)
and pick 256 Colors, Thousands or Millions. In Aura, View > Debug >
Benchmark times the scene, fills and the chrome and shows how long font
registration took (and the bundled part of it) and how much font data
is loaded; View > Debug > Font List (about:fonts) shows the fonts it
found, with the bundled ones marked; and View > Debug > Page Timings
shows the active page's first paint, load and layout times, times ten
32-pixel scroll repaints, and shows free heap and temporary memory.
The Debug submenu opens by dragging (a click closes it); the
`QMP.button` and `QMP.move` methods of `tools/guest-qmp.py` can hold
the button through View, Debug and the item.

The Finder ignores `guest-qmp.py drag` (it moves in one step). To drag
a file in the Finder (onto the Aura icon, or a copy to the hard disk),
hold the button for most of a second and move in about ten steps of
0.15 s each, with the QMP class from `tools/guest-qmp.py`.

To open a local page in the guest, push it with the application (for
example `tools/guest-push.sh -n AuraTest Aura.bin
tests/page/samples/*`) and choose File > Open File, drop it on the Aura
icon, or type its path (`AuraTest:wiki.html`) in the address field.

To stop the guest, run `tools/guest-run.sh stop`, or choose Special > Shut
Down in the guest.

## Browser chrome

To change how a browser window looks or behaves, work in `src/browser`:
`chrome.c` lays out, paints and hit-tests the tab strip (or left tab
column), toolbar and status bar; `browser.c` holds windows, tabs and
back/forward lists and routes input; `page.h` is the interface a page
implements, and `page_doc.c` implements it on the document view
(`src/page`). The model never calls the OS; the shells pass events in and
show the rectangles it reports through `AuBrowserHost.invalidate`.

Pages load asynchronously: `au_page_navigate` only starts a load, and
`au_page_idle` reads files (the document, then its style sheets,
@imports and images in request order) and feeds the parser, at most
40 ms per call. While a document loads it is laid out at most every
300 ms (four times the last layout's duration for slow pages), then once
at the end. Generated about: pages (home, blank, fonts) load inside
`au_page_navigate`; about:scene paints the test scene directly. A URL
that differs from the current one only by its fragment scrolls instead
of loading, and back and forward restore the scroll position each entry
was left at. To plug in networking, register a fetcher per scheme with
`au_browser_set_fetcher` (see `AuPageFetcher` in `page.h`); without one,
http and https show a placeholder after a simulated load. Images are
decoded by `src/image` (see Images); `au_page_idle` advances animations.
The font preferences reach the engine as the generic families of
the font set and a user style sheet (`html { font-size }`, monospace
size). `src/css` now has `au_css_resolver_set_font_prefs` (default
sizes, text zoom, minimum size); the shell does not call it yet.

To render a browser window on the host, run
`build/host/src/platform/host/aura-host chrome -w 800 -h 600 --tabs left
--tab about:home --tab file:tests/page/samples/wiki.html -o
run/chrome.png`. `--tab` takes a URL, a path or `file:` with a relative
path; `--scroll Y` scrolls the active tab and `--mouse X Y` moves the
pointer (hover, a link's status text). Run `aura-host` without arguments
for the other options (tab overflow, hover, address field text).

The Mac OS 9 shell (`src/platform/macos9`) keeps one module per kind of
window (`browserwin.c`, `listwin.c`, `prefswin.c`), with menus and the
event loop in `main.c` and files in `storage.c`. The address field is a
TextEdit record placed over the field the engine draws; the scroll bars
and menus are native; the Preferences window uses Appearance Manager
controls, declared in `appearance.h` because the Multiversal Interfaces
lack them, from AppearanceLib.

Aura keeps its files in the Aura folder of the system Preferences folder:
`Aura Preferences`, `Bookmarks.html` (Netscape format), `History`,
`Session` and `Downloads`. Each is written to a `.tmp` file and swapped
in; a file that fails to parse is renamed with ` (damaged)` before a new
one is written. To start with clean state in the guest, delete that
folder.

Downloads use a simulated transfer source (`au_sim_source` in
`downloads.c`) until networking exists; View > Debug > Start Simulated
Downloads adds a few.

Toolbox points learned the hard way:

- In a Rez `MENU`, the enable flags put item k at bit k-1 (the menu's
  own flag is the separate `enabled` field). A separator at item 5
  clears bit 4.
- Do not call `TEGetPoint` from PowerPC code; it crashed the guest. The
  caret position comes from `TextWidth` with the TextEdit handles
  locked.
- Scroll bar thumbs drawn as coloured noise on the second launch of
  Aura in a guest boot come from Enforcer, not Aura: they appear only on
  the Enforcer base, at any depth, with or without dithering. The
  Appearance theme draws thumbs through a 16x17 GWorld it keeps in the
  system heap, with its pixels in temporary memory, made the first time
  a thumb is drawn in the boot. Enforcer's temporary memory pool puts
  those pixels in a chunk of the current application (Aura) and frees
  the chunk when Aura quits; the stock Process Manager keeps them. The
  next Aura launch gets a chunk at the same address, and the theme's
  handle then names Aura's font data.

## Fonts

Aura draws with the system's fonts first and with the fonts bundled in
the `Fonts` folder beside the application for families and characters
the system lacks.

- **Where faces come from.** On Mac OS 9, `platform_mac.c` registers
  every outline face of the FONDs in the resource chain (their `sfnt`
  resources load on first use), then the bundled faces listed in
  `Fonts/fonts.idx`. On the host, `platform_host.c` loads a fixed list of
  macOS fonts that match Mac OS 9's, then the bundled faces of
  `assets/fonts` (`fontdir.c`), lazily. `aura-host` takes
  `--bundled-only` (no system fonts: reproducible renders, close to a
  Mac OS 9 machine without the system's fonts), `--no-bundled-fonts` and
  `--fonts-dir D`; `$AURA_FONTS` also names the folder. `aura-host
  fonts` lists the faces and marks the bundled ones.
- **The bundled set** is in `assets/fonts`, one folder per family with
  its licence: Aura Sans, Aura Serif and Aura Mono (Liberation 2.1.5,
  metric-compatible with Arial, Times New Roman and Courier New, renamed
  because Liberation's Reserved Font Name forbids a subset under its own
  name), Inter (modern interface fonts), DejaVu Sans (the last-resort
  text face) and Noto Emoji (monochrome emoji), about 3.4 MB.
  `assets/fonts/README.md` records sources, versions, licences, sizes
  and what was left out.
- **To add a font**, put its folder (licence included) in
  `assets/fonts`; reduce it with `tools/fonts-prepare.py` or pyftsubset
  to TrueType outlines (no CFF, no variable fonts, no colour tables),
  keeping file and folder names to 31 characters; regenerate the index
  with `build/host/src/platform/host/aura-host font-index assets/fonts`;
  add its sources to the README; and name it in the alias table or the
  fallback lists if it should stand in for other families. The paint
  tests check that `fonts.idx` matches the folder and that every face
  parses and draws.
- **The alias table** is `src/paint/fontalias.c`: rows map a family name
  as pages write it (ASCII case-insensitive) to the families tried in
  its place, installed system families first and bundled faces last.
  Rows for the CSS generic families also name the preference slot whose
  family (the user's font preferences, or the platform's choice) is
  tried before the row; a target naming a generic expands to it. To
  extend it, add a row. Matching (`au_fontset_match`) tries the families
  the page names up to its first generic family, then their alias
  targets, then the generic, and so on, then sans-serif. So "Roboto,
  Arial, sans-serif" uses an installed Arial before Inter, and "Roboto,
  sans-serif" uses Inter.
- **Per-character fallback** is `AuFontList` (`au_fontlist_new`) and
  `au_shape_text_list` in `src/paint/fallback.c`, which layout uses for
  all text through its `LFont`. Text is split into clusters (a base with
  its combining marks, variation selectors and emoji modifiers) and each
  cluster gets the first face that has all of it: the primary face, the
  list's other candidates in matching order, the text fallback list
  (DejaVu Sans first, then system faces with wide coverage, including
  CJK faces when a machine has them), the emoji list, then any face
  already loaded. Emoji presentation (by default or with U+FE0F) tries
  the emoji list first. Default-ignorable code points draw nothing, so
  U+FE0F is dropped and ZWJ sequences draw their components; skin tone
  modifiers are dropped. Marks are placed over their base by their ink
  boxes (the fonts carry no GPOS for Aura to read). Fallback faces keep
  the requested weight and style, synthesized when missing. A per-list
  cache and a per-set cache keyed by code point and style keep this
  fast; a face is loaded only when it is tried.
- **Line height.** With `line-height: normal`, a line holding fallback
  glyphs grows to the fallback face's ascent and descent (each with half
  its line gap) where they exceed the primary's, as Blink does; a fixed
  line height ignores fallback faces.
- **Not done:** OpenType shaping (GSUB, GPOS), so no ligatures, Arabic
  joining, Indic reordering or bidirectional text; colour emoji; a CJK
  font pack (see the fonts README for what one needs).

## Images

The decoders in `src/image` parse hostile data on a machine without
memory protection. Every length comes from the file and is checked
before use; every size computation is checked for overflow; dimensions
are capped at 32768 and source pixels at 64 million (`AuImageOptions`);
the decoded canvas never exceeds `max_bytes` (16 MB by default), and a
larger image decodes at 1/2, 1/4 and so on down to 1/256, or fails.
Keep it that way: a change to a decoder needs a vector, a truncation
test and a fuzz run under the sanitizers.

- **To decode bytes**, create an `AuImageDecoder` with
  `au_image_decoder_new` and call `au_image_decode` with each chunk as
  it arrives (`image.h`). The size is known as soon as the header
  arrives (`au_image_info`), rows fill in as they decode
  (`au_image_take_dirty`), and each animation frame returns
  `AU_IMAGE_FRAME`. Output is a premultiplied `AuSurface`; its `opaque`
  flag lets painting copy instead of blend. `au_image_decode_buffer`
  decodes a whole file, `au_image_probe` reads only the header, and
  `au_image_decode_favicon` picks and scales an icon.
- **Formats.** One file per format: `png.c` (all colour types and
  depths, Adam7, APNG, CRCs checked), `jpeg.c` (baseline and
  progressive Huffman, CMYK and YCCK, restarts, EXIF orientation,
  reduced decoding by IDCT scaling), `gif.c`, `webp.c` with
  `webp_vp8.c` and `webp_vp8l.c` (lossy, lossless, alpha, animation),
  `bmp.c` (BMP and ICO). `image.c` holds what they share: sniffing, the
  limits, row reduction, orientation and frame composition. Each codec
  is a state machine that consumes every byte it is given, so chunk
  boundaries never matter.
- **The cache** (`cache.h`) holds one document's images by URL. Layout
  gets the size from the header; pixels are decoded only when an image
  is used in a paint (`au_image_entry_use`), at the smallest power-of-two
  reduction that still covers the displayed size. The budget comes from
  the memory cache preference (`au_docview_set_image_budget`, 4 MB by
  default in the browser, 8 MB otherwise). Over the budget, images not
  used in the last paint lose their pixels, oldest first, and keep their
  compressed bytes to decode again. One image may use at most half the
  budget (at least 1 MB, at most 16 MB). A still image whose bytes are
  larger than its pixels keeps only the pixels. Animations decode frame
  by frame from the compressed bytes, keep one canvas, and pause when
  not painted; delays of 10 ms or less play at 100 ms, as browsers do.
- **The document view** (`src/page/docimages.c`) decodes images within
  a viewport above or below the visible area before they scroll in,
  invalidates only the rectangles of images that changed, and passes
  pixels to layout through the image callback. A pending image is an
  empty box; a failed one shows its alt text.
- **To try a file**, run `build/host/src/platform/host/aura-host image
  FILE -o out.png`. It prints the format, size, decode time and peak
  memory; `--scale N`, `--target WxH`, `--chunk N` (feed N bytes at a
  time), `--frames`, `--repeat N` and `--favicon SIZE` change what it
  does. `tests/page/samples/images.html` shows every format; its images
  are drawn by `tests/page/make-sample-images.py`.
- **Tests.** `tests/image` has one test per format with committed
  vectors and references made by the host's own decoders (the
  generator scripts and their headers say how). PngSuite and the libwebp
  test images are fetched into `build/` by
  `tests/image/fetch-pngsuite.sh` and a `git clone` of
  `https://chromium.googlesource.com/webm/libwebp-test-data` into
  `build/webp-testdata`; their tests skip without them. To fuzz longer,
  build with the sanitizers and run `build/asan/tests/image/fuzz_image
  ITERATIONS SEED tests/image/vectors/*/*`.
- **Not done:** arithmetic-coded, lossless and 12-bit JPEG; colour
  management (gamma, ICC profiles and colour spaces are ignored);
  object-fit, border-image and list-style-image in layout. At 256
  colours images band; nothing dithers them.
- Retro68's `<string.h>` pulls in the Toolbox `Strings.h`, so names like
  `FInfo` collide with Toolbox types. Prefix types with `Au`.

## Networking

The network stack is portable engine code that never touches a socket
itself. `src/net` talks to the platform through `AuNetOps`
(`src/platform/platform.h`): DNS, non-blocking TCP, a monotonic and a
wall clock, and an entropy source. The host implements it with BSD
sockets (`src/platform/host/net_host.c`); Mac OS 9 with Open Transport
(`src/platform/macos9/net_ot.c`). There is no global state: one `AuNet`
per application holds the connection pool, TLS configuration and the
policy stores the embedder attaches.

- **To fetch with the engine**, create the platform's ops, then
  `au_net_new(ops)`, attach a root store (`au_roots_load` on
  `assets/net/aura-roots.bin`), and optionally a cookie jar, HSTS store
  and cache (`au_net_set_cookie_jar`, `au_net_set_hsts`,
  `au_net_set_cache`). Start requests with `au_http_start` and call
  `au_net_pump` on every pass of the event loop; callbacks run only from
  the pump. `au_net_apply_prefs` copies the browser preferences in.
- **To load pages and resources**, use the loader (`src/net/loader.h`):
  it adds mixed content blocking, MIME sniffing with `nosniff`, encoding
  detection and conversion to UTF-8, `data:` URLs, HTTPS-first, and the
  download hand-off (`au_net_download_*`, shaped for `AuTransferSource`).
  `src/net/page_fetcher.c` adapts it to the browser's `AuPageFetcher`:
  `au_net_page_fetcher_interface` fills one, and the shell registers it
  for `http` and `https` with `au_browser_set_fetcher`.
- **To persist state**, serialize the stores with
  `au_cookie_jar_serialize`, `au_hsts_serialize` and the cache's
  `AuCacheStorage`, and parse them back at start. `aura-host fetch` keeps
  `cookies` and `hsts` in `$AURA_PROFILE` (default `~/.aura-host`).
- **To try a URL on the host**, run `build/host/src/platform/host/aura-host
  fetch -v URL`. It prints the response headers, the TLS version, cipher
  and certificate chain, and timings. `aura-host render URL -w 800 -o
  out.png` loads a page and its resources over the network and renders
  it. Set `AURA_FAKE_TIME` (seconds since 1970) to test the clock check.
- **To try the stack on Mac OS 9**, build with `./build.sh ppc` and push
  `build/ppc/src/platform/macos9/nettest/NetTest.bin` with a copy of
  `assets/net/aura-roots.bin` named `aura-roots` (`guest-push.sh` would
  decode a `.bin` file as MacBinary). NetTest times the crypto
  primitives, a DNS lookup and a TCP connection, then fetches a URL list
  twice and prints status, cipher and timings; a `NetTest URLs` file
  beside it replaces the list. It also writes `NetTest Log` in the Aura
  preferences folder.

TLS is BearSSL 0.6 with the 32-bit implementations: TLS 1.2 only, ECDHE
with AES-GCM or ChaCha20-Poly1305, SNI, ALPN `http/1.1`, session
resumption, and certificate errors reported with the chain
(`au_http_cert_info`). TLS 1.3 is not available in BearSSL; every
acceptance site still offers TLS 1.2.

All network work runs on the main task. Open Transport is called only
from the main task in asynchronous mode; its notifiers run at deferred
task time and only record which completion arrived, and the next pump
acts on it. Do not call Open Transport or the engine from an MP task or
a Time Manager task: Apple's MP documentation (TN2006) lists Open
Transport among the services that are not MP-safe, and the Toolbox is
not reentrant.

The Mac OS 9 entropy pool (`src/platform/macos9/entropy.c`) gathers
loop timing jitter, user input, the mouse, clocks and free memory into a
SHA-256 pool and refuses output until it has credited 128 bits, so the
first TLS handshake waits for it (about 15 s from a cold start with no
user input, less with a seed file or input). The credit rules are in the
file's header. A seed file, `Random Seed` in the Aura preferences folder,
carries state between runs and is rewritten at start, when the pool
first fills, and at exit. The shell calls `au_mac_entropy_event` for
every event, `au_mac_entropy_idle` once per loop pass, and
`au_ot_net_idle` with them.

Open Transport has no headers in the Multiversal Interfaces, and Aura has
no Apple glue library. `src/platform/macos9/ot.h` declares the calls
used, and `net_ot.c` calls the exported `Priv` routines with its own
client record, as Apple's glue did.

On the test machine, QEMU sets the guest clock to UTC while the guest's
Map control panel says UTC-7, so the wall time Aura computes there is
7 hours ahead. Certificates are still within their dates.

Limits: IPv4 only on Mac OS 9; no HTTP/2; no proxy auto-configuration;
cookies, HSTS and the cache are kept in memory until the shell saves
them.

## Design

The decisions and milestones are in the Gandalf vault,
`project:aura:decisions`.

The scope of version 1 (acceptance sites, required features, what is left
out) is in `docs/v1-scope.md`. Check a feature against it before building
it.
