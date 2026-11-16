/* Natives behind crypto.subtle (src/js/subtle.js): key handles and the
 * operations of src/net/webcrypto.c, for pages and workers alike.
 *
 * A key's material never reaches script as a value it can read unless the
 * key is exported: script holds a handle object (a class of its own whose
 * finalizer destroys the key and zeroes what it held). Errors are thrown
 * as Error objects whose name is the DOMException name the specification
 * gives (DataError, OperationError, NotSupportedError); subtle.js turns
 * them into DOMExceptions.
 *
 * RSA key generation can take seconds on a G4, so it does not run on the
 * caller's stack in a page: ckGenerate returns a job that runs on a
 * coroutine (AuJsHooks.coro), and each ckJobStep resumes it for one slice;
 * prime generation yields through au_tls_set_yield between candidates. In
 * a worker, which has a stack of its own, the generation runs inline and
 * the yield leaves the worker's stack when its slice is over, as the
 * script engine's interrupt check does. Without coroutines it runs to the
 * end in the first step. */
#include "js_int.h"
#include "../net/tls_platform.h"
#include "../net/webcrypto.h"

#include <string.h>

enum { CLS_KEY, CLS_JOB };

static JSClassID cls(AuJs *js, int which) { return js->root->crypto_cls[which]; }

/* ---- Errors and arguments ---- */

static JSValue ck_throw(JSContext *ctx, const char *name, const char *msg)
{
    JSValue e = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "name", JS_NewString(ctx, name), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return JS_Throw(ctx, e);
}

static JSValue ck_err(JSContext *ctx, AuWcErr err, const char *what)
{
    switch (err) {
    case AU_WC_EDATA: return ck_throw(ctx, "DataError", what);
    case AU_WC_ENOTSUP: return ck_throw(ctx, "NotSupportedError", what);
    case AU_WC_ENOMEM: return ck_throw(ctx, "OperationError", "The cryptography library ran out of memory");
    case AU_WC_ECANCEL: return ck_throw(ctx, "OperationError", "The key generation was stopped");
    default: return ck_throw(ctx, "OperationError", what);
    }
}

/* The bytes of a Uint8Array, or none for undefined and null. False (with
 * an exception) for anything else. */
static bool bytes_arg(JSContext *ctx, JSValueConst v, const uint8_t **p, size_t *len)
{
    *p = NULL;
    *len = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return true;
    uint8_t *b = JS_GetUint8Array(ctx, len, v);
    if (!b && JS_HasException(ctx))
        return false;
    *p = b;
    return true;
}

static AuWcKey *key_arg(AuJs *js, JSValueConst v)
{
    return JS_GetOpaque(v, cls(js, CLS_KEY));
}

static AuDigestAlg hash_arg(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return (AuDigestAlg)0;
    const char *s = JS_ToCString(ctx, v);
    if (!s)
        return (AuDigestAlg)0;
    AuDigestAlg h = au_digest_by_name(s);
    JS_FreeCString(ctx, s);
    return h;
}

static int int_arg(JSContext *ctx, JSValueConst v)
{
    int32_t n = 0;
    if (JS_ToInt32(ctx, &n, v))
        return -1;
    return n;
}

/* A new ArrayBuffer holding n bytes of p; p is then zeroed and freed with
 * js_free. */
static JSValue take_buffer(JSContext *ctx, uint8_t *p, size_t cap, size_t n)
{
    JSValue r = JS_NewArrayBufferCopy(ctx, p, n);
    memset(p, 0, cap);
    js_free(ctx, p);
    return r;
}

