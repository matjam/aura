/* Image cache tests: lazy decoding, sizes from headers, decoding at the
 * displayed size and again larger, stable surfaces, the budget and
 * eviction, progressive filling, animation timing, pausing and loop
 * counts, failures, and keeping pixels instead of bytes.
 *
 * usage: test_cache VECTOR_ROOT (tests/image/vectors) */
#include "imgtest.h"
#include "image/cache.h"

static char root[1024];

static uint8_t *load(const char *rel, size_t *len)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    uint8_t *d = it_read_file(path, len);
    IT_CHECK(d != NULL, "cannot read %s", path);
    return d;
}

static void feed(AuImageCache *c, AuImageEntry *e, const uint8_t *d, size_t n, size_t chunk)
{
    for (size_t i = 0; i < n; i += chunk)
        au_image_entry_data(c, e, d + i, n - i < chunk ? n - i : chunk);
}

static void test_lazy_and_sizes(void)
{
    size_t n;
    uint8_t *d = load("png/big.png", &n);
    if (!d)
        return;
    AuImageCache *c = au_image_cache_new(0);
    AuImageEntry *e = au_image_cache_add(c, "http://x/big.png");
    IT_CHECK(au_image_cache_add(c, "http://x/big.png") == e, "one entry per URL");
    IT_CHECK(au_image_entry_state(e) == AU_IMAGE_PENDING, "pending before data");
    const AuSurface *view = au_image_entry_surface(e);
    IT_CHECK(view->width == 1 && view->height == 1 && view->pixels[0] == 0, "blank surface before decoding");
    au_image_entry_data(c, e, d, 40); /* IHDR and its CRC */
    int w = 0, h = 0;
    IT_CHECK(au_image_entry_state(e) == AU_IMAGE_SIZED && au_image_entry_size(e, &w, &h) && w == 160 && h == 120,
             "size from the header: %dx%d", w, h);
    IT_CHECK(au_image_cache_take_layout_changed(c), "layout told about the size");
    feed(c, e, d + 40, n - 40, 1000);
    au_image_entry_done(c, e, true);
    AuImageCacheStats st;
    au_image_cache_stats(c, &st);
    IT_CHECK(st.decoded == 0 && st.decoded_bytes == 0, "decoded before use");
    /* Shown at 40 x 30: decoded at a quarter. */
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 40, 30);
    IT_CHECK(au_image_entry_state(e) == AU_IMAGE_READY, "ready after use");
    IT_CHECK(au_image_entry_surface(e) == view && view->width == 40 && view->height == 30, "decoded at 40x30: %dx%d",
             view->width, view->height);
    /* Shown at 100 x 75: decoded again, at full size (the next power of
     * two that covers it), into the same surface. */
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 100, 75);
    IT_CHECK(view->width == 160 && view->height == 120, "decoded again larger: %dx%d", view->width, view->height);
    IT_CHECK(view->opaque, "opaque image flagged");
    au_image_cache_stats(c, &st);
    IT_CHECK(st.decodes == 2, "%u decodes", st.decodes);
    au_image_cache_free(c);
    free(d);
}

/* Decoded at full size (before layout gave a size), then painted at a
 * quarter: decoded again smaller once that has lasted SHRINK_EPOCHS (8)
 * paints, and not when a larger use comes between. */
