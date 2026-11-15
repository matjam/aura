/* JPEG decoder tests: the vectors in vectors/jpeg (made by
 * gen-jpeg-vectors.py) against djpeg -nosmooth references, at full size,
 * scaled and with every EXIF orientation; chunked feeding, truncation,
 * corrupt and hostile files, header probing and row streaming.
 *
 * usage: test_jpeg VECTOR_DIR             run the tests
 *        test_jpeg --fuzz N SEED DIR      mutate the vectors N times
 *        test_jpeg --bench FILE [REPS]    decode speed and memory */
#include "imgtest.h"

#include <time.h>

static const char *vdir;

static uint8_t *load(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", vdir, name);
    uint8_t *b = it_read_file(path, len);
    IT_CHECK(b != NULL, "cannot read %s", path);
    return b;
}

static uint8_t *load_ref(const char *name, int *w, int *h)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", vdir, name);
    uint8_t *r = it_load_pnm(path, w, h);
    IT_CHECK(r != NULL, "cannot read %s", path);
    return r;
}

static bool same_pixels(const AuSurface *a, const AuSurface *b)
{
    if (!a || !b || a->width != b->width || a->height != b->height)
        return false;
    for (int y = 0; y < a->height; y++)
        if (memcmp(a->pixels + (size_t)y * a->stride, b->pixels + (size_t)y * b->stride, (size_t)a->width * 4))
            return false;
    return true;
}

typedef struct Vec {
    const char *name;
    bool scaled; /* has .s2, .s4 and .s8 references */
} Vec;

static const Vec vecs[] = {
    {"base444", true},   {"base422", false},  {"base420", true},   {"base411", false},  {"base440", false},
    {"base_odd", false}, {"grey", true},      {"prog420", true},   {"prog444", true},   {"prog422", false},
    {"prog411", false},  {"prog_grey", true}, {"rst1", false},     {"rst2b", false},    {"prog_rst", false},
    {"multiscan", true}, {"opt", false},     {"q5", false},       {"q100", false},     {"s1x1", false},
    {"s7x5", true},      {"s33x17", true},    {"s100x75", true},   {"p33x17", true},    {"ycck", false},
    {"cmyk", false},
};

/* Tolerances against djpeg -nosmooth. At full size the decode is the
 * same as libjpeg's (islow IDCT, replicated chroma, the same colour
 * conversion): the vectors match exactly; 1 allows for rounding. Scaled,
 * the reduced IDCTs here compute exact averages of the full-size samples
 * and libjpeg's nearly so (subsampled chroma gets a larger IDCT in both):
 * up to 3 apart, mean under 0.3 (0.25 on the 7x5 image). */
#define FULL_TOL 1
#define FULL_MEAN 0.02
#define SCALED_TOL 4
#define SCALED_MEAN 0.3

static void test_vector(const Vec *v)
{
    char name[128];
    size_t len;
    snprintf(name, sizeof name, "%s.jpg", v->name);
    uint8_t *data = load(name, &len);
    if (!data)
        return;
    int w, h;
    snprintf(name, sizeof name, "%s.ppm", v->name);
    uint8_t *ref = load_ref(name, &w, &h);
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(data, len, NULL, &info);
    IT_CHECK(s != NULL, "%s: decode failed", v->name);
    IT_CHECK(info.format == AU_IMAGE_FORMAT_JPEG && !info.partial && info.frame_count == 1, "%s: info", v->name);
    IT_CHECK(info.width == w && info.height == h, "%s: size %dx%d", v->name, info.width, info.height);
    if (ref && s)
        it_compare(s, ref, w, h, FULL_TOL, FULL_MEAN, v->name);
    IT_CHECK(!s || s->opaque, "%s: canvas not opaque", v->name);

    /* One byte at a time and random chunks give the same pixels. */
    AuSurface *s1 = it_decode_chunked(data, len, NULL, 1, 1, NULL);
    IT_CHECK(same_pixels(s, s1), "%s: byte-at-a-time decode differs", v->name);
    au_surface_free(s1);
    for (uint32_t seed = 1; seed <= 4; seed++) {
        AuSurface *s2 = it_decode_chunked(data, len, NULL, 1 + seed * 97, seed, NULL);
        IT_CHECK(same_pixels(s, s2), "%s: chunked decode (seed %u) differs", v->name, seed);
        au_surface_free(s2);
    }
    au_surface_free(s);
    free(ref);

    if (v->scaled) {
        for (int k = 1; k <= 3; k++) {
            int sc = 1 << k;
            snprintf(name, sizeof name, "%s.s%d.ppm", v->name, sc);
            int rw, rh;
            uint8_t *sref = load_ref(name, &rw, &rh);
            AuImageOptions o = {0};
            o.target_w = (w + sc - 1) / sc;
            o.target_h = (h + sc - 1) / sc;
            AuSurface *ss = au_image_decode_buffer(data, len, &o, &info);
            snprintf(name, sizeof name, "%s 1/%d", v->name, sc);
            IT_CHECK(ss != NULL, "%s: decode failed", name);
            if (ss && sref) {
                /* A 1-pixel image cannot be reduced (the canvas keeps at
                 * least one pixel); compare sizes the target allows. */
                it_compare(ss, sref, rw, rh, SCALED_TOL, SCALED_MEAN, name);
                IT_CHECK(info.scale_denom == sc, "%s: scale_denom %d", name, info.scale_denom);
            }
            au_surface_free(ss);
            free(sref);
        }
    }

    it_truncations(data, len, len > 3000 ? 3 : 1);
    free(data);
}