/* Starts the library from the embedder's random source if nothing has. */
static bool ensure_started(AuJs *js)
{
    if (au_wc_ready())
        return true;
    uint8_t seed[48];
    bool ok = js->hooks.random && js->hooks.random(js->hooks.ctx, seed, sizeof seed) && au_wc_start(seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    return ok;
}

#define NEED_STARTED(js, ctx)                                                                                  \
    do {                                                                                                       \
        if (!ensure_started(js))                                                                               \
            return ck_throw(ctx, "OperationError", "The random source is not ready");                         \
    } while (0)

/* ---- Key handles ---- */

static void key_finalizer(JSRuntime *rt, JSValueConst v)
{
    (void)rt;
    JSClassID id = 0;
    au_wc_key_free(JS_GetAnyOpaque(v, &id));
}

static JSValue wrap_key(AuJs *js, AuWcKey *k)
{
    JSValue o = JS_NewObjectClass(js->ctx, (int)cls(js, CLS_KEY));
    if (JS_IsException(o)) {
        au_wc_key_free(k);
        return o;
    }
    JS_SetOpaque(o, k);
    return o;
}

/* ckSecret(bytes) -> handle */
static JSValue n_ck_secret(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    const uint8_t *p;
    size_t len;
    if (argc < 1 || !bytes_arg(ctx, argv[0], &p, &len))
        return JS_EXCEPTION;
    if (len > AU_WC_SECRET_MAX)
        return ck_throw(ctx, "DataError", "The key is too long");
    return wrap_key(js, au_wc_secret(p, len));
}

/* ckBytes(handle) -> ArrayBuffer of a secret key's bytes (or an X25519
 * private key's), or null */
static JSValue n_ck_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuWcKey *k = argc > 0 ? key_arg(js_of_ctx(ctx), argv[0]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    size_t len;
    const uint8_t *b = au_wc_key_bytes(k, &len);
    return b ? JS_NewArrayBufferCopy(ctx, b, len) : JS_NULL;
}

/* ckBits(handle) -> the modulus or curve size, or the secret's bits */
static JSValue n_ck_bits(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuWcKey *k = argc > 0 ? key_arg(js_of_ctx(ctx), argv[0]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    return JS_NewFloat64(ctx, (double)au_wc_key_bits(k));
}

/* ckImport(kind, private, curveBits, alg, hash, bytes) -> handle */
static JSValue n_ck_import(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    if (argc < 6)
        return JS_ThrowTypeError(ctx, "ckImport: bad arguments");
    int kind = int_arg(ctx, argv[0]), curve = int_arg(ctx, argv[2]), alg = int_arg(ctx, argv[3]);
    bool priv = JS_ToBool(ctx, argv[1]);
    AuDigestAlg hash = hash_arg(ctx, argv[4]);
    const uint8_t *p;
    size_t len;
    if (!bytes_arg(ctx, argv[5], &p, &len))
        return JS_EXCEPTION;
    if (!p || !len)
        return ck_throw(ctx, "DataError", "The key data is empty");
    NEED_STARTED(js, ctx);
    AuWcKey *k = NULL;
    AuWcErr e = au_wc_import((AuWcKind)kind, priv, curve, (AuWcAlg)alg, hash, p, len, &k);
    if (e != AU_WC_OK)
        return ck_err(ctx, e, "The key data is not a valid key");
    return wrap_key(js, k);
}

/* ckExport(handle, publicPart) -> ArrayBuffer */
static JSValue n_ck_export(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuWcKey *k = argc > 1 ? key_arg(js_of_ctx(ctx), argv[0]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    size_t len = 0;
    uint8_t *b = au_wc_export(k, JS_ToBool(ctx, argv[1]), &len);
    if (!b)
        return ck_throw(ctx, "OperationError", "The key could not be exported");
    JSValue r = JS_NewArrayBufferCopy(ctx, b, len);
    au_wc_wipe_free(b, len);
    return r;
}

/* ---- Key generation ---- */

typedef struct CkJob {
    const AuCoroOps *ops;
    void *co;
    double (*now_ms)(void *ctx);
    void *now_ctx;
    double deadline;
    bool started, done, cancel;
    int kind, bits, alg;
    uint32_t e;
    AuDigestAlg hash;
    AuWcKey *key;
    AuWcErr err;
} CkJob;

/* The yield of a job's coroutine: back to the page when the slice is over;
 * nonzero once the job is cancelled. */
static int job_yield(void *p)
{
    CkJob *j = p;
    if (!j->cancel && j->now_ms(j->now_ctx) >= j->deadline) {
        au_tls_set_yield(NULL, NULL);
        j->ops->yield(j->ops->ctx, j->co);
        au_tls_set_yield(job_yield, j);
    }
    return j->cancel ? 1 : 0;
}

static void job_main(void *p)
{
    CkJob *j = p;
    if (j->cancel) {
        j->err = AU_WC_ECANCEL;
        j->done = true;
        return;
    }
    au_tls_set_yield(job_yield, j);
    j->err = au_wc_generate((AuWcKind)j->kind, j->bits, j->e, (AuWcAlg)j->alg, j->hash, &j->key);
    au_tls_set_yield(NULL, NULL);
    if (j->cancel && j->err != AU_WC_OK)
        j->err = AU_WC_ECANCEL;
    j->done = true;
}

/* The yield of an inline generation in a worker: the worker's own. */
static int worker_yield(void *p)
{
    AuJs *js = p;
    au_tls_set_yield(NULL, NULL);
    int r = js_worker_interrupt(js);
    au_tls_set_yield(worker_yield, js);
    return r > 0 ? 1 : 0;
}

static void job_finalizer(JSRuntime *rt, JSValueConst v)
{
    (void)rt;
    JSClassID id = 0;
    CkJob *j = JS_GetAnyOpaque(v, &id);
    if (!j)
        return;
    if (j->co) {
        /* Abandon a generation under way: its yield now returns nonzero,
         * so the library unwinds and the coroutine's entry returns. */
        j->cancel = true;
        while (!j->done && j->ops->resume(j->ops->ctx, j->co)) {
        }
        j->ops->destroy(j->ops->ctx, j->co);
    }
    au_wc_key_free(j->key);
    memset(j, 0, sizeof *j);
    au_free(j);
}

/* ckGenerate(kind, bits, e, alg, hash) -> handle (EC, X25519), or a job
 * (RSA) for ckJobStep */
static JSValue n_ck_generate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "ckGenerate: bad arguments");
    int kind = int_arg(ctx, argv[0]), bits = int_arg(ctx, argv[1]), alg = int_arg(ctx, argv[3]);
    uint32_t e = 0;
    if (JS_ToUint32(ctx, &e, argv[2]))
        return JS_EXCEPTION;
    AuDigestAlg hash = hash_arg(ctx, argv[4]);
    NEED_STARTED(js, ctx);
    if (kind != AU_WC_RSA) {
        AuWcKey *k = NULL;
        AuWcErr err = au_wc_generate((AuWcKind)kind, bits, e, (AuWcAlg)alg, hash, &k);
        if (err != AU_WC_OK)
            return ck_err(ctx, err, "The key could not be generated");
        return wrap_key(js, k);
    }
    if (bits < AU_WC_RSA_GEN_MIN_BITS || bits > AU_WC_RSA_MAX_BITS || bits % 8)
        return ck_throw(ctx, "OperationError", "The modulus length must be 1024 to 4096 bits, in whole bytes");
    if (e < 3 || !(e & 1) || e > 0x7fffffff)
        return ck_throw(ctx, "OperationError", "The public exponent is not supported");
    CkJob *j = au_calloc(sizeof *j);
    j->kind = kind;
    j->bits = bits;
    j->e = e;
    j->alg = alg;
    j->hash = hash;
    j->now_ms = js->hooks.now_ms;
    j->now_ctx = js->hooks.ctx;
    /* A page with coroutines generates on a stack of its own. */
    if (!js->worker && js->hooks.coro && js->hooks.now_ms) {
        j->ops = js->hooks.coro;
        size_t stack = 64 * 1024;
#if defined(__SANITIZE_ADDRESS__)
        stack *= 8;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
        stack *= 8;
#endif
#endif
        j->co = j->ops->create(j->ops->ctx, stack, job_main, j);
    }
    JSValue o = JS_NewObjectClass(ctx, (int)cls(js, CLS_JOB));
    if (JS_IsException(o)) {
        if (j->co)
            j->ops->destroy(j->ops->ctx, j->co);
        au_free(j);
        return o;
    }
    JS_SetOpaque(o, j);
    return o;
}

/* ckJobStep(job) -> the key's handle when the generation is done,
 * undefined while it goes on; throws when it failed. */
static JSValue n_ck_job_step(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    CkJob *j = argc > 0 ? JS_GetOpaque(argv[0], cls(js, CLS_JOB)) : NULL;
    if (!j)
        return JS_ThrowTypeError(ctx, "not a job");
    if (!j->done) {
        if (j->co) {
            uint32_t slice = js->root->settings.worker_slice_ms ? js->root->settings.worker_slice_ms : 8;
            j->deadline = js_now(js) + slice;
            j->started = true;
            if (!j->ops->resume(j->ops->ctx, j->co) && !j->done) {
                j->err = AU_WC_EOP; /* the stack went away */
                j->done = true;
            }
            if (!j->done)
                return JS_UNDEFINED;
            j->ops->destroy(j->ops->ctx, j->co);
            j->co = NULL;
        } else {
            bool in_worker = js->worker && js_worker_can_wait(js);
            if (in_worker)
                au_tls_set_yield(worker_yield, js);
            j->err = au_wc_generate((AuWcKind)j->kind, j->bits, j->e, (AuWcAlg)j->alg, j->hash, &j->key);
            au_tls_set_yield(NULL, NULL);
            if (in_worker && js_worker_terminating(js) && j->err != AU_WC_OK)
                j->err = AU_WC_ECANCEL;
            j->done = true;
        }
    }
    if (j->err != AU_WC_OK)
        return ck_err(ctx, j->err, "The key could not be generated");
    if (!j->key)
        return ck_throw(ctx, "OperationError", "The key was taken already");
    AuWcKey *k = j->key;
    j->key = NULL;
    return wrap_key(js, k);
}

/* ---- Operations ---- */

/* ckSign(handle, alg, hash, data) -> ArrayBuffer */
static JSValue n_ck_sign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuWcKey *k = argc > 3 ? key_arg(js, argv[0]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    int alg = int_arg(ctx, argv[1]);
    AuDigestAlg hash = hash_arg(ctx, argv[2]);
    const uint8_t *data;
    size_t len;
    if (!bytes_arg(ctx, argv[3], &data, &len))
        return JS_EXCEPTION;
    NEED_STARTED(js, ctx);
    size_t cap = au_wc_sig_len(k);
    if (!cap)
        return ck_throw(ctx, "InvalidAccessError", "The key cannot sign");
    uint8_t *out = js_malloc(ctx, cap);
    if (!out)
        return JS_EXCEPTION;
    size_t n = 0;
    AuWcErr err = au_wc_sign(k, (AuWcAlg)alg, hash, data, len, out, cap, &n);
    if (err != AU_WC_OK) {
        js_free(ctx, out);
        return ck_err(ctx, err, "The signature could not be made");
    }
    return take_buffer(ctx, out, cap, n);
}

/* ckVerify(handle, alg, hash, anySalt, data, signature) -> bool */
static JSValue n_ck_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuWcKey *k = argc > 5 ? key_arg(js, argv[0]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    int alg = int_arg(ctx, argv[1]);
    AuDigestAlg hash = hash_arg(ctx, argv[2]);
    bool any = JS_ToBool(ctx, argv[3]);
    const uint8_t *data, *sig;
    size_t len, slen;
    if (!bytes_arg(ctx, argv[4], &data, &len) || !bytes_arg(ctx, argv[5], &sig, &slen))
        return JS_EXCEPTION;
    NEED_STARTED(js, ctx);
    bool ok = false;
    AuWcErr err = au_wc_verify(k, (AuWcAlg)alg, hash, any, data, len, sig, slen, &ok);
    if (err != AU_WC_OK)
        return ck_err(ctx, err, "The signature could not be checked");
    return JS_NewBool(ctx, ok);
}

/* ckPssSalt(handle, hash) -> the salt length a PSS signature gets */
static JSValue n_ck_pss_salt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuWcKey *k = argc > 1 ? key_arg(js_of_ctx(ctx), argv[0]) : NULL;
    AuDigestAlg h = argc > 1 ? hash_arg(ctx, argv[1]) : (AuDigestAlg)0;
    if (!k || !h)
        return JS_ThrowTypeError(ctx, "ckPssSalt: bad arguments");
    return JS_NewFloat64(ctx, (double)au_wc_pss_salt(k, h));
}

