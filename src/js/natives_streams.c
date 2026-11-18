/* Natives behind src/js/streams.js and EventSource, for pages and
 * workers:
 *
 *   zNew(decompress, format) -> handle    format 0 gzip, 1 deflate (zlib), 2 deflate-raw
 *   zRun(handle, bytes or null, finish) -> [Uint8Array, more]
 *   zEnd(handle)
 *   sseNew(lastEventId) -> handle
 *   sseFeed(handle, bytes) -> [[kind, ...], ...]
 *   sseEnd(handle)
 *   wsNew(maxMessage) -> handle           WebSocket frames (src/net/websocket.c)
 *   wsFeed(handle, bytes) -> [[kind, ...], ...]
 *   wsEnd(handle)
 *   wsFrame(opcode, string or bytes) -> Uint8Array, a masked frame
 *
 * A WebSocket parser holds up to its largest message outside the heap;
 * a runtime may hold MAX_WS of them.
 *
 * A handle holds memory outside the script heap (an encoder is about
 * 300 KB, a decoder 45 KB, a parser up to 1.5 MB of buffers), so a
 * runtime may hold at most MAX_ZIP codecs and MAX_SSE parsers at once;
 * one more is refused with a RangeError after a collection has had the
 * chance to free handles nothing holds. The finalizer frees a handle
 * script dropped without ending it. */
#include "js_int.h"
#include "../codec/codec.h"
#include "../net/sse.h"
#include "../net/websocket.h"

#include <string.h>

#define MAX_ZIP 16
#define MAX_SSE 8
#define MAX_WS 16
/* zRun gives back at most this much output per call, so that a small
 * input that inflates to a great deal arrives in pieces; the caller asks
 * again (with no input) while "more" is true. */
#define ZIP_OUT_MAX (256u * 1024)

enum { H_DEFLATE, H_INFLATE, H_SSE, H_WS };

typedef struct StreamHandle {
    int kind;
    bool ended;
    AuDeflate *def;
    AuInflate *inf;
    bool inf_done;
    AuBuf pending; /* input the codec has not taken yet */
    size_t pending_off;
    AuSseParser sse;
    AuWsParser ws;
} StreamHandle;

static void handle_release(JSRuntime *rt, StreamHandle *h)
{
    if (h->ended)
        return;
    h->ended = true;
    AuJs *root = JS_GetRuntimeOpaque(rt);
    if (h->kind == H_SSE) {
        au_sse_free(&h->sse);
        if (root && root->nsse)
            root->nsse--;
    } else if (h->kind == H_WS) {
        au_ws_parser_free(&h->ws);
        if (root && root->nws)
            root->nws--;
    } else {
        au_deflate_free(h->def);
        au_inflate_free(h->inf);
        h->def = NULL;
        h->inf = NULL;
        au_buf_free(&h->pending);
        if (root && root->nzip)
            root->nzip--;
    }
}

static void handle_finalizer(JSRuntime *rt, JSValueConst val)
{
    AuJs *root = JS_GetRuntimeOpaque(rt);
    StreamHandle *h = root ? JS_GetOpaque(val, root->cls_stream_handle) : NULL;
    if (!h)
        return;
    handle_release(rt, h);
    au_free(h);
}

static JSClassID handle_class(AuJs *js)
{
    AuJs *root = js->root;
    if (!root->cls_stream_handle) {
        JSClassDef def;
        memset(&def, 0, sizeof def);
        def.class_name = "AuraStreamHandle";
        def.finalizer = handle_finalizer;
        JS_NewClassID(root->rt, &root->cls_stream_handle);
        JS_NewClass(root->rt, root->cls_stream_handle, &def);
    }
    return root->cls_stream_handle;
}

