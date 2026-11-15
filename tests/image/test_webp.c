/* WebP decoder tests: every vector in vectors/webp (made by
 * gen-webp-vectors.py) against dwebp's decode, whole and chunked, scaled,
 * truncated and probed; animations frame by frame against composited
 * references; hostile and corrupt files.
 *
 * usage: test_webp VECTOR_DIR
 *        test_webp --fuzz N SEED FILE...   mutate the files N times
 *        test_webp --bench FILE [REPS]     decode speed and memory
 *        test_webp --testdata DIR          the libwebp test data, with
 *                                          references in DIR/ref (exit 77,
 *                                          skipped, when DIR has none) */
#include "imgtest.h"

#include <dirent.h>
#include <time.h>

/* Lossy (VP8) output matches dwebp exactly: the YUV to RGB formula and
 * the fancy upsampler are libwebp's. Lossless must match exactly. */
#define LOSSY_TOL 0

static char pbuf[3][1024];

static const char *path3(int slot, const char *dir, const char *name, const char *ext)
{
    snprintf(pbuf[slot], sizeof pbuf[slot], "%s/%s%s", dir, name, ext);
    return pbuf[slot];
}

static bool same_surface(const AuSurface *a, const AuSurface *b)
{
    if (!a || !b || a->width != b->width || a->height != b->height)
        return false;
    for (int y = 0; y < a->height; y++)
        if (memcmp(a->pixels + (size_t)y * a->stride, b->pixels + (size_t)y * b->stride, (size_t)a->width * 4))
            return false;
    return true;
}

/* The 2x reduction image.c makes by averaging (premultiplied, rounded),
 * computed from the full-size canvas. */
static bool check_half(const AuSurface *full, const AuSurface *half, const char *name)
{
    int w = (full->width + 1) / 2, h = (full->height + 1) / 2;
    if (!half || half->width != w || half->height != h) {
        printf("FAIL %s: half size %dx%d\n", name, half ? half->width : -1, half ? half->height : -1);
        it_failures++;
        return false;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t s[4] = {0, 0, 0, 0}, n = 0;
            for (int dy = 0; dy < 2 && 2 * y + dy < full->height; dy++)
                for (int dx = 0; dx < 2 && 2 * x + dx < full->width; dx++) {
                    uint32_t p = full->pixels[(size_t)(2 * y + dy) * full->stride + 2 * x + dx];
                    s[0] += p >> 24;
                    s[1] += p >> 16 & 255;
                    s[2] += p >> 8 & 255;
                    s[3] += p & 255;
                    n++;
                }
            uint32_t v[4];
            for (int c = 0; c < 4; c++)
                v[c] = (s[c] + n / 2) / n;
            for (int c = 1; c < 4; c++)
                if (v[c] > v[0])
                    v[c] = v[0];
            uint32_t want = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
            uint32_t got = half->pixels[(size_t)y * half->stride + x];
            if (want != got) {
                printf("FAIL %s: half-size pixel %d,%d is %08x, expected %08x\n", name, x, y, (unsigned)got,
                       (unsigned)want);
                it_failures++;
                return false;
            }
        }
    return true;
}

static bool is_lossless_name(const char *name)
{
    return !strncmp(name, "ll_", 3) || strstr(name, "lossless") != NULL;
}

