/* Module scripts: static imports, dynamic import(), import.meta.url and
 * import maps.
 *
 * QuickJS resolves a module graph synchronously through its loader, but
 * module sources arrive asynchronously through the fetch interface. So
 * the prelude drives loading: it asks moduleEval to compile and run a
 * module; the loader serves sources already fetched and records the URLs
 * it lacks; when any are missing the attempt fails, the prelude fetches
 * them (moduleStore) and tries again. Modules that compiled stay loaded,
 * so each retry only compiles what is new (patch 0002 makes a failed
 * resolution retryable). Dynamic import() works the same way through
 * moduleLoad. */
#include "js_int.h"

#include <string.h>

typedef struct ModSrc {
    char *url;
    char *src;
    size_t len;
    struct ModSrc *next;
} ModSrc;

typedef struct Modules {
    ModSrc *srcs;
    char **missing;
    uint32_t nmissing;
    char **map; /* import map: from, to, from, to ... */
    uint32_t nmap;
    uint32_t inline_seq;
    bool probing;
    struct ModJob *jobs;
    uint32_t next_job;
} Modules;

static Modules *M(AuJs *js)
{
    return js->modules;
}

static void add_missing(Modules *m, const char *url)
{
    for (uint32_t i = 0; i < m->nmissing; i++)
        if (!strcmp(m->missing[i], url))
            return;
    if (m->nmissing >= 256)
        return;
    m->missing = au_realloc(m->missing, (m->nmissing + 1) * sizeof *m->missing);
    m->missing[m->nmissing++] = au_strndup(url, strlen(url));
}

static void clear_missing(Modules *m)
{
    for (uint32_t i = 0; i < m->nmissing; i++)
        au_free(m->missing[i]);
    au_free(m->missing);
    m->missing = NULL;
    m->nmissing = 0;
}

static ModSrc *find_src(Modules *m, const char *url)
{
    for (ModSrc *s = m->srcs; s; s = s->next)
        if (!strcmp(s->url, url))
            return s;
    return NULL;
}

/* Resolves a module specifier against a base URL: import map first, then
 * URLs and relative references; bare specifiers fail. Returns an
 * au_alloc'd URL or NULL. */
static char *resolve_spec(AuJs *js, const char *spec, const char *base)
{
    Modules *m = M(js);
    for (uint32_t i = 0; i < m->nmap; i++) {
        const char *from = m->map[i * 2], *to = m->map[i * 2 + 1];
        size_t fl = strlen(from);
        if (!strcmp(spec, from))
            return au_strndup(to, strlen(to));
        if (fl && from[fl - 1] == '/' && !strncmp(spec, from, fl)) {
            AuBuf b;
            au_buf_init(&b);
            au_buf_puts(&b, to);
            au_buf_puts(&b, spec + fl);
            return au_buf_take(&b, NULL);
        }
    }
    bool rel = spec[0] == '/' || !strncmp(spec, "./", 2) || !strncmp(spec, "../", 3);
    JsUrl u;
    if (rel ? !js_url_parse(spec, base, &u) : !js_url_parse(spec, NULL, &u))
        return NULL;
    char *r = au_strndup(u.href, strlen(u.href));
    js_url_free(&u);
    return r;
}

static char *normalize(JSContext *ctx, const char *base, const char *name, void *opaque)
{
    (void)opaque; /* the runtime is shared by frames: the context names the document */
    AuJs *js = js_of_ctx(ctx);
    char *r = resolve_spec(js, name, base && *base ? base : js->base);
    if (!r) {
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier \"%s\"", name);
        return NULL;
    }
    size_t n = strlen(r);
    char *q = js_malloc(ctx, n + 1);
    if (q)
        memcpy(q, r, n + 1);
    au_free(r);
    return q;
}

static void set_meta(JSContext *ctx, JSModuleDef *md, const char *url)
{
    JSValue meta = JS_GetImportMeta(ctx, md);
    if (JS_IsObject(meta))
        JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, url));
    JS_FreeValue(ctx, meta);
}

/* Compiles a module without resolving its imports (QuickJS patch 0010):
 * the module stays in the context's loaded modules, resolved or not, so
 * an attempt that finds an import missing does not compile it again.
 * NULL with an exception for a syntax error. */