static StreamHandle *get_handle(JSContext *ctx, JSValueConst v, int kind)
{
    AuJs *js = js_of_ctx(ctx);
    if (!js->root->cls_stream_handle)
        return NULL;
    StreamHandle *h = JS_GetOpaque(v, js->root->cls_stream_handle);
    if (!h || h->ended)
        return NULL;
    bool zip = h->kind == H_DEFLATE || h->kind == H_INFLATE;
    if (kind == H_INFLATE ? !zip : h->kind != kind)
        return NULL;
    return h;
}

/* Room for one more handle of a kind: collects once when the runtime is
 * at its limit, since handles script has dropped are freed only then. */
static bool room_for(AuJs *js, bool sse)
{
    AuJs *root = js->root;
    uint32_t *n = sse ? &root->nsse : &root->nzip;
    uint32_t max = sse ? MAX_SSE : MAX_ZIP;
    if (*n >= max)
        JS_RunGC(root->rt);
    return *n < max && !au_memory_low(sse ? 64u * 1024 : 512u * 1024);
}

static JSValue new_handle(JSContext *ctx, StreamHandle *h)
{
    AuJs *js = js_of_ctx(ctx);
    JSValue obj = JS_NewObjectClass(ctx, (int)handle_class(js));
    if (JS_IsException(obj)) {
        handle_release(JS_GetRuntime(ctx), h);
        au_free(h);
        return obj;
    }
    JS_SetOpaque(obj, h);
    return obj;
}

/* ---- Compression ---- */

static JSValue n_z_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    int32_t fmt = 0;
    bool decompress = argc > 0 && JS_ToBool(ctx, argv[0]);
    if (argc > 1)
        JS_ToInt32(ctx, &fmt, argv[1]);
    if (fmt < 0 || fmt > 2)
        return JS_ThrowTypeError(ctx, "Unsupported compression format");
    if (!room_for(js, false))
        return JS_ThrowRangeError(ctx, "Too many compression streams are open");
    StreamHandle *h = au_calloc(sizeof *h);
    au_buf_init(&h->pending);
    if (decompress) {
        static const AuInflateFormat f[3] = {AU_INFLATE_GZIP, AU_INFLATE_ZLIB, AU_INFLATE_RAW};
        h->kind = H_INFLATE;
        h->inf = au_inflate_new(f[fmt]);
    } else {
        static const AuDeflateFormat f[3] = {AU_DEFLATE_GZIP, AU_DEFLATE_ZLIB, AU_DEFLATE_RAW};
        h->kind = H_DEFLATE;
        h->def = au_deflate_new(f[fmt], 1);
    }
    if (!h->inf && !h->def) {
        au_free(h);
        return JS_ThrowOutOfMemory(ctx);
    }
    js->root->nzip++;
    return new_handle(ctx, h);
}

static JSValue z_result(JSContext *ctx, AuBuf *out, bool more)
{
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewUint8ArrayCopy(ctx, (const uint8_t *)out->data, out->len));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewBool(ctx, more));
    au_buf_free(out);
    return r;
}

/* zRun(handle, bytes or null, finish): runs the codec over the bytes
 * (added to what it has not taken yet) and returns [output, more]. With
 * finish, the input is complete: an encoder writes its last block, and a
 * decoder that has not reached the end of its stream throws. A decoder
 * throws TypeError for corrupt data and for data after the end. */
