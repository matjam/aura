/* BMP, ICO and favicon tests: the generated vectors against references
 * computed by gen-bmp-vectors.py, chunked feeding, truncation,
 * corruption, hostile sizes, ICO entry choice, au_image_decode_favicon
 * and au_image_resample.
 *
 * usage: test_bmp VECTOR_DIR */
#include "vectest.h"

static void test_ico_choice(void)
{
    size_t len;
    uint8_t *data = vt_load("icons.ico", &len);
    IT_CHECK(data != NULL, "icons.ico missing");
    if (!data)
        return;
    /* A 16-pixel target picks a 16 x 16 entry, the 8-bit one (deepest). */
    AuImageOptions o = {0};
    o.target_w = o.target_h = 16;
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(data, len, &o, &info);
    int rw, rh;
    uint8_t *ref = vt_load_ref("icons-16.pam.ref", &rw, &rh);
    IT_CHECK(s && ref, "16-pixel entry");
    if (s && ref)
        it_compare(s, ref, rw, rh, 0, 0, "icons.ico at 16");
    free(ref);
    au_surface_free(s);
    /* 20 pixels: the smallest at least as large is 24 x 24. */
    o.target_w = o.target_h = 20;
    s = au_image_decode_buffer(data, len, &o, &info);
    IT_CHECK(s && s->width == 24, "20-pixel target picks 24, got %d", s ? s->width : 0);
    au_surface_free(s);
    /* 100 pixels: none is that large, so the largest (48). */
    o.target_w = o.target_h = 100;
    s = au_image_decode_buffer(data, len, &o, &info);
    IT_CHECK(s && s->width == 48, "100-pixel target picks 48, got %d", s ? s->width : 0);
    au_surface_free(s);

    /* Favicons: exactly size x size, from the closest entry. */
    for (int size = 8; size <= 64; size *= 2) {
        AuSurface *f = au_image_decode_favicon(data, len, size);
        IT_CHECK(f && f->width == size && f->height == size, "favicon %d", size);
        au_surface_free(f);
    }
    AuSurface *f16 = au_image_decode_favicon(data, len, 16);
    s = au_image_decode_buffer(data, len, &(AuImageOptions){.target_w = 16, .target_h = 16}, NULL);
    IT_CHECK(f16 && s && vt_same_pixels(f16, s), "favicon 16 is the 16-pixel entry unchanged");
    au_surface_free(f16);
    au_surface_free(s);
    free(data);
    /* A favicon from a PNG (non-ICO) image, not square: fitted and centred. */
    data = vt_load("pngicon.ico", &len);
    if (data) {
        AuSurface *f = au_image_decode_favicon(data, len, 16);
        IT_CHECK(f && f->width == 16, "favicon from a PNG entry");
        au_surface_free(f);
        free(data);
    }
    IT_CHECK(!au_image_decode_favicon("hello world, not an image", 25, 16), "favicon from text");
}

static void test_resample(void)
{
    /* A 2x2 checker averaged to 1x1 is the mean; a gradient upscaled
     * stays within its ends; opaque carries over. */
    AuSurface *s = au_surface_new(2, 2);
    s->pixels[0] = 0xFF000000u;
    s->pixels[1] = 0xFFFFFFFFu;
    s->pixels[2] = 0xFFFFFFFFu;
    s->pixels[3] = 0xFF000000u;
    s->opaque = true;
    AuSurface *r = au_image_resample(s, 1, 1);
    IT_CHECK(r && (r->pixels[0] == 0xFF808080u || r->pixels[0] == 0xFF7F7F7Fu), "2x2 to 1x1: %08x",
             r ? r->pixels[0] : 0);
    IT_CHECK(r && r->opaque, "opaque carried over");
    au_surface_free(r);
    r = au_image_resample(s, 7, 5);
    IT_CHECK(r && r->width == 7 && r->height == 5, "upscale size");
    au_surface_free(r);
    IT_CHECK(!au_image_resample(s, 0, 5), "empty size");
    au_surface_free(s);
    /* Downscaling 1000 x 3 to 7 x 1: every source pixel counted once. */
    s = au_surface_new(1000, 3);
    for (int i = 0; i < 3000; i++)
        s->pixels[i] = (i % 2) ? 0xFFFFFFFFu : 0xFF000000u;
    r = au_image_resample(s, 7, 1);
    for (int x = 0; r && x < 7; x++) {
        int g = (int)(r->pixels[x] & 255);
        IT_CHECK(g >= 125 && g <= 130, "1000:7 average at %d: %d", x, g);
    }
    au_surface_free(r);
    au_surface_free(s);
}