static JSModuleDef *compile_module(JSContext *ctx, const char *name, const char *src, size_t len,
                                   const char *meta_url)
{
    /* A module stored by an earlier page or run is read instead (runtime.c,
     * "Compiled scripts kept between pages"). */
    AuJs *js = js_of_ctx(ctx);
    uint8_t key[AU_JS_CODE_KEY];
    bool keyed = js_code_key(js, true, name, 1, src, len, key), seen = false;
    JSValue fn = keyed ? js_code_load(js, key, src, len, &seen) : JS_UNINITIALIZED;
    if (JS_IsUninitialized(fn)) {
        fn = JS_Eval(ctx, src, len, name,
                     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_RESOLVE |
                         (len >= JS_NO_TEXT_SCRIPT ? JS_EVAL_FLAG_NO_SOURCE : 0));
        if (keyed && !JS_IsException(fn))
            js_code_save(js, key, fn, len, seen);
    } else if (JS_VALUE_GET_TAG(fn) != JS_TAG_MODULE) {
        JS_FreeValue(ctx, fn);
        JS_ThrowInternalError(ctx, "stored code of %.200s is not a module", name);
        return NULL;
    }
    if (JS_IsException(fn))
        return NULL;
    JSModuleDef *md = JS_VALUE_GET_PTR(fn);
    set_meta(ctx, md, meta_url);
    JS_FreeValue(ctx, fn);
    return md;
}

/* QuickJS asks for each module it has not loaded. A module's text is
 * freed once it compiles: the compiled module stays loaded in the
 * context, and the engine keeps its own copy of the text (patch 0008). */
static JSModuleDef *loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque; /* the runtime is shared by frames: the context names the document */
    AuJs *js = js_of_ctx(ctx);
    Modules *m = M(js);
    ModSrc *s = find_src(m, name);
    if (!s || !s->src) {
        add_missing(m, name);
        JS_ThrowReferenceError(ctx, "module %s is not loaded yet", name);
        return NULL;
    }
    JSModuleDef *md = compile_module(ctx, name, s->src, s->len, name);
    au_free(s->src);
    s->src = NULL;
    s->len = 0;
    return md;
}

static JSValue missing_array(AuJs *js)
{
    Modules *m = M(js);
    JSValue arr = JS_NewArray(js->ctx);
    for (uint32_t i = 0; i < m->nmissing; i++)
        JS_SetPropertyUint32(js->ctx, arr, i, JS_NewString(js->ctx, m->missing[i]));
    clear_missing(m);
    return arr;
}

/* moduleEval(name, source or null, inline) -> [missing urls] or
 * {promise}.
 *
 * The first call for a module script passes its URL and, for an inline
 * module or one whose text the page already has, its source. An inline
 * module gets a unique name based on the document URL, so its relative
 * imports resolve against the document. When imports are missing, the
 * array of their URLs carries the module's name as `module`; the prelude
 * fetches them all and calls again with that name and a null source.
 * The module compiled on the first call is resolved again, not
 * compiled again: one pass reports every import that is missing (patch
 * 0010), so a module graph takes one round per level of imports, and
 * each module is compiled once. (Every round used to compile the root
 * module again and stop at its first missing import: GitHub's 980 KB
 * code view module was compiled 22 times, 2.6 s of the page's 3.5 s of
 * script on the host.) */
static JSValue n_module_eval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    Modules *m = M(js);
    char *url = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    if (!url)
        return JS_EXCEPTION;
    bool inl = argc > 2 && JS_ToBool(ctx, argv[2]);
    bool has_src = argc > 1 && JS_IsString(argv[1]);
    /* The module's name is its whole URL: a data: URL module runs to
     * tens of kilobytes (Reddit's are 1.3 and 36 KB), and a name cut
     * short is not the name the loader and moduleStore use, so the
     * module was asked for again on every round. */
    char *name = url;
    if (inl && has_src) {
        AuBuf nb;
        au_buf_init(&nb);
        au_buf_puts(&nb, url);
        au_buf_printf(&nb, "#inline-module-%lu", (unsigned long)++m->inline_seq);
        name = au_buf_take(&nb, NULL);
    }
    JSValue ret = JS_EXCEPTION;
    clear_missing(m);
    /* A module script of a URL already loaded (as another script or as an
     * import) is that module: it is not compiled or run a second time. */
    JSModuleDef *md = inl && has_src ? NULL : JS_FindLoadedModule(ctx, name);
    if (!md && has_src) {
        size_t len = 0;
        char *src = js_tostr(ctx, argv[1], &len);
        if (!src)
            goto out;
        md = compile_module(ctx, name, src, len, inl ? js->url : url);
        au_free(src);
        if (!md)
            goto out;
    } else if (!md && inl) {
        ret = JS_ThrowInternalError(ctx, "inline module %.200s is gone", name);
        goto out;
    } else if (!md) {
        ModSrc *s = find_src(m, url);
        if (!s || !s->src) {
            add_missing(m, url);
            ret = missing_array(js);
            goto out;
        }
        md = compile_module(ctx, name, s->src, s->len, url);
        au_free(s->src);
        s->src = NULL;
        s->len = 0;
        if (!md)
            goto out;
    }
    JSValue mv = JS_DupValue(ctx, JS_MKPTR(JS_TAG_MODULE, md));
    if (JS_ResolveModule(ctx, mv) < 0) {
        JS_FreeValue(ctx, mv);
        if (!m->nmissing)
            goto out;
        JSValue e = JS_GetException(ctx);
        if (JS_IsUncatchableError(e)) {
            ret = JS_Throw(ctx, e);
            goto out;
        }
        JS_FreeValue(ctx, e);
        ret = missing_array(js);
        JS_SetPropertyStr(ctx, ret, "module", JS_NewString(ctx, name));
        goto out;
    }
    JSValue p = JS_EvalFunction(ctx, mv);
    if (JS_IsException(p))
        goto out;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "promise", p);
