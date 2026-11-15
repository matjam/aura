/* Brotli decoder tests: the vectors in brotli-vectors decode byte-exact
 * under any chunking; truncated, corrupt and hand-made streams.
 *
 * Usage: test_brotli VECTOR.br...
 *        test_brotli --bench FILE.br [ORIGINAL]   (decode speed) */
#include <time.h>

#include "brotli_util.h"

static int failures;
static AuBrotliDict *dict;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fprintf(stderr, "\n");                                                                           \
        }                                                                                                    \
    } while (0)

typedef struct {
    const char *name;
    uint8_t *br, *orig;
    size_t br_len, orig_len;
} Vector;

/* NAME.qQwW.br decodes to NAME; craft-X.br to craft-X. */
static bool load_vector(const char *path, Vector *v)
{
    char opath[1024];
    const char *base = strrchr(path, '/');
    size_t n = strlen(path);
    char *q;
    base = base ? base + 1 : path;
    v->name = base;
    if (n < 4 || n >= sizeof opath)
        return false;
    memcpy(opath, path, n - 3);
    opath[n - 3] = 0;
    q = strstr(opath + (base - path), ".q");
    if (q && strncmp(base, "craft-", 6) != 0)
        *q = 0;
    v->br = br_read_file(path, &v->br_len);
    if (!v->br)
        return false;
    if (strncmp(base, "gen-words.", 10) == 0) {
        v->orig_len = 200000;
        v->orig = br_gen_words(v->orig_len);
        return true;
    }
    v->orig = br_read_file(opath, &v->orig_len);
    return v->orig != NULL;
}

static void check_result(const Vector *v, BrResult r, const char *how)
{
    CHECK(!r.stalled, "%s %s: stalled", v->name, how);
    CHECK(r.st == AU_CODEC_DONE, "%s %s: status %d", v->name, how, (int)r.st);
    CHECK(r.out_len == v->orig_len && memcmp(r.out, v->orig, v->orig_len) == 0, "%s %s: output differs (%zu vs %zu)",
          v->name, how, r.out_len, v->orig_len);
    CHECK(r.in_left == 0, "%s %s: %zu input bytes left", v->name, how, r.in_left);
}

static void test_vector(const Vector *v)
{
    BrResult r;
    BrRng in_rng = {12345}, out_rng = {777};
    int k;
    /* gen-words and text.q11w24 need 2^24. */
    int maxw = 24;

    r = br_decode(dict, maxw, v->br, v->br_len, v->orig_len + 64, NULL, NULL);
    check_result(v, r, "whole");
    free(r.out);

    /* One byte of input and one byte of output space per call. */
    {
        AuBrotli *b = au_brotli_new(dict, maxw);
        uint8_t *out = malloc(v->orig_len + 1);
        size_t ip = 0, op = 0;
        AuCodecStatus st = AU_CODEC_OK;
        while (st == AU_CODEC_OK) {
            const uint8_t *i = v->br + ip;
            uint8_t *o = out + op;
            size_t il = ip < v->br_len ? 1 : 0, ol = op < v->orig_len ? 1 : 0, il0 = il, ol0 = ol;
            st = au_brotli_run(b, &i, &il, &o, &ol);
            ip += il0 - il;
            op += ol0 - ol;
            if (st == AU_CODEC_OK && il0 == il && ol0 == ol)
                break;
        }
        CHECK(st == AU_CODEC_DONE && op == v->orig_len && ip == v->br_len && memcmp(out, v->orig, op) == 0,
              "%s bytewise: status %d, out %zu of %zu, in %zu of %zu", v->name, (int)st, op, v->orig_len, ip,
              v->br_len);
        au_brotli_free(b);
        free(out);
    }

    for (k = 0; k < 4; k++) {
        r = br_decode(dict, maxw, v->br, v->br_len, v->orig_len + 64, &in_rng, &out_rng);
        check_result(v, r, "random chunks");
        free(r.out);
    }

    /* Trailing bytes stay in the input. */
    {
        uint8_t *buf = malloc(v->br_len + 3);
        memcpy(buf, v->br, v->br_len);
        memcpy(buf + v->br_len, "XYZ", 3);
        r = br_decode(dict, maxw, buf, v->br_len + 3, v->orig_len + 64, NULL, NULL);
        CHECK(r.st == AU_CODEC_DONE && r.in_left == 3 && r.out_len == v->orig_len, "%s trailing: st %d left %zu",
              v->name, (int)r.st, r.in_left);
        free(r.out);
        r = br_decode(dict, maxw, buf, v->br_len + 3, v->orig_len + 64, &in_rng, &out_rng);
        CHECK(r.st == AU_CODEC_DONE && r.in_left == 3 && r.out_len == v->orig_len,
              "%s trailing, chunked: st %d left %zu", v->name, (int)r.st, r.in_left);
        free(r.out);
        free(buf);
    }
}

