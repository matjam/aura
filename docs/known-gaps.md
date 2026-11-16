# Known gaps

This document gathers, by area, what is known to be missing, limited or
unverified in Aura. Read it when you look for work, or when a site
fails and you want to know whether the cause is already known. Each
area's document describes what is there; this one lists what is not.
When you finish an item, remove it here; when you find a gap, add it
here under its area.

What version 1 leaves out on purpose is in [`v1-scope.md`](v1-scope.md).

## Why a site fails: where to look first

| Symptom | Likely area |
|---|---|
| "Secure connection failed" | [Networking and TLS](#networking-and-tls): DHE-only or CBC-only servers |
| A script error about a missing API | [JavaScript](#javascript), [Script requests and CORS](#script-requests-and-cors), [Canvas](#canvas), [Workers](#workers) |
| Text without ligatures, wrong order of Arabic or Hebrew, missing CJK | [Fonts](#fonts) |
| A layout that depends on container queries, subgrid or baseline alignment in grid | [CSS](#css), [Layout](#layout) |
| An embedded widget that stays empty | [Frames](#frames) |
| Missing pictures or icons | [Images](#images), [SVG](#svg) |

## Not verified

- Aura has not run on a real Mac. No real G4 has been measured: every
  guest figure is from QEMU, whose speed depends on the host.
- The scroll hover delay (`AU_REHOVER_MS`, 150 ms) has not been
  measured again in the guest since hover restyles became targeted
  ([`browser-chrome.md`](browser-chrome.md)).
- Layout rules that name Blink come from reading Blink's source and
  from CSS specifications, not from running Chrome beside Aura
  ([`layout.md`](layout.md)).
- The expected values of the grid cases are worked out from the
  specification; a comment says where a browser was not consulted.
- The canvas reference pictures were checked by eye against the
  standard, not against another browser.
- The emoji set used for line breaking is not generated from the
  Unicode data files.
- X25519 costs about 26 ms per handshake in the guest and is not tuned
  yet ([`tls.md`](tls.md)).
- Acid2: the cause of the four differing rows of the eyes is not found
  ([`layout.md`](layout.md), "Acid2").
- The allocator's temporary memory path on Mac OS 9 has not supplied a
  block in the guest: the heap never ran short there
  ([`memory.md`](memory.md), "Temporary memory for the heap").
- Developer windows, in the guest
  ([`developer-tools.md`](developer-tools.md)):
  - the contextual menu's View Source opening the Page Source window
    (the guest tool has no Control-click);
  - the contents of the file Save Report wrote: the file appeared on
    the desktop with its name, and was not opened;
  - pasting the copied report into another application: Copy All of the
    Console window was pasted into the Console's input line, which uses
    the same `mac_put_scrap`; the report itself is longer than that
    line accepts;
  - Report a Problem and System Status on the stock base
    (`BASE=stock`), where the Enforcer line should read "not
    installed", and on any real Mac.

## Layout

- Text is segmented and measured again for each layout of its block
  (the word cache answers the widths).
- `l_layout_abspos_for` removes queue entries with `memmove`.
- A DOM insertion or removal builds the whole box tree again (the
  search suggestions on Wikipedia cost a tree build each time the list
  changes).
- Images: `currentSrc` in script, `image-set()`, and `sizes` lengths
  other than px, vw, vh, em and rem.
- Grid: `subgrid` (parsed; laid out as `none`), baseline alignment of
  items (they align at the start), `direction: rtl` and vertical
  writing modes, `safe` overflow alignment, painting items in `order`
  order, `masonry`. An item's min-content and max-content heights are
  both its height at its used width.
- `frameset` `rows` and `cols` sizes are not read.
- Shadow trees are not rendered: an element with a shadow root shows
  its light children. GitHub's `<relative-time>` elements therefore show
  the server's date ("Nov 16, 2026") instead of the text their shadow
  root holds ("20 minutes ago"); seen on 2026-11-15 while adding Intl.
- Acid2 still differs in four rows of the eyes and along the nose's
  border joins ([`layout.md`](layout.md), "Acid2").
- The top layer ([`layout.md`](layout.md), "The top layer") holds modal
  dialogs only: popovers and fullscreen elements are not in it. Several
  modal dialogs are painted in tree order, not in the order they were
  opened. A modal dialog removed from the document stays modal, and
  the rest of the document is not made inert (clicks outside the
  dialog still reach the page). A modal dialog with `position: static`
  is not treated as absolutely positioned.

## Fonts

- OpenType shaping of text (GPOS, and GSUB beyond emoji sequences), so
  no ligatures, kerning beyond `kern`, Arabic joining, Indic reordering
  or bidirectional text.
- CFF and CFF2 outlines.
- SVG, sbix and CBDT colour glyphs.
- COLR variations (version 1 draws at the default instance).
- A CJK font pack (see the fonts README for what one needs).
- CSS does not parse `font-tech()`, and layout passes only weight and
  style to the font list (`font-variation-settings` and
  `font-optical-sizing` are computed, in `AuStyleInhRare`).
- `webfont.c` still reads `unicode-range` from the sheet text, which it
  gets once, when the sheet is parsed; the parsed descriptor is there
  to replace that ([`css.md`](css.md)).
- A system font is read whole on first use (see [Memory](#memory)).

## CSS

- Targeted invalidation of child insertions and removals.
- `@container`, `@scope` and `@starting-style` are parsed and never
  apply.
- Preference changes (dark scheme, reduced motion) do not reach the
  device, so sheets deferred for them stay deferred.
- `getComputedStyle`: margins, insets and percentages in properties
  other than width and height are still computed values, `transform` is
  not a matrix, and the `border` and `font` shorthands serialize every
  part.

## Images

- Arithmetic-coded, lossless and 12-bit JPEG.
- Colour management (gamma, ICC profiles and colour spaces are
  ignored).
- border-image in layout.
- At 256 colours images band; nothing dithers them.

## SVG

- Filters, patterns, markers, textPath, foreignObject, multi-line text,
  animation.
- CSS basic shapes in `clip-path`.
- `mask: url(#id)` on HTML elements.
- One raster per SVG image URL (an image shown at two shapes on one
  page shares the larger raster).
- SVG documents in frames are shown as text (see [Frames](#frames)).

## Canvas

- WebGL and `bitmaprenderer` contexts.
- JPEG and WebP encoding (`toDataURL` and `toBlob` always give PNG).
- `filter`, `fontKerning`, `fontStretch`, `fontVariantCaps`,
  `textRendering` and `imageSmoothingQuality` are accepted and not
  applied.
- Colour glyphs (emoji draw as outlines).
- Bidirectional text (`direction` only turns `start` and `end` round).
- Shadow offsets are rounded to whole pixels and the blur is capped at
  256.
- A pattern taints when it is chosen, not when it is drawn.
- `createImageBitmap` options.
- Video frames and SVG `image` elements as sources; a canvas of another
  document as a source draws nothing.
- `object-fit` on a canvas.
- Workers (`transferControlToOffscreen`).
- Context loss events.
- Layout measures text word by word, so it applies no kerning between a
  space and its neighbours; `measureText` kerns the whole string.
- html5test.co (2026-11-15) counts as missing: JPEG, JPEG-XR and WebP
  export, hit regions and the bitmap renderer context.

## JavaScript

- **Range:** `normalize()` moves points in merged text nodes to their
  parent.
- **Selection:** script's selection is not joined to the selection the
  user makes with the mouse, and it is not painted
  ([`javascript.md`](javascript.md), "Selection").
- **CSSOM:** constructed sheets and `adoptedStyleSheets`, the sheets of
  `@import` rules, rules nested in a style rule, `@font-face` rules
  added through the CSSOM (they are not given to the web font loader).
  `cssRules` is not live.
- **crypto.subtle:** only `digest` and HMAC; AES, RSA, ECDSA, PBKDF2
  and HKDF are what pages ask for next.
- **Console:** a source position for `console.log` lines (only errors
  carry a stack).
- **Start-up:** the prelude's CSSOM, Range and observer parts are
  created in every context (see [Memory](#memory)).
- **`document.referrer`** is `""` in frames (a top-level document has it).
- **IndexedDB, the Cache API and service workers** are absent; what each
  would take is in [`web-storage.md`](web-storage.md).
- **`document.cookie`** in a sandboxed frame without
  `allow-same-origin` reads `""` and sets nothing; the standard throws
  SecurityError ([`networking.md`](networking.md), "Cookies and
  `document.cookie`").
- **The compile-once cache** does not cover module scripts, a script a
  worker loads with `importScripts`, a script that is mostly data, or a
  script the worker compiled before the page did
  ([`javascript.md`](javascript.md), "How script memory is kept down").
- **An out-of-memory script** gets a console line and no page-level
  notice.
- **Intl** ([`javascript.md`](javascript.md), "Intl") formats every
  locale as en-US: other languages, numbering systems and calendars are
  not there, and `resolvedOptions().locale` says `en-US`.
  - Time zones: the local zone is named by its offset (`Etc/GMT+8`,
    `+05:30`) because Mac OS 9 has no zone database, so its short name
    is `GMT-8`, not `PST`. 67 named zones are known, with their
    current daylight saving rules applied to every year; any other IANA
    name throws RangeError. Long zone names exist only for UTC, the US
    zones and London.
  - `formatRange` repeats the shared fields ("Jan 1 – Jan 5", where
    Chrome writes "Jan 1 – 5"); `NumberFormat.formatRange` is a plain
    "a–b".
  - `Collator` orders by folded code point within each class (spaces,
    punctuation, digits, letters), not by the full Unicode collation
    table; punctuation among itself, ligatures and `ß` sort by code
    point.
  - `Segmenter` follows a few rules of UAX 29 (combining marks, ZWJ
    sequences, flags; words with apostrophes, periods and digit groups;
    sentences end at `.`, `!`, `?`): no dictionary for Chinese, Japanese
    or Thai words, and "3.14" ends a sentence at the period.
  - `DisplayNames` knows about 30 languages, 35 regions, 11 scripts and
    the currencies `NumberFormat` has symbols for. `Intl.DurationFormat`
    is absent, and the `dayPeriod` option knows five periods.
  - A sort with `localeCompare` costs about 0.7 microseconds a
    comparison on the host (2000 strings in 15 to 20 ms), 15 to 20 times
    a plain `<` comparison; not measured on a G4.

## Script requests and CORS

- A `fetch` with a `ReadableStream` body reads the whole stream before
  it sends anything (a `Content-Length` request, not a chunked upload),
  so a stream that never ends never sends.
- A body read with `text()` and the like, or a clone nobody reads, is
  not paused while it arrives; only a body read through its stream
  pauses the request ([`script-requests-and-cors.md`](script-requests-and-cors.md#streams-and-streaming-bodies)).
- `EventSource` keeps at most four connections per document or worker;
  a fifth source waits. A stream silent for 60 seconds (the loader's
  idle timeout) or past 64 MB reconnects. After Back or Forward to a
  kept document its sources reconnect without an `error` event.
- `TextDecoderStream` decodes UTF-8, UTF-16 and windows-1252 only, the
  encodings `TextDecoder` knows.
- XML for `responseXML`; `responseType` `document` parses HTML only.
- A `keepalive` request does not outlive its document.
- `blob:` URLs work in `fetch`, `XMLHttpRequest`, `Worker` and
  `importScripts` but not as an image source or a download link.
- A synchronous `XMLHttpRequest` to a network URL throws NetworkError
  in a document.
- An image made by script and never put in the document is fetched
  without CORS.

## Workers

- `SharedWorker` and `ServiceWorker` are not defined.
- A worker cannot start workers.
- Without coroutines a long task blocks the page and nothing can wait
  for the network.
- No canvas in workers (`transferControlToOffscreen`).

## Frames

- XML documents in frames (`text/xml`, `application/xhtml+xml`,
  `image/svg+xml` are shown as text; the XML parser in `src/svg` builds
  SVG trees only).
- Session history for frame navigations.
- A nested document's own Referrer-Policy and the `referrerpolicy`
  attribute (the top-level policy is used).
- Frame scroll bars cannot be dragged (wheel and keys only).
- `rows` and `cols` sizes of a frameset.
- Find searches one document (the page, or the frame the selection is
  in) and Tab does not move between a page and its frames.
- Invalidation rectangles of a frame inside a transformed or scrolled
  container use the frame's untransformed box.
- The WindowProxy of a frame is a new object for each document,
  including the first real document after the initial `about:blank`:
  a reference taken earlier is not equal to `frames[i]` or
  `contentWindow` read again, still shows the blank document, and does
  not receive messages for the new window ([`frames.md`](frames.md),
  "Script").
- POST into a frame works only with a loader that implements
  `request_ex`.
- No CSP directive other than `frame-ancestors` is enforced anywhere
  (`script-src`, `frame-src`, `sandbox`, `default-src` and the rest are
  ignored), and neither is the `Permissions-Policy` header or the
  `allow` attribute. Sandbox tokens other than `allow-scripts`,
  `allow-forms`, `allow-same-origin`, `allow-top-navigation` and
  `allow-popups` are ignored.

### Acid3

The score is 87 ([`frames.md`](frames.md) says how to run it). Still
failing:

| Test | Cause |
|---|---|
| 2 | a NodeIterator whose filter removes nodes |
| 21, 23, 25 | namespace prefixes: `tagName` with a prefix, `createElementNS` name checks |
| 35 | `:first-child` matches a root without a parent |
| 46 | a media query case |
| 69, 74, 79 | XML and SVG documents in frames: they are shown as text, not parsed into a DOM |
| 72 | `cssRules` is not live |
| 75, 77 | SVG DOM interfaces |
| 80 | script in an XHTML document |

## Forms

- Date, time and color pickers (they are text fields).
- The dirname attribute's direction.
- A resend prompt for POST history entries.
- Form-associated custom elements.
- IME input.

## Networking and TLS

- IPv4 only on Mac OS 9.
- No HTTP/2.
- No proxy auto-configuration.
- Sites that offer only DHE, or only ciphers other than AES-GCM and
  ChaCha20-Poly1305 (CBC suites), fail with "Secure connection failed".
- No post-quantum key exchange, no revocation checking, no client
  certificates.
- The platforms give no DNS TTL: an answer is used for 5 minutes.

## Content blocking

- Lists per element or URL path (cosmetic and path rules are ignored).
- IDN mapping beyond the URL parser's.
- Removing the compiled file of a catalogue list that was turned off.

## Developer tools

- A source position for `console.log` lines (only errors carry a
  stack).
- A time for each console line (the page stores none).
- A case-sensitive find in the developer windows.
- A horizontal scroll bar: the Network window cuts URLs to fit instead,
  and the other windows wrap.
- Request bodies and cookies in the Network details.
- Saving the request log to a file (Copy All copies it).
- Request headers appear only for loads made after the Network window
  first opened.
- The developer windows are not part of the session: they are not
  reopened at the next launch, only placed where they were.
- The timing test and the paint benchmark exist only in the Mac shell.
- `README.md` does not mention Report a Problem (`CONTRIBUTING.md`
  does).

## Memory

Largest first.

1. **Parsed style sheets.** What is left of them, for GitHub's home
   page with 32-bit pointers: 2.4 MB (1.3 MB of rules and selectors,
   0.6 MB of packed text, 0.4 MB of token lists;
   [`memory.md`](memory.md)).
   - Each document parses its own sheets: a second tab, a frame or a
     kept document of the same site holds them again.
     `au_css_sheet_retain` already lets owners share a sheet; a table
     of parsed sheets by URL, media and text would let documents share
     them (not sheets with `@import` rules, whose imports are attached
     to the sheet).
   - The texts are packed by `au_docview_update`, so a document that
     stops being updated before its texts are packed (hidden or kept
     within seconds of loading) keeps them as they arrived. The page
     layer can call `au_docview_pack_sheets` when a document goes
     cold.
   - The tokens of a whole sheet exist while it is parsed, then are
     freed: 3.7 MB for GitHub's 800 KB sheet, 1.6 MB for Wikipedia's
     212 KB one (20 bytes a token, and arrays that grow by doubling).
     Parsing rule by rule would keep that to one rule's tokens.
   - A custom property map is flattened every 12 levels
     (`AuCustomProps.base`): GitHub's 2157 properties give 23 full
     maps, 0.39 MB with 32-bit pointers.
2. **Atoms live for the life of the program:** every class, id and
   custom property name of every page visited stays (270 KB for the
   article, 720 KB for GitHub), so a long session grows. Atoms need a
   count or a per-document table.
3. **`LBlock`** is 264 bytes on the host (about 216 on PowerPC) and
   every inline box is one; an inline box needs under half of it.
4. **`AuElement`** carries `template_content`, `shadow_root` and
   `control`, which almost no element uses (24 bytes of 160 on the
   host, 12 of about 92 on PowerPC).
5. **The prelude is created in every context** (1.4 MB per document,
   0.6 MB per worker). Its classes and prototypes must be per realm
   (`frame.contentWindow.Node !== Node`), and a function's bytecode
   belongs to one realm, so only the 109 KB of prelude bytecode and its
   function records could be shared, and only by teaching QuickJS to
   share bytecode between realms. The cheaper step: create the CSSOM,
   Range and observer parts (a quarter of its source) on first use,
   which means splitting the single prelude function and would save an
   estimated 150 to 250 KB per context.
6. **Compiling a script** holds about 4 MB beyond its result for an
   860 KB script (a record per function and all first-pass bytecode
   until the script ends). That is how QuickJS resolves variables
   ([`memory.md`](memory.md), "Where script memory goes").
7. **Line tables are kept** (153 KB per document for that script, 3%).
   Error locations in the Console depend on them.
8. **The pooled allocator's slabs** are `NewPtr` blocks that do not
   move: one long-lived block keeps a 32 KB slab, and the largest free
   block after closing a large page was 42 MB of 60 MB free.
9. **A system font is read whole on first use** (into temporary
   memory); reading only the tables used needs a different font data
   interface.

Also: a document estimated at more than 24 MB is not kept for Back (a
very long Wikipedia article is over 30 MB), and scripted frames cost
1.4 MB each against the page's script memory limit, so about 5 fill the
8 MB a Mac with little free memory gets
([`javascript.md`](javascript.md), "Script memory limits").

### Cold memory: what is not done

Kept documents free their layout, background tabs are unloaded, and the
page source and the HTTP cache hold compressed bytes
([`memory.md`](memory.md), "Cold memory"). These remain, with what each
would save for a Wikipedia article (host figures, 2026-11-15):

1. **Script source text** kept for `Function.prototype.toString` (992 KB
   in 8 texts for an article). Deflated it would be about 300 KB, a
   saving of 0.7 MB per document. It needs a QuickJS patch:
   `JSFunctionBytecode.source` is a pointer into `JSSourceText.buf`
   (patch 0008), read by `js_function_toString` and the bytecode dump,
   and patch 0008 finds a script's text again by comparing its bytes.
   The pointer would become an offset, the readers would inflate
   through a hook the embedder sets (QuickJS cannot call `src/codec`),
   and the content lookup would compare length and hash first. No hot
   path reads the text.
2. **Layout of the parts of a visible page that are off screen.** The
   box tree is one arena, so freeing part of it needs per-subtree
   arenas or a layout that builds lines only for blocks near the
   viewport. An article's layout is 2.5 MB and most of the page is off
   screen; how much could go was not measured (an estimate: 1 to 2 MB
   per visible article). Scrolling would then lay out as it goes.
3. **Layout of background tabs.** A background tab still runs its
   timers, so its layout is rebuilt whenever script asks for geometry;
   freeing it when the tab has been in the background for a while (as
   a kept document does) saves 0.9 to 2.5 MB per tab at the cost of a
   25 to 30 ms rebuild (host) on return.
4. **Function bytecode that has not run for a while** (464 KB per
   article, with 72 KB of line tables). QuickJS would have to compile a
   function again from its source text on the next call, which it has
   no path for; with item 1 the text would also have to be inflated
   first. About 0.4 MB per document, and the riskiest of these.
5. **The prelude per context** (1.7 MB for each document context, frames
   included). Its bytecode is shared; what each context holds is the
   objects the prelude creates (constructors, prototypes, 500 C
   functions). Creating the CSSOM, Range and observer parts on first
   use (item 5 of the list above) saves 150 to 250 KB; creating
   interface objects on first lookup of their global name would save
   perhaps half of the 1.7 MB for a frame that uses few of them.
6. **Atoms** (item 2 of the list above): 873 KB of script atoms per
   runtime go with the runtime, but the engine's own atoms (270 KB per
   article visited) never go.
7. **Free space inside the script heap's arenas** (1.9 MB of 12.8 MB
   after an article loads): a kept or background document cannot give
   it back while live blocks share its slabs.
8. **The page source and the cache entry are two compressed copies** of
   an article's HTML (180 KB each); the page could hold a reference to
   the cache entry instead when there is one.
9. **Image bytes are held twice** while a document is loaded: by the
   image cache (to decode again) and by the HTTP memory cache. With the
   disk cache on the HTTP cache keeps 1 MB resident, which bounds it
   there; the host tool holds both in full.
10. **Text selection is not kept across Back** (it never was: a
   navigation clears it).

## Fuzzing

What no fuzzer covers is listed in [`fuzzing.md`](fuzzing.md), "Not
fuzzed".

## Performance

- Keys typed while a page changes its DOM pay for a box tree build (see
  [Layout](#layout) and [`forms.md`](forms.md)).
- Loading a Wikipedia article in the guest took 30 to 70 seconds
  ([`browser-chrome.md`](browser-chrome.md)).
- A window zoom in the guest costs two passes of about 280 ms each on a
  large article ([`layout.md`](layout.md)).
