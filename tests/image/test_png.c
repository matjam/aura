/* PNG and APNG tests: the generated vectors (every colour type, depth,
 * interlace, tRNS, filter; APNG frames) against references computed by
 * gen-png-vectors.py, chunked feeding, truncation, corruption, hostile
 * sizes, scaled decoding, and PngSuite when build/pngsuite is present.
 *
 * usage: test_png VECTOR_DIR [PNGSUITE_DIR] */
#include "vectest.h"

#include "codec/codec.h"

static void test_scaled(void)
{
    size_t len;
    uint8_t *data = vt_load("big.png", &len);
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
        AuSurface *ref = vt_box_reduce(full, f);
        IT_CHECK(s && info.scale_denom == f, "scale 1/%d: denom %d", f, info.scale_denom);
        IT_CHECK(info.width == 160 && info.height == 120, "scale 1/%d keeps the intrinsic size", f);
        IT_CHECK(vt_same_pixels(s, ref), "scale 1/%d: not the box average", f);
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
    uint8_t *il = vt_load("c6d08i.png", &len);
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
        put32(out + n + 8 + lens[i], au_codec_crc32(0, out + n + 4, lens[i] + 4));
        n += 12 + lens[i];
    }
    return n;
}

static void test_hostile(void)
{
    size_t len;
    uint8_t *data = vt_load("c2d08.png", &len);
    if (!data)
        return;
    /* A CRC error in IHDR (a critical chunk) fails. */
    uint8_t *bad = malloc(len);
    memcpy(bad, data, len);
    bad[29] ^= 1;
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
     * zlib stream (one stored block of zero rows). Decoded scaled within
     * the caps: never a 3.6 GB canvas. */
    static uint8_t z[1 << 14], file[1 << 15];
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
    put32(z + zn, au_codec_adler32(1, z + 7, 4096));
    zn += 4;
    size_t fn = make_png(file, 30000, 30000, 8, 0, z, zn);
    s = au_image_decode_buffer(file, fn, NULL, &info);
    IT_CHECK(!s && !info.ready, "30000x30000 (over the source pixel cap) accepted");
    au_surface_free(s);
    fn = make_png(file, 8000, 8000, 8, 0, z, zn);
    s = au_image_decode_buffer(file, fn, NULL, &info);
    IT_CHECK(info.ready && info.scale_denom == 4, "bomb: header, scale 1/%d", info.scale_denom);
    IT_CHECK((size_t)info.out_w * info.out_h * 4 <= AU_IMAGE_DEFAULT_MAX_BYTES, "bomb: canvas %dx%d over the cap",
             info.out_w, info.out_h);
    IT_CHECK(info.peak_bytes < AU_IMAGE_DEFAULT_MAX_BYTES + 512 * 1024, "bomb: peak %zu", info.peak_bytes);
    au_surface_free(s);
    /* Over the dimension cap: refused. */
    fn = make_png(file, 40000, 2, 8, 0, z, zn);
    s = au_image_decode_buffer(file, fn, NULL, &info);
    IT_CHECK(!s && !info.ready, "40000-pixel width accepted");
    au_surface_free(s);
    /* Zero size, bad depth, bad colour type, missing palette. */
    fn = make_png(file, 0, 5, 8, 0, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "zero width accepted");
    fn = make_png(file, 5, 5, 3, 0, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "depth 3 accepted");
    fn = make_png(file, 5, 5, 8, 5, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "colour type 5 accepted");
    fn = make_png(file, 5, 5, 8, 3, z, zn);
    IT_CHECK(!au_image_decode_buffer(file, fn, NULL, NULL), "palette image without PLTE accepted");

    /* Probe: the size is known once IHDR's CRC has arrived (33 bytes). */
    data = vt_load("c6d08.png", &len);
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
 * 16-bit rounding); every x* image is refused. */
static bool test_suite(const char *dirpath)
{
    DIR *dir = opendir(dirpath);
    if (!dir) {
        printf("PngSuite not present (%s): skipped; run tests/image/fetch-pngsuite.sh\n", dirpath);
        return false;
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
                IT_CHECK(0, "PngSuite %s (corrupt) decoded without error", e->d_name);
        } else {
            good++;
            int rw, rh;
            uint8_t *ref = it_load_pnm(refp, &rw, &rh);
            IT_CHECK(s != NULL, "PngSuite %s: %s", e->d_name, info.ready ? "decode failed" : "no header");
            if (s && ref)
                it_compare(s, ref, rw, rh, strstr(e->d_name, "16") ? 1 : 0, 1, e->d_name);
            free(ref);
            it_truncations(data, len, 3);
        }
        au_surface_free(s);
        free(data);
    }
    closedir(dir);
    printf("PngSuite: %d good images checked, %d of %d corrupt ones refused\n", good, refused, bad);
    IT_CHECK(good > 100, "PngSuite: only %d images", good);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_png VECTOR_DIR [PNGSUITE_DIR]\n");
        return 2;
    }
    snprintf(vt_dir, sizeof vt_dir, "%s", argv[1]);
    if (argc > 2) {
        if (!test_suite(argv[2]))
            return 77; /* skipped */
    } else {
        int n = vt_stills("png", AU_IMAGE_FORMAT_PNG, 0, 0);
        IT_CHECK(n >= 50, "only %d still vectors", n);
        vt_animation("apng-basic", "png", 0);
        vt_animation("apng-hidden", "png", 0);
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
