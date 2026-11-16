# Conventions

This document holds the rules that apply to every change to Aura: how
the C code is written, what may enter the repository, how commits are
named and how the documentation is written. Read it before your first
change. The rules for one area (a decoder's limits, a formatting
context's bounds) are in that area's document; [`CLAUDE.md`](../CLAUDE.md)
names the document for each kind of work.

## Code rules

- **C99, portable.** Engine code (everything in `src/` outside
  `src/platform/`) makes no OS calls and includes no OS headers. It must
  build for the host and for PowerPC Mac OS 9 (big-endian, 32-bit,
  newlib).
- **Do not recurse over document depth** (tree walks, destruction,
  layout of nested boxes where it can be avoided): Mac OS 9 stacks are
  small and pages nest deeply. Use explicit loops or an explicit stack.
- **Allocate with `au_alloc`, `au_realloc`, `au_free`**
  (`src/core/core.h`). They do not return NULL. Do not call `malloc` in
  engine code. [`memory.md`](memory.md) describes the release handler,
  the out-of-memory handler and the pooled allocator behind them on
  Mac OS 9.
- **Do not use `alloca` or variable-length arrays** in Aura's own code:
  Retro68's GCC places spilled locals in the top 8 bytes of alloca
  space (see [`javascript.md`](javascript.md), "Retro68 compiler
  points").
- **Do not use `%lld` or `%llu`** in engine code: newlib on Mac OS 9
  lacks them and writes garbage (the Cookies file was unreadable for
  that reason). Use `au_put_i64` (`history.h`).
- **Text is UTF-8.** Names (tags, attributes, CSS properties and
  keywords) are atoms. Names known at compile time go in
  `src/core/atoms_html.def` or `src/core/atoms_css.def`.
- **Public names** start with `au_` (functions), `Au` (types), `AU_`
  (constants). Prefix types with `Au` in every file: Retro68's
  `<string.h>` pulls in the Toolbox `Strings.h`, so names like `FInfo`
  collide with Toolbox types.
- **Input is hostile.** Everything a server or a file gives Aura is
  parsed on a machine without memory protection. Check every length
  that comes from the data before use, check size computations for
  overflow, and bound every loop and every allocation that the data
  controls. Each area states its limits: see
  [`images.md`](images.md), [`svg.md`](svg.md), [`canvas.md`](canvas.md),
  [`layout.md`](layout.md) (grid), [`content-blocking.md`](content-blocking.md)
  and [`frames.md`](frames.md). A change to a parser or decoder needs a
  test vector, a truncation test and a fuzz run under the sanitizers
  ([`building.md`](building.md) says how to build with them;
  [`fuzzing.md`](fuzzing.md) lists the fuzzers and the rules they
  enforce on new code: `au_f2i` for floats a page controls, depth
  limits, capped allocations).

## Third-party code and licences

- Third-party code is limited to QuickJS-ng (JavaScript) and Mbed TLS
  (TLS, with its cryptography library TF-PSA-Crypto), in `third_party/`.
  Everything else is written here. Ask before adding another.
- To change either library, add or edit a patch in
  `third_party/quickjs-patches` or `third_party/mbedtls-patches`; never
  edit the imported tree alone. `third_party/README.md` records sources,
  licences and how each library is carried and updated. See
  [`javascript.md`](javascript.md) and [`tls.md`](tls.md).
- Aura's own code is under the MIT licence (`LICENSE`). Nothing under
  the GPL is bundled: the blocklists Aura can download are GPL-3.0, so
  no list goes into the repository or the application (see
  [`content-blocking.md`](content-blocking.md)).
- To credit a bundled font, library or data set, add it to the credits
  table (see [`browser-chrome.md`](browser-chrome.md), "The About box
  and about:licences").

## What goes in git

- Commit each working step to `main`.
- Commit only our own sources. Apple files, downloaded test suites and
  fonts stay out of git, except the redistributable bundled fonts in
  `assets/fonts`, which are committed with their licences.
- Disk images, the toolchain, QEMU and the run folders are git-ignored
  (see [`building.md`](building.md) and
  [`guest-testing.md`](guest-testing.md)).

## Commit messages

Commit messages follow Conventional Commits, because release-please
works out the next version and the changelog from them (see
[`ci-and-releases.md`](ci-and-releases.md)): a type, a colon, then the
subject as a plain sentence.

| Type | Use it for | Effect on the version |
|---|---|---|
| `fix:` | a bug fix | bug-fix digit |
| `feat:` | something a user of Aura can see or use | minor digit |
| `perf:` | faster or smaller | bug-fix digit |
| `docs:` | documentation only | none |
| `test:` | tests only | none |
| `chore:` | tooling, CI, merges and clean-up | none |
| `refactor:` | no change in behaviour | none |

- `feat!:` or `fix!:` marks a change that breaks something users rely
  on (stored preferences that no longer load, say).
- Only `feat`, `fix`, `perf` and breaking changes appear in the
  changelog, and only they cause a release.
- A merge commit keeps a conventional subject too: `chore: merge main
  into grid`, or `feat: CSS grid layout` when the merge is what brings
  the feature to `main` and its own commits were not conventional.

## Documentation

- Write docs instruction first ("To X, use Y"), in plain technical
  prose.
- `README.md` describes Aura for its users. Keep it short.
- Put what you learn in the topic document under `docs/`; keep
  `CLAUDE.md` a directory (it says how at its end).

## Where decisions and scope are recorded

- The decisions and milestones are in the Gandalf vault,
  `project:aura:decisions`.
- The scope of version 1 (acceptance sites, required features, what is
  left out) is in [`v1-scope.md`](v1-scope.md). Check a feature against
  it before building it.
- What is known to be missing is in [`known-gaps.md`](known-gaps.md).
