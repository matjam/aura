/* Deflate encoder tests: every file given, and inputs made here (empty,
 * one byte, runs, random bytes, text longer than the window, matches at
 * the largest distance), compressed at each level and in each format,
 * whole and with random chunking of input and output, and decoded again
 * with Aura's inflate.
 *
 * usage: test_deflate FILE...
 *        test_deflate --bench FILE...        sizes and speed per level
 *        test_deflate --emit DIR FILE...     writes DIR/NAME.LEVEL.{raw,z,gz}
 *                                            for another decoder to check
 *                                            (check-deflate.py) */
#include "brotli_util.h"
#include "codec/codec.h"

#include <time.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

static const AuDeflateFormat dfmt[3] = {AU_DEFLATE_RAW, AU_DEFLATE_ZLIB, AU_DEFLATE_GZIP};
static const AuInflateFormat ifmt[3] = {AU_INFLATE_RAW, AU_INFLATE_ZLIB, AU_INFLATE_GZIP};
static const char *const fname[3] = {"raw", "z", "gz"};

/* Compresses with chunked input and output; the result in out. */
static void deflate_chunked(AuDeflateFormat fmt, int level, const uint8_t *in, size_t n, BrRng *rng, size_t ichunk,
                            size_t ochunk, AuBuf *out)
{
    AuDeflate *z = au_deflate_new(fmt, level);
    size_t pos = 0;
    uint8_t buf[4096];
    for (;;) {
        size_t ic = ichunk ? ichunk : br_rng_chunk(rng, 70000);
        size_t oc = ochunk ? ochunk : 1 + br_rng_next(rng) % sizeof buf;
        if (ic > n - pos)
            ic = n - pos;
        if (oc > sizeof buf)
            oc = sizeof buf;
        const uint8_t *ip = in + pos;
        size_t il = ic;
        uint8_t *op = buf;
        size_t ol = oc;
        AuCodecStatus st = au_deflate_run(z, &ip, &il, &op, &ol, pos + ic == n);
        pos += ic - il;
        au_buf_append(out, buf, oc - ol);
        if (st == AU_CODEC_DONE)
            break;
        if (st != AU_CODEC_OK) {
            CHECK(false, "the encoder returned an error");
            break;
        }
    }
    CHECK(pos == n, "all input was taken: %u of %u", (unsigned)pos, (unsigned)n);
    au_deflate_free(z);
}

static bool round_trip(int f, const AuBuf *z, const uint8_t *in, size_t n)
{
    uint8_t *back = NULL;
    size_t blen = 0;
    bool ok = au_inflate_all(ifmt[f], z->data, z->len, n + 16, &back, &blen);
    ok = ok && blen == n && (!n || !memcmp(back, in, n));
    au_free(back);
    return ok;
}

static void check_input(const char *name, const uint8_t *in, size_t n, BrRng *rng)
{
    for (int level = 0; level <= 2; level++) {
        for (int f = 0; f < 3; f++) {
            AuBuf whole, parts, tiny;
            au_buf_init(&whole);
            au_buf_init(&parts);
            au_buf_init(&tiny);
            au_deflate_all(dfmt[f], level, in, n, &whole);
            CHECK(round_trip(f, &whole, in, n), "%s level %d %s: round trip (%u to %u bytes)", name, level, fname[f],
                  (unsigned)n, (unsigned)whole.len);
            deflate_chunked(dfmt[f], level, in, n, rng, 0, 0, &parts);
            CHECK(round_trip(f, &parts, in, n), "%s level %d %s: round trip with random chunks", name, level,
                  fname[f]);
            if (n <= 40000) {
                deflate_chunked(dfmt[f], level, in, n, rng, 1, 1, &tiny);
                CHECK(round_trip(f, &tiny, in, n), "%s level %d %s: round trip a byte at a time", name, level,
                      fname[f]);
            }
            /* Never much larger than the input. */
            CHECK(whole.len <= n + 5 * (n / 16000 + 1) + 20, "%s level %d %s: %u bytes from %u", name, level,
                  fname[f], (unsigned)whole.len, (unsigned)n);
            au_buf_free(&whole);
            au_buf_free(&parts);
            au_buf_free(&tiny);
        }
    }
}

