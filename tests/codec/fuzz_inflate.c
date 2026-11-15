/* Deterministic inflate fuzzer: mutates the vectors (bit flips, byte
 * changes, inserts, deletes, splices, truncation) and random bytes, and
 * decodes each in every format with random chunking and an output cap.
 * Under ASan/UBSan this finds memory errors; on its own it checks that the
 * decoder terminates and never writes past the output it was given.
 *
 * usage: fuzz_inflate ITERATIONS SEED VECTOR... */
#include "brotli_util.h"
#include "codec/codec.h"

static void decode_all(AuInflateFormat fmt, const uint8_t *in, size_t n, BrRng *rng)
{
    AuInflate *z = au_inflate_new(fmt);
    uint8_t out[4096 + 16];
    size_t pos = 0, total = 0;
    int stalls = 0;
    for (;;) {
        size_t ic = br_rng_chunk(rng, 4096), oc = 1 + br_rng_next(rng) % 4096;
        if (ic > n - pos)
            ic = n - pos;
        memset(out + oc, 0xA5, 16);
        const uint8_t *ip = in + pos;
        size_t il = ic;
        uint8_t *op = out;
        size_t ol = oc;
        AuCodecStatus st = au_inflate_run(z, &ip, &il, &op, &ol);
        for (int k = 0; k < 16; k++)
            if (out[oc + k] != 0xA5) {
                printf("fuzz_inflate: wrote past the output\n");
                abort();
            }
        if (ip != in + pos + (ic - il) || op != out + (oc - ol)) {
            printf("fuzz_inflate: cursor mismatch\n");
            abort();
        }
        pos += ic - il;
        total += oc - ol;
        if (st != AU_CODEC_OK || total > (8u << 20))
            break;
        if (ic == il && oc == ol) {
            if (pos == n || ++stalls > 4)
                break;
        } else {
            stalls = 0;
        }
    }
    au_inflate_free(z);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: fuzz_inflate ITERATIONS SEED VECTOR...\n");
        return 2;
    }
    long iters = atol(argv[1]);
    BrRng rng = {(uint32_t)atol(argv[2]) * 2654435761u + 7};
    int nv = argc - 3;
    uint8_t **vec = calloc((size_t)(nv ? nv : 1), sizeof *vec);
    size_t *vlen = calloc((size_t)(nv ? nv : 1), sizeof *vlen);
    for (int i = 0; i < nv; i++)
        vec[i] = br_read_file(argv[3 + i], &vlen[i]);
    uint8_t *buf = malloc(1 << 17);
    static const AuInflateFormat fmts[] = {AU_INFLATE_RAW, AU_INFLATE_ZLIB, AU_INFLATE_GZIP, AU_INFLATE_HTTP_DEFLATE};
    for (long it = 0; it < iters; it++) {
        size_t n;
        uint32_t r = br_rng_next(&rng);
        if (nv && (r & 7) != 0) {
            int vi = (int)(br_rng_next(&rng) % (uint32_t)nv);
            if (!vec[vi])
                continue;
            n = vlen[vi] > (1 << 16) ? (1 << 16) : vlen[vi];
            memcpy(buf, vec[vi], n);
            int muts = 1 + (int)(br_rng_next(&rng) % 6);
            for (int m = 0; m < muts && n; m++) {
                uint32_t k = br_rng_next(&rng);
                size_t at = br_rng_next(&rng) % n;
                switch (k % 6) {
                case 0: buf[at] ^= (uint8_t)(1u << (k >> 8 & 7)); break;
                case 1: buf[at] = (uint8_t)(k >> 8); break;
                case 2: /* insert */
                    if (n < (1 << 17) - 1) {
                        memmove(buf + at + 1, buf + at, n - at);
                        buf[at] = (uint8_t)(k >> 16);
                        n++;
                    }
                    break;
                case 3: /* delete */
                    memmove(buf + at, buf + at + 1, n - at - 1);
                    n--;
                    break;
                case 4: n = at; break; /* truncate */
                default: { /* splice from another vector */
                    int vj = (int)(br_rng_next(&rng) % (uint32_t)nv);
                    if (vec[vj] && vlen[vj]) {
                        size_t from = br_rng_next(&rng) % vlen[vj];
                        size_t len = vlen[vj] - from;
                        if (len > (1 << 17) - at)
                            len = (1 << 17) - at;
                        memcpy(buf + at, vec[vj] + from, len);
                        if (at + len > n)
                            n = at + len;
                    }
                }
                }
            }
        } else {
            n = br_rng_next(&rng) % 2048;
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)br_rng_next(&rng);
        }
        decode_all(fmts[br_rng_next(&rng) & 3], buf, n, &rng);
    }
    printf("fuzz_inflate: %ld iterations, no faults\n", iters);
    for (int i = 0; i < nv; i++)
        free(vec[i]);
    free(vec);
    free(vlen);
    free(buf);
    return 0;
}
