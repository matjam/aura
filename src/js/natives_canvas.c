/* The canvas bindings: CanvasRenderingContext2D, CanvasGradient,
 * CanvasPattern, Path2D, ImageBitmap and TextMetrics over
 * src/paint/canvas2d.h. The context's methods and attributes are C
 * functions on its prototype (script calls them thousands of times per
 * frame); canvas.js (read on first use) adds the element's methods, ImageData,
 * OffscreenCanvas and the few context methods that build script objects.
 *
 * Ownership. A context object owns its AuCanvas2D and holds its canvas
 * (the element's wrapper or the OffscreenCanvas) in its canvas property,
 * so the element outlives the context. The AuCanvas2D belongs to the
 * document's canvas set (AuJsHooks.canvas_set; without the hook, a set of
 * this AuJs's own). A set that goes before the script objects detaches
 * them: they stay valid and draw nothing.
 *
 * Origin-clean. js_canvas_image_origin_clean decides whether an image
 * may be read back after it is drawn; drawing one that fails it taints
 * the canvas, and getImageData, toDataURL and toBlob then throw
 * SecurityError. */
#include "js_int.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#endif
#include "../svg/svg.h"
#include "../image/image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "../image/pngenc.h"
#include "../paint/canvas2d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* canvas_css.c */
bool js_canvas_parse_color(const char *s, size_t len, const void *style, uint32_t *argb);
bool js_canvas_parse_font(const char *s, size_t len, const void *style, char **families, char **text, int *weight,
                          bool *italic, float *size);

#define CV_PENDING 8
#define CV_COLOR_CACHE 8

struct JsCanvas {
    JSClassID cls_ctx, cls_grad, cls_pat, cls_path, cls_bmp;
    JSValue tm_proto;
    AuCanvasSet *own_set;
    /* Canvas elements whose width or height attribute is changing: their
     * bitmaps are resized at the next canvas call or script exit. */
    AuElement *pending[CV_PENDING];
    int npending;
    bool pending_all;
    /* Colour strings parsed last (not hex ones, which need no parser). */
    struct {
        char text[40];
        uint32_t argb;
    } colors[CV_COLOR_CACHE];
    unsigned color_next;
};

typedef struct JsCanvas JsCanvas;

static JsCanvas *jc_of(JSContext *ctx)
{
    return js_of_ctx(ctx)->canvas;
}

static AuCanvasSet *cv_set(AuJs *js)
{
    if (js->hooks.canvas_set) {
        AuCanvasSet *s = js->hooks.canvas_set(js->hooks.ctx);
        if (s)
            return s;
    }
    JsCanvas *jc = js->canvas;
    if (!jc->own_set)
        jc->own_set = au_canvas_set_new(0);
    return jc->own_set;
}

/* ---- The origin-clean rule ----
 * Whether an image fetched from url may be read back by script of js's
 * document once drawn to a canvas: an image from a data: or blob: URL,
 * one of the document's origin, or one whose response passed the CORS
 * check because its <img> has a crossorigin attribute. The embedder
 * knows the last two (AuJsHooks.image_origin_clean, on
 * au_docview_image_origin_clean); without the hook the URL decides. */
bool js_canvas_image_origin_clean(AuJs *js, const char *url);
bool js_canvas_image_origin_clean(AuJs *js, const char *url)
{
    if (!url || !*url)
        return true;
    if (!strncmp(url, "data:", 5) || !strncmp(url, "blob:", 5))
        return true;
    if (js->hooks.image_origin_clean && (!strncmp(url, "http:", 5) || !strncmp(url, "https:", 6)))
        return js->hooks.image_origin_clean(js->hooks.ctx, url);
    if (js_same_origin(js, url))
        return true;
    /* Local files are one origin for a document that is a local file. */
    return !strncmp(url, "file:", 5) && !strncmp(js->url, "file:", 5);
}

/* ---- Canvas size attributes ---- */

static int attr_size(AuElement *el, const char *name, int def)
{
    const char *v = au_element_get_attr(el, au_atom(name, strlen(name)));
    if (!v)
        return def;
    while (au_is_space((unsigned char)*v))
        v++;
    if (*v == '+')
        v++;
    if (!au_is_digit((unsigned char)*v))
        return def;
    long n = 0;
    while (au_is_digit((unsigned char)*v)) {
        n = n * 10 + (*v - '0');
        if (n > 1000000)
            n = 1000000;
        v++;
    }
    return (int)n;
}

void js_canvas_attr_changing(AuJs *js, AuElement *el, AuAtom name)
{
    JsCanvas *jc = js->canvas;
    if (!jc || el->ns != AU_NS_HTML || el->tag != AU_ATOM_canvas)
        return;
    if (name != au_atom("width", 5) && name != au_atom("height", 6))
        return;
    for (int i = 0; i < jc->npending; i++)
        if (jc->pending[i] == el)
            return;
    if (jc->npending < CV_PENDING)
        jc->pending[jc->npending++] = el;
    else
        jc->pending_all = true;
    js->canvas_pending = true;
}

/* Brings the bitmaps of canvases whose size attributes changed up to
 * date. The entries are only compared with canvas owners, never
 * dereferenced unless a live canvas names them. */
void js_canvas_sync(AuJs *js)
{
    JsCanvas *jc = js->canvas;
    js->canvas_pending = false;
    if (!jc)
        return;
    AuCanvasSet *set = cv_set(js);
    for (int i = 0; i < jc->npending; i++) {
        AuElement *el = jc->pending[i];
        AuCanvas2D *c = au_canvas_set_find(set, el);
        if (c)
            au_c2d_resize(c, attr_size(el, "width", 300), attr_size(el, "height", 150));
    }
    jc->npending = 0;
    jc->pending_all = false;
}

/* ---- Helpers ---- */

static JSValue throw_dom(JSContext *ctx, const char *name, const char *msg)
{
    return JS_ThrowDOMException(ctx, name, "%s", msg);
}

static AuCanvas2D *this_c2d(JSContext *ctx, JSValueConst v)
{
    AuJs *js = js_of_ctx(ctx);
    if (js->canvas_pending)
        js_canvas_sync(js);
    return JS_GetOpaque2(ctx, v, js->canvas->cls_ctx);
}

/* Reads n numbers; -1 after an exception (also thrown for too few
 * arguments). */
static int args_f64(JSContext *ctx, int argc, JSValueConst *argv, int first, int n, double *out)
{
    if (argc < first + n) {
        JS_ThrowTypeError(ctx, "not enough arguments");
        return -1;
    }
    for (int i = 0; i < n; i++)
        if (JS_ToFloat64(ctx, &out[i], argv[first + i]))
            return -1;
    return 0;
}

/* A fill rule argument: 1 parsed, 0 absent, -1 after an exception. */
static int arg_rule(JSContext *ctx, int argc, JSValueConst *argv, int i, AuFillRule *rule)
{
    *rule = AU_FILL_NONZERO;
    if (i >= argc || JS_IsUndefined(argv[i]))
        return 0;
    const char *s = JS_ToCString(ctx, argv[i]);
    if (!s)
        return -1;
    int r = 1;
    if (!strcmp(s, "evenodd"))
        *rule = AU_FILL_EVENODD;
    else if (strcmp(s, "nonzero")) {
        JS_ThrowTypeError(ctx, "'%s' is not a fill rule", s);
        r = -1;
    }
    JS_FreeCString(ctx, s);
    return r;
}

/* ---- Finalizers ---- */

static void fin_ctx(JSRuntime *rt, JSValueConst v)
{
    (void)rt;
    /* Class ids are the runtime's; any AuJs of it has them, but none is
     * at hand here: the id is read from the object's own class. */
    AuCanvas2D *c = JS_GetOpaque(v, JS_GetClassID(v));
    au_c2d_free(c);
}

static void fin_grad(JSRuntime *rt, JSValueConst v)
{
    (void)rt;
    AuCanvasGradient *g = JS_GetOpaque(v, JS_GetClassID(v));
    if (g) {
        *au_cgradient_user(g) = NULL;
        au_cgradient_unref(g);
    }
}

static void fin_pat(JSRuntime *rt, JSValueConst v)
{
    (void)rt;
    AuCanvasPattern *p = JS_GetOpaque(v, JS_GetClassID(v));
    if (p) {
        *au_cpattern_user(p) = NULL;
        au_cpattern_unref(p);
    }
}

static void fin_path(JSRuntime *rt, JSValueConst v)
{
    (void)rt;
    AuCanvasPath *p = JS_GetOpaque(v, JS_GetClassID(v));
    if (p) {
        au_cpath_free(p);
        au_free(p);
    }
}

static void fin_bmp(JSRuntime *rt, JSValueConst v)
{
    (void)rt;
    AuCanvasBitmap **b = JS_GetOpaque(v, JS_GetClassID(v));
    if (b) {
        au_cbitmap_unref(*b);
        au_free(b);
    }
}

static JSClassDef def_ctx = {"CanvasRenderingContext2D", .finalizer = fin_ctx};
static JSClassDef def_grad = {"CanvasGradient", .finalizer = fin_grad};
static JSClassDef def_pat = {"CanvasPattern", .finalizer = fin_pat};
static JSClassDef def_path = {"Path2D", .finalizer = fin_path};
static JSClassDef def_bmp = {"ImageBitmap", .finalizer = fin_bmp};

