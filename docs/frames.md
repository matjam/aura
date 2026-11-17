# Frames

This document covers nested documents: `iframe`, `frame`, `object` and
`embed` elements, how they load, paint and take input, framing rules,
the sandbox, script access between documents, and lifetimes. Read it
before changing `src/page/frames.c` or `src/js/frames.c`, or anything
that frees a document while script may run.

`iframe`, `frame`, `object` and `embed` elements show nested documents.
To change them, work in:

- `src/page/frames.c`: the frame list, loading, painting, input,
  limits;
- `src/js/frames.c` and the "windows of other documents" part of
  `src/js/prelude_window.js`: script;
- the frame parts of `src/browser/page_doc.c`: fetch context,
  selection, hit information.

## To try it

Serve a page with frames from a local HTTP server and run `aura-host
chrome --net --tab URL --do wait:500 --do click:X,Y --do type:TEXT -o
out.png`; `--do js:CODE` reaches frames through `frames[0]`. `aura-host
page PAGE.html` loads file: frames too.

## Structure

Each frame element has an `AuFrame` in the view of the document that
holds it (`AuDocView.frames`, tree order). A frame that shows a
document owns an `AuDocView` for it, with its own document, style,
layout, images and script context. The view is created only when there
is content to show or script asks for the frame's window, so an empty
iframe costs one record.

`au_frames_sync` brings the list in step with the tree: after each
parser chunk, before a style pass, and at the end of any script native
that inserted, removed or changed a frame element.

## The embedder sees one view

Nested views load through the top-level view's loader and report
through its callbacks, always with the top-level view as `dv`.

- `au_docview_set_request_ex` gives the loader the requesting
  document's URL and a body (`AuResourceRequest`); data for any request
  is delivered to the top-level view, which passes it on
  (`FrameRoute`).
- Input goes to `au_docview_mouse`, `_key` and `_scroll_at` of the
  top-level view.
- `au_docview_view_at` and `au_docview_focus_view` name the nested view
  under a point or with the keyboard, for code that works on one
  document (the page's selection, hit information, the Edit commands,
  the caret).

## Loading

A frame's document is an `AU_RES_FRAME` request of the owner view
(`AU_FETCH_FRAME` in the page, `AU_DEST_IFRAME` in the net loader).

- The response head decides what the element shows: HTML or text is a
  nested document, an image is a generated document with an `<img>`
  (iframe) or an image box in layout (object, embed), anything else
  nothing.
- `srcdoc`, `about:blank` and `data:` URLs need no network;
  `page_doc.c` decodes `data:` URLs itself, for scripts too.
- A `srcdoc` or `about:blank` frame resolves URLs against its parent's
  base URL (`au_docview_begin`), so a relative script or image in a
  `srcdoc` document loads. (It used `about:srcdoc`, and they did not.)
- In `page_doc.c` the requesting document is the referrer and decides
  mixed content (an http frame in an https page is blocked by the net
  loader), and the tab's URL is the top-level site for cookies and the
  blocklist (a listed third-party frame host is not contacted and
  counts in `AuPageStats.blocked`).

## Framing rules

`framing_allowed`: `X-Frame-Options` DENY and SAMEORIGIN, and the CSP
`frame-ancestors` directive with `'none'`, `'self'`, scheme and host
sources, each checked against every ancestor; with `frame-ancestors`
present `X-Frame-Options` is ignored. A refused frame is empty and the
console names it.

No other CSP directive is enforced anywhere (`script-src`, `frame-src`,
`sandbox`, `default-src` and the rest are ignored), and neither is the
`Permissions-Policy` header or the `allow` attribute.

## Sandbox

The `sandbox` attribute turns off:

| Turned off | Unless |
|---|---|
| scripts | `allow-scripts` |
| form submission | `allow-forms` |
| the parent's origin (without it the document's origin is opaque, so it and its parent cannot reach each other) | `allow-same-origin` |
| navigation of anything but itself and its descendants | `allow-top-navigation` |
| new tabs | `allow-popups` |

A nested document inherits its ancestors' restrictions. Other tokens
are ignored.

## Limits

- 64 nested documents per page (`AU_FRAMES_MAX`), 8 levels
  (`AU_FRAMES_DEPTH`).
- A frame whose URL is that of one of its ancestors is refused.
- `loading=lazy` frames load when they come within a viewport of the
  visible area or are painted, and do not hold the page's load event.
- A refused frame stays an empty box and still gets its load event.
- One call of `au_frames_idle` begins at most 4 documents that need no
  request (about:blank, srcdoc; `AU_FRAMES_IDLE_DOCS`) and leaves the
  rest for the next call. A load listener that navigates its frame to
  about:blank or sets its `srcdoc` makes a loop that never ends, as in
  other browsers; without the cap each call began 512 documents (17 s
  in the sanitizer build from one line of script, found by `fuzz_dom`).