/* Every prefix: never DONE, never a stall, output a prefix of the
 * original. Vectors over 1200 bytes are cut at a stride, and at each of
 * their last 64 bytes. */
static void test_truncation(const Vector *v)
{
    size_t step = v->br_len > 1200 ? v->br_len / 400 : 1, n;
    for (n = 0; n < v->br_len; n = (n + step < v->br_len - 64 || step == 1) ? n + step : n + 1) {
        BrResult r = br_decode(dict, 24, v->br, n, v->orig_len + 64, NULL, NULL);
        CHECK(r.st != AU_CODEC_DONE, "%s: prefix %zu reports DONE", v->name, n);
        CHECK(!r.stalled, "%s: prefix %zu stalled", v->name, n);
        CHECK(r.out_len <= v->orig_len && memcmp(r.out, v->orig, r.out_len) == 0, "%s: prefix %zu output wrong",
              v->name, n);
        free(r.out);
    }
}

/* Corrupt copies: no crash and no stall (ASan finds the rest). */
static void test_corruption(const Vector *v)
{
    BrRng rng = {(uint32_t)v->br_len * 2654435761u + 1};
    uint8_t *buf = malloc(v->br_len + 1);
    int k;
    if (!v->br_len)
        return;
    for (k = 0; k < 300; k++) {
        BrResult r;
        memcpy(buf, v->br, v->br_len);
        if (k & 1) {
            size_t bit = br_rng_next(&rng) % (v->br_len * 8);
            buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        } else {
            buf[br_rng_next(&rng) % v->br_len] = (uint8_t)br_rng_next(&rng);
        }
        r = br_decode(dict, 24, buf, v->br_len, v->orig_len + 4096, (k & 2) ? &rng : NULL, NULL);
        CHECK(!r.stalled, "%s: corrupt copy %d stalled", v->name, k);
        free(r.out);
    }
    free(buf);
}

static BrResult decode_bytes(const AuBrotliDict *d, int maxw, const uint8_t *p, size_t n)
{
    return br_decode(d, maxw, p, n, 1 << 16, NULL, NULL);
}

