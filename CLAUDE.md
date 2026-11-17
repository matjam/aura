# Aura: where to look

Aura is a lightweight web browser for retro computers and legacy
operating systems, with its engine written from scratch in portable C99.
Its first platform is Mac OS 9 on PowerPC, built and tested on a modern
host and run in a QEMU guest (and on real Macs). Ports to mainstream
systems (Linux with GTK4 first) exist for testing and compatibility
work; decisions favour the legacy platforms
([`docs/porting.md`](docs/porting.md)).

This file is a directory. It states the rules every change must follow
and names the document under `docs/` to read for each kind of work.
Read that document before you change the area. `README.md` describes
Aura for its users; keep it short.

## Rules for every change

- **Portable C99 engine.** Code in `src/` outside `src/platform/` makes
  no OS calls and must build for the host and for big-endian 32-bit
  PowerPC. [`docs/conventions.md`](docs/conventions.md)
- **Input is hostile.** Check every length, overflow and loop bound
  that data from a server or file controls; a parser change needs a
  vector, a truncation test and a fuzz run.
  [`docs/conventions.md`](docs/conventions.md)
- **No recursion over document depth.** Use loops or an explicit stack.
  [`docs/conventions.md`](docs/conventions.md)
- **`au_alloc`, not `malloc`.** [`docs/memory.md`](docs/memory.md)
- **No `alloca`, no variable-length arrays.**
  [`docs/javascript.md`](docs/javascript.md)
- **No `%lld` or `%llu`** in engine code; use `au_put_i64`.
  [`docs/conventions.md`](docs/conventions.md)
- **Third-party code is QuickJS-ng and Mbed TLS only**, changed through
  patches. Ask before adding another.
  [`docs/conventions.md`](docs/conventions.md)
- **MIT, and nothing under the GPL is bundled** (no blocklist in the
  repository or the application).
  [`docs/conventions.md`](docs/conventions.md)
- **Conventional Commits** (`fix:`, `feat:`, `perf:`, `docs:`, `test:`,
  `chore:`, `refactor:`); release-please reads them.
  [`docs/conventions.md`](docs/conventions.md)
- **Commit and push each working step to `main`.** Commit only our own
  sources. [`docs/conventions.md`](docs/conventions.md)
- **Test on the Enforcer base by default, and on the stock base before
  a release.** [`docs/guest-testing.md`](docs/guest-testing.md)
- **Decisions and milestones are recorded in the Gandalf vault,
  `project:aura:decisions`.** Check a feature against
  [`docs/v1-scope.md`](docs/v1-scope.md) before building it.
- **Docs are written instruction first** ("To X, use Y"), in plain
  technical prose. [`docs/conventions.md`](docs/conventions.md)

## If you are working on … read …

| Work | Read |
|---|---|
| anything, for the first time | [`docs/architecture.md`](docs/architecture.md), [`docs/conventions.md`](docs/conventions.md) |
| building, sanitizer or Release trees, the toolchain, QEMU, disk images | [`docs/building.md`](docs/building.md) |
| driving the guest: starting it, pushing a build, clicks, screenshots, the Enforcer image, the guest clock | [`docs/guest-testing.md`](docs/guest-testing.md) |
| CI, making a release, the installer archive, BinHex | [`docs/ci-and-releases.md`](docs/ci-and-releases.md) |
| CSS parsing, selectors, the cascade, restyling | [`docs/css.md`](docs/css.md) |
| the box tree, block, inline, flex or grid layout, relayout, text painting | [`docs/layout.md`](docs/layout.md) |
| fonts, fallback, emoji, variable and web fonts | [`docs/fonts.md`](docs/fonts.md) |
| image decoders and the image cache | [`docs/images.md`](docs/images.md) |
| SVG, masks | [`docs/svg.md`](docs/svg.md) |
| the canvas element | [`docs/canvas.md`](docs/canvas.md) |
| the script runtime, DOM bindings, the prelude, QuickJS patches, `Date` | [`docs/javascript.md`](docs/javascript.md) |
| `fetch`, `XMLHttpRequest`, CORS | [`docs/script-requests-and-cors.md`](docs/script-requests-and-cors.md) |
| workers, coroutines | [`docs/workers.md`](docs/workers.md) |
| iframes, objects, script between documents, sandbox | [`docs/frames.md`](docs/frames.md) |
| form controls, text editing, submission, typing speed | [`docs/forms.md`](docs/forms.md) |
| HTTP, the cache, DNS, cookies, HSTS, HTTPS-first, the loader | [`docs/networking.md`](docs/networking.md) |
| `localStorage`, `sessionStorage`, the Local Storage folder, `document.referrer` | [`docs/web-storage.md`](docs/web-storage.md) |
| TLS, certificates, the root store, Mbed TLS and its patches | [`docs/tls.md`](docs/tls.md) (history: [`docs/tls13-feasibility.md`](docs/tls13-feasibility.md)) |
| blocklists | [`docs/content-blocking.md`](docs/content-blocking.md) |
| windows, tabs, history, Back and Forward, scrolling, selection, find, contextual menus, Internet Config, downloads, session files | [`docs/browser-chrome.md`](docs/browser-chrome.md) |
| the Mac OS 9 application, the Toolbox, Open Transport, entropy, the clock | [`docs/mac-os-9-shell.md`](docs/mac-os-9-shell.md) |
| the Develop menu and its windows, System Status, Report a Problem and the bug report builder | [`docs/developer-tools.md`](docs/developer-tools.md) |
| fuzzing, or any code a page's data reaches | [`docs/fuzzing.md`](docs/fuzzing.md) (Aura's fuzzers, Domato, and the rules they enforce) |
| a new platform port (Linux GTK4), or the line between `src/browser` and `src/platform` | [`docs/porting.md`](docs/porting.md) |
| the Linux GTK4 port: building, running, testing in a headless compositor, its windows, input, menus, dialogs, main loop | [`docs/gtk.md`](docs/gtk.md) |

