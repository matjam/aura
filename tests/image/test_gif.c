/* GIF tests: the generated vectors (palette sizes, interlace, local
 * palettes, transparency, clear codes, a full LZW table, clipping) and
 * animations (every disposal method, delays, loop counts) against
 * references computed by gen-gif-vectors.py; chunked feeding,
 * truncation, corruption, hostile sizes, scaled decoding.
 *
 * usage: test_gif VECTOR_DIR */
#include "vectest.h"

static void test_hostile(void)
{
    size_t len;
    uint8_t *data = vt_load("full.gif", &len);
    IT_CHECK(data != NULL, "full.gif missing");
    if (!data)
        return;
    uint8_t *bad = malloc(len + 64);
    ItRng rng = {5};
    for (int i = 0; i < 3000; i++) {
        memcpy(bad, data, len);
        int flips = 1 + (int)(it_rng(&rng) % 6);
        for (int k = 0; k < flips; k++)
            bad[it_rng(&rng) % len] = (uint8_t)it_rng(&rng);
        au_surface_free(au_image_decode_buffer(bad, len, NULL, NULL));
    }
    /* A screen of 65535 x 65535 is over the pixel cap: refused at once. */
    memcpy(bad, data, len);
    bad[6] = bad[7] = bad[8] = bad[9] = 0xFF;
    AuImageInfo info;
    AuSurface *s = au_image_decode_buffer(bad, len, NULL, &info);
    IT_CHECK(!s && !info.ready, "65535x65535 screen accepted");
    au_surface_free(s);
    /* 8000 x 8000 (64 megapixels, at the cap): a scaled canvas within
     * max_bytes; the small frame decodes into its corner. */
    memcpy(bad, data, len);
    bad[6] = 0x40, bad[7] = 0x1F, bad[8] = 0x40, bad[9] = 0x1F;
    s = au_image_decode_buffer(bad, len, NULL, &info);
    IT_CHECK(s && (size_t)s->width * s->height * 4 <= AU_IMAGE_DEFAULT_MAX_BYTES, "8000x8000: %dx%d", s ? s->width : 0,
             s ? s->height : 0);
    IT_CHECK(info.peak_bytes < AU_IMAGE_DEFAULT_MAX_BYTES + 256 * 1024, "8000x8000: peak %zu", info.peak_bytes);
    au_surface_free(s);
    /* Probe: the size is known after the 13-byte header. */
    IT_CHECK(!au_image_probe(data, 12, &info), "probe before the screen descriptor");
    IT_CHECK(au_image_probe(data, 13, &info) && info.width == 120 && info.height == 90, "probe after it");
    /* A frame whose data stops early is shown as far as it got. */
    s = au_image_decode_buffer(data, len / 2, NULL, &info);
    IT_CHECK(s && info.partial, "truncated GIF keeps its rows");
    au_surface_free(s);
    /* Scaled: the frame is point sampled into the reduced canvas. */
    AuImageOptions o = {0};
    o.target_w = 60;
    o.target_h = 45;
    s = au_image_decode_buffer(data, len, &o, &info);
    IT_CHECK(s && s->width == 60 && s->height == 45 && info.scale_denom == 2, "scaled GIF");
    AuSurface *full = au_image_decode_buffer(data, len, NULL, NULL);
    if (s && full)
        IT_CHECK(s->pixels[10 * s->stride + 7] == full->pixels[20 * full->stride + 14], "scaled GIF samples");
    au_surface_free(full);
    au_surface_free(s);
    free(bad);
    free(data);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_gif VECTOR_DIR\n");
        return 2;
    }
    snprintf(vt_dir, sizeof vt_dir, "%s", argv[1]);
    int n = vt_stills("gif", AU_IMAGE_FORMAT_GIF, 0, 0);
    IT_CHECK(n >= 13, "only %d still vectors", n);
    vt_animation("anim", "gif", 0);
    vt_animation("anim-loop2", "gif", 0);
    vt_animation("anim-once", "gif", 0);
    test_hostile();
    IT_CHECK(au_image_frame_delay(0) == 100 && au_image_frame_delay(10) == 100 && au_image_frame_delay(20) == 20,
             "minimum frame delay rule");
    if (it_failures) {
        printf("%d failures\n", it_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