static void test_still(const char *dir, const char *name)
{
    size_t len;
    uint8_t *data = it_read_file(path3(0, dir, name, ".webp"), &len);
    int w, h;
    uint8_t *ref = it_load_pnm(path3(1, dir, name, ".pam"), &w, &h);
    if (!data || !ref) {
        IT_CHECK(0, "%s: cannot read the vector or its reference", name);
        free(data);
        free(ref);
        return;
    }
    int tol = is_lossless_name(name) ? 0 : LOSSY_TOL;
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(data, len, NULL, &info);
    IT_CHECK(s != NULL, "%s: decode failed", name);
    if (s) {
        it_compare(s, ref, w, h, tol, tol, name);
        IT_CHECK(info.frame_count == 1 && !info.animated, "%s: frames %u", name, (unsigned)info.frame_count);
        /* One byte at a time, and random chunks, give the same pixels. */
        AuSurface *s1 = it_decode_chunked(data, len, NULL, 1, 0, NULL);
        IT_CHECK(same_surface(s, s1), "%s: byte-at-a-time decode differs", name);
        au_surface_free(s1);
        for (uint32_t seed = 1; seed <= 3; seed++) {
            AuSurface *s2 = it_decode_chunked(data, len, NULL, 1 + len / 5, seed, NULL);
            IT_CHECK(same_surface(s, s2), "%s: chunked decode (seed %u) differs", name, (unsigned)seed);
            au_surface_free(s2);
        }
        /* Half size by averaging. */
        if (w >= 2 && h >= 2) {
            AuImageOptions o = {0};
            o.target_w = (w + 1) / 2;
            o.target_h = (h + 1) / 2;
            AuSurface *hs = au_image_decode_buffer(data, len, &o, NULL);
            check_half(s, hs, name);
            au_surface_free(hs);
        }
        /* A canvas cap that forces a reduction. */
        AuImageOptions o = {0};
        o.max_bytes = (size_t)w * h; /* a quarter of the full size */
        AuImageInfo si;
        AuSurface *ss = au_image_decode_buffer(data, len, &o, &si);
        IT_CHECK(w * h < 4 || (ss && si.scale_denom >= 2), "%s: capped decode", name);
        au_surface_free(ss);
    }
    /* The header arrives within the first 30 bytes. */
    size_t need = 0;
    for (size_t n = 0; n <= len && n <= 64; n++) {
        AuImageInfo pi;
        if (au_image_probe(data, n, &pi)) {
            need = n;
            IT_CHECK(pi.width == w && pi.height == h, "%s: probe size %dx%d", name, pi.width, pi.height);
            break;
        }
    }
    IT_CHECK(need && need <= 30, "%s: probe needs %zu bytes", name, need);
    it_truncations(data, len, len > 3000 ? 3 : 1);
    au_surface_free(s);
    free(ref);
    free(data);
}

typedef struct {
    int loop, nframes;
    uint32_t delay[64];
    int tol[64];
} AnimSpec;

static bool read_anim(const char *path, AnimSpec *a)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    memset(a, 0, sizeof *a);
    char line[128];
    while (fgets(line, sizeof line, f)) {
        unsigned d;
        int t;
        if (sscanf(line, "loop %d", &a->loop) == 1)
            continue;
        if (sscanf(line, "frame %u %d", &d, &t) == 2 && a->nframes < 64) {
            a->delay[a->nframes] = d;
            a->tol[a->nframes++] = t;
        }
    }
    fclose(f);
    return true;
}

/* Decodes every frame, feeding chunks of at most maxchunk bytes (0: the
 * whole buffer), comparing each canvas with its reference. */
static void anim_pass(const char *dir, const char *name, const uint8_t *data, size_t len, const AnimSpec *a,
                      size_t maxchunk, uint32_t seed)
{
    AuImageDecoder *d = au_image_decoder_new(NULL);
    ItRng rng = {seed * 2654435761u + 3};
    size_t pos = 0;
    int frame = 0;
    for (int guard = 0; guard < 1000000; guard++) {
        size_t c = maxchunk ? 1 + it_rng(&rng) % maxchunk : len - pos;
        if (c > len - pos)
            c = len - pos;
        const uint8_t *p = data + pos;
        size_t n = c;
        AuImageStatus st = au_image_decode(d, &p, &n, pos + c == len);
        pos += c - n;
        if (st == AU_IMAGE_MORE) {
            IT_CHECK(n == 0, "%s: MORE without consuming input", name);
            if (n)
                break;
            continue;
        }
        if (st != AU_IMAGE_FRAME) {
            IT_CHECK(st == AU_IMAGE_DONE, "%s: error after %d frames: %s", name, frame, au_image_error(d));
            break;
        }
        const AuImageInfo *info = au_image_info(d);
        char label[160];
        snprintf(label, sizeof label, "%s frame %d (chunks %zu)", name, frame, maxchunk);
        if (frame < a->nframes) {
            char ext[16];
            snprintf(ext, sizeof ext, ".f%d.pam", frame);
            int w, h;
            uint8_t *ref = it_load_pnm(path3(2, dir, name, ext), &w, &h);
            IT_CHECK(ref != NULL, "%s: no reference", label);
            if (ref)
                it_compare(au_image_canvas(d), ref, w, h, a->tol[frame], a->tol[frame] ? 0.5 : 0, label);
            free(ref);
            IT_CHECK(info->frame_delay_ms == a->delay[frame], "%s: delay %u, expected %u", label,
                     (unsigned)info->frame_delay_ms, (unsigned)a->delay[frame]);
        }
        IT_CHECK(info->animated && info->loop_count == a->loop, "%s: animated %d loop %d", label, info->animated,
                 info->loop_count);
        frame++;
    }
    IT_CHECK(frame == a->nframes, "%s: %d frames, expected %d", name, frame, a->nframes);
    au_image_decoder_free(d);
}