/* ---- Colours ---- */

static bool parse_color(JSContext *ctx, AuCanvas2D *c, const char *s, size_t len, uint32_t *argb)
{
    AuJs *js = js_of_ctx(ctx);
    JsCanvas *jc = js->canvas;
    if (len && s[0] == '#')
        return js_canvas_parse_color(s, len, NULL, argb);
    bool cacheable = len < sizeof jc->colors[0].text;
    if (cacheable)
        for (int i = 0; i < CV_COLOR_CACHE; i++)
            if (jc->colors[i].text[0] && !strcmp(jc->colors[i].text, s)) {
                *argb = jc->colors[i].argb;
                return true;
            }
    if (!js_canvas_parse_color(s, len, NULL, argb))
        return false;
    if (cacheable) {
        unsigned slot = jc->color_next++ % CV_COLOR_CACHE;
        memcpy(jc->colors[slot].text, s, len + 1);
        jc->colors[slot].argb = *argb;
    }
    return true;
}

static JSValue color_string(JSContext *ctx, uint32_t argb)
{
    char buf[64];
    uint32_t a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    if (a == 255) {
        snprintf(buf, sizeof buf, "#%02x%02x%02x", (unsigned)r, (unsigned)g, (unsigned)b);
    } else {
        /* The shortest of two or three decimals that gives the alpha
         * back, as CSS serializes it. */
        double f = floor(a / 255.0 * 100 + 0.5) / 100;
        if ((uint32_t)floor(f * 255 + 0.5) != a)
            f = floor(a / 255.0 * 1000 + 0.5) / 1000;
        snprintf(buf, sizeof buf, "rgba(%u, %u, %u, %.3g)", (unsigned)r, (unsigned)g, (unsigned)b, f);
    }
    return JS_NewString(ctx, buf);
}

/* ---- Gradients and patterns as script objects ---- */

static JSValue wrap_gradient(JSContext *ctx, AuCanvasGradient *g)
{
    void **user = au_cgradient_user(g);
    if (*user)
        return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, *user));
    JSValue o = JS_NewObjectClass(ctx, (int)jc_of(ctx)->cls_grad);
    if (JS_IsException(o))
        return o;
    JS_SetOpaque(o, au_cgradient_ref(g));
    *user = JS_VALUE_GET_PTR(o);
    return o;
}

static JSValue wrap_pattern(JSContext *ctx, AuCanvasPattern *p)
{
    void **user = au_cpattern_user(p);
    if (*user)
        return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, *user));
    JSValue o = JS_NewObjectClass(ctx, (int)jc_of(ctx)->cls_pat);
    if (JS_IsException(o))
        return o;
    JS_SetOpaque(o, au_cpattern_ref(p));
    *user = JS_VALUE_GET_PTR(o);
    return o;
}

static JSValue grad_add_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuCanvasGradient *g = JS_GetOpaque2(ctx, this_val, jc_of(ctx)->cls_grad);
    double off;
    if (!g || args_f64(ctx, argc, argv, 0, 1, &off))
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "not enough arguments");
    if (!(off >= 0 && off <= 1))
        return throw_dom(ctx, "IndexSizeError", "The offset is outside 0 to 1");
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!s)
        return JS_EXCEPTION;
    uint32_t argb;
    bool ok = js_canvas_parse_color(s, n, NULL, &argb);
    JS_FreeCString(ctx, s);
    if (!ok)
        return throw_dom(ctx, "SyntaxError", "The colour could not be parsed");
    au_cgradient_add_stop(g, off, argb);
    return JS_UNDEFINED;
}

/* A DOMMatrix2DInit-like object: a..f, else m11, m12, m21, m22, m41, m42. */
static int matrix_init(JSContext *ctx, JSValueConst o, double m[6])
{
    static const char *const names[6][2] = {{"a", "m11"}, {"b", "m12"}, {"c", "m21"},
                                            {"d", "m22"}, {"e", "m41"}, {"f", "m42"}};
    static const double def[6] = {1, 0, 0, 1, 0, 0};
    for (int i = 0; i < 6; i++) {
        m[i] = def[i];
        if (!JS_IsObject(o))
            continue;
        for (int k = 0; k < 2; k++) {
            JSValue v = JS_GetPropertyStr(ctx, o, names[i][k]);
            if (JS_IsException(v))
                return -1;
            if (!JS_IsUndefined(v)) {
                int r = JS_ToFloat64(ctx, &m[i], v);
                JS_FreeValue(ctx, v);
                if (r)
                    return -1;
                break;
            }
        }
    }
    return 0;
}

static JSValue pat_set_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuCanvasPattern *p = JS_GetOpaque2(ctx, this_val, jc_of(ctx)->cls_pat);
    if (!p)
        return JS_EXCEPTION;
    double m[6];
    if (matrix_init(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, m))
        return JS_EXCEPTION;
    au_cpattern_set_transform(p, m[0], m[1], m[2], m[3], m[4], m[5]);
    return JS_UNDEFINED;
}

/* ---- Image sources ---- */

typedef struct Src {
    const AuSurface *surf;
    int nat_w, nat_h;
    bool opaque, clean;
    AuCanvasBitmap *hold; /* referenced while the source is used, or NULL */
} Src;

static void src_done(Src *s)
{
    au_cbitmap_unref(s->hold);
    s->hold = NULL;
}

static void src_bitmap(Src *s, AuCanvasBitmap *b)
{
    s->hold = au_cbitmap_ref(b);
    s->surf = b->surf;
    s->nat_w = b->nat_w;
    s->nat_h = b->nat_h;
    s->clean = b->clean;
    s->opaque = false;
}

static JSValue new_bitmap_object(JSContext *ctx, AuCanvasBitmap *b)
{
    JSValue o = JS_NewObjectClass(ctx, (int)jc_of(ctx)->cls_bmp);
    if (JS_IsException(o))
        return o;
    AuCanvasBitmap **slot = au_alloc(sizeof *slot);
    *slot = au_cbitmap_ref(b);
    JS_SetOpaque(o, slot);
    return o;
}

/* Decodes a whole image file at full size into a bitmap of the set. */
static AuCanvasBitmap *decode_bitmap(AuJs *js, const uint8_t *data, size_t len)
{
    AuImageOptions opt;
    memset(&opt, 0, sizeof opt);
    opt.max_bytes = AU_C2D_MAX_BYTES;
    opt.first_frame_only = true;
    AuImageInfo info;
    memset(&info, 0, sizeof info);
    AuSurface *surf = au_image_decode_buffer(data, len, &opt, &info);
    if (!surf)
        return NULL;
    return au_cbitmap_adopt(cv_set(js), surf, info.width > 0 ? info.width : surf->width,
                            info.height > 0 ? info.height : surf->height);
}

/* The absolute URL of an image element's src, into out. */
static void img_url(AuJs *js, AuElement *el, char *out, size_t cap)
{
    out[0] = 0;
    const char *src = au_element_get_attr(el, au_atom("src", 3));
    if (!src || !*src)
        return;
    JsUrl u;
    if (js_url_parse(src, js->base, &u)) {
        snprintf(out, cap, "%s", u.href);
        js_url_free(&u);
    }
}

/* Resolves a CanvasImageSource. 1: s is filled; 0: nothing to draw (an
 * image that is not loaded, a canvas never drawn to); -1: an exception
 * was thrown. */
