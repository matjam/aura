# Aura version 1 scope

Agreed on 2026-11-14. To change the scope, edit this file and say why in
the commit.

Version 1 is a browser a person can use daily on a 1 GHz G4 under
Mac OS 9.1 to 9.2.2, for server-rendered and moderately scripted sites,
safely.

## Acceptance sites

Version 1 is done when these work:

- old.reddit.com: browse, log in, vote, comment
- reddit.com: read
- Wikipedia
- Hacker News
- DuckDuckGo search
- GitHub: read repositories and issues
- typical news and documentation sites, and retro Mac sites

Gmail's web application is not a version 1 criterion.

## Performance on a 1 GHz G4

Version 1 has no size, memory or speed targets. Get the features working
first; optimize afterwards, from measurements on real hardware.

Until then, measure and report, without cutting a feature to meet a
number:

- the application's size on disk, when it changes
- memory in use when idle and with a page loaded
- the time to display a Wikipedia article after its bytes arrive
- whether a laid-out page scrolls smoothly

Keep code small and fast where that costs nothing.

## Must have

### Engine

- HTML: the full parser, the legacy encodings (windows-1252, ISO-8859-1)
  and the `<meta charset>` prescan.
- CSS: cascade, custom properties, media queries.
- Layout: block, inline, inline-block, floats, positioning (relative,
  absolute, fixed, sticky), flexbox, grid, tables, lists, overflow and
  scrolling boxes, `display: contents`, stacking contexts.
- Text: line breaking, white-space modes, alignment, decorations,
  ellipsis, font fallback, Unicode as far as the system fonts cover it.
  Web fonts: TrueType, WOFF, WOFF2.
- Painting: backgrounds, borders, radii, shadows, gradients, opacity, 2D
  transforms, clipping.
- Images: PNG, JPEG, animated GIF, WebP, favicons, SVG as images and
  inline SVG for icons (basic shapes, paths, fills, strokes).
- Forms: text fields, textarea, checkbox, radio, select, buttons, file
  upload, submission (GET, POST, multipart), validation attributes, focus
  and keyboard navigation.
- JavaScript (QuickJS-ng): DOM and events, CSSOM basics (`style`,
  `classList`, `getComputedStyle`, `matchMedia`), timers,
  `requestAnimationFrame`, `fetch` and XHR, URL and URLSearchParams,
  `localStorage` and `sessionStorage`, the history API, MutationObserver,
  IntersectionObserver, ResizeObserver, custom elements and shadow DOM,
  modules and dynamic import, TextEncoder, `document.cookie`.
- Incremental rendering: show the page as it arrives, and lay out again
  only what changed.

### Network

- Open Transport, DNS, HTTP/1.1 with keep-alive, redirects, chunked
  encoding, gzip, deflate and brotli.
- HTTP and HTTPS proxy; basic and digest authentication.
- TLS 1.2 at least, certificate validation, a bundled root store, Aura's
  own random source, and a check for an implausible system clock.
- Memory and disk cache with validation.
- Cookies with Secure, HttpOnly, SameSite, name prefixes and the public
  suffix list.

### Security and privacy

- HSTS with a stored list and a bundled preload list; mixed content
  blocking; HTTPS-first mode.
- Same-origin policy, CORS, Content-Security-Policy, `nosniff`,
  Referrer-Policy.
- Do Not Track and Global Privacy Control; third-party cookie blocking;
  tracker and advertisement blocking from a host list.
- Script limits: memory cap, stack cap, an interrupt for runaway scripts.
- Downloads are never opened or run automatically.
- Security indicator, certificate viewer, certificate error page.
- Every parser that reads network data is bounds-checked, run under
  sanitizers on the host, and fuzzed. Mac OS 9 has no memory protection,
  so this is the largest exposure.

### Application

- Windows and tabs (top row or left column), navigation bar, address
  field with completion and search, status of hovered links, loading
  progress, Stop.
- Bookmarks with a manager and import and export; history with a window;
  downloads with a manager; cookies with a manager; preferences with all
  panes.
- Find in page, text selection and copy, contextual menus (link, image,
  selection), Save Page As, view source, text zoom, local files, dragging
  a link or file onto the window.
- Session restore; recovery from out-of-memory per page.
- Multiprocessing Services: page work runs off the main task and the
  interface stays responsive.
- Internet Config: registration as the default browser, the `GURL` Apple
  event, helper applications for other schemes.

## Not in version 1

- A JavaScript JIT, and Gmail-class web applications at usable speed. A
  bytecode cache and lazy compilation go in if they are cheap.
- Audio and video, WebRTC, WebGL, WebAssembly.
- Shared workers, service workers, IndexedDB. (Dedicated workers are in:
  bot checks that run a proof of work need them.)
- WebSockets. The first to pull in if an acceptance site needs it.
  (Canvas 2D was listed here and has since been built: see
  [`canvas.md`](canvas.md).)
- CSS animations and transitions beyond jumping to the end state, 3D
  transforms, filters, blend modes. (Container queries were listed here
  and have since been built: see [`css.md`](css.md).)
- Right-to-left and vertical text, complex-script shaping (Arabic,
  Indic).
- HTTP/2, HTTP/3, TLS client certificates.
- Printing, private windows, saved passwords in the Keychain, extensions,
  sync.
- A Carbon build for Mac OS X; 68k Macs.