static void test_anim(const char *dir, const char *name)
{
    AnimSpec a;
    size_t len;
    uint8_t *data = it_read_file(path3(0, dir, name, ".webp"), &len);
    if (!data || !read_anim(path3(1, dir, name, ".anim"), &a)) {
        IT_CHECK(0, "%s: cannot read the vector", name);
        free(data);
        return;
    }
    anim_pass(dir, name, data, len, &a, 0, 0);
    anim_pass(dir, name, data, len, &a, 1, 0);
    anim_pass(dir, name, data, len, &a, 97, 5);
    IT_CHECK(it_count_frames(data, len) == a.nframes, "%s: frame count", name);
    /* First frame only. */
    int w, h;
    uint8_t *ref = it_load_pnm(path3(2, dir, name, ".f0.pam"), &w, &h);
    AuSurface *s = au_image_decode_buffer(data, len, NULL, NULL);
    if (ref)
        it_compare(s, ref, w, h, a.tol[0], a.tol[0], name);
    au_surface_free(s);
    free(ref);
    /* Scaled (point sampled) frames must not fail. */
    AuImageOptions o = {0};
    o.target_w = w / 4 + 1;
    o.target_h = h / 4 + 1;
    AuImageDecoder *d = au_image_decoder_new(&o);
    const uint8_t *p = data;
    size_t n = len;
    int frames = 0;
    while (au_image_decode(d, &p, &n, true) == AU_IMAGE_FRAME)
        frames++;
    IT_CHECK(frames == a.nframes && !au_image_error(d)[0], "%s: scaled animation: %d frames, %s", name, frames,
             au_image_error(d));
    au_image_decoder_free(d);
    /* Truncated animations: every prefix through all frames. */
    for (size_t k = 0; k < len; k += len > 3000 ? 5 : 1)
        it_count_frames(data, k);
    free(data);
}

/* ---- Hostile and corrupt files ---- */

static size_t put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return 4;
}

/* A RIFF file holding one chunk. */
static size_t riff1(uint8_t *out, const char *fourcc, const uint8_t *payload, size_t n)
{
    memcpy(out, "RIFF", 4);
    put_le32(out + 4, (uint32_t)(4 + 8 + n + (n & 1)));
    memcpy(out + 8, "WEBP", 4);
    memcpy(out + 12, fourcc, 4);
    put_le32(out + 16, (uint32_t)n);
    memcpy(out + 20, payload, n);
    if (n & 1)
        out[20 + n] = 0;
    return 20 + n + (n & 1);
}

static void expect_fail_fast(const uint8_t *data, size_t len, const char *what, size_t max_peak)
{
    clock_t t0 = clock();
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(data, len, NULL, &info);
    double t = (double)(clock() - t0) / CLOCKS_PER_SEC;
    IT_CHECK(s == NULL || info.partial, "%s: should fail", what);
    IT_CHECK(t < 1.0, "%s: took %.2f s", what, t);
    IT_CHECK(info.peak_bytes <= max_peak, "%s: peak %zu bytes", what, info.peak_bytes);
    au_surface_free(s);
    /* Chunked too. */
    s = it_decode_chunked(data, len, NULL, 7, 1, NULL);
    au_surface_free(s);
}