out:
    if (name != url)
        au_free(name);
    au_free(url);
    return ret;
}

static JSValue n_module_store(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    Modules *m = M(js);
    char *url = argc > 1 ? js_tostr(ctx, argv[0], NULL) : NULL;
    size_t len;
    char *src = url ? js_tostr(ctx, argv[1], &len) : NULL;
    if (!url || !src) {
        au_free(url);
        au_free(src);
        return JS_EXCEPTION;
    }
    ModSrc *s = find_src(m, url);
    if (s) {
        /* text freed after a compile: a module that failed to compile is
         * asked for again, and gets the new text */
        if (!s->src && !JS_FindLoadedModule(ctx, url)) {
            s->src = src;
            s->len = len;
        } else {
            au_free(src);
        }
        au_free(url);
        return JS_UNDEFINED;
    }
    s = au_calloc(sizeof *s);
    s->url = url;
    s->src = src;
    s->len = len;
    s->next = m->srcs;
    m->srcs = s;
    return JS_UNDEFINED;
}

static JSValue n_module_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    char *spec = argc > 0 ? js_tostr(ctx, argv[0], NULL) : NULL;
    char *base = argc > 1 && JS_IsString(argv[1]) ? js_tostr(ctx, argv[1], NULL) : NULL;
    if (!spec) {
        au_free(base);
        return JS_EXCEPTION;
    }
    char *r = resolve_spec(js, spec, base ? base : js->base);
    JSValue v = r ? JS_NewString(ctx, r) : JS_ThrowTypeError(ctx, "Failed to resolve module specifier \"%s\"", spec);
    au_free(r);
    au_free(spec);
    au_free(base);
    return v;
}

/* moduleLoad(basename, url, token) -> [missing] or null (finished). */
static JSValue n_module_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    Modules *m = M(js);
    if (argc < 3)
        return JS_UNDEFINED;
    char *base = js_tostr(ctx, argv[0], NULL);
    char *url = js_tostr(ctx, argv[1], NULL);
    if (!base || !url) {
        au_free(base);
        au_free(url);
        return JS_EXCEPTION;
    }
    clear_missing(m);
    m->probing = true;
    JSValue p = JS_LoadModule(ctx, base, url, JS_UNDEFINED);
    m->probing = false;
    au_free(base);
    au_free(url);
    if (m->nmissing) {
        JS_FreeValue(ctx, p);
        return missing_array(js);
    }
    if (JS_IsException(p)) {
        JSValue e = JS_GetException(ctx);
        JS_FinishDynamicImport(ctx, JS_DupValue(ctx, argv[2]), JS_EXCEPTION, e);
    } else {
        JS_FinishDynamicImport(ctx, JS_DupValue(ctx, argv[2]), p, JS_UNDEFINED);
    }
    return JS_NULL;
}