static void test_hostile(void)
{
    size_t len;
    uint8_t *data = vt_load("pal8.bmp", &len);
    if (!data)
        return;
    uint8_t *bad = malloc(len);
    AuImageInfo info;
    /* 40000 wide: refused. */
    memcpy(bad, data, len);
    bad[18] = 0x40, bad[19] = 0x9C, bad[20] = 0, bad[21] = 0;
    AuSurface *s = au_image_decode_buffer(bad, len, NULL, &info);
    IT_CHECK(!s && !info.ready, "40000-wide BMP accepted");
    au_surface_free(s);
    /* Height INT32_MIN: refused. */
    memcpy(bad, data, len);
    bad[22] = 0, bad[23] = 0, bad[24] = 0, bad[25] = 0x80;
    s = au_image_decode_buffer(bad, len, NULL, &info);
    IT_CHECK(!s, "INT32_MIN height accepted");
    au_surface_free(s);
    /* 8000 x 8000 in a 1.4 KB file: a scaled canvas, rows missing. */
    memcpy(bad, data, len);
    bad[18] = 0x40, bad[19] = 0x1F, bad[22] = 0x40, bad[23] = 0x1F;
    s = au_image_decode_buffer(bad, len, NULL, &info);
    IT_CHECK(info.ready && (size_t)info.out_w * info.out_h * 4 <= AU_IMAGE_DEFAULT_MAX_BYTES, "8000x8000 BMP canvas");
    IT_CHECK(info.peak_bytes < AU_IMAGE_DEFAULT_MAX_BYTES + 256 * 1024, "8000x8000 BMP peak %zu", info.peak_bytes);
    au_surface_free(s);
    /* Random corruption of every vector kind. */
    const char *names[] = {"pal1.bmp", "pal4.bmp", "rgb24.bmp", "f565.bmp", "v5rgba.bmp", "rle8.bmp", "rle4.bmp",
                           "icons.ico", "pngicon.ico", "os2rgb24.bmp"};
    ItRng rng = {17};
    for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
        size_t n;
        uint8_t *v = vt_load(names[k], &n);
        if (!v)
            continue;
        uint8_t *m = malloc(n);
        for (int i = 0; i < 800; i++) {
            memcpy(m, v, n);
            int flips = 1 + (int)(it_rng(&rng) % 5);
            for (int j = 0; j < flips; j++)
                m[it_rng(&rng) % n] = (uint8_t)it_rng(&rng);
            au_surface_free(au_image_decode_buffer(m, n, NULL, NULL));
            au_surface_free(it_decode_chunked(m, n, NULL, 7, (uint32_t)i, NULL));
        }
        free(m);
        free(v);
    }
    /* Probe: the BMP size is known after the info header. */
    IT_CHECK(!au_image_probe(data, 30, &info), "probe before the info header");
    IT_CHECK(au_image_probe(data, 14 + 40 + 1024, &info) && info.width == 23 && info.height == 13, "probe");
    free(bad);
    free(data);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_bmp VECTOR_DIR\n");
        return 2;
    }
    snprintf(vt_dir, sizeof vt_dir, "%s", argv[1]);
    int n = vt_stills("bmp", AU_IMAGE_FORMAT_BMP, 0, 0);
    IT_CHECK(n >= 15, "only %d BMP vectors", n);
    n = vt_stills("ico", AU_IMAGE_FORMAT_ICO, 0, 0);
    IT_CHECK(n >= 2, "only %d ICO vectors", n);
    test_ico_choice();
    test_resample();
    test_hostile();
    if (it_failures) {
        printf("%d failures\n", it_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