static void test_orientation(void)
{
    for (int o = 1; o <= 8; o++) {
        char name[64];
        size_t len;
        snprintf(name, sizeof name, "orient%d.jpg", o);
        uint8_t *data = load(name, &len);
        snprintf(name, sizeof name, "orient%d.ppm", o);
        int w, h;
        uint8_t *ref = load_ref(name, &w, &h);
        AuImageInfo info;
        AuSurface *s = data ? au_image_decode_buffer(data, len, NULL, &info) : NULL;
        IT_CHECK(s && info.orientation == o, "orient%d: orientation %d", o, s ? info.orientation : -1);
        if (s && ref) {
            snprintf(name, sizeof name, "orient%d", o);
            it_compare(s, ref, w, h, FULL_TOL, FULL_MEAN, name);
        }
        au_surface_free(s);
        /* Scaled and oriented together: the canvas is the reduced,
         * rotated size. */
        AuImageOptions opt = {0};
        opt.target_w = (w + 1) / 2;
        opt.target_h = (h + 1) / 2;
        s = data ? au_image_decode_buffer(data, len, &opt, &info) : NULL;
        IT_CHECK(s && s->width == (w + 1) / 2 && s->height == (h + 1) / 2, "orient%d 1/2: size", o);
        au_surface_free(s);
        free(ref);
        free(data);
    }
}

static void test_unsupported(void)
{
    const char *names[] = {"arith.jpg", "p12.jpg", "lossless.jpg"};
    for (int i = 0; i < 3; i++) {
        size_t len;
        uint8_t *data = load(names[i], &len);
        if (!data)
            continue;
        AuImageDecoder *d = au_image_decoder_new(NULL);
        const uint8_t *p = data;
        size_t n = len;
        AuImageStatus st = au_image_decode(d, &p, &n, true);
        IT_CHECK(st == AU_IMAGE_ERROR && strstr(au_image_error(d), "unsupported"), "%s: %d '%s'", names[i], st,
                 au_image_error(d));
        au_image_decoder_free(d);
        free(data);
    }
}

/* Offset just after the segment with marker m (0 if none). */
static size_t segment_end(const uint8_t *d, size_t len, int m)
{
    size_t i = 2;
    while (i + 4 <= len && d[i] == 0xFF) {
        int mk = d[i + 1];
        size_t l = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (mk == m)
            return i + 2 + l;
        if (mk == 0xDA)
            return 0;
        i += 2 + l;
    }
    return 0;
}

