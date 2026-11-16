# Building

This document covers building Aura for the host and for Mac OS 9, the
special build trees (sanitizers, Release, allocation counter), and
installing what the build and the test machine need: the Retro68
toolchain, QEMU, the disk images and hfsutils. Read it when you set up
a machine or need a build other than the default.

## To build and test

| To | Run |
|---|---|
| build for the host and run the tests | `./build.sh test` |
| build for Mac OS 9 | `./build.sh ppc` |
| do both | `./build.sh` |

## The Mac OS 9 build

`./build.sh ppc` builds with Retro68 (GCC 16.1, Multiversal Interfaces).

- The toolchain is found through `RETRO68`, then in
  `~/.local/opt/retro68` (`toolchain/find.sh`).
- A `build/ppc` folder keeps the toolchain it was first configured
  with; delete it to switch.
- The build also assembles the distributable folder
  `build/ppc/dist/Aura`: the application (`Aura.bin`, MacBinary) beside
  `Fonts` (the family folders of `assets/fonts`, `fonts.idx` and the
  fonts' README).
- Then it packs that folder as the self-extracting archive,
  `Aura-VERSION.sea.bin` and `Aura-VERSION.hqx` in `build/ppc/dist`
  (see [`ci-and-releases.md`](ci-and-releases.md)).
- The build must finish without warnings (`CONTRIBUTING.md`).

To copy the result into the test machine and run it, see
[`guest-testing.md`](guest-testing.md).

## Other build trees

**Sanitizers.** To run the tests under AddressSanitizer and UBSan,
configure a separate folder:

```bash
cmake -S . -B build/asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined,float-cast-overflow"
cmake --build build/asan
ctest --test-dir build/asan
```

`float-cast-overflow` reports a float converted to an integer type that
cannot hold it (a page reaches such conversions with 1e38px and NaN; see
[`fuzzing.md`](fuzzing.md), "Rules the fuzzers enforce"). Clang's
`undefined` group includes it (Apple Clang 21 reports it with
`undefined` alone, checked 2026-11-15, and Clang's documentation lists
it in the group); GCC's does not, so the flag names it and every
compiler checks the same. CI configures with the same sanitizers.

CI's Linux Clang finds two kinds of fault that the macOS build does
not report, so a local sanitizer run that passes can still fail in CI:
a call through a function pointer of another type (`-fsanitize=function`,
for example a `DynBufReallocFunc` cast in QuickJS, fixed by patch 0014),
and a NULL pointer passed to `memcmp` or `memcpy` with a length of 0
(glibc declares those arguments non-null). Never cast a function to
another function pointer type; write a wrapper with the right type.
Check a pointer that may be NULL before passing it to `memcmp`, even
with a length of 0.

Run this for any change to code that reads data from the network or a
file. The long fuzz sessions named in the area documents use this
tree's binaries (for example `build/asan/tests/image/fuzz_image`).
macOS does not detect leaks; CI runs the same build on Ubuntu with leak
detection off.

**Release.** To time the engine on the host, build a Release tree:

```bash
cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release
```

[`performance.md`](performance.md) says what to run in it.

**Allocation counter.** To see which code holds memory, configure with
`-DAURA_MEM_TRACK=ON` in `build/mem` ([`memory.md`](memory.md)).

**Prelude from source.** To load the JavaScript prelude from source
instead of bytecode (errors in it then carry line numbers), configure
with `-DAURA_JS_PRELUDE_SOURCE=ON` ([`javascript.md`](javascript.md)).

## The host tool

The host build makes `build/host/src/platform/host/aura-host`. Run it
without arguments for its commands and options. The area documents
give the command for each kind of work: `render`, `scene`, `chrome`,
`layout-bench`, `image`, `fonts`, `font-index`, `fetch`, `js`, `page`
and `blocklist`.

## Dependencies outside the repository

Aura depends on no other project's checkout. What it needs is installed
by its own scripts or kept in git-ignored folders here.

### Retro68

To install the toolchain, run `toolchain/build.sh`.

- It fetches Retro68 at the commit pinned in the script (a4a389c41e90,
  PowerPC only, Multiversal Interfaces) into `toolchain/Retro68`,
  builds in `toolchain/build`, and installs in `~/.local/opt/retro68`
  (or `$RETRO68`).
- It does nothing when that folder's `REVISION` names the pinned
  commit.
- To move to another commit, change it in the script, empty the install
  folder, and run the script.
- In CI, `~/.local/opt/retro68` is cached keyed on the hash of
  `toolchain/build.sh` (see [`ci-and-releases.md`](ci-and-releases.md)).

### QEMU

To install the QEMU the test machine runs, run `qemu/build.sh`.

- It clones the release pinned in the script (v11.1.3) into `qemu/src`,
  applies `qemu/patches/*.patch`, and installs in
  `~/.local/opt/qemu-os9` (or `$QEMU_PREFIX`).
- It does nothing when the install's `REVISION` matches.

### Disk images

The test machine's base images are in `disks/` (git-ignored):

| Image | System |
|---|---|
| `os9-novm.qcow2` | a stock Mac OS 9.2.2 with virtual memory off |
| `os9-vmoff.qcow2` | the same system with Enforcer installed |

Both are standalone qcow2 files with no backing file.
[`guest-testing.md`](guest-testing.md) records which Enforcer is on the
second image and how to update it.

### hfsutils

`tools/guest-push.sh` needs `hformat` and `hcopy`: `brew install
hfsutils`.

### Where these came from

The install scripts, the QEMU patch and the two disk images were copied
from the os9-enforcer project on 2026-11-14. The Enforcer image was
updated on 2026-11-15 (see [`guest-testing.md`](guest-testing.md)).

### Tools some tests and generators need

Building Aura needs none of these; the documents named say when each is
used.

| Tool | Used by | Document |
|---|---|---|
| fontTools, the `brotli` command | `tools/test-fonts.py`, `tools/fonts-prepare.py` | [`fonts.md`](fonts.md) |
| PngSuite, the libwebp test images | image tests that skip without them | [`images.md`](images.md) |
| `python3` | `src/net/gen_roots.py`, `tests/net/gen_tls_fixtures.py`, the archive tools | [`tls.md`](tls.md), [`ci-and-releases.md`](ci-and-releases.md) |
