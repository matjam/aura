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
  lists them). No CLI (`qjs`, `qjsc`), no `quickjs-libc`, no tests, no
  test262 harness.
- **Build:** `quickjs.cmake` builds the static library `aura_quickjs`
  with QuickJS-ng's own warning flags and `-funsigned-char`, and links it
  into `aura_engine`. Aura reaches it only through `src/js`.
- **Changes:** every change is a patch in `quickjs-patches/`, applied in
  name order. To update QuickJS-ng, fetch the new release and run
  `third_party/quickjs-patches/apply.sh PRISTINE_DIR`, which copies the
  engine files into `third_party/quickjs` and applies the patches; fix any
  patch that no longer applies; then run `./build.sh test` and the guest
  self-test (`src/platform/macos9/jstest`).
