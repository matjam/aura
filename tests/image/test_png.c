/* PNG and APNG tests: the generated vectors (every colour type, depth,
 * interlace, tRNS, filter; APNG frames) against references computed by
 * gen-png-vectors.py, chunked feeding, truncation, corruption, hostile
 * sizes, scaled decoding, and PngSuite when build/pngsuite is present.
 *
 * usage: test_png VECTOR_DIR [PNGSUITE_DIR] */
#include "imgtest.h"

#include <dirent.h>

static char vdir[1024];

static uint8_t *load(const char *name, size_t *len)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", vdir, name);
    return it_read_file(path, len);
}

static uint8_t *load_ref(const char *name, int *w, int *h)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", vdir, name);
    return it_load_pnm(path, w, h);
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

/* Every still vector: exact match, whole and chunked, and truncations. */
static void test_stills(void)
{
    DIR *dir = opendir(vdir);
    IT_CHECK(dir != NULL, "cannot open %s", vdir);
    if (!dir)
        return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(dir))) {
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".png") || !strncmp(e->d_name, "apng", 4))
            continue;
        char base[256], refname[300];
        snprintf(base, sizeof base, "%.*s", (int)(nl - 4), e->d_name);
        snprintf(refname, sizeof refname, "%s.pam", base);
        size_t len;
        uint8_t *data = load(e->d_name, &len);
        int rw, rh;
        uint8_t *ref = load_ref(refname, &rw, &rh);
        if (!data || !ref) {
            IT_CHECK(0, "%s: missing vector or reference", base);
            free(data);
            free(ref);
            continue;
        }
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(data, len, NULL, &info);
        IT_CHECK(s != NULL, "%s: decode failed", base);
        if (s) {
            it_compare(s, ref, rw, rh, 0, 0, base);
            IT_CHECK(info.format == AU_IMAGE_FORMAT_PNG, "%s: format", base);
            AuSurface *c1 = it_decode_chunked(data, len, NULL, 1, 1, NULL);
            IT_CHECK(same_pixels(s, c1), "%s: byte-at-a-time decode differs", base);
            AuSurface *c2 = it_decode_chunked(data, len, NULL, 50, (uint32_t)n + 3, NULL);
            IT_CHECK(same_pixels(s, c2), "%s: chunked decode differs", base);
            au_surface_free(c1);
            au_surface_free(c2);
            /* Opaque flag: set exactly when every reference alpha is 255. */
            bool opaque = true;
            for (size_t i = 0; i < (size_t)rw * rh; i++)
                opaque &= ref[i * 4 + 3] == 255;
            IT_CHECK(s->opaque == opaque, "%s: opaque flag %d, expected %d", base, s->opaque, opaque);
        }
        it_truncations(data, len, len > 4000 ? 7 : 1);
        au_surface_free(s);
        free(data);
        free(ref);
        n++;
    }
    closedir(dir);
    IT_CHECK(n >= 50, "only %d still vectors", n);
}