static JSValue n_z_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    StreamHandle *h = argc > 0 ? get_handle(ctx, argv[0], H_INFLATE) : NULL;
    if (!h)
        return JS_ThrowTypeError(ctx, "The compression stream has ended");
    bool finish = argc > 2 && JS_ToBool(ctx, argv[2]);
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        size_t n = 0;
        uint8_t *p = JS_GetUint8Array(ctx, &n, argv[1]);
        if (!p && n == 0 && JS_HasException(ctx))
            return JS_EXCEPTION;
        if (n) {
            if (h->pending_off == h->pending.len) {
                au_buf_clear(&h->pending);
                h->pending_off = 0;
            }
            if (h->pending.len + n < h->pending.len)
                return JS_ThrowRangeError(ctx, "The input is too large");
            au_buf_append(&h->pending, p, n);
        }
    }
    AuBuf out;
    au_buf_init(&out);
    uint8_t chunk[8192];
    for (;;) {
        const uint8_t *in = (const uint8_t *)h->pending.data + h->pending_off;
        size_t in_len = h->pending.len - h->pending_off;
        uint8_t *o = chunk;
        size_t o_len = sizeof chunk;
        AuCodecStatus st;
        if (h->kind == H_DEFLATE) {
            st = au_deflate_run(h->def, &in, &in_len, &o, &o_len, finish);
        } else {
            if (h->inf_done) {
                if (in_len) {
                    au_buf_free(&out);
                    return JS_ThrowTypeError(ctx, "Junk found after the end of the compressed data");
                }
                st = AU_CODEC_DONE;
            } else {
                st = au_inflate_run(h->inf, &in, &in_len, &o, &o_len);
            }
        }
        size_t took = (h->pending.len - h->pending_off) - in_len;
        h->pending_off = h->pending.len - in_len;
        size_t made = sizeof chunk - o_len;
        if (made)
            au_buf_append(&out, chunk, made);
        if (st == AU_CODEC_ERROR) {
            au_buf_free(&out);
            return JS_ThrowTypeError(ctx, "The compressed data is not valid: %s",
                                     h->inf ? au_inflate_error(h->inf) : "encoder error");
        }
        if (st == AU_CODEC_DONE) {
            if (h->kind == H_INFLATE) {
                h->inf_done = true;
                if (in_len) {
                    au_buf_free(&out);
                    return JS_ThrowTypeError(ctx, "Junk found after the end of the compressed data");
                }
            }
            break;
        }
        if (out.len >= ZIP_OUT_MAX)
            return z_result(ctx, &out, true);
        /* The codec stopped with output space left: it needs input. */
        if (o_len && !in_len) {
            if (finish && h->kind == H_INFLATE) {
                au_buf_free(&out);
                return JS_ThrowTypeError(ctx, "The compressed data ended early");
            }
            if (!finish || h->kind == H_INFLATE)
                break;
        }
        if (!took && !made && o_len && st == AU_CODEC_OK && !finish)
            break; /* no progress possible (not expected; a guard) */
    }
    if (h->pending_off == h->pending.len) {
        au_buf_free(&h->pending);
        au_buf_init(&h->pending);
        h->pending_off = 0;
    }
    return z_result(ctx, &out, false);
}

static JSValue n_end(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    StreamHandle *h = argc > 0 && js->root->cls_stream_handle ? JS_GetOpaque(argv[0], js->root->cls_stream_handle)
                                                              : NULL;
    if (h)
        handle_release(JS_GetRuntime(ctx), h);
    return JS_UNDEFINED;
}

/* ---- The event stream parser ---- */

typedef struct SseOut {
    JSContext *ctx;
    JSValue arr;
    uint32_t n;
} SseOut;

static void sse_dispatch(void *ctx, const char *type, size_t type_len, const char *data, size_t data_len,
                         const char *id, size_t id_len)
{
    SseOut *o = ctx;
    JSValue rec = JS_NewArray(o->ctx);
    if (data) {
        JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, "m"));
        JS_SetPropertyUint32(o->ctx, rec, 1, js_str(o->ctx, type, type_len));
        JS_SetPropertyUint32(o->ctx, rec, 2, js_str(o->ctx, data, data_len));
        JS_SetPropertyUint32(o->ctx, rec, 3, js_str(o->ctx, id, id_len));
    } else {
        JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, "i"));
        JS_SetPropertyUint32(o->ctx, rec, 1, js_str(o->ctx, id, id_len));
    }
    JS_SetPropertyUint32(o->ctx, o->arr, o->n++, rec);
}

static void sse_retry(void *ctx, uint32_t ms)
{
    SseOut *o = ctx;
    JSValue rec = JS_NewArray(o->ctx);
    JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, "r"));
    JS_SetPropertyUint32(o->ctx, rec, 1, JS_NewUint32(o->ctx, ms));
    JS_SetPropertyUint32(o->ctx, o->arr, o->n++, rec);
}