/* moduleFail(token, error): rejects a dynamic import. */
static JSValue n_module_fail(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_UNDEFINED;
    JS_FinishDynamicImport(ctx, JS_DupValue(ctx, argv[0]), JS_EXCEPTION, JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue n_set_import_map(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    Modules *m = M(js);
    int64_t n = 0;
    if (argc < 1 || JS_GetLength(ctx, argv[0], &n) || n < 0 || n > 1000)
        return JS_UNDEFINED;
    for (int64_t i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        JSValue a = JS_GetPropertyUint32(ctx, pair, 0), b = JS_GetPropertyUint32(ctx, pair, 1);
        char *as = js_tostr(ctx, a, NULL), *bs = js_tostr(ctx, b, NULL);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, pair);
        if (as && bs) {
            m->map = au_realloc(m->map, (m->nmap + 1) * 2 * sizeof *m->map);
            m->map[m->nmap * 2] = as;
            m->map[m->nmap * 2 + 1] = bs;
            m->nmap++;
        } else {
            au_free(as);
            au_free(bs);
        }
    }
    return JS_UNDEFINED;
}

static void dynamic_import(JSContext *ctx, const char *basename, const char *filename, JSValue token,
                           JSValueConst attributes, void *opaque)
{
    (void)opaque; /* the runtime is shared by frames: the context names the document */
    AuJs *js = js_of_ctx(ctx);
    JSValue args[3] = {JS_NewString(ctx, basename ? basename : js->base), JS_NewString(ctx, filename), token};
    JSValue r = js_call_prelude(js, "dynamicImport", 3, args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, token);
}

/* ---- Module jobs (au_js_run_module) ----
 * The prelude reports completion through moduleFinished(id, ok). */
typedef struct ModJob {
    uint32_t id;
    void (*done)(void *ctx, AuElement *el, bool ok);
    void *ctx;
    AuElement *el; /* referenced */
    struct ModJob *next;
} ModJob;

static JSValue n_module_finished(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    Modules *m = M(js);
    uint32_t id = 0;
    if (argc < 1 || JS_ToUint32(ctx, &id, argv[0]))
        return JS_UNDEFINED;
    bool ok = argc > 1 && JS_ToBool(ctx, argv[1]);
    for (ModJob **pp = &m->jobs; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) {
            ModJob *j = *pp;
            *pp = j->next;
            if (j->done)
                j->done(j->ctx, j->el, ok);
            if (j->el)
                au_node_unref(&j->el->node);
            au_free(j);
            break;
        }
    return JS_UNDEFINED;
}

void au_js_run_module(AuJs *js, AuElement *el, const char *src, size_t len, const char *url,
                      void (*done)(void *ctx, AuElement *el, bool ok), void *ctx)
{
    Modules *m = M(js);
    if (js->closing || !m || js->settings.scripts_off) {
        if (done)
            done(ctx, el, false);
        return;
    }
    ModJob *j = au_calloc(sizeof *j);
    j->id = ++m->next_job;
    j->done = done;
    j->ctx = ctx;
    j->el = el ? (AuElement *)au_node_ref(&el->node) : NULL;
    j->next = m->jobs;
    m->jobs = j;
    JSContext *c = js->ctx;
    JSValue args[4];
    args[0] = JS_NewString(c, url ? url : js->url);
    args[1] = src ? js_str(c, src, len) : JS_NULL;
    args[2] = js_wrap(js, el ? &el->node : NULL);
    args[3] = JS_NewUint32(c, j->id);
    JSValue r = js_call_prelude(js, "runModule", 4, args);
    JS_FreeValue(c, r);
    for (int i = 0; i < 4; i++)
        JS_FreeValue(c, args[i]);
    au_js_checkpoint(js);
}

static const JSCFunctionListEntry module_natives[] = {
    JS_CFUNC_DEF("moduleFinished", 2, n_module_finished),
    JS_CFUNC_DEF("moduleEval", 3, n_module_eval),
    JS_CFUNC_DEF("moduleStore", 2, n_module_store),
    JS_CFUNC_DEF("moduleResolve", 2, n_module_resolve),
    JS_CFUNC_DEF("moduleLoad", 3, n_module_load),
    JS_CFUNC_DEF("moduleFail", 2, n_module_fail),
    JS_CFUNC_DEF("setImportMap", 1, n_set_import_map),
};

void js_module_init(AuJs *js)
{
    js->modules = au_calloc(sizeof(Modules));
    if (js->root != js)
        return; /* the runtime's loader finds each document by its context */
    JS_SetModuleLoaderFunc(js->rt, normalize, loader, js);
    JS_SetDynamicImportFunc(js->rt, dynamic_import, js);
}

void js_module_natives(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, module_natives, JS_COUNTOF(module_natives));
}

void js_module_free(AuJs *js)
{
    Modules *m = M(js);
    if (!m)
        return;
    clear_missing(m);
    while (m->jobs) {
        ModJob *j = m->jobs;
        m->jobs = j->next;
        if (j->el)
            au_node_unref(&j->el->node);
        au_free(j);
    }
    while (m->srcs) {
        ModSrc *s = m->srcs;
        m->srcs = s->next;
        au_free(s->url);
        au_free(s->src);
        au_free(s);
    }
    for (uint32_t i = 0; i < m->nmap * 2; i++)
        au_free(m->map[i]);
    au_free(m->map);
    au_free(m);
    js->modules = NULL;
}

bool js_modules_pending(const AuJs *js)
{
    return js->modules && ((Modules *)js->modules)->jobs != NULL;
}

bool js_module_probing(const AuJs *js)
{
    return js->modules && ((Modules *)js->modules)->probing;
}