static void test_probe(void)
{
    size_t len;
    uint8_t *data = load("s100x75.jpg", &len);
    if (!data)
        return;
    size_t e = segment_end(data, len, 0xC0);
    IT_CHECK(e > 0, "no SOF0");
    AuImageInfo info;
    IT_CHECK(au_image_probe(data, e, &info) && info.width == 100 && info.height == 75, "probe at SOF end");
    IT_CHECK(!au_image_probe(data, e - 1, &info), "probe before SOF end");

    /* While feeding byte by byte, ready turns true with the last SOF byte. */
    AuImageDecoder *d = au_image_decoder_new(NULL);
    size_t ready_at = 0;
    for (size_t i = 0; i < len; i++) {
        const uint8_t *p = data + i;
        size_t n = 1;
        AuImageStatus st = au_image_decode(d, &p, &n, i + 1 == len);
        if (!ready_at && au_image_info(d)->ready)
            ready_at = i + 1;
        if (st != AU_IMAGE_MORE)
            break;
    }
    IT_CHECK(ready_at == e, "ready after %zu bytes, SOF ends at %zu", ready_at, e);
    au_image_decoder_free(d);
    free(data);
}

/* Baseline rows appear while the data arrives; a truncated file keeps
 * them. */
static void test_streaming(void)
{
    size_t len;
    uint8_t *data = load("s100x75.jpg", &len);
    if (!data)
        return;
    AuImageDecoder *d = au_image_decoder_new(NULL);
    const uint8_t *p = data;
    size_t n = len * 6 / 10;
    AuImageStatus st = au_image_decode(d, &p, &n, false);
    AuIRect r;
    bool dirty = au_image_take_dirty(d, &r);
    IT_CHECK(st == AU_IMAGE_MORE && dirty && r.y0 == 0 && r.y1 >= 16 && r.y1 < 75, "rows while streaming: %d %d",
             dirty ? r.y0 : -1, dirty ? r.y1 : -1);
    au_image_decoder_free(d);

    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(data, len * 6 / 10, NULL, &info);
    IT_CHECK(s && info.partial, "truncated baseline keeps its rows");
    au_surface_free(s);
    free(data);

    /* Progressive: a truncated file shows what its scans gave. */
    data = load("prog420.jpg", &len);
    if (!data)
        return;
    s = au_image_decode_buffer(data, len * 7 / 10, NULL, &info);
    IT_CHECK(s && info.partial && s->width == 48, "truncated progressive gives a partial image");
    au_surface_free(s);
    free(data);
}

/* Progressive under a small memory cap: the coefficient store (3 x
 * max_bytes) cannot hold whole blocks, so only the low frequencies the
 * reduced IDCT reads are kept. Dropping the folded high frequencies
 * leaves the result near the average, not on it: on these noisy patterns
 * up to 52 apart with a mean of 4. */
static void test_lowmem(void)
{
    const char *names[] = {"prog420", "prog444", "p33x17", "prog_grey"};
    for (int i = 0; i < 4; i++) {
        char name[128];
        size_t len;
        snprintf(name, sizeof name, "%s.jpg", names[i]);
        uint8_t *data = load(name, &len);
        if (!data)
            continue;
        AuImageOptions o = {0};
        o.max_bytes = 1024;
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(data, len, &o, &info);
        /* About 40 KB of that is fixed: tables and the entropy buffer. */
        IT_CHECK(s && info.peak_bytes < 48000, "%s low memory: peak %zu", names[i], info.peak_bytes);
        if (s) {
            snprintf(name, sizeof name, "%s.s%d.ppm", names[i], info.scale_denom);
            int w, h;
            uint8_t *ref = load_ref(name, &w, &h);
            snprintf(name, sizeof name, "%s low memory 1/%d", names[i], info.scale_denom);
            if (ref)
                it_compare(s, ref, w, h, 56, 4.5, name);
            free(ref);
        }
        au_surface_free(s);
        free(data);
    }
}

/* A minimal JPEG header (SOI, DQT, SOF, DHT, SOS) for w x h with the
 * given SOF marker, followed by entropy data of zeros and EOI. */
