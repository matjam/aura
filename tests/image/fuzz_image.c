/* Deterministic fuzzer over every image vector: mutates them (bit flips,
 * byte changes, 0xFF and marker bytes, inserts, deletes, splices between
 * files, truncation, length fields set to extremes), decodes each with
 * random options (target sizes, small canvas caps, header-only), through
 * every frame, whole and in random chunks, and checks that chunked
 * decoding gives the same result as whole-buffer decoding (status,
 * frame count, pixels of the first frame). Under ASan/UBSan this finds
 * memory errors; on its own it checks termination and determinism.
 *
 * usage: fuzz_image ITERATIONS SEED VECTOR... */
#include "../fuzz.h"
#include "imgtest.h"

typedef struct Result {
    AuImageStatus st;
    int frames;
    uint32_t hash; /* of the first frame's canvas */
    bool ready;
    int w, h;
} Result;

static uint32_t hash_surface(const AuSurface *s)
{
    uint32_t h = 2166136261u;
    if (!s)
        return 0;
    for (int y = 0; y < s->height; y++)
        for (int x = 0; x < s->width; x++) {
            h ^= s->pixels[(size_t)y * s->stride + x];
            h *= 16777619u;
        }
    return h;
}

static Result decode(const uint8_t *data, size_t len, const AuImageOptions *o, size_t maxchunk, ItRng *rng)
{
    Result r;
    memset(&r, 0, sizeof r);
    AuImageDecoder *d = au_image_decoder_new(o);
    size_t pos = 0;
    AuImageStatus st = AU_IMAGE_MORE;
    for (int guard = 0; guard < 200000; guard++) {
        size_t c = maxchunk ? 1 + it_rng(rng) % maxchunk : len - pos;
        if (c > len - pos)
            c = len - pos;
        const uint8_t *p = data + pos;
        size_t n = c;
        st = au_image_decode(d, &p, &n, pos + c == len);
        pos += c - n;
        if (st == AU_IMAGE_MORE && n) {
            it_write_file("fuzz-stall.bin", data, len);
            printf("fuzz_image: MORE without consuming input (%s); input in fuzz-stall.bin\n",
                   au_image_format_name(au_image_info(d)->format));
            abort();
        }
        if (st == AU_IMAGE_FRAME) {
            if (r.frames++ == 0)
                r.hash = hash_surface(au_image_canvas(d));
            if (r.frames >= 64)
                break; /* enough frames */
            continue;
        }
        if (st != AU_IMAGE_MORE)
            break;
    }
    AuIRect dr;
    while (au_image_take_dirty(d, &dr)) {
        const AuSurface *s = au_image_canvas(d);
        if (s && (dr.x0 < 0 || dr.y0 < 0 || dr.x1 > s->width || dr.y1 > s->height || dr.x0 >= dr.x1)) {
            printf("fuzz_image: dirty rectangle outside the canvas\n");
            abort();
        }
    }
    const AuImageInfo *info = au_image_info(d);
    const AuSurface *cv = au_image_canvas(d);
    size_t capb = o->max_bytes ? (o->max_bytes < 64 ? 64 : o->max_bytes) : AU_IMAGE_DEFAULT_MAX_BYTES;
    if ((cv && (size_t)cv->width * cv->height * 4 > capb) ||
        (info->ready && (size_t)info->out_w * info->out_h * 4 > capb)) {
        it_write_file("fuzz-cap.bin", data, len);
        printf("fuzz_image: canvas over the cap (%s, %dx%d, cap %zu); input in fuzz-cap.bin\n",
               au_image_format_name(info->format), info->out_w, info->out_h, capb);
        abort();
    }
    r.st = r.frames >= 64 ? AU_IMAGE_DONE : st;
    r.ready = info->ready;
    r.w = info->out_w;
    r.h = info->out_h;
    au_image_decoder_free(d);
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: fuzz_image ITERATIONS SEED VECTOR...\n");
        return 2;
    }
    long iters = atol(argv[1]);
    ItRng rng = {(uint32_t)atol(argv[2]) * 2654435761u + 7};
    int nv = 0;
    uint8_t **vec = calloc((size_t)argc, sizeof *vec);
    size_t *vlen = calloc((size_t)argc, sizeof *vlen);
    for (int i = 3; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (l > 4 && (!strcmp(argv[i] + l - 4, ".pam") || !strcmp(argv[i] + l - 4, ".txt") ||
                      !strcmp(argv[i] + l - 4, ".ppm") || !strcmp(argv[i] + l - 4, ".pgm") ||
                      !strcmp(argv[i] + l - 4, ".ref")))
            continue;
        vec[nv] = it_read_file(argv[i], &vlen[nv]);
        if (vec[nv] && vlen[nv] && vlen[nv] < (1 << 20))
            nv++;
    }
    if (!nv) {
        fprintf(stderr, "fuzz_image: no vectors\n");
        return 2;
    }
    size_t cap = 1 << 21;
    uint8_t *buf = malloc(cap);
    long mismatches = 0;
    for (long it = 0; it < iters; it++) {
        if (fz_capped()) { /* AURA_FUZZ_SECONDS */
            iters = it;
            break;
        }
        int vi = (int)(it_rng(&rng) % (uint32_t)nv);
        size_t n = vlen[vi];
        memcpy(buf, vec[vi], n);
        int muts = (int)(it_rng(&rng) % 7);
        for (int m = 0; m < muts && n; m++) {
            uint32_t k = it_rng(&rng);
            size_t at = it_rng(&rng) % n;
            switch (k % 9) {
            case 0: buf[at] ^= (uint8_t)(1u << (k >> 8 & 7)); break;
            case 1: buf[at] = (uint8_t)(k >> 8); break;
            case 2: buf[at] = 0xFF; break;
            case 3: /* a 32-bit field to an extreme */
                if (at + 4 <= n) {
                    uint32_t v = (k >> 8) & 1 ? 0xFFFFFFFFu : (k >> 9) & 1 ? 0x7FFFFFFFu : 0x00010000u;
                    memcpy(buf + at, &v, 4);
                }
                break;
            case 4: /* insert */
                if (n + 16 < cap) {
                    size_t c = 1 + (k >> 8) % 16;
                    memmove(buf + at + c, buf + at, n - at);
                    for (size_t j = 0; j < c; j++)
                        buf[at + j] = (uint8_t)it_rng(&rng);
                    n += c;
                }
                break;
            case 5: { /* delete */
                size_t c = 1 + (k >> 8) % 64;
                if (at + c > n)
                    c = n - at;
                memmove(buf + at, buf + at + c, n - at - c);
                n -= c;
                break;
            }
            case 6: { /* splice from another vector */
                int vj = (int)(it_rng(&rng) % (uint32_t)nv);
                size_t from = it_rng(&rng) % vlen[vj], c = 1 + (k >> 8) % 256;
                if (from + c > vlen[vj])
                    c = vlen[vj] - from;
                if (at + c > n)
                    c = n - at;
                memcpy(buf + at, vec[vj] + from, c);
                break;
            }
            case 7: /* truncate */
                n = at;
                break;
            default: /* a run of a byte */
                for (size_t j = at; j < n && j < at + 1 + (k >> 8) % 32; j++)
                    buf[j] = (uint8_t)(k >> 16);
                break;
            }
        }
        AuImageOptions o;
        memset(&o, 0, sizeof o);
        uint32_t r = it_rng(&rng);
        if (r & 1) {
            o.target_w = 1 + (int)(it_rng(&rng) % 64);
            o.target_h = 1 + (int)(it_rng(&rng) % 64);
        }
        if (r & 2)
            o.max_bytes = 64 + it_rng(&rng) % 40000;
        if ((r & 28) == 4)
            o.header_only = true;
        Result a = decode(buf, n, &o, 0, &rng);
        Result b = decode(buf, n, &o, 1 + it_rng(&rng) % ((r & 32) ? 3 : 700), &rng);
        bool same = a.st == b.st && a.frames == b.frames && a.ready == b.ready && a.w == b.w && a.h == b.h &&
                    (a.frames == 0 || a.hash == b.hash);
        if (!same) {
            mismatches++;
            if (mismatches <= 5) {
                char path[64];
                snprintf(path, sizeof path, "fuzz-mismatch-%ld.bin", it);
                it_write_file(path, buf, n);
                printf("fuzz_image: chunked decoding differs at iteration %ld (st %d/%d frames %d/%d hash %08x/%08x); "
                       "input in %s\n",
                       it, a.st, b.st, a.frames, b.frames, a.hash, b.hash, path);
            }
        }
    }
    printf("fuzz_image: %ld iterations over %d vectors, %ld chunking mismatches\n", iters, nv, mismatches);
    for (int i = 0; i < nv; i++)
        free(vec[i]);
    free(vec);
    free(vlen);
    free(buf);
    return mismatches ? 1 : 0;
}