static void test_apng(const char *name)
{
    char fname[300], txt[300];
    snprintf(fname, sizeof fname, "%s.png", name);
    snprintf(txt, sizeof txt, "%s.txt", name);
    size_t len, tl;
    uint8_t *data = load(fname, &len);
    uint8_t *meta = load(txt, &tl);
    IT_CHECK(data && meta, "%s: missing", name);
    if (!data || !meta) {
        free(data);
        free(meta);
        return;
    }
    char mbuf[512];
    snprintf(mbuf, sizeof mbuf, "%.*s", (int)(tl < 500 ? tl : 500), (char *)meta);
    int frames = 0, loop = 0, delays[64], nd = 0;
    char *p = mbuf;
    sscanf(p, "frames %d loop %d delays", &frames, &loop);
    p = strstr(p, "delays");
    if (p) {
        p += 6;
        int off;
        while (nd < 64 && sscanf(p, "%d%n", &delays[nd], &off) == 1) {
            nd++;
            p += off;
        }
    }
    for (int chunked = 0; chunked < 2; chunked++) {
        AuImageDecoder *d = au_image_decoder_new(NULL);
        const uint8_t *in = data;
        size_t left = len;
        int f = 0;
        for (int guard = 0; guard < 100000; guard++) {
            size_t c = chunked ? (left < 3 ? left : 3) : left;
            const uint8_t *q = in;
            size_t qn = c;
            AuImageStatus st = au_image_decode(d, &q, &qn, c == left);
            in += c - qn;
            left -= c - qn;
            if (st == AU_IMAGE_FRAME) {
                char rn[300], label[300];
                snprintf(rn, sizeof rn, "%s.f%d.pam", name, f);
                snprintf(label, sizeof label, "%s frame %d%s", name, f, chunked ? " (chunked)" : "");
                int rw, rh;
                uint8_t *ref = load_ref(rn, &rw, &rh);
                IT_CHECK(ref != NULL, "%s: no reference", label);
                if (ref)
                    it_compare(au_image_canvas(d), ref, rw, rh, 0, 0, label);
                free(ref);
                const AuImageInfo *info = au_image_info(d);
                IT_CHECK(f >= nd || info->frame_delay_ms == (uint32_t)delays[f], "%s: delay %u, expected %d", label,
                         info->frame_delay_ms, f < nd ? delays[f] : -1);
                IT_CHECK(info->loop_count == loop, "%s: loop %d, expected %d", label, info->loop_count, loop);
                f++;
                continue;
            }
            if (st == AU_IMAGE_MORE)
                continue;
            IT_CHECK(st == AU_IMAGE_DONE, "%s: ended with %d (%s)", name, st, au_image_error(d));
            break;
        }
        IT_CHECK(f == frames, "%s: %d frames, expected %d", name, f, frames);
        IT_CHECK(au_image_info(d)->animated == (frames > 1), "%s: animated flag", name);
        au_image_decoder_free(d);
    }
    it_truncations(data, len, 1);
    free(data);
    free(meta);
}

/* Box-averages a premultiplied surface by f (the reference for scaled
 * decoding: rounding as the decoder does it). */
static AuSurface *box_reduce(const AuSurface *s, int f)
{
    int ow = (s->width + f - 1) / f, oh = (s->height + f - 1) / f;
    AuSurface *o = au_surface_new(ow, oh);
    for (int oy = 0; oy < oh; oy++)
        for (int ox = 0; ox < ow; ox++) {
            uint32_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (int y = oy * f; y < oy * f + f && y < s->height; y++)
                for (int x = ox * f; x < ox * f + f && x < s->width; x++) {
                    uint32_t p = s->pixels[(size_t)y * s->stride + x];
                    sum[0] += p >> 24;
                    sum[1] += p >> 16 & 255;
                    sum[2] += p >> 8 & 255;
                    sum[3] += p & 255;
                    n++;
                }
            uint32_t v[4];
            for (int c = 0; c < 4; c++)
                v[c] = (sum[c] + n / 2) / n;
            o->pixels[(size_t)oy * o->stride + ox] = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
        }
    return o;
}