| Problem | Read |
|---|---|
| a site does not render right | [`docs/known-gaps.md`](docs/known-gaps.md) first, then [`docs/layout.md`](docs/layout.md), [`docs/css.md`](docs/css.md), [`docs/fonts.md`](docs/fonts.md); see what it requested with [`docs/developer-tools.md`](docs/developer-tools.md) |
| a site's script fails | [`docs/javascript.md`](docs/javascript.md) ("To debug a script on a live page"), [`docs/script-requests-and-cors.md`](docs/script-requests-and-cors.md), [`docs/known-gaps.md`](docs/known-gaps.md) |
| a site does not load, or its certificate fails | [`docs/networking.md`](docs/networking.md), [`docs/tls.md`](docs/tls.md), [`docs/content-blocking.md`](docs/content-blocking.md) |
| something is slow | [`docs/performance.md`](docs/performance.md) |
| running out of memory | [`docs/memory.md`](docs/memory.md); for script, [`docs/javascript.md`](docs/javascript.md) ("Script memory limits") |
| adding a test | [`docs/architecture.md`](docs/architecture.md) ("To add a test"), then the area's document |
| changing third-party code | [`docs/conventions.md`](docs/conventions.md), then [`docs/javascript.md`](docs/javascript.md) or [`docs/tls.md`](docs/tls.md), and `third_party/README.md` |
| making a release | [`docs/ci-and-releases.md`](docs/ci-and-releases.md) |
| looking for work, or for what is missing | [`docs/known-gaps.md`](docs/known-gaps.md), [`docs/v1-scope.md`](docs/v1-scope.md) |

## Build and test

```bash
./build.sh test                                            # host build and tests
./build.sh ppc                                             # Mac OS 9 build and installer archive
tools/guest-run.sh start                                   # start the test machine (Enforcer base)
tools/guest-push.sh -n AuraTest build/ppc/dist/Aura/*      # copy the build in (quit Aura first)
tools/guest-qmp.py open AuraTest Aura                      # launch it; `screenshot run/NAME.png` to look
```

After a merge, `tools/smoke.sh` checks the Mac OS 9 build in a stock
guest end to end ([`docs/guest-testing.md`](docs/guest-testing.md)).

For code that reads data from the network or a file, also run the tests
under the sanitizers ([`docs/building.md`](docs/building.md)).

## Keeping the docs current

- Put new knowledge in the topic document, in the same change as the
  code: recipes ("To X, run Y") near the top, measurements in tables
  with their date and base, and gaps in
  [`docs/known-gaps.md`](docs/known-gaps.md).
- State a fact in one document and link to it from the others.
- When a new area appears, add a document under `docs/` and a row to
  the table above.
- Keep this file a directory. Do not add explanations, measurements or
  history here; it should stay under 150 lines.