static int image_source(JSContext *ctx, JSValueConst v, Src *s)
{
    AuJs *js = js_of_ctx(ctx);
    JsCanvas *jc = js->canvas;
    memset(s, 0, sizeof *s);
    s->clean = true;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "The image is not an image, canvas or ImageBitmap");
        return -1;
    }
    AuCanvasBitmap **bp = JS_GetOpaque(v, jc->cls_bmp);
    if (bp) {
        if (!*bp) {
            throw_dom(ctx, "InvalidStateError", "The ImageBitmap is closed");
            return -1;
        }
        src_bitmap(s, *bp);
        return s->surf ? 1 : 0;
    }
    AuCanvas2D *c = NULL;
    bool is_canvas = false;
    AuNode *n = js_unwrap(js, v);
    AuElement *el = n && au_is_element(n) ? au_as_element(n) : NULL;
    if (el && el->ns == AU_NS_HTML && el->tag == AU_ATOM_canvas) {
        if (js->canvas_pending)
            js_canvas_sync(js);
        is_canvas = true;
        c = au_canvas_set_find(cv_set(js), el);
        if (!c) {
            /* No context yet: a blank canvas of the attributes' size. */
            if (attr_size(el, "width", 300) == 0 || attr_size(el, "height", 150) == 0) {
                throw_dom(ctx, "InvalidStateError", "The canvas has no pixels");
                return -1;
            }
            return 0;
        }
    } else if (!n) {
        /* An OffscreenCanvas keeps its context in _c2d. */
        JSValue cv = JS_GetPropertyStr(ctx, v, "_c2d");
        if (JS_IsException(cv))
            return -1;
        c = JS_GetOpaque(cv, jc->cls_ctx);
        JS_FreeValue(ctx, cv);
        if (c) {
            is_canvas = true;
        } else {
            JSValue w = JS_GetPropertyStr(ctx, v, "_ow");
            bool off = !JS_IsUndefined(w) && !JS_IsException(w);
            JS_FreeValue(ctx, w);
            if (off)
                return 0; /* an OffscreenCanvas without a context: blank */
        }
    }
    if (is_canvas) {
        if (au_c2d_width(c) == 0 || au_c2d_height(c) == 0) {
            throw_dom(ctx, "InvalidStateError", "The canvas has no pixels");
            return -1;
        }
        if (!au_c2d_alpha(c))
            au_c2d_materialize(c); /* opaque black even when nothing was drawn */
        s->surf = au_c2d_surface(c);
        s->nat_w = au_c2d_width(c);
        s->nat_h = au_c2d_height(c);
        s->opaque = !au_c2d_alpha(c);
        s->clean = au_c2d_clean(c);
        return s->surf ? 1 : 0;
    }
    if (el && el->ns == AU_NS_HTML && el->tag == AU_ATOM_img) {
        char url[2048];
        img_url(js, el, url, sizeof url);
        /* An image script loaded: the bitmap decoded before, or its bytes. */
        JSValue bv = JS_GetPropertyStr(ctx, v, "_ibmp");
        if (JS_IsException(bv))
            return -1;
        bp = JS_GetOpaque(bv, jc->cls_bmp);
        JS_FreeValue(ctx, bv);
        if (bp && *bp) {
            src_bitmap(s, *bp);
            return 1;
        }
        JSValue by = JS_GetPropertyStr(ctx, v, "_ibytes");
        if (JS_IsException(by))
            return -1;
        size_t len = 0;
        uint8_t *data = JS_IsObject(by) && JS_GetTypedArrayType(by) == JS_TYPED_ARRAY_UINT8
                            ? JS_GetUint8Array(ctx, &len, by)
                            : NULL;
        if (data && len) {
            AuCanvasBitmap *b = decode_bitmap(js, data, len);
            JS_FreeValue(ctx, by);
            if (!b)
                return 0;
            b->clean = js_canvas_image_origin_clean(js, url);
            JSValue o = new_bitmap_object(ctx, b);
            src_bitmap(s, b);
            au_cbitmap_unref(b);
            if (!JS_IsException(o))
                JS_SetPropertyStr(ctx, v, "_ibmp", o);
            /* The compressed bytes are no longer needed. */
            JS_SetPropertyStr(ctx, v, "_ibytes", JS_NULL);
            return 1;
        }
        JS_FreeValue(ctx, by);
        /* An image the document loaded. */
        if (js->hooks.canvas_image) {
            const AuSurface *px = NULL;
            int w = 0, h = 0;
            char durl[2048];
            durl[0] = 0;
            if (js->hooks.canvas_image(js->hooks.ctx, el, &px, &w, &h, durl, sizeof durl) && px && w > 0 && h > 0) {
                s->surf = px;
                s->nat_w = w;
                s->nat_h = h;
                s->opaque = px->opaque;
                s->clean = js_canvas_image_origin_clean(js, durl[0] ? durl : url);
                return 1;
            }
        }
        return 0;
    }
    if (el && el->ns == AU_NS_HTML && el->tag == AU_ATOM_video)
        return 0; /* no video frames */
    JS_ThrowTypeError(ctx, "The image is not an image, canvas or ImageBitmap");
    return -1;
}

/* A bitmap holding a copy of (or a reference to) a source's pixels. */
static AuCanvasBitmap *src_to_bitmap(AuJs *js, const Src *s)
{
    if (s->hold && !s->opaque)
        return au_cbitmap_ref(s->hold);
    AuCanvasBitmap *b = au_cbitmap_new(cv_set(js), s->surf->width, s->surf->height);
    if (!b)
        return NULL;
    for (int y = 0; y < s->surf->height; y++) {
        const AuPixel *sp = s->surf->pixels + (size_t)y * s->surf->stride;
        AuPixel *dp = b->surf->pixels + (size_t)y * b->surf->stride;
        for (int x = 0; x < s->surf->width; x++)
            dp[x] = s->opaque ? sp[x] | 0xFF000000u : sp[x];
    }
    b->nat_w = s->nat_w;
    b->nat_h = s->nat_h;
    b->clean = s->clean;
    return b;
}

/* ---- Context: attributes ---- */

enum {
    A_GLOBAL_ALPHA, A_COMPOSITE, A_SMOOTHING, A_SMOOTHING_QUALITY, A_FILL_STYLE, A_STROKE_STYLE, A_LINE_WIDTH,
    A_LINE_CAP, A_LINE_JOIN, A_MITER, A_DASH_OFFSET, A_SHADOW_COLOR, A_SHADOW_BLUR, A_SHADOW_X, A_SHADOW_Y,
    A_FONT, A_TEXT_ALIGN, A_TEXT_BASELINE, A_DIRECTION, A_LETTER_SPACING, A_WORD_SPACING, A_KERNING, A_FILTER,
    A_FONT_STRETCH, A_FONT_VARIANT_CAPS, A_TEXT_RENDERING
};

static JSValue px_string(JSContext *ctx, double v)
{
    char buf[40];
    snprintf(buf, sizeof buf, "%.6gpx", v);
    return JS_NewString(ctx, buf);
}

static JSValue c2d_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    switch (magic) {
    case A_GLOBAL_ALPHA: return JS_NewFloat64(ctx, au_c2d_get_alpha(c));
    case A_COMPOSITE: return JS_NewString(ctx, au_c2d_get_composite(c));
    case A_SMOOTHING: return JS_NewBool(ctx, au_c2d_get_smoothing(c));
    case A_SMOOTHING_QUALITY: return JS_NewString(ctx, au_c2d_get_smoothing_quality(c));
    case A_FILL_STYLE:
    case A_STROKE_STYLE: {
        int which = magic == A_FILL_STYLE ? AU_C2D_FILL : AU_C2D_STROKE;
        switch (au_c2d_style_kind(c, which)) {
        case AU_C2D_STYLE_GRADIENT: return wrap_gradient(ctx, au_c2d_style_gradient(c, which));
        case AU_C2D_STYLE_PATTERN: return wrap_pattern(ctx, au_c2d_style_pattern(c, which));
        default: return color_string(ctx, au_c2d_style_color(c, which));
        }
    }
    case A_LINE_WIDTH: return JS_NewFloat64(ctx, au_c2d_get_line_width(c));
    case A_LINE_CAP: return JS_NewString(ctx, au_c2d_get_line_cap(c));
    case A_LINE_JOIN: return JS_NewString(ctx, au_c2d_get_line_join(c));
    case A_MITER: return JS_NewFloat64(ctx, au_c2d_get_miter_limit(c));
    case A_DASH_OFFSET: return JS_NewFloat64(ctx, au_c2d_get_dash_offset(c));
    case A_SHADOW_COLOR: return color_string(ctx, au_c2d_get_shadow_color(c));
    case A_SHADOW_BLUR: return JS_NewFloat64(ctx, au_c2d_get_shadow_blur(c));
    case A_SHADOW_X: return JS_NewFloat64(ctx, au_c2d_get_shadow_offset(c, 0));
    case A_SHADOW_Y: return JS_NewFloat64(ctx, au_c2d_get_shadow_offset(c, 1));
    case A_FONT: return JS_NewString(ctx, au_c2d_get_font(c));
    case A_TEXT_ALIGN: return JS_NewString(ctx, au_c2d_get_text_align(c));
    case A_TEXT_BASELINE: return JS_NewString(ctx, au_c2d_get_text_baseline(c));
    case A_DIRECTION: return JS_NewString(ctx, au_c2d_get_direction(c));
    case A_LETTER_SPACING: return px_string(ctx, au_c2d_get_spacing(c, 0));
    case A_WORD_SPACING: return px_string(ctx, au_c2d_get_spacing(c, 1));
    case A_KERNING: return JS_NewString(ctx, au_c2d_get_kerning(c));
    case A_FILTER: return JS_NewString(ctx, "none");
    case A_FONT_STRETCH:
    case A_FONT_VARIANT_CAPS: return JS_NewString(ctx, "normal");
    default: return JS_NewString(ctx, "auto");
    }
}

/* The computed style of the context's canvas element, or NULL. */
static const void *canvas_style(JSContext *ctx, JSValueConst this_val)
{
    AuJs *js = js_of_ctx(ctx);
    if (!js->hooks.computed_style)
        return NULL;
    JSValue cv = JS_GetPropertyStr(ctx, this_val, "canvas");
    AuNode *n = JS_IsException(cv) ? NULL : js_unwrap(js, cv);
    JS_FreeValue(ctx, cv);
    if (!n || !au_is_element(n) || !au_node_is_connected(n))
        return NULL;
    return js->hooks.computed_style(js->hooks.ctx, au_as_element(n));
}

/* "12px": a length in pixels for letterSpacing and wordSpacing. */
static bool parse_px(const char *s, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || !isfinite(v))
        return false;
    if (*end == 0 && v == 0) {
        *out = 0;
        return true;
    }
    if (au_lower((unsigned char)end[0]) != 'p' || au_lower((unsigned char)end[1]) != 'x' || end[2])
        return false;
    *out = v;
    return true;
}

