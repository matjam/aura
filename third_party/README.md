# Third-party code

Aura carries two third-party libraries. Each entry records where the code
came from and how to update it.

## QuickJS-ng (JavaScript engine)

- **Version:** v0.18.0, commit d7215b0ad3fc2c562e8317fd9bca058fd4863804
  (2026-11-01).
- **Source:** https://github.com/quickjs-ng/quickjs, a shallow clone of
  the tag `v0.18.0`.
- **Licence:** MIT, `quickjs/LICENSE`.
- **What is kept:** the engine only: `quickjs.c`, `libregexp.c`,
  `libunicode.c`, `dtoa.c` and their headers (`quickjs-patches/apply.sh`
  lists them). No CLI (`qjs`, `qjsc`), no `quickjs-libc`, no test262
  harness; a subset of the tests (below).
- **Build:** `quickjs.cmake` builds the static library `aura_quickjs`
  with QuickJS-ng's own warning flags, `-funsigned-char` and
  `-fno-strict-aliasing` (QuickJS passes `uint32_t *` as `int *`, and
  `uint32_t` is `unsigned long` in Retro68's newlib), and links it into
  `aura_engine`. Aura reaches it only through `src/js`.
- **Changes:** every change is a patch in `quickjs-patches/`, applied in
  name order. To update QuickJS-ng, fetch the new release and run
  `third_party/quickjs-patches/apply.sh PRISTINE_DIR`, which copies the
  engine files into `third_party/quickjs` and applies the patches; fix any
  patch that no longer applies; then run `./build.sh test` and the guest
  self-test (`src/platform/macos9/jstest`). The patches are:
  `0001` newlib portability (no pthreads or C11 atomics, big-endian
  checks), `0002` a retry hook in module resolution so the embedder can
  fetch a missing module and resolve again, `0003` a GC mark for
  WeakMap values with symbol keys (fixes a realm leak), and `0004` 16
  bytes of padding on every `alloca` on Mac OS 9, because Retro68's GCC
  places spilled locals in the top 8 bytes of alloca space (the patch
  header has a reproduction).
- **Tests:** `quickjs/tests` holds the subset of upstream's `tests/`
  folder that needs no `quickjs-libc` (no `qjs:std`, `qjs:os`, `bjson`,
  workers or test262 harness); the microbenchmark is left out.
  `tests/js/CMakeLists.txt` runs each through `aura-host js`. Copy them
  again from the new release when updating.
