/* Deterministic deflate round-trip fuzzer: makes inputs of several kinds
 * (random bytes, few symbols, copies from earlier in the input at random
 * distances and lengths, mutated corpus files, long runs), compresses
 * each at a random level and format with random chunking of input and
 * output, checks that the encoder never writes past the output it was
 * given and ends, and that Aura's inflate gives the input back.
 *
 * usage: fuzz_deflate ITERATIONS SEED [FILE...]
 * AURA_FUZZ_SECONDS caps the run; it prints the iterations done. */
#include "../fuzz.h"
#include "brotli_util.h"
#include "codec/codec.h"

#define MAX_IN (300u * 1024)

static size_t make_input(uint8_t *b, BrRng *r, uint8_t **vec, size_t *vlen, int nv)
{
    uint32_t kind = br_rng_next(r) % 6;
    size_t n;
    switch (br_rng_next(r) % 5) {
    case 0: n = br_rng_next(r) % 64; break;
    case 1: n = br_rng_next(r) % 4096; break;
    case 2: n = 60000 + br_rng_next(r) % 12000; break; /* around the window's size */
    case 3: n = br_rng_next(r) % MAX_IN; break;
    default: n = br_rng_next(r) % 40000; break;
    }
    if (kind == 0) {
        for (size_t i = 0; i < n; i++)
            b[i] = (uint8_t)br_rng_next(r);
    } else if (kind == 1) {
        uint32_t syms = 1 + br_rng_next(r) % 8;
        for (size_t i = 0; i < n; i++)
            b[i] = (uint8_t)('a' + br_rng_next(r) % syms);
    } else if (kind == 2 || kind == 3) {
        /* Literal stretches and copies: what a match finder should find. */
        size_t i = 0;
        while (i < n) {
            uint32_t k = br_rng_next(r);
            if (i > 0 && (k & 3) != 0) {
                size_t dist = 1 + (kind == 2 ? br_rng_next(r) % 40000 : br_rng_next(r) % 300);
                size_t len = 1 + ((k >> 2) & 1 ? br_rng_next(r) % 600 : br_rng_next(r) % 12);
                if (dist > i)
                    dist = i;
                for (size_t j = 0; j < len && i < n; j++, i++)
                    b[i] = b[i - dist];
            } else {
                size_t len = 1 + br_rng_next(r) % 40;
                for (size_t j = 0; j < len && i < n; j++, i++)
                    b[i] = (uint8_t)br_rng_next(r);
            }
        }
    } else if (kind == 4 && nv) {
        int vi = (int)(br_rng_next(r) % (uint32_t)nv);
        n = 0;
        /* A corpus file, several times, with a few bytes changed. */
        int reps = 1 + (int)(br_rng_next(r) % 8);
        for (int k = 0; k < reps && vec[vi]; k++) {
            size_t take = vlen[vi] < MAX_IN - n ? vlen[vi] : MAX_IN - n;
            memcpy(b + n, vec[vi], take);
            n += take;
        }
        for (int m = 0; m < 20 && n; m++)
            b[br_rng_next(r) % n] = (uint8_t)br_rng_next(r);
    } else {
        size_t i = 0;
        while (i < n) {
            size_t len = 1 + br_rng_next(r) % 70000;
            uint8_t c = (uint8_t)br_rng_next(r);
            for (size_t j = 0; j < len && i < n; j++, i++)
                b[i] = c;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: fuzz_deflate ITERATIONS SEED [FILE...]\n");
        return 2;
    }
    long iters = atol(argv[1]);
    BrRng rng = {(uint32_t)atol(argv[2]) * 2654435761u + 11};
    int nv = argc - 3;
    uint8_t **vec = calloc((size_t)(nv ? nv : 1), sizeof *vec);
    size_t *vlen = calloc((size_t)(nv ? nv : 1), sizeof *vlen);
    for (int i = 0; i < nv; i++)
        vec[i] = br_read_file(argv[3 + i], &vlen[i]);
    uint8_t *in = malloc(MAX_IN + 1);
    static const AuDeflateFormat dfmt[3] = {AU_DEFLATE_RAW, AU_DEFLATE_ZLIB, AU_DEFLATE_GZIP};
    static const AuInflateFormat ifmt[3] = {AU_INFLATE_RAW, AU_INFLATE_ZLIB, AU_INFLATE_GZIP};
    AuDeflate *kept[9] = {0}; /* encoders reused through au_deflate_reset */
    long done = 0;
    for (long it = 0; it < iters; it++) {
        if (fz_capped())
            break;
        done = it + 1;
        size_t n = make_input(in, &rng, vec, vlen, nv);
        int f = (int)(br_rng_next(&rng) % 3), level = (int)(br_rng_next(&rng) % 3);
        AuDeflate *z;
        bool reuse = (br_rng_next(&rng) & 1) != 0;
        if (reuse) {
            AuDeflate **slot = &kept[f * 3 + level];
            if (!*slot)
                *slot = au_deflate_new(dfmt[f], level);
            else
                au_deflate_reset(*slot);
            z = *slot;
        } else {
            z = au_deflate_new(dfmt[f], level);
        }
        AuBuf out;
        au_buf_init(&out);
        size_t pos = 0;
        uint8_t buf[8192 + 16];
        bool tiny = (br_rng_next(&rng) & 7) == 0 && n < 20000;
        long calls = 0;
        for (;;) {
            size_t ic = tiny ? 1 + br_rng_next(&rng) % 3 : br_rng_chunk(&rng, 70000);
            size_t oc = tiny ? 1 + br_rng_next(&rng) % 3 : 1 + br_rng_next(&rng) % 8192;
            if ((br_rng_next(&rng) & 15) == 0)
                ic = 0; /* a call with no input */
            if (ic > n - pos)
                ic = n - pos;
            bool finish = pos + ic == n && (ic || (br_rng_next(&rng) & 1));
            if (pos == n)
                finish = true;
            memset(buf + oc, 0x5A, 16);
            const uint8_t *ip = in + pos;
            size_t il = ic;
            uint8_t *op = buf;
            size_t ol = oc;
            AuCodecStatus st = au_deflate_run(z, &ip, &il, &op, &ol, finish);
            for (int k = 0; k < 16; k++)
                if (buf[oc + k] != 0x5A) {
                    printf("fuzz_deflate: wrote past the output (iteration %ld)\n", it);
                    abort();
                }
            if (ip != in + pos + (ic - il) || op != buf + (oc - ol) || il > ic || ol > oc) {
                printf("fuzz_deflate: cursor mismatch (iteration %ld)\n", it);
                abort();
            }
            pos += ic - il;
            au_buf_append(&out, buf, oc - ol);
            if (st == AU_CODEC_DONE)
                break;
            if (st != AU_CODEC_OK || ++calls > 4000000) {
                printf("fuzz_deflate: the encoder did not finish (iteration %ld, status %d)\n", it, (int)st);
                abort();
            }
        }
        if (pos != n) {
            printf("fuzz_deflate: input left over (iteration %ld)\n", it);
            abort();
        }
        if (out.len > n + 5 * (n / 16000 + 1) + 20) {
            printf("fuzz_deflate: %lu bytes from %lu (iteration %ld)\n", (unsigned long)out.len, (unsigned long)n, it);
            abort();
        }
        uint8_t *back = NULL;
        size_t blen = 0;
        if (!au_inflate_all(ifmt[f], out.data, out.len, n + 16, &back, &blen) || blen != n ||
            (n && memcmp(back, in, n))) {
            printf("fuzz_deflate: round trip failed (iteration %ld, seed %s, %lu bytes, level %d, format %d)\n", it,
                   argv[2], (unsigned long)n, level, f);
            abort();
        }
        au_free(back);
        au_buf_free(&out);
        if (!reuse)
            au_deflate_free(z);
    }
    for (int i = 0; i < 9; i++)
        au_deflate_free(kept[i]);
    for (int i = 0; i < nv; i++)
        free(vec[i]);
    free(vec);
    free(vlen);
    free(in);
    printf("fuzz_deflate: %ld iterations\n", done);
    return 0;
}