static JSValue n_sse_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    if (!room_for(js, true))
        return JS_ThrowRangeError(ctx, "Too many event streams are open");
    StreamHandle *h = au_calloc(sizeof *h);
    h->kind = H_SSE;
    au_sse_init(&h->sse);
    if (argc > 0 && JS_IsString(argv[0])) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (s) {
            au_sse_set_id(&h->sse, s, n);
            JS_FreeCString(ctx, s);
        }
    }
    js->root->nsse++;
    return new_handle(ctx, h);
}

/* sseFeed(handle, bytes) -> the records the bytes completed, in order:
 * ["m", type, data, lastEventId] an event, ["i", lastEventId] a dispatch
 * without one, ["r", ms] a reconnection time. */
static JSValue n_sse_feed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    StreamHandle *h = argc > 0 ? get_handle(ctx, argv[0], H_SSE) : NULL;
    if (!h)
        return JS_ThrowTypeError(ctx, "The event stream has ended");
    size_t n = 0;
    uint8_t *p = argc > 1 ? JS_GetUint8Array(ctx, &n, argv[1]) : NULL;
    if (!p && JS_HasException(ctx))
        return JS_EXCEPTION;
    SseOut o = {ctx, JS_NewArray(ctx), 0};
    AuSseCallbacks cb = {&o, sse_dispatch, sse_retry};
    if (p && n)
        au_sse_feed(&h->sse, p, n, &cb);
    return o.arr;
}

/* ---- WebSocket frames ---- */

static JSValue n_ws_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuJs *root = js->root;
    if (root->nws >= MAX_WS)
        JS_RunGC(root->rt);
    if (root->nws >= MAX_WS || au_memory_low(256u * 1024))
        return JS_ThrowRangeError(ctx, "Too many WebSocket connections are open");
    uint32_t max = 4u * 1024 * 1024;
    if (argc > 0)
        JS_ToUint32(ctx, &max, argv[0]);
    if (max < 1024)
        max = 1024;
    StreamHandle *h = au_calloc(sizeof *h);
    h->kind = H_WS;
    au_ws_parser_init(&h->ws, max);
    root->nws++;
    return new_handle(ctx, h);
}

static void ws_push(SseOut *o, JSValue rec) { JS_SetPropertyUint32(o->ctx, o->arr, o->n++, rec); }

static void ws_message(void *ctx, int opcode, const uint8_t *data, size_t len)
{
    SseOut *o = ctx;
    JSValue rec = JS_NewArray(o->ctx);
    if (opcode == AU_WS_TEXT) {
        JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, "t"));
        JS_SetPropertyUint32(o->ctx, rec, 1, js_str(o->ctx, (const char *)data, len));
    } else {
        JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, "b"));
        JS_SetPropertyUint32(o->ctx, rec, 1, JS_NewArrayBufferCopy(o->ctx, data, len));
    }
    ws_push(o, rec);
}

static void ws_control(void *ctx, int opcode, const uint8_t *data, size_t len)
{
    SseOut *o = ctx;
    JSValue rec = JS_NewArray(o->ctx);
    JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, opcode == AU_WS_PING ? "p" : "o"));
    JS_SetPropertyUint32(o->ctx, rec, 1, JS_NewUint8ArrayCopy(o->ctx, data, len));
    ws_push(o, rec);
}

static void ws_close(void *ctx, uint16_t code, const char *reason, size_t reason_len)
{
    SseOut *o = ctx;
    JSValue rec = JS_NewArray(o->ctx);
    JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, "c"));
    JS_SetPropertyUint32(o->ctx, rec, 1, JS_NewInt32(o->ctx, code));
    JS_SetPropertyUint32(o->ctx, rec, 2, js_str(o->ctx, reason, reason_len));
    ws_push(o, rec);
}