/* ckOaep(handle, decrypt, hash, label, data) -> ArrayBuffer */
static JSValue n_ck_oaep(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuWcKey *k = argc > 4 ? key_arg(js, argv[0]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    bool decrypt = JS_ToBool(ctx, argv[1]);
    AuDigestAlg hash = hash_arg(ctx, argv[2]);
    const uint8_t *label, *data;
    size_t llen, len;
    if (!bytes_arg(ctx, argv[3], &label, &llen) || !bytes_arg(ctx, argv[4], &data, &len))
        return JS_EXCEPTION;
    NEED_STARTED(js, ctx);
    size_t cap = (au_wc_key_bits(k) + 7) / 8;
    uint8_t *out = js_malloc(ctx, cap ? cap : 1);
    if (!out)
        return JS_EXCEPTION;
    size_t n = 0;
    AuWcErr err = au_wc_oaep(k, decrypt, hash, label, llen, data, len, out, cap, &n);
    if (err != AU_WC_OK) {
        memset(out, 0, cap);
        js_free(ctx, out);
        return ck_err(ctx, err, decrypt ? "The data could not be decrypted" : "The data could not be encrypted");
    }
    return take_buffer(ctx, out, cap, n);
}

/* ckAgree(privateHandle, publicHandle) -> ArrayBuffer of the secret */
static JSValue n_ck_agree(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuWcKey *k = argc > 1 ? key_arg(js, argv[0]) : NULL, *peer = argc > 1 ? key_arg(js, argv[1]) : NULL;
    if (!k || !peer)
        return JS_ThrowTypeError(ctx, "not a key");
    NEED_STARTED(js, ctx);
    uint8_t out[80];
    size_t n = 0;
    AuWcErr err = au_wc_agree(k, peer, out, sizeof out, &n);
    JSValue r = err == AU_WC_OK ? JS_NewArrayBufferCopy(ctx, out, n) : ck_err(ctx, err, "The keys do not agree");
    memset(out, 0, sizeof out);
    return r;
}

/* ckAes(handle, mode, decrypt, iv, aad, param, data) -> ArrayBuffer */
static JSValue n_ck_aes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuWcKey *k = argc > 6 ? key_arg(js, argv[0]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    int mode = int_arg(ctx, argv[1]), param = int_arg(ctx, argv[5]);
    bool decrypt = JS_ToBool(ctx, argv[2]);
    const uint8_t *iv, *aad, *data;
    size_t ivlen, aadlen, len;
    if (!bytes_arg(ctx, argv[3], &iv, &ivlen) || !bytes_arg(ctx, argv[4], &aad, &aadlen) ||
        !bytes_arg(ctx, argv[6], &data, &len))
        return JS_EXCEPTION;
    if (param < 0)
        return ck_throw(ctx, "OperationError", "Bad parameter");
    NEED_STARTED(js, ctx);
    size_t need = au_wc_aes_out_max((AuWcAes)mode, decrypt, len, (unsigned)param);
    if (!need)
        return ck_throw(ctx, "OperationError", decrypt ? "The data could not be decrypted"
                                                       : "The data could not be encrypted");
    /* room for the library's own idea of an update's output */
    size_t cap = need + 32;
    uint8_t *out = js_malloc(ctx, cap);
    if (!out)
        return JS_EXCEPTION;
    size_t n = 0;
    AuWcErr err = au_wc_aes(k, (AuWcAes)mode, decrypt, iv, ivlen, aad, aadlen, (unsigned)param, data, len, out, cap, &n);
    if (err != AU_WC_OK) {
        memset(out, 0, cap);
        js_free(ctx, out);
        return ck_err(ctx, err, decrypt ? "The data could not be decrypted" : "The data could not be encrypted");
    }
    return take_buffer(ctx, out, cap, n);
}

/* ckDerive(kind, hash, handle, salt, info or iterations, bytes) ->
 * ArrayBuffer; kind 1 is PBKDF2, 2 HKDF */
static JSValue n_ck_derive(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuWcKey *k = argc > 5 ? key_arg(js, argv[2]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    int kind = int_arg(ctx, argv[0]);
    AuDigestAlg hash = hash_arg(ctx, argv[1]);
    const uint8_t *salt, *info = NULL;
    size_t slen, ilen = 0;
    uint32_t iter = 0, n = 0;
    if (!bytes_arg(ctx, argv[3], &salt, &slen))
        return JS_EXCEPTION;
    if (kind == 1 ? JS_ToUint32(ctx, &iter, argv[4]) != 0 : !bytes_arg(ctx, argv[4], &info, &ilen))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &n, argv[5]))
        return JS_EXCEPTION;
    /* HKDF gives at most 255 blocks; PBKDF2 output is held to 1 MB */
    if (n > (1u << 20))
        return ck_throw(ctx, "OperationError", "The length is too large");
    NEED_STARTED(js, ctx);
    uint8_t *out = js_malloc(ctx, n ? n : 1);
    if (!out)
        return JS_EXCEPTION;
    AuWcErr err = kind == 1 ? au_wc_pbkdf2(hash, k, salt, slen, iter, out, n)
                            : au_wc_hkdf(hash, k, salt, slen, info, ilen, out, n);
    if (err != AU_WC_OK) {
        js_free(ctx, out);
        return ck_err(ctx, err, "The bits could not be derived");
    }
    return take_buffer(ctx, out, n ? n : 1, n);
}