static void test_hostile(void)
{
    uint8_t f[256], pl[64];
    size_t n;
    /* Lossy key frame header declaring 16383 x 16383. */
    static const uint8_t vp8hdr[10] = {0x50, 0x01, 0x00, 0x9d, 0x01, 0x2a, 0xff, 0x3f, 0xff, 0x3f};
    memset(pl, 0, sizeof pl);
    memcpy(pl, vp8hdr, 10);
    n = riff1(f, "VP8 ", pl, 40);
    expect_fail_fast(f, n, "VP8 16383x16383", 1 << 20);
    /* 8000 x 8000 is under the pixel limit: the canvas is scaled to the
     * cap and decoding stops at the end of the data. */
    pl[6] = 8000 & 255;
    pl[7] = 8000 >> 8;
    pl[8] = 8000 & 255;
    pl[9] = 8000 >> 8;
    n = riff1(f, "VP8 ", pl, 40);
    expect_fail_fast(f, n, "VP8 8000x8000", (size_t)20 << 20);
    /* Lossless 16384 x 16384. */
    memset(pl, 0, sizeof pl);
    pl[0] = 0x2f;
    put_le32(pl + 1, 0x3fffu | (0x3fffu << 14));
    n = riff1(f, "VP8L", pl, 40);
    expect_fail_fast(f, n, "VP8L 16384x16384", 1 << 20);
    /* Lossless 8000 x 8000: over the pixel-array cap. */
    put_le32(pl + 1, 7999u | (7999u << 14));
    n = riff1(f, "VP8L", pl, 40);
    expect_fail_fast(f, n, "VP8L 8000x8000", (size_t)20 << 20);
    /* VP8X with a 2^24 x 2^24 canvas. */
    memset(pl, 0, sizeof pl);
    memset(pl + 4, 0xff, 6);
    n = riff1(f, "VP8X", pl, 10);
    expect_fail_fast(f, n, "VP8X 2^24", 1 << 20);
    /* An interframe. */
    memset(pl, 0, sizeof pl);
    memcpy(pl, vp8hdr, 10);
    pl[0] |= 1;
    pl[6] = pl[8] = 16;
    pl[7] = pl[9] = 0;
    n = riff1(f, "VP8 ", pl, 40);
    expect_fail_fast(f, n, "VP8 interframe", 1 << 20);
    /* Bad RIFF sizes. */
    n = riff1(f, "VP8L", pl, 10);
    put_le32(f + 4, 3);
    expect_fail_fast(f, n, "RIFF size 3", 1 << 20);
    put_le32(f + 4, 0xffffffffu);
    expect_fail_fast(f, n, "RIFF size 2^32-1", 1 << 20);
    /* Chunk size near 2^32. */
    n = riff1(f, "VP8L", pl, 10);
    put_le32(f + 16, 0xfffffff0u);
    expect_fail_fast(f, n, "chunk size 2^32-16", 1 << 20);
    put_le32(f + 4, 0xfffffff6u);
    expect_fail_fast(f, n, "chunk size 2^32-16 in a 4 GB RIFF", 1 << 20);
    /* Not WebP, and an empty RIFF. */
    memcpy(f + 8, "WEBX", 4);
    expect_fail_fast(f, n, "not WEBP", 1 << 20);
    static const uint8_t empty[12] = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'};
    expect_fail_fast(empty, 12, "empty RIFF", 1 << 20);
}

/* Corruptions of real vectors at the container level. */
static void test_corrupt(const char *dir)
{
    static const char *names[] = {"l_q90", "ll_z6", "la_default", "a_blend", "x_lossy_meta"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t len;
        uint8_t *data = it_read_file(path3(0, dir, names[i], ".webp"), &len);
        if (!data) {
            IT_CHECK(0, "%s: missing", names[i]);
            continue;
        }
        uint8_t *m = malloc(len + 64);
        /* RIFF size larger than the file: the image still decodes. */
        memcpy(m, data, len);
        put_le32(m + 4, 0x7ffffff0u);
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(m, len, NULL, &info);
        IT_CHECK(s != NULL, "%s: RIFF size beyond the file", names[i]);
        au_surface_free(s);
        /* Every chunk size field set to large values. */
        for (size_t pos = 12; pos + 8 <= len;) {
            const uint8_t *q = data + pos + 4;
            uint32_t sz = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
            static const uint32_t bad[] = {0xffffffffu, 0x80000000u, 0x7fffffffu, 0, 1};
            for (size_t b = 0; b < sizeof bad / sizeof bad[0]; b++) {
                memcpy(m, data, len);
                put_le32(m + pos + 4, bad[b]);
                s = au_image_decode_buffer(m, len, NULL, &info);
                au_surface_free(s);
                it_count_frames(m, len);
            }
            if (!memcmp(data + pos, "ANMF", 4))
                pos += 8 + 16; /* step into the frame */
            else
                pos += 8 + sz + (sz & 1);
        }
        /* Bit flips. */
        ItRng rng = {(uint32_t)i * 77 + 1};
        for (int k = 0; k < 300; k++) {
            memcpy(m, data, len);
            for (int j = 0; j < 1 + k % 4; j++)
                m[it_rng(&rng) % len] ^= (uint8_t)(1u << (it_rng(&rng) % 8));
            s = au_image_decode_buffer(m, len, NULL, &info);
            au_surface_free(s);
            it_count_frames(m, len);
        }
        free(m);
        free(data);
    }
}

