/* The JavaScript prelude, embedded at build time. The parts are one
 * function expression split over several files (see prelude.js): their
 * concatenation, in this order, is the prelude. The build compiles it to
 * bytecode (prelude.bc) and that is what the runtime loads; with the
 * CMake option AURA_JS_PRELUDE_SOURCE the source is embedded and compiled
 * at every runtime start instead (errors then carry line numbers). */
#include <stddef.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#ifdef AU_JS_PRELUDE_SOURCE

extern const char au_js_prelude_src[];
extern const size_t au_js_prelude_len;

const char au_js_prelude_src[] = {
#embed "prelude.js"
    , '\n',
#embed "prelude_dom.js"
    , '\n',
#embed "prelude_html.js"
    , '\n',
#embed "prelude_range.js"
    , '\n',
#embed "prelude_cssom.js"
    , '\n',
#embed "prelude_window.js"
    , '\n',
#embed "prelude_web.js"
    , '\n',
#embed "prelude_net.js"
    , '\n',
#embed "prelude_canvas.js"
    , '\n',
#embed "prelude_workers.js"
    , '\n',
#embed "prelude_obs.js"
    , 0};

const size_t au_js_prelude_len = sizeof au_js_prelude_src - 1;

/* A worker's prelude: prelude.js, prelude_web.js, prelude_net.js and
 * prelude_worker.js. */
extern const char au_js_worker_prelude_src[];
extern const size_t au_js_worker_prelude_len;

const char au_js_worker_prelude_src[] = {
#embed "prelude.js"
    , '\n',
#embed "prelude_web.js"
    , '\n',
#embed "prelude_net.js"
    , '\n',
#embed "prelude_worker.js"
    , 0};

const size_t au_js_worker_prelude_len = sizeof au_js_worker_prelude_src - 1;

/* Intl, read when a page first touches it (N.intl in runtime.c). */
extern const char au_js_intl_src[];
extern const size_t au_js_intl_len;

const char au_js_intl_src[] = {
#embed "intl.js"
    , 0};

const size_t au_js_intl_len = sizeof au_js_intl_src - 1;

#else

/* The prelude as QuickJS bytecode, compiled at build time by
 * tools/jsprelude from the same files (third_party/quickjs.cmake), in the
 * byte order of the machine this is built for. */
extern const unsigned char au_js_prelude_bc[];
extern const size_t au_js_prelude_bc_len;

const unsigned char au_js_prelude_bc[] = {
#embed "prelude.bc"
};

const size_t au_js_prelude_bc_len = sizeof au_js_prelude_bc;

extern const unsigned char au_js_worker_prelude_bc[];
extern const size_t au_js_worker_prelude_bc_len;

const unsigned char au_js_worker_prelude_bc[] = {
#embed "prelude_worker.bc"
};

const size_t au_js_worker_prelude_bc_len = sizeof au_js_worker_prelude_bc;

extern const unsigned char au_js_intl_bc[];
extern const size_t au_js_intl_bc_len;

const unsigned char au_js_intl_bc[] = {
#embed "intl.bc"
};

const size_t au_js_intl_bc_len = sizeof au_js_intl_bc;

#endif