static size_t make_header(uint8_t *b, int sof, int w, int h, int ncomp, size_t zeros)
{
    size_t i = 0;
    b[i++] = 0xFF;
    b[i++] = 0xD8;
    b[i++] = 0xFF;
    b[i++] = 0xDB;
    b[i++] = 0;
    b[i++] = 67;
    b[i++] = 0;
    for (int k = 0; k < 64; k++)
        b[i++] = 1;
    b[i++] = 0xFF;
    b[i++] = (uint8_t)sof;
    b[i++] = 0;
    b[i++] = (uint8_t)(8 + 3 * ncomp);
    b[i++] = 8;
    b[i++] = (uint8_t)(h >> 8);
    b[i++] = (uint8_t)h;
    b[i++] = (uint8_t)(w >> 8);
    b[i++] = (uint8_t)w;
    b[i++] = (uint8_t)ncomp;
    for (int c = 0; c < ncomp; c++) {
        b[i++] = (uint8_t)(c + 1);
        b[i++] = c ? 0x11 : 0x22;
        b[i++] = 0;
    }
    /* DC and AC table 0: one code of length 1 (symbol 0). */
    for (int t = 0; t < 2; t++) {
        b[i++] = 0xFF;
        b[i++] = 0xC4;
        b[i++] = 0;
        b[i++] = 20;
        b[i++] = (uint8_t)(t << 4);
        b[i++] = 1;
        for (int k = 1; k < 16; k++)
            b[i++] = 0;
        b[i++] = 0;
    }
    b[i++] = 0xFF;
    b[i++] = 0xDA;
    b[i++] = 0;
    b[i++] = (uint8_t)(6 + 2 * ncomp);
    b[i++] = (uint8_t)ncomp;
    for (int c = 0; c < ncomp; c++) {
        b[i++] = (uint8_t)(c + 1);
        b[i++] = 0;
    }
    b[i++] = 0;
    b[i++] = sof == 0xC2 ? 0 : 63;
    b[i++] = 0;
    for (size_t k = 0; k < zeros; k++)
        b[i++] = 0;
    b[i++] = 0xFF;
    b[i++] = 0xD9;
    return i;
}

static void test_hostile(void)
{
    uint8_t b[4096];
    AuImageInfo info;
    clock_t t0 = clock();
    size_t n = make_header(b, 0xC0, 65535, 65535, 3, 16);
    AuSurface *s = au_image_decode_buffer(b, n, NULL, &info);
    IT_CHECK(!s && !info.partial, "65535x65535 must fail");
    n = make_header(b, 0xC0, 30000, 30000, 3, 16);
    s = au_image_decode_buffer(b, n, NULL, &info);
    IT_CHECK(!s, "30000x30000 must fail (too many pixels)");
    n = make_header(b, 0xC2, 30000, 30000, 3, 16);
    s = au_image_decode_buffer(b, n, NULL, &info);
    IT_CHECK(!s, "30000x30000 progressive must fail");
    IT_CHECK((double)(clock() - t0) / CLOCKS_PER_SEC < 1.0, "huge sizes must fail fast");

    /* Large but allowed: decoded scaled, grey where the data is missing,
     * within the canvas and coefficient caps. */
    n = make_header(b, 0xC0, 8000, 6000, 3, 16);
    s = au_image_decode_buffer(b, n, NULL, &info);
    IT_CHECK(s && s->width * s->height * 4 <= (int)AU_IMAGE_DEFAULT_MAX_BYTES, "8000x6000 baseline decodes scaled");
    IT_CHECK(info.peak_bytes < AU_IMAGE_DEFAULT_MAX_BYTES + (2u << 20), "8000x6000 baseline peak %zu",
             info.peak_bytes);
    au_surface_free(s);
    n = make_header(b, 0xC2, 8000, 6000, 3, 16);
    s = au_image_decode_buffer(b, n, NULL, &info);
    IT_CHECK(s && s->width == 2000, "8000x6000 progressive decodes scaled");
    IT_CHECK(info.peak_bytes < AU_IMAGE_DEFAULT_MAX_BYTES + (49u << 20), "8000x6000 progressive peak %zu",
             info.peak_bytes);
    au_surface_free(s);

    /* Bad marker lengths, zero sizes, a scan before the frame. */
    n = make_header(b, 0xC0, 16, 16, 3, 16);
    uint8_t c[4096];
    memcpy(c, b, n);
    c[4] = 0;
    c[5] = 1; /* DQT length 1 */
    s = au_image_decode_buffer(c, n, NULL, &info);
    IT_CHECK(!s, "marker length 1 must fail");
    memcpy(c, b, n);
    c[5] = 0xFF; /* DQT length past the end */
    s = au_image_decode_buffer(c, n, NULL, &info);
    IT_CHECK(!s, "DQT length past the end must fail");
    n = make_header(b, 0xC0, 0, 16, 3, 16);
    s = au_image_decode_buffer(b, n, NULL, &info);
    IT_CHECK(!s, "zero width must fail");
    /* Sampling factor 0 and 5. */
    n = make_header(b, 0xC0, 16, 16, 3, 16);
    memcpy(c, b, n);
    c[2 + 4 + 65 + 4 + 6 + 1] = 0x05;
    s = au_image_decode_buffer(c, n, NULL, &info);
    IT_CHECK(!s, "sampling 0x5 must fail");
    c[2 + 4 + 65 + 4 + 6 + 1] = 0x44; /* 16 + 1 + 1 blocks per MCU */
    s = au_image_decode_buffer(c, n, NULL, &info);
    IT_CHECK(!s, "18 blocks per MCU must fail");
    /* An over-subscribed Huffman table: three codes of length 1. */
    memcpy(c, b, n);
    size_t dht = segment_end(c, n, 0xC0);
    c[dht + 5] = 3;
    s = au_image_decode_buffer(c, n, NULL, &info);
    IT_CHECK(!s, "over-subscribed Huffman table must fail");

    /* The minimal file itself decodes (all zero coefficients: grey). */
    s = au_image_decode_buffer(b, n, NULL, &info);
    IT_CHECK(s && s->width == 16 && (s->pixels[0] & 0xFFFFFF) == 0x808080, "minimal file");
    au_surface_free(s);
}