static JSValue c2d_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    switch (magic) {
    case A_GLOBAL_ALPHA:
    case A_LINE_WIDTH:
    case A_MITER:
    case A_DASH_OFFSET:
    case A_SHADOW_BLUR:
    case A_SHADOW_X:
    case A_SHADOW_Y: {
        double d;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        switch (magic) {
        case A_GLOBAL_ALPHA: au_c2d_set_alpha(c, d); break;
        case A_LINE_WIDTH: au_c2d_set_line_width(c, d); break;
        case A_MITER: au_c2d_set_miter_limit(c, d); break;
        case A_DASH_OFFSET: au_c2d_set_dash_offset(c, d); break;
        case A_SHADOW_BLUR: au_c2d_set_shadow_blur(c, d); break;
        case A_SHADOW_X: au_c2d_set_shadow_offset(c, 0, d); break;
        default: au_c2d_set_shadow_offset(c, 1, d); break;
        }
        return JS_UNDEFINED;
    }
    case A_SMOOTHING:
        au_c2d_set_smoothing(c, JS_ToBool(ctx, val) > 0);
        return JS_UNDEFINED;
    case A_FILL_STYLE:
    case A_STROKE_STYLE: {
        int which = magic == A_FILL_STYLE ? AU_C2D_FILL : AU_C2D_STROKE;
        JsCanvas *jc = jc_of(ctx);
        if (JS_IsObject(val)) {
            AuCanvasGradient *g = JS_GetOpaque(val, jc->cls_grad);
            if (g) {
                au_c2d_set_gradient(c, which, g);
                return JS_UNDEFINED;
            }
            AuCanvasPattern *p = JS_GetOpaque(val, jc->cls_pat);
            if (p) {
                au_c2d_set_pattern(c, which, p);
                /* A pattern made from an image that may not be read back
                 * taints the canvas when it is chosen. */
                if (!au_cpattern_clean(p))
                    au_c2d_taint(c);
                return JS_UNDEFINED;
            }
        }
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, val);
        if (!s)
            return JS_EXCEPTION;
        uint32_t argb;
        bool ok;
        if (n >= 12 && strstr(s, "urrent")) /* currentcolor: needs the element's style */
            ok = js_canvas_parse_color(s, n, canvas_style(ctx, this_val), &argb);
        else
            ok = parse_color(ctx, c, s, n, &argb);
        JS_FreeCString(ctx, s);
        if (ok)
            au_c2d_set_color(c, which, argb);
        return JS_UNDEFINED;
    }
    default:
        break;
    }
    /* The rest take a string; an unknown value changes nothing. */
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, val);
    if (!s)
        return JS_EXCEPTION;
    switch (magic) {
    case A_COMPOSITE: au_c2d_set_composite(c, s); break;
    case A_SMOOTHING_QUALITY: au_c2d_set_smoothing_quality(c, s); break;
    case A_LINE_CAP: au_c2d_set_line_cap(c, s); break;
    case A_LINE_JOIN: au_c2d_set_line_join(c, s); break;
    case A_TEXT_ALIGN: au_c2d_set_text_align(c, s); break;
    case A_TEXT_BASELINE: au_c2d_set_text_baseline(c, s); break;
    case A_DIRECTION: au_c2d_set_direction(c, s); break;
    case A_KERNING: au_c2d_set_kerning(c, s); break;
    case A_SHADOW_COLOR: {
        uint32_t argb;
        if (n >= 12 && strstr(s, "urrent") ? js_canvas_parse_color(s, n, canvas_style(ctx, this_val), &argb)
                                           : parse_color(ctx, c, s, n, &argb))
            au_c2d_set_shadow_color(c, argb);
        break;
    }
    case A_LETTER_SPACING:
    case A_WORD_SPACING: {
        double px;
        if (parse_px(s, &px))
            au_c2d_set_spacing(c, magic == A_WORD_SPACING, px);
        break;
    }
    case A_FONT:
        /* The text the attribute reads back parses to itself: nothing to do. */
        if (strcmp(s, au_c2d_get_font(c)) != 0) {
            char *fam = NULL, *text = NULL;
            int weight = 400;
            bool italic = false;
            float size = 10;
            if (js_canvas_parse_font(s, n, canvas_style(ctx, this_val), &fam, &text, &weight, &italic, &size)) {
                au_c2d_set_font(c, fam, weight, italic, size, text);
                au_free(fam);
                au_free(text);
            }
        }
        break;
    default:
        break; /* filter, fontStretch, fontVariantCaps, textRendering: accepted, not applied */
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- Context: methods with number arguments ---- */

enum {
    M_SAVE, M_RESTORE, M_RESET, M_RESET_TRANSFORM, M_BEGIN_PATH, M_SCALE, M_ROTATE, M_TRANSLATE, M_TRANSFORM,
    M_CLEAR_RECT, M_FILL_RECT, M_STROKE_RECT
};

static JSValue c2d_simple(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const uint8_t nargs[] = {0, 0, 0, 0, 0, 2, 1, 2, 6, 4, 4, 4};
    AuCanvas2D *c = this_c2d(ctx, this_val);
    double a[6];
    if (!c || args_f64(ctx, argc, argv, 0, nargs[magic], a))
        return JS_EXCEPTION;
    switch (magic) {
    case M_SAVE: au_c2d_save(c); break;
    case M_RESTORE: au_c2d_restore(c); break;
    case M_RESET: au_c2d_reset(c); break;
    case M_RESET_TRANSFORM: au_c2d_set_transform(c, 1, 0, 0, 1, 0, 0); break;
    case M_BEGIN_PATH: au_c2d_begin_path(c); break;
    case M_SCALE: au_c2d_scale(c, a[0], a[1]); break;
    case M_ROTATE: au_c2d_rotate(c, a[0]); break;
    case M_TRANSLATE: au_c2d_translate(c, a[0], a[1]); break;
    case M_TRANSFORM: au_c2d_transform(c, a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case M_CLEAR_RECT: au_c2d_clear_rect(c, a[0], a[1], a[2], a[3]); break;
    case M_FILL_RECT: au_c2d_fill_rect(c, a[0], a[1], a[2], a[3]); break;
    default: au_c2d_stroke_rect(c, a[0], a[1], a[2], a[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue c2d_set_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    double m[6];
    if (argc >= 6) {
        if (args_f64(ctx, argc, argv, 0, 6, m))
            return JS_EXCEPTION;
    } else if (argc <= 1) {
        if (matrix_init(ctx, argc ? argv[0] : JS_UNDEFINED, m))
            return JS_EXCEPTION;
    } else {
        return JS_ThrowTypeError(ctx, "setTransform takes a matrix or six numbers");
    }
    au_c2d_set_transform(c, m[0], m[1], m[2], m[3], m[4], m[5]);
    return JS_UNDEFINED;
}

/* ---- Paths: shared by the context and Path2D ---- */

enum { P_MOVE, P_LINE, P_QUAD, P_CUBIC, P_ARC, P_ARC_TO, P_ELLIPSE, P_RECT, P_CLOSE };

/* A radius of roundRect: a number, or an object with x and y. */
static int radius_arg(JSContext *ctx, JSValueConst v, double *x, double *y)
{
    if (JS_IsObject(v)) {
        *x = *y = 0;
        JSValue vx = JS_GetPropertyStr(ctx, v, "x"), vy = JS_GetPropertyStr(ctx, v, "y");
        int r = 0;
        if (JS_IsException(vx) || JS_IsException(vy))
            r = -1;
        if (!r && !JS_IsUndefined(vx))
            r = JS_ToFloat64(ctx, x, vx);
        if (!r && !JS_IsUndefined(vy))
            r = JS_ToFloat64(ctx, y, vy);
        JS_FreeValue(ctx, vx);
        JS_FreeValue(ctx, vy);
        return r ? -1 : 0;
    }
    if (JS_ToFloat64(ctx, x, v))
        return -1;
    *y = *x;
    return 0;
}

static bool path_target(JSContext *ctx, JSValueConst this_val, AuCanvasPath **p, const AuMatrix **m)
{
    JsCanvas *jc = jc_of(ctx);
    AuCanvasPath *pp = JS_IsObject(this_val) ? JS_GetOpaque(this_val, jc->cls_path) : NULL;
    if (pp) {
        *p = pp;
        *m = NULL;
        return true;
    }
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return false;
    *p = au_c2d_path(c);
    *m = au_c2d_matrix(c);
    return true;
}

static JSValue path_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const uint8_t nargs[] = {2, 2, 4, 6, 5, 5, 7, 4, 0};
    AuCanvasPath *p;
    const AuMatrix *m;
    double a[8];
    if (!path_target(ctx, this_val, &p, &m) || args_f64(ctx, argc, argv, 0, nargs[magic], a))
        return JS_EXCEPTION;
    bool ok = true;
    switch (magic) {
    case P_MOVE: au_cpath_move_to(p, m, a[0], a[1]); break;
    case P_LINE: au_cpath_line_to(p, m, a[0], a[1]); break;
    case P_QUAD: au_cpath_quad_to(p, m, a[0], a[1], a[2], a[3]); break;
    case P_CUBIC: au_cpath_cubic_to(p, m, a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case P_ARC: ok = au_cpath_arc(p, m, a[0], a[1], a[2], a[3], a[4], argc > 5 && JS_ToBool(ctx, argv[5]) > 0); break;
    case P_ARC_TO: ok = au_cpath_arc_to(p, m, a[0], a[1], a[2], a[3], a[4]); break;
    case P_ELLIPSE:
        ok = au_cpath_ellipse(p, m, a[0], a[1], a[2], a[3], a[4], a[5], a[6], argc > 7 && JS_ToBool(ctx, argv[7]) > 0);
        break;
    case P_RECT: au_cpath_rect(p, m, a[0], a[1], a[2], a[3]); break;
    default: au_cpath_close(p); break;
    }
    if (!ok)
        return throw_dom(ctx, "IndexSizeError", "The radius is negative");
    return JS_UNDEFINED;
}

static JSValue path_round_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuCanvasPath *p;
    const AuMatrix *m;
    double a[4];
    if (!path_target(ctx, this_val, &p, &m) || args_f64(ctx, argc, argv, 0, 4, a))
        return JS_EXCEPTION;
    double rx[4] = {0, 0, 0, 0}, ry[4] = {0, 0, 0, 0};
    int n = 1;
    if (argc > 4 && !JS_IsUndefined(argv[4])) {
        if (JS_IsArray(argv[4])) {
            int64_t len;
            if (JS_GetLength(ctx, argv[4], &len))
                return JS_EXCEPTION;
            if (len < 1 || len > 4)
                return JS_ThrowRangeError(ctx, "roundRect takes 1 to 4 radii");
            n = (int)len;
            for (int i = 0; i < n; i++) {
                JSValue v = JS_GetPropertyUint32(ctx, argv[4], (uint32_t)i);
                if (JS_IsException(v))
                    return JS_EXCEPTION;
                int r = radius_arg(ctx, v, &rx[i], &ry[i]);
                JS_FreeValue(ctx, v);
                if (r)
                    return JS_EXCEPTION;
            }
        } else if (radius_arg(ctx, argv[4], &rx[0], &ry[0])) {
            return JS_EXCEPTION;
        }
    }
    /* 1: all corners; 2: top-left and bottom-right, top-right and
     * bottom-left; 3: top-left, top-right and bottom-left, bottom-right. */
    static const uint8_t map[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    double radii[8];
    for (int k = 0; k < 4; k++) {
        radii[2 * k] = rx[map[n - 1][k]];
        radii[2 * k + 1] = ry[map[n - 1][k]];
        if (radii[2 * k] < 0 || radii[2 * k + 1] < 0)
            return JS_ThrowRangeError(ctx, "A radius is negative");
    }
    au_cpath_round_rect(p, m, a[0], a[1], a[2], a[3], radii);
    return JS_UNDEFINED;
}

#define PATH_METHODS                                         \
    JS_CFUNC_MAGIC_DEF("moveTo", 2, path_method, P_MOVE),    \
    JS_CFUNC_MAGIC_DEF("lineTo", 2, path_method, P_LINE),    \
    JS_CFUNC_MAGIC_DEF("quadraticCurveTo", 4, path_method, P_QUAD), \
    JS_CFUNC_MAGIC_DEF("bezierCurveTo", 6, path_method, P_CUBIC),   \
    JS_CFUNC_MAGIC_DEF("arc", 5, path_method, P_ARC),        \
    JS_CFUNC_MAGIC_DEF("arcTo", 5, path_method, P_ARC_TO),   \
    JS_CFUNC_MAGIC_DEF("ellipse", 7, path_method, P_ELLIPSE), \
    JS_CFUNC_MAGIC_DEF("rect", 4, path_method, P_RECT),      \
    JS_CFUNC_MAGIC_DEF("closePath", 0, path_method, P_CLOSE), \
    JS_CFUNC_DEF("roundRect", 4, path_round_rect)

/* ---- Path2D ---- */

static JSValue path2d_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JsCanvas *jc = jc_of(ctx);
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        return proto;
    JSValue o = JS_NewObjectProtoClass(ctx, proto, jc->cls_path);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(o))
        return o;
    AuCanvasPath *p = au_alloc(sizeof *p);
    au_cpath_init(p);
    JS_SetOpaque(o, p);
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        AuCanvasPath *src = JS_IsObject(argv[0]) ? JS_GetOpaque(argv[0], jc->cls_path) : NULL;
        if (src) {
            au_cpath_add(p, src, NULL);
        } else {
            /* SVG path data; what precedes an error is kept, as SVG does. */
            size_t n;
            const char *d = JS_ToCStringLen(ctx, &n, argv[0]);
            if (!d) {
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
            AuPath raw;
            au_path_init(&raw);
            if (n <= 1024 * 1024)
                au_svg_path_parse(d, n, &raw);
            au_cpath_add_raw(p, &raw);
            au_path_free(&raw);
            JS_FreeCString(ctx, d);
        }
    }
    return o;
}

static JSValue path2d_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JsCanvas *jc = jc_of(ctx);
    AuCanvasPath *p = JS_GetOpaque2(ctx, this_val, jc->cls_path);
    if (!p)
        return JS_EXCEPTION;
    AuCanvasPath *src = argc > 0 ? JS_GetOpaque2(ctx, argv[0], jc->cls_path) : NULL;
    if (!src)
        return argc > 0 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "not enough arguments");
    double m[6];
    if (matrix_init(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, m))
        return JS_EXCEPTION;
    for (int i = 0; i < 6; i++)
        if (!isfinite(m[i]))
            return JS_UNDEFINED;
    AuMatrix mm = {(float)m[0], (float)m[1], (float)m[2], (float)m[3], (float)m[4], (float)m[5]};
    au_cpath_add(p, src, &mm);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry path2d_funcs[] = {
    PATH_METHODS,
    JS_CFUNC_DEF("addPath", 1, path2d_add),
};

/* ---- Context: gradients, patterns, dashes ---- */

static JSValue c2d_gradient(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const uint8_t nargs[] = {4, 6, 3};
    AuCanvas2D *c = this_c2d(ctx, this_val);
    double a[6];
    if (!c || args_f64(ctx, argc, argv, 0, nargs[magic], a))
        return JS_EXCEPTION;
    for (int i = 0; i < nargs[magic]; i++)
        if (!isfinite(a[i]))
            return JS_ThrowTypeError(ctx, "The gradient's arguments must be finite");
    AuCanvasGradient *g;
    if (magic == 0) {
        g = au_cgradient_linear(a[0], a[1], a[2], a[3]);
    } else if (magic == 1) {
        if (a[2] < 0 || a[5] < 0)
            return throw_dom(ctx, "IndexSizeError", "A radius is negative");
        g = au_cgradient_radial(a[0], a[1], a[2], a[3], a[4], a[5]);
    } else {
        g = au_cgradient_conic(a[0], a[1], a[2]);
    }
    JSValue o = wrap_gradient(ctx, g);
    au_cgradient_unref(g);
    return o;
}

static JSValue c2d_create_pattern(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuJs *js = js_of_ctx(ctx);
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "not enough arguments");
    int repeat = 3;
    if (!JS_IsNull(argv[1])) {
        const char *r = JS_ToCString(ctx, argv[1]);
        if (!r)
            return JS_EXCEPTION;
        bool ok = true;
        if (!*r || !strcmp(r, "repeat"))
            repeat = 3;
        else if (!strcmp(r, "repeat-x"))
            repeat = 1;
        else if (!strcmp(r, "repeat-y"))
            repeat = 2;
        else if (!strcmp(r, "no-repeat"))
            repeat = 0;
        else
            ok = false;
        JS_FreeCString(ctx, r);
        if (!ok)
            return throw_dom(ctx, "SyntaxError", "The repetition is not one of repeat, repeat-x, repeat-y, no-repeat");
    }
    Src s;
    int r = image_source(ctx, argv[0], &s);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0 || !s.surf)
        return JS_NULL;
    AuCanvasBitmap *b = src_to_bitmap(js, &s);
    src_done(&s);
    if (!b)
        return JS_NULL;
    AuCanvasPattern *p = au_cpattern_new(b, repeat);
    au_cbitmap_unref(b);
    JSValue o = wrap_pattern(ctx, p);
    au_cpattern_unref(p);
    return o;
}

