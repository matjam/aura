// Setup for QuickJS-ng's own tests (third_party/quickjs/tests), which run
// as modules in a window: the qjs shell's gc() becomes a no-op.
globalThis.gc = () => {};