/* ckHmac(hash, handle, data) -> ArrayBuffer */
static JSValue n_ck_hmac(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    AuJs *js = js_of_ctx(ctx);
    AuWcKey *k = argc > 2 ? key_arg(js, argv[1]) : NULL;
    if (!k)
        return JS_ThrowTypeError(ctx, "not a key");
    AuDigestAlg hash = hash_arg(ctx, argv[0]);
    const uint8_t *data;
    size_t len, klen;
    if (!bytes_arg(ctx, argv[2], &data, &len))
        return JS_EXCEPTION;
    const uint8_t *kb = au_wc_key_bytes(k, &klen);
    uint8_t out[AU_DIGEST_MAX];
    if (!hash || au_wc_key_kind(k) != AU_WC_SECRET || !au_hmac(hash, kb, klen, data ? data : out, len, out))
        return ck_throw(ctx, "OperationError", "The HMAC could not be computed");
    JSValue r = JS_NewArrayBufferCopy(ctx, out, au_digest_len(hash));
    memset(out, 0, sizeof out);
    return r;
}

/* ---- subtle.js ---- */

#ifdef AU_JS_PRELUDE_SOURCE
extern const char au_js_subtle_src[];
extern const size_t au_js_subtle_len;
#else
extern const unsigned char au_js_subtle_bc[];
extern const size_t au_js_subtle_bc_len;
#endif