static void test_corrupt(void)
{
    const char *names[] = {"base420.jpg", "prog420.jpg", "rst2b.jpg", "prog_rst.jpg", "base_odd.jpg", "grey.jpg"};
    ItRng rng = {12345};
    for (int f = 0; f < 6; f++) {
        size_t len;
        uint8_t *data = load(names[f], &len);
        if (!data)
            continue;
        uint8_t *m = malloc(len);
        for (int it = 0; it < 300; it++) {
            memcpy(m, data, len);
            int flips = 1 + (int)(it_rng(&rng) % 8);
            for (int k = 0; k < flips; k++) {
                size_t pos = len / 3 + it_rng(&rng) % (len - len / 3);
                m[pos] ^= (uint8_t)(1u << (it_rng(&rng) % 8));
            }
            AuImageInfo info;
            AuSurface *s = au_image_decode_buffer(m, len, NULL, &info);
            AuSurface *s2 = it_decode_chunked(m, len, NULL, 7, (uint32_t)it, NULL);
            IT_CHECK((s == NULL) == (s2 == NULL) && (!s || same_pixels(s, s2)), "%s: corrupt %d chunked differs",
                     names[f], it);
            au_surface_free(s);
            au_surface_free(s2);
        }
        free(m);
        free(data);
    }

    /* A lost restart marker: the decoder resynchronises at the next one
     * and the rest of the image is right. */
    size_t len;
    uint8_t *data = load("rst2b.jpg", &len);
    if (data) {
        uint8_t *m = malloc(len);
        size_t k = 0, removed = 0;
        int seen = 0;
        for (size_t i = 0; i < len; i++) {
            if (!removed && i + 1 < len && data[i] == 0xFF && data[i + 1] >= 0xD0 && data[i + 1] <= 0xD7 &&
                ++seen == 3) {
                i++;
                removed = 1;
                continue;
            }
            m[k++] = data[i];
        }
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(m, k, NULL, &info);
        int w, h;
        uint8_t *ref = load_ref("rst2b.ppm", &w, &h);
        IT_CHECK(s && !info.partial, "lost RST: decode");
        if (s && ref) {
            /* The last rows come after the damage: they must match. */
            int bad = 0;
            for (int y = h - 8; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint32_t p = s->pixels[(size_t)y * s->stride + x];
                    const uint8_t *r = ref + ((size_t)y * w + x) * 4;
                    if (abs((int)(p >> 16 & 255) - r[0]) > 2 || abs((int)(p & 255) - r[2]) > 2)
                        bad++;
                }
            IT_CHECK(bad == 0, "lost RST: %d pixels wrong after resync", bad);
        }
        free(ref);
        au_surface_free(s);
        free(m);
        free(data);
    }
}

/* Mutates the vectors (bit flips, byte runs, truncation, inserted
 * markers) and decodes them whole and chunked, scaled and not. */