## Geometry

Layout makes the element a replaced box (300 x 150 by default) and
calls `AuLayoutEnv.paint_frame` with its content box; `au_frames_paint`
sets the nested view's viewport to the box, paints it there with the
frame's own scroll offset (`AuFrame.sx`, `sy`) and draws the scroll
bars (as the page's overflow scrollers, unless `scrolling=no`).

- A nested document's canvas is transparent unless its root or body has
  a background (`au_layout_set_transparent`).
- Rectangles a nested view invalidates are moved into the owner's
  coordinates and clipped to the box.
- When the page scrolls by moving pixels a frame moves as a box does;
  content fixed inside a frame is fixed to the frame (`frames.html` in
  `tests/browser/test_scroll_shift.c`).
- `AuLayoutEnv.embed_kind` tells layout whether an object or embed is a
  nested document, an image or its fallback content.
- `frameset` is a flex container in the UA sheet: rows or columns of
  equal size (the `rows` and `cols` lengths are not read).

## Input

A press inside a frame goes to the nested view, which keeps the drag
and the release, and gives the frame the keyboard
(`AuDocView.focus_frame`) until a press elsewhere. Keys the nested
document does not use scroll the frame, and at its end the page. The
wheel scrolls the innermost scroller under the pointer, then the frame,
then the page.

A click reaches the nested document through the owner's hit test, so
the owner's clips and stacking contexts decide which frame is under the
point, and the nested document's own hit test decides the element
([`layout.md`](layout.md), "Hit testing follows the paint order and its
clips").

**To try an image-tile challenge without a real one**, use the replica
in `tests/frames/replica/`: `index.html` places `bframe.html` in a
frame inside an absolutely positioned container with a large z-index
that script moves into view, as reCAPTCHA places its challenge, and
`bframe.html` has the 4x4 table with the class names and rules of
reCAPTCHA's style sheet and a click handler that toggles the selected
class. Run `aura-host chrome --tab tests/frames/replica/index.html --do
wait:800 --do click:76,240 --do shot:a.png --do wait:300 --do
shot:b.png -o c.png`; the second shot shows the check mark, which the
first paint asks for. Never load a real challenge for this.

Links and forms follow `target`: a frame's name, `_self`, `_parent`,
`_top`, `_blank` (`au_frames_route`); a link in a frame without a
target loads in that frame.

## Script

The documents of a top-level page share one QuickJS runtime; each has
its own context (`au_js_new_child`), so a same-origin document's
objects are reached directly: `contentWindow`, `contentDocument`,
`window.parent`, `top`, `frames`, `length`, `frameElement`, indexed and
named frames (own accessor properties of the window, set by
`P.framesChanged`).

- The memory limit, the stack limit and the time budget are the
  runtime's ([`javascript.md`](javascript.md), "Script memory limits").
  A large script that several documents of the page load is compiled
  once ("How script memory is kept down" there).
- **A frame's window exists as soon as its element does.** A script
  that follows an `<iframe>` in the same parser chunk finds
  `frames.length`, `window[0]`, `window["name"]` and `contentWindow`:
  `run_classic` in `script.c` brings the frame list up to date before a
  parser script runs. The parser's insertions do not mark the list, so
  the check walks the document, and is made only while the parser runs.
- **Indexed and named frames are set before the frames' documents
  start** (`au_frames_sync`), so the script of a `srcdoc` frame finds
  an earlier sibling through `parent.frames[name]` while it loads.
- **The initial document.** A frame whose response has not arrived gets
  an `about:blank` document and a script context when script first asks
  for its window (`au_frames_view_of`); no context is made for a frame
  nobody asks about.
- **A new window object per document (differs from the standard).**
  When the frame's real document begins, the frame gets a new window
  object. The standard keeps one WindowProxy per frame for its whole
  life, and reuses the Window of the initial `about:blank` document for
  a first same-origin document. In Aura, `frames[i]`, `window["name"]`
  and `contentWindow` read again give the new window, and a reference
  taken earlier is not equal to it. Through the old reference,
  `document` is still the blank document and properties script put on
  the window are still there; `location` answers for the top-level
  document; a message posted to it does not reach the new window.
  Keeping the identity needs a context that can take a new document
  (the prelude captures the document when it starts), which is a large
  change.
- **`document.cookie`** in a frame: see
  [`networking.md`](networking.md), "Cookies and `document.cookie`".
- A node's wrapper takes its prototypes from the realm of the node's
  document.
- **Another origin.** For a document of another origin `wrapWindow`
  returns an object with only `postMessage`, `location` (set only),
  `closed`, `parent`, `top`, `frames`, `length`, indexed frames, child
  frames by name, `focus`, `blur` and `close`; anything else throws
  SecurityError, and `contentDocument` is null.
- **postMessage** checks the target origin, the receiving document
  clones the data in its own realm, and `event.source` is the sender's
  window as the receiver may see it. `MessagePort`s in the transfer
  list move to the receiving document (reCAPTCHA-style widgets hand
  their parent a port this way).
- `about:blank` and `srcdoc` frames have their parent's origin.
- **A sandboxed frame without `allow-scripts`** has a window object for
  other documents' script, as in other browsers: the parent may call
  into it (`contentWindow.eval`, which Cloudflare's challenge does), but
  none of the frame's own script runs. The view keeps `scripting` off
  and sets `script_window`, so it gets a script object whose runtime
  has `AuJsSettings.scripts_off`: the parser runs no script, and script
  elements inserted later, event handler attributes, `javascript:` URLs
  and modules do not run. `noscript` content shows, and
  `@media (scripting)` is `none`.
- With the JavaScript preference off nothing has a window, and
  `contentWindow` is null.
- **Frames in shadow trees.** `au_frames_sync` walks the document in
  shadow-including order, so an iframe in a shadow root (Turnstile puts
  its widget in a closed one) loads and has a window. It is not one of
  `window.frames` or `frames.length`, as the standard says. A host whose
  shadow tree script built before inserting the host marks the frame
  list when it is inserted (`subtree_kinds` in `docview.c` walks shadow
  trees too).
- `location.ancestorOrigins` lists the origins of the documents a frame
  is nested in.

## Lifetime

Script may remove or navigate a frame while that frame's script, or any
script of the page, is on the stack. Nothing is freed then.

- A removed frame leaves its owner's list at once (script sees no
  window) and waits in the top view's `dead` list; navigations,
  restarts and load events wait in the frames; `au_frames_idle` does
  the work when no script of the page runs (`safe`).
- `au_js_free` of a frame's context leaves the objects other documents
  still hold valid: wrappers belong to the runtime, the freed context's
  natives then act for the top-level document (its opaque pointer is
  set to the root), and the frame's document lives while a wrapper
  names one of its nodes.
- So a held reference to a removed frame's function still runs, but
  `location`, timers and `document.URL` reached through a dead window
  answer for the top-level document.
- Free a frame's `AuJs` before the top-level one, and never while
  `au_js_running`.

## The document cache and history

- A kept document keeps its frames
  ([`browser-chrome.md`](browser-chrome.md)). `au_docview_can_persist`,
  `au_docview_set_hidden` (pagehide, pageshow, dropped image pixels)
  and `au_docview_memory_estimate` include them; a page with a frame
  still loading is not kept.
- A frame navigation adds no session history entry: Back leaves the
  page, and a kept page comes back with each frame's current document.
  `history.back()` inside a frame acts on the tab.

## Tests

- `tests/frames/test_frames.c`: the document view with a table of pages
  (paint against a reference render, scrolling, clicks and keys, script
  access both ways, postMessage and ports, sandbox, framing headers,
  limits, lazy loading, objects, removal and navigation while script
  runs, a kept document, a cross-origin widget, an image-tile challenge
  in a positioned frame: clicks reach the right tile and the check marks
  paint).
- `tests/frames/replica/`: the image-tile challenge as pages to open by
  hand (see "Input").
- `test_frames_net.c`: a browser window over the fake transport
  (referrer, blocklist, X-Frame-Options from a real response, typing
  and Edit commands in a frame, Back to a kept page with frames).

- `tests/js/pages/frame_initial.html`: a script right after two frames
  sees their windows by index, by name and through `contentWindow`; a
  frame finds its earlier sibling through its parent while it loads; an
  iframe made by script has a window before its document loads.

Run the first two under the sanitizers after a change to lifetimes.

## Cost, measured on the host

Release build: a frame without script adds about 6 KB to the estimate
and 0.15 ms; a frame with script adds a context with its own copy of
the prelude, 1.4 MB and 16 ms (57 ms each with 20 of them, as
collections of the growing heap add up). The runtime's memory limit
bounds the total ([`javascript.md`](javascript.md), "Script memory
limits"): at 1.4 MB each, about 5 scripted frames fit the 8 MB floor of
the Automatic limit and about 45 fit the 64 MB the host tools get.

## Acid3

`run/acid3` is not in git: fetch `http://acid3.acidtests.org/` and the
files it names once, keep each response's status and Content-Type, and
serve them from a local HTTP server (the 404 answers matter). The score
went from 59 to 87 with frames. The tests still failing are listed in
[`known-gaps.md`](known-gaps.md#acid3).

## Not done

See [`known-gaps.md`](known-gaps.md#frames).