static void made_inputs(BrRng *rng)
{
    size_t cap = 400000;
    uint8_t *b = malloc(cap);
    check_input("empty", b, 0, rng);
    b[0] = 'x';
    check_input("one byte", b, 1, rng);
    memcpy(b, "ab", 2);
    check_input("two bytes", b, 2, rng);
    memcpy(b, "abc", 3);
    check_input("three bytes", b, 3, rng);
    memset(b, 0, cap);
    check_input("zeros", b, cap, rng);
    check_input("zeros to the window's end", b, 65536, rng);
    check_input("zeros past it by one", b, 65537, rng);
    for (size_t i = 0; i < cap; i++)
        b[i] = (uint8_t)br_rng_next(rng);
    check_input("random bytes", b, 200000, rng);
    /* Text-like: words drawn from a small set, longer than the window
     * several times over. */
    static const char *const words[] = {"the ",  "window ", "slides ", "<div class=\"x\">", "</div>\n", "match ",
                                        "and ",  "literal", " ",       "0123456789",        "aaaaaaaa", "\n"};
    size_t n = 0;
    while (n + 20 < cap) {
        const char *w = words[br_rng_next(rng) % 12];
        size_t l = strlen(w);
        memcpy(b + n, w, l);
        n += l;
    }
    check_input("words", b, n, rng);
    /* A block repeated at the largest distance a match may have, and
     * just past it. */
    for (int k = 0; k < 2; k++) {
        size_t dist = 32768 - 262 + (size_t)k;
        for (size_t i = 0; i < dist; i++)
            b[i] = (uint8_t)br_rng_next(rng);
        memcpy(b + dist, b, 3000);
        check_input(k ? "a repeat past the window" : "a repeat at the window's reach", b, dist + 3000, rng);
    }
    /* Runs of every length around the longest match. */
    n = 0;
    for (int len = 250; len < 270; len++) {
        memset(b + n, 'a' + len % 20, (size_t)len);
        n += (size_t)len;
        b[n++] = (uint8_t)len;
    }
    check_input("runs around 258", b, n, rng);
    /* One distinct symbol and two (the smallest Huffman codes). */
    memset(b, 'q', 5);
    check_input("five of one byte", b, 5, rng);
    for (size_t i = 0; i < 4000; i++)
        b[i] = (br_rng_next(rng) & 1) ? 'a' : 'b';
    check_input("two symbols", b, 4000, rng);
    /* Very skewed frequencies: code lengths want to exceed 15 bits. */
    n = 0;
    for (int s = 0; s < 40 && n < cap; s++) {
        size_t count = (size_t)1 << (s < 17 ? s : 17);
        for (size_t i = 0; i < count && n < cap; i++)
            b[n++] = (uint8_t)(s * 5 + (int)(br_rng_next(rng) % 3 == 0));
    }
    /* Shuffled a little so that matches do not swallow everything. */
    for (size_t i = 0; i + 1 < n; i += 2) {
        size_t j = br_rng_next(rng) % n;
        uint8_t t = b[i];
        b[i] = b[j];
        b[j] = t;
    }
    check_input("skewed frequencies", b, n, rng);
    free(b);
}