/* A small still must not hold more than its canvas and a few rows. */
static void test_memory(const char *dir)
{
    static const struct {
        const char *name;
        size_t max;
    } t[] = {{"l_120x88", 120 * 88 * 4 + 64 * 1024}, {"ll_120x80", 120 * 80 * 4 * 2 + 64 * 1024}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        size_t len;
        uint8_t *data = it_read_file(path3(0, dir, t[i].name, ".webp"), &len);
        AuImageInfo info;
        AuSurface *s = data ? au_image_decode_buffer(data, len, NULL, &info) : NULL;
        IT_CHECK(s && info.peak_bytes <= t[i].max, "%s: peak %zu bytes (limit %zu)", t[i].name,
                 s ? info.peak_bytes : 0, t[i].max);
        IT_CHECK(s && info.mem_bytes < info.peak_bytes, "%s: memory released", t[i].name);
        au_surface_free(s);
        free(data);
    }
}

/* A file cut inside its bitstream keeps the rows that decoded: the top
 * rows match the reference and the error is "truncated". */
static void test_partial(const char *dir)
{
    static const char *names[] = {"l_120x88", "ll_120x80", "la_default", "l_parts8"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t len;
        int w, h;
        uint8_t *data = it_read_file(path3(0, dir, names[i], ".webp"), &len);
        uint8_t *ref = it_load_pnm(path3(1, dir, names[i], ".pam"), &w, &h);
        if (!data || !ref) {
            IT_CHECK(0, "%s: missing", names[i]);
            free(data);
            free(ref);
            continue;
        }
        AuImageDecoder *d = au_image_decoder_new(NULL);
        const uint8_t *p = data;
        size_t n = len * 3 / 4;
        AuImageStatus st = au_image_decode(d, &p, &n, true);
        const AuImageInfo *info = au_image_info(d);
        IT_CHECK(st == AU_IMAGE_ERROR && !strcmp(au_image_error(d), "truncated") && info->partial,
                 "%s: cut file: status %d, error '%s', partial %d", names[i], st, au_image_error(d), info->partial);
        /* Rows that match the reference, then only empty rows. */
        const AuSurface *s = au_image_canvas(d);
        int rows = 0;
        bool rest_empty = true;
        for (int y = 0; s && y < h; y++) {
            const uint32_t *row = s->pixels + (size_t)y * s->stride;
            bool match = true, empty = true;
            for (int x = 0; x < w; x++) {
                const uint8_t *r = ref + ((size_t)y * w + x) * 4;
                uint32_t a = r[3];
                uint32_t want = a << 24 | (r[0] * a + 127) / 255 << 16 | (r[1] * a + 127) / 255 << 8 |
                                (r[2] * a + 127) / 255;
                match &= row[x] == want;
                empty &= row[x] == 0;
            }
            if (match && rows == y)
                rows++;
            else if (y >= rows && !empty)
                rest_empty = false;
        }
        IT_CHECK(rows >= 1 && rows < h && rest_empty, "%s: %d of %d rows decoded, rest empty %d", names[i],
                 rows, h, rest_empty);
        au_image_decoder_free(d);
        free(ref);
        free(data);
    }
}

static int run_dir(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) {
        printf("cannot open %s\n", dir);
        return 2;
    }
    struct dirent *e;
    int stills = 0, anims = 0;
    while ((e = readdir(dp))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".webp"))
            continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(n - 5), e->d_name);
        FILE *f = fopen(path3(1, dir, name, ".anim"), "r");
        if (f) {
            fclose(f);
            test_anim(dir, name);
            anims++;
        } else {
            test_still(dir, name);
            stills++;
        }
    }
    closedir(dp);
    printf("%d stills, %d animations\n", stills, anims);
    return stills + anims;
}

/* The libwebp test data: every file that dwebp decoded (it has a
 * reference in ref/) must decode the same; the others must not crash. */
