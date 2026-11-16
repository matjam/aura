/* Fuzzes the event stream parser (src/net/sse.c). Each iteration mutates
 * a stream from the corpus and parses it twice: whole, and in random
 * pieces (some of one byte). Both must give the same callbacks, every
 * string a callback gets must be valid UTF-8 ending in NUL, and the
 * limits must hold. Small limits are used on some iterations so that the
 * over-long line and over-large event paths run.
 *
 *     fuzz_sse ITERATIONS SEED SECONDS
 *     fuzz_sse replay FILE...
 *
 * See tests/fuzz.h and docs/fuzzing.md. */
#include "../fuzz.h"
#include "net/sse.h"

static const char *const dict[] = {"data:", "data: ", "event:", "id:", "retry:", "\n", "\r", "\r\n", "\n\n", ":",
                                   " ", "\xEF\xBB\xBF", "\xC3\xA9", "\xF0\x9F\x98\x80", "\xFF", "\0", "message",
                                   "1000", "99999999999"};

typedef struct Out {
    FzBuf log;
    int bad;
    size_t max;
} Out;

static int valid_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        size_t k, j;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            k = 1, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            k = 2, cp = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            k = 3, cp = c & 0x07;
        else
            return 0;
        for (j = 1; j <= k; j++) {
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if ((k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return 0;
        i += k + 1;
    }
    return 1;
}

static void on_dispatch(void *ctx, const char *type, size_t tlen, const char *data, size_t dlen, const char *id,
                        size_t idlen)
{
    Out *o = ctx;
    if ((type && !valid_utf8((const unsigned char *)type, tlen)) ||
        (data && !valid_utf8((const unsigned char *)data, dlen)) || !valid_utf8((const unsigned char *)id, idlen) ||
        id[idlen] != 0 || (data && data[dlen] != 0) || (data && dlen > o->max))
        o->bad++;
    fz_insert(&o->log, o->log.n, data ? "E" : "I", 1);
    if (data) {
        fz_insert(&o->log, o->log.n, type, tlen);
        fz_insert(&o->log, o->log.n, "|", 1);
        fz_insert(&o->log, o->log.n, data, dlen);
    }
    fz_insert(&o->log, o->log.n, "|", 1);
    fz_insert(&o->log, o->log.n, id, idlen);
    fz_insert(&o->log, o->log.n, ";", 1);
}

static void on_retry(void *ctx, uint32_t ms)
{
    Out *o = ctx;
    char t[24];
    int n = snprintf(t, sizeof t, "R%lu;", (unsigned long)ms);
    if (ms > 0x7fffffffu)
        o->bad++;
    fz_insert(&o->log, o->log.n, t, (size_t)n);
}

static void parse(const FzBuf *in, int pieces, size_t max_line, size_t max_data, Out *o)
{
    AuSseParser p;
    AuSseCallbacks cb = {o, on_dispatch, on_retry};
    size_t at = 0;
    au_sse_init(&p);
    if (max_line) {
        p.max_line = max_line;
        p.max_data = max_data;
    }
    o->max = p.max_data;
    while (at < in->n) {
        size_t n = in->n - at;
        if (pieces) {
            size_t k = fz_rnd(3) ? 1 + fz_rnd(16) : 1 + fz_rnd((uint32_t)n);
            if (k < n)
                n = k;
        }
        /* a copy of just this piece, so the sanitizer sees a read past it */
        uint8_t *copy = (uint8_t *)malloc(n);
        memcpy(copy, in->d + at, n);
        au_sse_feed(&p, copy, n, &cb);
        free(copy);
        at += n;
    }
    au_sse_free(&p);
}

static void run_one(const FzBuf *in)
{
    Out whole = {{0}, 0, 0}, split = {{0}, 0, 0};
    size_t max_line = 0, max_data = 0;
    if (fz_rnd(3) == 0) {
        max_line = 1 + fz_rnd(64);
        max_data = 1 + fz_rnd(128);
    }
    parse(in, 0, max_line, max_data, &whole);
    parse(in, 1, max_line, max_data, &split);
    if (whole.bad || split.bad) {
        printf("fuzz_sse: a callback got a bad string at seed %lu iteration %ld\n", (unsigned long)fz_base_seed, fz_iter);
        abort();
    }
    if (whole.log.n != split.log.n || (whole.log.n && memcmp(whole.log.d, split.log.d, whole.log.n))) {
        printf("fuzz_sse: pieces changed the result at seed %lu iteration %ld\n", (unsigned long)fz_base_seed, fz_iter);
        abort();
    }
    free(whole.log.d);
    free(split.log.d);
}

static int fuzz_main(int argc, char **argv)
{
    long iters = 0, it;
    int first, i;
    FzCorpus corpus = {0};
    FzBuf in = {0};

    if (fz_args(argc, argv, &iters, &first)) {
        for (i = first; i < argc; i++) {
            if (!fz_read_file(argv[i], &in, 0))
                continue;
            fz_replay_seed(argv[i]);
            run_one(&in);
            printf("%s: ok\n", argv[i]);
        }
        return 0;
    }
    fz_corpus_add_str(&corpus, "retry: 1000\nid: 1\nevent: ping\ndata: hello\ndata: world\n\n");
    fz_corpus_add_str(&corpus, "\xEF\xBB\xBF:comment\r\ndata: caf\xC3\xA9\r\n\r\nid\r\rdata:x\r\r");
    fz_corpus_add_str(&corpus, "data\n\ndata:\n\nevent:\n\nid: a\n\n");
    fz_corpus_add_str(&corpus, "data: \xF0\x9F\x98\x80\xFF\xC3\n\nretry: 99999999999999\nid: x\n\n");
    for (it = fz_first; it < iters && !fz_expired(); it++) {
        fz_iteration(it);
        const FzBuf *seed = &corpus.v[fz_rnd((uint32_t)corpus.n)];
        fz_set(&in, seed->d, seed->n);
        fz_mutate(&in, dict, (int)(sizeof dict / sizeof dict[0]), &corpus, 8192);
        fz_save(in.d, in.n);
        run_one(&in);
    }
    printf("fuzz_sse: %ld iterations\n", it - fz_first);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