static JSValue c2d_set_line_dash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setLineDash takes a sequence of numbers");
    int64_t len;
    if (JS_GetLength(ctx, argv[0], &len))
        return JS_EXCEPTION;
    if (len < 0 || len > AU_STROKE_MAX_DASHES)
        return JS_UNDEFINED; /* longer than the cap: ignored (canvas2d.h) */
    double d[AU_STROKE_MAX_DASHES];
    for (int64_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        if (JS_IsException(v))
            return JS_EXCEPTION;
        int r = JS_ToFloat64(ctx, &d[i], v);
        JS_FreeValue(ctx, v);
        if (r)
            return JS_EXCEPTION;
    }
    au_c2d_set_line_dash(c, d, (size_t)len);
    return JS_UNDEFINED;
}

static JSValue c2d_get_line_dash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    const float *d;
    size_t n = au_c2d_get_line_dash(c, &d);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, d[i]));
    return arr;
}

/* ---- Context: drawing paths ---- */

enum { D_FILL, D_STROKE, D_CLIP, D_IN_PATH, D_IN_STROKE };

static JSValue c2d_draw_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JsCanvas *jc = jc_of(ctx);
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    int i = 0;
    const AuCanvasPath *path = NULL;
    if (argc > 0 && JS_IsObject(argv[0])) {
        path = JS_GetOpaque2(ctx, argv[0], jc->cls_path);
        if (!path)
            return JS_EXCEPTION;
        i = 1;
    }
    double xy[2] = {0, 0};
    if (magic >= D_IN_PATH) {
        if (args_f64(ctx, argc, argv, i, 2, xy))
            return JS_EXCEPTION;
        i += 2;
    }
    AuFillRule rule = AU_FILL_NONZERO;
    if (magic != D_STROKE && magic != D_IN_STROKE && arg_rule(ctx, argc, argv, i, &rule) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case D_FILL: au_c2d_fill(c, path, rule); break;
    case D_STROKE: au_c2d_stroke(c, path); break;
    case D_CLIP: au_c2d_clip(c, path, rule); break;
    case D_IN_PATH: return JS_NewBool(ctx, au_c2d_point_in_path(c, path, xy[0], xy[1], rule));
    default: return JS_NewBool(ctx, au_c2d_point_in_stroke(c, path, xy[0], xy[1]));
    }
    return JS_UNDEFINED;
}

