/* The JavaScript prelude, embedded at build time. The parts are one
 * function expression split over several files (see prelude.js): their
 * concatenation, in this order, is the source runtime.c evaluates. */
#include <stddef.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

extern const char au_js_prelude_src[];
extern const size_t au_js_prelude_len;

const char au_js_prelude_src[] = {
#embed "prelude.js"
    , '\n',
#embed "prelude_dom.js"
    , '\n',
#embed "prelude_html.js"
    , '\n',
#embed "prelude_window.js"
    , '\n',
#embed "prelude_net.js"
    , '\n',
#embed "prelude_obs.js"
    , 0};

const size_t au_js_prelude_len = sizeof au_js_prelude_src - 1;