static void test_shrink(void)
{
    size_t n;
    uint8_t *d = load("png/big.png", &n);
    if (!d)
        return;
    AuImageCache *c = au_image_cache_new(0);
    AuImageEntry *e = au_image_cache_add(c, "http://x/big.png");
    feed(c, e, d, n, 1000);
    au_image_entry_done(c, e, true);
    const AuSurface *view = au_image_entry_surface(e);
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 0, 0); /* a prefetch: the natural size */
    IT_CHECK(view->width == 160 && view->height == 120, "full size first: %dx%d", view->width, view->height);
    for (int i = 0; i < 6; i++) {
        au_image_cache_frame_begin(c);
        au_image_entry_use(c, e, 40, 30);
    }
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 160, 120); /* shown large once: starts over */
    for (int i = 0; i < 7; i++) {
        au_image_cache_frame_begin(c);
        au_image_entry_use(c, e, 40, 30);
    }
    au_image_cache_frame_begin(c);
    IT_CHECK(view->width == 160 && view->height == 120, "kept while a larger use is recent: %dx%d", view->width,
             view->height);
    au_image_entry_use(c, e, 40, 30);
    au_image_cache_frame_begin(c);
    IT_CHECK(view->width == 40 && view->height == 30, "decoded again at the size shown: %dx%d", view->width,
             view->height);
    au_image_cache_free(c);
    free(d);
}

static void test_budget(void)
{
    size_t n;
    uint8_t *d = load("png/big.png", &n);
    if (!d)
        return;
    /* Budget: room for one 160 x 120 image (75 KB) and a bit. */
    AuImageCache *c = au_image_cache_new(100 * 1024);
    AuImageEntry *e[3];
    char url[64];
    for (int i = 0; i < 3; i++) {
        snprintf(url, sizeof url, "http://x/%d.png", i);
        e[i] = au_image_cache_add(c, url);
        feed(c, e[i], d, n, 4096);
        au_image_entry_done(c, e[i], true);
    }
    /* All three visible at once: none evicted (visible images stay). */
    au_image_cache_frame_begin(c);
    for (int i = 0; i < 3; i++)
        au_image_entry_use(c, e[i], 0, 0);
    au_image_cache_trim(c);
    AuImageCacheStats st;
    au_image_cache_stats(c, &st);
    IT_CHECK(st.decoded == 3 && st.evictions == 0, "visible images kept: %u decoded, %u evicted", st.decoded,
             st.evictions);
    /* Scroll: only the last is visible; the others go, oldest first. */
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e[2], 0, 0);
    au_image_cache_trim(c);
    au_image_cache_stats(c, &st);
    IT_CHECK(st.decoded == 1 && st.evictions == 2, "evicted: %u decoded, %u evicted", st.decoded, st.evictions);
    IT_CHECK(st.decoded_bytes <= st.budget, "under budget: %zu", st.decoded_bytes);
    IT_CHECK(au_image_entry_surface(e[0])->width == 1, "evicted surface is blank");
    IT_CHECK(au_image_entry_state(e[0]) == AU_IMAGE_SIZED, "evicted entry keeps its size");
    IT_CHECK(st.compressed_bytes == 3 * n, "bytes kept for decoding again");
    /* Back into view: decoded again. */
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e[0], 0, 0);
    IT_CHECK(au_image_entry_surface(e[0])->width == 160, "decoded again after eviction");
    /* A per-image cap of half the budget: a large image decodes reduced. */
    au_image_cache_set_budget(c, 2 * 1024 * 1024);
    au_image_cache_free(c);
    free(d);
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

/* A 1-bit BMP of side x side pixels, stripes. */
static uint8_t *make_bmp(int side, size_t *len)
{
    size_t row = (size_t)((side + 31) / 32) * 4, off = 14 + 40 + 8;
    *len = off + row * (size_t)side;
    uint8_t *d = calloc(1, *len);
    d[0] = 'B';
    d[1] = 'M';
    put32(d + 2, (uint32_t)*len);
    put32(d + 10, (uint32_t)off);
    put32(d + 14, 40);
    put32(d + 18, (uint32_t)side);
    put32(d + 22, (uint32_t)side);
    put16(d + 26, 1);
    put16(d + 28, 1);
    put32(d + 34, (uint32_t)(row * (size_t)side));
    d[54 + 4] = d[54 + 5] = d[54 + 6] = 0xFF; /* palette: black, white */
    for (int y = 0; y < side; y++)
        memset(d + off + row * (size_t)y, (y & 8) ? 0xF0 : 0x0F, row);
    return d;
}

