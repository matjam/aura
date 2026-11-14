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
- Commit each working step to `main`. Commit only our own sources. Fonts,
  Apple files and downloaded test suites stay out of git.
- Write docs instruction first ("To X, use Y"), in plain technical prose.

## Layout

| Folder | Holds |
|---|---|
| `src/core` | allocation, buffers, UTF-8, atoms |
| `src/dom` | the node tree |
| `src/html` | HTML tokenizer and tree builder |
| `src/css` | CSS parser, selectors, cascade |
| `src/layout` | box tree and layout |
| `src/paint` | surfaces, rasterizer, fonts |
| `src/platform/host` | host tools: headless render to PNG, tree dumps |
| `src/platform/macos9` | the Mac OS 9 application |
| `tests/<area>` | one test area per folder, each with a `CMakeLists.txt` |

`CMakeLists.txt` globs `src/**/*.c` into the `aura_engine` library and adds
every `tests/*/CMakeLists.txt`, so a new source file or test area needs no
change to the top-level file.

## Build and test

- `./build.sh test` builds for the host and runs the tests.
- `./build.sh ppc` builds for Mac OS 9 with Retro68 (GCC 16.1, Multiversal
  Interfaces). The toolchain is found through `RETRO68`, then in
  `../os9-protect/toolchain/build/toolchain`.

The test machine is the QEMU Mac OS 9.2.2 guest in `os9-protect`
(`run.sh`, `tools/push.sh` to copy files in, `tools/qmp.py` for
screenshots and input). `os9-enforcer/CLAUDE.md` describes how to use it.
The guest has outbound network access.

## Design

The decisions and milestones are in the Gandalf vault,
`project:aura:decisions`.