static void test_scaled(void)
{
    size_t len;
    uint8_t *data = load("big.png", &len);
    IT_CHECK(data != NULL, "big.png missing");
    if (!data)
        return;
    AuSurface *full = au_image_decode_buffer(data, len, NULL, NULL);
    IT_CHECK(full && full->width == 160 && full->height == 120, "big.png full size");
    for (int f = 2; f <= 8 && full; f *= 2) {
        AuImageOptions o = {0};
        o.target_w = 160 / f;
        o.target_h = 120 / f;
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(data, len, &o, &info);
        AuSurface *ref = box_reduce(full, f);
        IT_CHECK(s && info.scale_denom == f, "scale 1/%d: denom %d", f, info.scale_denom);
        IT_CHECK(info.width == 160 && info.height == 120, "scale 1/%d keeps the intrinsic size", f);
        IT_CHECK(same_pixels(s, ref), "scale 1/%d: not the box average", f);
        au_surface_free(s);
        au_surface_free(ref);
    }
    /* A canvas cap below the image forces a reduction. */
    AuImageOptions o = {0};
    o.max_bytes = 160 * 120; /* a quarter of the full canvas */
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(data, len, &o, &info);
    IT_CHECK(s && s->width == 80 && s->height == 60, "max_bytes reduction: %dx%d", s ? s->width : 0,
             s ? s->height : 0);
    au_surface_free(s);
    /* Interlaced vectors reduce by point sampling. */
    uint8_t *il = load("c6d08i.png", &len);
    if (il) {
        AuImageOptions o2 = {0};
        o2.target_w = 18;
        AuSurface *si = au_image_decode_buffer(il, len, &o2, &info);
        IT_CHECK(si && si->width == 19 && si->height == 12, "interlaced scaled size");
        au_surface_free(si);
        free(il);
    }
    au_surface_free(full);
    free(data);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Builds a PNG from an IHDR and a raw zlib stream (CRCs computed). */
static size_t make_png(uint8_t *out, uint32_t w, uint32_t h, int depth, int ct, const uint8_t *z, size_t zn)
{
    size_t n = 0;
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    n = 8;
    uint8_t ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = (uint8_t)depth;
    ihdr[9] = (uint8_t)ct;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    const char *types[3] = {"IHDR", "IDAT", "IEND"};
    const uint8_t *datas[3] = {ihdr, z, NULL};
    size_t lens[3] = {13, zn, 0};
    for (int i = 0; i < 3; i++) {
        put32(out + n, (uint32_t)lens[i]);
        memcpy(out + n + 4, types[i], 4);
        if (lens[i])
            memcpy(out + n + 8, datas[i], lens[i]);
        put32(out + n + 8 + lens[i], au_crc32(0, out + n + 4, lens[i] + 4));
        n += 12 + lens[i];
    }
    return n;
}

static void test_hostile(void)
{
    size_t len;
    uint8_t *data = load("c2d08.png", &len);
    if (!data)
        return;
    /* CRC errors: in IHDR (critical) fails; in IDAT fails. */
    uint8_t *bad = malloc(len);
    memcpy(bad, data, len);
    bad[29] ^= 1; /* IHDR CRC */
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(bad, len, NULL, &info);
    IT_CHECK(!s, "IHDR CRC error accepted");
    au_surface_free(s);
    /* Bit flips anywhere must not crash (exactness not checked). */
    ItRng rng = {99};
    for (int i = 0; i < 3000; i++) {
        memcpy(bad, data, len);
        int flips = 1 + (int)(it_rng(&rng) % 4);
        for (int k = 0; k < flips; k++)
            bad[it_rng(&rng) % len] ^= (uint8_t)(1u << (it_rng(&rng) % 8));
        au_surface_free(au_image_decode_buffer(bad, len, NULL, NULL));
    }
    free(bad);
    free(data);

    /* A decompression bomb: a tiny file declaring 30000 x 30000 with a
     * zlib stream of all-zero rows. Decoded scaled within the caps (or
     * refused by the source pixel cap): never a 3.6 GB canvas. */
    static uint8_t raw[1 << 16];
    memset(raw, 0, sizeof raw);
    static uint8_t z[1 << 14], file[1 << 15];
    /* Stored zlib of 64 KB of zeros would be large; a fixed-Huffman
     * stream of zeros is small: build it with au_inflate's inverse is
     * not available, so use a hand-made stream: one stored block. */
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    z[zn++] = 0x01; /* final stored block */
    z[zn++] = 0x00;
    z[zn++] = 0x10; /* LEN = 4096 */
    z[zn++] = 0xFF;
    z[zn++] = 0xEF;
    memset(z + zn, 0, 4096);
    zn += 4096;
    uint32_t ad = au_adler32(1, z + 7, 4096);
    put32(z + zn, ad);
    zn += 4;
    size_t fn = make_png(file, 30000, 30000, 8, 0, z, zn);
    s = au_image_decode_buffer(file, fn, NULL, &info);
    IT_CHECK(info.ready || strlen(au_image_format_name(info.format)), "bomb: header");
    IT_CHECK((size_t)info.out_w * info.out_h * 4 <= AU_IMAGE_DEFAULT_MAX_BYTES, "bomb: canvas %dx%d over the cap",
             info.out_w, info.out_h);
    IT_CHECK(info.peak_bytes < AU_IMAGE_DEFAULT_MAX_BYTES + 512 * 1024, "bomb: peak %zu", info.peak_bytes);
    au_surface_free(s);
    /* Over the dimension cap: refused. */
    fn = make_png(file, 40000, 2, 8, 0, z, zn);
    s = au_image_decode_buffer(file, fn, NULL, &info);
    IT_CHECK(!s && !info.ready, "40000-pixel width accepted");
    au_surface_free(s);
    /* Zero size, bad depth, bad colour type. */
    fn = make_png(file, 0, 5, 8, 0, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "zero width accepted");
    fn = make_png(file, 5, 5, 3, 0, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "depth 3 accepted");
    fn = make_png(file, 5, 5, 8, 5, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "colour type 5 accepted");
    fn = make_png(file, 5, 5, 8, 3, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "palette image without PLTE accepted");

    /* Probe: the size is known once IHDR's CRC has arrived (33 bytes). */
    data = load("c6d08.png", &len);
    if (data) {
        IT_CHECK(!au_image_probe(data, 32, &info), "probe before the IHDR CRC");
        IT_CHECK(au_image_probe(data, 33, &info) && info.width == 37 && info.height == 23, "probe after IHDR");
        /* Partial: half the file gives some rows. */
        s = au_image_decode_buffer(data, len / 2, NULL, &info);
        IT_CHECK(s && info.partial, "truncated file keeps its rows");
        au_surface_free(s);
        free(data);
    }
}

/* PngSuite: every good image matches ImageMagick's decode (within 1 for
 * 16-bit rounding); every x* image is refused (or at least not crashed). */
static void test_suite(const char *dirpath)
{
    DIR *dir = opendir(dirpath);
    if (!dir) {
        printf("PngSuite not present (%s): skipped; run tests/image/fetch-pngsuite.sh\n", dirpath);
        return;
    }
    struct dirent *e;
    int good = 0, bad = 0, refused = 0;
    while ((e = readdir(dir))) {
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".png"))
            continue;
        char path[1400], refp[1400];
        snprintf(path, sizeof path, "%s/%s", dirpath, e->d_name);
        snprintf(refp, sizeof refp, "%s/ref/%.*s.pam", dirpath, (int)(nl - 4), e->d_name);
        size_t len;
        uint8_t *data = it_read_file(path, &len);
        if (!data)
            continue;
        AuImageInfo info;
        AuSurface *s = au_image_decode_buffer(data, len, NULL, &info);
        if (e->d_name[0] == 'x') {
            bad++;
            if (!s || info.partial)
                refused++;
            else
                printf("note: PngSuite %s (corrupt) decoded without error\n", e->d_name);
        } else {
            good++;
            int rw, rh;
            uint8_t *ref = it_load_pnm(refp, &rw, &rh);
            IT_CHECK(s != NULL, "PngSuite %s: %s", e->d_name, info.ready ? "decode failed" : "no header");
            if (s && ref) {
                /* 16-bit images: ImageMagick rounds, Aura keeps the high byte. */
                it_compare(s, ref, rw, rh, strstr(e->d_name, "16") ? 1 : 0, 1, e->d_name);
            }
            free(ref);
            it_truncations(data, len, 3);
        }
        au_surface_free(s);
        free(data);
    }
    closedir(dir);
    printf("PngSuite: %d good images checked, %d of %d corrupt ones refused\n", good, refused, bad);
    IT_CHECK(good > 100, "PngSuite: only %d images", good);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_png VECTOR_DIR [PNGSUITE_DIR]\n");
        return 2;
    }
    snprintf(vdir, sizeof vdir, "%s", argv[1]);
    if (argc > 2) {
        test_suite(argv[2]);
    } else {
        test_stills();
        test_apng("apng-basic");
        test_apng("apng-hidden");
        test_scaled();
        test_hostile();
    }
    if (it_failures) {
        printf("%d failures\n", it_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