/* N.subtle(): reads src/js/subtle.js (its bytecode, subtle.bc) and
 * returns the function it evaluates to. The prelude calls it once per
 * context, the first time script touches crypto.subtle. */
static JSValue n_subtle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
#ifdef AU_JS_PRELUDE_SOURCE
    return JS_Eval(ctx, au_js_subtle_src, au_js_subtle_len, "aura:subtle", JS_EVAL_TYPE_GLOBAL);
#else
    JSValue fn = JS_ReadObject(ctx, au_js_subtle_bc, au_js_subtle_bc_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(fn))
        return fn;
    return JS_EvalFunction(ctx, fn);
#endif
}

static const JSCFunctionListEntry crypto_natives[] = {
    JS_CFUNC_DEF("subtle", 0, n_subtle),
    JS_CFUNC_DEF("ckSecret", 1, n_ck_secret),
    JS_CFUNC_DEF("ckBytes", 1, n_ck_bytes),
    JS_CFUNC_DEF("ckBits", 1, n_ck_bits),
    JS_CFUNC_DEF("ckImport", 6, n_ck_import),
    JS_CFUNC_DEF("ckExport", 2, n_ck_export),
    JS_CFUNC_DEF("ckGenerate", 5, n_ck_generate),
    JS_CFUNC_DEF("ckJobStep", 1, n_ck_job_step),
    JS_CFUNC_DEF("ckSign", 4, n_ck_sign),
    JS_CFUNC_DEF("ckVerify", 6, n_ck_verify),
    JS_CFUNC_DEF("ckPssSalt", 2, n_ck_pss_salt),
    JS_CFUNC_DEF("ckOaep", 5, n_ck_oaep),
    JS_CFUNC_DEF("ckAgree", 2, n_ck_agree),
    JS_CFUNC_DEF("ckAes", 7, n_ck_aes),
    JS_CFUNC_DEF("ckDerive", 6, n_ck_derive),
    JS_CFUNC_DEF("ckHmac", 3, n_ck_hmac),
};

void js_natives_crypto_init(AuJs *js, JSValue N)
{
    AuJs *root = js->root;
    if (!root->crypto_cls[CLS_KEY]) {
        static const JSClassDef def_key = {"CryptoKeyHandle", .finalizer = key_finalizer};
        static const JSClassDef def_job = {"CryptoKeyJob", .finalizer = job_finalizer};
        JS_NewClassID(root->rt, &root->crypto_cls[CLS_KEY]);
        JS_NewClass(root->rt, root->crypto_cls[CLS_KEY], &def_key);
        JS_NewClassID(root->rt, &root->crypto_cls[CLS_JOB]);
        JS_NewClass(root->rt, root->crypto_cls[CLS_JOB], &def_job);
    }
    JS_SetPropertyFunctionList(js->ctx, N, crypto_natives, JS_COUNTOF(crypto_natives));
}
