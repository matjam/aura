/* jsprelude: compiles the JavaScript prelude (src/js/prelude*.js) to
 * QuickJS bytecode at build time. src/js/prelude.c embeds the result and
 * the runtime loads it with JS_ReadObject instead of compiling 300 KB of
 * source for every document.
 *
 * Usage: jsprelude [--swap] [--debug] -o OUT FILE.js...
 *
 * The files are joined in the order given with a line feed between them,
 * as prelude.c joins the sources. --swap writes the bytecode for a machine
 * of the other byte order (JS_WRITE_OBJ_BSWAP, patch 0005): QuickJS
 * bytecode is in the byte order of the machine that reads it, and the
 * tool runs on a little-endian host for the big-endian PowerPC build.
 * --debug keeps line numbers and the source text (larger).
 *
 * The tool must be built from the same QuickJS sources and options as the
 * engine that reads its output: the bytecode refers to the engine's
 * built-in atoms by number. This is a build tool: it uses malloc and
 * stdio, and is not part of the engine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

static char *src;
static size_t src_len, src_cap;

static int add_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "jsprelude: cannot open %s\n", path);
        return -1;
    }
    char chunk[65536];
    size_t n;
    if (src_len) {
        chunk[0] = '\n';
        n = 1;
    } else {
        n = fread(chunk, 1, sizeof chunk, f);
    }
    while (n > 0) {
        if (src_len + n + 1 > src_cap) {
            src_cap = (src_len + n + 1) * 2;
            src = realloc(src, src_cap);
            if (!src) {
                fprintf(stderr, "jsprelude: out of memory\n");
                exit(1);
            }
        }
        memcpy(src + src_len, chunk, n);
        src_len += n;
        n = fread(chunk, 1, sizeof chunk, f);
    }
    fclose(f);
    return 0;
}

static void report(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, e);
    fprintf(stderr, "jsprelude: %s\n", msg ? msg : "error");
    if (msg)
        JS_FreeCString(ctx, msg);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    const char *st = JS_ToCString(ctx, stack);
    if (st) {
        fprintf(stderr, "%s\n", st);
        JS_FreeCString(ctx, st);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    int flags = JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--swap"))
            flags |= JS_WRITE_OBJ_BSWAP;
        else if (!strcmp(argv[i], "--debug"))
            flags &= ~(JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (add_file(argv[i]))
            return 1;
    }
    if (!out || !src_len) {
        fprintf(stderr, "usage: jsprelude [--swap] [--debug] -o OUT FILE.js...\n");
        return 2;
    }
    src[src_len] = 0;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    if (!ctx) {
        fprintf(stderr, "jsprelude: cannot start the engine\n");
        return 1;
    }
    JSValue fn = JS_Eval(ctx, src, src_len, "aura:prelude", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) {
        report(ctx);
        return 1;
    }
    size_t len = 0;
    uint8_t *bc = JS_WriteObject(ctx, &len, fn, flags);
    if (!bc) {
        report(ctx);
        return 1;
    }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(bc, 1, len, f) != len || fclose(f)) {
        fprintf(stderr, "jsprelude: cannot write %s\n", out);
        return 1;
    }
    js_free(ctx, bc);
    JS_FreeValue(ctx, fn);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(src);
    return 0;
}