/* An image larger than the per-image cap decodes reduced and is drawn
 * larger: it is not decoded again on each paint (a 1200 x 1200 page
 * background made every scroll step a full decode). */
static void test_capped_not_redecoded(void)
{
    size_t n;
    uint8_t *d = make_bmp(1024, &n); /* 4 MB decoded; the cap is 1 MB */
    AuImageCache *c = au_image_cache_new(1024 * 1024);
    AuImageEntry *e = au_image_cache_add(c, "http://x/bg.bmp");
    feed(c, e, d, n, 65536);
    au_image_entry_done(c, e, true);
    const AuSurface *view = au_image_entry_surface(e);
    AuImageCacheStats st;
    for (int i = 0; i < 5; i++) {
        au_image_cache_frame_begin(c);
        au_image_entry_use(c, e, 1024, 1024);
    }
    au_image_cache_stats(c, &st);
    IT_CHECK(view->width < 1024 && view->width > 1, "decoded reduced: %dx%d", view->width, view->height);
    IT_CHECK(st.decodes == 1, "decoded once over five paints: %u decodes", st.decodes);
    /* At the natural size (no display size): also once. */
    AuImageEntry *f = au_image_cache_add(c, "http://x/bg2.bmp");
    feed(c, f, d, n, 65536);
    au_image_entry_done(c, f, true);
    for (int i = 0; i < 5; i++) {
        au_image_cache_frame_begin(c);
        au_image_entry_use(c, f, 0, 0);
    }
    au_image_cache_stats(c, &st);
    IT_CHECK(st.decodes == 2, "natural size decoded once: %u decodes", st.decodes);
    au_image_cache_free(c);
    free(d);
}

static void test_progressive(void)
{
    size_t n;
    uint8_t *d = load("png/big.png", &n);
    if (!d)
        return;
    AuImageCache *c = au_image_cache_new(0);
    AuImageEntry *e = au_image_cache_add(c, "http://x/p.png");
    au_image_entry_data(c, e, d, 100);
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 0, 0); /* in view while loading */
    au_image_entry_data(c, e, d + 100, n / 2 - 100);
    IT_CHECK(au_image_entry_take_changed(e), "rows arrived: changed");
    const AuSurface *v = au_image_entry_surface(e);
    IT_CHECK(v->width == 160 && (v->pixels[0] >> 24) == 255 && v->pixels[(size_t)119 * v->stride] == 0,
             "top rows in, bottom rows not yet");
    au_image_entry_data(c, e, d + n / 2, n - n / 2);
    au_image_entry_done(c, e, true);
    IT_CHECK((v->pixels[(size_t)119 * v->stride] >> 24) == 255, "all rows in");
    au_image_cache_free(c);
    free(d);
}

