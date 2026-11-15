/* Helpers shared by test_brotli.c and fuzz_brotli.c. */
#ifndef AURA_TEST_BROTLI_UTIL_H
#define AURA_TEST_BROTLI_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/brotli.h"

typedef struct {
    uint32_t s;
} BrRng;

static inline uint32_t br_rng_next(BrRng *r)
{
    uint32_t x = r->s ? r->s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

/* A number in 1..max, small sizes much more often than large ones. */
static inline size_t br_rng_chunk(BrRng *r, size_t max)
{
    uint32_t k = br_rng_next(r);
    size_t n;
    switch (k & 3) {
    case 0: n = 1; break;
    case 1: n = 1 + ((k >> 2) & 15); break;
    case 2: n = 1 + ((k >> 2) & 1023); break;
    default: n = 1 + ((k >> 2) % 70000); break;
    }
    return n > max ? max : n;
}

static inline uint8_t *br_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* The generated input behind gen-words.*.br (see make-vectors.sh). */
static inline uint8_t *br_gen_words(size_t n)
{
    static const char *const words[4] = {"the ", "window ", "of ", "ring\n"};
    uint8_t *out = malloc(n + 16);
    uint32_t x = 1;
    size_t len = 0;
    while (len < n) {
        const char *w;
        size_t wl;
        x = (x * 1103515245u + 12345u) & 0x7FFFFFFFu;
        w = words[(x >> 16) & 3];
        wl = strlen(w);
        memcpy(out + len, w, wl);
        len += wl;
    }
    return out;
}

typedef struct {
    AuCodecStatus st;
    uint8_t *out;
    size_t out_len;
    size_t in_left; /* input bytes not consumed when the decode stopped */
    int stalled;    /* a call with input and output space made no progress */
} BrResult;

/* Decodes in with chunk sizes from rng (whole buffers when rng is NULL),
 * stopping at DONE, ERROR, the end of the input or out_cap bytes. */
static inline BrResult br_decode(const AuBrotliDict *dict, int maxw, const uint8_t *in, size_t in_len,
                                 size_t out_cap, BrRng *in_rng, BrRng *out_rng)
{
    BrResult r = {AU_CODEC_OK, NULL, 0, 0, 0};
    AuBrotli *b = au_brotli_new(dict, maxw);
    size_t pos = 0, buf_cap = out_cap < 65536 ? out_cap : 65536;
    r.out = malloc(buf_cap + 1);
    for (;;) {
        size_t ic = in_len - pos, oc, ic0, oc0;
        const uint8_t *ip;
        uint8_t *op;
        if (r.out_len == buf_cap && buf_cap < out_cap) {
            buf_cap = buf_cap * 2 < out_cap ? buf_cap * 2 : out_cap;
            r.out = realloc(r.out, buf_cap + 1);
        }
        oc = buf_cap - r.out_len;
        if (in_rng && ic)
            ic = br_rng_chunk(in_rng, ic);
        if (out_rng && oc)
            oc = br_rng_chunk(out_rng, oc);
        ic0 = ic;
        oc0 = oc;
        ip = in + pos;
        op = r.out + r.out_len;
        r.st = au_brotli_run(b, &ip, &ic, &op, &oc);
        if (ip != in + pos + (ic0 - ic) || op != r.out + r.out_len + (oc0 - oc) || ic > ic0 || oc > oc0) {
            fprintf(stderr, "pointer and length disagree\n");
            abort();
        }
        pos += ic0 - ic;
        r.out_len += oc0 - oc;
        if (r.st != AU_CODEC_OK)
            break;
        /* OK means the decoder used all the input or filled the output. */
        if (ic && oc) {
            r.stalled = 1;
            break;
        }
        /* No progress: the input ended (truncated) or the cap was hit. */
        if (ic0 == ic && oc0 == oc)
            break;
    }
    r.in_left = in_len - pos;
    au_brotli_free(b);
    return r;
}

#endif