static void test_reset_and_formats(void)
{
    const char *text = "one stream, then another from the same encoder: the second must not see the first";
    size_t n = strlen(text);
    AuDeflate *z = au_deflate_new(AU_DEFLATE_GZIP, 2);
    AuBuf a, b;
    au_buf_init(&a);
    au_buf_init(&b);
    for (int pass = 0; pass < 2; pass++) {
        AuBuf *o = pass ? &b : &a;
        const uint8_t *ip = (const uint8_t *)text;
        size_t il = n;
        au_buf_reserve(o, 4096);
        uint8_t *op = (uint8_t *)o->data;
        size_t ol = 4096;
        AuCodecStatus st = au_deflate_run(z, &ip, &il, &op, &ol, true);
        o->len = 4096 - ol;
        CHECK(st == AU_CODEC_DONE && il == 0, "pass %d finishes in one call", pass);
        /* Calling again after the end changes nothing. */
        st = au_deflate_run(z, &ip, &il, &op, &ol, true);
        CHECK(st == AU_CODEC_DONE && o->len == 4096 - ol, "a call after the end writes nothing");
        au_deflate_reset(z);
    }
    CHECK(a.len == b.len && !memcmp(a.data, b.data, a.len), "a reset encoder gives the same stream");
    CHECK(a.len > 18 && (uint8_t)a.data[0] == 0x1F && (uint8_t)a.data[1] == 0x8B && a.data[2] == 8,
          "the gzip header");
    CHECK(round_trip(2, &a, (const uint8_t *)text, n), "gzip decodes");
    au_deflate_free(z);
    au_buf_clear(&a);
    au_deflate_all(AU_DEFLATE_ZLIB, 1, text, n, &a);
    CHECK(a.len > 6 && (a.data[0] & 0x0F) == 8 && (((uint8_t)a.data[0] << 8) | (uint8_t)a.data[1]) % 31 == 0,
          "the zlib header checks");
    /* The HTTP "deflate" decoder takes both the zlib and the raw form. */
    uint8_t *back;
    size_t blen;
    CHECK(au_inflate_all(AU_INFLATE_HTTP_DEFLATE, a.data, a.len, n, &back, &blen) && blen == n, "zlib as http deflate");
    au_free(back);
    au_buf_free(&a);
    au_buf_free(&b);
    au_deflate_free(NULL);
}

static double now_s(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

static void bench(const char *path, const uint8_t *in, size_t n)
{
    for (int level = 1; level <= 2; level++) {
        AuBuf z;
        au_buf_init(&z);
        int reps = 0;
        double t0 = now_s(), t = 0;
        do {
            au_buf_clear(&z);
            au_deflate_all(AU_DEFLATE_RAW, level, in, n, &z);
            reps++;
            t = now_s() - t0;
        } while (t < 0.3);
        uint8_t *back;
        size_t blen;
        int ireps = 0;
        double i0 = now_s(), it = 0;
        do {
            au_inflate_all(AU_INFLATE_RAW, z.data, z.len, n + 1, &back, &blen);
            au_free(back);
            ireps++;
            it = now_s() - i0;
        } while (it < 0.3);
        printf("%-40s level %d: %8u -> %8u bytes (%4.1f%%), deflate %6.1f MB/s, inflate %6.1f MB/s\n", path, level,
               (unsigned)n, (unsigned)z.len, n ? 100.0 * (double)z.len / (double)n : 0,
               (double)n * reps / t / 1e6, (double)n * ireps / it / 1e6);
        au_buf_free(&z);
    }
}

static void emit(const char *dir, const char *path, const uint8_t *in, size_t n)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int level = 0; level <= 2; level++) {
        for (int f = 0; f < 3; f++) {
            AuBuf z;
            au_buf_init(&z);
            au_deflate_all(dfmt[f], level, in, n, &z);
            char out[1024];
            snprintf(out, sizeof out, "%s/%s.%d.%s", dir, base, level, fname[f]);
            FILE *fp = fopen(out, "wb");
            if (!fp || (z.len && fwrite(z.data, 1, z.len, fp) != z.len)) {
                printf("cannot write %s\n", out);
                failures++;
            }
            if (fp)
                fclose(fp);
            au_buf_free(&z);
        }
    }
}

int main(int argc, char **argv)
{
    BrRng rng = {12345};
    int first = 1;
    bool benching = argc > 1 && !strcmp(argv[1], "--bench");
    const char *emit_dir = argc > 2 && !strcmp(argv[1], "--emit") ? argv[2] : NULL;
    if (benching)
        first = 2;
    if (emit_dir)
        first = 3;
    for (int i = first; i < argc; i++) {
        size_t n = 0;
        uint8_t *in = br_read_file(argv[i], &n);
        if (!in) {
            printf("cannot read %s\n", argv[i]);
            failures++;
            continue;
        }
        if (benching)
            bench(argv[i], in, n);
        else if (emit_dir)
            emit(emit_dir, argv[i], in, n);
        else
            check_input(argv[i], in, n, &rng);
        free(in);
    }
    if (!benching && !emit_dir) {
        made_inputs(&rng);
        test_reset_and_formats();
        printf("deflate: %d checks, %d failures\n", checks, failures);
    }
    return failures ? 1 : 0;
}