static void test_hand_made(void)
{
    BrResult r;
    static const uint8_t empty16[] = {0x06};
    static const uint8_t empty17[] = {0x81, 0x01};
    static const uint8_t pad_bits[] = {0x16};
    static const uint8_t large_window[] = {0x11, 0x00};
    static const uint8_t last_metadata[] = {0x1A, 0x00};
    static const uint8_t metadata_reserved[] = {0x1C, 0x00};

    r = decode_bytes(NULL, 0, empty16, 1);
    CHECK(r.st == AU_CODEC_DONE && r.out_len == 0 && r.in_left == 0, "0x06: st %d", (int)r.st);
    free(r.out);
    r = decode_bytes(NULL, 0, empty17, 2);
    CHECK(r.st == AU_CODEC_DONE && r.out_len == 0 && r.in_left == 0, "WBITS 17 empty: st %d", (int)r.st);
    free(r.out);
    r = decode_bytes(NULL, 0, pad_bits, 1);
    CHECK(r.st == AU_CODEC_ERROR, "nonzero end padding: st %d", (int)r.st);
    free(r.out);
    r = decode_bytes(NULL, 0, large_window, 2);
    CHECK(r.st == AU_CODEC_ERROR, "large window: st %d", (int)r.st);
    free(r.out);
    /* An empty metadata block as the last meta-block ends the stream. */
    r = decode_bytes(NULL, 0, last_metadata, 2);
    CHECK(r.st == AU_CODEC_DONE && r.in_left == 1 && r.out_len == 0, "metadata as the last meta-block: st %d",
          (int)r.st);
    free(r.out);
    r = decode_bytes(NULL, 0, metadata_reserved, 2);
    CHECK(r.st == AU_CODEC_ERROR, "metadata reserved bit: st %d", (int)r.st);
    free(r.out);

    /* No input at all: OK, not done. */
    {
        AuBrotli *b = au_brotli_new(NULL, 0);
        const uint8_t *in = empty16;
        size_t il = 0, ol = 16;
        uint8_t out[16], *o = out;
        CHECK(au_brotli_run(b, &in, &il, &o, &ol) == AU_CODEC_OK, "no input");
        /* No output space either: the empty stream still ends. */
        il = 1;
        ol = 0;
        CHECK(au_brotli_run(b, &in, &il, &o, &ol) == AU_CODEC_DONE && il == 0, "empty stream, no output space");
        /* DONE again, consuming nothing. */
        il = 1;
        in = empty16;
        CHECK(au_brotli_run(b, &in, &il, &o, &ol) == AU_CODEC_DONE && il == 1, "after DONE");
        au_brotli_free(b);
    }

    /* Errors stick and have a message. */
    {
        AuBrotli *b = au_brotli_new(NULL, 0);
        const uint8_t *in = large_window;
        size_t il = 2, ol = 16;
        uint8_t out[16], *o = out;
        CHECK(au_brotli_run(b, &in, &il, &o, &ol) == AU_CODEC_ERROR, "large window error");
        CHECK(au_brotli_error(b)[0] != 0, "error message");
        in = empty16;
        il = 1;
        CHECK(au_brotli_run(b, &in, &il, &o, &ol) == AU_CODEC_ERROR, "error sticks");
        au_brotli_free(b);
    }
}

static Vector *find(Vector *vs, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(vs[i].name, name) == 0)
            return &vs[i];
    return NULL;
}

static void test_window_cap(Vector *vs, int n)
{
    static const char *const names[] = {"text.q11w24.br", "gen-words.q5w24.br"};
    size_t k;
    for (k = 0; k < 2; k++) {
        Vector *v = find(vs, n, names[k]);
        BrResult r;
        CHECK(v != NULL, "%s missing", names[k]);
        if (!v)
            continue;
        r = br_decode(dict, 22, v->br, v->br_len, v->orig_len, NULL, NULL);
        CHECK(r.st == AU_CODEC_ERROR && r.out_len == 0, "%s with a 22-bit cap: st %d", v->name, (int)r.st);
        free(r.out);
        r = br_decode(dict, 0, v->br, v->br_len, v->orig_len, NULL, NULL);
        CHECK(r.st == AU_CODEC_ERROR, "%s with the default cap: st %d", v->name, (int)r.st);
        free(r.out);
        r = br_decode(dict, 24, v->br, v->br_len, v->orig_len, NULL, NULL);
        CHECK(r.st == AU_CODEC_DONE && r.out_len == v->orig_len, "%s with a 24-bit cap: st %d", v->name, (int)r.st);
        free(r.out);
    }
    /* A 22-bit stream decodes under the default cap. */
    {
        Vector *v = find(vs, n, "text.q5w22.br");
        CHECK(v != NULL, "text.q5w22.br missing");
        if (v) {
            BrResult r = br_decode(dict, 0, v->br, v->br_len, v->orig_len, NULL, NULL);
            CHECK(r.st == AU_CODEC_DONE, "text.q5w22 default cap: st %d", (int)r.st);
            free(r.out);
        }
    }
}

