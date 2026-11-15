/* Shared parts of the deterministic fuzzers: a PRNG, a wall-clock cap, a
 * corpus read from files, byte mutation with a token dictionary, and
 * saving the current input.
 *
 * A fuzzer built on this takes
 *
 *     fuzz_X ITERATIONS SEED SECONDS [CORPUS FILE...]
 *     fuzz_X replay FILE...
 *
 * It stops at the iteration count or after SECONDS (0: no cap), whichever
 * comes first, and prints the iterations done. The environment variable
 * AURA_FUZZ_SECONDS caps a fuzzer that takes no SECONDS argument. With
 * AURA_FUZZ_SAVE=PATH each input is written to PATH before it is used, so
 * after a crash or a hang PATH holds the input; `replay` runs saved
 * inputs unchanged. With AURA_FUZZ_STACK_KB=N the fuzzer runs on a thread
 * whose stack is N KB, so recursion over input nesting overflows it as it
 * would on Mac OS 9 (256 KB there; sanitizer frames are several times
 * larger, so use a plain build for this). Run the fuzzers from the
 * sanitizer build. */
#ifndef AURA_TESTS_FUZZ_H
#define AURA_TESTS_FUZZ_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

static uint32_t fz_state = 1;

static inline uint32_t fz_next(void)
{
    fz_state ^= fz_state << 13;
    fz_state ^= fz_state >> 17;
    fz_state ^= fz_state << 5;
    return fz_state;
}

/* A number below n (0 when n is 0). */
static inline uint32_t fz_rnd(uint32_t n) { return n ? fz_next() % n : 0; }

static inline void fz_seed(uint32_t s) { fz_state = s ? s : 1; }

/* ---- the time cap ---- */
static time_t fz_deadline;

static inline void fz_cap(long seconds)
{
    const char *e = getenv("AURA_FUZZ_SECONDS");
    if (seconds <= 0 && e)
        seconds = atol(e);
    fz_deadline = seconds > 0 ? time(NULL) + seconds : 0;
}

static inline int fz_expired(void) { return fz_deadline && time(NULL) >= fz_deadline; }

/* For a fuzzer with no SECONDS argument: true once AURA_FUZZ_SECONDS
 * have passed since the first call. */
static inline int fz_capped(void)
{
    static int started;
    if (!started) {
        started = 1;
        fz_cap(0);
    }
    return fz_expired();
}

/* ---- inputs ---- */
typedef struct FzBuf {
    uint8_t *d;
    size_t n, cap;
} FzBuf;

static inline void fz_reserve(FzBuf *b, size_t n)
{
    if (n + 1 > b->cap) {
        b->cap = (n + 1) * 2 + 64;
        b->d = (uint8_t *)realloc(b->d, b->cap);
        if (!b->d)
            abort();
    }
}

static inline void fz_set(FzBuf *b, const void *src, size_t n)
{
    fz_reserve(b, n);
    if (n)
        memcpy(b->d, src, n);
    b->n = n;
    b->d[n] = 0;
}

static inline void fz_insert(FzBuf *b, size_t at, const void *src, size_t n)
{
    if (at > b->n)
        at = b->n;
    fz_reserve(b, b->n + n);
    memmove(b->d + at + n, b->d + at, b->n - at);
    if (n)
        memcpy(b->d + at, src, n);
    b->n += n;
    b->d[b->n] = 0;
}

static inline void fz_delete(FzBuf *b, size_t at, size_t n)
{
    if (at >= b->n)
        return;
    if (n > b->n - at)
        n = b->n - at;
    memmove(b->d + at, b->d + at + n, b->n - at - n);
    b->n -= n;
    b->d[b->n] = 0;
}

static inline void fz_puts(FzBuf *b, const char *s) { fz_insert(b, b->n, s, strlen(s)); }

typedef struct FzCorpus {
    FzBuf *v;
    int n;
} FzCorpus;

static inline int fz_read_file(const char *path, FzBuf *out, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || (max && (size_t)len > max)) {
        fclose(f);
        return 0;
    }
    memset(out, 0, sizeof *out);
    fz_reserve(out, (size_t)len);
    out->n = fread(out->d, 1, (size_t)len, f);
    out->d[out->n] = 0;
    fclose(f);
    return 1;
}

/* Adds a file to the corpus; files over max bytes are skipped. */
static inline void fz_corpus_add_file(FzCorpus *c, const char *path, size_t max)
{
    FzBuf b;
    if (!fz_read_file(path, &b, max))
        return;
    c->v = (FzBuf *)realloc(c->v, (size_t)(c->n + 1) * sizeof *c->v);
    c->v[c->n++] = b;
}

static inline void fz_corpus_add(FzCorpus *c, const void *d, size_t n)
{
    FzBuf b;
    memset(&b, 0, sizeof b);
    fz_set(&b, d, n);
    c->v = (FzBuf *)realloc(c->v, (size_t)(c->n + 1) * sizeof *c->v);
    c->v[c->n++] = b;
}

static inline void fz_corpus_add_str(FzCorpus *c, const char *s) { fz_corpus_add(c, s, strlen(s)); }

/* Mutates b in place: 1 to 8 of bit flips, byte changes, interesting
 * bytes, dictionary tokens, repeated tokens (nesting), deletes, copies of
 * a range of b, splices from another corpus entry, truncation and number
 * replacement. The result never exceeds max bytes. */