static void test_animation(void)
{
    size_t n;
    uint8_t *d = load("gif/anim-loop2.gif", &n);
    if (!d)
        return;
    AuImageCache *c = au_image_cache_new(0);
    AuImageEntry *e = au_image_cache_add(c, "http://x/a.gif");
    feed(c, e, d, n, 64);
    au_image_entry_done(c, e, true);
    uint32_t t = 1000;
    au_image_cache_tick(c, t);
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 0, 0);
    IT_CHECK(au_image_entry_state(e) == AU_IMAGE_READY, "first frame");
    /* Three frames of 500 ms, loop count 2 (three plays). */
    uint32_t next = au_image_cache_tick(c, t);
    IT_CHECK(next == 500, "next frame in %u ms", next);
    int frames = 1;
    au_image_entry_take_changed(e);
    for (int step = 0; step < 100; step++) {
        t += 100;
        next = au_image_cache_tick(c, t);
        if (au_image_entry_take_changed(e))
            frames++;
        au_image_cache_frame_begin(c);
        au_image_entry_use(c, e, 0, 0); /* stays in view */
        if (next == UINT32_MAX)
            break;
    }
    IT_CHECK(frames == 9, "three plays of three frames: %d frames shown", frames);
    IT_CHECK(au_image_entry_animated(e), "animated");
    AuImageCacheStats st;
    au_image_cache_stats(c, &st);
    IT_CHECK(st.animating == 0 && st.decoder_bytes == 0, "played out: decoder freed");
    au_image_cache_free(c);
    free(d);

    /* A looping-forever animation pauses when not in use. */
    d = load("gif/anim.gif", &n);
    if (!d)
        return;
    c = au_image_cache_new(0);
    e = au_image_cache_add(c, "http://x/b.gif");
    feed(c, e, d, n, n);
    au_image_entry_done(c, e, true);
    t = 0;
    au_image_cache_tick(c, t);
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 0, 0);
    /* Frame 1 has 100 ms; frame 2 a delay of 0 (played at 100 ms). */
    IT_CHECK(au_image_cache_tick(c, t) == 100, "first delay 100 ms");
    au_image_cache_tick(c, t + 100);
    IT_CHECK(au_image_entry_take_changed(e), "frame 2 at 100 ms");
    IT_CHECK(au_image_cache_tick(c, t + 150) == 50, "frame 2 delay 0 plays at 100 ms");
    au_image_cache_frame_begin(c); /* a paint without the image */
    IT_CHECK(au_image_cache_tick(c, t + 1000) == UINT32_MAX, "paused when not in use");
    IT_CHECK(!au_image_entry_take_changed(e), "no frames while paused");
    au_image_cache_stats(c, &st);
    IT_CHECK(st.animating == 0, "paused animation does not count");
    au_image_cache_frame_begin(c);
    au_image_entry_use(c, e, 0, 0);
    au_image_cache_tick(c, t + 1001);
    IT_CHECK(au_image_entry_take_changed(e), "resumes in view");
    au_image_cache_free(c);
    free(d);
}

static void test_failures_and_bytes(void)
{
    AuImageCache *c = au_image_cache_new(0);
    AuImageEntry *e = au_image_cache_add(c, "http://x/bad");
    au_image_entry_data(c, e, "<html>not an image, an error page</html>", 40);
    IT_CHECK(au_image_entry_state(e) == AU_IMAGE_FAILED, "not an image: failed");
    e = au_image_cache_add(c, "http://x/404");
    au_image_entry_done(c, e, false);
    IT_CHECK(au_image_entry_state(e) == AU_IMAGE_FAILED, "failed load");
    /* A BMP's bytes are larger than its pixels... only just: a 24-bit
     * BMP is 3/4 of them. A 32-bit one (pixels + header) is larger. */
    size_t n;
    uint8_t *d = load("bmp/v4argb.bmp", &n);
    if (d) {
        e = au_image_cache_add(c, "http://x/v4.bmp");
        feed(c, e, d, n, 100);
        au_image_entry_done(c, e, true);
        au_image_cache_frame_begin(c);
        au_image_entry_use(c, e, 0, 0);
        AuImageCacheStats st;
        au_image_cache_stats(c, &st);
        IT_CHECK(st.compressed_bytes == 0, "bytes dropped, pixels kept: %zu bytes", st.compressed_bytes);
        au_image_cache_frame_begin(c);
        au_image_cache_set_budget(c, 1);
        IT_CHECK(au_image_entry_surface(e)->width == 15, "pixels kept when their bytes are gone");
        free(d);
    }
    au_image_cache_free(c);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_cache VECTOR_ROOT\n");
        return 2;
    }
    snprintf(root, sizeof root, "%s", argv[1]);
    test_lazy_and_sizes();
    test_shrink();
    test_budget();
    test_capped_not_redecoded();
    test_progressive();
    test_animation();
    test_failures_and_bytes();
    if (it_failures) {
        printf("%d failures\n", it_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