static void test_no_dict(Vector *vs, int n)
{
    Vector *v = find(vs, n, "craft-dict.br");
    CHECK(v != NULL, "craft-dict.br missing");
    if (v) {
        BrResult r = br_decode(NULL, 0, v->br, v->br_len, v->orig_len, NULL, NULL);
        CHECK(r.st == AU_CODEC_ERROR, "dictionary stream without a dictionary: st %d", (int)r.st);
        free(r.out);
    }
    /* A stream without dictionary words needs none. */
    v = find(vs, n, "repeat.q0w16.br");
    if (v) {
        BrResult r = br_decode(NULL, 0, v->br, v->br_len, v->orig_len, NULL, NULL);
        CHECK(r.st == AU_CODEC_DONE && r.out_len == v->orig_len, "repeat without a dictionary: st %d", (int)r.st);
        free(r.out);
    }
}

static void test_decoder_interface(Vector *vs, int n)
{
    Vector *v = find(vs, n, "page.html.q11w16.br");
    AuDecoder *d;
    const uint8_t *in;
    uint8_t *out, *o;
    size_t il, ol;
    if (!v)
        return;
    d = au_brotli_decoder(dict, 0);
    in = v->br;
    il = v->br_len;
    out = malloc(v->orig_len);
    o = out;
    ol = v->orig_len;
    CHECK(au_decoder_run(d, &in, &il, &o, &ol) == AU_CODEC_DONE && ol == 0 && memcmp(out, v->orig, v->orig_len) == 0,
          "AuDecoder interface");
    CHECK(au_decoder_error(d)[0] == 0, "no error message after success");
    au_decoder_free(d);
    free(out);
}

static void test_dict(void)
{
    const uint8_t *p = au_brotli_dict_bytes(dict);
    CHECK(memcmp(p, "timedownlifeleftback", 20) == 0, "dictionary start");
    CHECK(au_crc32(0, p, AU_BROTLI_DICT_SIZE) == 0x5136CB04u, "dictionary CRC");
}

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

static int bench(const char *path)
{
    size_t len, out_len = 0;
    uint8_t *br = br_read_file(path, &len), *out;
    size_t cap = 64u << 20;
    double t0, t;
    int rounds = 0;
    if (!br)
        return 1;
    out = malloc(cap);
    t0 = now();
    do {
        AuBrotli *b = au_brotli_new(dict, 24);
        const uint8_t *in = br;
        size_t il = len, ol = cap;
        uint8_t *o = out;
        if (au_brotli_run(b, &in, &il, &o, &ol) != AU_CODEC_DONE) {
            fprintf(stderr, "bench: decode failed: %s\n", au_brotli_error(b));
            return 1;
        }
        out_len = cap - ol;
        au_brotli_free(b);
        rounds++;
        t = now() - t0;
    } while (t < 2.0);
    printf("%s: %zu bytes out, %.1f MB/s\n", path, out_len, (double)out_len * rounds / t / 1e6);
    return 0;
}

int main(int argc, char **argv)
{
    Vector *vs;
    int n = 0, i;
    dict = au_brotli_dict_new();
    if (!dict) {
        fprintf(stderr, "FAIL: au_brotli_dict_new\n");
        return 1;
    }
    if (argc == 3 && strcmp(argv[1], "--bench") == 0)
        return bench(argv[2]);
    test_dict();
    vs = calloc((size_t)argc, sizeof *vs);
    for (i = 1; i < argc; i++) {
        if (!load_vector(argv[i], &vs[n])) {
            fprintf(stderr, "FAIL: cannot load %s or its original\n", argv[i]);
            failures++;
            continue;
        }
        n++;
    }
    CHECK(n >= 30, "only %d vectors", n);
    for (i = 0; i < n; i++) {
        test_vector(&vs[i]);
        test_truncation(&vs[i]);
        test_corruption(&vs[i]);
    }
    test_hand_made();
    test_window_cap(vs, n);
    test_no_dict(vs, n);
    test_decoder_interface(vs, n);
    for (i = 0; i < n; i++) {
        free(vs[i].br);
        free(vs[i].orig);
    }
    free(vs);
    au_brotli_dict_free(dict);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("brotli: %d vectors ok\n", n);
    return 0;
}