/* ---- Context: text ---- */

static JSValue c2d_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "not enough arguments");
    double xy[2], max = -1;
    if (args_f64(ctx, argc, argv, 1, 2, xy))
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3])) {
        if (JS_ToFloat64(ctx, &max, argv[3]))
            return JS_EXCEPTION;
        if (!(max > 0) || !isfinite(max))
            return JS_UNDEFINED; /* zero, negative or not a number: nothing is drawn */
    }
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    au_c2d_draw_text(c, s, n, xy[0], xy[1], max, magic != 0);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue c2d_measure_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "not enough arguments");
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    AuTextMetrics2D m;
    au_c2d_measure_text(c, s, n, &m);
    JS_FreeCString(ctx, s);
    JSValue o = JS_NewObjectProto(ctx, jc_of(ctx)->tm_proto);
    if (JS_IsException(o))
        return o;
    static const char *const names[] = {"width", "actualBoundingBoxLeft", "actualBoundingBoxRight",
                                        "actualBoundingBoxAscent", "actualBoundingBoxDescent",
                                        "fontBoundingBoxAscent", "fontBoundingBoxDescent", "emHeightAscent",
                                        "emHeightDescent", "hangingBaseline", "alphabeticBaseline",
                                        "ideographicBaseline"};
    const double vals[] = {m.width, m.left, m.right, m.ascent, m.descent, m.font_ascent, m.font_descent,
                           m.em_ascent, m.em_descent, m.hanging, m.alphabetic, m.ideographic};
    for (size_t i = 0; i < JS_COUNTOF(names); i++)
        JS_DefinePropertyValueStr(ctx, o, names[i], JS_NewFloat64(ctx, vals[i]), JS_PROP_ENUMERABLE);
    return o;
}

/* ---- Context: images ---- */

static JSValue c2d_draw_image(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc != 3 && argc != 5 && argc != 9)
        return JS_ThrowTypeError(ctx, "drawImage takes 3, 5 or 9 arguments");
    double a[8];
    if (args_f64(ctx, argc, argv, 1, argc - 1, a))
        return JS_EXCEPTION;
    Src s;
    int r = image_source(ctx, argv[0], &s);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return JS_UNDEFINED;
    double sx = 0, sy = 0, sw = s.nat_w, sh = s.nat_h, dx, dy, dw = s.nat_w, dh = s.nat_h;
    if (argc == 3) {
        dx = a[0], dy = a[1];
    } else if (argc == 5) {
        dx = a[0], dy = a[1], dw = a[2], dh = a[3];
    } else {
        sx = a[0], sy = a[1], sw = a[2], sh = a[3];
        dx = a[4], dy = a[5], dw = a[6], dh = a[7];
    }
    au_c2d_draw_image(c, s.surf, s.nat_w, s.nat_h, s.opaque, sx, sy, sw, sh, dx, dy, dw, dh);
    if (!s.clean)
        au_c2d_taint(c);
    src_done(&s);
    return JS_UNDEFINED;
}

static JSValue c2d_attributes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    AuCanvas2D *c = this_c2d(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "alpha", JS_NewBool(ctx, au_c2d_alpha(c)));
    JS_SetPropertyStr(ctx, o, "colorSpace", JS_NewString(ctx, "srgb"));
    JS_SetPropertyStr(ctx, o, "desynchronized", JS_FALSE);
    JS_SetPropertyStr(ctx, o, "willReadFrequently", JS_FALSE);
    return o;
}

static JSValue c2d_false(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_FALSE;
}

static JSValue c2d_nothing(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

#define ATTR(name, magic) JS_CGETSET_MAGIC_DEF2(name, c2d_get, c2d_set, magic, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE)

static const JSCFunctionListEntry c2d_funcs[] = {
    ATTR("globalAlpha", A_GLOBAL_ALPHA),
    ATTR("globalCompositeOperation", A_COMPOSITE),
    ATTR("imageSmoothingEnabled", A_SMOOTHING),
    ATTR("imageSmoothingQuality", A_SMOOTHING_QUALITY),
    ATTR("fillStyle", A_FILL_STYLE),
    ATTR("strokeStyle", A_STROKE_STYLE),
    ATTR("lineWidth", A_LINE_WIDTH),
    ATTR("lineCap", A_LINE_CAP),
    ATTR("lineJoin", A_LINE_JOIN),
    ATTR("miterLimit", A_MITER),
    ATTR("lineDashOffset", A_DASH_OFFSET),
    ATTR("shadowColor", A_SHADOW_COLOR),
    ATTR("shadowBlur", A_SHADOW_BLUR),
    ATTR("shadowOffsetX", A_SHADOW_X),
    ATTR("shadowOffsetY", A_SHADOW_Y),
    ATTR("font", A_FONT),
    ATTR("textAlign", A_TEXT_ALIGN),
    ATTR("textBaseline", A_TEXT_BASELINE),
    ATTR("direction", A_DIRECTION),
    ATTR("letterSpacing", A_LETTER_SPACING),
    ATTR("wordSpacing", A_WORD_SPACING),
    ATTR("fontKerning", A_KERNING),
    ATTR("filter", A_FILTER),
    ATTR("fontStretch", A_FONT_STRETCH),
    ATTR("fontVariantCaps", A_FONT_VARIANT_CAPS),
    ATTR("textRendering", A_TEXT_RENDERING),
    JS_CFUNC_MAGIC_DEF("save", 0, c2d_simple, M_SAVE),
    JS_CFUNC_MAGIC_DEF("restore", 0, c2d_simple, M_RESTORE),
    JS_CFUNC_MAGIC_DEF("reset", 0, c2d_simple, M_RESET),
    JS_CFUNC_MAGIC_DEF("resetTransform", 0, c2d_simple, M_RESET_TRANSFORM),
    JS_CFUNC_MAGIC_DEF("beginPath", 0, c2d_simple, M_BEGIN_PATH),
    JS_CFUNC_MAGIC_DEF("scale", 2, c2d_simple, M_SCALE),
    JS_CFUNC_MAGIC_DEF("rotate", 1, c2d_simple, M_ROTATE),
    JS_CFUNC_MAGIC_DEF("translate", 2, c2d_simple, M_TRANSLATE),
    JS_CFUNC_MAGIC_DEF("transform", 6, c2d_simple, M_TRANSFORM),
    JS_CFUNC_MAGIC_DEF("clearRect", 4, c2d_simple, M_CLEAR_RECT),
    JS_CFUNC_MAGIC_DEF("fillRect", 4, c2d_simple, M_FILL_RECT),
    JS_CFUNC_MAGIC_DEF("strokeRect", 4, c2d_simple, M_STROKE_RECT),
    JS_CFUNC_DEF("setTransform", 0, c2d_set_transform),
    PATH_METHODS,
    JS_CFUNC_MAGIC_DEF("createLinearGradient", 4, c2d_gradient, 0),
    JS_CFUNC_MAGIC_DEF("createRadialGradient", 6, c2d_gradient, 1),
    JS_CFUNC_MAGIC_DEF("createConicGradient", 3, c2d_gradient, 2),
    JS_CFUNC_DEF("createPattern", 2, c2d_create_pattern),
    JS_CFUNC_DEF("setLineDash", 1, c2d_set_line_dash),
    JS_CFUNC_DEF("getLineDash", 0, c2d_get_line_dash),
    JS_CFUNC_MAGIC_DEF("fill", 0, c2d_draw_path, D_FILL),
    JS_CFUNC_MAGIC_DEF("stroke", 0, c2d_draw_path, D_STROKE),
    JS_CFUNC_MAGIC_DEF("clip", 0, c2d_draw_path, D_CLIP),
    JS_CFUNC_MAGIC_DEF("isPointInPath", 2, c2d_draw_path, D_IN_PATH),
    JS_CFUNC_MAGIC_DEF("isPointInStroke", 2, c2d_draw_path, D_IN_STROKE),
    JS_CFUNC_MAGIC_DEF("fillText", 3, c2d_text, 0),
    JS_CFUNC_MAGIC_DEF("strokeText", 3, c2d_text, 1),
    JS_CFUNC_DEF("measureText", 1, c2d_measure_text),
    JS_CFUNC_DEF("drawImage", 3, c2d_draw_image),
    JS_CFUNC_DEF("getContextAttributes", 0, c2d_attributes),
    JS_CFUNC_DEF("isContextLost", 0, c2d_false),
    JS_CFUNC_DEF("drawFocusIfNeeded", 1, c2d_nothing),
};

static const JSCFunctionListEntry grad_funcs[] = {
    JS_CFUNC_DEF("addColorStop", 2, grad_add_stop),
};

static const JSCFunctionListEntry pat_funcs[] = {
    JS_CFUNC_DEF("setTransform", 0, pat_set_transform),
};

/* ---- ImageBitmap ---- */

static JSValue bmp_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    AuCanvasBitmap **b = JS_GetOpaque2(ctx, this_val, jc_of(ctx)->cls_bmp);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, *b ? (magic ? (*b)->nat_h : (*b)->nat_w) : 0);
}