static int testdata(const char *dir)
{
    char refdir[1024];
    snprintf(refdir, sizeof refdir, "%s/ref", dir);
    DIR *dp = opendir(dir);
    if (!dp) {
        printf("no test data in %s: skipped\n", dir);
        return 77;
    }
    struct dirent *e;
    int files = 0, matched = 0;
    while ((e = readdir(dp))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".webp"))
            continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(n - 5), e->d_name);
        size_t len;
        uint8_t *data = it_read_file(path3(0, dir, name, ".webp"), &len);
        if (!data)
            continue;
        files++;
        FILE *af = fopen(path3(1, refdir, name, ".anim"), "r");
        int w, h;
        uint8_t *ref = it_load_pnm(path3(1, refdir, name, ".pam"), &w, &h);
        if (af) {
            fclose(af);
            AnimSpec a;
            read_anim(path3(1, refdir, name, ".anim"), &a);
            int before = it_failures;
            anim_pass(refdir, name, data, len, &a, 0, 0);
            matched += it_failures == before;
        } else if (ref) {
            AuImageOptions o = {0};
            o.max_bytes = (size_t)256 << 20;
            o.max_source_pixels = (uint32_t)256 << 20;
            AuImageInfo info;
            AuSurface *s = au_image_decode_buffer(data, len, &o, &info);
            bool lossless = len > 16 && !memcmp(data + 12, "VP8L", 4);
            int before = it_failures;
            if (!s)
                IT_CHECK(0, "%s: decode failed: %s", name, "error");
            else
                it_compare(s, ref, w, h, lossless ? 0 : LOSSY_TOL, lossless ? 0 : LOSSY_TOL, name);
            matched += it_failures == before;
            au_surface_free(s);
        } else {
            it_count_frames(data, len);
            AuImageInfo info;
            AuSurface *s = au_image_decode_buffer(data, len, NULL, &info);
            printf("%s: no reference (dwebp failed); Aura: %s\n", name, s ? "decoded" : "failed");
            au_surface_free(s);
        }
        it_truncations(data, len, len / 8 + 1);
        free(ref);
        free(data);
    }
    closedir(dp);
    if (!files) {
        printf("no test data in %s: skipped\n", dir);
        return 77;
    }
    printf("test data: %d files, %d match their reference\n", files, matched);
    return it_failures ? 1 : 0;
}

/* Mutates the files (bit flips, byte runs, chunk sizes, truncation) and
 * decodes them whole, chunked, scaled and through every frame. */
static int fuzz(long iters, uint32_t seed, int nfiles, char **files)
{
    uint8_t *bufs[512];
    size_t lens[512];
    int nb = 0;
    for (int i = 0; i < nfiles && nb < 512; i++) {
        size_t n = strlen(files[i]);
        if (n < 5 || strcmp(files[i] + n - 5, ".webp"))
            continue;
        bufs[nb] = it_read_file(files[i], &lens[nb]);
        if (bufs[nb] && lens[nb] > 12)
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
            switch (r % 7) {
            case 0:
            case 1: m[pos] ^= (uint8_t)(1u << (r >> 8) % 8); break;
            case 2: m[pos] = (uint8_t)(r >> 8); break;
            case 3:
                if (pos + 4 <= len)
                    put_le32(m + pos, r & 0x8000 ? r >> 12 : r);
                break;
            case 4:
                if (it_rng(&rng) % 4 == 0)
                    len = pos + 1;
                break;
            case 5:
                for (size_t q = pos; q < len && q < pos + 8; q++)
                    m[q] = (uint8_t)(r >> (q & 7));
                break;
            default:
                if (len >= 20)
                    put_le32(m + 16, (uint32_t)(it_rng(&rng) % (len * 2)));
                break;
            }
        }
        AuImageOptions o = {0};
        if (it & 1) {
            o.target_w = 1 + (int)(it_rng(&rng) % 64);
            o.target_h = 1 + (int)(it_rng(&rng) % 64);
        }
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(m, len, &o, &info);
        au_surface_free(s);
        s = it_decode_chunked(m, len, &o, 1 + it_rng(&rng) % 300, (uint32_t)it, NULL);
        au_surface_free(s);
        if (it % 4 == 0)
            it_count_frames(m, len);
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
    for (int k = 0; k <= 2; k++) {
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
        printf("%s 1/%d: %dx%d -> %dx%d, %.1f ms, %.1f MP/s, peak %zu bytes\n", path, 1 << k, info.width,
               info.height, info.out_w, info.out_h, t * 1000, (double)info.width * info.height / t / 1e6,
               info.peak_bytes);
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
    if (argc >= 3 && !strcmp(argv[1], "--testdata"))
        return testdata(argv[2]);
    if (argc < 2) {
        printf("usage: test_webp VECTOR_DIR\n");
        return 2;
    }
    int n = run_dir(argv[1]);
    IT_CHECK(n >= 40, "only %d vectors", n);
    test_hostile();
    test_corrupt(argv[1]);
    test_memory(argv[1]);
    test_partial(argv[1]);
    if (it_failures) {
        printf("%d failures\n", it_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
