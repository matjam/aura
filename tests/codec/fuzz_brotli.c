/* Deterministic fuzzer for the brotli decoder. Mutates the committed
 * vectors (bit flips, byte changes, inserts, deletes, splices,
 * truncations) and random bytes, and decodes each input twice: whole, and
 * with random input and output chunking. The decoder must not crash
 * (build with -fsanitize=address,undefined to catch memory errors), must
 * not stall, and both decodes must agree.
 *
 * Usage: fuzz_brotli [ITERATIONS [SEED]] VECTOR.br... */
#include "../fuzz.h"
#include "brotli_util.h"

#define OUT_CAP (8u << 20)

typedef struct {
    uint8_t *p;
    size_t len;
} Buf;

static void buf_insert(Buf *b, size_t at, const uint8_t *src, size_t n)
{
    b->p = realloc(b->p, b->len + n + 1);
    memmove(b->p + at + n, b->p + at, b->len - at);
    memcpy(b->p + at, src, n);
    b->len += n;
}

static void buf_delete(Buf *b, size_t at, size_t n)
{
    memmove(b->p + at, b->p + at + n, b->len - at - n);
    b->len -= n;
}

static void mutate(BrRng *r, Buf *b, const Buf *vecs, int nvec)
{
    int k, n = 1 + (int)(br_rng_next(r) % 8);
    for (k = 0; k < n; k++) {
        uint32_t op = br_rng_next(r) % 10;
        size_t at = b->len ? br_rng_next(r) % b->len : 0;
        switch (op) {
        case 0:
        case 1:
        case 2:
            if (b->len) {
                size_t bit = br_rng_next(r) % (b->len * 8);
                b->p[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            }
            break;
        case 3:
            if (b->len)
                b->p[at] = (uint8_t)br_rng_next(r);
            break;
        case 4:
            if (b->len) {
                static const uint8_t special[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE};
                b->p[at] = special[br_rng_next(r) % sizeof special];
            }
            break;
        case 5: {
            uint8_t ins[16];
            size_t m = 1 + br_rng_next(r) % 16, i;
            for (i = 0; i < m; i++)
                ins[i] = (uint8_t)br_rng_next(r);
            buf_insert(b, at, ins, m);
            break;
        }
        case 6:
            if (b->len) {
                size_t m = 1 + br_rng_next(r) % 32;
                if (m > b->len - at)
                    m = b->len - at;
                buf_delete(b, at, m);
            }
            break;
        case 7:
            /* Truncate. */
            b->len = at;
            break;
        case 8: {
            /* Splice: the tail of another vector replaces ours. */
            const Buf *o = &vecs[br_rng_next(r) % nvec];
            if (o->len) {
                size_t from = br_rng_next(r) % o->len;
                b->len = at;
                buf_insert(b, at, o->p + from, o->len - from);
            }
            break;
        }
        default:
            /* Duplicate a chunk of ourselves. */
            if (b->len) {
                size_t m = 1 + br_rng_next(r) % 64;
                uint8_t tmp[64];
                if (m > b->len - at)
                    m = b->len - at;
                memcpy(tmp, b->p + at, m);
                buf_insert(b, br_rng_next(r) % (b->len + 1), tmp, m);
            }
            break;
        }
    }
}

/* Saves a failing input as fuzz_brotli_fail.br in the current folder. */
static int failed(const Buf *b, unsigned long it, uint32_t seed, int maxw, bool dict, const char *what)
{
    FILE *f = fopen("fuzz_brotli_fail.br", "wb");
    if (f) {
        fwrite(b->p, 1, b->len, f);
        fclose(f);
    }
    fprintf(stderr, "FAIL: iteration %lu (seed %u, max window %d, %s): %s; input in fuzz_brotli_fail.br\n", it, seed,
            maxw, dict ? "dictionary" : "no dictionary", what);
    return 1;
}

int main(int argc, char **argv)
{
    unsigned long iters = 2000, it;
    uint32_t seed = 1;
    int argi = 1, nvec = 0, i;
    Buf *vecs;
    BrRng r;
    AuBrotliDict *dict = au_brotli_dict_new();
    unsigned long done = 0, errors = 0;

    if (argi < argc && argv[argi][0] >= '0' && argv[argi][0] <= '9')
        iters = strtoul(argv[argi++], NULL, 10);
    if (argi < argc && argv[argi][0] >= '0' && argv[argi][0] <= '9')
        seed = (uint32_t)strtoul(argv[argi++], NULL, 10);
    if (!dict) {
        fprintf(stderr, "FAIL: au_brotli_dict_new\n");
        return 1;
    }
    vecs = calloc((size_t)(argc + 1), sizeof *vecs);
    for (i = argi; i < argc; i++) {
        vecs[nvec].p = br_read_file(argv[i], &vecs[nvec].len);
        if (!vecs[nvec].p) {
            fprintf(stderr, "FAIL: cannot read %s\n", argv[i]);
            return 1;
        }
        nvec++;
    }
    if (!nvec) {
        vecs[0].p = malloc(1);
        vecs[0].p[0] = 0x06;
        vecs[0].len = 1;
        nvec = 1;
    }
    r.s = seed * 2654435761u + 1;

    for (it = 0; it < iters; it++) {
        if (fz_capped()) { /* AURA_FUZZ_SECONDS */
            iters = (unsigned long)it;
            break;
        }
        Buf b;
        BrRng in_rng, out_rng;
        BrResult whole, chunked;
        static const int windows[] = {0, 10, 16, 22, 24};
        int maxw = windows[br_rng_next(&r) % 5];
        const AuBrotliDict *d = (br_rng_next(&r) % 8) ? dict : NULL;
        uint32_t kind = br_rng_next(&r) % 20;

        if (kind == 0) {
            /* Random bytes. */
            size_t n = br_rng_next(&r) % 600, j;
            b.p = malloc(n + 1);
            b.len = n;
            for (j = 0; j < n; j++)
                b.p[j] = (uint8_t)br_rng_next(&r);
        } else {
            const Buf *v = &vecs[br_rng_next(&r) % nvec];
            b.p = malloc(v->len + 1);
            memcpy(b.p, v->p, v->len);
            b.len = v->len;
            if (kind != 1)
                mutate(&r, &b, vecs, nvec);
        }

        in_rng.s = br_rng_next(&r);
        out_rng.s = br_rng_next(&r);
        whole = br_decode(d, maxw, b.p, b.len, OUT_CAP, NULL, NULL);
        chunked = br_decode(d, maxw, b.p, b.len, OUT_CAP, &in_rng, &out_rng);
        if (whole.stalled || chunked.stalled)
            return failed(&b, it, seed, maxw, d != NULL, "stalled");
        /* Output decoded but not yet delivered when an error is found is
         * dropped, so how much comes out before an ERROR depends on the
         * output space offered: compare what both have. */
        if (memcmp(chunked.out, whole.out, chunked.out_len < whole.out_len ? chunked.out_len : whole.out_len) != 0 ||
            (whole.st != AU_CODEC_ERROR && chunked.st != AU_CODEC_ERROR && chunked.out_len > whole.out_len)) {
            fprintf(stderr, "whole: st %d out %zu; chunked: st %d out %zu\n", (int)whole.st, whole.out_len,
                    (int)chunked.st, chunked.out_len);
            return failed(&b, it, seed, maxw, d != NULL, "chunked output differs");
        }
        if (whole.out_len < OUT_CAP && chunked.out_len < OUT_CAP) {
            if (whole.st != chunked.st || whole.in_left != chunked.in_left ||
                (whole.st != AU_CODEC_ERROR && whole.out_len != chunked.out_len)) {
                fprintf(stderr, "whole: st %d left %zu out %zu; chunked: st %d left %zu out %zu\n", (int)whole.st,
                        whole.in_left, whole.out_len, (int)chunked.st, chunked.in_left, chunked.out_len);
                return failed(&b, it, seed, maxw, d != NULL, "decodes disagree");
            }
        }
        done += whole.st == AU_CODEC_DONE;
        errors += whole.st == AU_CODEC_ERROR;
        free(whole.out);
        free(chunked.out);
        free(b.p);
        if (iters >= 100000 && (it + 1) % 100000 == 0) {
            printf("%lu iterations\n", it + 1);
            fflush(stdout);
        }
    }
    printf("fuzz_brotli: %lu iterations, seed %u: %lu done, %lu errors, %lu truncated\n", iters, seed, done, errors,
           iters - done - errors);
    for (i = 0; i < nvec; i++)
        free(vecs[i].p);
    free(vecs);
    au_brotli_dict_free(dict);
    return 0;
}