static void ws_error(void *ctx, uint16_t code, const char *why)
{
    SseOut *o = ctx;
    JSValue rec = JS_NewArray(o->ctx);
    JS_SetPropertyUint32(o->ctx, rec, 0, JS_NewString(o->ctx, "e"));
    JS_SetPropertyUint32(o->ctx, rec, 1, JS_NewInt32(o->ctx, code));
    JS_SetPropertyUint32(o->ctx, rec, 2, JS_NewString(o->ctx, why));
    ws_push(o, rec);
}

/* wsFeed(handle, bytes) -> what the bytes completed, in order: ["t", text]
 * and ["b", ArrayBuffer] messages, ["p", bytes] a ping, ["o", bytes] a
 * pong, ["c", code, reason] the server's close frame, ["e", code, why]
 * a broken stream (the close code to send). */
static JSValue n_ws_feed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    StreamHandle *h = argc > 0 ? get_handle(ctx, argv[0], H_WS) : NULL;
    if (!h)
        return JS_ThrowTypeError(ctx, "The WebSocket has ended");
    size_t n = 0;
    uint8_t *p = argc > 1 ? JS_GetUint8Array(ctx, &n, argv[1]) : NULL;
    if (!p && JS_HasException(ctx))
        return JS_EXCEPTION;
    SseOut o = {ctx, JS_NewArray(ctx), 0};
    AuWsCallbacks cb = {&o, ws_message, ws_control, ws_close, ws_error};
    if (p && n)
        au_ws_feed(&h->ws, p, n, &cb);
    return o.arr;
}

/* wsFrame(opcode, data) -> a masked frame: data is a string (sent as
 * UTF-8) or a Uint8Array. The mask comes from the embedder's random
 * source, or, before it has any entropy, a generator seeded from the
 * clock. */
static JSValue n_ws_frame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    int32_t op = AU_WS_BINARY;
    if (argc < 2 || JS_ToInt32(ctx, &op, argv[0]))
        return JS_ThrowTypeError(ctx, "wsFrame: bad arguments");
    size_t n = 0;
    char *s = NULL;
    const uint8_t *p;
    if (JS_IsString(argv[1])) {
        s = js_tostr(ctx, argv[1], &n);
        if (!s)
            return JS_EXCEPTION;
        p = (const uint8_t *)s;
    } else {
        p = JS_GetUint8Array(ctx, &n, argv[1]);
        if (!p)
            return JS_EXCEPTION;
    }
    uint8_t mask[4];
    if (!js->hooks.random || !js->hooks.random(js->hooks.ctx, mask, sizeof mask)) {
        static uint32_t x;
        x ^= (uint32_t)(int64_t)js_now(js) * 2654435761u + 0x9E3779B9u;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        memcpy(mask, &x, 4);
    }
    AuBuf b;
    au_buf_init(&b);
    au_ws_frame(&b, op, p, n, mask);
    au_free(s);
    JSValue r = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)b.data, b.len);
    au_buf_free(&b);
    return r;
}

static const JSCFunctionListEntry streams_natives[] = {
    JS_CFUNC_DEF("wsNew", 1, n_ws_new),
    JS_CFUNC_DEF("wsFeed", 2, n_ws_feed),
    JS_CFUNC_DEF("wsEnd", 1, n_end),
    JS_CFUNC_DEF("wsFrame", 2, n_ws_frame),
    JS_CFUNC_DEF("zNew", 2, n_z_new),
    JS_CFUNC_DEF("zRun", 3, n_z_run),
    JS_CFUNC_DEF("zEnd", 1, n_end),
    JS_CFUNC_DEF("sseNew", 1, n_sse_new),
    JS_CFUNC_DEF("sseFeed", 2, n_sse_feed),
    JS_CFUNC_DEF("sseEnd", 1, n_end),
};

void js_streams_natives_init(AuJs *js, JSValue N)
{
    JS_SetPropertyFunctionList(js->ctx, N, streams_natives, JS_COUNTOF(streams_natives));
}
