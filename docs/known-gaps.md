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
| Text without ligatures, wrong order of Arabic or Hebrew, rare CJK characters missing | [Fonts](#fonts) |
| A layout that depends on style queries, scoped styles, subgrid or baseline alignment in grid | [CSS](#css), [Layout](#layout) |
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
  - Report a Problem and System Status on the stock base, where the
    Enforcer line should read "not installed", and on any real Mac.

## Layout

- BBC News (bbc.com/news) at 400 pixels is 1291 wide where Chrome's
  fits (2026-11-17): a card grid's implicit `auto` column takes the
  max-content width of a description paragraph (994 pixels on one line)
  through nested grids of `1fr` columns whose items have
  `max-width: calc(100% - 32px)`; Chrome keeps the column at the
  container's width. A page built from those rules alone lays out as in
  Chrome, so the case still needs reducing.
- Text is segmented and measured again for each layout of its block
  (the word cache answers the widths).
- `l_layout_abspos_for` removes queue entries with `memmove`.
- A DOM change still builds the whole box tree when the change uses a
  counter or a quote from outside its subtree (a numbered heading, a
  `<q>`, a `<summary>`), when the nearest block that can be rebuilt is
  the root element, when more than 64 separate places change between
  two updates,
  or when the rebuilt box changes its outer kind ([`layout.md`](layout.md),
  "A DOM change").
- A relayout after a viewport change or a style change that keeps the
  boxes differs from a layout from scratch in one case, so the
  differential check compares geometry only while no such relayout has
  run since the last whole build ([`layout.md`](layout.md), "A relayout
  must give what a layout from scratch gives"): overflow and the
  document size are found before relative offsets are applied, and a
  relayout that reuses content keeps the offset positions. With geometry
  forced on (2026-11-16), `fuzz_page` seeds 24 and 26 find it, reduced
  to `<table><td style="position:relative;display:grid;top:1px"><caption><a href><center><table><td><ul>`
  (a row group's overflow is 2 pixels tall from scratch and 3 after a
  relayout); seeds 7, 21, 22, 41, 44 and 47 run clean for 150 s each. To
  fix it, apply each offset to the overflow of the boxes that contain
  it in `relative_pass`. The percentage heights in table cells that also
  differed are fixed ([`layout.md`](layout.md)).
- After a subtree rebuild, layout lays out again every block on the path
  to the root and the in-flow siblings that do not establish a block
  formatting context, not only the rebuilt subtree.
- Images: `currentSrc` in script, `image-set()`, and `sizes` lengths
  other than px, vw, vh, em and rem.
- Grid: `subgrid` (parsed; laid out as `none`; `@supports` says no),
  baseline alignment of
  items (they align at the start), `direction: rtl` and vertical
  writing modes, `safe` overflow alignment, painting items in `order`
  order, `masonry`. An item's min-content and max-content heights are
  both its height at its used width.
- `frameset` `rows` and `cols` sizes are not read.
- Shadow trees ([`layout.md`](layout.md), "Shadow trees";
  [`css.md`](css.md), "Shadow trees and scoped styles"):
  - Tab order walks the document in shadow-including tree order; the
    standard orders each shadow tree as a scope of its own, so a
    positive `tabindex` inside a shadow tree is ordered with the whole
    document's.
  - A click on a host whose shadow root delegates focus focuses the
    host's nearest focusable ancestor, not the first focusable element
    of the shadow tree (script's `focus()` does delegate).
  - A change to a host's children, a slot's `name` or a node's `slot`
    restyles the host's whole flat subtree and rebuilds its boxes, not
    only the slots whose assigned nodes changed.
  - `adoptedStyleSheets` rejects a sheet constructed in another frame
    with a TypeError; the standard asks for NotAllowedError.
  - `getHTML()`, `serializable` roots and `ElementInternals` are not
    done.
  - A slot of a declarative shadow root that takes nodes while the
    page parses fires `slotchange` once, after the first mutation
    script makes; browsers fire it as the parser assigns them.
- Acid2 still differs in four rows of the eyes and along the nose's
  border joins ([`layout.md`](layout.md), "Acid2").
- The top layer ([`layout.md`](layout.md), "The top layer") holds modal
  dialogs and popovers; fullscreen elements are not in it. Popovers and
  `popovertarget` buttons need the script runtime, so they do nothing
  with script turned off. `hint` popovers share the auto popovers'
  stack, popover anchoring (`anchor`, implicit anchors) and the
  `interestfor` and `commandfor` attributes are not done, and a
  popover whose `popover` attribute changes while it shows hides
  without `beforetoggle` and `toggle`.
- Inert content ([`layout.md`](layout.md), "Inert content") is still
  painted with its old `:hover` until the pointer moves, a focused
  element that becomes inert through the `inert` attribute keeps the
  focus (no focus fixup), and a selection made before a modal dialog
  opened keeps its range (only its inert part stops painting).
- A stacking context that clips its overflow clips every positioned
  descendant, in painting and in hit testing, including one whose
  containing block is outside it (an absolutely positioned box under a
  static `overflow: hidden; opacity: .5` box); CSS clips only the
  descendants whose containing block chain passes through the clipping
  box ([`layout.md`](layout.md), "Hit testing follows the paint order
  and its clips").

## Fonts

- OpenType shaping of text (GPOS, and GSUB beyond emoji sequences), so
  no ligatures, kerning beyond `kern`, Arabic joining, Indic reordering
  or bidirectional text.
- CFF and CFF2 outlines.
- SVG, sbix and CBDT colour glyphs.
- COLR variations (version 1 draws at the default instance).
- Regional ideograph shapes: Japanese, Korean and traditional Chinese
  text uses Noto Sans SC's ideographs, and the fallback does not read
  `lang`. CJK characters outside the bundled sets (Big5 level 2, the
  hangul syllables beyond KS X 1001, CJK extension blocks) are missing
  unless the system has a face for them. Vertical writing is not drawn.
- CSS does not parse `font-tech()`, and layout passes only weight and
  style to the font list (`font-variation-settings` and
  `font-optical-sizing` are computed, in `AuStyleInhRare`).
- `webfont.c` still reads `unicode-range` from the sheet text, which it
  gets once, when the sheet is parsed; the parsed descriptor is there
  to replace that ([`css.md`](css.md)).
- A system font is read whole on first use (see [Memory](#memory)).

## CSS

- CSS animations play only in part ([`css.md`](css.md), "Animations"):
  an element's first animation, when it repeats forever, and only its
  `transform` and `opacity` keyframes. A finite animation, any other
  animated property, per-keyframe timing functions and transitions do
  not play (a transition jumps to its end). Inline boxes do not
  animate.
- `@scope` and `@starting-style` are parsed and never apply.
- `accent-color`, `caret-color`, `overscroll-behavior`,
  `scrollbar-gutter`, `content-visibility`, `field-sizing` and
  `interpolate-size` are parsed only ([`css.md`](css.md), "Properties
  parsed only"). Scroll snapping does not follow a scroll bar drag or
  `window.scrollTo`, and does not snap again after a relayout
  ([`layout.md`](layout.md), "Scroll snapping"); the logical
  `scroll-padding-*` and `scroll-margin-*` longhands are not parsed.
  Relative colours work in `rgb()` and `hsl()` but not in
  `lab()`, `oklch()` or `color()`.
- Container queries ([`css.md`](css.md), "Container queries"):
  - `style()` and `scroll-state()` queries are unknown, so a rule that
    needs one to be true does not apply.
  - The list form of `@container` (several queries separated by commas)
    is an invalid prelude and the rule is dropped.
  - Container names are not tree-scoped: a name set in a shadow tree
    can be found from outside it, and the reverse.
  - `container-type` does not imply style containment, so counters and
    quotes inside a container still affect the page outside it.
  - In a condition, `cqi`, `cqb`, `cqmin` and `cqmax` resolve against
    the viewport.
  - The document view stops after four rounds of query, restyle and
    layout ([`layout.md`](layout.md), "Container query rounds"); a
    deeper chain of dependent containers keeps the previous round's
    styles.
  - Size containment does not apply to tables or replaced elements
    (images, form controls), and with no vertical writing modes in
    layout, `inline-size` always contains the width.
- `prefers-reduced-motion` is always `no-preference`: no preference or
  system setting sets it.
- `light-dark()` always gives its light value, and the `color-scheme`
  property is not implemented, so a page cannot opt its form controls
  and scroll bars into dark; only `prefers-color-scheme` follows the
  Appearance preference.
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

- Filters, patterns, markers, textPath, multi-line text, animation.
- `foreignObject` draws only the first HTML `<img>` in it
  ([svg.md](svg.md#inline-svg)); other HTML in it is not laid out.
- `clip-path`: `path()`, `rect()`, `xywh()` and `url(#id)` do not clip
  (the box paints unclipped), `polygon` ignores its fill rule, `inset`
  takes only the horizontal radii, and no clip-path clips hit testing.
- The `clip` property clips only the painting of the absolutely
  positioned box and its in-flow content: positioned descendants of it
  paint unclipped, it does not clip hit testing, and a box with both
  `clip` and `clip-path` takes only `clip-path`.
- `mask: url(#id)` on HTML elements.
- `border-radius` does not clip the content of a canvas, an iframe or an
  `<img>` showing an SVG (raster images are clipped).
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

The ranked list of missing web platform features is
[`web-platform.md`](web-platform.md); this section lists what is partly
done.

- **Custom elements:** `:state()` never matches (the selector parses;
  `ElementInternals.states` is kept in script only); a form-associated
  element's File value is not submitted, and `formStateRestoreCallback`
  and `formDisabledCallback` are never called.
- **Pointer events:** the mouse only (pointer 1); there are no touch or
  pen pointers, `getCoalescedEvents()` holds the event itself, and
  `offsetX`/`offsetY` ignore CSS transforms. `screenX`/`screenY` equal
  `clientX`/`clientY`.
- **IndexedDB:** databases live in memory per document or worker; a
  reload or another tab of the site starts empty, and nothing is saved
  to disk. `DOMStringList` is not a global in workers.
- **Shims within one document:** `navigator.locks` and
  `BroadcastChannel` reach only the document that made them, not other
  tabs or frames.
- **Range:** `normalize()` moves points in merged text nodes to their
  parent.
- **Selection:** script's selection is not joined to the selection the
  user makes with the mouse, and it is not painted
  ([`javascript.md`](javascript.md), "Selection").
- **CSSOM:** the sheets of `@import` rules, `@font-face` rules
  added through the CSSOM (they are not given to the web font loader).
  `cssRules` is not live. Declarations after a nested rule are not a
  separate `CSSNestedDeclarations` rule. A rule's `style` gives colour
  keywords as `rgb()` where other browsers keep the keyword.
- **crypto.subtle** ([`javascript.md`](javascript.md), "crypto.subtle"):
  - Ed25519, Ed448 and X448: TF-PSA-Crypto has no Edwards curves.
  - RSA-PSS signs only with a salt as long as the hash (shorter when
    the key has no room), which is what the library does; another
    `saltLength` rejects with NotSupportedError, and verifying with one
    accepts any salt length.
  - RSA keys over 4096 bits or not in whole bytes, multi-prime keys,
    and private JWKs without the CRT members (`p`, `q`, `dp`, `dq`,
    `qi`); generation below 1024 bits.
  - HMAC keys whose length is not a whole number of bytes; compressed
    EC points.
  - A `CryptoKey` cannot be cloned (`structuredClone`, `postMessage`):
    it throws DataCloneError.
  - PBKDF2, RSA signing and decryption and EC key generation run on the
    caller's stack: a PBKDF2 of hundreds of thousands of iterations
    stops the page while it runs (only RSA key generation yields).
  - Whether a context is secure is decided when it starts.
- **Console:** a source position for `console.log` lines (only errors
  carry a stack).
- **Start-up:** the prelude's CSSOM, Range and observer parts are
  created in every context (see [Memory](#memory)).
- **`document.referrer`** is `""` in frames (a top-level document has it).
- **Web Animations** (`Element.animate`, `Animation`,
  `document.getAnimations`) run on the clock and fire `finish`,
  `cancel` and their promises, but change nothing on screen: keyframes
  and easing are not applied, and `fill: forwards` leaves the element's
  own style. Before they existed, GitHub's landing page threw in a
  React ref and remounted a section about 600 times (150 MB of script
  heap, every image fetched 22 times).
- **XPath** (`document.evaluate`, `XPathResult`) is absent; a call
  throws "not a function".
- **`document.fonts`** is a set that is always loaded (`ready`
  resolves at once, `check` is true); a `FontFace` made by script
  cannot load and its `load()` rejects with NetworkError.
- **The Cache API and service workers** are absent; what each would
  take is in [`web-storage.md`](web-storage.md).
- **Audio and video** do not play. `play()` rejects with
  NotSupportedError and `canPlayType` answers `""`, but `MediaError`,
  `TimeRanges`, the track lists and the read-only state exist so that
  players load and show their own controls (Reddit's player reads
  `MediaError` when its bundle runs). A `<video poster>` shows the
  poster as its image; a video without one is an empty box.
- **`document.cookie`** in a sandboxed frame without
  `allow-same-origin` reads `""` and sets nothing; the standard throws
  SecurityError ([`networking.md`](networking.md), "Cookies and
  `document.cookie`").
- **The compile-once cache** does not cover module scripts, a script a
  worker loads with `importScripts`, a script that is mostly data, or a
  script the worker compiled before the page did
  ([`javascript.md`](javascript.md), "How script memory is kept down").
- **A script of 2 MB or more keeps no source text**: its functions'
  `toString` gives the native function form with the function's kind,
  and a class gives `class C { }`. A page that reads a function's text
  from such a script gets the placeholder
  ([`javascript.md`](javascript.md), "How script memory is kept down").
- **YouTube's home page** runs within the 64 MB limit with 2 MB to spare
  (62.7 MB peak on the host, all of it at the end of compiling the
  9.8 MB bundle); a larger bundle would run out while compiling. Two
  "not a function" errors remain (`playerApi`). In the guest it runs
  with the Automatic limit (96 MB; peak 45 to 51 MB), but the machine
  takes no events for 4 to 6 minutes while the bundle compiles
  ([`javascript.md`](javascript.md), "Large scripts").
- **A module graph that fails to load** (an import that cannot be
  fetched, or one with a syntax error) keeps the modules it compiled
  until the document goes: unresolved modules stay loaded so that the
  next round does not compile them again ([`javascript.md`](javascript.md),
  "Module scripts").
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
- `WebSocket` offers no extensions (no `permessage-deflate`), so
  messages arrive uncompressed. `bufferedAmount` drops to 0 at the next
  task, not as bytes leave. A message over 4 MB fails the connection.
  A document kept for Back and Forward closes its sockets.
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
- An `<img>` the parser made (or whose `src` attribute script set) loads
  only once layout gives it a box, so an image under `display: none`
  never loads, fires no `load` or `error` event, and reads
  `naturalWidth` 0. Its events can also fire after the window's `load`
  event, and `complete` is true while it loads.
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
- Slashdot's Cloudflare rule challenges every request whose user agent
  says `PPC Mac OS 9`, even with a solved challenge's cookie, so Mac OS
  9 cannot read Slashdot. Aura does not send another platform's string
  to get past it ([`networking.md`](networking.md), "The user agent").

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

- reCAPTCHA's invisible badge on Reddit holds about 5 MB in its frame's
  script and 6 MB in its worker on the 64-bit host (2026-11-18), each
  peaking near 20 MB while it runs and collected afterwards. What stays
  is live data: only engine-level cuts to objects and functions would
  shrink it.

1. **Parsed style sheets.** What is left of them, for GitHub's home
   page with 32-bit pointers: 2.4 MB (1.3 MB of rules and selectors,
   0.6 MB of packed text, 0.4 MB of token lists;
   [`memory.md`](memory.md)).
   - Documents share a sheet only while one holds it
     ([`css.md`](css.md), "Sheets shared between documents"): a page
     loaded after the last document with the sheet went parses it
     again. A sheet with `@import` rules is never shared (its imports
     are attached to it), nor are the imported sheets' parents, the
     user sheet, constructed sheets and the sheets of SVG images.
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
2. **Atoms that outlive their page.** A page's atoms go with its view
   ([`memory.md`](memory.md), "Atoms"), except:
   - atoms that code outside the page's entry points interns or looks
     up while a page's atom is live (the browser layer reading a
     document, the host tools) become permanent;
   - with 31 top-level documents alive (tabs and kept documents), the
     32nd and later get no scope and their atoms are permanent;
   - pages that share a style sheet each hold all its atoms, so a
     sheet's names live while any of them does.
   QuickJS's atoms belong to the page's runtime and go with it; none
   grew over ten pages.
3. **`LBlock`** is 264 bytes on the host (about 216 on PowerPC) and
   every inline box is one; an inline box needs under half of it.
4. **`AuElement`** carries `template_content`, `shadow_root` and
   `control`, which almost no element uses (24 bytes of 160 on the
   host, 12 of about 92 on PowerPC).
5. **The prelude is created in every context** (1.3 MB in use per
   document, 0.6 MB per worker). Its classes and prototypes must be per
   realm (`frame.contentWindow.Node !== Node`), and a function's
   bytecode belongs to one realm, so only the prelude bytecode and its
   function records could be shared, and only by teaching QuickJS to
   share bytecode between realms. Range, the CSSOM and the canvas are
   read on first use (164 KB less per context,
   [`javascript.md`](javascript.md), "Modules read on first use"); the
   observers, XMLHttpRequest and the top layer are the next largest
   parts, and most script-heavy pages use the observers.
6. **Compiling a script** holds a record per function and all
   first-pass bytecode until the script ends, because that is how
   QuickJS resolves variables: about 4 MB beyond its result for an
   860 KB script before patch 0013, and 30 MB beyond it for YouTube's
   9.8 MB bundle after it ([`memory.md`](memory.md), "Where script
   memory goes").
7. **Line tables are kept** (153 KB per document for that script, 3%).
   Error locations in the Console depend on them.
8. **MDN's CSS page leaks** about 40 KB each time it closes: seven
   computed styles with their groups, custom property maps
   (`cascade.c`) and token lists (`tokenizer.c`), and the text of three
   text nodes (`dom.c`), still allocated after the browser, the network
   stack and the cache are freed. To see them, run `AURA_MEM_EXIT=1
   AURA_MEM_MIN=1 aura-host chrome --net --tab about:blank --do
   newtab:https://developer.mozilla.org/en-US/docs/Web/CSS --do
   wait:8000 --do closetab` in a tree built with `-DAURA_MEM_TRACK=ON`
   ([`memory.md`](memory.md), "To see where blocks sit in the Mac's
   heap"). The blocks also split the free space the page leaves. The
   cause was not found: the retains and releases of styles and groups
   in `cascade.c`, `style.c` and `layout.c` pair up as read.
9. **Pages share the page zone.** The long-lived zone keeps the
   network stack, the cache, fonts and atoms out of the space a page
   leaves ([`memory.md`](memory.md), "Zones"), but two pages open at
   once (two tabs, or a kept document and the page shown) take their
   slabs and large blocks from the same free space, so closing one
   leaves holes among the other's blocks until it goes too. A region
   per document would keep them apart.
   - A block is in the zone of the file that allocates it; a helper
     outside `src/core`, `src/codec` and `src/url` that allocates for
     long-lived callers puts their blocks in the page zone unless the
     caller holds the zone. The heap map finds such blocks.
   - With no temporary memory free, long-lived blocks come from the
     application heap, among a page's, as before the zones.
   - The long-lived zone's temporary memory (7.7 MB holding 4.7 MB of
     blocks in the guest after GitHub, YouTube, Wikipedia and ten
     pages; mostly the HTTP cache, TLS and fonts) is outside the
     partition that Get Info sets, as font data and the blocklists are.
   - After the ten pages the application heap's largest free block was
     53.6 MB of 60.7 MB free in the guest: what is left splits it.
10. **Mac OS 9.2.2 can hang in the Memory Manager** when temporary
    memory grows past its first zone, shrinks and grows again. Aura
    keeps the first handle freed in each extra zone (shrunk to 16
    bytes, up to 256), and with that the sequences that hung in the
    guest ran ([`memory.md`](memory.md), "Temporary memory areas"). The
    cause inside the system was not found; another program's
    temporary memory, or more than 256 zones, could still meet it, and
    it has not been tried on real hardware or on the stock base.
11. **A system font is read whole on first use** (into temporary
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

## The Linux GTK4 port

What the GTK4 port does not do yet is listed in [`gtk.md`](gtk.md),
"Known gaps".

## Fuzzing

What no fuzzer covers is listed in [`fuzzing.md`](fuzzing.md), "Not
fuzzed".

## Performance

- Of the work that holds the window, only script and style passes
  park. Layout, image decoding and HTML parsing run to their end.
- While a page is parked, a click or a typed key reaches the page only
  when the slice ends (16 are kept; past that the slice runs to its end
  first), and dragging does not select (as in Chrome, whose selection
  waits for a long task too). The page shows the layout of up to
  250 ms before, or longer on a document whose layout is slow.
- Loading a Wikipedia article in the guest took 30 to 70 seconds
  ([`browser-chrome.md`](browser-chrome.md)).
- A window zoom in the guest costs two passes of about 280 ms each on a
  large article ([`layout.md`](layout.md)).
- **Compiling script** is about 45% of the script time of GitHub's
  repository page on the host (5.1 MB of modules, 245 ms): QuickJS
  compiles every function of a script when it loads, and nothing keeps
  compiled scripts from one page load to the next, although GitHub's
  pages load the same bundles each time
  ([`performance.md`](performance.md), "Script on GitHub's repository
  page").
- **Blocks over 16 KB on Mac OS 9** are still a `NewPtr` each (about
  1,800 of GitHub's 45,600 blocks over 2 KB, 200 live at once); the
  4 KB script arenas and the other blocks up to 16 KB come from the
  pool's slabs since 2026-11-16 ([`memory.md`](memory.md), "Blocks
  over 2 KB"). Classes up to 64 KB would remove most of the rest at the
  cost of more memory pinned by long-lived blocks.
- **The pool rounds blocks over 2 KB up to their class** (up to 25%),
  and long-lived blocks pin slabs: GitHub's page took about 1.4 MB
  more heap in the guest with the classes, and its closed tab left
  0.8 MB less free and a largest free block 1.2 MB smaller
  ([`performance.md`](performance.md), "The pooled allocator's classes
  over 2 KB").
- **Forced layout from script** remains where script asks for geometry
  (`getBoundingClientRect`, `offsetWidth`, `getComputedStyle` widths
  through `N.rect`); on GitHub's page after the changes of 2026-11-16 it
  is about 3% of script time on the host.
