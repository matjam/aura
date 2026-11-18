# Web platform coverage

This document lists the web platform features modern sites use, which
of them Aura has, and the order to build the missing ones in. Read it
before choosing what to build next, and update it in the same change
as the feature.

## To check a feature

Run the probe in Aura and in Chrome on an https page (on `about:blank`
storage and crypto are hidden in both), and compare:

```bash
build/host/src/platform/host/aura-host chrome --net --tab https://example.com/ --do wait:2000 --do "js:$(cat tools/feature-probe.js)"
```

```bash
python3 tools/chrome-shot.py --eval "$(cat tools/feature-probe.js)" https://example.com/ 800 600 run/probe.png
```

Each prints the features it lacks. On 2026-11-18 Aura lacked 54 of 229
and headless Chrome 3 (`Node.prototype.moveBefore`,
`ReadableStream.from`, `SharedArrayBuffer`).

The probe tests that a name exists, or that `CSS.supports` accepts a
declaration. It cannot see a feature that exists and does nothing. Those
are listed by hand under "Present but not working".

## Present but not working

These were found by reading the bindings (`src/js/prelude*.js`) for
stubs. They break sites quietly, so they come first.

| Feature | What is wrong | Breaks |
|---|---|---|
| `new FontFace(name, buffer)` | rejects | icon fonts built by script |
| `DOMParser` with an XML type | throws | sites that parse SVG or RSS by script |

## Missing, in build order

Ranked by how many sites a gap breaks, then by cost. "v1" marks items
inside [`v1-scope.md`](v1-scope.md); the others need the scope changed
first.

| # | Feature | Why | Cost | v1 |
|---|---|---|---|---|
| 1 | ~~History bridge~~: done 2026-11-18 ([`browser-chrome.md`](browser-chrome.md), "Entries a page's script makes") | single-page navigation | | yes |
| 2 | ~~Named access on `window`~~: done 2026-11-18 ([`javascript.md`](javascript.md), "Named access on the window") | older sites and inline scripts use `foo` for `id=foo` | | yes |
| 3 | ~~`HTMLImageElement.decode()` that waits~~: done 2026-11-18; it resolves at once for an image without a box, which Aura does not load ([`known-gaps.md`](known-gaps.md)) | | | yes |
| 4 | ~~`ElementInternals`~~: done 2026-11-18 ([`forms.md`](forms.md), "Script"); `:state()` matching is left ([`known-gaps.md`](known-gaps.md)) | | | yes |
| 5 | Shims that make a site's normal path work. Done 2026-11-18 (`prelude_window.js`, "small APIs pages call without checking", and `prelude_html.js`): `scheduler.postTask` and `yield`, `Element.checkVisibility`, `document.startViewTransition` (runs the update; the transition is skipped), `navigator.permissions.query` (all denied), `navigator.locks` within the document, `requestFullscreen` (rejects), pointer capture (recorded, not yet redirecting events), the `DragEvent`, `ClipboardEvent` and `FormDataEvent` constructors, `Notification` (denied), `BroadcastChannel` within the document. Left: `caretPositionFromPoint`, pointer capture that redirects events. `TouchEvent` stays out: sites take its presence for a touch screen | sites call these without a check, or take a broken fallback | small each | yes |
| 6 | ~~`navigator.clipboard.writeText`~~: done 2026-11-18 ([`javascript.md`](javascript.md), "User activation and the clipboard"); reading stays refused | copy buttons | | yes |
| 7 | CSS relative colours (`rgb(from …)`), `accent-color`, `field-sizing`, `content-visibility` (treated as `visible`), `overscroll-behavior` and `scrollbar-gutter` (parsed; the gutter reserves space), `interpolate-size` (parsed) | declarations dropped today; `CSS.supports` answers false, so sites take fallbacks | small each | yes |
| 8 | `URLPattern` | routers in newer frameworks | medium | yes |
| 8b | Nested rules in CSSOM (`CSSStyleRule.cssRules`, `insertRule` on a style rule) | nesting itself applies; only script that reads or edits nested rules sees an empty list (the probe's "css nesting" line) | small | yes |
| 9 | `CSS.registerProperty` and `@property` initial values | animated custom properties fall back to nothing | medium | yes |
| 10 | Scroll snapping (`scroll-snap-type`, `scroll-snap-align`) | carousels stop between items | medium | yes |
| 11 | WebSocket | live updates and chat; sites fall back to polling or show stale data | large: framing over the existing TLS connection | no: "the first to pull in" |
| 12 | IndexedDB (in memory, per page) | sites that store state there fail to start | large | no |
| 13 | The Navigation API (`navigation`, `NavigateEvent`) | newer routers; all detect it and fall back to `history` | large; after item 1 | no |

## Not planned

Out of scope for a 1 GHz G4 or for version 1 ([`v1-scope.md`](v1-scope.md),
"Not in version 1"): WebGL and WebGL 2, WebGPU, WebAssembly, Web Audio,
media playback and Media Source, speech synthesis, service workers and
the Cache API, shared workers, `Temporal`, scroll-driven animations
(`animation-timeline`), anchor positioning, view transition animations,
`navigator.share`, `navigator.userAgentData` (Aura does not claim to be
Chrome).