static int fuzz(long iters, uint32_t seed, int nfiles, char **files)
{
    uint8_t *bufs[256];
    size_t lens[256];
    int nb = 0;
    for (int i = 0; i < nfiles && nb < 256; i++) {
        bufs[nb] = it_read_file(files[i], &lens[nb]);
        if (bufs[nb] && lens[nb] > 4)
            nb++;
    }
    if (!nb)
        return 1;
    ItRng rng = {seed * 2654435761u + 7};
    uint8_t *m = malloc(1 << 20);
    for (long it = 0; it < iters; it++) {
        int f = (int)(it_rng(&rng) % (uint32_t)nb);
        size_t len = lens[f];
        if (len > (1 << 19))
            len = 1 << 19;
        memcpy(m, bufs[f], len);
        int ops = 1 + (int)(it_rng(&rng) % 6);
        for (int k = 0; k < ops; k++) {
            uint32_t r = it_rng(&rng);
            size_t pos = it_rng(&rng) % len;
            switch (r % 6) {
            case 0: m[pos] ^= (uint8_t)(1u << (r >> 8) % 8); break;
            case 1: m[pos] = (uint8_t)(r >> 8); break;
            case 2: m[pos] = 0xFF; break;
            case 3:
                if (pos + 1 < len)
                    m[pos] = 0xFF, m[pos + 1] = (uint8_t)(0xC0 + (r >> 8) % 0x30);
                break;
            case 4: len = pos + 1; break;
            default:
                for (size_t q = pos; q < len && q < pos + 8; q++)
                    m[q] = (uint8_t)(r >> (q & 7));
                break;
            }
        }
        AuImageOptions o = {0};
        if (it & 1) {
            o.target_w = 1 + (int)(it_rng(&rng) % 64);
            o.target_h = 1 + (int)(it_rng(&rng) % 64);
        }
        /* Whatever the data, chunked feeding gives what whole-buffer
         * decoding gives. */
        AuImageInfo info, info2;
        AuSurface *s = au_image_decode_buffer(m, len, &o, &info);
        AuSurface *s2 = it_decode_chunked(m, len, &o, 1 + it_rng(&rng) % 300, (uint32_t)it, &info2);
        IT_CHECK((s == NULL) == (s2 == NULL) && (!s || same_pixels(s, s2)) && info.partial == info2.partial,
                 "fuzz %ld (file %s): chunked decode differs", it, files[f]);
        au_surface_free(s);
        au_surface_free(s2);
    }
    free(m);
    for (int i = 0; i < nb; i++)
        free(bufs[i]);
    printf("fuzz: %ld iterations, %d files\n", iters, nb);
    return it_failures ? 1 : 0;
}

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

static int bench(const char *path, int reps)
{
    size_t len;
    uint8_t *data = it_read_file(path, &len);
    if (!data)
        return 1;
    for (int k = 0; k <= 3; k++) {
        AuImageInfo info = {0};
        double t = now();
        for (int r = 0; r < reps; r++) {
            AuImageOptions o = {0};
            o.max_bytes = (size_t)64 << 20;
            if (k) {
                AuImageInfo pi;
                au_image_probe(data, len, &pi);
                o.target_w = pi.width >> k;
                o.target_h = pi.height >> k;
            }
            AuSurface *s = au_image_decode_buffer(data, len, &o, &info);
            au_surface_free(s);
        }
        t = (now() - t) / reps;
        printf("%s 1/%d: %dx%d -> %dx%d, %.1f ms, %.1f MP/s (source pixels), peak %zu bytes\n", path, 1 << k,
               info.width, info.height, info.out_w, info.out_h, t * 1000,
               (double)info.width * info.height / t / 1e6, info.peak_bytes);
    }
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "--fuzz"))
        return fuzz(atol(argv[2]), (uint32_t)atol(argv[3]), argc - 4, argv + 4);
    if (argc >= 3 && !strcmp(argv[1], "--bench"))
        return bench(argv[2], argc > 3 ? atoi(argv[3]) : 5);
    if (argc < 2) {
        printf("usage: test_jpeg VECTOR_DIR\n");
        return 2;
    }
    vdir = argv[1];
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++)
        test_vector(&vecs[i]);
    test_orientation();
    test_unsupported();
    test_probe();
    test_streaming();
    test_lowmem();
    test_hostile();
    test_corrupt();
    if (it_failures) {
        printf("%d failures\n", it_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