static JSValue bmp_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    AuCanvasBitmap **b = JS_GetOpaque2(ctx, this_val, jc_of(ctx)->cls_bmp);
    if (!b)
        return JS_EXCEPTION;
    au_cbitmap_unref(*b);
    *b = NULL;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry bmp_funcs[] = {
    JS_CGETSET_MAGIC_DEF("width", bmp_size, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", bmp_size, NULL, 1),
    JS_CFUNC_DEF("close", 0, bmp_close),
};

/* ---- Natives for the prelude ---- */

/* canvasContext(owner, w, h, alpha, canvas): a new context. owner is the
 * canvas element or null; canvas is what the context's canvas attribute
 * returns. */
static JSValue n_context(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuJs *js = js_of_ctx(ctx);
    AuNode *n = js_unwrap(js, argv[0]);
    int32_t w = 300, h = 150;
    JS_ToInt32(ctx, &w, argv[1]);
    JS_ToInt32(ctx, &h, argv[2]);
    bool alpha = JS_ToBool(ctx, argv[3]) > 0;
    void *owner = n && au_is_element(n) ? (void *)au_as_element(n) : NULL;
    if (owner && au_canvas_set_find(cv_set(js), owner))
        return JS_ThrowTypeError(ctx, "The canvas already has a context");
    JSValue o = JS_NewObjectClass(ctx, (int)js->canvas->cls_ctx);
    if (JS_IsException(o))
        return o;
    AuCanvas2D *c = au_c2d_new(cv_set(js), owner, w, h, alpha);
    JS_SetOpaque(o, c);
    JS_DefinePropertyValueStr(ctx, o, "canvas", JS_DupValue(ctx, argv[4]), JS_PROP_ENUMERABLE);
    return o;
}

static AuCanvas2D *arg_c2d(JSContext *ctx, JSValueConst v)
{
    return this_c2d(ctx, v);
}

/* canvasResize(ctx, w, h): the width and height setters. */
static JSValue n_resize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuCanvas2D *c = arg_c2d(ctx, argv[0]);
    int32_t w = 0, h = 0;
    if (!c || JS_ToInt32(ctx, &w, argv[1]) || JS_ToInt32(ctx, &h, argv[2]))
        return JS_EXCEPTION;
    au_c2d_resize(c, w, h);
    return JS_UNDEFINED;
}

/* canvasSync(): applies width and height attribute changes now. */
static JSValue n_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    AuJs *js = js_of_ctx(ctx);
    if (js->canvas_pending)
        js_canvas_sync(js);
    return JS_UNDEFINED;
}

/* c2dMatrix(ctx): [a, b, c, d, e, f]. */
static JSValue n_matrix(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuCanvas2D *c = arg_c2d(ctx, argv[0]);
    if (!c)
        return JS_EXCEPTION;
    double m[6];
    au_c2d_get_transform(c, m);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 6; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, m[i]));
    return arr;
}

/* The bytes of a Uint8ClampedArray (or Uint8Array). */
static uint8_t *bytes_of(JSContext *ctx, JSValueConst v, size_t *len)
{
    size_t off, n, bpe;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &n, &bpe);
    if (JS_IsException(buf))
        return NULL;
    size_t size;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!p || bpe != 1 || off > size || n > size - off) {
        if (p)
            JS_ThrowTypeError(ctx, "The data is not an array of bytes");
        return NULL;
    }
    *len = n;
    return p + off;
}

/* c2dRead(ctx, x, y, w, h, data): getImageData into data (w * h * 4). */
static JSValue n_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuCanvas2D *c = arg_c2d(ctx, argv[0]);
    int32_t r[4];
    if (!c)
        return JS_EXCEPTION;
    for (int i = 0; i < 4; i++)
        if (JS_ToInt32(ctx, &r[i], argv[1 + i]))
            return JS_EXCEPTION;
    if (!au_c2d_clean(c))
        return throw_dom(ctx, "SecurityError", "The canvas has been tainted by cross-origin data");
    size_t len;
    uint8_t *p = bytes_of(ctx, argv[5], &len);
    if (!p)
        return JS_EXCEPTION;
    if (r[2] <= 0 || r[3] <= 0 || (uint64_t)r[2] * (uint64_t)r[3] * 4 != len)
        return JS_ThrowRangeError(ctx, "The data does not match the size");
    au_c2d_get_image_data(c, r[0], r[1], r[2], r[3], p);
    return JS_UNDEFINED;
}

/* c2dWrite(ctx, data, w, h, x, y, dx, dy, dw, dh): putImageData. */
static JSValue n_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuCanvas2D *c = arg_c2d(ctx, argv[0]);
    int32_t r[8];
    if (!c)
        return JS_EXCEPTION;
    for (int i = 0; i < 8; i++)
        if (JS_ToInt32(ctx, &r[i], argv[2 + i]))
            return JS_EXCEPTION;
    size_t len;
    uint8_t *p = bytes_of(ctx, argv[1], &len);
    if (!p)
        return JS_EXCEPTION;
    if (r[0] <= 0 || r[1] <= 0 || (uint64_t)r[0] * (uint64_t)r[1] * 4 != len)
        return throw_dom(ctx, "InvalidStateError", "The data does not match the size");
    au_c2d_put_image_data(c, p, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    return JS_UNDEFINED;
}

typedef struct RowCtx {
    AuCanvas2D *c;
    int w;
} RowCtx;

static void encode_row(void *ctx, int y, uint8_t *rgba)
{
    RowCtx *r = ctx;
    au_c2d_get_image_data(r->c, 0, y, r->w, 1, rgba);
}

static void base64(const uint8_t *p, size_t n, AuBuf *out)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    au_buf_reserve(out, (n + 2) / 3 * 4 + 1);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        char q[4] = {tab[v >> 18], tab[(v >> 12) & 63], tab[(v >> 6) & 63], tab[v & 63]};
        au_buf_append(out, q, 4);
    }
    if (i < n) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)p[i + 1] << 8;
        char q[4] = {tab[v >> 18], tab[(v >> 12) & 63], i + 1 < n ? tab[(v >> 6) & 63] : '=', '='};
        au_buf_append(out, q, 4);
    }
}

/* canvasEncode(ctx, asUrl): the canvas as a PNG: a data: URL string, or
 * the bytes; null for a canvas without pixels. */
static JSValue n_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuCanvas2D *c = arg_c2d(ctx, argv[0]);
    if (!c)
        return JS_EXCEPTION;
    if (!au_c2d_clean(c))
        return throw_dom(ctx, "SecurityError", "The canvas has been tainted by cross-origin data");
    if (au_c2d_inert(c) || au_c2d_width(c) <= 0 || au_c2d_height(c) <= 0)
        return JS_NULL;
    AuBuf png;
    au_buf_init(&png);
    RowCtx rc = {c, au_c2d_width(c)};
    au_png_encode_rgba(au_c2d_width(c), au_c2d_height(c), encode_row, &rc, &png);
    JSValue r;
    if (JS_ToBool(ctx, argv[1]) > 0) {
        AuBuf url;
        au_buf_init(&url);
        au_buf_puts(&url, "data:image/png;base64,");
        base64((const uint8_t *)png.data, png.len, &url);
        r = JS_NewStringLen(ctx, url.data, url.len);
        au_buf_free(&url);
    } else {
        r = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)png.data, png.len);
    }
    au_buf_free(&png);
    return r;
}

/* canvasBitmap(source, crop, sx, sy, sw, sh): an ImageBitmap of an image,
 * canvas, ImageBitmap, ImageData or the bytes of an image file
 * (createImageBitmap, transferToImageBitmap). */