static inline void fz_mutate(FzBuf *b, const char *const *dict, int ndict, const FzCorpus *c, size_t max)
{
    static const char *const nums[] = {"0",     "-1",        "1e9",      "1e38",       "-1e38", "4294967296", "2147483647",
                                       "65536", "-2147483648", "99999999999999999999", "1e-45", ".5",         "0x7fffffff",
                                       "NaN",   "infinity",  "32767",    "16777216",   "255",   "256",        "1e400"};
    int k, ops = 1 + (int)fz_rnd(8);
    for (k = 0; k < ops; k++) {
        size_t at = b->n ? fz_rnd((uint32_t)b->n) : 0;
        switch (fz_rnd(14)) {
        case 0:
            if (b->n)
                b->d[at] ^= (uint8_t)(1u << fz_rnd(8));
            break;
        case 1:
            if (b->n)
                b->d[at] = (uint8_t)fz_next();
            break;
        case 2:
            if (b->n) {
                static const uint8_t interesting[] = {0, 0xFF, 0x7F, 0x80, '\n', '\r', '<', '>', '"', '\'', '&',
                                                      '{', '}', '(', ')',  ';',  ':',  '\\', '/', ' ',  '\t', 0xC3, 0xED, 0xF4};
                b->d[at] = interesting[fz_rnd(sizeof interesting)];
            }
            break;
        case 3:
        case 4:
        case 5:
            if (ndict) {
                const char *t = dict[fz_rnd((uint32_t)ndict)];
                fz_insert(b, at, t, strlen(t));
            }
            break;
        case 6:
            if (ndict) {
                /* the same token many times: nesting */
                const char *t = dict[fz_rnd((uint32_t)ndict)];
                size_t tl = strlen(t);
                int reps = 1 << fz_rnd(11), r;
                if (fz_rnd(4))
                    reps = 1 + (int)fz_rnd(40);
                for (r = 0; r < reps && b->n + tl <= max; r++)
                    fz_insert(b, at, t, tl);
            }
            break;
        case 7:
            fz_delete(b, at, 1 + fz_rnd(fz_rnd(4) ? 8 : 400));
            break;
        case 8:
            if (b->n) {
                /* copy a range of the input elsewhere */
                size_t from = fz_rnd((uint32_t)b->n), len = 1 + fz_rnd(fz_rnd(3) ? 32 : 2000);
                uint8_t *tmp;
                if (len > b->n - from)
                    len = b->n - from;
                tmp = (uint8_t *)malloc(len + 1);
                memcpy(tmp, b->d + from, len);
                fz_insert(b, at, tmp, len);
                free(tmp);
            }
            break;
        case 9:
            if (c && c->n) {
                const FzBuf *o = &c->v[fz_rnd((uint32_t)c->n)];
                if (o->n) {
                    size_t from = fz_rnd((uint32_t)o->n), len = 1 + fz_rnd(fz_rnd(3) ? 64 : 4000);
                    if (len > o->n - from)
                        len = o->n - from;
                    fz_insert(b, at, o->d + from, len);
                }
            }
            break;
        case 10:
            if (fz_rnd(4) == 0)
                b->n = at, b->d[at] = 0;
            break;
        case 11:
        case 12: {
            /* replace a run of digits (or insert a number) */
            size_t e = at;
            const char *v = nums[fz_rnd(sizeof nums / sizeof nums[0])];
            while (e < b->n && e - at < 12 && ((b->d[e] >= '0' && b->d[e] <= '9') || b->d[e] == '.'))
                e++;
            fz_delete(b, at, e - at);
            fz_insert(b, at, v, strlen(v));
            break;
        }
        default:
            if (b->n > 1) {
                /* swap two bytes */
                size_t o = fz_rnd((uint32_t)b->n);
                uint8_t t = b->d[at];
                b->d[at] = b->d[o];
                b->d[o] = t;
            }
            break;
        }
        if (b->n > max) {
            b->n = max;
            b->d[max] = 0;
        }
    }
}

/* Writes the input to $AURA_FUZZ_SAVE when that is set. */
static inline void fz_save(const void *d, size_t n)
{
    static const char *path;
    static int looked;
    FILE *f;
    if (!looked) {
        path = getenv("AURA_FUZZ_SAVE");
        looked = 1;
    }
    if (!path)
        return;
    f = fopen(path, "wb");
    if (f) {
        fwrite(d, 1, n, f);
        fclose(f);
    }
}

/* The common command line. Returns 1 for `replay` (argv[2..] are the files
 * to run), 0 for a fuzz run, in which case *iters is set, the PRNG seeded,
 * the cap started and *first names the first corpus argument. */
static inline int fz_args(int argc, char **argv, long *iters, int *first)
{
    if (argc > 1 && !strcmp(argv[1], "replay")) {
        *first = 2;
        fz_seed(1);
        fz_cap(0);
        return 1;
    }
    *iters = argc > 1 ? atol(argv[1]) : 1000;
    fz_seed(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1);
    fz_cap(argc > 3 ? atol(argv[3]) : 60);
    *first = argc > 4 ? 4 : argc;
    return 0;
}

/* ---- a small stack ---- */
typedef struct FzMain {
    int (*fn)(int, char **);
    int argc;
    char **argv;
    int rc;
} FzMain;

static inline void *fz_thread(void *p)
{
    FzMain *m = (FzMain *)p;
    m->rc = m->fn(m->argc, m->argv);
    return NULL;
}

/* Runs fn(argc, argv), on a thread with a stack of AURA_FUZZ_STACK_KB
 * when that is set. */
static inline int fz_main(int (*fn)(int, char **), int argc, char **argv)
{
    const char *e = getenv("AURA_FUZZ_STACK_KB");
    FzMain m;
    pthread_attr_t a;
    pthread_t t;
    if (!e || atol(e) < 64)
        return fn(argc, argv);
    m.fn = fn;
    m.argc = argc;
    m.argv = argv;
    m.rc = 1;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (size_t)atol(e) * 1024);
    if (pthread_create(&t, &a, fz_thread, &m))
        return fn(argc, argv);
    pthread_join(t, NULL);
    return m.rc;
}

#endif