static JSValue n_bitmap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuJs *js = js_of_ctx(ctx);
    AuCanvasBitmap *b = NULL;
    if (JS_IsObject(argv[0]) && JS_GetTypedArrayType(argv[0]) == JS_TYPED_ARRAY_UINT8) {
        size_t len;
        uint8_t *p = bytes_of(ctx, argv[0], &len);
        if (!p)
            return JS_EXCEPTION;
        b = decode_bitmap(js, p, len);
        if (!b)
            return throw_dom(ctx, "InvalidStateError", "The image could not be decoded");
    } else {
        /* ImageData: an object with data, width and height. */
        JSValue dv = JS_IsObject(argv[0]) ? JS_GetPropertyStr(ctx, argv[0], "data") : JS_UNDEFINED;
        if (JS_IsException(dv))
            return JS_EXCEPTION;
        if (JS_IsObject(dv) && JS_GetTypedArrayType(dv) == JS_TYPED_ARRAY_UINT8C) {
            int32_t w = 0, h = 0;
            JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width"), hv = JS_GetPropertyStr(ctx, argv[0], "height");
            JS_ToInt32(ctx, &w, wv);
            JS_ToInt32(ctx, &h, hv);
            JS_FreeValue(ctx, wv);
            JS_FreeValue(ctx, hv);
            size_t len;
            uint8_t *p = bytes_of(ctx, dv, &len);
            JS_FreeValue(ctx, dv);
            if (!p)
                return JS_EXCEPTION;
            if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h * 4 != len)
                return throw_dom(ctx, "InvalidStateError", "The ImageData is detached");
            b = au_cbitmap_new(cv_set(js), w, h);
            if (!b)
                return throw_dom(ctx, "InvalidStateError", "The bitmap could not be allocated");
            bool be = au_c2d_native_big_endian();
            for (int y = 0; y < h; y++)
                au_c2d_row_from_rgba(p + (size_t)y * (size_t)w * 4, b->surf->pixels + (size_t)y * b->surf->stride,
                                     (size_t)w, be);
        } else {
            JS_FreeValue(ctx, dv);
            Src s;
            int r = image_source(ctx, argv[0], &s);
            if (r < 0)
                return JS_EXCEPTION;
            if (r == 0 || !s.surf) {
                /* A blank canvas gives a blank bitmap; an image that is
                 * not loaded is an error. */
                AuNode *n = js_unwrap(js, argv[0]);
                if (n && au_is_html_element(n, AU_ATOM_img))
                    return throw_dom(ctx, "InvalidStateError", "The image is not loaded");
                JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width"), hv = JS_GetPropertyStr(ctx, argv[0], "height");
                int32_t w = 0, h = 0;
                JS_ToInt32(ctx, &w, wv);
                JS_ToInt32(ctx, &h, hv);
                JS_FreeValue(ctx, wv);
                JS_FreeValue(ctx, hv);
                b = au_cbitmap_new(cv_set(js), w, h);
            } else {
                b = src_to_bitmap(js, &s);
                src_done(&s);
            }
            if (!b)
                return throw_dom(ctx, "InvalidStateError", "The bitmap could not be allocated");
        }
    }
    /* The crop rectangle, in the bitmap's natural units. */
    if (JS_ToBool(ctx, argv[1]) > 0) {
        double r[4];
        if (args_f64(ctx, argc, argv, 2, 4, r)) {
            au_cbitmap_unref(b);
            return JS_EXCEPTION;
        }
        if (!isfinite(r[0]) || !isfinite(r[1]) || !isfinite(r[2]) || !isfinite(r[3]) || r[2] == 0 || r[3] == 0) {
            au_cbitmap_unref(b);
            return JS_ThrowRangeError(ctx, "The crop rectangle is empty");
        }
        if (r[2] < 0) {
            r[0] += r[2];
            r[2] = -r[2];
        }
        if (r[3] < 0) {
            r[1] += r[3];
            r[3] = -r[3];
        }
        double cw = floor(r[2] + 0.5), ch = floor(r[3] + 0.5);
        AuCanvasBitmap *crop = cw >= 1 && ch >= 1 && cw <= AU_C2D_MAX_DIM && ch <= AU_C2D_MAX_DIM
                                   ? au_cbitmap_new(cv_set(js), (int)cw, (int)ch)
                                   : NULL;
        if (!crop) {
            au_cbitmap_unref(b);
            return throw_dom(ctx, "InvalidStateError", "The bitmap could not be allocated");
        }
        /* Draw the source into the crop through a scratch context. */
        AuCanvas2D *tmp = au_c2d_new(cv_set(js), NULL, (int)cw, (int)ch, true);
        au_c2d_draw_image(tmp, b->surf, b->nat_w, b->nat_h, false, r[0], r[1], r[2], r[3], 0, 0, cw, ch);
        const AuSurface *ts = au_c2d_surface(tmp);
        if (ts)
            for (int y = 0; y < (int)ch; y++)
                memcpy(crop->surf->pixels + (size_t)y * crop->surf->stride, ts->pixels + (size_t)y * ts->stride,
                       (size_t)cw * sizeof(AuPixel));
        au_c2d_free(tmp);
        crop->clean = b->clean;
        au_cbitmap_unref(b);
        b = crop;
    }
    JSValue o = new_bitmap_object(ctx, b);
    au_cbitmap_unref(b);
    return o;
}

/* canvasTainted(ctx). */
static JSValue n_tainted(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuCanvas2D *c = arg_c2d(ctx, argv[0]);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !au_c2d_clean(c));
}

/* bitmapTainted(bitmap). */
static JSValue n_bmp_tainted(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    AuCanvasBitmap **b = JS_GetOpaque2(ctx, argv[0], jc_of(ctx)->cls_bmp);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, *b && !(*b)->clean);
}

static const JSCFunctionListEntry canvas_natives[] = {
    JS_CFUNC_DEF("canvasContext", 5, n_context),   JS_CFUNC_DEF("canvasResize", 3, n_resize),
    JS_CFUNC_DEF("canvasSync", 0, n_sync),         JS_CFUNC_DEF("c2dMatrix", 1, n_matrix),
    JS_CFUNC_DEF("c2dRead", 6, n_read),            JS_CFUNC_DEF("c2dWrite", 10, n_write),
    JS_CFUNC_DEF("canvasEncode", 2, n_encode),     JS_CFUNC_DEF("canvasBitmap", 6, n_bitmap),
    JS_CFUNC_DEF("canvasTainted", 1, n_tainted),   JS_CFUNC_DEF("bitmapTainted", 1, n_bmp_tainted),
};

/* ---- Set-up ---- */

static JSValue make_proto(JSContext *ctx, JSClassID id, const JSCFunctionListEntry *funcs, int n)
{
    JSValue proto = JS_NewObject(ctx);
    if (n)
        JS_SetPropertyFunctionList(ctx, proto, funcs, n);
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, proto));
    return proto;
}

void js_canvas_init(AuJs *js, JSValue N)
{
    JSContext *ctx = js->ctx;
    JsCanvas *jc = au_calloc(sizeof *jc);
    js->canvas = jc;
    jc->tm_proto = JS_UNDEFINED;
    if (js->root != js && js->root->canvas) {
        /* classes belong to the runtime */
        JsCanvas *r = js->root->canvas;
        jc->cls_ctx = r->cls_ctx;
        jc->cls_grad = r->cls_grad;
        jc->cls_pat = r->cls_pat;
        jc->cls_path = r->cls_path;
        jc->cls_bmp = r->cls_bmp;
    } else {
        JS_NewClassID(js->rt, &jc->cls_ctx);
        JS_NewClass(js->rt, jc->cls_ctx, &def_ctx);
        JS_NewClassID(js->rt, &jc->cls_grad);
        JS_NewClass(js->rt, jc->cls_grad, &def_grad);
        JS_NewClassID(js->rt, &jc->cls_pat);
        JS_NewClass(js->rt, jc->cls_pat, &def_pat);
        JS_NewClassID(js->rt, &jc->cls_path);
        JS_NewClass(js->rt, jc->cls_path, &def_path);
        JS_NewClassID(js->rt, &jc->cls_bmp);
        JS_NewClass(js->rt, jc->cls_bmp, &def_bmp);
    }
    JS_SetPropertyFunctionList(ctx, N, canvas_natives, JS_COUNTOF(canvas_natives));
    /* The prototypes, for the prelude to hang constructors on. */
    JS_SetPropertyStr(ctx, N, "c2dProto", make_proto(ctx, jc->cls_ctx, c2d_funcs, JS_COUNTOF(c2d_funcs)));
    JS_SetPropertyStr(ctx, N, "gradProto", make_proto(ctx, jc->cls_grad, grad_funcs, JS_COUNTOF(grad_funcs)));
    JS_SetPropertyStr(ctx, N, "patProto", make_proto(ctx, jc->cls_pat, pat_funcs, JS_COUNTOF(pat_funcs)));
    JS_SetPropertyStr(ctx, N, "bmpProto", make_proto(ctx, jc->cls_bmp, bmp_funcs, JS_COUNTOF(bmp_funcs)));
    JSValue path_proto = make_proto(ctx, jc->cls_path, path2d_funcs, JS_COUNTOF(path2d_funcs));
    JSValue path_ctor = JS_NewCFunction2(ctx, path2d_ctor, "Path2D", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, path_ctor, path_proto);
    JS_FreeValue(ctx, path_proto);
    JS_SetPropertyStr(ctx, N, "Path2D", path_ctor);
    jc->tm_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, N, "tmProto", JS_DupValue(ctx, jc->tm_proto));
}

void js_canvas_free(AuJs *js)
{
    JsCanvas *jc = js->canvas;
    if (!jc)
        return;
    JS_FreeValue(js->ctx, jc->tm_proto);
    au_canvas_set_free(jc->own_set);
    au_free(jc);
    js->canvas = NULL;
}
